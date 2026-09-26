// sauer_nrd.cpp — MSVC Vulkan dispatcher for NVIDIA NRD REBLUR_DIFFUSE.
// C ABI only. No pointers the MinGW module must free.

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include "NRD.h"
#include "nrd_gateway/sauer_nrd.h"

#include <cstring>
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>

#ifndef NRD_CALL
#define NRD_CALL
#endif

#define LOAD(name) do { \
    g.vk.name = (PFN_##name)g.gdpa(g.device, #name); \
    if(!g.vk.name && g.gipa) g.vk.name = (PFN_##name)g.gipa(g.instance, #name); \
    if(!g.vk.name) { seterr(SAUER_NRD_ERR_VK, "missing " #name); return SAUER_NRD_ERR_VK; } \
} while(0)

static const uint32_t kIdent = 1;

struct VkFns
{
    PFN_vkCreateShaderModule vkCreateShaderModule;
    PFN_vkDestroyShaderModule vkDestroyShaderModule;
    PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout;
    PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout;
    PFN_vkCreatePipelineLayout vkCreatePipelineLayout;
    PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout;
    PFN_vkCreateComputePipelines vkCreateComputePipelines;
    PFN_vkDestroyPipeline vkDestroyPipeline;
    PFN_vkCreateDescriptorPool vkCreateDescriptorPool;
    PFN_vkDestroyDescriptorPool vkDestroyDescriptorPool;
    PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets;
    PFN_vkResetDescriptorPool vkResetDescriptorPool;
    PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets;
    PFN_vkCreateSampler vkCreateSampler;
    PFN_vkDestroySampler vkDestroySampler;
    PFN_vkCreateBuffer vkCreateBuffer;
    PFN_vkDestroyBuffer vkDestroyBuffer;
    PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements;
    PFN_vkBindBufferMemory vkBindBufferMemory;
    PFN_vkCreateImage vkCreateImage;
    PFN_vkDestroyImage vkDestroyImage;
    PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements;
    PFN_vkBindImageMemory vkBindImageMemory;
    PFN_vkCreateImageView vkCreateImageView;
    PFN_vkDestroyImageView vkDestroyImageView;
    PFN_vkAllocateMemory vkAllocateMemory;
    PFN_vkFreeMemory vkFreeMemory;
    PFN_vkMapMemory vkMapMemory;
    PFN_vkUnmapMemory vkUnmapMemory;
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties;
    PFN_vkCmdBindPipeline vkCmdBindPipeline;
    PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets;
    PFN_vkCmdDispatch vkCmdDispatch;
    PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier;
    PFN_vkCmdCopyImageToBuffer vkCmdCopyImageToBuffer;
};

struct Tex
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t w = 0, h = 0;
    bool owned = false;
};

struct Pipe
{
    VkShaderModule module = VK_NULL_HANDLE;
    VkDescriptorSetLayout setRes = VK_NULL_HANDLE;
    VkDescriptorSetLayout setCb = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    uint32_t nTex = 0, nStor = 0;
    uint32_t texBind0 = 0, storBind0 = 0;
    bool hasCb = false;
};

struct Gateway
{
    bool alive = false;
    char msg[SAUER_NRD_MSG] = {};
    int32_t last = 0;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    VkFns vk = {};
    nrd::Instance *nrd = nullptr;
    const nrd::InstanceDesc *idesc = nullptr;
    const nrd::LibraryDesc *ldesc = nullptr;
    uint32_t maxw = 0, maxh = 0, queued = 3, qframe = 0;
    uint32_t permMb = 0, transMb = 0, lastDisp = 0;
    bool poolsFresh = true;
    std::vector<Tex> perm, trans;
    Tex user[(size_t)nrd::ResourceType::MAX_NUM];
    std::vector<Pipe> pipes;
    VkSampler sampNear = VK_NULL_HANDLE, sampLin = VK_NULL_HANDLE;
    VkBuffer cb = VK_NULL_HANDLE;
    VkDeviceMemory cbMem = VK_NULL_HANDLE;
    uint8_t *cbMap = nullptr;
    uint32_t cbAlign = 256, cbSlot = 0, cbSlots = 0;
    std::vector<VkDescriptorPool> pools;
    uint32_t lastRsW = 0, lastRsH = 0, lastRectW = 0, lastRectH = 0;
    VkBuffer probeBuf = VK_NULL_HANDLE;
    VkDeviceMemory probeMem = VK_NULL_HANDLE;
    uint8_t *probeMap = nullptr;
    uint32_t probeBytes = 0;
    bool probeArmed = false;
    bool probeCopied = false;
    uint32_t probeN = 0;
    uint16_t probeX[SAUER_NRD_PROBE_MAX] = {};
    uint16_t probeY[SAUER_NRD_PROBE_MAX] = {};
    uint16_t probeGX[SAUER_NRD_PROBE_MAX] = {};
    uint16_t probeGY[SAUER_NRD_PROBE_MAX] = {};
};

static Gateway g;

static void seterr(int32_t code, const char *m)
{
    g.last = code;
    if(m) snprintf(g.msg, sizeof(g.msg), "%s", m);
}

static bool nrdToVkFormat(nrd::Format f, VkFormat &out)
{
    using F = nrd::Format;
    switch(f)
    {
        case F::R8_UNORM: out = VK_FORMAT_R8_UNORM; return true;
        case F::R8_SNORM: out = VK_FORMAT_R8_SNORM; return true;
        case F::R8_UINT: out = VK_FORMAT_R8_UINT; return true;
        case F::R8_SINT: out = VK_FORMAT_R8_SINT; return true;
        case F::RG8_UNORM: out = VK_FORMAT_R8G8_UNORM; return true;
        case F::RG8_SNORM: out = VK_FORMAT_R8G8_SNORM; return true;
        case F::RG8_UINT: out = VK_FORMAT_R8G8_UINT; return true;
        case F::RG8_SINT: out = VK_FORMAT_R8G8_SINT; return true;
        case F::RGBA8_UNORM: out = VK_FORMAT_R8G8B8A8_UNORM; return true;
        case F::RGBA8_SNORM: out = VK_FORMAT_R8G8B8A8_SNORM; return true;
        case F::RGBA8_UINT: out = VK_FORMAT_R8G8B8A8_UINT; return true;
        case F::RGBA8_SINT: out = VK_FORMAT_R8G8B8A8_SINT; return true;
        case F::RGBA8_SRGB: out = VK_FORMAT_R8G8B8A8_SRGB; return true;
        case F::R16_UNORM: out = VK_FORMAT_R16_UNORM; return true;
        case F::R16_SNORM: out = VK_FORMAT_R16_SNORM; return true;
        case F::R16_UINT: out = VK_FORMAT_R16_UINT; return true;
        case F::R16_SINT: out = VK_FORMAT_R16_SINT; return true;
        case F::R16_SFLOAT: out = VK_FORMAT_R16_SFLOAT; return true;
        case F::RG16_UNORM: out = VK_FORMAT_R16G16_UNORM; return true;
        case F::RG16_SNORM: out = VK_FORMAT_R16G16_SNORM; return true;
        case F::RG16_UINT: out = VK_FORMAT_R16G16_UINT; return true;
        case F::RG16_SINT: out = VK_FORMAT_R16G16_SINT; return true;
        case F::RG16_SFLOAT: out = VK_FORMAT_R16G16_SFLOAT; return true;
        case F::RGBA16_UNORM: out = VK_FORMAT_R16G16B16A16_UNORM; return true;
        case F::RGBA16_SNORM: out = VK_FORMAT_R16G16B16A16_SNORM; return true;
        case F::RGBA16_UINT: out = VK_FORMAT_R16G16B16A16_UINT; return true;
        case F::RGBA16_SINT: out = VK_FORMAT_R16G16B16A16_SINT; return true;
        case F::RGBA16_SFLOAT: out = VK_FORMAT_R16G16B16A16_SFLOAT; return true;
        case F::R32_UINT: out = VK_FORMAT_R32_UINT; return true;
        case F::R32_SINT: out = VK_FORMAT_R32_SINT; return true;
        case F::R32_SFLOAT: out = VK_FORMAT_R32_SFLOAT; return true;
        case F::RG32_UINT: out = VK_FORMAT_R32G32_UINT; return true;
        case F::RG32_SINT: out = VK_FORMAT_R32G32_SINT; return true;
        case F::RG32_SFLOAT: out = VK_FORMAT_R32G32_SFLOAT; return true;
        case F::RGB32_UINT: out = VK_FORMAT_R32G32B32_UINT; return true;
        case F::RGB32_SINT: out = VK_FORMAT_R32G32B32_SINT; return true;
        case F::RGB32_SFLOAT: out = VK_FORMAT_R32G32B32_SFLOAT; return true;
        case F::RGBA32_UINT: out = VK_FORMAT_R32G32B32A32_UINT; return true;
        case F::RGBA32_SINT: out = VK_FORMAT_R32G32B32A32_SINT; return true;
        case F::RGBA32_SFLOAT: out = VK_FORMAT_R32G32B32A32_SFLOAT; return true;
        case F::R10_G10_B10_A2_UNORM: out = VK_FORMAT_A2B10G10R10_UNORM_PACK32; return true;
        case F::R10_G10_B10_A2_UINT: out = VK_FORMAT_A2B10G10R10_UINT_PACK32; return true;
        case F::R11_G11_B10_UFLOAT: out = VK_FORMAT_B10G11R11_UFLOAT_PACK32; return true;
        case F::R9_G9_B9_E5_UFLOAT: out = VK_FORMAT_E5B9G9R9_UFLOAT_PACK32; return true;
        default: return false;
    }
}

static int findmem(uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    g.vk.vkGetPhysicalDeviceMemoryProperties(g.phys, &mp);
    for(uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return int(i);
    return -1;
}

static void destroytex(Tex &t)
{
    if(!g.device) { t = {}; return; }
    if(t.view) g.vk.vkDestroyImageView(g.device, t.view, nullptr);
    if(t.owned)
    {
        if(t.image) g.vk.vkDestroyImage(g.device, t.image, nullptr);
        if(t.memory) g.vk.vkFreeMemory(g.device, t.memory, nullptr);
    }
    t = {};
}

static int32_t maketex(Tex &t, uint32_t w, uint32_t h, VkFormat fmt, bool sampled)
{
    destroytex(t);
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent = { w, h, 1 };
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | (sampled ? VK_IMAGE_USAGE_SAMPLED_BIT : 0);
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if(g.vk.vkCreateImage(g.device, &ii, nullptr, &t.image) != VK_SUCCESS)
        return SAUER_NRD_ERR_VK;
    VkMemoryRequirements req;
    g.vk.vkGetImageMemoryRequirements(g.device, t.image, &req);
    int mt = findmem(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if(mt < 0) mt = findmem(req.memoryTypeBits, 0);
    if(mt < 0) return SAUER_NRD_ERR_VK;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = uint32_t(mt);
    if(g.vk.vkAllocateMemory(g.device, &ai, nullptr, &t.memory) != VK_SUCCESS)
        return SAUER_NRD_ERR_VK;
    if(g.vk.vkBindImageMemory(g.device, t.image, t.memory, 0) != VK_SUCCESS)
        return SAUER_NRD_ERR_VK;
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = t.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    if(g.vk.vkCreateImageView(g.device, &vi, nullptr, &t.view) != VK_SUCCESS)
        return SAUER_NRD_ERR_VK;
    t.format = fmt;
    t.w = w;
    t.h = h;
    t.owned = true;
    return SAUER_NRD_OK;
}

static int32_t viewuser(Tex &t, VkImage image, VkFormat fmt, uint32_t w, uint32_t h)
{
    destroytex(t);
    if(!image) return SAUER_NRD_ERR_ARGS;
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    if(g.vk.vkCreateImageView(g.device, &vi, nullptr, &t.view) != VK_SUCCESS)
        return SAUER_NRD_ERR_VK;
    t.image = image;
    t.format = fmt;
    t.w = w;
    t.h = h;
    t.owned = false;
    return SAUER_NRD_OK;
}

static Tex *slot(nrd::ResourceType ty, uint16_t poolIndex)
{
    using T = nrd::ResourceType;
    if(ty == T::TRANSIENT_POOL)
    {
        if(poolIndex >= g.trans.size()) return nullptr;
        return &g.trans[poolIndex];
    }
    if(ty == T::PERMANENT_POOL)
    {
        if(poolIndex >= g.perm.size()) return nullptr;
        return &g.perm[poolIndex];
    }
    size_t i = size_t(ty);
    if(i >= (size_t)T::MAX_NUM) return nullptr;
    if(!g.user[i].view) return nullptr;
    return &g.user[i];
}

static void destroypipes()
{
    for(Pipe &p : g.pipes)
    {
        if(p.pipeline) g.vk.vkDestroyPipeline(g.device, p.pipeline, nullptr);
        if(p.layout) g.vk.vkDestroyPipelineLayout(g.device, p.layout, nullptr);
        if(p.setRes) g.vk.vkDestroyDescriptorSetLayout(g.device, p.setRes, nullptr);
        if(p.setCb) g.vk.vkDestroyDescriptorSetLayout(g.device, p.setCb, nullptr);
        if(p.module) g.vk.vkDestroyShaderModule(g.device, p.module, nullptr);
    }
    g.pipes.clear();
}

static int32_t makepipes()
{
    destroypipes();
    g.pipes.resize(g.idesc->pipelinesNum);
    uint32_t sOff = g.ldesc->spirvBindingOffsets.samplerOffset;
    uint32_t tOff = g.ldesc->spirvBindingOffsets.textureOffset;
    uint32_t bOff = g.ldesc->spirvBindingOffsets.constantBufferOffset;
    uint32_t uOff = g.ldesc->spirvBindingOffsets.storageTextureAndBufferOffset;
    for(uint32_t i = 0; i < g.idesc->pipelinesNum; i++)
    {
        const nrd::PipelineDesc &pd = g.idesc->pipelines[i];
        Pipe &p = g.pipes[i];
        const nrd::ComputeShaderDesc &spv = pd.computeShaderSPIRV;
        if(!spv.bytecode || !spv.size) { seterr(SAUER_NRD_ERR_NRD, "pipeline missing SPIR-V"); return SAUER_NRD_ERR_NRD; }
        VkShaderModuleCreateInfo mi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        mi.codeSize = (size_t)spv.size;
        mi.pCode = (const uint32_t *)spv.bytecode;
        if(g.vk.vkCreateShaderModule(g.device, &mi, nullptr, &p.module) != VK_SUCCESS)
            return SAUER_NRD_ERR_VK;

        p.nTex = p.nStor = 0;
        for(uint32_t r = 0; r < pd.resourceRangesNum; r++)
        {
            if(pd.resourceRanges[r].descriptorType == nrd::DescriptorType::TEXTURE)
                p.nTex += pd.resourceRanges[r].descriptorsNum;
            else
                p.nStor += pd.resourceRanges[r].descriptorsNum;
        }
        p.texBind0 = tOff + g.idesc->resourcesBaseRegisterIndex;
        p.storBind0 = uOff + g.idesc->resourcesBaseRegisterIndex;
        p.hasCb = pd.hasConstantData;

        VkDescriptorSetLayoutBinding rb[64];
        uint32_t nrb = 0;
        for(uint32_t k = 0; k < p.nTex && nrb < 64; k++)
        {
            rb[nrb] = {};
            rb[nrb].binding = p.texBind0 + k;
            rb[nrb].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            rb[nrb].descriptorCount = 1;
            rb[nrb].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            nrb++;
        }
        for(uint32_t k = 0; k < p.nStor && nrb < 64; k++)
        {
            rb[nrb] = {};
            rb[nrb].binding = p.storBind0 + k;
            rb[nrb].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            rb[nrb].descriptorCount = 1;
            rb[nrb].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            nrb++;
        }
        VkDescriptorSetLayoutCreateInfo sl = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        sl.bindingCount = nrb;
        sl.pBindings = rb;
        if(g.vk.vkCreateDescriptorSetLayout(g.device, &sl, nullptr, &p.setRes) != VK_SUCCESS)
            return SAUER_NRD_ERR_VK;

        VkSampler imms[2] = { g.sampNear, g.sampLin };
        VkDescriptorSetLayoutBinding cb[4] = {};
        uint32_t ncb = 0;
        for(uint32_t s = 0; s < g.idesc->samplersNum && s < 2; s++)
        {
            cb[ncb].binding = sOff + g.idesc->samplersBaseRegisterIndex + s;
            cb[ncb].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            cb[ncb].descriptorCount = 1;
            cb[ncb].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            cb[ncb].pImmutableSamplers = &imms[s];
            ncb++;
        }
        cb[ncb].binding = bOff + g.idesc->constantBufferRegisterIndex;
        cb[ncb].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        cb[ncb].descriptorCount = 1;
        cb[ncb].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        ncb++;
        sl.bindingCount = ncb;
        sl.pBindings = cb;
        if(g.vk.vkCreateDescriptorSetLayout(g.device, &sl, nullptr, &p.setCb) != VK_SUCCESS)
            return SAUER_NRD_ERR_VK;

        VkDescriptorSetLayout sets[2];
        uint32_t nsets = 0;
        uint32_t resSpace = g.idesc->resourcesSpaceIndex;
        uint32_t cbSpace = g.idesc->constantBufferAndSamplersSpaceIndex;
        if(resSpace == 0 && cbSpace == 1)
        {
            sets[0] = p.setRes; sets[1] = p.setCb; nsets = 2;
        }
        else if(cbSpace == 0 && resSpace == 1)
        {
            sets[0] = p.setCb; sets[1] = p.setRes; nsets = 2;
        }
        else { seterr(SAUER_NRD_ERR_UNSUPPORTED, "unexpected NRD descriptor spaces"); return SAUER_NRD_ERR_UNSUPPORTED; }

        VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pl.setLayoutCount = nsets;
        pl.pSetLayouts = sets;
        if(g.vk.vkCreatePipelineLayout(g.device, &pl, nullptr, &p.layout) != VK_SUCCESS)
            return SAUER_NRD_ERR_VK;

        VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = p.module;
        ci.stage.pName = g.idesc->shaderEntryPoint ? g.idesc->shaderEntryPoint : "main";
        ci.layout = p.layout;
        if(g.vk.vkCreateComputePipelines(g.device, VK_NULL_HANDLE, 1, &ci, nullptr, &p.pipeline) != VK_SUCCESS)
            return SAUER_NRD_ERR_VK;
    }
    return SAUER_NRD_OK;
}

static void destroyall()
{
    if(!g.device) { g = {}; return; }
    destroypipes();
    for(VkDescriptorPool p : g.pools)
        if(p) g.vk.vkDestroyDescriptorPool(g.device, p, nullptr);
    g.pools.clear();
    if(g.cbMap) { g.vk.vkUnmapMemory(g.device, g.cbMem); g.cbMap = nullptr; }
    if(g.cb) { g.vk.vkDestroyBuffer(g.device, g.cb, nullptr); g.cb = nullptr; }
    if(g.cbMem) { g.vk.vkFreeMemory(g.device, g.cbMem, nullptr); g.cbMem = nullptr; }
    if(g.probeMap) { g.vk.vkUnmapMemory(g.device, g.probeMem); g.probeMap = nullptr; }
    if(g.probeBuf) { g.vk.vkDestroyBuffer(g.device, g.probeBuf, nullptr); g.probeBuf = nullptr; }
    if(g.probeMem) { g.vk.vkFreeMemory(g.device, g.probeMem, nullptr); g.probeMem = nullptr; }
    if(g.sampNear) g.vk.vkDestroySampler(g.device, g.sampNear, nullptr);
    if(g.sampLin) g.vk.vkDestroySampler(g.device, g.sampLin, nullptr);
    for(Tex &t : g.perm) destroytex(t);
    for(Tex &t : g.trans) destroytex(t);
    for(size_t i = 0; i < (size_t)nrd::ResourceType::MAX_NUM; i++) destroytex(g.user[i]);
    g.perm.clear();
    g.trans.clear();
    if(g.nrd) { nrd::DestroyInstance(*g.nrd); g.nrd = nullptr; }
    g.alive = false;
    g.idesc = nullptr;
}

extern "C" __declspec(dllexport) uint32_t sauer_nrd_abi_version(void)
{
    return SAUER_NRD_ABI_VERSION;
}

extern "C" __declspec(dllexport) void sauer_nrd_destroy(void)
{
    destroyall();
    g = {};
}

extern "C" __declspec(dllexport) int32_t sauer_nrd_create(const SauerNrdInit *init)
{
    sauer_nrd_destroy();
    if(!init || init->struct_bytes != sizeof(SauerNrdInit))
        return SAUER_NRD_ERR_SIZE;
    if(!init->instance || !init->physical_device || !init->device || !init->gdpa)
        return SAUER_NRD_ERR_ARGS;
    if(init->max_w < 8 || init->max_h < 8)
        return SAUER_NRD_ERR_ARGS;

    g.instance = (VkInstance)init->instance;
    g.phys = (VkPhysicalDevice)init->physical_device;
    g.device = (VkDevice)init->device;
    g.gipa = (PFN_vkGetInstanceProcAddr)init->gipa;
    g.gdpa = (PFN_vkGetDeviceProcAddr)init->gdpa;
    g.maxw = init->max_w;
    g.maxh = init->max_h;
    g.queued = init->queued_frames ? init->queued_frames : 3;
    if(g.queued > 4) g.queued = 4;

    LOAD(vkCreateShaderModule);
    LOAD(vkDestroyShaderModule);
    LOAD(vkCreateDescriptorSetLayout);
    LOAD(vkDestroyDescriptorSetLayout);
    LOAD(vkCreatePipelineLayout);
    LOAD(vkDestroyPipelineLayout);
    LOAD(vkCreateComputePipelines);
    LOAD(vkDestroyPipeline);
    LOAD(vkCreateDescriptorPool);
    LOAD(vkDestroyDescriptorPool);
    LOAD(vkAllocateDescriptorSets);
    LOAD(vkResetDescriptorPool);
    LOAD(vkUpdateDescriptorSets);
    LOAD(vkCreateSampler);
    LOAD(vkDestroySampler);
    LOAD(vkCreateBuffer);
    LOAD(vkDestroyBuffer);
    LOAD(vkGetBufferMemoryRequirements);
    LOAD(vkBindBufferMemory);
    LOAD(vkCreateImage);
    LOAD(vkDestroyImage);
    LOAD(vkGetImageMemoryRequirements);
    LOAD(vkBindImageMemory);
    LOAD(vkCreateImageView);
    LOAD(vkDestroyImageView);
    LOAD(vkAllocateMemory);
    LOAD(vkFreeMemory);
    LOAD(vkMapMemory);
    LOAD(vkUnmapMemory);
    LOAD(vkGetPhysicalDeviceMemoryProperties);
    LOAD(vkCmdBindPipeline);
    LOAD(vkCmdBindDescriptorSets);
    LOAD(vkCmdDispatch);
    LOAD(vkCmdPipelineBarrier);
    LOAD(vkCmdCopyImageToBuffer);

    g.ldesc = nrd::GetLibraryDesc();
    if(!g.ldesc || g.ldesc->versionMajor != NRD_VERSION_MAJOR)
    {
        seterr(SAUER_NRD_ERR_VERSION, "NRD version mismatch");
        return SAUER_NRD_ERR_VERSION;
    }

    nrd::DenoiserDesc dd = {};
    dd.identifier = kIdent;
    dd.denoiser = nrd::Denoiser::REBLUR_DIFFUSE;
    nrd::InstanceCreationDesc icd = {};
    icd.denoisers = &dd;
    icd.denoisersNum = 1;
    if(nrd::CreateInstance(icd, g.nrd) != nrd::Result::SUCCESS)
    {
        seterr(SAUER_NRD_ERR_NRD, "CreateInstance failed");
        return SAUER_NRD_ERR_NRD;
    }
    g.idesc = nrd::GetInstanceDesc(*g.nrd);

    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.minFilter = si.magFilter = VK_FILTER_NEAREST;
    if(g.vk.vkCreateSampler(g.device, &si, nullptr, &g.sampNear) != VK_SUCCESS)
        return SAUER_NRD_ERR_VK;
    si.minFilter = si.magFilter = VK_FILTER_LINEAR;
    if(g.vk.vkCreateSampler(g.device, &si, nullptr, &g.sampLin) != VK_SUCCESS)
        return SAUER_NRD_ERR_VK;

    auto addpool = [&](const nrd::TextureDesc *src, uint32_t n, std::vector<Tex> &dst, uint32_t &mb) -> int32_t {
        dst.resize(n);
        mb = 0;
        for(uint32_t i = 0; i < n; i++)
        {
            uint32_t f = src[i].downsampleFactor ? src[i].downsampleFactor : 1;
            uint32_t w = (g.maxw + f - 1) / f;
            uint32_t h = (g.maxh + f - 1) / f;
            if(w < 1) w = 1;
            if(h < 1) h = 1;
            VkFormat vkfmt = VK_FORMAT_UNDEFINED;
            if(!nrdToVkFormat(src[i].format, vkfmt))
            {
                char m[SAUER_NRD_MSG];
                snprintf(m, sizeof(m), "unsupported NRD format %u pool[%u]", unsigned(src[i].format), i);
                seterr(SAUER_NRD_ERR_UNSUPPORTED, m);
                return SAUER_NRD_ERR_UNSUPPORTED;
            }
            int32_t e = maketex(dst[i], w, h, vkfmt, true);
            if(e) return e;
            mb += (w * h * 8 + 1023) / 1024;
        }
        mb = (mb + 1023) / 1024;
        return SAUER_NRD_OK;
    };
    int32_t e = addpool(g.idesc->permanentPool, g.idesc->permanentPoolSize, g.perm, g.permMb);
    if(e) { destroyall(); return e; }
    e = addpool(g.idesc->transientPool, g.idesc->transientPoolSize, g.trans, g.transMb);
    if(e) { destroyall(); return e; }

    e = makepipes();
    if(e) { destroyall(); return e; }

    uint32_t sets = g.idesc->descriptorPoolDesc.setsMaxNum;
    if(sets < 16) sets = 16;
    uint32_t ntex = g.idesc->descriptorPoolDesc.perSetTexturesMaxNum;
    uint32_t nstor = g.idesc->descriptorPoolDesc.perSetStorageTexturesMaxNum;
    if(ntex < 8) ntex = 8;
    if(nstor < 8) nstor = 8;
    g.pools.resize(g.queued);
    for(uint32_t i = 0; i < g.queued; i++)
    {
        VkDescriptorPoolSize ps[3] = {};
        ps[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        ps[0].descriptorCount = sets * ntex;
        ps[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        ps[1].descriptorCount = sets * nstor;
        ps[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ps[2].descriptorCount = sets;
        VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pi.maxSets = sets * 2;
        pi.poolSizeCount = 3;
        pi.pPoolSizes = ps;
        if(g.vk.vkCreateDescriptorPool(g.device, &pi, nullptr, &g.pools[i]) != VK_SUCCESS)
        {
            destroyall();
            return SAUER_NRD_ERR_VK;
        }
    }

    g.cbSlots = sets * g.queued;
    uint32_t cbBytes = (g.idesc->constantBufferMaxDataSize + g.cbAlign - 1) / g.cbAlign * g.cbAlign;
    if(cbBytes < g.cbAlign) cbBytes = g.cbAlign;
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = uint64_t(cbBytes) * g.cbSlots;
    bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if(g.vk.vkCreateBuffer(g.device, &bi, nullptr, &g.cb) != VK_SUCCESS)
    {
        destroyall();
        return SAUER_NRD_ERR_VK;
    }
    VkMemoryRequirements req;
    g.vk.vkGetBufferMemoryRequirements(g.device, g.cb, &req);
    int mt = findmem(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if(mt < 0) mt = findmem(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if(mt < 0) { destroyall(); return SAUER_NRD_ERR_VK; }
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = uint32_t(mt);
    if(g.vk.vkAllocateMemory(g.device, &ai, nullptr, &g.cbMem) != VK_SUCCESS)
    {
        destroyall();
        return SAUER_NRD_ERR_VK;
    }
    if(g.vk.vkBindBufferMemory(g.device, g.cb, g.cbMem, 0) != VK_SUCCESS)
    {
        destroyall();
        return SAUER_NRD_ERR_VK;
    }
    if(g.vk.vkMapMemory(g.device, g.cbMem, 0, bi.size, 0, (void **)&g.cbMap) != VK_SUCCESS)
    {
        destroyall();
        return SAUER_NRD_ERR_VK;
    }

    g.alive = true;
    g.poolsFresh = true;
    g.probeArmed = false;
    g.probeCopied = false;
    {
        g.probeBytes = 64 * 1024;
        VkBufferCreateInfo pbi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        pbi.size = g.probeBytes;
        pbi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if(g.vk.vkCreateBuffer(g.device, &pbi, nullptr, &g.probeBuf) != VK_SUCCESS)
        {
            destroyall();
            return SAUER_NRD_ERR_VK;
        }
        VkMemoryRequirements preq;
        g.vk.vkGetBufferMemoryRequirements(g.device, g.probeBuf, &preq);
        int pmt = findmem(preq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if(pmt < 0) pmt = findmem(preq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        if(pmt < 0) { destroyall(); return SAUER_NRD_ERR_VK; }
        VkMemoryAllocateInfo pai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        pai.allocationSize = preq.size;
        pai.memoryTypeIndex = uint32_t(pmt);
        if(g.vk.vkAllocateMemory(g.device, &pai, nullptr, &g.probeMem) != VK_SUCCESS)
        {
            destroyall();
            return SAUER_NRD_ERR_VK;
        }
        if(g.vk.vkBindBufferMemory(g.device, g.probeBuf, g.probeMem, 0) != VK_SUCCESS)
        {
            destroyall();
            return SAUER_NRD_ERR_VK;
        }
        if(g.vk.vkMapMemory(g.device, g.probeMem, 0, pbi.size, 0, (void **)&g.probeMap) != VK_SUCCESS)
        {
            destroyall();
            return SAUER_NRD_ERR_VK;
        }
        memset(g.probeMap, 0, g.probeBytes);
    }
    {
        char m[SAUER_NRD_MSG];
        int n = snprintf(m, sizeof(m), "ok pool %ux%u perm", unsigned(g.maxw), unsigned(g.maxh));
        for(uint32_t i = 0; i < g.idesc->permanentPoolSize && n > 0 && n < int(sizeof(m) - 16); i++)
            n += snprintf(m + n, sizeof(m) - size_t(n), " %u:%ux%u", unsigned(g.idesc->permanentPool[i].format),
                          unsigned(g.perm[i].w), unsigned(g.perm[i].h));
        if(n > 0 && n < int(sizeof(m) - 16))
            n += snprintf(m + n, sizeof(m) - size_t(n), " trans");
        for(uint32_t i = 0; i < g.idesc->transientPoolSize && n > 0 && n < int(sizeof(m) - 8); i++)
            n += snprintf(m + n, sizeof(m) - size_t(n), " %u:%ux%u", unsigned(g.idesc->transientPool[i].format),
                          unsigned(g.trans[i].w), unsigned(g.trans[i].h));
        seterr(SAUER_NRD_OK, m);
    }
    return SAUER_NRD_OK;
}

extern "C" __declspec(dllexport) int32_t sauer_nrd_set_images(const SauerNrdImages *im)
{
    if(!g.alive) return SAUER_NRD_ERR_STATE;
    if(!im || im->struct_bytes != sizeof(SauerNrdImages)) return SAUER_NRD_ERR_SIZE;
    if(!im->in_diff || !im->in_viewz || !im->in_normal || !im->in_mv || !im->out_diff)
        return SAUER_NRD_ERR_ARGS;
    using T = nrd::ResourceType;
    int32_t e;
    e = viewuser(g.user[(size_t)T::IN_DIFF_RADIANCE_HITDIST], (VkImage)im->in_diff, VK_FORMAT_R16G16B16A16_SFLOAT, im->w, im->h);
    if(e) return e;
    e = viewuser(g.user[(size_t)T::IN_VIEWZ], (VkImage)im->in_viewz, VK_FORMAT_R32_SFLOAT, im->w, im->h);
    if(e) return e;
    e = viewuser(g.user[(size_t)T::IN_NORMAL_ROUGHNESS], (VkImage)im->in_normal, VK_FORMAT_R16G16B16A16_UNORM, im->w, im->h);
    if(e) return e;
    e = viewuser(g.user[(size_t)T::IN_MV], (VkImage)im->in_mv, VK_FORMAT_R16G16B16A16_SFLOAT, im->w, im->h);
    if(e) return e;
    e = viewuser(g.user[(size_t)T::OUT_DIFF_RADIANCE_HITDIST], (VkImage)im->out_diff, VK_FORMAT_R16G16B16A16_SFLOAT, im->w, im->h);
    if(e) return e;
    return SAUER_NRD_OK;
}

/* Staging layout, 256 bytes per probe pixel. */
static const uint32_t kProbeStride = 256;
static const uint32_t kOffPrevXY = 0;     /* R32F 2x2 at (x,y) TA input */
static const uint32_t kOffPrevG = 16;     /* R32F 2x2 at gather texel */
static const uint32_t kOffPrevAfter = 32; /* R32F 1x1 after Blur */
static const uint32_t kOffInZ = 48;       /* R32F 1x1 */
static const uint32_t kOffHist = 64;      /* R16U 2x2 at (x,y) */
static const uint32_t kOffHistG = 80;     /* R16U 2x2 at gather */
static const uint32_t kOffOut = 96;       /* RGBA16F 1x1 */
static const uint32_t kOffMV = 112;       /* RGBA16F 1x1 */
static const uint32_t kOffData1 = 128;    /* R8 1x1 */
static const uint32_t kOffData2 = 144;    /* R8 1x1 */

static void copyrect(VkCommandBuffer cmd, Tex &t, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t off)
{
    if(!t.image || !g.probeBuf) return;
    if(t.w < 1 || t.h < 1) return;
    if(x >= t.w) x = t.w - 1;
    if(y >= t.h) y = t.h - 1;
    if(x + w > t.w) w = t.w - x;
    if(y + h > t.h) h = t.h - y;
    if(w < 1 || h < 1) return;
    VkBufferImageCopy r = {};
    r.bufferOffset = off;
    r.bufferRowLength = w;
    r.bufferImageHeight = h;
    r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.imageSubresource.layerCount = 1;
    r.imageOffset = { int32_t(x), int32_t(y), 0 };
    r.imageExtent = { w, h, 1 };
    g.vk.vkCmdCopyImageToBuffer(cmd, t.image, VK_IMAGE_LAYOUT_GENERAL, g.probeBuf, 1, &r);
}

static void probexfer(VkCommandBuffer cmd, bool before)
{
    if(!g.probeArmed || !g.probeMap || !g.vk.vkCmdCopyImageToBuffer) return;
    using T = nrd::ResourceType;
    Tex &prevZ = g.perm.empty() ? g.user[0] : g.perm[0];
    Tex &prevHist = g.perm.size() > 2 ? g.perm[2] : g.user[0];
    Tex &inZ = g.user[(size_t)T::IN_VIEWZ];
    Tex &inMV = g.user[(size_t)T::IN_MV];
    Tex &outD = g.user[(size_t)T::OUT_DIFF_RADIANCE_HITDIST];
    Tex &data1 = g.trans.empty() ? g.user[0] : g.trans[0];
    Tex &data2 = g.trans.size() > 1 ? g.trans[1] : g.user[0];

    VkImageMemoryBarrier bars[8];
    uint32_t nb = 0;
    auto addbar = [&](Tex &t, VkAccessFlags src, VkAccessFlags dst)
    {
        if(!t.image || nb >= 8) return;
        bars[nb] = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        bars[nb].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bars[nb].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bars[nb].image = t.image;
        bars[nb].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bars[nb].subresourceRange.levelCount = 1;
        bars[nb].subresourceRange.layerCount = 1;
        bars[nb].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        bars[nb].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        bars[nb].srcAccessMask = src;
        bars[nb].dstAccessMask = dst;
        nb++;
    };
    if(before)
    {
        addbar(prevZ, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        addbar(prevHist, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        addbar(inMV, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        g.vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  0, 0, nullptr, 0, nullptr, nb, bars);
        for(uint32_t i = 0; i < g.probeN; i++)
        {
            uint32_t base = i * kProbeStride;
            uint32_t x = g.probeX[i], y = g.probeY[i];
            uint32_t gx = g.probeGX[i], gy = g.probeGY[i];
            copyrect(cmd, prevZ, x, y, 2, 2, base + kOffPrevXY);
            copyrect(cmd, prevZ, gx, gy, 2, 2, base + kOffPrevG);
            copyrect(cmd, prevHist, x, y, 2, 2, base + kOffHist);
            copyrect(cmd, prevHist, gx, gy, 2, 2, base + kOffHistG);
            copyrect(cmd, inMV, x, y, 1, 1, base + kOffMV);
        }
        nb = 0;
        addbar(prevZ, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        addbar(prevHist, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        addbar(inMV, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        g.vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  0, 0, nullptr, 0, nullptr, nb, bars);
    }
    else
    {
        addbar(prevZ, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        addbar(inZ, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        addbar(outD, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        addbar(data1, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        addbar(data2, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        g.vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  0, 0, nullptr, 0, nullptr, nb, bars);
        for(uint32_t i = 0; i < g.probeN; i++)
        {
            uint32_t base = i * kProbeStride;
            uint32_t x = g.probeX[i], y = g.probeY[i];
            copyrect(cmd, prevZ, x, y, 1, 1, base + kOffPrevAfter);
            copyrect(cmd, inZ, x, y, 1, 1, base + kOffInZ);
            copyrect(cmd, outD, x, y, 1, 1, base + kOffOut);
            copyrect(cmd, data1, x, y, 1, 1, base + kOffData1);
            copyrect(cmd, data2, x, y, 1, 1, base + kOffData2);
        }
        g.probeCopied = true;
        g.probeArmed = false;
    }
}

extern "C" __declspec(dllexport) int32_t sauer_nrd_denoise(const SauerNrdFrame *fr)
{
    if(!g.alive || !g.nrd) return SAUER_NRD_ERR_STATE;
    if(!fr || fr->struct_bytes != sizeof(SauerNrdFrame)) return SAUER_NRD_ERR_SIZE;
    if(!fr->command_buffer) return SAUER_NRD_ERR_ARGS;
    if(fr->w < 8 || fr->h < 8 || fr->w > g.maxw || fr->h > g.maxh)
        return SAUER_NRD_ERR_ARGS;

    using TU = nrd::ResourceType;
    Tex &userZ = g.user[(size_t)TU::IN_VIEWZ];
    Tex &userOut = g.user[(size_t)TU::OUT_DIFF_RADIANCE_HITDIST];
    if(!userZ.view || userZ.w != g.maxw || userZ.h != g.maxh ||
       !userOut.view || userOut.w != g.maxw || userOut.h != g.maxh)
    {
        char m[SAUER_NRD_MSG];
        snprintf(m, sizeof(m), "size mismatch pool %ux%u userZ %ux%u userOut %ux%u rect %ux%u",
                 unsigned(g.maxw), unsigned(g.maxh), unsigned(userZ.w), unsigned(userZ.h),
                 unsigned(userOut.w), unsigned(userOut.h), unsigned(fr->w), unsigned(fr->h));
        seterr(SAUER_NRD_ERR_ARGS, m);
        return SAUER_NRD_ERR_ARGS;
    }
    /* Circumscribed contract: every VkImage is the active size. resourceSize
     * is the physical allocation (pools + user). DRS would keep a larger
     * capacity and pass a smaller rectSize; that needs every image, including
     * guides, at that capacity. Recreate on change instead of mixing sizes. */
    if(fr->w != g.maxw || fr->h != g.maxh)
    {
        char m[SAUER_NRD_MSG];
        snprintf(m, sizeof(m), "rect %ux%u != pool %ux%u (recreate instance)",
                 unsigned(fr->w), unsigned(fr->h), unsigned(g.maxw), unsigned(g.maxh));
        seterr(SAUER_NRD_ERR_ARGS, m);
        return SAUER_NRD_ERR_ARGS;
    }

    nrd::CommonSettings cs = {};
    memcpy(cs.viewToClipMatrix, fr->view_to_clip, sizeof(cs.viewToClipMatrix));
    memcpy(cs.viewToClipMatrixPrev, fr->view_to_clip_prev, sizeof(cs.viewToClipMatrixPrev));
    memcpy(cs.worldToViewMatrix, fr->world_to_view, sizeof(cs.worldToViewMatrix));
    memcpy(cs.worldToViewMatrixPrev, fr->world_to_view_prev, sizeof(cs.worldToViewMatrixPrev));
    cs.motionVectorScale[0] = fr->mv_scale[0];
    cs.motionVectorScale[1] = fr->mv_scale[1];
    cs.motionVectorScale[2] = fr->mv_scale[2];
    cs.cameraJitter[0] = fr->jitter[0];
    cs.cameraJitter[1] = fr->jitter[1];
    cs.cameraJitterPrev[0] = fr->jitter_prev[0];
    cs.cameraJitterPrev[1] = fr->jitter_prev[1];
    cs.resourceSize[0] = (uint16_t)g.maxw;
    cs.resourceSize[1] = (uint16_t)g.maxh;
    cs.resourceSizePrev[0] = (uint16_t)g.maxw;
    cs.resourceSizePrev[1] = (uint16_t)g.maxh;
    cs.rectSize[0] = (uint16_t)fr->w;
    cs.rectSize[1] = (uint16_t)fr->h;
    cs.rectSizePrev[0] = (uint16_t)(fr->w_prev ? fr->w_prev : fr->w);
    cs.rectSizePrev[1] = (uint16_t)(fr->h_prev ? fr->h_prev : fr->h);
    g.lastRsW = cs.resourceSize[0];
    g.lastRsH = cs.resourceSize[1];
    g.lastRectW = cs.rectSize[0];
    g.lastRectH = cs.rectSize[1];
    cs.timeDeltaBetweenFrames = fr->time_delta_ms;
    cs.denoisingRange = fr->denoising_range > 1.0f ? fr->denoising_range : 500000.0f;
    cs.frameIndex = fr->frame_index;
    cs.isMotionVectorInWorldSpace = false;
    if(fr->accum == SAUER_NRD_ACCUM_CLEAR) cs.accumulationMode = nrd::AccumulationMode::CLEAR_AND_RESTART;
    else if(fr->accum == SAUER_NRD_ACCUM_RESTART) cs.accumulationMode = nrd::AccumulationMode::RESTART;
    else cs.accumulationMode = nrd::AccumulationMode::CONTINUE;

    if(nrd::SetCommonSettings(*g.nrd, cs) != nrd::Result::SUCCESS)
    {
        seterr(SAUER_NRD_ERR_NRD, "SetCommonSettings failed");
        return SAUER_NRD_ERR_NRD;
    }
    nrd::ReblurSettings rs = {};
    // Official defaults. Previous antilag / anti-firefly / stabilize=0 /
    // maxBlur=8 were tuned on frames that never displayed NRD (flags died in
    // float16). Hit-distance reconstruction stays OFF: every pixel has a
    // skyvis sample, not a probabilistic skip. Diffuse prepass 0: hitlight
    // already runs an experimental plane-tested 5x5; NRD's 30px prepass has
    // no plane test (v1 snow bled into gravel). Not a "REBLUR needs 4 rays"
    // rule — the pinned README targets 1 path/pixel.
    rs.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::OFF;
    rs.diffusePrepassBlurRadius = 0.0f;
    // .w of the output is unused by skycompose (YCoCg luminance). History
    // length in frames is the actual accumulation signal; occlusion AO is not.
    rs.returnHistoryLengthInsteadOfOcclusion = true;
    if(fr->options & SAUER_NRD_OPT_ANTILAG_OFF)
    {
        // README: first integration with antilag disabled. Huge sensitivity
        // makes ComputeAntilag ≈ 1 (mode 2: 1 / (1 + d * N / magic)).
        rs.antilagSettings.luminanceSigmaScale = 100.0f;
        rs.antilagSettings.luminanceSensitivity = 1.0e6f;
    }
    if(fr->options & SAUER_NRD_OPT_FAST_OFF)
        rs.maxFastAccumulatedFrameNum = rs.maxAccumulatedFrameNum;
    if(nrd::SetDenoiserSettings(*g.nrd, kIdent, &rs) != nrd::Result::SUCCESS)
    {
        seterr(SAUER_NRD_ERR_NRD, "SetDenoiserSettings failed");
        return SAUER_NRD_ERR_NRD;
    }

    const nrd::DispatchDesc *dis = nullptr;
    uint32_t ndis = 0;
    nrd::Identifier id = kIdent;
    if(nrd::GetComputeDispatches(*g.nrd, &id, 1, dis, ndis) != nrd::Result::SUCCESS)
    {
        seterr(SAUER_NRD_ERR_NRD, "GetComputeDispatches failed");
        return SAUER_NRD_ERR_NRD;
    }

    g.qframe++;
    uint32_t pooli = g.qframe % g.queued;
    g.vk.vkResetDescriptorPool(g.device, g.pools[pooli], 0);
    g.lastDisp = ndis;

    VkCommandBuffer cmd = (VkCommandBuffer)fr->command_buffer;
    uint32_t cbBytes = (g.idesc->constantBufferMaxDataSize + g.cbAlign - 1) / g.cbAlign * g.cbAlign;
    if(cbBytes < g.cbAlign) cbBytes = g.cbAlign;

    if(g.poolsFresh)
    {
        VkImageMemoryBarrier bars[64];
        uint32_t nb = 0;
        auto addbar = [&](Tex &t)
        {
            if(!t.image || nb >= 64) return;
            bars[nb] = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            bars[nb].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bars[nb].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bars[nb].image = t.image;
            bars[nb].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            bars[nb].subresourceRange.levelCount = 1;
            bars[nb].subresourceRange.layerCount = 1;
            bars[nb].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            bars[nb].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            bars[nb].srcAccessMask = 0;
            bars[nb].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            nb++;
        };
        for(Tex &t : g.perm) addbar(t);
        for(Tex &t : g.trans) addbar(t);
        if(nb)
            g.vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                      0, 0, nullptr, 0, nullptr, nb, bars);
        g.poolsFresh = false;
    }

    if(g.probeArmed && g.probeN)
    {
        float rsW = float(cs.resourceSize[0]);
        float rsH = float(cs.resourceSize[1]);
        float physW = float(g.maxw);
        float physH = float(g.maxh);
        for(uint32_t i = 0; i < g.probeN; i++)
        {
            float originX = float(g.probeX[i]);
            float originY = float(g.probeY[i]);
            float gatherUvX = (originX + 1.0f) / rsW;
            float gatherUvY = (originY + 1.0f) / rsH;
            int gx = (int)floorf(gatherUvX * physW - 0.5f);
            int gy = (int)floorf(gatherUvY * physH - 0.5f);
            if(gx < 0) gx = 0;
            if(gy < 0) gy = 0;
            if(gx > int(g.maxw) - 1) gx = int(g.maxw) - 1;
            if(gy > int(g.maxh) - 1) gy = int(g.maxh) - 1;
            g.probeGX[i] = (uint16_t)gx;
            g.probeGY[i] = (uint16_t)gy;
        }
        probexfer(cmd, true);
    }

    for(uint32_t d = 0; d < ndis; d++)
    {
        const nrd::DispatchDesc &dd = dis[d];
        if(dd.pipelineIndex >= g.pipes.size()) return SAUER_NRD_ERR_NRD;
        Pipe &p = g.pipes[dd.pipelineIndex];

        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = g.pools[pooli];
        VkDescriptorSetLayout layouts[2] = { p.setRes, p.setCb };
        if(g.idesc->constantBufferAndSamplersSpaceIndex == 0)
        {
            layouts[0] = p.setCb;
            layouts[1] = p.setRes;
        }
        ai.descriptorSetCount = 2;
        ai.pSetLayouts = layouts;
        VkDescriptorSet sets[2] = {};
        if(g.vk.vkAllocateDescriptorSets(g.device, &ai, sets) != VK_SUCCESS)
        {
            seterr(SAUER_NRD_ERR_VK, "AllocateDescriptorSets failed");
            return SAUER_NRD_ERR_VK;
        }
        VkDescriptorSet setRes = (g.idesc->resourcesSpaceIndex == 0) ? sets[0] : sets[1];
        VkDescriptorSet setCb = (g.idesc->constantBufferAndSamplersSpaceIndex == 0) ? sets[0] : sets[1];

        uint32_t iTex = 0, iStor = 0;
        VkWriteDescriptorSet writes[64];
        VkDescriptorImageInfo imgs[64];
        uint32_t nw = 0;
        uint32_t rangeCursor = 0;
        const nrd::PipelineDesc &pd = g.idesc->pipelines[dd.pipelineIndex];
        uint32_t resI = 0;
        for(uint32_t r = 0; r < pd.resourceRangesNum; r++)
        {
            const nrd::ResourceRangeDesc &rg = pd.resourceRanges[r];
            for(uint32_t k = 0; k < rg.descriptorsNum; k++)
            {
                if(resI >= dd.resourcesNum) break;
                const nrd::ResourceDesc &rd = dd.resources[resI++];
                Tex *tx = slot(rd.type, rd.indexInPool);
                if(!tx || !tx->view)
                {
                    seterr(SAUER_NRD_ERR_ARGS, "NRD resource missing");
                    return SAUER_NRD_ERR_ARGS;
                }
                imgs[nw] = {};
                imgs[nw].imageView = tx->view;
                imgs[nw].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                writes[nw] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                writes[nw].dstSet = setRes;
                writes[nw].descriptorCount = 1;
                writes[nw].pImageInfo = &imgs[nw];
                if(rd.descriptorType == nrd::DescriptorType::TEXTURE)
                {
                    writes[nw].dstBinding = p.texBind0 + iTex++;
                    writes[nw].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                }
                else
                {
                    writes[nw].dstBinding = p.storBind0 + iStor++;
                    writes[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                }
                nw++;
                if(nw >= 64) break;
            }
        }
        (void)rangeCursor;

        uint32_t slot = g.cbSlot++ % g.cbSlots;
        uint32_t off = slot * cbBytes;
        if(dd.constantBufferData && dd.constantBufferDataSize)
            memcpy(g.cbMap + off, dd.constantBufferData, dd.constantBufferDataSize);
        VkDescriptorBufferInfo binfo = {};
        binfo.buffer = g.cb;
        binfo.offset = off;
        binfo.range = cbBytes;
        writes[nw] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[nw].dstSet = setCb;
        writes[nw].dstBinding = g.ldesc->spirvBindingOffsets.constantBufferOffset + g.idesc->constantBufferRegisterIndex;
        writes[nw].descriptorCount = 1;
        writes[nw].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[nw].pBufferInfo = &binfo;
        nw++;
        g.vk.vkUpdateDescriptorSets(g.device, nw, writes, 0, nullptr);

        VkMemoryBarrier bar = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        bar.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        g.vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  0, 1, &bar, 0, nullptr, 0, nullptr);

        g.vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
        g.vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 2, sets, 0, nullptr);
        g.vk.vkCmdDispatch(cmd, dd.gridWidth, dd.gridHeight, 1);
    }
    {
        VkMemoryBarrier bar = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        bar.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        g.vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  0, 1, &bar, 0, nullptr, 0, nullptr);
    }
    if(g.probeArmed && g.probeN)
        probexfer(cmd, false);
    return SAUER_NRD_OK;
}

extern "C" __declspec(dllexport) int32_t sauer_nrd_stats(SauerNrdStats *st)
{
    if(!st || st->struct_bytes != sizeof(SauerNrdStats)) return SAUER_NRD_ERR_SIZE;
    st->dispatches = g.lastDisp;
    st->permanent_mb = g.permMb;
    st->transient_mb = g.transMb;
    st->pipelines = (uint32_t)g.pipes.size();
    st->last_error = g.last;
    using T = nrd::ResourceType;
    Tex &uz = g.user[(size_t)T::IN_VIEWZ];
    snprintf(st->message, sizeof(st->message),
             "%s rs %ux%u rect %ux%u userZ %ux%u",
             g.msg[0] ? g.msg : "ok",
             unsigned(g.lastRsW ? g.lastRsW : g.maxw), unsigned(g.lastRsH ? g.lastRsH : g.maxh),
             unsigned(g.lastRectW), unsigned(g.lastRectH),
             unsigned(uz.w), unsigned(uz.h));
    return SAUER_NRD_OK;
}

static float f16tofloat(uint16_t h)
{
    uint32_t s = (h >> 15) & 1u;
    int32_t e = (h >> 10) & 0x1f;
    uint32_t m = h & 0x3ffu;
    uint32_t o;
    if(e == 0)
    {
        if(!m) o = s << 31;
        else
        {
            e = -1;
            do { m <<= 1; --e; } while(!(m & 0x400u));
            m &= 0x3ffu;
            o = (s << 31) | (uint32_t(e + 127) << 23) | (m << 13);
        }
    }
    else if(e == 31) o = (s << 31) | 0x7f800000u | (m << 13);
    else o = (s << 31) | (uint32_t(e + 127 - 15) << 23) | (m << 13);
    float f;
    memcpy(&f, &o, 4);
    return f;
}

static float loadf32(const uint8_t *p)
{
    float v;
    memcpy(&v, p, 4);
    return v;
}

static uint16_t loadu16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}

extern "C" __declspec(dllexport) int32_t sauer_nrd_probe_arm(const SauerNrdProbeIn *in)
{
    if(!g.alive) return SAUER_NRD_ERR_STATE;
    if(!in || in->struct_bytes != sizeof(SauerNrdProbeIn)) return SAUER_NRD_ERR_SIZE;
    g.probeN = in->n;
    if(g.probeN > SAUER_NRD_PROBE_MAX) g.probeN = SAUER_NRD_PROBE_MAX;
    for(uint32_t i = 0; i < g.probeN; i++)
    {
        g.probeX[i] = in->x[i];
        g.probeY[i] = in->y[i];
        g.probeGX[i] = in->x[i];
        g.probeGY[i] = in->y[i];
    }
    g.probeArmed = g.probeN > 0;
    g.probeCopied = false;
    if(g.probeMap) memset(g.probeMap, 0, g.probeBytes);
    return SAUER_NRD_OK;
}

extern "C" __declspec(dllexport) int32_t sauer_nrd_probe_read(SauerNrdProbeOut *out)
{
    if(!out || out->struct_bytes != sizeof(SauerNrdProbeOut)) return SAUER_NRD_ERR_SIZE;
    memset(out, 0, sizeof(*out));
    out->struct_bytes = sizeof(SauerNrdProbeOut);
    using T = nrd::ResourceType;
    Tex &uz = g.user[(size_t)T::IN_VIEWZ];
    out->pool_w = g.maxw;
    out->pool_h = g.maxh;
    out->user_w = uz.w;
    out->user_h = uz.h;
    out->rs_w = g.lastRsW;
    out->rs_h = g.lastRsH;
    out->rect_w = g.lastRectW;
    out->rect_h = g.lastRectH;
    out->n = g.probeN;
    if(!g.alive || !g.probeCopied || !g.probeMap) return SAUER_NRD_OK;
    out->ready = 1;
    for(uint32_t i = 0; i < g.probeN; i++)
    {
        const uint8_t *b = g.probeMap + i * kProbeStride;
        SauerNrdProbePx &p = out->px[i];
        p.x = g.probeX[i];
        p.y = g.probeY[i];
        p.gather_ix = g.probeGX[i];
        p.gather_iy = g.probeGY[i];
        p.prev_z_xy = loadf32(b + kOffPrevXY);
        p.prev_z_g00 = loadf32(b + kOffPrevG);
        p.prev_z_g10 = loadf32(b + kOffPrevG + 4);
        p.prev_z_g01 = loadf32(b + kOffPrevG + 8);
        p.prev_z_g11 = loadf32(b + kOffPrevG + 12);
        p.prev_z_after = loadf32(b + kOffPrevAfter);
        p.in_z = loadf32(b + kOffInZ);
        p.prev_hist = loadu16(b + kOffHist) & 63u;
        p.prev_hist_g = loadu16(b + kOffHistG) & 63u;
        p.out_w = f16tofloat(loadu16(b + kOffOut + 6));
        p.mv_x = f16tofloat(loadu16(b + kOffMV + 0));
        p.mv_y = f16tofloat(loadu16(b + kOffMV + 2));
        p.mv_z = f16tofloat(loadu16(b + kOffMV + 4));
        p.data1 = b[kOffData1];
        p.data2 = b[kOffData2];
        p.data1_frames = roundf((float)p.data1 / 255.0f * 63.0f);
    }
    return SAUER_NRD_OK;
}
