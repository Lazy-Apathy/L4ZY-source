/* sauer_fsr.h — versioned C ABI between the MinGW game and the MSVC FSR gateway DLL.
 *
 * The gateway (bin64\fsr\sauer_fsr.dll) statically contains the AMD FidelityFX
 * SDK v1.1.4 FSR 3.1 upscaler and its Vulkan backend, nothing else: no frame
 * generation, no frame interpolation swap chain, no optical flow. FidelityFX
 * objects never leave the DLL. The game talks through these structs and
 * GetProcAddress. No STL, no exceptions, no allocation the other module frees.
 * Separate from the NGX gateway (bin64\ngx-hdr\sauer_ngx.dll); never mixed.
 */
#ifndef SAUER_FSR_H
#define SAUER_FSR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SAUER_FSR_ABI_VERSION 1u
#define SAUER_FSR_MSG 512u

enum
{
    SAUER_FSR_OK = 0,
    SAUER_FSR_ERR_VERSION = 1,
    SAUER_FSR_ERR_SIZE = 2,
    SAUER_FSR_ERR_STATE = 3,
    SAUER_FSR_ERR_FFX = 4,
    SAUER_FSR_ERR_UNSUPPORTED = 5,
    SAUER_FSR_ERR_ARGS = 6,
    SAUER_FSR_ERR_IO = 7
};

/* Gateway enum, not the engine hwrtngxmode values. NATIVE is 1:1 (the FSR
 * equivalent of DLAA); the others are the FSR 3.1 ratios 1.5 / 1.7 / 2.0. */
enum
{
    SAUER_FSR_MODE_NATIVE = 0,
    SAUER_FSR_MODE_QUALITY = 1,
    SAUER_FSR_MODE_BALANCED = 2,
    SAUER_FSR_MODE_PERFORMANCE = 3
};

/* Device features the game actually enabled on its VkDevice. FidelityFX
 * picks shader variants from what the physical device *supports*; the gateway
 * clamps that to what is enabled so no variant needs a feature that is off. */
enum
{
    SAUER_FSR_DEV_FORMATLESS_STORAGE = 1u << 0, /* shaderStorageImageRead/WriteWithoutFormat */
    SAUER_FSR_DEV_FP16 = 1u << 1                /* shaderFloat16 + shaderInt16 */
};

typedef void (*SauerFsrLogFn)(const char *message);

typedef struct SauerFsrInit
{
    uint32_t struct_bytes;
    uint64_t instance;          /* VkInstance */
    uint64_t physical_device;   /* VkPhysicalDevice */
    uint64_t device;            /* VkDevice */
    uint64_t gipa;              /* PFN_vkGetInstanceProcAddr */
    uint64_t gdpa;              /* PFN_vkGetDeviceProcAddr */
    uint32_t device_features;   /* SAUER_FSR_DEV_* actually enabled */
    uint32_t reserved;
} SauerFsrInit;

typedef struct SauerFsrInfo
{
    uint32_t struct_bytes;
    uint32_t version_major, version_minor, version_patch; /* FSR upscaler 3.1.x */
    uint32_t fp16;              /* 1 = half precision variants in use */
    uint32_t wave_min, wave_max;
    char sdk[64];               /* "FidelityFX SDK v1.1.4 c6efa6bf" */
} SauerFsrInfo;

/* in: out_w/out_h/mode. out: render size FSR recommends for that ratio. */
typedef struct SauerFsrOptimal
{
    uint32_t struct_bytes;
    uint32_t out_w, out_h, mode;
    uint32_t render_w, render_h;
    float ratio;
    int32_t jitter_phases;      /* ffxFsr3UpscalerGetJitterPhaseCount */
} SauerFsrOptimal;

typedef struct SauerFsrCreate
{
    uint32_t struct_bytes;
    uint32_t in_w, in_h;        /* render (max) size */
    uint32_t out_w, out_h;      /* display size */
    uint32_t mode;              /* SAUER_FSR_MODE_* */
    uint32_t hdr;               /* 1: scene-linear RGBA16F colour + exposure texture */
    uint32_t auto_exposure;     /* 1: FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE (SDR only) */
    uint32_t depth_inverted;    /* 0: 0 near .. 1 far */
    uint32_t debug_checking;    /* 1: FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING (lab) */
} SauerFsrCreate;

typedef struct SauerFsrCreateInfo
{
    uint32_t struct_bytes;
    uint32_t in_w, in_h, out_w, out_h, mode;
    uint32_t flags;             /* FfxFsr3UpscalerInitializationFlagBits used */
    uint32_t hdr, auto_exposure;
    int32_t ffx_result;
} SauerFsrCreateInfo;

/* A Vulkan image the game owns, in VK_IMAGE_LAYOUT_GENERAL before and after
 * the dispatch. The gateway makes its own views. */
typedef struct SauerFsrVkImage
{
    uint64_t image;             /* VkImage */
    uint32_t format;            /* VkFormat */
    uint32_t width, height;
} SauerFsrVkImage;

typedef struct SauerFsrDispatch
{
    uint32_t struct_bytes;
    uint64_t command_buffer;    /* recording VkCommandBuffer */
    SauerFsrVkImage color;      /* render size, row 0 = top */
    SauerFsrVkImage depth;      /* render size, R32F 0 near .. 1 far */
    SauerFsrVkImage motion;     /* render size, .xy = previous - current, pixels, +Y down */
    SauerFsrVkImage reactive;   /* optional (image 0): .r in [0,1] */
    SauerFsrVkImage exposure;   /* optional (image 0): 1x1 R32F, required when hdr */
    SauerFsrVkImage output;     /* display size, storage */
    float jitter_x, jitter_y;   /* render pixels, same values NGX receives */
    float mv_scale_x, mv_scale_y;
    uint32_t render_w, render_h;
    uint32_t reset;
    uint32_t sharpen;           /* RCAS on/off */
    float sharpness;            /* 0..1 */
    float frame_ms;             /* > 0 */
    float pre_exposure;         /* > 0 */
    float camera_near, camera_far, camera_fov_v; /* radians */
    float view_to_meters;
    float reactive_scale;       /* 0 = gateway default (1) */
} SauerFsrDispatch;

typedef struct SauerFsrStats
{
    uint32_t struct_bytes;
    uint64_t vram_bytes;
    int32_t ffx_result;
} SauerFsrStats;

typedef struct SauerFsrError
{
    uint32_t struct_bytes;
    int32_t code;
    int32_t ffx_result;
    char message[SAUER_FSR_MSG];
} SauerFsrError;

typedef uint32_t (__cdecl *PFN_sauer_fsr_abi_version)(void);
typedef int32_t (__cdecl *PFN_sauer_fsr_set_log)(SauerFsrLogFn fn);
typedef int32_t (__cdecl *PFN_sauer_fsr_init)(const SauerFsrInit *in, uint32_t bytes, SauerFsrInfo *out, uint32_t outbytes);
typedef int32_t (__cdecl *PFN_sauer_fsr_query_optimal)(SauerFsrOptimal *io, uint32_t bytes);
typedef int32_t (__cdecl *PFN_sauer_fsr_create)(const SauerFsrCreate *in, uint32_t bytes, uint64_t *out_handle, SauerFsrCreateInfo *out_info, uint32_t infobytes);
typedef int32_t (__cdecl *PFN_sauer_fsr_dispatch)(uint64_t handle, const SauerFsrDispatch *in, uint32_t bytes);
typedef int32_t (__cdecl *PFN_sauer_fsr_get_stats)(uint64_t handle, SauerFsrStats *out, uint32_t bytes);
typedef int32_t (__cdecl *PFN_sauer_fsr_release)(uint64_t handle);
typedef int32_t (__cdecl *PFN_sauer_fsr_shutdown)(void);
typedef int32_t (__cdecl *PFN_sauer_fsr_last_error)(SauerFsrError *out, uint32_t bytes);

#ifdef SAUER_FSR_BUILD
__declspec(dllexport) uint32_t __cdecl sauer_fsr_abi_version(void);
__declspec(dllexport) int32_t __cdecl sauer_fsr_set_log(SauerFsrLogFn fn);
__declspec(dllexport) int32_t __cdecl sauer_fsr_init(const SauerFsrInit *in, uint32_t bytes, SauerFsrInfo *out, uint32_t outbytes);
__declspec(dllexport) int32_t __cdecl sauer_fsr_query_optimal(SauerFsrOptimal *io, uint32_t bytes);
__declspec(dllexport) int32_t __cdecl sauer_fsr_create(const SauerFsrCreate *in, uint32_t bytes, uint64_t *out_handle, SauerFsrCreateInfo *out_info, uint32_t infobytes);
__declspec(dllexport) int32_t __cdecl sauer_fsr_dispatch(uint64_t handle, const SauerFsrDispatch *in, uint32_t bytes);
__declspec(dllexport) int32_t __cdecl sauer_fsr_get_stats(uint64_t handle, SauerFsrStats *out, uint32_t bytes);
__declspec(dllexport) int32_t __cdecl sauer_fsr_release(uint64_t handle);
__declspec(dllexport) int32_t __cdecl sauer_fsr_shutdown(void);
__declspec(dllexport) int32_t __cdecl sauer_fsr_last_error(SauerFsrError *out, uint32_t bytes);
#endif

#ifdef __cplusplus
}
#endif

#endif
