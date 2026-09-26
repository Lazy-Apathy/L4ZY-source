/* sauer_nrd.h — C ABI between the MinGW client and the MSVC NRD gateway DLL.
 *
 * NRD C++ objects, STL and Vulkan allocations live only inside the DLL.
 * The game never frees a pointer the DLL allocated.
 */
#ifndef SAUER_NRD_H
#define SAUER_NRD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SAUER_NRD_ABI_VERSION 2u
#define SAUER_NRD_MSG 512u
#define SAUER_NRD_OPT_ANTILAG_OFF 1u
#define SAUER_NRD_OPT_FAST_OFF    2u

enum
{
    SAUER_NRD_OK = 0,
    SAUER_NRD_ERR_VERSION = 1,
    SAUER_NRD_ERR_SIZE = 2,
    SAUER_NRD_ERR_STATE = 3,
    SAUER_NRD_ERR_NRD = 4,
    SAUER_NRD_ERR_VK = 5,
    SAUER_NRD_ERR_ARGS = 6,
    SAUER_NRD_ERR_UNSUPPORTED = 7
};

enum
{
    SAUER_NRD_ACCUM_CONTINUE = 0,
    SAUER_NRD_ACCUM_RESTART = 1,
    SAUER_NRD_ACCUM_CLEAR = 2
};

typedef struct SauerNrdInit
{
    uint32_t struct_bytes;
    uint64_t instance;          /* VkInstance */
    uint64_t physical_device;   /* VkPhysicalDevice */
    uint64_t device;            /* VkDevice */
    uint64_t gipa;              /* PFN_vkGetInstanceProcAddr */
    uint64_t gdpa;              /* PFN_vkGetDeviceProcAddr */
    uint32_t queue_family;
    uint32_t queued_frames;     /* 1..4, match engine in-flight */
    uint32_t max_w;
    uint32_t max_h;
} SauerNrdInit;

typedef struct SauerNrdImages
{
    uint32_t struct_bytes;
    uint64_t in_diff;           /* VkImage RGBA16F */
    uint64_t in_viewz;          /* VkImage R32F */
    uint64_t in_normal;         /* VkImage RGBA16F */
    uint64_t in_mv;             /* VkImage RGBA16F */
    uint64_t out_diff;          /* VkImage RGBA16F */
    uint32_t w;
    uint32_t h;
} SauerNrdImages;

typedef struct SauerNrdFrame
{
    uint32_t struct_bytes;
    uint64_t command_buffer;    /* recording VkCommandBuffer */
    float view_to_clip[16];
    float view_to_clip_prev[16];
    float world_to_view[16];
    float world_to_view_prev[16];
    float jitter[2];            /* [-0.5, 0.5] pixel, sampleUv = pixelUv + jitter/size internally if needed */
    float jitter_prev[2];
    float mv_scale[3];          /* pixelUvPrev = pixelUv + mv.xy * scale */
    float time_delta_ms;
    float denoising_range;
    uint32_t frame_index;
    uint32_t accum;             /* SAUER_NRD_ACCUM_* */
    uint32_t w;
    uint32_t h;
    uint32_t w_prev;
    uint32_t h_prev;
    uint32_t options; /* SAUER_NRD_OPT_* */
} SauerNrdFrame;

typedef struct SauerNrdStats
{
    uint32_t struct_bytes;
    uint32_t dispatches;
    uint32_t permanent_mb;
    uint32_t transient_mb;
    uint32_t pipelines;
    int32_t last_error;
    char message[SAUER_NRD_MSG];
} SauerNrdStats;

/* GPU probe of the resources NRD actually samples. ABI 2 structs above
 * stay unchanged; these are extra exports. Engine waits idle before read. */
#define SAUER_NRD_PROBE_MAX 8

typedef struct SauerNrdProbeIn
{
    uint32_t struct_bytes;
    uint32_t n;
    uint16_t x[SAUER_NRD_PROBE_MAX];
    uint16_t y[SAUER_NRD_PROBE_MAX];
} SauerNrdProbeIn;

typedef struct SauerNrdProbePx
{
    uint16_t x, y;
    uint16_t gather_ix, gather_iy; /* floor((origin+1)*rsInv*phys - 0.5) */
    float in_z;
    float prev_z_xy;
    float prev_z_g00, prev_z_g10, prev_z_g01, prev_z_g11;
    float prev_z_after;
    uint16_t prev_hist;
    uint16_t prev_hist_g;
    uint8_t data2;
    uint8_t data1;
    float data1_frames;
    float out_w;
    float mv_x, mv_y, mv_z;
} SauerNrdProbePx;

typedef struct SauerNrdProbeOut
{
    uint32_t struct_bytes;
    uint32_t ready;
    uint32_t pool_w, pool_h;
    uint32_t user_w, user_h;
    uint32_t rs_w, rs_h;
    uint32_t rect_w, rect_h;
    uint32_t n;
    SauerNrdProbePx px[SAUER_NRD_PROBE_MAX];
} SauerNrdProbeOut;

typedef uint32_t (*PFN_sauer_nrd_abi_version)(void);
typedef int32_t (*PFN_sauer_nrd_create)(const SauerNrdInit *init);
typedef int32_t (*PFN_sauer_nrd_set_images)(const SauerNrdImages *images);
typedef int32_t (*PFN_sauer_nrd_denoise)(const SauerNrdFrame *frame);
typedef void (*PFN_sauer_nrd_destroy)(void);
typedef int32_t (*PFN_sauer_nrd_stats)(SauerNrdStats *stats);
typedef int32_t (*PFN_sauer_nrd_probe_arm)(const SauerNrdProbeIn *in);
typedef int32_t (*PFN_sauer_nrd_probe_read)(SauerNrdProbeOut *out);

#ifdef __cplusplus
}
#endif

#endif
