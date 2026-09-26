// trace.cpp: the Vulkan half of the frame.
//
// Phase 1 dispatches a compute shader that writes the shared image. Phase 2
// adds a second compute pipeline that rayQueryKHRs the world TLAS. The submit
// waits on the semaphore GL just signalled and signals the one GL is about to
// wait on, so the GPU serialises GL -> VK -> GL every frame while the CPU
// keeps running ahead.

#include "engine.h"
#include "hwrt/hwrt.h"
#include "shaders/debug_comp.h"
#include "shaders/silhouette_comp.h"
#include "shaders/rtao_comp.h"
#include "shaders/hitshade_comp.h"
#include "shaders/hitlight_comp.h"
#include "shaders/skycompose_comp.h"

int hwrtfencestalls = 0;

struct hwrttracestate
{
    VkShaderModule module, rtmodule, aomodule, shademodule, lightmodule, composemodule;
    VkDescriptorSetLayout setlayout, rtsetlayout, aosetlayout, shadesetlayout, lightsetlayout, composesetlayout;
    VkDescriptorPool pool;
    VkDescriptorSet set;
    VkDescriptorSet rtset[HWRT_FRAMES_IN_FLIGHT], aoset[HWRT_FRAMES_IN_FLIGHT], shadeset[HWRT_FRAMES_IN_FLIGHT], lightset[HWRT_FRAMES_IN_FLIGHT], composeset[HWRT_FRAMES_IN_FLIGHT];
    VkPipelineLayout pipelayout, rtpipelayout, aopipelayout, shadepipelayout, lightpipelayout, composepipelayout;
    VkPipeline pipeline, rtpipeline, aopipeline, shadepipeline, lightpipeline, composepipeline;
    VkCommandPool cmdpool;
    VkCommandBuffer cmd[HWRT_FRAMES_IN_FLIGHT];
    VkFence fence[HWRT_FRAMES_IN_FLIGHT];
    VkQueryPool stamps;
    bool pending[HWRT_FRAMES_IN_FLIGHT];
    VkImageView boundview, bounddepth;
    VkAccelerationStructureKHR boundtlas[HWRT_FRAMES_IN_FLIGHT];
    int boundshadeepoch[HWRT_FRAMES_IN_FLIGHT];
    int boundlightepoch[HWRT_FRAMES_IN_FLIGHT];
    int slot;
    int frame;
    int skyflip;  // which of the two skyvis history images the next dispatch writes
};

static hwrttracestate tr;

struct hwrtdebugparams
{
    int32_t w, h, mode, frame;
};

struct hwrtsilhouetteparams
{
    int32_t w, h, mode, frame;
    float invcamproj[16];
    float origin[4];
    float range[4];
};

struct hwrtlightparams
{
    int32_t w, h, mode, frame;
    float invcamproj[16];
    float origin[4];
    float range[4];
    float lighting[4];
};
static_assert(sizeof(hwrtlightparams) == 128, "light push constants");

struct hwrtcomposeparams
{
    int32_t w, h, mode, dbg;
    float lighting[4];
};
static_assert(sizeof(hwrtcomposeparams) == 32, "compose push constants");

int hwrthdrpushmode = 0;
// Set when the RT lighting pass was really recorded this frame (hwrt.cpp reads it).
bool hwrtlightrecorded = false;

extern int farplane;
extern float nearplane;
extern float rtaoradius, rtaobias, rtaoscale;
extern int hwrtdlights;
extern int hwrtskytemporal;
extern bvec ambientcolor;

// Temporal skyvis. Two screen-sized images the lighting dispatch alternates
// between, plus a matching r16f age pair. rgb is the hit position relative
// to the camera that wrote it, a is the running visibility. Age is the
// accepted sample count used as 1/age until N. Half float is 0.05% of the
// stored value and the match test below allows 1%, so the offset survives
// the format with room to spare. Nothing outside the dispatch reads these,
// so they are plain device-local images: no NT handle, no GL import, no
// place in the interop teardown.
struct hwrtskyhist
{
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
};
static hwrtskyhist skyhist[2];
static hwrtskyhist skyage[2];
static int skyhistw = 0, skyhisth = 0;
static bool skyhistfailed = false, skyhistfresh = true;

static void destroystorage2d(hwrtskyhist &img)
{
    if(img.view) vkDestroyImageView(hwrtdev.device, img.view, NULL);
    if(img.image) vkDestroyImage(hwrtdev.device, img.image, NULL);
    if(img.memory) vkFreeMemory(hwrtdev.device, img.memory, NULL);
    memset(&img, 0, sizeof(img));
}

static void destroyskyhistory()
{
    loopi(2)
    {
        destroystorage2d(skyhist[i]);
        destroystorage2d(skyage[i]);
    }
    skyhistw = skyhisth = 0;
}

bool hwrtskyhistready()
{
    return !skyhistfailed && skyhist[0].view && skyhist[1].view &&
           skyage[0].view && skyage[1].view &&
           skyhistw == hwrtio.w && skyhisth == hwrtio.h;
}

void hwrtskyhistsize(int &w, int &h)
{
    w = skyhistw;
    h = skyhisth;
}

static bool createstorage2d(hwrtskyhist &img, int w, int h, VkFormat format, VkImageUsageFlags extra = 0)
{
    VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent.width = uint32_t(w);
    info.extent.height = uint32_t(h);
    info.extent.depth = 1;
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_STORAGE_BIT | extra;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if(vkCreateImage(hwrtdev.device, &info, NULL, &img.image) != VK_SUCCESS) return false;

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(hwrtdev.device, img.image, &req);
    int memtype = hwrtfindmemtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if(memtype < 0) memtype = hwrtfindmemtype(req.memoryTypeBits, 0);
    if(memtype < 0) return false;
    VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = uint32_t(memtype);
    if(vkAllocateMemory(hwrtdev.device, &alloc, NULL, &img.memory) != VK_SUCCESS) return false;
    if(vkBindImageMemory(hwrtdev.device, img.image, img.memory, 0) != VK_SUCCESS) return false;

    VkImageViewCreateInfo view = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    view.image = img.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    if(vkCreateImageView(hwrtdev.device, &view, NULL, &img.view) != VK_SUCCESS) return false;
    return true;
}

static bool createskypair(int w, int h)
{
    loopi(2) if(!createstorage2d(skyhist[i], w, h, VK_FORMAT_R16G16B16A16_SFLOAT)) return false;
    loopi(2) if(!createstorage2d(skyage[i], w, h, VK_FORMAT_R16_SFLOAT)) return false;
    skyhistw = w;
    skyhisth = h;
    // Nothing has written either image yet, so the first dispatch discards
    // both instead of reading whatever the allocator handed back.
    skyhistfresh = true;
    return true;
}

// Called at the top of a lighting dispatch, before hwrtupdatelights() packs the
// blend weight, so the weight and the images can never disagree. The shader
// names bindings 13 and 14 unconditionally, so a failure still has to leave two
// valid views behind: it falls back to a 1x1 pair the weight then tells the
// shader to ignore. The temporal pass is lost; nothing else is.
bool hwrtensureskyhistory()
{
    if(!hwrtdev.ok() || hwrtio.w < 1 || hwrtio.h < 1) return false;
    if(hwrtskyhistready()) return true;
    if(skyhistfailed) return false;

    // Only reached on the first dispatch and on a resize, both of which are
    // already wait-idle paths. Nothing may still be reading the old pair.
    if(skyhist[0].image || skyhist[1].image || skyage[0].image || skyage[1].image)
    {
        if(!hwrtwaitidle("vkDeviceWaitIdle (skyvis history resize)")) return false;
    }
    destroyskyhistory();
    hwrtinvalidateskyhistory();

    if(createskypair(hwrtio.w, hwrtio.h)) return true;

    destroyskyhistory();
    skyhistfailed = true;
    conoutf(CON_WARN, "hwrt: could not allocate the %dx%d skyvis history, temporal sky off", hwrtio.w, hwrtio.h);
    if(!createskypair(1, 1))
    {
        destroyskyhistory();
        conoutf(CON_WARN, "hwrt: skyvis history placeholder failed too");
    }
    return false;
}

enum { HWRT_NRD_IMG_COUNT = 9 };
static hwrtskyhist nrdimg[HWRT_NRD_IMG_COUNT];
static int nrdimgw = 0, nrdimgh = 0;
static bool nrdimgfailed = false, nrdimgfresh = true;
extern int hwrtnrd, hwrtnrddbg;

static VkFormat nrdimgformat(int i)
{
    if(i == 1) return VK_FORMAT_R32_SFLOAT;
    if(i == 2) return VK_FORMAT_R16G16B16A16_UNORM;
    return VK_FORMAT_R16G16B16A16_SFLOAT;
}

static void destroynrdimages()
{
    loopi(HWRT_NRD_IMG_COUNT) destroystorage2d(nrdimg[i]);
    nrdimgw = nrdimgh = 0;
}

void hwrtdestroynrdguides()
{
    if(hwrtdev.ok() && nrdimg[0].image) hwrtwaitidle("nrd guides");
    destroynrdimages();
    nrdimgfailed = false;
    nrdimgfresh = true;
}

bool hwrtnrdguidesready()
{
    loopi(HWRT_NRD_IMG_COUNT) if(!nrdimg[i].view) return false;
    return nrdimgw > 0 && nrdimgh > 0;
}

int hwrtnrdguidemem()
{
    if(!hwrtnrdguidesready()) return 0;
    // rough: 8 images rgba16f + 1 r32f at nrdimgw*h
    return int((uint64_t(nrdimgw) * uint64_t(nrdimgh) * (8 * 8 + 4)) / (1024 * 1024));
}

static bool createnrdset(int w, int h)
{
    VkImageUsageFlags extra = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    loopi(HWRT_NRD_IMG_COUNT)
        if(!createstorage2d(nrdimg[i], w, h, nrdimgformat(i), extra)) return false;
    nrdimgw = w;
    nrdimgh = h;
    nrdimgfresh = true;
    return true;
}

bool hwrtensurenrdguides()
{
    if(!hwrtdev.ok() || hwrtio.w < 1 || hwrtio.h < 1) return false;
    int wantw = 1, wanth = 1;
    if(hwrtnrdsession())
    {
        wantw = hwrtio.w;
        wanth = hwrtio.h;
    }
    if(hwrtnrdguidesready() && nrdimgw == wantw && nrdimgh == wanth) return wantw > 1;
    if(nrdimgfailed && wantw == 1) return false;
    if(nrdimg[0].image)
    {
        if(!hwrtwaitidle("vkDeviceWaitIdle (nrd guides resize)")) return false;
    }
    destroynrdimages();
    hwrtnrdinvalidate();
    if(createnrdset(wantw, wanth))
    {
        conoutf("hwrt nrd: guides %dx%d (exact)", wantw, wanth);
        nrdimgfailed = false;
        return wantw > 1;
    }
    destroynrdimages();
    if(wantw > 1)
    {
        conoutf(CON_WARN, "hwrt nrd: could not allocate %dx%d guides, skyage filter stays", wantw, wanth);
        nrdimgfailed = true;
    }
    if(!createnrdset(1, 1))
    {
        destroynrdimages();
        conoutf(CON_WARN, "hwrt nrd: 1x1 guide placeholder failed");
    }
    return false;
}

static bool initrtpipeline()
{
    if(!hwrtdev.rayquery) return true;

    VkShaderModuleCreateInfo modinfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    modinfo.codeSize = sizeof(hwrtsilhouettespv);
    modinfo.pCode = hwrtsilhouettespv;
    VkResult r = vkCreateShaderModule(hwrtdev.device, &modinfo, NULL, &tr.rtmodule);
    if(r != VK_SUCCESS) { conoutf(CON_WARN, "hwrt: silhouette shader failed (%s)", hwrtresultstr(r)); return false; }

    VkDescriptorSetLayoutBinding bindings[2];
    memset(bindings, 0, sizeof(bindings));
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo setinfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    setinfo.bindingCount = 2;
    setinfo.pBindings = bindings;
    r = vkCreateDescriptorSetLayout(hwrtdev.device, &setinfo, NULL, &tr.rtsetlayout);
    if(r != VK_SUCCESS) { conoutf(CON_WARN, "hwrt: silhouette descriptor layout failed (%s)", hwrtresultstr(r)); return false; }

    VkPushConstantRange range = {};
    range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    range.size = sizeof(hwrtsilhouetteparams);
    VkPipelineLayoutCreateInfo layoutinfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutinfo.setLayoutCount = 1;
    layoutinfo.pSetLayouts = &tr.rtsetlayout;
    layoutinfo.pushConstantRangeCount = 1;
    layoutinfo.pPushConstantRanges = &range;
    r = vkCreatePipelineLayout(hwrtdev.device, &layoutinfo, NULL, &tr.rtpipelayout);
    if(r != VK_SUCCESS) { conoutf(CON_WARN, "hwrt: silhouette pipeline layout failed (%s)", hwrtresultstr(r)); return false; }

    VkComputePipelineCreateInfo pipeinfo = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    pipeinfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeinfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeinfo.stage.module = tr.rtmodule;
    pipeinfo.stage.pName = "main";
    pipeinfo.layout = tr.rtpipelayout;
    r = vkCreateComputePipelines(hwrtdev.device, VK_NULL_HANDLE, 1, &pipeinfo, NULL, &tr.rtpipeline);
    if(r != VK_SUCCESS) { conoutf(CON_WARN, "hwrt: silhouette pipeline failed (%s)", hwrtresultstr(r)); return false; }
    return true;
}

static bool initaopipeline()
{
    if(!hwrtdev.rayquery) return true;

    VkShaderModuleCreateInfo modinfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    modinfo.codeSize = sizeof(hwrtrtaospv);
    modinfo.pCode = hwrtrtaospv;
    VkResult r = vkCreateShaderModule(hwrtdev.device, &modinfo, NULL, &tr.aomodule);
    if(r != VK_SUCCESS) { conoutf(CON_WARN, "hwrt: RTAO shader failed (%s)", hwrtresultstr(r)); return false; }

    VkDescriptorSetLayoutBinding bindings[3];
    memset(bindings, 0, sizeof(bindings));
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo setinfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    setinfo.bindingCount = 3;
    setinfo.pBindings = bindings;
    r = vkCreateDescriptorSetLayout(hwrtdev.device, &setinfo, NULL, &tr.aosetlayout);
    if(r != VK_SUCCESS) { conoutf(CON_WARN, "hwrt: RTAO descriptor layout failed (%s)", hwrtresultstr(r)); return false; }

    VkPushConstantRange range = {};
    range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    range.size = sizeof(hwrtsilhouetteparams);
    VkPipelineLayoutCreateInfo layoutinfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutinfo.setLayoutCount = 1;
    layoutinfo.pSetLayouts = &tr.aosetlayout;
    layoutinfo.pushConstantRangeCount = 1;
    layoutinfo.pPushConstantRanges = &range;
    r = vkCreatePipelineLayout(hwrtdev.device, &layoutinfo, NULL, &tr.aopipelayout);
    if(r != VK_SUCCESS) { conoutf(CON_WARN, "hwrt: RTAO pipeline layout failed (%s)", hwrtresultstr(r)); return false; }

    VkComputePipelineCreateInfo pipeinfo = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    pipeinfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeinfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeinfo.stage.module = tr.aomodule;
    pipeinfo.stage.pName = "main";
    pipeinfo.layout = tr.aopipelayout;
    r = vkCreateComputePipelines(hwrtdev.device, VK_NULL_HANDLE, 1, &pipeinfo, NULL, &tr.aopipeline);
    if(r != VK_SUCCESS) { conoutf(CON_WARN, "hwrt: RTAO pipeline failed (%s)", hwrtresultstr(r)); return false; }
    return true;
}

static bool initshadepipeline()
{
    if(!hwrtdev.rayquery) return true;

    VkShaderModuleCreateInfo modinfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    modinfo.codeSize = sizeof(hwrthitshadespv);
    modinfo.pCode = hwrthitshadespv;
    VkResult r = vkCreateShaderModule(hwrtdev.device, &modinfo, NULL, &tr.shademodule);
    if(r != VK_SUCCESS) { conoutf(CON_WARN, "hwrt: hit-shade shader failed (%s)", hwrtresultstr(r)); return false; }

    VkDescriptorSetLayoutBinding bindings[7];
    memset(bindings, 0, sizeof(bindings));
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[5].binding = 5;
    bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[5].descriptorCount = 1;
    bindings[5].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[6].binding = 6;
    bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[6].descriptorCount = 1;
    bindings[6].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo setinfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    setinfo.bindingCount = 7;
    setinfo.pBindings = bindings;
    r = vkCreateDescriptorSetLayout(hwrtdev.device, &setinfo, NULL, &tr.shadesetlayout);
    if(r != VK_SUCCESS) { conoutf(CON_WARN, "hwrt: hit-shade descriptor layout failed (%s)", hwrtresultstr(r)); return false; }

    VkPushConstantRange range = {};
    range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    range.size = sizeof(hwrtsilhouetteparams);
    VkPipelineLayoutCreateInfo layoutinfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutinfo.setLayoutCount = 1;
    layoutinfo.pSetLayouts = &tr.shadesetlayout;
    layoutinfo.pushConstantRangeCount = 1;
    layoutinfo.pPushConstantRanges = &range;
    r = vkCreatePipelineLayout(hwrtdev.device, &layoutinfo, NULL, &tr.shadepipelayout);
    if(r != VK_SUCCESS) { conoutf(CON_WARN, "hwrt: hit-shade pipeline layout failed (%s)", hwrtresultstr(r)); return false; }

    VkComputePipelineCreateInfo pipeinfo = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    pipeinfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeinfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeinfo.stage.module = tr.shademodule;
    pipeinfo.stage.pName = "main";
    pipeinfo.layout = tr.shadepipelayout;
    r = vkCreateComputePipelines(hwrtdev.device, VK_NULL_HANDLE, 1, &pipeinfo, NULL, &tr.shadepipeline);
    if(r != VK_SUCCESS) { conoutf(CON_WARN, "hwrt: hit-shade pipeline failed (%s)", hwrtresultstr(r)); return false; }
    return true;
}

static bool initlightpipeline()
{
    if(!hwrtdev.rayquery) return true;

    VkShaderModuleCreateInfo modinfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    modinfo.codeSize = sizeof(hwrthitlightspv);
    modinfo.pCode = hwrthitlightspv;
    VkResult r = vkCreateShaderModule(hwrtdev.device, &modinfo, NULL, &tr.lightmodule);
    if(r != VK_SUCCESS) { conoutf(CON_WARN, "hwrt: hit-light shader failed (%s)", hwrtresultstr(r)); return false; }
    {
        unsigned words = unsigned(sizeof(hwrthitlightspv) / 4);
        uint32_t h = 2166136261u;
        loopi(int(words))
        {
            h ^= hwrthitlightspv[i];
            h *= 16777619u;
        }
        conoutf(CON_INIT, "hwrt: hitlight SPIR-V %u words hash %08X", words, h);
    }

    VkDescriptorSetLayoutBinding bindings[25];
    memset(bindings, 0, sizeof(bindings));
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[5].binding = 5;
    bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[5].descriptorCount = 1;
    bindings[5].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[6].binding = 6;
    bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[6].descriptorCount = 1;
    bindings[6].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[7].binding = 7;
    bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[7].descriptorCount = 1;
    bindings[7].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[8].binding = 8;
    bindings[8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[8].descriptorCount = 1;
    bindings[8].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    // The GL window depth RTAO already shares, read here so a hit behind an
    // alpha-tested mesh the TLAS never saw can be handed back to GL.
    bindings[9].binding = 9;
    bindings[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[9].descriptorCount = 1;
    bindings[9].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[10].binding = 10;
    bindings[10].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[10].descriptorCount = 1;
    bindings[10].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[11].binding = 11;
    bindings[11].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[11].descriptorCount = 1;
    bindings[11].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[12].binding = 12;
    bindings[12].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[12].descriptorCount = 1;
    bindings[12].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    // The skyvis history pair, swapped every dispatch: 13 is what the previous
    // one wrote, 14 is what this one writes. 15/16 are the matching age images.
    bindings[13].binding = 13;
    bindings[13].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[13].descriptorCount = 1;
    bindings[13].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[14].binding = 14;
    bindings[14].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[14].descriptorCount = 1;
    bindings[14].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[15].binding = 15;
    bindings[15].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[15].descriptorCount = 1;
    bindings[15].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[16].binding = 16;
    bindings[16].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[16].descriptorCount = 1;
    bindings[16].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    for(int i = 17; i < 25; i++)
    {
        bindings[i].binding = uint32_t(i);
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo setinfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    setinfo.bindingCount = 25;
    setinfo.pBindings = bindings;
    r = vkCreateDescriptorSetLayout(hwrtdev.device, &setinfo, NULL, &tr.lightsetlayout);
    if(r != VK_SUCCESS) { conoutf(CON_WARN, "hwrt: hit-light descriptor layout failed (%s)", hwrtresultstr(r)); return false; }

    VkPushConstantRange range = {};
    range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    range.size = sizeof(hwrtlightparams);
    VkPipelineLayoutCreateInfo layoutinfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutinfo.setLayoutCount = 1;
    layoutinfo.pSetLayouts = &tr.lightsetlayout;
    layoutinfo.pushConstantRangeCount = 1;
    layoutinfo.pPushConstantRanges = &range;
    r = vkCreatePipelineLayout(hwrtdev.device, &layoutinfo, NULL, &tr.lightpipelayout);
    if(r != VK_SUCCESS) { conoutf(CON_WARN, "hwrt: hit-light pipeline layout failed (%s)", hwrtresultstr(r)); return false; }

    VkComputePipelineCreateInfo pipeinfo = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    pipeinfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeinfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeinfo.stage.module = tr.lightmodule;
    pipeinfo.stage.pName = "main";
    pipeinfo.layout = tr.lightpipelayout;
    r = vkCreateComputePipelines(hwrtdev.device, VK_NULL_HANDLE, 1, &pipeinfo, NULL, &tr.lightpipeline);
    if(r != VK_SUCCESS) { conoutf(CON_WARN, "hwrt: hit-light pipeline failed (%s)", hwrtresultstr(r)); return false; }
    return true;
}

static bool initcomposepipeline()
{
    if(!hwrtdev.rayquery) return true;
    VkShaderModuleCreateInfo modinfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    modinfo.codeSize = sizeof(hwrtskycomposespv);
    modinfo.pCode = hwrtskycomposespv;
    VkResult r = vkCreateShaderModule(hwrtdev.device, &modinfo, NULL, &tr.composemodule);
    if(r != VK_SUCCESS) { conoutf(CON_WARN, "hwrt: skycompose shader failed (%s)", hwrtresultstr(r)); return false; }

    VkDescriptorSetLayoutBinding bindings[8];
    memset(bindings, 0, sizeof(bindings));
    loopi(6)
    {
        bindings[i].binding = uint32_t(i);
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    bindings[6].binding = 6;
    bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[6].descriptorCount = 1;
    bindings[6].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[7].binding = 7;
    bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[7].descriptorCount = 1;
    bindings[7].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo setinfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    setinfo.bindingCount = 8;
    setinfo.pBindings = bindings;
    r = vkCreateDescriptorSetLayout(hwrtdev.device, &setinfo, NULL, &tr.composesetlayout);
    if(r != VK_SUCCESS) { conoutf(CON_WARN, "hwrt: skycompose layout failed (%s)", hwrtresultstr(r)); return false; }

    VkPushConstantRange range = {};
    range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    range.size = sizeof(hwrtcomposeparams);
    VkPipelineLayoutCreateInfo layoutinfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutinfo.setLayoutCount = 1;
    layoutinfo.pSetLayouts = &tr.composesetlayout;
    layoutinfo.pushConstantRangeCount = 1;
    layoutinfo.pPushConstantRanges = &range;
    r = vkCreatePipelineLayout(hwrtdev.device, &layoutinfo, NULL, &tr.composepipelayout);
    if(r != VK_SUCCESS) { conoutf(CON_WARN, "hwrt: skycompose pipeline layout failed (%s)", hwrtresultstr(r)); return false; }

    VkComputePipelineCreateInfo pipeinfo = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    pipeinfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeinfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeinfo.stage.module = tr.composemodule;
    pipeinfo.stage.pName = "main";
    pipeinfo.layout = tr.composepipelayout;
    r = vkCreateComputePipelines(hwrtdev.device, VK_NULL_HANDLE, 1, &pipeinfo, NULL, &tr.composepipeline);
    if(r != VK_SUCCESS) { conoutf(CON_WARN, "hwrt: skycompose pipeline failed (%s)", hwrtresultstr(r)); return false; }
    return true;
}

bool hwrtinittrace()
{
    if(tr.pipeline) return true;
    memset(&tr, 0, sizeof(tr));

    VkShaderModuleCreateInfo modinfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    modinfo.codeSize = sizeof(hwrtdebugspv);
    modinfo.pCode = hwrtdebugspv;
    HWRTCHECK(vkCreateShaderModule(hwrtdev.device, &modinfo, NULL, &tr.module), "vkCreateShaderModule");

    VkDescriptorSetLayoutBinding binding = {};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo setinfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    setinfo.bindingCount = 1;
    setinfo.pBindings = &binding;
    HWRTCHECK(vkCreateDescriptorSetLayout(hwrtdev.device, &setinfo, NULL, &tr.setlayout), "vkCreateDescriptorSetLayout");

    if(!initrtpipeline())
    {
        conoutf(CON_WARN, "hwrt: ray query pipeline unavailable, leaving the phase 1 probe");
        if(tr.rtpipeline) { vkDestroyPipeline(hwrtdev.device, tr.rtpipeline, NULL); tr.rtpipeline = VK_NULL_HANDLE; }
        if(tr.rtpipelayout) { vkDestroyPipelineLayout(hwrtdev.device, tr.rtpipelayout, NULL); tr.rtpipelayout = VK_NULL_HANDLE; }
        if(tr.rtsetlayout) { vkDestroyDescriptorSetLayout(hwrtdev.device, tr.rtsetlayout, NULL); tr.rtsetlayout = VK_NULL_HANDLE; }
        if(tr.rtmodule) { vkDestroyShaderModule(hwrtdev.device, tr.rtmodule, NULL); tr.rtmodule = VK_NULL_HANDLE; }
    }
    if(tr.rtpipeline && !initaopipeline())
    {
        conoutf(CON_WARN, "hwrt: RTAO pipeline unavailable");
        if(tr.aopipeline) { vkDestroyPipeline(hwrtdev.device, tr.aopipeline, NULL); tr.aopipeline = VK_NULL_HANDLE; }
        if(tr.aopipelayout) { vkDestroyPipelineLayout(hwrtdev.device, tr.aopipelayout, NULL); tr.aopipelayout = VK_NULL_HANDLE; }
        if(tr.aosetlayout) { vkDestroyDescriptorSetLayout(hwrtdev.device, tr.aosetlayout, NULL); tr.aosetlayout = VK_NULL_HANDLE; }
        if(tr.aomodule) { vkDestroyShaderModule(hwrtdev.device, tr.aomodule, NULL); tr.aomodule = VK_NULL_HANDLE; }
    }
    if(tr.rtpipeline && !initshadepipeline())
    {
        conoutf(CON_WARN, "hwrt: hit-shade pipeline unavailable");
        if(tr.shadepipeline) { vkDestroyPipeline(hwrtdev.device, tr.shadepipeline, NULL); tr.shadepipeline = VK_NULL_HANDLE; }
        if(tr.shadepipelayout) { vkDestroyPipelineLayout(hwrtdev.device, tr.shadepipelayout, NULL); tr.shadepipelayout = VK_NULL_HANDLE; }
        if(tr.shadesetlayout) { vkDestroyDescriptorSetLayout(hwrtdev.device, tr.shadesetlayout, NULL); tr.shadesetlayout = VK_NULL_HANDLE; }
        if(tr.shademodule) { vkDestroyShaderModule(hwrtdev.device, tr.shademodule, NULL); tr.shademodule = VK_NULL_HANDLE; }
    }
    if(tr.rtpipeline && !initlightpipeline())
    {
        conoutf(CON_WARN, "hwrt: hit-light pipeline unavailable");
        if(tr.lightpipeline) { vkDestroyPipeline(hwrtdev.device, tr.lightpipeline, NULL); tr.lightpipeline = VK_NULL_HANDLE; }
        if(tr.lightpipelayout) { vkDestroyPipelineLayout(hwrtdev.device, tr.lightpipelayout, NULL); tr.lightpipelayout = VK_NULL_HANDLE; }
        if(tr.lightsetlayout) { vkDestroyDescriptorSetLayout(hwrtdev.device, tr.lightsetlayout, NULL); tr.lightsetlayout = VK_NULL_HANDLE; }
        if(tr.lightmodule) { vkDestroyShaderModule(hwrtdev.device, tr.lightmodule, NULL); tr.lightmodule = VK_NULL_HANDLE; }
    }
    if(tr.lightpipeline && !initcomposepipeline())
    {
        conoutf(CON_WARN, "hwrt: skycompose pipeline unavailable, NRD off");
        if(tr.composepipeline) { vkDestroyPipeline(hwrtdev.device, tr.composepipeline, NULL); tr.composepipeline = VK_NULL_HANDLE; }
        if(tr.composepipelayout) { vkDestroyPipelineLayout(hwrtdev.device, tr.composepipelayout, NULL); tr.composepipelayout = VK_NULL_HANDLE; }
        if(tr.composesetlayout) { vkDestroyDescriptorSetLayout(hwrtdev.device, tr.composesetlayout, NULL); tr.composesetlayout = VK_NULL_HANDLE; }
        if(tr.composemodule) { vkDestroyShaderModule(hwrtdev.device, tr.composemodule, NULL); tr.composemodule = VK_NULL_HANDLE; }
    }

    VkDescriptorPoolSize poolsizes[4];
    memset(poolsizes, 0, sizeof(poolsizes));
    int copies = HWRT_FRAMES_IN_FLIGHT;
    int npool = 1, maxsets = 1;
    int nstorage = 1, nas = 0, nssbo = 0, ncombined = 0;
    if(tr.rtpipeline) { nstorage += copies; nas += copies; maxsets += copies; }
    if(tr.aopipeline) { nstorage += 2*copies; nas += copies; maxsets += copies; }
    if(tr.shadepipeline) { nstorage += copies; nas += copies; nssbo += 3*copies; ncombined += 2*copies; maxsets += copies; }
    // Six storage images per light set: the shared colour output, the shared
    // GL depth, both halves of the skyvis history, and both halves of the age
    // count. Miss the pool and vkAllocateDescriptorSets fails silently.
    // Light set: colour, depth, skyvis pair, age pair, plus 8 NRD/payload images.
    if(tr.lightpipeline) { nstorage += 14*copies; nas += copies; nssbo += 5*copies; ncombined += 5*copies; maxsets += copies; }
    if(tr.composepipeline) { nstorage += 7*copies; nssbo += copies; maxsets += copies; }
    poolsizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolsizes[0].descriptorCount = uint32_t(nstorage);
    if(nas)
    {
        poolsizes[npool].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        poolsizes[npool].descriptorCount = uint32_t(nas);
        npool++;
    }
    if(nssbo)
    {
        poolsizes[npool].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolsizes[npool].descriptorCount = uint32_t(nssbo);
        npool++;
    }
    if(ncombined)
    {
        poolsizes[npool].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolsizes[npool].descriptorCount = uint32_t(ncombined);
        npool++;
    }
    VkDescriptorPoolCreateInfo poolinfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    poolinfo.maxSets = uint32_t(maxsets);
    poolinfo.poolSizeCount = uint32_t(npool);
    poolinfo.pPoolSizes = poolsizes;
    HWRTCHECK(vkCreateDescriptorPool(hwrtdev.device, &poolinfo, NULL, &tr.pool), "vkCreateDescriptorPool");

    VkDescriptorSetAllocateInfo allocinfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    allocinfo.descriptorPool = tr.pool;
    allocinfo.descriptorSetCount = 1;
    allocinfo.pSetLayouts = &tr.setlayout;
    HWRTCHECK(vkAllocateDescriptorSets(hwrtdev.device, &allocinfo, &tr.set), "vkAllocateDescriptorSets");
    if(tr.rtpipeline)
    {
        allocinfo.pSetLayouts = &tr.rtsetlayout;
        loopi(HWRT_FRAMES_IN_FLIGHT)
            HWRTCHECK(vkAllocateDescriptorSets(hwrtdev.device, &allocinfo, &tr.rtset[i]), "vkAllocateDescriptorSets");
    }
    if(tr.aopipeline)
    {
        allocinfo.pSetLayouts = &tr.aosetlayout;
        loopi(HWRT_FRAMES_IN_FLIGHT)
            HWRTCHECK(vkAllocateDescriptorSets(hwrtdev.device, &allocinfo, &tr.aoset[i]), "vkAllocateDescriptorSets");
    }
    if(tr.shadepipeline)
    {
        allocinfo.pSetLayouts = &tr.shadesetlayout;
        loopi(HWRT_FRAMES_IN_FLIGHT)
            HWRTCHECK(vkAllocateDescriptorSets(hwrtdev.device, &allocinfo, &tr.shadeset[i]), "vkAllocateDescriptorSets");
    }
    if(tr.lightpipeline)
    {
        allocinfo.pSetLayouts = &tr.lightsetlayout;
        loopi(HWRT_FRAMES_IN_FLIGHT)
        {
            VkResult lr = vkAllocateDescriptorSets(hwrtdev.device, &allocinfo, &tr.lightset[i]);
            if(lr != VK_SUCCESS)
            {
                conoutf(CON_WARN, "hwrt: hit-light descriptors failed (%s), point lights disabled", hwrtresultstr(lr));
                loopj(HWRT_FRAMES_IN_FLIGHT) tr.lightset[j] = VK_NULL_HANDLE;
                if(tr.lightpipeline) { vkDestroyPipeline(hwrtdev.device, tr.lightpipeline, NULL); tr.lightpipeline = VK_NULL_HANDLE; }
                break;
            }
        }
    }
    if(tr.composepipeline)
    {
        allocinfo.pSetLayouts = &tr.composesetlayout;
        loopi(HWRT_FRAMES_IN_FLIGHT)
        {
            VkResult cr = vkAllocateDescriptorSets(hwrtdev.device, &allocinfo, &tr.composeset[i]);
            if(cr != VK_SUCCESS)
            {
                conoutf(CON_WARN, "hwrt: skycompose descriptors failed (%s), NRD off", hwrtresultstr(cr));
                loopj(HWRT_FRAMES_IN_FLIGHT) tr.composeset[j] = VK_NULL_HANDLE;
                if(tr.composepipeline) { vkDestroyPipeline(hwrtdev.device, tr.composepipeline, NULL); tr.composepipeline = VK_NULL_HANDLE; }
                break;
            }
        }
    }

    VkPushConstantRange range = {};
    range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    range.size = sizeof(hwrtdebugparams);
    VkPipelineLayoutCreateInfo layoutinfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutinfo.setLayoutCount = 1;
    layoutinfo.pSetLayouts = &tr.setlayout;
    layoutinfo.pushConstantRangeCount = 1;
    layoutinfo.pPushConstantRanges = &range;
    HWRTCHECK(vkCreatePipelineLayout(hwrtdev.device, &layoutinfo, NULL, &tr.pipelayout), "vkCreatePipelineLayout");

    VkComputePipelineCreateInfo pipeinfo = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    pipeinfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeinfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeinfo.stage.module = tr.module;
    pipeinfo.stage.pName = "main";
    pipeinfo.layout = tr.pipelayout;
    HWRTCHECK(vkCreateComputePipelines(hwrtdev.device, VK_NULL_HANDLE, 1, &pipeinfo, NULL, &tr.pipeline), "vkCreateComputePipelines");

    VkCommandPoolCreateInfo cmdpoolinfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cmdpoolinfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cmdpoolinfo.queueFamilyIndex = hwrtdev.queuefamily;
    HWRTCHECK(vkCreateCommandPool(hwrtdev.device, &cmdpoolinfo, NULL, &tr.cmdpool), "vkCreateCommandPool");

    VkCommandBufferAllocateInfo cmdinfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cmdinfo.commandPool = tr.cmdpool;
    cmdinfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdinfo.commandBufferCount = HWRT_FRAMES_IN_FLIGHT;
    HWRTCHECK(vkAllocateCommandBuffers(hwrtdev.device, &cmdinfo, tr.cmd), "vkAllocateCommandBuffers");

    VkFenceCreateInfo fenceinfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    loopi(HWRT_FRAMES_IN_FLIGHT)
        HWRTCHECK(vkCreateFence(hwrtdev.device, &fenceinfo, NULL, &tr.fence[i]), "vkCreateFence");

    tr.stamps = VK_NULL_HANDLE;
    if(hwrtdev.timestampbits && hwrtdev.timestampperiod > 0 && vkCreateQueryPool && vkCmdWriteTimestamp && vkGetQueryPoolResults)
    {
        VkQueryPoolCreateInfo qinfo = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qinfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qinfo.queryCount = uint32_t(HWRT_FRAMES_IN_FLIGHT * HWRT_TS_COUNT);
        if(vkCreateQueryPool(hwrtdev.device, &qinfo, NULL, &tr.stamps) != VK_SUCCESS)
            tr.stamps = VK_NULL_HANDLE;
    }

    return true;
}

void hwrtdestroytrace()
{
    if(!hwrtdev.device) { memset(&tr, 0, sizeof(tr)); memset(skyhist, 0, sizeof(skyhist)); memset(skyage, 0, sizeof(skyage)); skyhistw = skyhisth = 0; memset(nrdimg, 0, sizeof(nrdimg)); nrdimgw = nrdimgh = 0; return; }
    vkDeviceWaitIdle(hwrtdev.device);
    hwrtdestroynrd();
    destroyskyhistory();
    skyhistfailed = false;
    hwrtinvalidateskyhistory();
    destroynrdimages();
    nrdimgfailed = false;

    loopi(HWRT_FRAMES_IN_FLIGHT) if(tr.fence[i]) vkDestroyFence(hwrtdev.device, tr.fence[i], NULL);
    if(tr.stamps) vkDestroyQueryPool(hwrtdev.device, tr.stamps, NULL);
    if(tr.cmdpool) vkDestroyCommandPool(hwrtdev.device, tr.cmdpool, NULL);
    if(tr.composepipeline) vkDestroyPipeline(hwrtdev.device, tr.composepipeline, NULL);
    if(tr.lightpipeline) vkDestroyPipeline(hwrtdev.device, tr.lightpipeline, NULL);
    if(tr.shadepipeline) vkDestroyPipeline(hwrtdev.device, tr.shadepipeline, NULL);
    if(tr.aopipeline) vkDestroyPipeline(hwrtdev.device, tr.aopipeline, NULL);
    if(tr.rtpipeline) vkDestroyPipeline(hwrtdev.device, tr.rtpipeline, NULL);
    if(tr.pipeline) vkDestroyPipeline(hwrtdev.device, tr.pipeline, NULL);
    if(tr.composepipelayout) vkDestroyPipelineLayout(hwrtdev.device, tr.composepipelayout, NULL);
    if(tr.lightpipelayout) vkDestroyPipelineLayout(hwrtdev.device, tr.lightpipelayout, NULL);
    if(tr.shadepipelayout) vkDestroyPipelineLayout(hwrtdev.device, tr.shadepipelayout, NULL);
    if(tr.aopipelayout) vkDestroyPipelineLayout(hwrtdev.device, tr.aopipelayout, NULL);
    if(tr.rtpipelayout) vkDestroyPipelineLayout(hwrtdev.device, tr.rtpipelayout, NULL);
    if(tr.pipelayout) vkDestroyPipelineLayout(hwrtdev.device, tr.pipelayout, NULL);
    if(tr.pool) vkDestroyDescriptorPool(hwrtdev.device, tr.pool, NULL);
    if(tr.composesetlayout) vkDestroyDescriptorSetLayout(hwrtdev.device, tr.composesetlayout, NULL);
    if(tr.lightsetlayout) vkDestroyDescriptorSetLayout(hwrtdev.device, tr.lightsetlayout, NULL);
    if(tr.shadesetlayout) vkDestroyDescriptorSetLayout(hwrtdev.device, tr.shadesetlayout, NULL);
    if(tr.aosetlayout) vkDestroyDescriptorSetLayout(hwrtdev.device, tr.aosetlayout, NULL);
    if(tr.rtsetlayout) vkDestroyDescriptorSetLayout(hwrtdev.device, tr.rtsetlayout, NULL);
    if(tr.setlayout) vkDestroyDescriptorSetLayout(hwrtdev.device, tr.setlayout, NULL);
    if(tr.composemodule) vkDestroyShaderModule(hwrtdev.device, tr.composemodule, NULL);
    if(tr.lightmodule) vkDestroyShaderModule(hwrtdev.device, tr.lightmodule, NULL);
    if(tr.shademodule) vkDestroyShaderModule(hwrtdev.device, tr.shademodule, NULL);
    if(tr.aomodule) vkDestroyShaderModule(hwrtdev.device, tr.aomodule, NULL);
    if(tr.rtmodule) vkDestroyShaderModule(hwrtdev.device, tr.rtmodule, NULL);
    if(tr.module) vkDestroyShaderModule(hwrtdev.device, tr.module, NULL);
    memset(&tr, 0, sizeof(tr));
}

void hwrtunbindshared()
{
    tr.boundview = VK_NULL_HANDLE;
    tr.bounddepth = VK_NULL_HANDLE;
}

static void writeimagebinding(VkDescriptorSet set)
{
    if(!set) return;
    VkDescriptorImageInfo iminfo = {};
    iminfo.imageView = hwrtio.view;
    iminfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = &iminfo;
    vkUpdateDescriptorSets(hwrtdev.device, 1, &write, 0, NULL);
}

// RTAO reads the shared depth at binding 2, the lighting shader at binding 9.
static void writedepthbinding(VkDescriptorSet set, uint32_t binding)
{
    if(!set || !hwrtio.depthview) return;
    VkDescriptorImageInfo iminfo = {};
    iminfo.imageView = hwrtio.depthview;
    iminfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = &iminfo;
    vkUpdateDescriptorSets(hwrtdev.device, 1, &write, 0, NULL);
}

// The descriptor points at the shared image, so it has to follow every resize.
// hwrtdestroyshared() waits for the device first, so nothing can be reading it.
// boundview is cleared when the view is destroyed: Vulkan can recycle the same
// handle after a resize, and skipping the write then leaves a black overlay.
static void bindsharedimage()
{
    if(tr.boundview != hwrtio.view)
    {
        writeimagebinding(tr.set);
        loopi(HWRT_FRAMES_IN_FLIGHT)
        {
            writeimagebinding(tr.rtset[i]);
            writeimagebinding(tr.aoset[i]);
            writeimagebinding(tr.shadeset[i]);
            writeimagebinding(tr.lightset[i]);
        }
        tr.boundview = hwrtio.view;
    }
    if(tr.bounddepth != hwrtio.depthview)
    {
        loopi(HWRT_FRAMES_IN_FLIGHT)
        {
            writedepthbinding(tr.aoset[i], 2);
            writedepthbinding(tr.lightset[i], 9);
        }
        tr.bounddepth = hwrtio.depthview;
    }
}

static void writetlas(VkDescriptorSet set, VkAccelerationStructureKHR tlas)
{
    if(!set || tlas == VK_NULL_HANDLE) return;
    VkWriteDescriptorSetAccelerationStructureKHR asinfo = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
    asinfo.accelerationStructureCount = 1;
    asinfo.pAccelerationStructures = &tlas;
    VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    write.pNext = &asinfo;
    write.dstSet = set;
    write.dstBinding = 1;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    vkUpdateDescriptorSets(hwrtdev.device, 1, &write, 0, NULL);
}

static void bindslottlas(int slot, VkAccelerationStructureKHR tlas)
{
    if(slot < 0 || slot >= HWRT_FRAMES_IN_FLIGHT) return;
    if(tr.boundtlas[slot] == tlas) return;
    tr.boundtlas[slot] = tlas;
    writetlas(tr.rtset[slot], tlas);
    writetlas(tr.aoset[slot], tlas);
    writetlas(tr.shadeset[slot], tlas);
    writetlas(tr.lightset[slot], tlas);
}

void hwrtbindtlas()
{
    VkAccelerationStructureKHR tlas = hwrtgettlas();
    loopi(HWRT_FRAMES_IN_FLIGHT) bindslottlas(i, tlas);
}

static float hwrttsms(uint64_t a, uint64_t b)
{
    uint32_t bits = hwrtdev.timestampbits;
    uint64_t mask = bits >= 64 ? ~uint64_t(0) : ((uint64_t(1) << bits) - 1);
    uint64_t d = (b - a) & mask;
    return float(double(d) * double(hwrtdev.timestampperiod) * 1.0e-6);
}

static void harveststamps(int slot)
{
    if(!tr.stamps || hwrtdev.timestampperiod <= 0) return;
    uint64_t ts[HWRT_TS_COUNT];
    VkResult r = vkGetQueryPoolResults(hwrtdev.device, tr.stamps,
                                       uint32_t(slot * HWRT_TS_COUNT), uint32_t(HWRT_TS_COUNT),
                                       sizeof(ts), ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    if(r != VK_SUCCESS) return;
    hwrttime.vkglwait = hwrttsms(ts[HWRT_TS_BEGIN], ts[HWRT_TS_GLWAIT]);
    hwrttime.vkblas = hwrttsms(ts[HWRT_TS_GLWAIT], ts[HWRT_TS_BLAS]);
    hwrttime.vktlas = hwrttsms(ts[HWRT_TS_BLAS], ts[HWRT_TS_TLAS]);
    hwrttime.vklight = hwrttsms(ts[HWRT_TS_TLAS], ts[HWRT_TS_LIGHT]);
    hwrttime.vknrd = hwrttsms(ts[HWRT_TS_LIGHT], ts[HWRT_TS_NRD]);
    hwrttime.vkdispatch = hwrttsms(ts[HWRT_TS_TLAS], ts[HWRT_TS_DISPATCH]);
    hwrttime.vktotal = hwrttsms(ts[HWRT_TS_GLWAIT], ts[HWRT_TS_DISPATCH]);
    hwrttally();
}

void hwrtwritestamp(VkCommandBuffer cmd, int slot, int mark)
{
    if(!cmd || !tr.stamps || !vkCmdWriteTimestamp) return;
    if(slot < 0 || slot >= HWRT_FRAMES_IN_FLIGHT) return;
    if(mark < 0 || mark >= HWRT_TS_COUNT) return;
    VkPipelineStageFlagBits stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    if(mark == HWRT_TS_GLWAIT) stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    else if(mark == HWRT_TS_BLAS || mark == HWRT_TS_TLAS)
        stage = vkCmdBuildAccelerationStructuresKHR ? VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    else if(mark == HWRT_TS_LIGHT || mark == HWRT_TS_NRD) stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    else if(mark == HWRT_TS_DISPATCH) stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    vkCmdWriteTimestamp(cmd, stage, tr.stamps, uint32_t(slot * HWRT_TS_COUNT + mark));
}

// Reclaims the slot we are about to record into. It was submitted
// HWRT_FRAMES_IN_FLIGHT frames ago and the GL/VK ping-pong already forces it to
// have retired, so this normally consumes an already signalled fence. Anything
// else is a genuine CPU stall and gets counted.
static bool reclaimslot(int slot)
{
    if(!tr.pending[slot]) return true;
    if(vkGetFenceStatus(hwrtdev.device, tr.fence[slot]) == VK_NOT_READY)
    {
        hwrtfencestalls++;
        HWRTCHECK(vkWaitForFences(hwrtdev.device, 1, &tr.fence[slot], VK_TRUE, 1000000000ULL), "vkWaitForFences");
    }
    harveststamps(slot);
    HWRTCHECK(vkResetFences(hwrtdev.device, 1, &tr.fence[slot]), "vkResetFences");
    tr.pending[slot] = false;
    return true;
}

// GL wrote the shared depth before it signalled glready, so the wait already
// orders the two queues; this only makes the write visible to the shader read.
static void recorddepthbarrier(VkCommandBuffer cmd)
{
    VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = hwrtio.depthimage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 1, &barrier);
}

static void recordclear(VkCommandBuffer cmd, float r, float g, float b, float a)
{
    VkClearColorValue colour;
    colour.float32[0] = r; colour.float32[1] = g; colour.float32[2] = b; colour.float32[3] = a;
    VkImageSubresourceRange range = {};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;
    vkCmdClearColorImage(cmd, hwrtio.image, VK_IMAGE_LAYOUT_GENERAL, &colour, 1, &range);
}

static void recordprobe(VkCommandBuffer cmd, int mode)
{
    hwrtdebugparams params;
    params.w = hwrtio.w;
    params.h = hwrtio.h;
    params.mode = mode;
    params.frame = tr.frame++;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tr.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tr.pipelayout, 0, 1, &tr.set, 0, NULL);
    vkCmdPushConstants(cmd, tr.pipelayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
    vkCmdDispatch(cmd, (hwrtio.w + 7)/8, (hwrtio.h + 7)/8, 1);
}

static void recordsilhouette(VkCommandBuffer cmd, int mode, int slot)
{
    hwrtsilhouetteparams params;
    memset(&params, 0, sizeof(params));
    params.w = hwrtio.w;
    params.h = hwrtio.h;
    params.mode = mode;
    params.frame = tr.frame++;
    {
        matrix4 invrt;
        hwrttemporalinvcamprojforrt(invrt);
        memcpy(params.invcamproj, &invrt, sizeof(params.invcamproj));
    }
    params.origin[0] = camera1->o.x;
    params.origin[1] = camera1->o.y;
    params.origin[2] = camera1->o.z;
    params.origin[3] = nearplane;
    params.range[0] = float(farplane);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tr.rtpipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tr.rtpipelayout, 0, 1, &tr.rtset[slot], 0, NULL);
    vkCmdPushConstants(cmd, tr.rtpipelayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
    vkCmdDispatch(cmd, (hwrtio.w + 7)/8, (hwrtio.h + 7)/8, 1);
}

static void recordao(VkCommandBuffer cmd, int mode, int slot)
{
    hwrtsilhouetteparams params;
    memset(&params, 0, sizeof(params));
    params.w = hwrtio.w;
    params.h = hwrtio.h;
    params.mode = mode;
    params.frame = tr.frame++;
    {
        matrix4 invrt;
        hwrttemporalinvcamprojforrt(invrt);
        memcpy(params.invcamproj, &invrt, sizeof(params.invcamproj));
    }
    params.origin[0] = camera1->o.x;
    params.origin[1] = camera1->o.y;
    params.origin[2] = camera1->o.z;
    params.origin[3] = rtaobias;
    params.range[0] = rtaoradius;
    params.range[1] = rtaoscale;
    params.range[2] = float(farplane);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tr.aopipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tr.aopipelayout, 0, 1, &tr.aoset[slot], 0, NULL);
    vkCmdPushConstants(cmd, tr.aopipelayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
    vkCmdDispatch(cmd, (hwrtio.w + 7)/8, (hwrtio.h + 7)/8, 1);
}

static void bindshade(int slot)
{
    if(slot < 0 || slot >= HWRT_FRAMES_IN_FLIGHT || !tr.shadeset[slot] || !hwrthasshade()) return;
    hwrtwriteshadebindings(tr.shadeset[slot]);
    tr.boundshadeepoch[slot] = hwrtshadeepoch;
}

// The pair alternates every dispatch, and the descriptor sets are per in-flight
// slot rather than per parity, so the two bindings are rewritten each time.
// reclaimslot() has already retired this slot's fence, so nothing is reading
// the set we are about to touch.
static void writeskyhistory(VkDescriptorSet set, int flip)
{
    if(!set || !skyhist[0].view || !skyhist[1].view || !skyage[0].view || !skyage[1].view) return;
    VkDescriptorImageInfo iminfo[4] = {};
    iminfo[0].imageView = skyhist[flip^1].view;
    iminfo[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    iminfo[1].imageView = skyhist[flip].view;
    iminfo[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    iminfo[2].imageView = skyage[flip^1].view;
    iminfo[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    iminfo[3].imageView = skyage[flip].view;
    iminfo[3].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet writes[4] = {};
    loopi(4)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = uint32_t(13 + i);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo = &iminfo[i];
    }
    vkUpdateDescriptorSets(hwrtdev.device, 4, writes, 0, NULL);
}

static void writenrdimages(VkDescriptorSet set)
{
    if(!set) return;
    if(!nrdimg[0].view)
    {
        createnrdset(1, 1);
        if(!nrdimg[0].view) return;
    }
    VkDescriptorImageInfo iminfo[8] = {};
    VkWriteDescriptorSet writes[8] = {};
    loopi(8)
    {
        iminfo[i].imageView = nrdimg[i].view;
        iminfo[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = uint32_t(17 + i);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo = &iminfo[i];
    }
    vkUpdateDescriptorSets(hwrtdev.device, 8, writes, 0, NULL);
}

static void writecomposebindings(VkDescriptorSet set, int usesrc)
{
    if(!set || !nrdimg[0].view || !nrdimg[4].view) return;
    VkDescriptorImageInfo iminfo[7] = {};
    iminfo[0].imageView = hwrtio.view;
    iminfo[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    int src = usesrc;
    if(src < 0 || src >= HWRT_NRD_IMG_COUNT || !nrdimg[src].view) src = 0;
    iminfo[1].imageView = nrdimg[src].view;
    iminfo[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    loopi(4)
    {
        iminfo[2 + i].imageView = nrdimg[4 + i].view;
        iminfo[2 + i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    iminfo[6].imageView = nrdimg[3].view;
    iminfo[6].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet writes[7] = {};
    const uint32_t bindimg[7] = { 0, 1, 2, 3, 4, 5, 7 };
    loopi(7)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = bindimg[i];
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo = &iminfo[i];
    }
    vkUpdateDescriptorSets(hwrtdev.device, 7, writes, 0, NULL);
    hwrtwritelightbuffer(set);
}

static void bindlight(int slot)
{
    if(slot < 0 || slot >= HWRT_FRAMES_IN_FLIGHT || !tr.lightset[slot] || !hwrthasshade() || !hwrthaslights()) return;
    hwrtwritelightgeombindings(tr.lightset[slot]);
    hwrtwritelightbuffer(tr.lightset[slot]);
    hwrtwritemodelbindings(tr.lightset[slot], slot);
    tr.skyflip ^= 1;
    writeskyhistory(tr.lightset[slot], tr.skyflip);
    writenrdimages(tr.lightset[slot]);
    tr.boundlightepoch[slot] = hwrtshadeepoch;
}

// Orders the previous dispatch's history writes against this one's reads. The
// two submits sit on the same queue but that alone makes nothing visible. On
// the first use after an allocation the contents are worth nothing, so the
// layout comes from UNDEFINED and the driver may throw them away -- the blend
// weight is at seed that frame and reads nothing anyway.
static void recordskyhistbarrier(VkCommandBuffer cmd)
{
    if(!skyhist[0].image || !skyhist[1].image || !skyage[0].image || !skyage[1].image) return;
    VkImageMemoryBarrier bars[4];
    loopi(4)
    {
        hwrtskyhist &img = i < 2 ? skyhist[i] : skyage[i - 2];
        bars[i] = VkImageMemoryBarrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        bars[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bars[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bars[i].image = img.image;
        bars[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bars[i].subresourceRange.levelCount = 1;
        bars[i].subresourceRange.layerCount = 1;
        bars[i].oldLayout = skyhistfresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
        bars[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        bars[i].srcAccessMask = skyhistfresh ? 0 : VK_ACCESS_SHADER_WRITE_BIT;
        bars[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
    }
    vkCmdPipelineBarrier(cmd, skyhistfresh ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 4, bars);
    skyhistfresh = false;
}

static void recordlight(VkCommandBuffer cmd, int mode, int slot)
{
    hwrtlightparams params;
    memset(&params, 0, sizeof(params));
    params.w = hwrtio.w;
    params.h = hwrtio.h;
    // Bit 8 = MASK_MODELS (composite has the two depth snapshots). Bit 9 =
    // SHADE_MODELS (primary rays hit models and the lighting shader shades
    // them). Both can be set: shading wins for the primary cull mask, and
    // MASK_MODELS then means "write model hits at alpha 0.5" so the
    // composite can keep them while still dropping world hits that punched
    // through an untraced mesh. Bit 10 = TEST_GLDEPTH: this frame's window
    // depth reached the shared R32F, so every hit can be compared against
    // what GL actually rasterised. Push constants are 128 bytes; the flags
    // ride in `mode`.
    params.mode = mode;
    if(hwrtmodelshadeready()) params.mode |= HWRT_MODE_SHADE_MODELS;
    if(hwrtmaskready()) params.mode |= HWRT_MODE_MASK_MODELS;
    if(hwrtdepthlive) params.mode |= HWRT_MODE_GLDEPTH;
    params.frame = tr.frame++;
    {
        matrix4 invrt;
        hwrttemporalinvcamprojforrt(invrt);
        memcpy(params.invcamproj, &invrt, sizeof(params.invcamproj));
    }
    params.origin[0] = camera1->o.x;
    params.origin[1] = camera1->o.y;
    params.origin[2] = camera1->o.z;
    params.origin[3] = nearplane;
    params.range[0] = float(farplane);
    params.range[1] = float(hwrtshadediff);
    params.range[2] = float(hwrtdlights);
    params.range[3] = float(hwrtworldtris);
    params.lighting[0] = ambientcolor.x / 255.0f;
    params.lighting[1] = ambientcolor.y / 255.0f;
    params.lighting[2] = ambientcolor.z / 255.0f;
    params.lighting[3] = float(hwrtlightcount);
    if(hwrthdractive()) params.mode |= HWRT_MODE_HDR;
    if(hwrthdractive() && hwrthdrprobe) params.mode |= HWRT_MODE_HDRPROBE;
    hwrthdrpushmode = params.mode;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tr.lightpipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tr.lightpipelayout, 0, 1, &tr.lightset[slot], 0, NULL);
    vkCmdPushConstants(cmd, tr.lightpipelayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
    // hitlight.comp is 16x16 so the skyvis filter has a usable radius. An
    // apron of extra skyvis (12x12 owned tile) was measured slower than
    // four unfiltered rays, so the tile stays 16x16 with no extra traces.
    vkCmdDispatch(cmd, (hwrtio.w + 15)/16, (hwrtio.h + 15)/16, 1);
}

static void recordnrdbarrier(VkCommandBuffer cmd)
{
    if(!nrdimg[0].image) return;
    VkImageMemoryBarrier bars[HWRT_NRD_IMG_COUNT];
    loopi(HWRT_NRD_IMG_COUNT)
    {
        bars[i] = VkImageMemoryBarrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        bars[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bars[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bars[i].image = nrdimg[i].image;
        bars[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bars[i].subresourceRange.levelCount = 1;
        bars[i].subresourceRange.layerCount = 1;
        bars[i].oldLayout = nrdimgfresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
        bars[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        bars[i].srcAccessMask = nrdimgfresh ? 0 : VK_ACCESS_SHADER_WRITE_BIT;
        bars[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
    }
    vkCmdPipelineBarrier(cmd, nrdimgfresh ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, NULL, 0, NULL, HWRT_NRD_IMG_COUNT, bars);
    nrdimgfresh = false;
}

static void recordcompose(VkCommandBuffer cmd, int mode, int slot, int src)
{
    if(!tr.composepipeline || !tr.composeset[slot]) return;
    writecomposebindings(tr.composeset[slot], src);
    hwrtcomposeparams params;
    memset(&params, 0, sizeof(params));
    params.w = hwrtio.w;
    params.h = hwrtio.h;
    params.mode = mode;
    params.dbg = hwrtnrddbg;
    params.lighting[0] = ambientcolor.x / 255.0f;
    params.lighting[1] = ambientcolor.y / 255.0f;
    params.lighting[2] = ambientcolor.z / 255.0f;
    params.lighting[3] = float(hwrtlightcount);
    if(hwrthdractive()) params.mode |= HWRT_MODE_HDR;
    if(hwrthdractive() && hwrthdrprobe) params.mode |= HWRT_MODE_HDRPROBE;
    hwrthdrpushmode = params.mode;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tr.composepipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tr.composepipelayout, 0, 1, &tr.composeset[slot], 0, NULL);
    vkCmdPushConstants(cmd, tr.composepipelayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
    vkCmdDispatch(cmd, (hwrtio.w + 7)/8, (hwrtio.h + 7)/8, 1);
}

static void recordshade(VkCommandBuffer cmd, int mode, int slot)
{
    hwrtsilhouetteparams params;
    memset(&params, 0, sizeof(params));
    params.w = hwrtio.w;
    params.h = hwrtio.h;
    params.mode = mode;
    params.frame = tr.frame++;
    {
        matrix4 invrt;
        hwrttemporalinvcamprojforrt(invrt);
        memcpy(params.invcamproj, &invrt, sizeof(params.invcamproj));
    }
    params.origin[0] = camera1->o.x;
    params.origin[1] = camera1->o.y;
    params.origin[2] = camera1->o.z;
    params.origin[3] = nearplane;
    params.range[0] = float(farplane);
    params.range[1] = float(hwrtshadelm);
    params.range[2] = float(hwrtshadediff);
    params.range[3] = float(hwrtworldtris);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tr.shadepipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tr.shadepipelayout, 0, 1, &tr.shadeset[slot], 0, NULL);
    vkCmdPushConstants(cmd, tr.shadepipelayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
    vkCmdDispatch(cmd, (hwrtio.w + 7)/8, (hwrtio.h + 7)/8, 1);
}

bool hwrtdispatch(int mode)
{
    bool silhouette = mode == HWRT_TRACE_SILHOUETTE && tr.rtpipeline && hwrthasworld();
    bool ao = mode == HWRT_TRACE_RTAO && tr.aopipeline && hwrthasworld() && hwrtio.depthok();
    bool shade = mode == HWRT_TRACE_SHADE && tr.shadepipeline && hwrthasworld() && hwrthasshade();
    bool light = mode == HWRT_TRACE_LIGHT && tr.lightpipeline && tr.lightset[0] && hwrthasworld() && hwrthasshade();
    if(light)
    {
        // Before hwrtupdatelights(), which packs the blend weight and has to
        // see whether the pair it refers to actually exists at this size.
        hwrtensureskyhistory();
        if(hwrtnrd) hwrtnrdensure(hwrtio.w, hwrtio.h);
        hwrtensurenrdguides();
        if(!skyhist[0].view || !skyhist[1].view || !skyage[0].view || !skyage[1].view) light = false;
    }
    if(light)
    {
        hwrtlightrecorded = true;
        double tl = hwrtnow();
        hwrtupdatelights();
        hwrttime.cpulights = float((hwrtnow() - tl) * 1000.0);
        light = hwrthaslights();
    }
    else hwrttime.cpulights = 0;
    if(mode != HWRT_TRACE_CLEAR) bindsharedimage();

    int slot = tr.slot;
    if(!reclaimslot(slot)) return false;

    bool livetlas = false;
    if((silhouette || ao || shade || light) && hwrthasdynents())
    {
        double t0 = hwrtnow();
        livetlas = hwrtpreparedynents(slot);
        hwrttime.cpuprep = float((hwrtnow() - t0) * 1000.0);
        hwrttally();
    }
    else hwrttime.cpuprep = 0;
    if(silhouette || ao || shade || light)
        bindslottlas(slot, livetlas ? hwrtgetslottlas(slot) : hwrtgettlas());
    if(shade) bindshade(slot);
    if(light) bindlight(slot);

    VkCommandBuffer cmd = tr.cmd[slot];

    HWRTCHECK(vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    HWRTCHECK(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");

    if(tr.stamps && vkCmdResetQueryPool)
        vkCmdResetQueryPool(cmd, tr.stamps, uint32_t(slot * HWRT_TS_COUNT), uint32_t(HWRT_TS_COUNT));
    hwrtwritestamp(cmd, slot, HWRT_TS_BEGIN);
    hwrtwritestamp(cmd, slot, HWRT_TS_GLWAIT);
    if(livetlas) hwrtrecordtlas(cmd, slot);
    else
    {
        hwrtwritestamp(cmd, slot, HWRT_TS_BLAS);
        hwrtwritestamp(cmd, slot, HWRT_TS_TLAS);
    }

    VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = hwrtio.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;

    // The dispatch overwrites every pixel, so the previous contents can be
    // discarded and no read-back path has to be kept alive.
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &barrier);

    if(mode == HWRT_TRACE_CLEAR)
    {
        // Bisect probe: no shader, no descriptor, just a fixed colour straight
        // into the shared allocation. If GL still reads zeros after this, the
        // fault is the memory import and nothing downstream matters.
        recordclear(cmd, 1, 0, 1, 1);
    }
    else if(ao)
    {
        recorddepthbarrier(cmd);
        recordao(cmd, mode, slot);
    }
    else if(mode == HWRT_TRACE_SILHOUETTE && tr.rtpipeline)
    {
        if(hwrthasworld()) recordsilhouette(cmd, mode, slot);
        else recordclear(cmd, 0, 0, 0, 0);
    }
    else if(mode == HWRT_TRACE_SHADE && tr.shadepipeline)
    {
        if(hwrthasworld() && hwrthasshade()) recordshade(cmd, mode, slot);
        else recordclear(cmd, 0, 0, 0, 0);
    }
    else if(mode == HWRT_TRACE_LIGHT && tr.lightpipeline && tr.lightset[slot])
    {
        if(light)
        {
            if(hwrtdepthlive) recorddepthbarrier(cmd);
            recordskyhistbarrier(cmd);
            recordnrdbarrier(cmd);
            recordlight(cmd, mode, slot);
            hwrtwritestamp(cmd, slot, HWRT_TS_LIGHT);
            if(hwrtnrdsession() && tr.composepipeline && nrdimgw == hwrtio.w && nrdimgh == hwrtio.h)
            {
                VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
                mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     0, 1, &mb, 0, NULL, 0, NULL);
                bool nrdok = true;
                // dbg 1 / 6: skip REBLUR so compose sees the packed input.
                // 6 is the full-compose identity (same lighting, two skyvis sources).
                if(hwrtnrddbg != 1 && hwrtnrddbg != 6)
                {
                    nrdok = hwrtnrdsetimages(nrdimg[0].image, nrdimg[1].image, nrdimg[2].image, nrdimg[3].image, nrdimg[8].image, hwrtio.w, hwrtio.h)
                         && hwrtnrddenoise(cmd, hwrtio.w, hwrtio.h, 0);
                }
                int src = (hwrtnrddbg == 1 || hwrtnrddbg == 6 || !nrdok) ? 0 : 8;
                recordcompose(cmd, mode, slot, src);
            }
            hwrtwritestamp(cmd, slot, HWRT_TS_NRD);
        }
        else
        {
            recordclear(cmd, 0, 0, 0, 0);
            hwrtwritestamp(cmd, slot, HWRT_TS_LIGHT);
            hwrtwritestamp(cmd, slot, HWRT_TS_NRD);
        }
    }
    else
    {
        recordprobe(cmd, mode == HWRT_TRACE_SILHOUETTE || mode == HWRT_TRACE_RTAO || mode == HWRT_TRACE_SHADE || mode == HWRT_TRACE_LIGHT ? HWRT_TRACE_OVERLAY : mode);
        hwrtwritestamp(cmd, slot, HWRT_TS_LIGHT);
        hwrtwritestamp(cmd, slot, HWRT_TS_NRD);
    }

    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = 0;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, NULL, 0, NULL, 1, &barrier);

    hwrtwritestamp(cmd, slot, HWRT_TS_DISPATCH);
    HWRTCHECK(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

    VkPipelineStageFlags waitstage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &hwrtio.vkglready;
    submit.pWaitDstStageMask = &waitstage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &hwrtio.vkvkdone;
    HWRTCHECK(vkQueueSubmit(hwrtdev.queue, 1, &submit, tr.fence[slot]), "vkQueueSubmit");

    tr.pending[slot] = true;
    tr.slot = (slot + 1)%HWRT_FRAMES_IN_FLIGHT;
    return true;
}
