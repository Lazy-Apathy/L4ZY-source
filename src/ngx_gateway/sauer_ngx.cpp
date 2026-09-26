/* sauer_ngx.cpp — MSVC gateway around the public NVIDIA NGX Vulkan SDK.
 *
 * Compile with /MT and link lib/Windows_x86_64/x64/nvsdk_ngx_s.lib from NVIDIA/DLSS.
 * NGX C++ objects never leave this DLL. The MinGW client uses the C ABI in
 * include/ngx_gateway/sauer_ngx.h via LoadLibrary/GetProcAddress.
 */
#define SAUER_NGX_BUILD
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "nvsdk_ngx_vk.h"
#include "nvsdk_ngx_helpers.h"
#include "nvsdk_ngx_helpers_vk.h"
#include "ngx_gateway/sauer_ngx.h"

static const char *kProjectId = "c3a7e9d1-4b52-4f08-9e6a-7d1c2b8f4a90";
static const char *kEngineVer = "cube2-hwrt-ngx";
static const uint32_t kFeatureMagic = 0x4E475831u; /* 'NGX1' */

struct FeatureSlot
{
    uint32_t magic;
    uint32_t in_w;
    uint32_t in_h;
    uint32_t out_w;
    uint32_t out_h;
    uint32_t mode;
    uint32_t hdr;
    uint32_t auto_exposure;
    int flags;
    NVSDK_NGX_Handle *handle;
};

static SauerNgxLogFn g_log = NULL;
static wchar_t g_dllw[MAX_PATH];
static wchar_t g_logw[MAX_PATH];
static const wchar_t *g_pathlist[1];
static NVSDK_NGX_FeatureCommonInfo g_common;
static bool g_paths_set = false;
static bool g_inited = false;
static VkDevice g_device = VK_NULL_HANDLE;
static NVSDK_NGX_Parameter *g_caps = NULL;
static NVSDK_NGX_Parameter *g_params = NULL;
static FeatureSlot *g_feature = NULL;
static SauerNgxError g_err;

static void seterr(int32_t code, uint32_t ngx, const char *msg)
{
    g_err.struct_bytes = sizeof(g_err);
    g_err.code = code;
    g_err.ngx_result = ngx;
    g_err.message[0] = 0;
    if(msg)
    {
        strncpy(g_err.message, msg, SAUER_NGX_MSG - 1);
        g_err.message[SAUER_NGX_MSG - 1] = 0;
    }
}

static void ngxlog(const char *message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)
{
    if(!message || !message[0]) return;
    if(g_log) g_log(message);
}

static void utf8tow(const char *src, wchar_t *dst, int dstw)
{
    dst[0] = 0;
    if(!src) return;
    MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, dstw);
    dst[dstw - 1] = 0;
}

static int checksize(uint32_t got, uint32_t expect, const char *what)
{
    if(got == expect) return 1;
    char buf[192];
    sprintf(buf, "%s size %u != %u (rebuild game and DLL together)", what, got, expect);
    seterr(SAUER_NGX_ERR_SIZE, 0, buf);
    return 0;
}

static const char *ngxstr(NVSDK_NGX_Result r)
{
    const wchar_t *w = GetNGXResultAsString(r);
    static char tmp[160];
    tmp[0] = 0;
    if(w) WideCharToMultiByte(CP_UTF8, 0, w, -1, tmp, (int)sizeof(tmp), NULL, NULL);
    if(!tmp[0]) sprintf(tmp, "NGX 0x%08X", (unsigned)r);
    return tmp;
}

static void fillcommon(void)
{
    memset(&g_common, 0, sizeof(g_common));
    g_pathlist[0] = g_dllw;
    g_common.PathListInfo.Path = g_pathlist;
    g_common.PathListInfo.Length = g_dllw[0] ? 1u : 0u;
    g_common.LoggingInfo.LoggingCallback = ngxlog;
    g_common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
    g_common.LoggingInfo.DisableOtherLoggingSinks = false;
}

static void filldiscovery(NVSDK_NGX_FeatureDiscoveryInfo *d)
{
    memset(d, 0, sizeof(*d));
    d->SDKVersion = NVSDK_NGX_Version_API;
    d->FeatureID = NVSDK_NGX_Feature_SuperSampling;
    d->Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    d->Identifier.v.ProjectDesc.ProjectId = kProjectId;
    d->Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    d->Identifier.v.ProjectDesc.EngineVersion = kEngineVer;
    d->ApplicationDataPath = g_logw[0] ? g_logw : L".";
    fillcommon();
    d->FeatureInfo = &g_common;
}

static int copyexts(const VkExtensionProperties *src, uint32_t n, SauerNgxExtList *out)
{
    out->count = 0;
    if(!src) return SAUER_NGX_OK;
    for(uint32_t i = 0; i < n && out->count < SAUER_NGX_MAX_EXTS; i++)
    {
        if(!src[i].extensionName[0]) continue;
        strncpy(out->names[out->count], src[i].extensionName, SAUER_NGX_EXT_NAME - 1);
        out->names[out->count][SAUER_NGX_EXT_NAME - 1] = 0;
        out->count++;
    }
    return SAUER_NGX_OK;
}

static NVSDK_NGX_Resource_VK makeimg(const SauerNgxVkImage *in)
{
    VkImageSubresourceRange range;
    memset(&range, 0, sizeof(range));
    range.aspectMask = in->aspect_mask ? in->aspect_mask : VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;
    return NVSDK_NGX_Create_ImageView_Resource_VK(
        (VkImageView)(uintptr_t)in->image_view,
        (VkImage)(uintptr_t)in->image,
        range,
        (VkFormat)in->format,
        in->width,
        in->height,
        in->read_write != 0);
}

static void destroyparams(void)
{
    if(g_params)
    {
        NVSDK_NGX_VULKAN_DestroyParameters(g_params);
        g_params = NULL;
    }
    if(g_caps)
    {
        NVSDK_NGX_VULKAN_DestroyParameters(g_caps);
        g_caps = NULL;
    }
}

uint32_t __cdecl sauer_ngx_abi_version(void)
{
    return SAUER_NGX_ABI_VERSION;
}

int32_t __cdecl sauer_ngx_probe_link(char *buf, uint32_t bufbytes)
{
    if(!buf || bufbytes < 8)
    {
        seterr(SAUER_NGX_ERR_ARGS, 0, "probe_link buffer too small");
        return SAUER_NGX_ERR_ARGS;
    }
    /* Taking these addresses pulls the official SDK wrapper out of nvsdk_ngx_s.lib.
     * That is the public C API, not a guessed CORE export. */
    const void *p_setvoid = (const void *)&NVSDK_NGX_Parameter_SetVoidPointer;
    const void *p_init = (const void *)&NVSDK_NGX_VULKAN_Init_with_ProjectID;
    const void *p_eval = (const void *)&NVSDK_NGX_VULKAN_EvaluateFeature_C;
    const void *p_create = (const void *)&NVSDK_NGX_VULKAN_CreateFeature1;
    const void *p_instext = (const void *)&NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements;
    _snprintf(buf, bufbytes,
              "abi=%u sdk_api=0x%X "
              "SetVoidPointer=%p Init_with_ProjectID=%p EvaluateFeature_C=%p "
              "CreateFeature1=%p GetFeatureInstanceExtensionRequirements=%p "
              "lib=nvsdk_ngx_s.lib crt=/MT",
              SAUER_NGX_ABI_VERSION, (unsigned)NVSDK_NGX_Version_API,
              p_setvoid, p_init, p_eval, p_create, p_instext);
    buf[bufbytes - 1] = 0;
    if(!p_setvoid || !p_init || !p_eval || !p_create || !p_instext)
    {
        seterr(SAUER_NGX_ERR_NGX, 0, "official NGX symbol is null after link");
        return SAUER_NGX_ERR_NGX;
    }
    seterr(SAUER_NGX_OK, 0, buf);
    return SAUER_NGX_OK;
}

int32_t __cdecl sauer_ngx_set_paths(const SauerNgxPaths *in, uint32_t bytes)
{
    if(!in || !checksize(bytes, sizeof(*in), "SauerNgxPaths") || !checksize(in->struct_bytes, sizeof(*in), "SauerNgxPaths.struct_bytes"))
        return g_err.code ? g_err.code : SAUER_NGX_ERR_SIZE;
    utf8tow(in->dll_dir, g_dllw, MAX_PATH);
    utf8tow(in->log_dir, g_logw, MAX_PATH);
    if(g_logw[0]) CreateDirectoryW(g_logw, NULL);
    g_paths_set = g_dllw[0] != 0;
    if(!g_paths_set)
    {
        seterr(SAUER_NGX_ERR_ARGS, 0, "dll_dir empty (need nvngx_dlss.dll folder)");
        return SAUER_NGX_ERR_ARGS;
    }
    seterr(SAUER_NGX_OK, 0, "paths set");
    return SAUER_NGX_OK;
}

int32_t __cdecl sauer_ngx_set_log(SauerNgxLogFn fn)
{
    g_log = fn;
    return SAUER_NGX_OK;
}

int32_t __cdecl sauer_ngx_query_instance_exts(SauerNgxExtList *out, uint32_t bytes)
{
    if(!out || !checksize(bytes, sizeof(*out), "SauerNgxExtList") || !checksize(out->struct_bytes, sizeof(*out), "SauerNgxExtList.struct_bytes"))
        return g_err.code ? g_err.code : SAUER_NGX_ERR_SIZE;
    memset(out->names, 0, sizeof(out->names));
    out->count = 0;
    NVSDK_NGX_FeatureDiscoveryInfo disc;
    filldiscovery(&disc);
    uint32_t n = 0;
    VkExtensionProperties *props = NULL;
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&disc, &n, &props);
    if(NVSDK_NGX_FAILED(r))
    {
        char msg[192];
        sprintf(msg, "GetFeatureInstanceExtensionRequirements %s", ngxstr(r));
        seterr(SAUER_NGX_ERR_NGX, (uint32_t)r, msg);
        return SAUER_NGX_ERR_NGX;
    }
    copyexts(props, n, out);
    seterr(SAUER_NGX_OK, (uint32_t)r, "instance extensions queried");
    return SAUER_NGX_OK;
}

int32_t __cdecl sauer_ngx_query_device_exts(uint64_t instance, uint64_t physical_device, SauerNgxExtList *out, uint32_t bytes)
{
    if(!out || !checksize(bytes, sizeof(*out), "SauerNgxExtList") || !checksize(out->struct_bytes, sizeof(*out), "SauerNgxExtList.struct_bytes"))
        return g_err.code ? g_err.code : SAUER_NGX_ERR_SIZE;
    if(!instance || !physical_device)
    {
        seterr(SAUER_NGX_ERR_ARGS, 0, "device ext query needs VkInstance and VkPhysicalDevice");
        return SAUER_NGX_ERR_ARGS;
    }
    memset(out->names, 0, sizeof(out->names));
    out->count = 0;
    NVSDK_NGX_FeatureDiscoveryInfo disc;
    filldiscovery(&disc);
    uint32_t n = 0;
    VkExtensionProperties *props = NULL;
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
        (VkInstance)(uintptr_t)instance,
        (VkPhysicalDevice)(uintptr_t)physical_device,
        &disc, &n, &props);
    if(NVSDK_NGX_FAILED(r))
    {
        char msg[192];
        sprintf(msg, "GetFeatureDeviceExtensionRequirements %s", ngxstr(r));
        seterr(SAUER_NGX_ERR_NGX, (uint32_t)r, msg);
        return SAUER_NGX_ERR_NGX;
    }
    copyexts(props, n, out);
    seterr(SAUER_NGX_OK, (uint32_t)r, "device extensions queried");
    return SAUER_NGX_OK;
}

int32_t __cdecl sauer_ngx_query_caps(uint64_t instance, uint64_t physical_device, SauerNgxCaps *out, uint32_t bytes)
{
    if(!out || !checksize(bytes, sizeof(*out), "SauerNgxCaps") || !checksize(out->struct_bytes, sizeof(*out), "SauerNgxCaps.struct_bytes"))
        return g_err.code ? g_err.code : SAUER_NGX_ERR_SIZE;
    memset(out->min_os, 0, sizeof(out->min_os));
    out->feature_supported = -1;
    out->min_hw_arch = 0;
    out->super_sampling_available = 0;
    out->ngx_result = 0;
    NVSDK_NGX_FeatureDiscoveryInfo disc;
    filldiscovery(&disc);
    NVSDK_NGX_FeatureRequirement req;
    memset(&req, 0, sizeof(req));
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetFeatureRequirements(
        (VkInstance)(uintptr_t)instance,
        (VkPhysicalDevice)(uintptr_t)physical_device,
        &disc, &req);
    out->ngx_result = (uint32_t)r;
    if(NVSDK_NGX_FAILED(r))
    {
        char msg[192];
        sprintf(msg, "GetFeatureRequirements %s", ngxstr(r));
        seterr(SAUER_NGX_ERR_NGX, (uint32_t)r, msg);
        return SAUER_NGX_ERR_NGX;
    }
    out->feature_supported = (int32_t)req.FeatureSupported;
    out->min_hw_arch = req.MinHWArchitecture;
    strncpy(out->min_os, req.MinOSVersion, sizeof(out->min_os) - 1);
    if(g_caps)
    {
        unsigned int avail = 0;
        if(NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_SuperSampling_Available, &avail)))
            out->super_sampling_available = avail;
    }
    seterr(SAUER_NGX_OK, (uint32_t)r, "caps queried");
    return SAUER_NGX_OK;
}

int32_t __cdecl sauer_ngx_init(const SauerNgxInit *in, uint32_t bytes)
{
    if(!in || !checksize(bytes, sizeof(*in), "SauerNgxInit") || !checksize(in->struct_bytes, sizeof(*in), "SauerNgxInit.struct_bytes"))
        return g_err.code ? g_err.code : SAUER_NGX_ERR_SIZE;
    if(!in->instance || !in->physical_device || !in->device)
    {
        seterr(SAUER_NGX_ERR_ARGS, 0, "init needs VkInstance, VkPhysicalDevice, VkDevice");
        return SAUER_NGX_ERR_ARGS;
    }
    if(g_inited)
    {
        seterr(SAUER_NGX_ERR_STATE, 0, "already initialised; call shutdown first");
        return SAUER_NGX_ERR_STATE;
    }
    fillcommon();
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_Init_with_ProjectID(
        kProjectId,
        NVSDK_NGX_ENGINE_TYPE_CUSTOM,
        kEngineVer,
        g_logw[0] ? g_logw : L".",
        (VkInstance)(uintptr_t)in->instance,
        (VkPhysicalDevice)(uintptr_t)in->physical_device,
        (VkDevice)(uintptr_t)in->device,
        (PFN_vkGetInstanceProcAddr)(uintptr_t)in->gipa,
        (PFN_vkGetDeviceProcAddr)(uintptr_t)in->gdpa,
        &g_common,
        NVSDK_NGX_Version_API);
    if(NVSDK_NGX_FAILED(r))
    {
        char msg[192];
        sprintf(msg, "NVSDK_NGX_VULKAN_Init_with_ProjectID %s", ngxstr(r));
        seterr(SAUER_NGX_ERR_NGX, (uint32_t)r, msg);
        return SAUER_NGX_ERR_NGX;
    }
    g_device = (VkDevice)(uintptr_t)in->device;
    r = NVSDK_NGX_VULKAN_GetCapabilityParameters(&g_caps);
    if(NVSDK_NGX_FAILED(r) || !g_caps)
    {
        NVSDK_NGX_VULKAN_Shutdown1(g_device);
        g_device = VK_NULL_HANDLE;
        char msg[192];
        sprintf(msg, "GetCapabilityParameters %s", ngxstr(r));
        seterr(SAUER_NGX_ERR_NGX, (uint32_t)r, msg);
        return SAUER_NGX_ERR_NGX;
    }
    r = NVSDK_NGX_VULKAN_AllocateParameters(&g_params);
    if(NVSDK_NGX_FAILED(r) || !g_params)
    {
        destroyparams();
        NVSDK_NGX_VULKAN_Shutdown1(g_device);
        g_device = VK_NULL_HANDLE;
        char msg[192];
        sprintf(msg, "AllocateParameters %s", ngxstr(r));
        seterr(SAUER_NGX_ERR_NGX, (uint32_t)r, msg);
        return SAUER_NGX_ERR_NGX;
    }
    unsigned int avail = 0;
    NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_SuperSampling_Available, &avail);
    if(!avail)
    {
        destroyparams();
        NVSDK_NGX_VULKAN_Shutdown1(g_device);
        g_device = VK_NULL_HANDLE;
        seterr(SAUER_NGX_ERR_UNSUPPORTED, 0, "SuperSampling.Available is 0");
        return SAUER_NGX_ERR_UNSUPPORTED;
    }
    g_inited = true;
    seterr(SAUER_NGX_OK, (uint32_t)NVSDK_NGX_Result_Success, "NGX Vulkan Init_with_ProjectID ok");
    return SAUER_NGX_OK;
}

static int mode_ok(uint32_t mode)
{
    return mode == SAUER_NGX_MODE_DLAA
        || mode == SAUER_NGX_MODE_QUALITY
        || mode == SAUER_NGX_MODE_BALANCED
        || mode == SAUER_NGX_MODE_PERFORMANCE;
}

static int mode_is_sr(uint32_t mode)
{
    return mode == SAUER_NGX_MODE_QUALITY
        || mode == SAUER_NGX_MODE_BALANCED
        || mode == SAUER_NGX_MODE_PERFORMANCE;
}

static NVSDK_NGX_PerfQuality_Value perf_for_mode(uint32_t mode)
{
    if(mode == SAUER_NGX_MODE_QUALITY) return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    if(mode == SAUER_NGX_MODE_BALANCED) return NVSDK_NGX_PerfQuality_Value_Balanced;
    if(mode == SAUER_NGX_MODE_PERFORMANCE) return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    return NVSDK_NGX_PerfQuality_Value_DLAA;
}

static const char *preset_name_for_mode(uint32_t mode)
{
    if(mode == SAUER_NGX_MODE_QUALITY) return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality;
    if(mode == SAUER_NGX_MODE_BALANCED) return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced;
    if(mode == SAUER_NGX_MODE_PERFORMANCE) return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance;
    return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA;
}

/* Documented Super Resolution presets in this SDK: K for DLAA/Quality/Balanced,
 * M for Performance. Do not use Ray Reconstruction / DLSSD hint names. */
static unsigned preset_value_for_mode(uint32_t mode)
{
    if(mode == SAUER_NGX_MODE_PERFORMANCE) return (unsigned)NVSDK_NGX_DLSS_Hint_Render_Preset_M;
    return (unsigned)NVSDK_NGX_DLSS_Hint_Render_Preset_K;
}

static int32_t fill_optimal(uint32_t out_w, uint32_t out_h, uint32_t mode, SauerNgxOptimal *io)
{
    io->out_w = out_w;
    io->out_h = out_h;
    io->mode = mode;
    io->opt_w = out_w;
    io->opt_h = out_h;
    io->max_w = out_w;
    io->max_h = out_h;
    io->min_w = out_w;
    io->min_h = out_h;
    io->sharpness = 0;
    io->ngx_result = 0;
    io->callback_ok = 0;
    if(!g_inited)
    {
        seterr(SAUER_NGX_ERR_STATE, 0, "query_optimal before init");
        return SAUER_NGX_ERR_STATE;
    }
    NVSDK_NGX_Parameter *p = g_caps ? g_caps : g_params;
    if(!p)
    {
        seterr(SAUER_NGX_ERR_STATE, 0, "query_optimal: no capability parameters");
        return SAUER_NGX_ERR_STATE;
    }
    unsigned int optw = 0, opth = 0, maxw = 0, maxh = 0, minw = 0, minh = 0;
    float sharp = 0;
    NVSDK_NGX_Result r = NGX_DLSS_GET_OPTIMAL_SETTINGS(
        p, out_w, out_h, perf_for_mode(mode),
        &optw, &opth, &maxw, &maxh, &minw, &minh, &sharp);
    io->ngx_result = (uint32_t)r;
    if(NVSDK_NGX_FAILED(r))
    {
        char msg[192];
        sprintf(msg, "NGX_DLSS_GET_OPTIMAL_SETTINGS %s", ngxstr(r));
        seterr(SAUER_NGX_ERR_NGX, (uint32_t)r, msg);
        return SAUER_NGX_ERR_NGX;
    }
    io->callback_ok = 1;
    io->opt_w = optw;
    io->opt_h = opth;
    io->max_w = maxw ? maxw : optw;
    io->max_h = maxh ? maxh : opth;
    io->min_w = minw ? minw : optw;
    io->min_h = minh ? minh : opth;
    io->sharpness = sharp;
    if(g_log)
    {
        char msg[256];
        sprintf(msg, "GET_OPTIMAL_SETTINGS mode=%u display %ux%u -> opt %ux%u min %ux%u max %ux%u sharp %.3f",
                (unsigned)mode, (unsigned)out_w, (unsigned)out_h,
                (unsigned)io->opt_w, (unsigned)io->opt_h,
                (unsigned)io->min_w, (unsigned)io->min_h,
                (unsigned)io->max_w, (unsigned)io->max_h, io->sharpness);
        g_log(msg);
    }
    seterr(SAUER_NGX_OK, (uint32_t)r, "GET_OPTIMAL_SETTINGS ok");
    return SAUER_NGX_OK;
}

int32_t __cdecl sauer_ngx_query_optimal(SauerNgxOptimal *io, uint32_t bytes)
{
    if(!io || !checksize(bytes, sizeof(*io), "SauerNgxOptimal") || !checksize(io->struct_bytes, sizeof(*io), "SauerNgxOptimal.struct_bytes"))
        return g_err.code ? g_err.code : SAUER_NGX_ERR_SIZE;
    if(io->out_w < 8 || io->out_h < 8)
    {
        seterr(SAUER_NGX_ERR_ARGS, 0, "query_optimal needs display size >= 8");
        return SAUER_NGX_ERR_ARGS;
    }
    if(!mode_ok(io->mode))
    {
        seterr(SAUER_NGX_ERR_ARGS, 0, "query_optimal mode must be DLAA, Quality, Balanced or Performance");
        return SAUER_NGX_ERR_ARGS;
    }
    uint32_t out_w = io->out_w, out_h = io->out_h, mode = io->mode;
    return fill_optimal(out_w, out_h, mode, io);
}

static int32_t create_feature_common(uint64_t device, uint64_t command_buffer,
                                     uint32_t out_w, uint32_t out_h,
                                     uint32_t in_w, uint32_t in_h,
                                     uint32_t mode, uint32_t preset_k, uint32_t auto_exposure,
                                     uint32_t hdr,
                                     uint64_t *out_handle, SauerNgxCreateInfo *info)
{
    if(out_handle) *out_handle = 0;
    if(info)
    {
        memset(info, 0, sizeof(*info));
        info->struct_bytes = sizeof(*info);
        info->mode_requested = mode;
        info->out_w = out_w;
        info->out_h = out_h;
    }
    if(!g_inited || !g_params)
    {
        seterr(SAUER_NGX_ERR_STATE, 0, "create_feature before init");
        return SAUER_NGX_ERR_STATE;
    }
    if(!command_buffer || out_w < 8 || out_h < 8)
    {
        seterr(SAUER_NGX_ERR_ARGS, 0, "create_feature needs recording command buffer and display size");
        return SAUER_NGX_ERR_ARGS;
    }
    if(!mode_ok(mode))
    {
        seterr(SAUER_NGX_ERR_ARGS, 0, "create_feature mode must be DLAA, Quality, Balanced or Performance");
        return SAUER_NGX_ERR_ARGS;
    }
    if(g_feature)
    {
        seterr(SAUER_NGX_ERR_STATE, 0, "feature already created; release first");
        return SAUER_NGX_ERR_STATE;
    }

    SauerNgxOptimal opt;
    memset(&opt, 0, sizeof(opt));
    opt.struct_bytes = sizeof(opt);
    int32_t oq = fill_optimal(out_w, out_h, mode, &opt);
    if(info)
    {
        info->optimal_w = opt.opt_w;
        info->optimal_h = opt.opt_h;
        info->ngx_result = opt.ngx_result;
    }
    if(oq != SAUER_NGX_OK)
        return oq;
    if(!opt.opt_w || !opt.opt_h)
    {
        seterr(SAUER_NGX_ERR_NGX, opt.ngx_result, "GET_OPTIMAL_SETTINGS returned 0x0");
        return SAUER_NGX_ERR_NGX;
    }

    uint32_t use_w = opt.opt_w;
    uint32_t use_h = opt.opt_h;
    uint32_t used_optimal = 1;
    if(mode == SAUER_NGX_MODE_DLAA)
    {
        /* DLAA is native 1:1. The callback is recorded, then input is forced to the display size. */
        use_w = out_w;
        use_h = out_h;
        used_optimal = 0;
        if(g_log && (opt.opt_w != out_w || opt.opt_h != out_h))
        {
            char msg[192];
            sprintf(msg, "DLAA GET_OPTIMAL suggested %ux%u; forcing native 1:1 %ux%u",
                    (unsigned)opt.opt_w, (unsigned)opt.opt_h, (unsigned)out_w, (unsigned)out_h);
            g_log(msg);
        }
    }
    else if(mode_is_sr(mode))
    {
        if(in_w >= 8 && in_h >= 8)
        {
            if(in_w < opt.min_w || in_h < opt.min_h || in_w > opt.max_w || in_h > opt.max_h)
            {
                char msg[192];
                sprintf(msg, "SR mode %u input %ux%u outside NGX min %ux%u max %ux%u",
                        (unsigned)mode, (unsigned)in_w, (unsigned)in_h,
                        (unsigned)opt.min_w, (unsigned)opt.min_h,
                        (unsigned)opt.max_w, (unsigned)opt.max_h);
                seterr(SAUER_NGX_ERR_ARGS, 0, msg);
                return SAUER_NGX_ERR_ARGS;
            }
            use_w = in_w;
            use_h = in_h;
            used_optimal = (in_w == opt.opt_w && in_h == opt.opt_h) ? 1u : 0u;
        }
        if(use_w == out_w && use_h == out_h)
        {
            seterr(SAUER_NGX_ERR_UNSUPPORTED, opt.ngx_result,
                   "SR GET_OPTIMAL_SETTINGS returned 1:1; refusing fake native Super Resolution");
            return SAUER_NGX_ERR_UNSUPPORTED;
        }
    }
    else
    {
        seterr(SAUER_NGX_ERR_ARGS, 0, "create_feature mode not a Super Resolution mode");
        return SAUER_NGX_ERR_ARGS;
    }
    if(info)
    {
        info->in_w = use_w;
        info->in_h = use_h;
        info->used_optimal = used_optimal;
        info->mode_applied = mode;
    }

    if(hdr && auto_exposure)
    {
        seterr(SAUER_NGX_ERR_ARGS, 0, "HDR refuses AutoExposure; normalisation is the exposure texture, not a content estimate");
        return SAUER_NGX_ERR_ARGS;
    }
    int flags = 0;
    if(hdr) flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    else if(auto_exposure) flags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    /* Depth is 0-1: do not set DepthInverted. Packed MVs are the same
     * resolution as the colour input, in that buffer's pixel units, and are
     * not jittered. MVLowRes must be set: without it NGX treats those values
     * as display-pixel units. That is harmless for 1:1 DLAA and under-scales
     * motion by in/out for SR modes. IsHDR is set only for the float contract. */
    flags |= NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;

    unsigned preset = 0;
    if(preset_k)
    {
        preset = preset_value_for_mode(mode);
        NVSDK_NGX_Parameter_SetUI(g_params, preset_name_for_mode(mode), preset);
    }

    NVSDK_NGX_DLSS_Create_Params create{};
    create.Feature.InWidth = use_w;
    create.Feature.InHeight = use_h;
    create.Feature.InTargetWidth = out_w;
    create.Feature.InTargetHeight = out_h;
    create.Feature.InPerfQualityValue = perf_for_mode(mode);
    create.InFeatureCreateFlags = flags;
    create.InEnableOutputSubrects = false;

    size_t scratch = 0;
    NVSDK_NGX_VULKAN_GetScratchBufferSize(NVSDK_NGX_Feature_SuperSampling, g_params, &scratch);
    if(scratch && g_log)
    {
        char msg[128];
        sprintf(msg, "GetScratchBufferSize=%zu (CreateFeature1 proceeds; SDK usually wants 0 for Vulkan DLSS)", scratch);
        g_log(msg);
    }

    FeatureSlot *slot = (FeatureSlot *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(FeatureSlot));
    if(!slot)
    {
        seterr(SAUER_NGX_ERR_IO, 0, "HeapAlloc feature slot failed");
        return SAUER_NGX_ERR_IO;
    }

    VkDevice dev = device ? (VkDevice)(uintptr_t)device : g_device;
    NVSDK_NGX_Result r = NGX_VULKAN_CREATE_DLSS_EXT1(
        dev,
        (VkCommandBuffer)(uintptr_t)command_buffer,
        1, 1,
        &slot->handle,
        g_params,
        &create);
    if(info) info->ngx_result = (uint32_t)r;
    if(NVSDK_NGX_FAILED(r) || !slot->handle)
    {
        HeapFree(GetProcessHeap(), 0, slot);
        char msg[192];
        sprintf(msg, "NGX_VULKAN_CREATE_DLSS_EXT1 %s", ngxstr(r));
        seterr(SAUER_NGX_ERR_NGX, (uint32_t)r, msg);
        return SAUER_NGX_ERR_NGX;
    }
    slot->magic = kFeatureMagic;
    slot->in_w = use_w;
    slot->in_h = use_h;
    slot->out_w = out_w;
    slot->out_h = out_h;
    slot->mode = mode;
    slot->hdr = hdr ? 1u : 0u;
    slot->auto_exposure = (!hdr && auto_exposure) ? 1u : 0u;
    slot->flags = flags;
    g_feature = slot;
    if(out_handle) *out_handle = (uint64_t)(uintptr_t)slot;
    if(info)
    {
        info->flags = (uint32_t)flags;
        info->hdr = slot->hdr;
        info->auto_exposure = slot->auto_exposure;
    }
    char okmsg[220];
    sprintf(okmsg, "NGX_VULKAN_CREATE_DLSS_EXT1 mode=%u in %ux%u out %ux%u used_optimal=%u flags=0x%x IsHDR=%u AutoExposure=%u MVLowRes=1 preset=%u",
            (unsigned)mode, (unsigned)use_w, (unsigned)use_h, (unsigned)out_w, (unsigned)out_h,
            (unsigned)used_optimal, (unsigned)flags, (unsigned)slot->hdr, (unsigned)slot->auto_exposure, preset);
    seterr(SAUER_NGX_OK, (uint32_t)r, okmsg);
    return SAUER_NGX_OK;
}

int32_t __cdecl sauer_ngx_create_dlaa(const SauerNgxCreate *in, uint32_t bytes, uint64_t *out_handle)
{
    if(!out_handle) { seterr(SAUER_NGX_ERR_ARGS, 0, "create_dlaa null handle"); return SAUER_NGX_ERR_ARGS; }
    *out_handle = 0;
    if(!in || !checksize(bytes, sizeof(*in), "SauerNgxCreate") || !checksize(in->struct_bytes, sizeof(*in), "SauerNgxCreate.struct_bytes"))
        return g_err.code ? g_err.code : SAUER_NGX_ERR_SIZE;
    return create_feature_common(in->device, in->command_buffer, in->width, in->height, 0, 0,
                                 SAUER_NGX_MODE_DLAA, in->preset_k, in->auto_exposure, 0, out_handle, NULL);
}

int32_t __cdecl sauer_ngx_create_feature(const SauerNgxCreateFeature *in, uint32_t bytes, uint64_t *out_handle, SauerNgxCreateInfo *out_info, uint32_t infobytes)
{
    if(!out_handle) { seterr(SAUER_NGX_ERR_ARGS, 0, "create_feature null handle"); return SAUER_NGX_ERR_ARGS; }
    *out_handle = 0;
    if(!in || !checksize(bytes, sizeof(*in), "SauerNgxCreateFeature") || !checksize(in->struct_bytes, sizeof(*in), "SauerNgxCreateFeature.struct_bytes"))
        return g_err.code ? g_err.code : SAUER_NGX_ERR_SIZE;
    if(out_info)
    {
        if(!checksize(infobytes, sizeof(*out_info), "SauerNgxCreateInfo"))
            return g_err.code ? g_err.code : SAUER_NGX_ERR_SIZE;
        if(out_info->struct_bytes && out_info->struct_bytes != sizeof(*out_info))
        {
            seterr(SAUER_NGX_ERR_SIZE, 0, "SauerNgxCreateInfo.struct_bytes mismatch");
            return SAUER_NGX_ERR_SIZE;
        }
    }
    return create_feature_common(in->device, in->command_buffer, in->out_w, in->out_h, in->in_w, in->in_h,
                                 in->mode, in->preset_k, in->auto_exposure, in->hdr, out_handle, out_info);
}

int32_t __cdecl sauer_ngx_evaluate(uint64_t handle, const SauerNgxEval *in, uint32_t bytes)
{
    if(!in || !checksize(bytes, sizeof(*in), "SauerNgxEval") || !checksize(in->struct_bytes, sizeof(*in), "SauerNgxEval.struct_bytes"))
        return g_err.code ? g_err.code : SAUER_NGX_ERR_SIZE;
    FeatureSlot *slot = (FeatureSlot *)(uintptr_t)handle;
    if(!g_inited || !g_params || !slot || slot->magic != kFeatureMagic || !slot->handle)
    {
        seterr(SAUER_NGX_ERR_STATE, 0, "evaluate: invalid handle or not created");
        return SAUER_NGX_ERR_STATE;
    }
    if(!in->command_buffer || !in->color.image_view || !in->depth.image_view ||
       !in->motion.image_view || !in->output.image_view)
    {
        seterr(SAUER_NGX_ERR_ARGS, 0, "evaluate needs Color, Depth, MotionVectors, Output image views");
        return SAUER_NGX_ERR_ARGS;
    }

    uint32_t rw = in->render_w ? in->render_w : in->color.width;
    uint32_t rh = in->render_h ? in->render_h : in->color.height;
    if(rw != slot->in_w || rh != slot->in_h)
    {
        char msg[192];
        sprintf(msg, "evaluate render %ux%u != create input %ux%u",
                (unsigned)rw, (unsigned)rh, (unsigned)slot->in_w, (unsigned)slot->in_h);
        seterr(SAUER_NGX_ERR_ARGS, 0, msg);
        return SAUER_NGX_ERR_ARGS;
    }
    if(in->output.width != slot->out_w || in->output.height != slot->out_h)
    {
        char msg[192];
        sprintf(msg, "evaluate output %ux%u != create target %ux%u",
                (unsigned)in->output.width, (unsigned)in->output.height,
                (unsigned)slot->out_w, (unsigned)slot->out_h);
        seterr(SAUER_NGX_ERR_ARGS, 0, msg);
        return SAUER_NGX_ERR_ARGS;
    }
    if((in->hdr ? 1u : 0u) != slot->hdr)
    {
        seterr(SAUER_NGX_ERR_ARGS, 0, "evaluate hdr flag does not match the created feature");
        return SAUER_NGX_ERR_ARGS;
    }
    const uint32_t kHdrFmt = (uint32_t)VK_FORMAT_R16G16B16A16_SFLOAT;
    if(slot->hdr)
    {
        if(in->color.format != kHdrFmt || in->output.format != kHdrFmt)
        {
            seterr(SAUER_NGX_ERR_ARGS, 0, "IsHDR requires RGBA16F colour in and out");
            return SAUER_NGX_ERR_ARGS;
        }
        if(!in->exposure.image_view || !in->exposure.image || in->exposure.width < 1 || in->exposure.height < 1)
        {
            seterr(SAUER_NGX_ERR_ARGS, 0, "IsHDR requires the 1x1 exposure texture (AutoExposure is off)");
            return SAUER_NGX_ERR_ARGS;
        }
    }
    else if(in->color.format == kHdrFmt || in->output.format == kHdrFmt)
    {
        seterr(SAUER_NGX_ERR_ARGS, 0, "float colour without IsHDR");
        return SAUER_NGX_ERR_ARGS;
    }

    /* Official helper: NVSDK_NGX_Create_ImageView_Resource_VK + NGX_VULKAN_EVALUATE_DLSS_EXT
     * which calls NVSDK_NGX_Parameter_SetVoidPointer from nvsdk_ngx_s.lib. */
    NVSDK_NGX_Resource_VK color = makeimg(&in->color);
    NVSDK_NGX_Resource_VK depth = makeimg(&in->depth);
    NVSDK_NGX_Resource_VK motion = makeimg(&in->motion);
    NVSDK_NGX_Resource_VK output = makeimg(&in->output);

    NVSDK_NGX_VK_DLSS_Eval_Params eval;
    memset(&eval, 0, sizeof(eval));
    eval.Feature.pInColor = &color;
    eval.Feature.pInOutput = &output;
    eval.pInDepth = &depth;
    eval.pInMotionVectors = &motion;
    eval.InJitterOffsetX = in->jitter_x;
    eval.InJitterOffsetY = in->jitter_y;
    eval.InReset = in->reset ? 1 : 0;
    /* Helpers treat 0 as 1. Packed MVs are already in pixels, so pass 1 explicitly.
     * Do not copy Streamline mvecScale (1/w, 1/h): that API wants [-1,1], NGX wants pixels. */
    eval.InMVScaleX = (in->mv_scale_x != 0.0f) ? in->mv_scale_x : 1.0f;
    eval.InMVScaleY = (in->mv_scale_y != 0.0f) ? in->mv_scale_y : 1.0f;
    eval.InRenderSubrectDimensions.Width = in->render_w ? in->render_w : in->color.width;
    eval.InRenderSubrectDimensions.Height = in->render_h ? in->render_h : in->color.height;

    /* Public SDK: pInTransparencyMask is unused/reserved. pInIsParticleMask is
     * documented as research-only. Only BiasCurrentColorMask is bound, and only
     * when the game actually filled the optional image. */
    NVSDK_NGX_Resource_VK biasres;
    memset(&biasres, 0, sizeof(biasres));
    if(in->bias.image_view && in->bias.image)
    {
        biasres = makeimg(&in->bias);
        eval.pInBiasCurrentColorMask = &biasres;
    }

    /* Helper stores 0 as 1. The colour buffer is not pre-multiplied, so the
     * value passed here is 1. ExposureScale stays 1: it is not the artistic
     * exposure. The 1x1 texture is the normalisation DLSS inverts on output. */
    float pre = in->pre_exposure == 0.0f ? 1.0f : in->pre_exposure;
    float scale = in->exposure_scale == 0.0f ? 1.0f : in->exposure_scale;
    eval.InPreExposure = pre;
    eval.InExposureScale = scale;
    NVSDK_NGX_Resource_VK exposureres;
    memset(&exposureres, 0, sizeof(exposureres));
    if(slot->hdr)
    {
        exposureres = makeimg(&in->exposure);
        eval.pInExposureTexture = &exposureres;
    }

    NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSS_EXT(
        (VkCommandBuffer)(uintptr_t)in->command_buffer,
        slot->handle,
        g_params,
        &eval);
    if(NVSDK_NGX_FAILED(r))
    {
        char msg[192];
        sprintf(msg, "NGX_VULKAN_EVALUATE_DLSS_EXT %s", ngxstr(r));
        seterr(SAUER_NGX_ERR_NGX, (uint32_t)r, msg);
        return SAUER_NGX_ERR_NGX;
    }
    char okmsg[240];
    sprintf(okmsg, "NGX_VULKAN_EVALUATE_DLSS_EXT ok IsHDR=%u AutoExposure=%u flags=0x%x pre_exposure=%.4f exposure_scale=%.4f exposure=%ux%u fmt=0x%x",
            (unsigned)slot->hdr, (unsigned)slot->auto_exposure, (unsigned)slot->flags,
            pre, scale,
            slot->hdr ? (unsigned)in->exposure.width : 0u,
            slot->hdr ? (unsigned)in->exposure.height : 0u,
            slot->hdr ? (unsigned)in->exposure.format : 0u);
    seterr(SAUER_NGX_OK, (uint32_t)r, okmsg);
    return SAUER_NGX_OK;
}

int32_t __cdecl sauer_ngx_get_stats(uint64_t handle, SauerNgxStats *out, uint32_t bytes)
{
    if(!out || !checksize(bytes, sizeof(*out), "SauerNgxStats") || !checksize(out->struct_bytes, sizeof(*out), "SauerNgxStats.struct_bytes"))
        return g_err.code ? g_err.code : SAUER_NGX_ERR_SIZE;
    FeatureSlot *slot = (FeatureSlot *)(uintptr_t)handle;
    (void)slot;
    out->vram_bytes = 0;
    out->ngx_result = 0;
    NVSDK_NGX_Parameter *p = g_caps ? g_caps : g_params;
    if(!p)
    {
        seterr(SAUER_NGX_ERR_STATE, 0, "get_stats before init");
        return SAUER_NGX_ERR_STATE;
    }
    unsigned long long vram = 0;
    NVSDK_NGX_Result r = NGX_DLSS_GET_STATS(p, &vram);
    out->ngx_result = (uint32_t)r;
    if(NVSDK_NGX_FAILED(r))
    {
        char msg[192];
        sprintf(msg, "NGX_DLSS_GET_STATS %s", ngxstr(r));
        seterr(SAUER_NGX_ERR_NGX, (uint32_t)r, msg);
        return SAUER_NGX_ERR_NGX;
    }
    out->vram_bytes = (uint64_t)vram;
    seterr(SAUER_NGX_OK, (uint32_t)r, "stats ok");
    return SAUER_NGX_OK;
}

int32_t __cdecl sauer_ngx_release(uint64_t handle)
{
    FeatureSlot *slot = (FeatureSlot *)(uintptr_t)handle;
    if(!slot)
    {
        if(g_feature)
        {
            slot = g_feature;
        }
        else
        {
            seterr(SAUER_NGX_OK, 0, "release: nothing to free");
            return SAUER_NGX_OK;
        }
    }
    if(slot->magic != kFeatureMagic)
    {
        seterr(SAUER_NGX_ERR_ARGS, 0, "release: bad handle");
        return SAUER_NGX_ERR_ARGS;
    }
    if(slot->handle)
    {
        NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_ReleaseFeature(slot->handle);
        slot->handle = NULL;
        if(NVSDK_NGX_FAILED(r))
        {
            char msg[192];
            sprintf(msg, "ReleaseFeature %s", ngxstr(r));
            seterr(SAUER_NGX_ERR_NGX, (uint32_t)r, msg);
            slot->magic = 0;
            if(g_feature == slot) g_feature = NULL;
            HeapFree(GetProcessHeap(), 0, slot);
            return SAUER_NGX_ERR_NGX;
        }
    }
    slot->magic = 0;
    if(g_feature == slot) g_feature = NULL;
    HeapFree(GetProcessHeap(), 0, slot);
    seterr(SAUER_NGX_OK, 0, "feature released");
    return SAUER_NGX_OK;
}

int32_t __cdecl sauer_ngx_shutdown(void)
{
    if(g_feature)
    {
        sauer_ngx_release((uint64_t)(uintptr_t)g_feature);
        g_feature = NULL;
    }
    destroyparams();
    if(g_inited)
    {
        NVSDK_NGX_VULKAN_Shutdown1(g_device);
        g_inited = false;
    }
    g_device = VK_NULL_HANDLE;
    seterr(SAUER_NGX_OK, 0, "shutdown");
    return SAUER_NGX_OK;
}

int32_t __cdecl sauer_ngx_last_error(SauerNgxError *out, uint32_t bytes)
{
    if(!out || bytes < sizeof(*out)) return SAUER_NGX_ERR_SIZE;
    if(out->struct_bytes && out->struct_bytes != sizeof(*out) && bytes != sizeof(*out))
        return SAUER_NGX_ERR_SIZE;
    *out = g_err;
    out->struct_bytes = sizeof(*out);
    return SAUER_NGX_OK;
}
