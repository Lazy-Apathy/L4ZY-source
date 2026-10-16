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
#include "shaders/hitlight_norefl_comp.h"
#include "shaders/hitlight_half_comp.h"
#include "shaders/hitlight_half_norefl_comp.h"
#include "shaders/skycompose_comp.h"
#include "shaders/bluenoise_tab.h"

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
    // hitlight without the traced reflections and world spec, used while both
    // options are off (optional: the full pipeline stands in if it fails).
    VkShaderModule lightmodulenorefl;
    VkPipeline lightpipelinenorefl;
    // RT quality Medium / Low (hwrtskyhalf): the same two, with the sky rays of
    // half the pixels. Created the first time they are wanted; the full-rate
    // pipelines stand in if that fails.
    VkShaderModule lightmodulehalf[2];
    VkPipeline lightpipelinehalf[2];
    bool lighthalftried[2];
    // hitlight with the extended light sources compiled in (specialization
    // constant 0), used while one of their sizes is not 0 (lights.cpp); the
    // half-rate sky one is built on first use.
    VkPipeline lightpipelinesoft, lightpipelinehalfsoft;
    bool lighthalfsofttried;
    // The optimised extended sources (lights.cpp hwrtsoftopt): one pipeline per set of parts compiled in
    // (specialization constants 1-4), full or half-rate sky, built on first use.
    VkPipeline lightpipelineopt[2][8];
    bool lightopttried[2][8];
    // The same without optional parts with 2 to 8 rays for the sun and the extended lamps or for the sun alone
    // (lights.cpp hwrtsoftshadowrays, specialization constants 5 and 6), by full or half-rate sky, rays - 1, lamps.
    VkPipeline lightpipelinerays[2][8][2];
    bool lightraystried[2][8][2];
    // The same with the soft sources' lit fractions handed to NRD SIGMA (specialization constant 7, lights.cpp
    // hwrtsoftsigma) by full or half-rate sky, rays - 1, lamps; and the compose pass that adds them back.
    VkPipeline lightpipelinesigma[2][8][2];
    bool lightsigmatried[2][8][2];
    VkPipeline composepipelinesigma;
    VkCommandPool cmdpool;
    VkCommandBuffer cmd[HWRT_FRAMES_IN_FLIGHT];
    VkFence fence[HWRT_FRAMES_IN_FLIGHT];
    VkQueryPool stamps;
    bool pending[HWRT_FRAMES_IN_FLIGHT];
    VkImageView boundview, bounddepth, boundrefl;
    VkAccelerationStructureKHR boundtlas[HWRT_FRAMES_IN_FLIGHT];
    int boundshadeepoch[HWRT_FRAMES_IN_FLIGHT];
    int boundlightepoch[HWRT_FRAMES_IN_FLIGHT];
    int slot;
    int frame;
    int skyflip;  // which of the two skyvis history images the next dispatch writes
    VkBuffer skybnbuf;        // hitlight binding 25: the sky-ray blue-noise tile
    VkDeviceMemory skybnmem;
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

extern void hwrtdestroysigma();

static void destroynrdimages()
{
    // SIGMA holds views of the guides.
    hwrtdestroysigma();
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

// NRD SIGMA for the soft shadows (lights.cpp hwrtsoftsigma): 0 the extended lamps' light before their shadows (rgb)
// and the sun's N.L (a), 1 the penumbra radius, 2 the noisy lit fractions (sun x, lamps yzw), 3 SIGMA's output.
// Screen sized only while SIGMA runs, 1x1 otherwise: hitlight bindings 30-32 and compose 8-9 always need an image.
enum { HWRT_SIGMA_IMG_COUNT = 4 };
static hwrtskyhist sigimg[HWRT_SIGMA_IMG_COUNT];
static int sigimgw = 0, sigimgh = 0;
static bool sigimgfailed = false, sigimgfresh = true;

static VkFormat sigimgformat(int i)
{
    if(i == 0) return VK_FORMAT_R16G16B16A16_SFLOAT;
    if(i == 1) return VK_FORMAT_R16_SFLOAT;
    return VK_FORMAT_R8G8B8A8_UNORM;
}

static void destroysigimages()
{
    hwrtdestroysigma();
    loopi(HWRT_SIGMA_IMG_COUNT) destroystorage2d(sigimg[i]);
    sigimgw = sigimgh = 0;
}

static bool createsigimages(int w, int h)
{
    VkImageUsageFlags extra = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    loopi(HWRT_SIGMA_IMG_COUNT)
        if(!createstorage2d(sigimg[i], w, h, sigimgformat(i), extra)) { destroysigimages(); return false; }
    sigimgw = w;
    sigimgh = h;
    sigimgfresh = true;
    return true;
}

// Screen sized while want, else the 1x1 placeholders. True when SIGMA's images match the screen.
static bool ensuresigimages(bool want)
{
    if(sigimgfailed) want = false;
    int w = want ? hwrtio.w : 1, h = want ? hwrtio.h : 1;
    if(sigimg[0].view && sigimgw == w && sigimgh == h) return want;
    if(sigimg[0].image && !hwrtwaitidle("vkDeviceWaitIdle (nrd sigma images)")) return false;
    destroysigimages();
    if(want)
    {
        if(createsigimages(w, h)) return true;
        sigimgfailed = true;
        conoutf(CON_WARN, "hwrt nrd sigma: could not allocate the %dx%d images", w, h);
        conoutf(CON_WARN, "Soft shadow denoiser off: not enough video memory.");
    }
    createsigimages(1, 1);
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

// The sky-ray blue-noise tile (hitlight binding 25). 64 KB, written once and
// never touched again. The shader names the binding unconditionally, so the
// light pipeline does not come up without it.
static void destroyskybluenoise()
{
    if(tr.skybnbuf) vkDestroyBuffer(hwrtdev.device, tr.skybnbuf, NULL);
    if(tr.skybnmem) vkFreeMemory(hwrtdev.device, tr.skybnmem, NULL);
    tr.skybnbuf = VK_NULL_HANDLE;
    tr.skybnmem = VK_NULL_HANDLE;
}

static bool initskybluenoise()
{
    VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    info.size = sizeof(hwrtskybluenoisetab);
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult r = vkCreateBuffer(hwrtdev.device, &info, NULL, &tr.skybnbuf);
    if(r != VK_SUCCESS) { conoutf(CON_WARN, "hwrt: sky blue-noise buffer failed (%s)", hwrtresultstr(r)); tr.skybnbuf = VK_NULL_HANDLE; return false; }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(hwrtdev.device, tr.skybnbuf, &req);
    // VRAM the CPU can write once if the device has it, host memory otherwise.
    int memtype = hwrtfindmemtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT|VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if(memtype < 0) memtype = hwrtfindmemtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if(memtype < 0) { conoutf(CON_WARN, "hwrt: no host-visible memory for the sky blue-noise tile"); destroyskybluenoise(); return false; }
    VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = uint32_t(memtype);
    r = vkAllocateMemory(hwrtdev.device, &alloc, NULL, &tr.skybnmem);
    if(r != VK_SUCCESS) { conoutf(CON_WARN, "hwrt: sky blue-noise memory failed (%s)", hwrtresultstr(r)); tr.skybnmem = VK_NULL_HANDLE; destroyskybluenoise(); return false; }
    r = vkBindBufferMemory(hwrtdev.device, tr.skybnbuf, tr.skybnmem, 0);
    void *mapped = NULL;
    if(r == VK_SUCCESS) r = vkMapMemory(hwrtdev.device, tr.skybnmem, 0, sizeof(hwrtskybluenoisetab), 0, &mapped);
    if(r != VK_SUCCESS || !mapped) { conoutf(CON_WARN, "hwrt: sky blue-noise upload failed (%s)", hwrtresultstr(r)); destroyskybluenoise(); return false; }
    memcpy(mapped, hwrtskybluenoisetab, sizeof(hwrtskybluenoisetab));
    vkUnmapMemory(hwrtdev.device, tr.skybnmem);
    return true;
}

bool hwrtskybluenoiseready()
{
    return tr.skybnbuf != VK_NULL_HANDLE && tr.lightpipeline != VK_NULL_HANDLE;
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

    if(!initskybluenoise()) return false;

    VkDescriptorSetLayoutBinding bindings[33];
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
    bindings[25].binding = 25;
    bindings[25].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[25].descriptorCount = 1;
    bindings[25].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    // Traced reflections: 26 is the water reflection image (shared with GL),
    // 27 the sky cubemap a reflected ray sees when it escapes, 28 the normal
    // maps of the world's spec / envmap faces.
    bindings[26].binding = 26;
    bindings[26].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[26].descriptorCount = 1;
    bindings[26].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[27].binding = 27;
    bindings[27].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[27].descriptorCount = 1;
    bindings[27].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[28].binding = 28;
    bindings[28].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[28].descriptorCount = 1;
    bindings[28].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    // 29: the world diffuse through the anisotropic sampler (hwrtdiffmip 2).
    bindings[29].binding = 29;
    bindings[29].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[29].descriptorCount = 1;
    bindings[29].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    // 30-32: the soft shadows' data for NRD SIGMA (written by the SOFT_SIGMA pipeline alone).
    for(int i = 30; i < 33; i++)
    {
        bindings[i].binding = uint32_t(i);
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo setinfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    setinfo.bindingCount = 33;
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

    // Same layout, same bindings, the shader built without the reflection and
    // world spec code: with RT Reflections and RT Specular off the lighting
    // pass then costs what it did before they existed (a bigger shader is
    // slower even when its extra branches are never taken).
    modinfo.codeSize = sizeof(hwrthitlightnoreflspv);
    modinfo.pCode = hwrthitlightnoreflspv;
    r = vkCreateShaderModule(hwrtdev.device, &modinfo, NULL, &tr.lightmodulenorefl);
    if(r == VK_SUCCESS)
    {
        pipeinfo.stage.module = tr.lightmodulenorefl;
        r = vkCreateComputePipelines(hwrtdev.device, VK_NULL_HANDLE, 1, &pipeinfo, NULL, &tr.lightpipelinenorefl);
    }
    if(r != VK_SUCCESS)
    {
        conoutf(CON_WARN, "hwrt: hit-light pipeline without reflections failed (%s), the full one is used", hwrtresultstr(r));
        tr.lightpipelinenorefl = VK_NULL_HANDLE;
    }

    // The full shader with the extended sources on (SOFT_SOURCES = true).
    VkSpecializationMapEntry softentry = { 0, 0, sizeof(VkBool32) };
    VkBool32 softon = VK_TRUE;
    VkSpecializationInfo softspec = { 1, &softentry, sizeof(softon), &softon };
    pipeinfo.stage.module = tr.lightmodule;
    pipeinfo.stage.pSpecializationInfo = &softspec;
    r = vkCreateComputePipelines(hwrtdev.device, VK_NULL_HANDLE, 1, &pipeinfo, NULL, &tr.lightpipelinesoft);
    if(r != VK_SUCCESS)
    {
        conoutf(CON_WARN, "hwrt: hit-light pipeline with extended light sources failed (%s), point sources are used", hwrtresultstr(r));
        tr.lightpipelinesoft = VK_NULL_HANDLE;
    }
    return true;
}

// The optional hit-light variants: without reflections, with extended sources.
static void destroylightnorefl()
{
    if(tr.lightpipelinesoft) { vkDestroyPipeline(hwrtdev.device, tr.lightpipelinesoft, NULL); tr.lightpipelinesoft = VK_NULL_HANDLE; }
    if(tr.lightpipelinehalfsoft) { vkDestroyPipeline(hwrtdev.device, tr.lightpipelinehalfsoft, NULL); tr.lightpipelinehalfsoft = VK_NULL_HANDLE; }
    tr.lighthalfsofttried = false;
    loopi(2) loopj(8)
    {
        if(tr.lightpipelineopt[i][j]) { vkDestroyPipeline(hwrtdev.device, tr.lightpipelineopt[i][j], NULL); tr.lightpipelineopt[i][j] = VK_NULL_HANDLE; }
        tr.lightopttried[i][j] = false;
        loopk(2)
        {
            if(tr.lightpipelinerays[i][j][k]) { vkDestroyPipeline(hwrtdev.device, tr.lightpipelinerays[i][j][k], NULL); tr.lightpipelinerays[i][j][k] = VK_NULL_HANDLE; }
            tr.lightraystried[i][j][k] = false;
            if(tr.lightpipelinesigma[i][j][k]) { vkDestroyPipeline(hwrtdev.device, tr.lightpipelinesigma[i][j][k], NULL); tr.lightpipelinesigma[i][j][k] = VK_NULL_HANDLE; }
            tr.lightsigmatried[i][j][k] = false;
        }
    }
    if(tr.lightpipelinenorefl) { vkDestroyPipeline(hwrtdev.device, tr.lightpipelinenorefl, NULL); tr.lightpipelinenorefl = VK_NULL_HANDLE; }
    if(tr.lightmodulenorefl) { vkDestroyShaderModule(hwrtdev.device, tr.lightmodulenorefl, NULL); tr.lightmodulenorefl = VK_NULL_HANDLE; }
    loopi(2)
    {
        if(tr.lightpipelinehalf[i]) { vkDestroyPipeline(hwrtdev.device, tr.lightpipelinehalf[i], NULL); tr.lightpipelinehalf[i] = VK_NULL_HANDLE; }
        if(tr.lightmodulehalf[i]) { vkDestroyShaderModule(hwrtdev.device, tr.lightmodulehalf[i], NULL); tr.lightmodulehalf[i] = VK_NULL_HANDLE; }
        tr.lighthalftried[i] = false;
    }
}

// The half-rate sky variant of the lighting pipeline (norefl: without the
// reflection code), built on first use with the layout of the full one.
static VkPipeline lighthalfpipeline(bool norefl)
{
    int k = norefl ? 1 : 0;
    if(tr.lightpipelinehalf[k] || tr.lighthalftried[k] || !tr.lightpipelayout) return tr.lightpipelinehalf[k];
    tr.lighthalftried[k] = true;
    VkShaderModuleCreateInfo modinfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    modinfo.codeSize = norefl ? sizeof(hwrthitlighthalfnoreflspv) : sizeof(hwrthitlighthalfspv);
    modinfo.pCode = norefl ? hwrthitlighthalfnoreflspv : hwrthitlighthalfspv;
    VkResult r = vkCreateShaderModule(hwrtdev.device, &modinfo, NULL, &tr.lightmodulehalf[k]);
    if(r == VK_SUCCESS)
    {
        VkComputePipelineCreateInfo pipeinfo = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        pipeinfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipeinfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipeinfo.stage.module = tr.lightmodulehalf[k];
        pipeinfo.stage.pName = "main";
        pipeinfo.layout = tr.lightpipelayout;
        r = vkCreateComputePipelines(hwrtdev.device, VK_NULL_HANDLE, 1, &pipeinfo, NULL, &tr.lightpipelinehalf[k]);
    }
    if(r != VK_SUCCESS)
    {
        conoutf(CON_WARN, "hwrt: hit-light pipeline for RT quality below High failed (%s), the full one is used", hwrtresultstr(r));
        tr.lightpipelinehalf[k] = VK_NULL_HANDLE;
    }
    return tr.lightpipelinehalf[k];
}

// The half-rate sky variant with the extended light sources compiled in (the
// half module with specialization constant 0), built on first use.
static VkPipeline lighthalfsoftpipeline()
{
    if(tr.lightpipelinehalfsoft || tr.lighthalfsofttried || !tr.lightpipelayout) return tr.lightpipelinehalfsoft;
    tr.lighthalfsofttried = true;
    if(!lighthalfpipeline(false) || !tr.lightmodulehalf[0]) return VK_NULL_HANDLE;
    VkSpecializationMapEntry softentry = { 0, 0, sizeof(VkBool32) };
    VkBool32 softon = VK_TRUE;
    VkSpecializationInfo softspec = { 1, &softentry, sizeof(softon), &softon };
    VkComputePipelineCreateInfo pipeinfo = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    pipeinfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeinfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeinfo.stage.module = tr.lightmodulehalf[0];
    pipeinfo.stage.pName = "main";
    pipeinfo.stage.pSpecializationInfo = &softspec;
    pipeinfo.layout = tr.lightpipelayout;
    VkResult r = vkCreateComputePipelines(hwrtdev.device, VK_NULL_HANDLE, 1, &pipeinfo, NULL, &tr.lightpipelinehalfsoft);
    if(r != VK_SUCCESS)
    {
        conoutf(CON_WARN, "hwrt: hit-light pipeline with extended light sources for RT quality below High failed (%s)", hwrtresultstr(r));
        tr.lightpipelinehalfsoft = VK_NULL_HANDLE;
    }
    return tr.lightpipelinehalfsoft;
}

// The optimised extended sources: SOFT_SOURCES and SOFT_LEAN on, plus SOFT_DETECT (parts bit 0), SOFT_FILTER
// (bit 1), SOFT_TEMPORAL (bit 2), on the full module or on the half-rate sky one.
// rays: 2 to 8 rays for the sun (specialization constant 5) and, with lamps, for each extended lamp (constant 6),
// without optional parts only. sigma: the soft sources' lit fractions for NRD SIGMA (constant 7), without parts.
static VkPipeline lightoptpipeline(bool half, int parts, int rays = 1, bool lamps = false, bool sigma = false)
{
    parts &= 7;
    rays = parts ? 1 : clamp(rays, 1, 8);
    if(parts) sigma = false;
    if(rays == 1) lamps = false;
    int lk = lamps ? 1 : 0;
    VkPipeline &pipe = sigma ? tr.lightpipelinesigma[half ? 1 : 0][rays - 1][lk] : rays > 1 ? tr.lightpipelinerays[half ? 1 : 0][rays - 1][lk] : tr.lightpipelineopt[half ? 1 : 0][parts];
    bool &tried = sigma ? tr.lightsigmatried[half ? 1 : 0][rays - 1][lk] : rays > 1 ? tr.lightraystried[half ? 1 : 0][rays - 1][lk] : tr.lightopttried[half ? 1 : 0][parts];
    if(pipe || tried || !tr.lightpipelayout) return pipe;
    tried = true;
    if(half && (!lighthalfpipeline(false) || !tr.lightmodulehalf[0])) return VK_NULL_HANDLE;
    VkShaderModule module = half ? tr.lightmodulehalf[0] : tr.lightmodule;
    if(!module) return VK_NULL_HANDLE;
    VkSpecializationMapEntry entries[8];
    VkBool32 values[8] = { VK_TRUE, VK_TRUE, (parts & 1) ? VK_TRUE : VK_FALSE, (parts & 2) ? VK_TRUE : VK_FALSE, (parts & 4) ? VK_TRUE : VK_FALSE, VkBool32(rays), VkBool32(lamps ? rays : 1), sigma ? VK_TRUE : VK_FALSE };
    loopi(8) { entries[i].constantID = uint32_t(i); entries[i].offset = uint32_t(i * sizeof(VkBool32)); entries[i].size = sizeof(VkBool32); }
    // One ray: the five constants of before (constants 5 and 6 keep their default, 1); SIGMA: all eight.
    uint32_t nspec = sigma ? 8 : rays > 1 ? 7 : 5;
    VkSpecializationInfo spec = { nspec, entries, nspec * uint32_t(sizeof(VkBool32)), values };
    VkComputePipelineCreateInfo pipeinfo = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    pipeinfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeinfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeinfo.stage.module = module;
    pipeinfo.stage.pName = "main";
    pipeinfo.stage.pSpecializationInfo = &spec;
    pipeinfo.layout = tr.lightpipelayout;
    VkResult r = vkCreateComputePipelines(hwrtdev.device, VK_NULL_HANDLE, 1, &pipeinfo, NULL, &pipe);
    if(r != VK_SUCCESS)
    {
        conoutf(CON_WARN, "hwrt: hit-light pipeline with optimised extended light sources (%d, %d rays%s%s) failed (%s), the prototype's rays are used", parts, rays, lamps ? "" : ", sun", sigma ? ", SIGMA" : "", hwrtresultstr(r));
        pipe = VK_NULL_HANDLE;
    }
    return pipe;
}

static bool initcomposepipeline()
{
    if(!hwrtdev.rayquery) return true;
    VkShaderModuleCreateInfo modinfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    modinfo.codeSize = sizeof(hwrtskycomposespv);
    modinfo.pCode = hwrtskycomposespv;
    VkResult r = vkCreateShaderModule(hwrtdev.device, &modinfo, NULL, &tr.composemodule);
    if(r != VK_SUCCESS) { conoutf(CON_WARN, "hwrt: skycompose shader failed (%s)", hwrtresultstr(r)); return false; }

    VkDescriptorSetLayoutBinding bindings[10];
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
    // 8-9: the soft shadows from NRD SIGMA (read by the SIGMA_ON pipeline alone).
    for(int i = 8; i < 10; i++)
    {
        bindings[i].binding = uint32_t(i);
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo setinfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    setinfo.bindingCount = 10;
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
    // The same adding the soft shadows denoised by NRD SIGMA (specialization constant 0); optional.
    VkSpecializationMapEntry sigentry = { 0, 0, sizeof(VkBool32) };
    VkBool32 sigon = VK_TRUE;
    VkSpecializationInfo sigspec = { 1, &sigentry, sizeof(sigon), &sigon };
    pipeinfo.stage.pSpecializationInfo = &sigspec;
    r = vkCreateComputePipelines(hwrtdev.device, VK_NULL_HANDLE, 1, &pipeinfo, NULL, &tr.composepipelinesigma);
    if(r != VK_SUCCESS)
    {
        conoutf(CON_WARN, "hwrt: skycompose pipeline with soft shadow denoising failed (%s)", hwrtresultstr(r));
        tr.composepipelinesigma = VK_NULL_HANDLE;
    }
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
        destroylightnorefl();
        if(tr.lightpipelayout) { vkDestroyPipelineLayout(hwrtdev.device, tr.lightpipelayout, NULL); tr.lightpipelayout = VK_NULL_HANDLE; }
        if(tr.lightsetlayout) { vkDestroyDescriptorSetLayout(hwrtdev.device, tr.lightsetlayout, NULL); tr.lightsetlayout = VK_NULL_HANDLE; }
        if(tr.lightmodule) { vkDestroyShaderModule(hwrtdev.device, tr.lightmodule, NULL); tr.lightmodule = VK_NULL_HANDLE; }
        destroyskybluenoise();
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
    // Six SSBOs: verts, indices, tris, lights, model geometry, sky blue noise.
    // Plus the reflection image, the sky cubemap and the world normal maps
    // (traced reflections and world spec), and the anisotropic diffuse (29).
    // Plus the three images of the soft shadows for NRD SIGMA (light set) and its two for the compose set.
    if(tr.lightpipeline) { nstorage += 18*copies; nas += copies; nssbo += 6*copies; ncombined += 8*copies; maxsets += copies; }
    if(tr.composepipeline) { nstorage += 9*copies; nssbo += copies; maxsets += copies; }
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
                destroylightnorefl();
                break;
            }
        }
        // Binding 25 never changes: write it once per set.
        if(tr.lightpipeline) loopi(HWRT_FRAMES_IN_FLIGHT)
        {
            VkDescriptorBufferInfo bninfo = { tr.skybnbuf, 0, VK_WHOLE_SIZE };
            VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            write.dstSet = tr.lightset[i];
            write.dstBinding = 25;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = &bninfo;
            vkUpdateDescriptorSets(hwrtdev.device, 1, &write, 0, NULL);
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
    if(!hwrtdev.device) { memset(&tr, 0, sizeof(tr)); memset(skyhist, 0, sizeof(skyhist)); memset(skyage, 0, sizeof(skyage)); skyhistw = skyhisth = 0; memset(nrdimg, 0, sizeof(nrdimg)); nrdimgw = nrdimgh = 0; memset(sigimg, 0, sizeof(sigimg)); sigimgw = sigimgh = 0; return; }
    vkDeviceWaitIdle(hwrtdev.device);
    hwrtdestroynrd();
    destroyskyhistory();
    skyhistfailed = false;
    hwrtinvalidateskyhistory();
    destroynrdimages();
    nrdimgfailed = false;
    destroysigimages();
    sigimgfailed = false;

    loopi(HWRT_FRAMES_IN_FLIGHT) if(tr.fence[i]) vkDestroyFence(hwrtdev.device, tr.fence[i], NULL);
    if(tr.stamps) vkDestroyQueryPool(hwrtdev.device, tr.stamps, NULL);
    if(tr.cmdpool) vkDestroyCommandPool(hwrtdev.device, tr.cmdpool, NULL);
    if(tr.composepipeline) vkDestroyPipeline(hwrtdev.device, tr.composepipeline, NULL);
    if(tr.composepipelinesigma) vkDestroyPipeline(hwrtdev.device, tr.composepipelinesigma, NULL);
    if(tr.lightpipeline) vkDestroyPipeline(hwrtdev.device, tr.lightpipeline, NULL);
    destroylightnorefl();
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
    destroyskybluenoise();
    memset(&tr, 0, sizeof(tr));
}

void hwrtunbindshared()
{
    tr.boundview = VK_NULL_HANDLE;
    tr.bounddepth = VK_NULL_HANDLE;
    tr.boundrefl = VK_NULL_HANDLE;
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
// Traced reflections. Binding 25 must always name a valid rgba16f storage
// image because the shader uses it statically; the result image stands in
// when the reflection image could not be created (the shader then never
// writes it: the mode bit stays off).
static void writereflbinding(VkDescriptorSet set, VkImageView view)
{
    if(!set || !view) return;
    VkDescriptorImageInfo iminfo = {};
    iminfo.imageView = view;
    iminfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    write.dstSet = set;
    write.dstBinding = 26;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = &iminfo;
    vkUpdateDescriptorSets(hwrtdev.device, 1, &write, 0, NULL);
}

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
    VkImageView reflview = hwrtreflview() ? hwrtreflview() : hwrtio.view;
    if(tr.boundrefl != reflview)
    {
        loopi(HWRT_FRAMES_IN_FLIGHT) writereflbinding(tr.lightset[i], reflview);
        tr.boundrefl = reflview;
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
        double t0 = latency_watchblocks() ? latency_now() : 0;
        HWRTCHECK(vkWaitForFences(hwrtdev.device, 1, &tr.fence[slot], VK_TRUE, 1000000000ULL), "vkWaitForFences");
        if(t0) latency_blocked(t0, latency_now());
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
    // 30-32: the soft shadows' images for NRD SIGMA (1x1 while it is off).
    if(!sigimg[0].view) createsigimages(1, 1);
    if(!sigimg[0].view) return;
    VkDescriptorImageInfo siginfo[3] = {};
    VkWriteDescriptorSet sigwrites[3] = {};
    loopi(3)
    {
        siginfo[i].imageView = sigimg[i].view;
        siginfo[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        sigwrites[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        sigwrites[i].dstSet = set;
        sigwrites[i].dstBinding = uint32_t(30 + i);
        sigwrites[i].descriptorCount = 1;
        sigwrites[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        sigwrites[i].pImageInfo = &siginfo[i];
    }
    vkUpdateDescriptorSets(hwrtdev.device, 3, sigwrites, 0, NULL);
}

// sigsrc: the lit fractions the SIGMA_ON pipeline reads at binding 9 (3 SIGMA's output, 2 hitlight's noisy ones).
static void writecomposebindings(VkDescriptorSet set, int usesrc, int sigsrc = 2)
{
    if(!set || !nrdimg[0].view || !nrdimg[4].view) return;
    if(!sigimg[0].view) createsigimages(1, 1);
    if(!sigimg[0].view) return;
    VkDescriptorImageInfo iminfo[9] = {};
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
    iminfo[7].imageView = sigimg[0].view;
    iminfo[7].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    iminfo[8].imageView = sigimg[sigsrc == 3 ? 3 : 2].view;
    iminfo[8].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet writes[9] = {};
    const uint32_t bindimg[9] = { 0, 1, 2, 3, 4, 5, 7, 8, 9 };
    loopi(9)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = bindimg[i];
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo = &iminfo[i];
    }
    vkUpdateDescriptorSets(hwrtdev.device, 9, writes, 0, NULL);
    hwrtwritelightbuffer(set);
}

static void bindlight(int slot)
{
    if(slot < 0 || slot >= HWRT_FRAMES_IN_FLIGHT || !tr.lightset[slot] || !hwrthasshade() || !hwrthaslights()) return;
    hwrtwritelightgeombindings(tr.lightset[slot]);
    hwrtwritelightbuffer(tr.lightset[slot]);
    hwrtwritemodelbindings(tr.lightset[slot], slot);
    hwrtwriteskyenvbinding(tr.lightset[slot]);
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

// Performance options, all off by default (the image changes).
// hwrtskyrange: how far a sky ray looks for an occluder (world units, 0 = to the
// far plane). Beyond that it counts as open sky: far less traversal outdoors,
// big halls with a ceiling higher than this get some sky light.
VARP(hwrtskyrange, 0, 0, 2016);
// hwrtrtadapt: rays spent where they show (a "variable rate" for the RT
// lighting). Sky rays reach less far at the screen edges, on far surfaces and on
// dark textures (1024 units at most when hwrtskyrange is 0); while the camera
// turns or moves fast, where the upscaler blurs anyway, the sky rays reach half
// as far and at most 2 nearest lamps cast shadow rays.
VARP(hwrtrtadapt, 0, 0, 1);
// hwrtskyopaque: sky rays treat alpha-tested models (leaves, grids) as solid and
// ignore smoke. Every candidate then stays in the ray-tracing hardware instead of
// coming back to the shader for an alpha test: cheaper under trees, and a denser
// sky shadow below foliage.
VARP(hwrtskyopaque, 0, 0, 1);
// Turn rate (degrees/s) and speed (units/s) above which the camera counts as fast.
VARP(hwrtrtadaptturn, 30, 240, 2000);
VARP(hwrtrtadaptspeed, 50, 400, 5000);

static bool camerafast()
{
    static vec lastpos(0, 0, 0);
    static float lastyaw = 0, lastpitch = 0;
    static double lasttime = 0;
    static bool fast = false;
    double now = hwrtnow();
    if(!camera1) return false;
    if(lasttime > 0 && now > lasttime)
    {
        float dt = float(now - lasttime);
        float dyaw = fabs(camera1->yaw - lastyaw);
        if(dyaw > 180) dyaw = 360 - dyaw;
        float turn = (dyaw + fabs(camera1->pitch - lastpitch)) / dt;
        float speed = camera1->o.dist(lastpos) / dt;
        fast = turn > hwrtrtadaptturn || speed > hwrtrtadaptspeed;
    }
    lastpos = camera1->o;
    lastyaw = camera1->yaw;
    lastpitch = camera1->pitch;
    lasttime = now;
    return fast;
}

// The SOFT_SIGMA lighting pipeline when NRD SIGMA runs this frame (hwrtdispatch), else null.
static VkPipeline sigmalightpipe = VK_NULL_HANDLE;

// The SOFT_SIGMA pipeline for the current rays and sky rate (null if it cannot be built).
static VkPipeline softsigmapipeline()
{
    extern int hwrtsoftshadowrays;
    extern bool hwrtsoftlampsextended();
    int rays = clamp(hwrtsoftshadowrays, 1, 8);
    bool lamps = hwrtsoftlampsextended();
    VkPipeline p = VK_NULL_HANDLE;
    if(hwrtskyhalf()) p = lightoptpipeline(true, 0, rays, lamps, true);
    if(!p) p = lightoptpipeline(false, 0, rays, lamps, true);
    return p;
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
    // Traced reflections. The plane count rides above the flags; the planes
    // themselves were packed behind the lights by hwrtupdatelights().
    if(hwrtreflections && hwrtreflview() && hwrtwaterplanecount > 0)
    {
        params.mode |= HWRT_MODE_REFL_WATER | (min(hwrtwaterplanecount, int(HWRT_MAX_WATERPLANES)) << HWRT_MODE_REFL_PLANESHIFT);
        hwrtsetreflwritten(true);
    }
    if(hwrtreflections) params.mode |= HWRT_MODE_REFL_WORLD;
    if(hwrtspecular) params.mode |= HWRT_MODE_SPEC;
    if(hwrtskyrange > 0) params.mode |= clamp((hwrtskyrange + 31) / 32, 1, 63) << HWRT_MODE_SKYRANGE_SHIFT;
    if(hwrtskyopaque) params.mode |= HWRT_MODE_SKYOPAQUE;
    bool fast = camerafast();
    if(hwrtrtadapt)
    {
        params.mode |= HWRT_MODE_RT_ADAPT;
        if(fast)
        {
            params.mode |= HWRT_MODE_RT_FAST;
            params.range[2] = float(min(int(hwrtdlights), 2));
        }
    }

    // Both options off: the variant without their code (same image as before
    // they existed); either one on: the full shader.
    bool norefl = !hwrtreflections && !hwrtspecular && tr.lightpipelinenorefl;
    VkPipeline lightpipe = norefl ? tr.lightpipelinenorefl : tr.lightpipeline;
    // RT quality Medium / Low: the half-rate sky variant when it could be built.
    if(hwrtskyhalf())
    {
        VkPipeline half = lighthalfpipeline(norefl);
        if(half) lightpipe = half;
    }
    // An extended light source size not 0: the full shader with their code
    // (the reflection code stays, its mode bits are off), at the same sky rate.
    extern bool hwrtsoftactive();
    extern int hwrtsoftopt, hwrtsoftshadowrays;
    extern bool hwrtsoftlampsextended();
    if(hwrtsoftactive())
    {
        // hwrtsoftopt bit 0: the optimised rays, with the parts it asks for (the temporal part needs the filter),
        // and without parts as many rays as hwrtsoftshadowrays for the sun and, when one may be extended, the lamps
        // (one ray if that pipeline failed).
        // NRD SIGMA this frame: its pipeline, chosen by hwrtdispatch.
        VkPipeline soft = sigmalightpipe;
        if(!soft && (hwrtsoftopt & 1))
        {
            int parts = ((hwrtsoftopt >> 1) & 3) | ((hwrtsoftopt & 12) == 12 ? 4 : 0);
            int rays = parts ? 1 : clamp(hwrtsoftshadowrays, 1, 8);
            bool lamps = hwrtsoftlampsextended();
            if(hwrtskyhalf() && rays > 1) soft = lightoptpipeline(true, parts, rays, lamps);
            if(!soft && rays > 1) soft = lightoptpipeline(false, parts, rays, lamps);
            if(!soft && hwrtskyhalf()) soft = lightoptpipeline(true, parts);
            if(!soft) soft = lightoptpipeline(false, parts);
        }
        if(!soft && hwrtskyhalf()) soft = lighthalfsoftpipeline();
        if(!soft) soft = tr.lightpipelinesoft;
        if(soft) lightpipe = soft;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, lightpipe);
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

// sigsrc: -1 without NRD SIGMA, else the image of the lit fractions to add back (3 SIGMA's output, 2 the noisy input).
static void recordsigbarrier(VkCommandBuffer cmd)
{
    if(!sigimg[0].image) return;
    VkImageMemoryBarrier bars[HWRT_SIGMA_IMG_COUNT];
    loopi(HWRT_SIGMA_IMG_COUNT)
    {
        bars[i] = VkImageMemoryBarrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        bars[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bars[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bars[i].image = sigimg[i].image;
        bars[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bars[i].subresourceRange.levelCount = 1;
        bars[i].subresourceRange.layerCount = 1;
        bars[i].oldLayout = sigimgfresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
        bars[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        bars[i].srcAccessMask = sigimgfresh ? 0 : VK_ACCESS_SHADER_WRITE_BIT;
        bars[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
    }
    vkCmdPipelineBarrier(cmd, sigimgfresh ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, NULL, 0, NULL, HWRT_SIGMA_IMG_COUNT, bars);
    sigimgfresh = false;
}

static void recordcompose(VkCommandBuffer cmd, int mode, int slot, int src, int sigsrc = -1)
{
    if(!tr.composepipeline || !tr.composeset[slot]) return;
    writecomposebindings(tr.composeset[slot], src, sigsrc);
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
    VkPipeline composepipe = tr.composepipeline;
    if(sigsrc >= 0 && tr.composepipelinesigma)
    {
        composepipe = tr.composepipelinesigma;
        // Bit 28: SIGMA did not run, the fractions are the noisy ones (not square root encoded).
        if(sigsrc != 3) params.mode |= 0x10000000;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composepipe);
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
        // NRD SIGMA for the soft shadows this frame: asked for (lights.cpp), the REBLUR session and its guides at
        // this size, SIGMA's images, its instance and the SOFT_SIGMA pipeline. Otherwise the soft shadows of before.
        extern bool hwrtsoftsigmawanted();
        extern bool hwrtsigmaensure(int w, int h);
        sigmalightpipe = VK_NULL_HANDLE;
        bool sigwant = light && hwrtsoftsigmawanted() && hwrtnrdsession() && tr.composepipelinesigma &&
                       hwrtnrdguidesready() && nrdimgw == hwrtio.w && nrdimgh == hwrtio.h;
        if(ensuresigimages(sigwant) && hwrtsigmaensure(hwrtio.w, hwrtio.h)) sigmalightpipe = softsigmapipeline();
        extern int hwrtsigmaactive;
        if(!sigmalightpipe) hwrtsigmaactive = 0;
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
    // Traced reflections: same treatment for the reflection image. Every
    // pixel is rewritten whenever GL is told to read it. Reflections off: the
    // image is left alone (hwrtsignalgl / hwrtwaitgl do not hand it over).
    VkImageMemoryBarrier reflbarrier = barrier;
    reflbarrier.image = hwrtreflections ? hwrtreflimage() : VK_NULL_HANDLE;
    reflbarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    if(reflbarrier.image)
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, NULL, 0, NULL, 1, &reflbarrier);

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
            recordsigbarrier(cmd);
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
                // NRD SIGMA: denoise the soft sources' lit fractions hitlight wrote; if that fails, the noisy ones.
                extern bool hwrtsigmasetimages(VkImage pen, VkImage frac, VkImage viewz, VkImage normal, VkImage mv, VkImage out, int w, int h);
                extern bool hwrtsigmadenoise(VkCommandBuffer cmd, int w, int h, int frames);
                extern int hwrtsoftsigmahistory, hwrtsigmaactive;
                int sigsrc = -1;
                if(sigmalightpipe)
                {
                    bool sok = hwrtsigmasetimages(sigimg[1].image, sigimg[2].image, nrdimg[1].image, nrdimg[2].image, nrdimg[3].image, sigimg[3].image, hwrtio.w, hwrtio.h)
                            && hwrtsigmadenoise(cmd, hwrtio.w, hwrtio.h, hwrtsoftsigmahistory);
                    sigsrc = sok ? 3 : 2;
                }
                hwrtsigmaactive = sigsrc == 3 ? 1 : 0;
                recordcompose(cmd, mode, slot, src, sigsrc);
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
    if(reflbarrier.image)
    {
        reflbarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        reflbarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        reflbarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        reflbarrier.dstAccessMask = 0;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, NULL, 0, NULL, 1, &reflbarrier);
    }

    hwrtwritestamp(cmd, slot, HWRT_TS_DISPATCH);
    HWRTCHECK(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

    // Everything above is CPU work: with hwrtperfpath bit 1 the GL queue is only
    // handed over now, so the GPU does not wait for it between the two APIs.
    if(hwrtdispatchsignals)
    {
        hwrtdispatchsignals = false;
        hwrtsignalgl();
    }
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
