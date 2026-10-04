/* sauer_fsr.cpp — MSVC gateway around the AMD FidelityFX SDK v1.1.4 FSR 3.1
 * upscaler with its Vulkan backend (MIT licence, see LICENSE-FidelityFX.txt).
 *
 * Built by src\fsr_gateway\build-dll.bat together with the SDK sources for the
 * upscaler, the Vulkan backend and the shared helpers only. Frame generation,
 * frame interpolation, optical flow and the FI swap chain are not compiled in:
 * the one backend hook that points at the FI swap chain is a stub that refuses.
 * FidelityFX objects never leave this DLL. The MinGW client uses the C ABI in
 * include/fsr_gateway/sauer_fsr.h via LoadLibrary/GetProcAddress.
 */
#define SAUER_FSR_BUILD
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include <vulkan/vulkan.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include "fsr_gateway/sauer_fsr.h"

static const uint32_t kContextMagic = 0x46535231u; /* 'FSR1' */
static const char *kSdkName = "FidelityFX SDK v1.1.4 c6efa6bf";

/* ---- Vulkan instance-level entry points used directly by ffx_vk.cpp -------
 * The SDK calls a few physical-device queries as plain prototypes (it expects
 * vulkan-1.lib). The game already owns the loader, so resolve them through the
 * vkGetInstanceProcAddr it hands over instead of linking a second loader. */
static PFN_vkGetInstanceProcAddr g_gipa = NULL;
static VkInstance g_instance = VK_NULL_HANDLE;
static PFN_vkEnumerateDeviceExtensionProperties g_EnumDevExt = NULL;
static PFN_vkGetPhysicalDeviceMemoryProperties g_GetMemProps = NULL;
static PFN_vkGetPhysicalDeviceProperties g_GetProps = NULL;
static PFN_vkGetPhysicalDeviceProperties2 g_GetProps2 = NULL;
static PFN_vkGetPhysicalDeviceFeatures g_GetFeats = NULL;
static PFN_vkGetPhysicalDeviceFeatures2 g_GetFeats2 = NULL;

extern "C" {
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice pd, const char *layer, uint32_t *n, VkExtensionProperties *p)
{
    if(!g_EnumDevExt) { if(n) *n = 0; return VK_ERROR_INITIALIZATION_FAILED; }
    return g_EnumDevExt(pd, layer, n, p);
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties *p)
{
    if(g_GetMemProps) g_GetMemProps(pd, p); else memset(p, 0, sizeof(*p));
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice pd, VkPhysicalDeviceProperties *p)
{
    if(g_GetProps) g_GetProps(pd, p); else memset(p, 0, sizeof(*p));
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p)
{
    if(g_GetProps2) g_GetProps2(pd, p);
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures(VkPhysicalDevice pd, VkPhysicalDeviceFeatures *p)
{
    if(g_GetFeats) g_GetFeats(pd, p); else memset(p, 0, sizeof(*p));
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *p)
{
    if(g_GetFeats2) g_GetFeats2(pd, p);
}
/* Two device-level calls the SDK also makes as prototypes: the fallback when no
 * vkGetDeviceProcAddr is given (never taken, the game always passes one), and
 * the breadcrumbs test buffer created while the backend context starts. */
static PFN_vkGetDeviceProcAddr g_gdpa_fwd = NULL;
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice dev, const char *name)
{
    return g_gdpa_fwd ? g_gdpa_fwd(dev, name) : NULL;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice dev, const VkBufferCreateInfo *info, const VkAllocationCallbacks *alloc, VkBuffer *buf)
{
    PFN_vkCreateBuffer fn = g_gdpa_fwd ? (PFN_vkCreateBuffer)g_gdpa_fwd(dev, "vkCreateBuffer") : NULL;
    if(!fn) return VK_ERROR_INITIALIZATION_FAILED;
    return fn(dev, info, alloc, buf);
}

/* Frame generation is never built into this gateway. ffx_vk.cpp wires this
 * hook into every backend interface; it is only reached by frame generation. */
FFX_API FfxErrorCode ffxSetFrameGenerationConfigToSwapchainVK(FfxFrameGenerationConfig const *)
{
    return FFX_ERROR_INVALID_ARGUMENT;
}
}

struct FsrSlot
{
    uint32_t magic;
    uint32_t in_w, in_h, out_w, out_h, mode, flags, hdr;
    void *scratch;
    size_t scratchbytes;
    FfxInterface iface;
    FfxFsr3UpscalerContext ctx;
    FfxResourceInternal shared[3];
    bool ctxok;
};

static SauerFsrLogFn g_log = NULL;
static SauerFsrError g_err;
static bool g_inited = false;
static VkDevice g_device = VK_NULL_HANDLE;
static VkPhysicalDevice g_phys = VK_NULL_HANDLE;
static PFN_vkGetDeviceProcAddr g_gdpa = NULL;
static uint32_t g_devfeatures = 0;
static FsrSlot *g_slot = NULL;
static FfxErrorCode (*g_origcaps)(FfxInterface *, FfxDeviceCapabilities *) = NULL;
static SauerFsrInfo g_info;

static void seterr(int32_t code, int32_t ffx, const char *msg)
{
    g_err.struct_bytes = sizeof(g_err);
    g_err.code = code;
    g_err.ffx_result = ffx;
    g_err.message[0] = 0;
    if(msg)
    {
        strncpy(g_err.message, msg, SAUER_FSR_MSG - 1);
        g_err.message[SAUER_FSR_MSG - 1] = 0;
    }
}

static int checksize(uint32_t got, uint32_t expect, const char *what)
{
    if(got == expect) return 1;
    char buf[192];
    sprintf(buf, "%s size %u != %u (rebuild game and DLL together)", what, got, expect);
    seterr(SAUER_FSR_ERR_SIZE, 0, buf);
    return 0;
}

static void ffxmsg(FfxMsgType type, const wchar_t *message)
{
    if(!g_log || !message) return;
    char tmp[1100];
    char body[1024];
    body[0] = 0;
    WideCharToMultiByte(CP_UTF8, 0, message, -1, body, (int)sizeof(body), NULL, NULL);
    body[sizeof(body) - 1] = 0;
    _snprintf(tmp, sizeof(tmp), "%s %s", type == FFX_MESSAGE_TYPE_ERROR ? "error:" : "warning:", body);
    tmp[sizeof(tmp) - 1] = 0;
    g_log(tmp);
}

static void ffxglobalmsg(uint32_t type, const wchar_t *message)
{
    ffxmsg((FfxMsgType)type, message);
}

static const char *ffxerrstr(FfxErrorCode e)
{
    switch(e)
    {
        case FFX_OK: return "FFX_OK";
        case FFX_ERROR_INVALID_POINTER: return "FFX_ERROR_INVALID_POINTER";
        case FFX_ERROR_INVALID_ALIGNMENT: return "FFX_ERROR_INVALID_ALIGNMENT";
        case FFX_ERROR_INVALID_SIZE: return "FFX_ERROR_INVALID_SIZE";
        case FFX_EOF: return "FFX_EOF";
        case FFX_ERROR_INVALID_PATH: return "FFX_ERROR_INVALID_PATH";
        case FFX_ERROR_EOF: return "FFX_ERROR_EOF";
        case FFX_ERROR_MALFORMED_DATA: return "FFX_ERROR_MALFORMED_DATA";
        case FFX_ERROR_OUT_OF_MEMORY: return "FFX_ERROR_OUT_OF_MEMORY";
        case FFX_ERROR_INCOMPLETE_INTERFACE: return "FFX_ERROR_INCOMPLETE_INTERFACE";
        case FFX_ERROR_INVALID_ENUM: return "FFX_ERROR_INVALID_ENUM";
        case FFX_ERROR_INVALID_ARGUMENT: return "FFX_ERROR_INVALID_ARGUMENT";
        case FFX_ERROR_OUT_OF_RANGE: return "FFX_ERROR_OUT_OF_RANGE";
        case FFX_ERROR_NULL_DEVICE: return "FFX_ERROR_NULL_DEVICE";
        case FFX_ERROR_BACKEND_API_ERROR: return "FFX_ERROR_BACKEND_API_ERROR";
        case FFX_ERROR_INSUFFICIENT_MEMORY: return "FFX_ERROR_INSUFFICIENT_MEMORY";
        default: return "FFX error";
    }
}

static void setffxerr(const char *what, FfxErrorCode e)
{
    char msg[256];
    sprintf(msg, "%s %s (0x%08X)", what, ffxerrstr(e), (unsigned)e);
    seterr(SAUER_FSR_ERR_FFX, (int32_t)e, msg);
}

/* The SDK derives fp16 and wave-size variants from what the physical device
 * supports. Only report what the game enabled on its VkDevice: a variant that
 * needs shaderFloat16 on a device where it is off is undefined behaviour. The
 * Vulkan backend reports shader model 5.1, so wave64 is never forced anyway;
 * keep the lane counts it measured (they only select the reprojection LUT). */
static FfxErrorCode clampedcaps(FfxInterface *iface, FfxDeviceCapabilities *caps)
{
    FfxErrorCode r = g_origcaps ? g_origcaps(iface, caps) : FFX_ERROR_INCOMPLETE_INTERFACE;
    if(r != FFX_OK) return r;
    if(!(g_devfeatures & SAUER_FSR_DEV_FP16)) caps->fp16Supported = false;
    caps->raytracingSupported = false;
    return FFX_OK;
}

static bool resolveinstancefuns(void)
{
    if(!g_gipa || !g_instance) return false;
    g_EnumDevExt = (PFN_vkEnumerateDeviceExtensionProperties)g_gipa(g_instance, "vkEnumerateDeviceExtensionProperties");
    g_GetMemProps = (PFN_vkGetPhysicalDeviceMemoryProperties)g_gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties");
    g_GetProps = (PFN_vkGetPhysicalDeviceProperties)g_gipa(g_instance, "vkGetPhysicalDeviceProperties");
    g_GetProps2 = (PFN_vkGetPhysicalDeviceProperties2)g_gipa(g_instance, "vkGetPhysicalDeviceProperties2");
    if(!g_GetProps2) g_GetProps2 = (PFN_vkGetPhysicalDeviceProperties2)g_gipa(g_instance, "vkGetPhysicalDeviceProperties2KHR");
    g_GetFeats = (PFN_vkGetPhysicalDeviceFeatures)g_gipa(g_instance, "vkGetPhysicalDeviceFeatures");
    g_GetFeats2 = (PFN_vkGetPhysicalDeviceFeatures2)g_gipa(g_instance, "vkGetPhysicalDeviceFeatures2");
    if(!g_GetFeats2) g_GetFeats2 = (PFN_vkGetPhysicalDeviceFeatures2)g_gipa(g_instance, "vkGetPhysicalDeviceFeatures2KHR");
    return g_EnumDevExt && g_GetMemProps && g_GetProps && g_GetProps2 && g_GetFeats && g_GetFeats2;
}

/* The backend looks up some Vulkan 1.1 core entry points by their KHR alias
 * (vkGetBufferMemoryRequirements2KHR, called without a null check). A KHR alias
 * only resolves when that extension was enabled on the device; here the core
 * function is the same one, so fall back to the name without the suffix. */
static PFN_vkVoidFunction VKAPI_CALL gdpacore(VkDevice dev, const char *name)
{
    PFN_vkVoidFunction fn = g_gdpa ? g_gdpa(dev, name) : NULL;
    if(fn || !name) return fn;
    size_t n = strlen(name);
    if(n > 3 && n < 128 && !strcmp(name + n - 3, "KHR"))
    {
        char core[128];
        memcpy(core, name, n - 3);
        core[n - 3] = 0;
        fn = g_gdpa(dev, core);
    }
    return fn;
}

static FfxFsr3UpscalerQualityMode ffxmode(uint32_t mode)
{
    switch(mode)
    {
        case SAUER_FSR_MODE_QUALITY: return FFX_FSR3UPSCALER_QUALITY_MODE_QUALITY;
        case SAUER_FSR_MODE_BALANCED: return FFX_FSR3UPSCALER_QUALITY_MODE_BALANCED;
        case SAUER_FSR_MODE_PERFORMANCE: return FFX_FSR3UPSCALER_QUALITY_MODE_PERFORMANCE;
        default: return FFX_FSR3UPSCALER_QUALITY_MODE_NATIVEAA;
    }
}

static int modeok(uint32_t mode) { return mode <= SAUER_FSR_MODE_PERFORMANCE; }

uint32_t __cdecl sauer_fsr_abi_version(void)
{
    return SAUER_FSR_ABI_VERSION;
}

int32_t __cdecl sauer_fsr_set_log(SauerFsrLogFn fn)
{
    g_log = fn;
    return SAUER_FSR_OK;
}

int32_t __cdecl sauer_fsr_init(const SauerFsrInit *in, uint32_t bytes, SauerFsrInfo *out, uint32_t outbytes)
{
    if(!in || !checksize(bytes, sizeof(*in), "SauerFsrInit") || !checksize(in->struct_bytes, sizeof(*in), "SauerFsrInit.struct_bytes"))
        return g_err.code ? g_err.code : SAUER_FSR_ERR_SIZE;
    if(out && !checksize(outbytes, sizeof(*out), "SauerFsrInfo"))
        return g_err.code ? g_err.code : SAUER_FSR_ERR_SIZE;
    if(g_inited)
    {
        seterr(SAUER_FSR_ERR_STATE, 0, "already initialised; call shutdown first");
        return SAUER_FSR_ERR_STATE;
    }
    if(!in->instance || !in->physical_device || !in->device || !in->gipa || !in->gdpa)
    {
        seterr(SAUER_FSR_ERR_ARGS, 0, "init needs VkInstance, VkPhysicalDevice, VkDevice and both proc addr");
        return SAUER_FSR_ERR_ARGS;
    }
    g_gipa = (PFN_vkGetInstanceProcAddr)(uintptr_t)in->gipa;
    g_instance = (VkInstance)(uintptr_t)in->instance;
    if(!resolveinstancefuns())
    {
        seterr(SAUER_FSR_ERR_UNSUPPORTED, 0, "Vulkan instance lacks physical-device queries FSR needs (Vulkan 1.1)");
        return SAUER_FSR_ERR_UNSUPPORTED;
    }
    g_phys = (VkPhysicalDevice)(uintptr_t)in->physical_device;
    g_device = (VkDevice)(uintptr_t)in->device;
    g_gdpa = (PFN_vkGetDeviceProcAddr)(uintptr_t)in->gdpa;
    g_gdpa_fwd = g_gdpa;
    g_devfeatures = in->device_features;

    if(!(g_devfeatures & SAUER_FSR_DEV_FORMATLESS_STORAGE))
    {
        seterr(SAUER_FSR_ERR_UNSUPPORTED, 0, "storage images without format (read and write) are not enabled on this device");
        return SAUER_FSR_ERR_UNSUPPORTED;
    }
    /* Measured on the compiled shaders: every variant uses GroupNonUniformQuad
     * in compute (luma pyramid). */
    VkPhysicalDeviceSubgroupProperties sub = {};
    sub.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
    VkPhysicalDeviceProperties2 p2 = {};
    p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    p2.pNext = &sub;
    g_GetProps2(g_phys, &p2);
    if(!(sub.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) || !(sub.supportedOperations & VK_SUBGROUP_FEATURE_QUAD_BIT))
    {
        seterr(SAUER_FSR_ERR_UNSUPPORTED, 0, "no subgroup quad operations in compute shaders");
        return SAUER_FSR_ERR_UNSUPPORTED;
    }

    memset(&g_info, 0, sizeof(g_info));
    g_info.struct_bytes = sizeof(g_info);
    g_info.version_major = FFX_FSR3UPSCALER_VERSION_MAJOR;
    g_info.version_minor = FFX_FSR3UPSCALER_VERSION_MINOR;
    g_info.version_patch = FFX_FSR3UPSCALER_VERSION_PATCH;
    g_info.fp16 = (g_devfeatures & SAUER_FSR_DEV_FP16) ? 1u : 0u;
    g_info.wave_min = sub.subgroupSize;
    g_info.wave_max = sub.subgroupSize;
    strncpy(g_info.sdk, kSdkName, sizeof(g_info.sdk) - 1);
    if(out) *out = g_info;

    ffxFsr3UpscalerSetGlobalDebugMessage(ffxglobalmsg, 0);
    g_inited = true;
    char msg[256];
    sprintf(msg, "FSR %u.%u.%u upscaler (%s) on %s subgroup %u fp16 %u",
            g_info.version_major, g_info.version_minor, g_info.version_patch, kSdkName,
            p2.properties.deviceName, sub.subgroupSize, g_info.fp16);
    seterr(SAUER_FSR_OK, 0, msg);
    return SAUER_FSR_OK;
}

int32_t __cdecl sauer_fsr_query_optimal(SauerFsrOptimal *io, uint32_t bytes)
{
    if(!io || !checksize(bytes, sizeof(*io), "SauerFsrOptimal") || !checksize(io->struct_bytes, sizeof(*io), "SauerFsrOptimal.struct_bytes"))
        return g_err.code ? g_err.code : SAUER_FSR_ERR_SIZE;
    if(io->out_w < 8 || io->out_h < 8 || !modeok(io->mode))
    {
        seterr(SAUER_FSR_ERR_ARGS, 0, "query_optimal needs display size >= 8 and a FSR mode");
        return SAUER_FSR_ERR_ARGS;
    }
    uint32_t rw = io->out_w, rh = io->out_h;
    FfxErrorCode r = ffxFsr3UpscalerGetRenderResolutionFromQualityMode(&rw, &rh, io->out_w, io->out_h, ffxmode(io->mode));
    if(r != FFX_OK)
    {
        setffxerr("GetRenderResolutionFromQualityMode", r);
        return SAUER_FSR_ERR_FFX;
    }
    io->render_w = rw;
    io->render_h = rh;
    io->ratio = ffxFsr3UpscalerGetUpscaleRatioFromQualityMode(ffxmode(io->mode));
    io->jitter_phases = ffxFsr3UpscalerGetJitterPhaseCount((int32_t)rw, (int32_t)io->out_w);
    seterr(SAUER_FSR_OK, 0, "query_optimal ok");
    return SAUER_FSR_OK;
}

static void freeslot(FsrSlot *slot)
{
    if(!slot) return;
    if(slot->ctxok)
    {
        for(int i = 0; i < 3; i++)
            if(slot->shared[i].internalIndex)
                slot->iface.fpDestroyResource(&slot->iface, slot->shared[i], 0);
        ffxFsr3UpscalerContextDestroy(&slot->ctx);
        slot->ctxok = false;
    }
    if(slot->scratch) HeapFree(GetProcessHeap(), 0, slot->scratch);
    slot->magic = 0;
    HeapFree(GetProcessHeap(), 0, slot);
}

int32_t __cdecl sauer_fsr_create(const SauerFsrCreate *in, uint32_t bytes, uint64_t *out_handle, SauerFsrCreateInfo *out_info, uint32_t infobytes)
{
    if(!out_handle) { seterr(SAUER_FSR_ERR_ARGS, 0, "create null handle"); return SAUER_FSR_ERR_ARGS; }
    *out_handle = 0;
    if(!in || !checksize(bytes, sizeof(*in), "SauerFsrCreate") || !checksize(in->struct_bytes, sizeof(*in), "SauerFsrCreate.struct_bytes"))
        return g_err.code ? g_err.code : SAUER_FSR_ERR_SIZE;
    if(out_info)
    {
        if(!checksize(infobytes, sizeof(*out_info), "SauerFsrCreateInfo")) return g_err.code ? g_err.code : SAUER_FSR_ERR_SIZE;
        memset(out_info, 0, sizeof(*out_info));
        out_info->struct_bytes = sizeof(*out_info);
    }
    if(!g_inited)
    {
        seterr(SAUER_FSR_ERR_STATE, 0, "create before init");
        return SAUER_FSR_ERR_STATE;
    }
    if(g_slot)
    {
        seterr(SAUER_FSR_ERR_STATE, 0, "context already created; release first");
        return SAUER_FSR_ERR_STATE;
    }
    if(!modeok(in->mode) || in->in_w < 8 || in->in_h < 8 || in->out_w < in->in_w || in->out_h < in->in_h)
    {
        seterr(SAUER_FSR_ERR_ARGS, 0, "create needs a FSR mode and render size <= display size");
        return SAUER_FSR_ERR_ARGS;
    }
    if(in->mode == SAUER_FSR_MODE_NATIVE ? (in->in_w != in->out_w || in->in_h != in->out_h)
                                        : (in->in_w == in->out_w && in->in_h == in->out_h))
    {
        seterr(SAUER_FSR_ERR_ARGS, 0, in->mode == SAUER_FSR_MODE_NATIVE ? "FSR Native must be 1:1" : "FSR upscale mode at 1:1 refused");
        return SAUER_FSR_ERR_ARGS;
    }
    if(in->hdr && in->auto_exposure)
    {
        seterr(SAUER_FSR_ERR_ARGS, 0, "HDR uses the game exposure texture, not auto exposure");
        return SAUER_FSR_ERR_ARGS;
    }

    FsrSlot *slot = (FsrSlot *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(FsrSlot));
    if(!slot) { seterr(SAUER_FSR_ERR_IO, 0, "HeapAlloc slot failed"); return SAUER_FSR_ERR_IO; }
    slot->scratchbytes = ffxGetScratchMemorySizeVK(g_phys, FFX_FSR3UPSCALER_CONTEXT_COUNT);
    slot->scratch = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, slot->scratchbytes);
    if(!slot->scratch)
    {
        HeapFree(GetProcessHeap(), 0, slot);
        seterr(SAUER_FSR_ERR_IO, 0, "HeapAlloc scratch failed");
        return SAUER_FSR_ERR_IO;
    }
    VkDeviceContext devctx = { g_device, g_phys, gdpacore };
    FfxErrorCode r = ffxGetInterfaceVK(&slot->iface, ffxGetDeviceVK(&devctx), slot->scratch, slot->scratchbytes, FFX_FSR3UPSCALER_CONTEXT_COUNT);
    if(r != FFX_OK)
    {
        freeslot(slot);
        setffxerr("ffxGetInterfaceVK", r);
        return SAUER_FSR_ERR_FFX;
    }
    g_origcaps = slot->iface.fpGetDeviceCapabilities;
    slot->iface.fpGetDeviceCapabilities = clampedcaps;

    FfxFsr3UpscalerContextDescription desc;
    memset(&desc, 0, sizeof(desc));
    uint32_t flags = 0;
    if(in->hdr) flags |= FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE;
    if(!in->hdr && in->auto_exposure) flags |= FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
    if(in->depth_inverted) flags |= FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED;
    if(in->debug_checking) flags |= FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING;
    /* Render-resolution motion vectors, not jittered: no DISPLAY_RESOLUTION_MV,
     * no MOTION_VECTORS_JITTER_CANCELLATION. Depth is finite (0 near .. 1 far). */
    desc.flags = flags;
    desc.maxRenderSize.width = in->in_w;
    desc.maxRenderSize.height = in->in_h;
    desc.maxUpscaleSize.width = in->out_w;
    desc.maxUpscaleSize.height = in->out_h;
    desc.fpMessage = ffxmsg;
    desc.backendInterface = slot->iface;
    r = ffxFsr3UpscalerContextCreate(&slot->ctx, &desc);
    if(out_info) out_info->ffx_result = (int32_t)r;
    if(r != FFX_OK)
    {
        freeslot(slot);
        setffxerr("ffxFsr3UpscalerContextCreate", r);
        return SAUER_FSR_ERR_FFX;
    }
    slot->ctxok = true;

    FfxFsr3UpscalerSharedResourceDescriptions sd;
    memset(&sd, 0, sizeof(sd));
    r = ffxFsr3UpscalerGetSharedResourceDescriptions(&slot->ctx, &sd);
    if(r == FFX_OK) r = slot->iface.fpCreateResource(&slot->iface, &sd.dilatedDepth, 0, &slot->shared[0]);
    if(r == FFX_OK) r = slot->iface.fpCreateResource(&slot->iface, &sd.dilatedMotionVectors, 0, &slot->shared[1]);
    if(r == FFX_OK) r = slot->iface.fpCreateResource(&slot->iface, &sd.reconstructedPrevNearestDepth, 0, &slot->shared[2]);
    if(r != FFX_OK)
    {
        freeslot(slot);
        setffxerr("FSR shared resources", r);
        return SAUER_FSR_ERR_FFX;
    }

    slot->magic = kContextMagic;
    slot->in_w = in->in_w;
    slot->in_h = in->in_h;
    slot->out_w = in->out_w;
    slot->out_h = in->out_h;
    slot->mode = in->mode;
    slot->flags = flags;
    slot->hdr = in->hdr ? 1u : 0u;
    g_slot = slot;
    *out_handle = (uint64_t)(uintptr_t)slot;
    if(out_info)
    {
        out_info->in_w = slot->in_w;
        out_info->in_h = slot->in_h;
        out_info->out_w = slot->out_w;
        out_info->out_h = slot->out_h;
        out_info->mode = slot->mode;
        out_info->flags = flags;
        out_info->hdr = slot->hdr;
        out_info->auto_exposure = (flags & FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE) ? 1u : 0u;
    }
    char msg[256];
    sprintf(msg, "ffxFsr3UpscalerContextCreate mode=%u in %ux%u out %ux%u flags=0x%x HDR=%u AutoExposure=%u fp16=%u",
            (unsigned)in->mode, (unsigned)in->in_w, (unsigned)in->in_h, (unsigned)in->out_w, (unsigned)in->out_h,
            (unsigned)flags, (unsigned)slot->hdr, (flags & FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE) ? 1u : 0u, g_info.fp16);
    seterr(SAUER_FSR_OK, 0, msg);
    return SAUER_FSR_OK;
}

static FfxResource makeres(const SauerFsrVkImage &img, const wchar_t *name)
{
    if(!img.image)
    {
        FfxResource none;
        memset(&none, 0, sizeof(none));
        return none;
    }
    FfxResourceDescription d;
    memset(&d, 0, sizeof(d));
    d.type = FFX_RESOURCE_TYPE_TEXTURE2D;
    d.format = ffxGetSurfaceFormatVK((VkFormat)img.format);
    d.width = img.width;
    d.height = img.height;
    d.depth = 1;
    d.mipCount = 1;
    d.flags = FFX_RESOURCE_FLAGS_NONE;
    /* The game creates every shared image with VK_IMAGE_USAGE_STORAGE_BIT. */
    d.usage = FFX_RESOURCE_USAGE_UAV;
    /* GENERAL layout before and after: the backend puts it back when done. */
    return ffxGetResourceVK((void *)(uintptr_t)img.image, d, name, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
}

int32_t __cdecl sauer_fsr_dispatch(uint64_t handle, const SauerFsrDispatch *in, uint32_t bytes)
{
    if(!in || !checksize(bytes, sizeof(*in), "SauerFsrDispatch") || !checksize(in->struct_bytes, sizeof(*in), "SauerFsrDispatch.struct_bytes"))
        return g_err.code ? g_err.code : SAUER_FSR_ERR_SIZE;
    FsrSlot *slot = (FsrSlot *)(uintptr_t)handle;
    if(!g_inited || !slot || slot != g_slot || slot->magic != kContextMagic || !slot->ctxok)
    {
        seterr(SAUER_FSR_ERR_STATE, 0, "dispatch: invalid handle or not created");
        return SAUER_FSR_ERR_STATE;
    }
    if(!in->command_buffer || !in->color.image || !in->depth.image || !in->motion.image || !in->output.image)
    {
        seterr(SAUER_FSR_ERR_ARGS, 0, "dispatch needs colour, depth, motion and output images");
        return SAUER_FSR_ERR_ARGS;
    }
    uint32_t rw = in->render_w ? in->render_w : in->color.width;
    uint32_t rh = in->render_h ? in->render_h : in->color.height;
    if(rw != slot->in_w || rh != slot->in_h || in->output.width != slot->out_w || in->output.height != slot->out_h)
    {
        char msg[192];
        sprintf(msg, "dispatch render %ux%u out %ux%u != create %ux%u -> %ux%u",
                (unsigned)rw, (unsigned)rh, (unsigned)in->output.width, (unsigned)in->output.height,
                (unsigned)slot->in_w, (unsigned)slot->in_h, (unsigned)slot->out_w, (unsigned)slot->out_h);
        seterr(SAUER_FSR_ERR_ARGS, 0, msg);
        return SAUER_FSR_ERR_ARGS;
    }
    const uint32_t kHdrFmt = (uint32_t)VK_FORMAT_R16G16B16A16_SFLOAT;
    if(slot->hdr)
    {
        if(in->color.format != kHdrFmt || in->output.format != kHdrFmt || !in->exposure.image)
        {
            seterr(SAUER_FSR_ERR_ARGS, 0, "HDR needs RGBA16F colour in and out and the 1x1 exposure texture");
            return SAUER_FSR_ERR_ARGS;
        }
    }
    else if(in->color.format == kHdrFmt || in->output.format == kHdrFmt)
    {
        seterr(SAUER_FSR_ERR_ARGS, 0, "float colour without the HDR context");
        return SAUER_FSR_ERR_ARGS;
    }

    FfxFsr3UpscalerDispatchDescription d;
    memset(&d, 0, sizeof(d));
    d.commandList = ffxGetCommandListVK((VkCommandBuffer)(uintptr_t)in->command_buffer);
    d.color = makeres(in->color, L"sauer_color");
    d.depth = makeres(in->depth, L"sauer_depth");
    d.motionVectors = makeres(in->motion, L"sauer_motion");
    d.exposure = slot->hdr ? makeres(in->exposure, L"sauer_exposure") : makeres(SauerFsrVkImage(), NULL);
    d.reactive = makeres(in->reactive, L"sauer_reactive");
    d.transparencyAndComposition = makeres(SauerFsrVkImage(), NULL);
    d.output = makeres(in->output, L"sauer_output");
    d.dilatedDepth = slot->iface.fpGetResource(&slot->iface, slot->shared[0]);
    d.dilatedMotionVectors = slot->iface.fpGetResource(&slot->iface, slot->shared[1]);
    d.reconstructedPrevNearestDepth = slot->iface.fpGetResource(&slot->iface, slot->shared[2]);
    d.jitterOffset.x = in->jitter_x;
    d.jitterOffset.y = in->jitter_y;
    d.motionVectorScale.x = in->mv_scale_x != 0.0f ? in->mv_scale_x : 1.0f;
    d.motionVectorScale.y = in->mv_scale_y != 0.0f ? in->mv_scale_y : 1.0f;
    d.renderSize.width = rw;
    d.renderSize.height = rh;
    d.upscaleSize.width = slot->out_w;
    d.upscaleSize.height = slot->out_h;
    d.enableSharpening = in->sharpen != 0;
    d.sharpness = in->sharpness < 0.0f ? 0.0f : (in->sharpness > 1.0f ? 1.0f : in->sharpness);
    d.frameTimeDelta = in->frame_ms > 0.0f ? in->frame_ms : 16.6f;
    d.preExposure = in->pre_exposure > 0.0f ? in->pre_exposure : 1.0f;
    d.reset = in->reset != 0;
    d.cameraNear = in->camera_near;
    d.cameraFar = in->camera_far;
    d.cameraFovAngleVertical = in->camera_fov_v;
    d.viewSpaceToMetersFactor = in->view_to_meters > 0.0f ? in->view_to_meters : 1.0f;
    d.flags = 0;

    float rs = in->reactive_scale > 0.0f ? in->reactive_scale : 1.0f;
    ffxFsr3UpscalerSetConstant(&slot->ctx, FFX_FSR3UPSCALER_CONFIGURE_UPSCALE_KEY_FREACTIVENESSSCALE, &rs);

    FfxErrorCode r = ffxFsr3UpscalerContextDispatch(&slot->ctx, &d);
    if(r != FFX_OK)
    {
        setffxerr("ffxFsr3UpscalerContextDispatch", r);
        return SAUER_FSR_ERR_FFX;
    }
    seterr(SAUER_FSR_OK, 0, "dispatch ok");
    return SAUER_FSR_OK;
}

int32_t __cdecl sauer_fsr_get_stats(uint64_t handle, SauerFsrStats *out, uint32_t bytes)
{
    if(!out || !checksize(bytes, sizeof(*out), "SauerFsrStats") || !checksize(out->struct_bytes, sizeof(*out), "SauerFsrStats.struct_bytes"))
        return g_err.code ? g_err.code : SAUER_FSR_ERR_SIZE;
    FsrSlot *slot = (FsrSlot *)(uintptr_t)handle;
    out->vram_bytes = 0;
    out->ffx_result = 0;
    if(!slot || slot != g_slot || slot->magic != kContextMagic || !slot->ctxok)
    {
        seterr(SAUER_FSR_ERR_STATE, 0, "get_stats: no context");
        return SAUER_FSR_ERR_STATE;
    }
    FfxEffectMemoryUsage mem;
    memset(&mem, 0, sizeof(mem));
    FfxErrorCode r = ffxFsr3UpscalerContextGetGpuMemoryUsage(&slot->ctx, &mem);
    out->ffx_result = (int32_t)r;
    if(r != FFX_OK)
    {
        setffxerr("GetGpuMemoryUsage", r);
        return SAUER_FSR_ERR_FFX;
    }
    out->vram_bytes = mem.totalUsageInBytes;
    seterr(SAUER_FSR_OK, 0, "stats ok");
    return SAUER_FSR_OK;
}

int32_t __cdecl sauer_fsr_release(uint64_t handle)
{
    FsrSlot *slot = (FsrSlot *)(uintptr_t)handle;
    if(!slot) slot = g_slot;
    if(!slot)
    {
        seterr(SAUER_FSR_OK, 0, "release: nothing to free");
        return SAUER_FSR_OK;
    }
    if(slot != g_slot || slot->magic != kContextMagic)
    {
        seterr(SAUER_FSR_ERR_ARGS, 0, "release: bad handle");
        return SAUER_FSR_ERR_ARGS;
    }
    g_slot = NULL;
    freeslot(slot);
    seterr(SAUER_FSR_OK, 0, "context released");
    return SAUER_FSR_OK;
}

int32_t __cdecl sauer_fsr_shutdown(void)
{
    if(g_slot)
    {
        FsrSlot *slot = g_slot;
        g_slot = NULL;
        freeslot(slot);
    }
    g_inited = false;
    g_device = VK_NULL_HANDLE;
    g_phys = VK_NULL_HANDLE;
    g_gdpa = NULL;
    g_gdpa_fwd = NULL;
    g_gipa = NULL;
    g_instance = VK_NULL_HANDLE;
    g_EnumDevExt = NULL; g_GetMemProps = NULL; g_GetProps = NULL; g_GetProps2 = NULL; g_GetFeats = NULL; g_GetFeats2 = NULL;
    seterr(SAUER_FSR_OK, 0, "shutdown");
    return SAUER_FSR_OK;
}

int32_t __cdecl sauer_fsr_last_error(SauerFsrError *out, uint32_t bytes)
{
    if(!out || bytes < sizeof(*out)) return SAUER_FSR_ERR_SIZE;
    *out = g_err;
    out->struct_bytes = sizeof(*out);
    return SAUER_FSR_OK;
}
