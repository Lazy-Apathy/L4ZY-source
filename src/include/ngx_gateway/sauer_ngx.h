/* sauer_ngx.h — versioned C ABI between the MinGW game and the MSVC NGX gateway DLL.
 *
 * NGX C++ objects, parameter maps and the official nvsdk_ngx_* helpers live only
 * inside the DLL. The game talks through these structs and GetProcAddress.
 * No STL, no C++ exceptions, no allocations that the other module must free.
 */
#ifndef SAUER_NGX_H
#define SAUER_NGX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SAUER_NGX_ABI_VERSION 5u
#define SAUER_NGX_MAX_EXTS 24u
#define SAUER_NGX_EXT_NAME 128u
#define SAUER_NGX_PATH 260u
#define SAUER_NGX_MSG 512u

enum
{
    SAUER_NGX_OK = 0,
    SAUER_NGX_ERR_VERSION = 1,
    SAUER_NGX_ERR_SIZE = 2,
    SAUER_NGX_ERR_STATE = 3,
    SAUER_NGX_ERR_NGX = 4,
    SAUER_NGX_ERR_UNSUPPORTED = 5,
    SAUER_NGX_ERR_ARGS = 6,
    SAUER_NGX_ERR_IO = 7
};

/* Super Resolution mode for create_feature / query_optimal.
 * These values are the gateway enum, not engine hwrtngxmode
 * (engine: 0 Off, 1 DLAA, 2 Quality, 3 Balanced, 4 Performance).
 * ABI 1 only had native 1:1 DLAA via sauer_ngx_create_dlaa.
 * ABI 2/3: DLAA + Quality. ABI 4 adds Balanced + Performance.
 * ABI 5 adds the HDR colour/exposure contract. It is not binary-compatible
 * with ABI 4: struct sizes and sauer_ngx_abi_version both change. */
enum
{
    SAUER_NGX_MODE_DLAA = 0,
    SAUER_NGX_MODE_QUALITY = 1,
    SAUER_NGX_MODE_BALANCED = 2,
    SAUER_NGX_MODE_PERFORMANCE = 3
};

/* 0 = SuperSampling/DLAA is supported (mirrors NVSDK_NGX_FeatureRequirement). */
typedef struct SauerNgxExtList
{
    uint32_t struct_bytes;
    uint32_t count;
    char names[SAUER_NGX_MAX_EXTS][SAUER_NGX_EXT_NAME];
} SauerNgxExtList;

typedef struct SauerNgxPaths
{
    uint32_t struct_bytes;
    char dll_dir[SAUER_NGX_PATH]; /* folder that contains nvngx_dlss.dll */
    char log_dir[SAUER_NGX_PATH]; /* writable NGX log directory */
} SauerNgxPaths;

typedef struct SauerNgxCaps
{
    uint32_t struct_bytes;
    int32_t feature_supported;           /* 0 = supported */
    uint32_t min_hw_arch;
    char min_os[255];
    uint32_t super_sampling_available;   /* valid after init */
    uint32_t ngx_result;                 /* NVSDK_NGX_Result bits, 0 if unused */
} SauerNgxCaps;

typedef struct SauerNgxInit
{
    uint32_t struct_bytes;
    uint64_t instance;          /* VkInstance */
    uint64_t physical_device;   /* VkPhysicalDevice */
    uint64_t device;            /* VkDevice */
    uint64_t gipa;              /* PFN_vkGetInstanceProcAddr */
    uint64_t gdpa;              /* PFN_vkGetDeviceProcAddr */
} SauerNgxInit;

typedef struct SauerNgxCreate
{
    uint32_t struct_bytes;
    uint64_t device;            /* VkDevice */
    uint64_t command_buffer;    /* recording VkCommandBuffer */
    uint32_t width;
    uint32_t height;
    uint32_t preset_k;          /* 1 = documented transformer preset for this mode */
    uint32_t auto_exposure;     /* 1 = AutoExposure create flag */
} SauerNgxCreate;

/* ABI 2: recommended input size for a display size + mode.
 * in: struct_bytes, out_w, out_h, mode. out: opt/min/max and callback_ok. */
typedef struct SauerNgxOptimal
{
    uint32_t struct_bytes;
    uint32_t out_w;
    uint32_t out_h;
    uint32_t mode;              /* SAUER_NGX_MODE_* */
    uint32_t opt_w;
    uint32_t opt_h;
    uint32_t max_w;
    uint32_t max_h;
    uint32_t min_w;
    uint32_t min_h;
    float sharpness;
    uint32_t ngx_result;
    uint32_t callback_ok;       /* 1 = NGX_DLSS_GET_OPTIMAL_SETTINGS succeeded */
} SauerNgxOptimal;

typedef struct SauerNgxCreateFeature
{
    uint32_t struct_bytes;
    uint64_t device;            /* VkDevice */
    uint64_t command_buffer;    /* recording VkCommandBuffer */
    uint32_t out_w;             /* display / NGX output */
    uint32_t out_h;
    uint32_t in_w;              /* 0 = use GET_OPTIMAL_SETTINGS for mode */
    uint32_t in_h;
    uint32_t mode;              /* SAUER_NGX_MODE_* */
    uint32_t preset_k;          /* 1 = documented SR preset: K for DLAA/Quality/Balanced, M for Performance */
    uint32_t auto_exposure;
    /* ABI 5. 1 = NVSDK_NGX_DLSS_Feature_Flags_IsHDR. Legal only when colour
     * in and out are VK_FORMAT_R16G16B16A16_SFLOAT. With preset K (DLAA,
     * Quality, Balanced) HDR refuses AutoExposure: normalisation is the
     * exposure texture. With preset M (Performance) the gateway sets
     * AutoExposure itself (DLSS guide 3.9: exposure texture only for J/K);
     * the caller sees it in SauerNgxCreateInfo.auto_exposure. */
    uint32_t hdr;
} SauerNgxCreateFeature;

typedef struct SauerNgxCreateInfo
{
    uint32_t struct_bytes;
    uint32_t mode_requested;
    uint32_t mode_applied;
    uint32_t in_w;
    uint32_t in_h;
    uint32_t out_w;
    uint32_t out_h;
    uint32_t optimal_w;
    uint32_t optimal_h;
    uint32_t used_optimal;      /* 1 = InWidth/Height came from GET_OPTIMAL_SETTINGS */
    uint32_t ngx_result;
    /* ABI 5. flags is the NVSDK_NGX_DLSS_Feature_Flags value actually passed. */
    uint32_t flags;
    uint32_t hdr;
    uint32_t auto_exposure;
} SauerNgxCreateInfo;

typedef struct SauerNgxVkImage
{
    uint64_t image;             /* VkImage */
    uint64_t image_view;        /* VkImageView */
    uint32_t format;            /* VkFormat */
    uint32_t width;
    uint32_t height;
    uint32_t aspect_mask;       /* VkImageAspectFlags, 0 = COLOR */
    int32_t read_write;         /* 1 = storage/UAV */
} SauerNgxVkImage;

typedef struct SauerNgxEval
{
    uint32_t struct_bytes;
    uint64_t command_buffer;    /* recording VkCommandBuffer */
    SauerNgxVkImage color;
    SauerNgxVkImage depth;
    SauerNgxVkImage motion;
    SauerNgxVkImage output;
    float jitter_x;             /* input-pixel space, D3D +Y down */
    float jitter_y;
    float mv_scale_x;           /* scale packed MVs into pixel space; 1 if already pixels */
    float mv_scale_y;
    int32_t reset;
    uint32_t render_w;
    uint32_t render_h;
    /* ABI 3+ field, ignored since the DLSS guide 310.4 (3.15): BiasCurrentColor
     * must not be used with the current presets (K, M). Leave it zero. */
    SauerNgxVkImage bias;
    /* ABI 5 HDR contract. Ignored unless hdr matches the created feature.
     * pre_exposure is the multiplier already baked into the colour buffer.
     * 0 is stored as 1 (the colour is not pre-exposed). exposure_scale is an
     * extra NGX scale, 0 stored as 1; it is not a second artistic exposure.
     * exposure is a 1x1 texture whose R channel is the normalisation scale
     * DLSS applies and then inverts. image_view 0 is unused. Required by an
     * HDR feature with preset K; ignored when the feature uses AutoExposure
     * (preset M, Performance). */
    float pre_exposure;
    float exposure_scale;
    SauerNgxVkImage exposure;
    uint32_t hdr;
} SauerNgxEval;

typedef struct SauerNgxStats
{
    uint32_t struct_bytes;
    uint64_t vram_bytes;
    uint32_t ngx_result;
} SauerNgxStats;

typedef struct SauerNgxError
{
    uint32_t struct_bytes;
    int32_t code;               /* SAUER_NGX_ERR_* */
    uint32_t ngx_result;        /* NVSDK_NGX_Result or 0 */
    char message[SAUER_NGX_MSG];
} SauerNgxError;

typedef void (__cdecl *SauerNgxLogFn)(const char *message);

typedef uint32_t (__cdecl *PFN_sauer_ngx_abi_version)(void);
typedef int32_t  (__cdecl *PFN_sauer_ngx_probe_link)(char *buf, uint32_t bufbytes);
typedef int32_t  (__cdecl *PFN_sauer_ngx_set_paths)(const SauerNgxPaths *in, uint32_t bytes);
typedef int32_t  (__cdecl *PFN_sauer_ngx_set_log)(SauerNgxLogFn fn);
typedef int32_t  (__cdecl *PFN_sauer_ngx_query_instance_exts)(SauerNgxExtList *out, uint32_t bytes);
typedef int32_t  (__cdecl *PFN_sauer_ngx_query_device_exts)(uint64_t instance, uint64_t physical_device, SauerNgxExtList *out, uint32_t bytes);
typedef int32_t  (__cdecl *PFN_sauer_ngx_query_caps)(uint64_t instance, uint64_t physical_device, SauerNgxCaps *out, uint32_t bytes);
typedef int32_t  (__cdecl *PFN_sauer_ngx_init)(const SauerNgxInit *in, uint32_t bytes);
typedef int32_t  (__cdecl *PFN_sauer_ngx_create_dlaa)(const SauerNgxCreate *in, uint32_t bytes, uint64_t *out_handle);
typedef int32_t  (__cdecl *PFN_sauer_ngx_query_optimal)(SauerNgxOptimal *io, uint32_t bytes);
typedef int32_t  (__cdecl *PFN_sauer_ngx_create_feature)(const SauerNgxCreateFeature *in, uint32_t bytes, uint64_t *out_handle, SauerNgxCreateInfo *out_info, uint32_t infobytes);
typedef int32_t  (__cdecl *PFN_sauer_ngx_evaluate)(uint64_t handle, const SauerNgxEval *in, uint32_t bytes);
typedef int32_t  (__cdecl *PFN_sauer_ngx_get_stats)(uint64_t handle, SauerNgxStats *out, uint32_t bytes);
typedef int32_t  (__cdecl *PFN_sauer_ngx_release)(uint64_t handle);
typedef int32_t  (__cdecl *PFN_sauer_ngx_shutdown)(void);
typedef int32_t  (__cdecl *PFN_sauer_ngx_last_error)(SauerNgxError *out, uint32_t bytes);

#ifdef SAUER_NGX_BUILD
#ifdef _WIN32
#define SAUER_NGX_EXPORT extern "C" __declspec(dllexport)
#else
#define SAUER_NGX_EXPORT extern "C"
#endif
SAUER_NGX_EXPORT uint32_t __cdecl sauer_ngx_abi_version(void);
SAUER_NGX_EXPORT int32_t  __cdecl sauer_ngx_probe_link(char *buf, uint32_t bufbytes);
SAUER_NGX_EXPORT int32_t  __cdecl sauer_ngx_set_paths(const SauerNgxPaths *in, uint32_t bytes);
SAUER_NGX_EXPORT int32_t  __cdecl sauer_ngx_set_log(SauerNgxLogFn fn);
SAUER_NGX_EXPORT int32_t  __cdecl sauer_ngx_query_instance_exts(SauerNgxExtList *out, uint32_t bytes);
SAUER_NGX_EXPORT int32_t  __cdecl sauer_ngx_query_device_exts(uint64_t instance, uint64_t physical_device, SauerNgxExtList *out, uint32_t bytes);
SAUER_NGX_EXPORT int32_t  __cdecl sauer_ngx_query_caps(uint64_t instance, uint64_t physical_device, SauerNgxCaps *out, uint32_t bytes);
SAUER_NGX_EXPORT int32_t  __cdecl sauer_ngx_init(const SauerNgxInit *in, uint32_t bytes);
SAUER_NGX_EXPORT int32_t  __cdecl sauer_ngx_create_dlaa(const SauerNgxCreate *in, uint32_t bytes, uint64_t *out_handle);
SAUER_NGX_EXPORT int32_t  __cdecl sauer_ngx_query_optimal(SauerNgxOptimal *io, uint32_t bytes);
SAUER_NGX_EXPORT int32_t  __cdecl sauer_ngx_create_feature(const SauerNgxCreateFeature *in, uint32_t bytes, uint64_t *out_handle, SauerNgxCreateInfo *out_info, uint32_t infobytes);
SAUER_NGX_EXPORT int32_t  __cdecl sauer_ngx_evaluate(uint64_t handle, const SauerNgxEval *in, uint32_t bytes);
SAUER_NGX_EXPORT int32_t  __cdecl sauer_ngx_get_stats(uint64_t handle, SauerNgxStats *out, uint32_t bytes);
SAUER_NGX_EXPORT int32_t  __cdecl sauer_ngx_release(uint64_t handle);
SAUER_NGX_EXPORT int32_t  __cdecl sauer_ngx_shutdown(void);
SAUER_NGX_EXPORT int32_t  __cdecl sauer_ngx_last_error(SauerNgxError *out, uint32_t bytes);
#endif

#ifdef __cplusplus
}
#endif

#endif
