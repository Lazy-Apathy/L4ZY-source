// hwrt.h: internal contract of the Vulkan hardware ray tracing layer.
//
// The layer runs *beside* the GL renderer instead of replacing it. Per frame GL
// hands the frame over on a binary semaphore, Vulkan writes into an image whose
// memory both APIs map, then GL waits on a second binary semaphore and
// composites the result before the HUD. Nothing here is allowed to stall the CPU
// on the GPU, and every failure path must fall back to vanilla GL.
//
// Only files under engine/hwrt/ include this header; the rest of the engine sees
// the entry points declared in engine.h.

#ifndef __HWRT_H__
#define __HWRT_H__

#define VK_NO_PROTOTYPES
#ifdef WIN32
  #define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>

// ---------------------------------------------------------------------------
// Vulkan entry points
//
// vulkan-1.dll / libvulkan.so is opened by hand at runtime so the client keeps
// linking and running on machines with no Vulkan at all. Every function below is
// a plain global, so the rest of the module reads like ordinary Vulkan code.
// ---------------------------------------------------------------------------

#define HWRT_VK_GLOBAL_FUNCS(f) \
    f(vkCreateInstance) \
    f(vkEnumerateInstanceExtensionProperties)

#define HWRT_VK_INSTANCE_FUNCS(f) \
    f(vkDestroyInstance) \
    f(vkEnumeratePhysicalDevices) \
    f(vkGetPhysicalDeviceProperties) \
    f(vkGetPhysicalDeviceProperties2) \
    f(vkGetPhysicalDeviceFeatures2) \
    f(vkGetPhysicalDeviceMemoryProperties) \
    f(vkGetPhysicalDeviceQueueFamilyProperties) \
    f(vkEnumerateDeviceExtensionProperties) \
    f(vkCreateDevice) \
    f(vkGetDeviceProcAddr)

#ifdef WIN32
  #define HWRT_VK_EXTERNAL_FUNCS(f) \
    f(vkGetMemoryWin32HandleKHR) \
    f(vkGetSemaphoreWin32HandleKHR)
#else
  #define HWRT_VK_EXTERNAL_FUNCS(f) \
    f(vkGetMemoryFdKHR) \
    f(vkGetSemaphoreFdKHR)
#endif

// Acceleration-structure and device-address entry points. Declared with the
// rest of the device table, but loaded only when hwrtdev.rayquery is true.
#define HWRT_VK_AS_FUNCS(f) \
    f(vkCreateAccelerationStructureKHR) \
    f(vkDestroyAccelerationStructureKHR) \
    f(vkGetAccelerationStructureBuildSizesKHR) \
    f(vkCmdBuildAccelerationStructuresKHR) \
    f(vkGetAccelerationStructureDeviceAddressKHR) \
    f(vkGetBufferDeviceAddress)

#define HWRT_VK_DEVICE_CORE_FUNCS(f) \
    f(vkDestroyDevice) \
    f(vkDeviceWaitIdle) \
    f(vkGetDeviceQueue) \
    f(vkQueueSubmit) \
    f(vkQueueWaitIdle) \
    f(vkCreateImage) \
    f(vkDestroyImage) \
    f(vkGetImageMemoryRequirements) \
    f(vkBindImageMemory) \
    f(vkCreateImageView) \
    f(vkDestroyImageView) \
    f(vkCreateBuffer) \
    f(vkDestroyBuffer) \
    f(vkGetBufferMemoryRequirements) \
    f(vkBindBufferMemory) \
    f(vkMapMemory) \
    f(vkUnmapMemory) \
    f(vkAllocateMemory) \
    f(vkFreeMemory) \
    f(vkCreateSemaphore) \
    f(vkDestroySemaphore) \
    f(vkCreateFence) \
    f(vkDestroyFence) \
    f(vkWaitForFences) \
    f(vkResetFences) \
    f(vkGetFenceStatus) \
    f(vkCreateQueryPool) \
    f(vkDestroyQueryPool) \
    f(vkCmdResetQueryPool) \
    f(vkCmdWriteTimestamp) \
    f(vkGetQueryPoolResults) \
    f(vkCreateCommandPool) \
    f(vkDestroyCommandPool) \
    f(vkAllocateCommandBuffers) \
    f(vkFreeCommandBuffers) \
    f(vkBeginCommandBuffer) \
    f(vkEndCommandBuffer) \
    f(vkResetCommandBuffer) \
    f(vkCmdPipelineBarrier) \
    f(vkCmdBindPipeline) \
    f(vkCmdBindDescriptorSets) \
    f(vkCmdPushConstants) \
    f(vkCmdDispatch) \
    f(vkCmdClearColorImage) \
    f(vkCmdCopyImage) \
    f(vkCmdBlitImage) \
    f(vkCreateShaderModule) \
    f(vkDestroyShaderModule) \
    f(vkCreateDescriptorSetLayout) \
    f(vkDestroyDescriptorSetLayout) \
    f(vkCreateDescriptorPool) \
    f(vkDestroyDescriptorPool) \
    f(vkAllocateDescriptorSets) \
    f(vkUpdateDescriptorSets) \
    f(vkCreatePipelineLayout) \
    f(vkDestroyPipelineLayout) \
    f(vkCreateComputePipelines) \
    f(vkDestroyPipeline) \
    HWRT_VK_EXTERNAL_FUNCS(f)

#define HWRT_VK_DEVICE_FUNCS(f) \
    HWRT_VK_DEVICE_CORE_FUNCS(f) \
    HWRT_VK_AS_FUNCS(f)

#define HWRT_VK_DECLARE(name) extern PFN_##name name;
extern PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
HWRT_VK_GLOBAL_FUNCS(HWRT_VK_DECLARE)
HWRT_VK_INSTANCE_FUNCS(HWRT_VK_DECLARE)
HWRT_VK_DEVICE_FUNCS(HWRT_VK_DECLARE)
#undef HWRT_VK_DECLARE

// ---------------------------------------------------------------------------
// Device state
// ---------------------------------------------------------------------------

// How far the CPU may run ahead of the Vulkan queue. Each in-flight frame owns a
// command buffer and a fence; the fence is only ever inspected when its slot is
// about to be reused, which under the GL/VK ping-pong is several frames stale.
enum { HWRT_FRAMES_IN_FLIGHT = 3 };

// Vulkan timestamp marks written into each in-flight command buffer. Harvested
// when that slot's fence is already signalled, so the CPU never waits on them.
// BEGIN is written at TOP_OF_PIPE, which the glready wait does not gate, so it
// lands when the command buffer is scheduled rather than when it starts working.
// GLWAIT is the same point written at COMPUTE_SHADER, the stage the wait does
// gate, so GLWAIT - BEGIN is the queue sitting on OpenGL and everything after it
// is real tracing. Measuring from BEGIN charged that wait to the dispatch.
enum {
    HWRT_TS_BEGIN = 0,
    HWRT_TS_GLWAIT,
    HWRT_TS_BLAS,
    HWRT_TS_TLAS,
    HWRT_TS_LIGHT,
    HWRT_TS_NRD,
    HWRT_TS_DISPATCH,
    HWRT_TS_COUNT
};

// GL_TIME_ELAPSED queries, same ring depth, also never waited on.
enum {
    HWRT_GLT_MASKW = 0,
    HWRT_GLT_MASKS,
    HWRT_GLT_DEPTH,
    HWRT_GLT_COMPOSITE,
    HWRT_GLT_HANDOFF,
    HWRT_GLT_COUNT
};

// Last completed samples in milliseconds. GL and VK are different queues; gptotal
// is the sum of the GL interop draws plus the Vulkan CB, which matches the
// serialized GL -> VK -> GL sandwich without counting the semaphore wait twice.
//
// gptotal only adds up the work each queue does. It says nothing about the gaps
// between them, and glready/vkdone are one binary pair for the whole engine, not
// one pair per frame in flight, so the two queues cannot overlap at all: the GL
// queue sits idle for the whole Vulkan round trip and vice versa. handoff spans
// glSignalSemaphoreEXT to the end of glWaitSemaphoreEXT on the GL timeline, so it
// is that round trip as GL experiences it, and handoff - vktotal is what the
// serialization costs on top of the tracing itself. frame is the wall-clock
// period between two hwrtrender() calls, so frame - gptotal - handoff is
// everything neither queue accounted for.
struct hwrttimings
{
    float glmaskworld, glmaskscene, gldepth, glcomposite, gltotal;
    float glhandoff;
    float vkglwait, vkblas, vktlas, vklight, vknrd, vkdispatch, vktotal;
    float gptotal;
    float frame;
    float cpuprep, cpusync, cputotal;
    // CPU time sitting between the GL signal and the GL wait. GL_TIME_ELAPSED
    // over that span counts GPU idle too, so anything slow here shows up as
    // handoff without being a synchronisation problem at all.
    float cpudispatch, cpulights;
};
extern hwrttimings hwrttime;
extern void hwrttally();
extern double hwrtnow();
extern void hwrtwritestamp(VkCommandBuffer cmd, int slot, int mark);
extern void hwrtbegingltime(int stage);
extern void hwrtendgltime(int stage);
extern void hwrtcleanupgltimes();
extern void hwrtdrawtimes(int conw, int conh);

// What the Vulkan side is asked to put in the shared image. The clear mode is a
// diagnostic that bypasses the shader entirely. Mode 4 is the phase 2
// silhouette view. Mode 5 is the phase 3 RTAO probe (same as rtaodebug 1).
// Mode 6 is phase 4 hit shading (diffuse * baked lightmap). Mode 7 is phase 5
// point lights + one shadow ray, plus sun and skylight (same as hwrtlight 1).
enum {
    HWRT_TRACE_OVERLAY = 1,
    HWRT_TRACE_FULL = 2,
    HWRT_TRACE_CLEAR = 3,
    HWRT_TRACE_SILHOUETTE = 4,
    HWRT_TRACE_RTAO = 5,
    HWRT_TRACE_SHADE = 6,
    HWRT_TRACE_LIGHT = 7
};

struct hwrtdevice
{
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice device;
    VkQueue queue;
    uint32_t queuefamily;
    uint32_t queuecount;       // queues created in this family
    uint32_t slcomputeqindex;  // unused by NGX (evaluate uses the graphics/compute queue we submit)
    VkPhysicalDeviceMemoryProperties memprops;

    char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    uint32_t apiversion;
    uint8_t uuid[VK_UUID_SIZE];

    // Ray tracing is detected and enabled in phase 0. Phase 2 uses it for the
    // world BLAS/TLAS; interop still works when this is false.
    bool rayquery;
    uint32_t scratchalign;
    float timestampperiod;  // nanoseconds per timestamp tick
    uint32_t timestampbits; // 0 = this queue cannot timestamp
    // samplerAnisotropy enabled on this device: the largest ratio a sampler
    // may ask for (limits.maxSamplerAnisotropy). 0 = not available.
    float maxaniso;

    bool ok() const { return device != VK_NULL_HANDLE; }
};

extern hwrtdevice hwrtdev;

// gluuid is the device GL is running on; on a hybrid machine picking any other
// physical device silently breaks every shared allocation, so it is required.
extern bool hwrtinitdevice(const uint8_t *gluuid);
extern void hwrtdestroydevice();
extern int hwrtfindmemtype(uint32_t typebits, VkMemoryPropertyFlags props);

// NGX Vulkan / DLAA (dlaa.cpp). Best-effort: failure never sets hwrtfailed.
extern bool hwrtdlaainitbeforeinstance();
extern int hwrtdlaacollectinstanceexts(const char **names, int maxnames);
extern int hwrtdlaacollectdeviceexts(const char **names, int maxnames);
extern uint32_t hwrtdlaaextraqueues();
extern void hwrtdlaamergefeatures12(void *v12); // VkPhysicalDeviceVulkan12Features *
extern void hwrtdlaamergefeatures13(void *v13); // VkPhysicalDeviceVulkan13Features *, no-op if unused
extern bool hwrtdlaawantsfeatures12();
extern bool hwrtdlaawantsfeatures13();
extern bool hwrtdlaaondevice();
extern void hwrtdlaashutdown();
extern void hwrtdlaacleanup();
extern void hwrtdlaaframe();
extern bool hwrtdlaaneedsdata();
extern bool hwrtdlaaactive();

// AMD FSR 3.1 upscaler (fsr.cpp, bin64\fsr\sauer_fsr.dll). Same best-effort
// rules as NGX; dlaa.cpp drives it through the shared images for modes 5-8.
struct hwrtfsrimage
{
    VkImage image;
    VkFormat format;
    int w, h;
};
struct hwrtfsrdispatchargs
{
    VkCommandBuffer cmd;
    hwrtfsrimage color, depth, motion, reactive, exposure, output; // reactive/exposure: image 0 = none
    float jitterx, jittery;  // render pixels, +Y down (the values NGX receives)
    int renderw, renderh;
    bool reset;
};
extern bool hwrtfsrloadgateway();
extern bool hwrtfsrgatewayloaded();
// Device features vkdevice.cpp enabled for FSR (same bits as SAUER_FSR_DEV_*).
enum { HWRT_FSR_DEV_FORMATLESS = 1u << 0, HWRT_FSR_DEV_FP16 = 1u << 1 };
extern void hwrtfsrondevice(uint32_t enabledfeatures);
extern void hwrtfsrshutdown();
extern bool hwrtfsravailable();
extern const char *hwrtfsrreason();
extern bool hwrtfsrsimulating(const char *mode);
extern bool hwrtfsrquerysize(int enginemode, int outw, int outh, int &inw, int &inh);
extern bool hwrtfsrcreate(int enginemode, int inw, int inh, int outw, int outh, bool hdr, char *err, int errlen);
extern bool hwrtfsrdispatch(const hwrtfsrdispatchargs &a, char *err, int errlen);
extern void hwrtfsrrelease();
extern bool hwrtfsrconfigured();
extern uint64_t hwrtfsrvram();

// ---------------------------------------------------------------------------
// GL <-> Vulkan interop
// ---------------------------------------------------------------------------

struct hwrtinterop
{
    int w, h;

    // Result image: one allocation, mapped by Vulkan as a storage image and by
    // GL as a plain 2D texture.
    VkImage image;
    VkDeviceMemory memory;
    VkDeviceSize memorysize;
    VkImageView view;
    GLuint glmemory;
    GLuint gltex;

    // Phase 3: GL window depth copied into a shared R32F. Same NT-handle rules
    // as the colour output. A true D24/D32 import is skipped; the extra colour
    // format is the path that already works on this driver.
    VkImage depthimage;
    VkDeviceMemory depthmemory;
    VkDeviceSize depthmemorysize;
    VkImageView depthview;
    GLuint gldepthmemory;
    GLuint gldepthtex;
    bool depthcopied; // this frame: include the depth tex in the GL signal
    bool skipcomposite; // this frame: shared image just recreated; keep GL raster

    // glready is signalled by GL and waited on by Vulkan, vkdone the other way
    // round. Both are binary: exactly one signal is outstanding per wait.
    VkSemaphore vkglready, vkvkdone;
    GLuint glglready, glvkdone;

#ifdef WIN32
    // Importing an NT handle does not transfer ownership, and NVIDIA's GL only
    // resolves the memory handle when storage is attached, so ours stay open
    // until teardown.
    HANDLE memhandle, depthmemhandle, glreadyhandle, vkdonehandle;
#endif

    bool ok() const { return gltex != 0; }
    bool depthok() const { return gldepthtex != 0 && depthview != VK_NULL_HANDLE; }
};

extern hwrtinterop hwrtio;

extern bool hwrtloadglinterop();
extern bool hwrtglgetdeviceuuid(uint8_t *uuid);
extern bool hwrtcreateshared(int w, int h);
extern void hwrtdestroyshared();
extern bool hwrtcopydepth();     // GL window depth -> shared R32F, before glready
// Set once per frame by hwrtrender(): the copy above ran and succeeded, so the
// lighting shader may compare its hits against what GL rasterised. hwrtio's own
// depthcopied is consumed by the semaphore signal and is gone by then.
extern bool hwrtdepthlive;
// Model mask: window depth snapshotted either side of the model passes so the
// composite can leave GL's own raster of players / pickups / mapmodels alone.
extern bool hwrtmasksmodels();
extern void hwrtsnapworlddepth();
extern void hwrtsnapscenedepth();
extern bool hwrtmaskready();
extern void hwrtcleanupmask();
extern void hwrtsignalgl();      // GL -> Vulkan handoff
extern void hwrtwaitgl();        // Vulkan -> GL handoff
extern void hwrtcomposite(bool replace, float alpha);
extern void hwrtcompositeao();
extern void hwrtprobeshared();

// Traced reflections and world spec (Options > Graphics, RT only). A second
// shared RGBA32F image, same size as the result: the lighting pass writes the
// water reflection seen at each pixel into it (hitlight.comp waterReflection),
// GL draws the particles of each traced plane into it, and the water shader
// samples it in screen space in place of its 2^reflectsize planar texture.
extern int hwrtreflections, hwrtspecular;
extern bool hwrtrefllive();       // image exists and the pass wrote it this frame
extern VkImage hwrtreflimage();
extern VkImageView hwrtreflview();
extern GLuint hwrtreflgltex();
extern int hwrtwaterplanecount;   // planes packed by hwrtupdatelights() this frame
extern void hwrtsetreflwritten(bool on);
enum { HWRT_MAX_WATERPLANES = 16, HWRT_MAX_WATERRECTS = 512 };
// min x, min y, max x, max y (with the margin), z, group, first rectangle,
// rectangle count: one row per plane GL will reflect this frame that the RT
// can take over; rects: min x, min y, max x, max y of its surfaces (water.cpp).
extern int hwrtgatherwaterplanes(float (*out)[8], int maxplanes, float (*rects)[4], int maxrects);
extern GLuint hwrtskyenvtex();
extern void hwrtwriteskyenvbinding(VkDescriptorSet set);

// ---------------------------------------------------------------------------
// Vulkan side of the frame
// ---------------------------------------------------------------------------

extern bool hwrtinittrace();
extern void hwrtdestroytrace();
extern bool hwrtdispatch(int mode);
extern void hwrtunbindshared();
extern void hwrtbindtlas();

// Number of times the frame ring had to block on a fence. Should stay at 0; any
// growth means the CPU is waiting on the GPU on the hot path.
extern int hwrtfencestalls;

// ---------------------------------------------------------------------------
// Static world BLAS / TLAS (phase 2)
// ---------------------------------------------------------------------------

extern bool hwrthasworld();
extern void hwrtrebuildworld();
extern void hwrtdestroyworld();
extern VkAccelerationStructureKHR hwrtgettlas();
extern VkDeviceAddress hwrtworldblasaddr();
extern int hwrtworldtris, hwrtworldverts;

// Packed 64-byte TLAS instance. Bitfields on VkAccelerationStructureInstanceKHR
// are not reliable across MinGW / MSVC, so the words are written by hand.
struct hwrtinstance
{
    float transform[12];
    uint32_t custommask;
    uint32_t sbtflags;
    uint64_t reference;
};
static_assert(sizeof(hwrtinstance) == 64, "TLAS instance stride");

// ---------------------------------------------------------------------------
// Mapmodels / dynents as extra TLAS instances (phase 6)
// ---------------------------------------------------------------------------
//
// One rest-pose BLAS per unique model, rebuilt on allchanged() / first use.
// Animated dynents get a per-pose BLAS (ALLOW_UPDATE, refit on the frame
// command buffer) keyed like the skeleton cache, not by dynent. customIndex 0
// is the world; anything else is a dynent / mapmodel. Failure of this path
// logs once and leaves the world-only TLAS in place.

// 4096 instances at 64 bytes is 256 KB per in-flight slot, 768 KB in all, and
// the TLAS build measured 0.12 ms for 1024 entries. The old 1024 was not a
// budget any card cared about: cmvalley alone posts more mapmodels than that,
// and gatherinstances filled the whole table with them before it ever reached
// the players, so that map traced `1023 mapmodels + 0 dynents`.
enum { HWRT_MAX_MODEL_BLAS = 64, HWRT_MAX_ANIM_BLAS = 32, HWRT_MAX_INSTANCES = 4096 };

// customIndex 0 is the world; every model instance carries a real index into
// the per-frame geometry table instead of the old flat 1. Rest-pose BLASes take
// the first HWRT_MAX_MODEL_BLAS slots, animated ones the rest, so an index is
// stable for as long as its BLAS is.
enum
{
    HWRT_GEOM_MODEL0 = 1,
    HWRT_GEOM_ANIM0 = HWRT_GEOM_MODEL0 + HWRT_MAX_MODEL_BLAS,
    HWRT_MAX_GEOMS = HWRT_GEOM_ANIM0 + HWRT_MAX_ANIM_BLAS
};
// Room for more rest-pose BLASes (64 used to fill up on triforts). The first 64 keep their customIndex
// (1..64), the extra slots go after the animated ones (97..), so every index the
// old layout produced is unchanged and the whole table still fits the 8 bits the
// smoke puffs leave for it (customIndex bits 8-15 carry their opacity).
enum
{
    HWRT_MODEL_BLAS_CAP = 128,
    HWRT_GEOM_EXTRA0 = HWRT_MAX_GEOMS,
    HWRT_MAX_GEOMS_CAP = HWRT_GEOM_EXTRA0 + (HWRT_MODEL_BLAS_CAP - HWRT_MAX_MODEL_BLAS)
};

// Skins are per-`skin` in animmodel and are not in the world diffuse array, so
// they get a second sampled 2D array. compactskins() may rewrite layer indices.
// The GPU-memory cap before falling back to 512 is hwrtskinbudget (MB), not a
// compile-time constant: it is a threshold, not a reservation.
enum { HWRT_MAX_SKINS = 192, HWRT_MAX_SKINDIM = 1024 };
extern int hwrtskinbudget;

// Full BLAS rebuild (not a refit) at most this often per animated slot, and at
// most this many MODE_BUILD commands per frame. Refits keep the original BVH
// topology; a round-robin rebuild resets it before it drifts too far.
enum { HWRT_ANIM_REBUILD_EVERY = 24, HWRT_ANIM_REBUILDS_PER_FRAME = 2 };

// Instance ray masks, mirrored by the RAYMASK_ constants in hitlight.comp.
// Kept separate so a primary ray can ask for the world alone while shadow and
// sky rays still see every model.
enum { HWRT_RAYMASK_WORLD = 0x01, HWRT_RAYMASK_MODEL = 0x02, HWRT_RAYMASK_SELF = 0x04, HWRT_RAYMASK_CASTER = 0x08, HWRT_RAYMASK_PORTAL = 0x10, HWRT_RAYMASK_ALL = 0xFF };

// Extra bits in the lighting push-constant `mode` int (the struct is 128 bytes).
// They can be set together: SHADE_MODELS wins for the primary cull mask,
// MASK_MODELS then tags shaded model hits at alpha 0.5 so the composite
// can keep them while still dropping world hits behind an untraced mesh.
// GLDEPTH says this frame's window depth reached the shared R32F, so the
// shader can drop any hit GL rasterised something in front of.
enum { HWRT_MODE_MASK_MODELS = 0x100, HWRT_MODE_SHADE_MODELS = 0x200, HWRT_MODE_GLDEPTH = 0x400, HWRT_MODE_HDR = 0x800, HWRT_MODE_HDRPROBE = 0x1000 };
// Traced reflections: water planes traced into the reflection image, envmapped
// world faces traced instead of left matte (hwrtreflections); SPEC gives the
// world's spec shaders their highlights back (hwrtspecular). Bits 16-20 carry
// the water plane count.
enum { HWRT_MODE_REFL_WATER = 0x2000, HWRT_MODE_REFL_WORLD = 0x4000, HWRT_MODE_SPEC = 0x8000, HWRT_MODE_REFL_PLANESHIFT = 16 };

extern int hwrthdrprobe;
extern int hwrthdrpushmode;
extern bool hwrthdractive();
// Copies the shared RT colour image through the same sampler the composite
// uses, into a linear RGBA16F texture, then reads it back. Optimal-tiled
// imported memory often returns zeros from a direct glReadPixels.
extern void hwrtresultsize(int *w, int *h);
extern bool hwrtcopyresult(float *dst, int *w, int *h, int *glerr);
extern int hwrtresultglformat();

// One geometry table entry per model BLAS. The three device addresses are
// dereferenced through GL_EXT_buffer_reference in the shader, which is what
// keeps every BLAS owning its own attribute buffers instead of sharing an
// arena that would have to be re-laid-out whenever a pose changed size.
struct hwrtmodelgeom
{
    uint64_t verts;   // hwrtmodelvert[], indexed by the BLAS-local vertex index
    uint64_t indices; // uint[], 3 per triangle
    uint64_t tris;    // uint[], packed per triangle: skin 8 | mask 8 | glow u8.8
                      // | specscale 8 (modelshader maskscale.x = 0.5*mdlspec)
                      // (~0 = none). compactskins() remaps both layer fields.
    uint32_t ntris;
    uint32_t flags;   // HWRT_GEOM_SHADE when the three buffers are usable
                      // | HWRT_GEOM_FULLBRIGHT when GL rasters it MDL_FULLBRIGHT
                      // | (nopaque << HWRT_GEOM_NOPAQUE_SHIFT): triangles in
                      //   BLAS geometry 0 (opaque). Geometry 1 is MESH_ALPHA.
};
static_assert(sizeof(hwrtmodelgeom) == 32, "model geometry table stride");

// tc / normal / position per BLAS vertex. Same 48-byte stride as the world's
// shade vert. A zero normal means "no vertex normals", and the shader falls
// back to the triangle's geometric normal (BIH rest-pose geometry).
// pad0 = mdlenvmap min/max. pad1 = alphatest cutoff (0 = opaque mesh,
// typically 0.9 for MESH_ALPHA). Push constants are full, so it lives here.
struct hwrtmodelvert { float tc[2], pad0[2], n[3], pad1, pos[3], pad2; };
static_assert(sizeof(hwrtmodelvert) == 48, "model vert stride");

// FULLBRIGHT marks the instances GL draws with MDL_FULLBRIGHT (ENT_PLAYER), so
// the lighting shader can give them the menu's modelshader look: wrap volumes,
// Blinn-Phong spec, mdlenvmap cubemap (masks.b), a modest visibility floor, not
// GL's flattening 1.5. Crates, pickups and monsters do not get it, because GL
// does not give it to them either. NOPAQUE_SHIFT packs the opaque-triangle
// count into the leftover flag bits (markfullbright only ORs bit 1).
enum { HWRT_GEOM_SHADE = 1, HWRT_GEOM_FULLBRIGHT = 2, HWRT_GEOM_NOPAQUE_SHIFT = 2 };

extern bool hwrthasdynents();
extern void hwrtrebuilddynents();
extern void hwrtdestroydynents();
extern void hwrtsyncdynents();
extern bool hwrtpreparedynents(int slot);
extern void hwrtrecordtlas(VkCommandBuffer cmd, int slot);
extern VkAccelerationStructureKHR hwrtgetslottlas(int slot);
// Default "teleporter" aperture, in that model's BLAS space (scale and
// mdltrans already applied). lights.cpp puts the projected omni there.
extern vec hwrtteleporthole;
extern bool hwrthasteleporthole;
extern int hwrtmapmodelinsts, hwrtdynentinsts, hwrtragdollinsts, hwrtinstancecount;
// Mapmodels the instance budget could not take this frame. They are the
// farthest ones, so a map that overruns loses its horizon, not its foreground.
extern int hwrtdroppedinsts;
extern int hwrtmaxinsts;
extern int hwrtanimcount, hwrtanimbuilds, hwrtanimrefits;

// Model shading (slice 2): true once every model instance in the TLAS can be
// looked up and textured by the lighting shader. False leaves the old
// vec4(0) on model hits, and the model mask stays load-bearing.
extern bool hwrtmodelshadeready();
extern int hwrtskinlayers, hwrtskinw, hwrtskinh, hwrtgeomcount;
extern void hwrtdirtyskins();
extern void hwrtwritemodelbindings(VkDescriptorSet set, int slot);
extern VkImageView hwrtworldglowview();
extern VkSampler hwrtworldglowsampler();
// Game may override with a strong definition (players / monsters / movables).
extern const char *hwrtdynentmdlname(dynent *d);
// Game may override to match the spin / bob its renderentities() applies.
extern void hwrtentxform(const extentity &e, vec &o, float &yaw);
// First-person camera body to hide (the followed player while spectating).
// GL already skips followingplayer(); without this, a follow-cam primary
// ray starts inside that mesh. NULL from the weak fallback is fine.
extern dynent *hwrtcameradynent();
// Whatever GL draws has to be in the TLAS, and whatever GL hides has to stay
// out of it: an opaque composite paints over the models it cannot see, and
// shades the ones GL decided not to draw. The game owns those rules
// (rendergame's `hidedead`), so it answers both questions.
extern bool hwrtdynentvisible(dynent *d);
// The corpses GL keeps after a respawn. game::ragdolls is a second list that
// rendergame draws after the dynents and that numdynents() does not reach, so
// without these the composite paints every one of them over.
extern int hwrtnumragdolls();
extern dynent *hwrtragdoll(int i);
// Game lists every playermodel a client can wear. Called from map load so the
// RT copies and the skin atlas are built on the loading screen, not the first
// time that look walks into view. Weak fallback does nothing.
extern void hwrtpreloadplayermodels();
extern void hwrtpreloadmodel(const char *name);
extern void hwrtbeginscenemodels();
extern void hwrtendscenemodels();
extern void hwrtnotescenemodel(const char *mdl, const vec &o, float yaw, float pitch, int flags, dynent *d);
extern void hwrtnotesmokepuff(const vec &o, float radius, int opacity);

extern bool hwrthasshade();
extern int hwrtshadeepoch;
extern int hwrtshadelm, hwrtshadediff, hwrtshadeglow;
extern void hwrtwriteshadebindings(VkDescriptorSet set);
extern void hwrtwritelightgeombindings(VkDescriptorSet set);

// A sampled 2D array copied back from GL with glGetTexImage. The world's
// lightmaps and diffuse live in two of these; model skins in a third.
struct hwrttexarray
{
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    int w, h, layers, mips;
};

// upscale (world diffuse only, needs mips): 0 nearest (blocks), 1 bilinear,
// 2 bicubic Catmull-Rom, 3 Lanczos-3; 1-3 build every mip level from the
// original texture (as.cpp, hwrtdiffupscale).
extern bool hwrtuploadtexarray(VkCommandBuffer cmd, const vector<GLuint> &ids, int maxdim, hwrttexarray &out, const char *what, bool mips = false, int upscale = 0);
extern bool hwrtuploadcubemap(VkCommandBuffer cmd, GLuint gltex, hwrttexarray &out, const char *what);
// Same layers as hwrtuploadtexarray into an image with spare layers;
// first > 0 appends ids[first..] without touching the others (see as.cpp).
extern bool hwrtuploadtexlayers(const vector<GLuint> &ids, int first, int maxdim, int capacity, hwrttexarray &out, int &outcap, const char *what, bool mips);
extern void hwrtreaptexupload(bool wait);
extern void hwrtdestroytexupload();
extern bool hwrtuploadcubemapcap(VkCommandBuffer cmd, GLuint gltex, hwrttexarray &out, const char *what, int dimcap);
extern void hwrtdestroytexarray(hwrttexarray &t);
extern void hwrtnoteenvmap(GLuint gltex);
extern bool hwrtcreatesampler(bool repeat, VkSampler &out, bool mips = false);
extern void hwrtdestroysampler(VkSampler &s);
// Fallback for the model skin binding: a descriptor has to point somewhere
// valid even when no skin array exists, and the world diffuse always does.
extern VkImageView hwrtworlddiffuseview();
extern VkSampler hwrtworlddiffusesampler();

// ---------------------------------------------------------------------------
// Map point lights (phase 5) plus sun / sky (priority 6). Tiny SSBO with a
// 48-byte header, updated every frame. Failure of this path must not take
// down colour interop, silhouettes, RTAO, hit-shade, or dynents.
// ---------------------------------------------------------------------------

extern bool hwrthaslights();
extern int hwrtlightcount;
extern int hwrtunlimcount; // radius-0 ET_LIGHT, packed first, always evaluated
extern int hwrtglowlightcount; // teleport / jumppad / screen omnis, always evaluated
extern int hwrtsunon, hwrtskyon;
extern void hwrtupdatelights();
extern void hwrtdestroylights();
extern void hwrtnotelightsrebuild();
extern void hwrtwritelightbuffer(VkDescriptorSet set);

// Temporal skyvis. trace.cpp owns the two history images the lighting shader
// alternates between; lights.cpp packs the weight and the previous camera that
// let a hit find its own entry. Must be ensured before hwrtupdatelights().
extern bool hwrtensureskyhistory();
extern bool hwrtskyhistready();
extern void hwrtinvalidateskyhistory();
extern void hwrtskyhistsize(int &w, int &h);
// hitlight's sky-ray blue-noise tile (binding 25) is uploaded and bound.
extern bool hwrtskybluenoiseready();

// NRD REBLUR_DIFFUSE on the raw skyvis scalar. Failure leaves skyage running.
extern void hwrtnrdpreload();
extern bool hwrtnrdensure(int w, int h);
extern bool hwrtnrdsession();
// Sky rays actually traced: hwrtskyrays with NRD, at least 4 otherwise (lights.cpp).
extern int hwrtskyrayseffective();
extern bool hwrtnrdsetimages(VkImage diff, VkImage viewz, VkImage normal, VkImage mv, VkImage outdiff, int w, int h);
extern bool hwrtnrddenoise(VkCommandBuffer cmd, int w, int h, int reset);
extern void hwrtdestroynrd();
extern void hwrtnrdinvalidate();
extern void hwrtnrdcameramats(matrix4 &worldtoview, matrix4 &viewtoclip);
extern bool hwrtensurenrdguides();
extern bool hwrtnrdguidesready();
extern void hwrtdestroynrdguides();
extern int hwrtnrdguidemem();

// World glow clusters, rebuilt with the BLAS. lights.cpp uploads them as
// always-eval finite omnis so a jumppad floor or a screen actually lights
// the room instead of only painting itself.
enum { HWRT_MAX_GLOWOMNI = 96 };
struct hwrtglowomni
{
    float pos[3];
    float radius;
    float color[3];
    float flags; // 2 = aura (no shadow), 3 = always-eval with shadow
};
extern int hwrtglowomnicount;
extern hwrtglowomni hwrtglowomnis[HWRT_MAX_GLOWOMNI];

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

extern bool hwrtfailed;          // set once something went wrong; disables the layer
extern void hwrtfail(const char *what, VkResult r);
extern void hwrtfail(const char *what);
extern const char *hwrtresultstr(VkResult r);
extern bool hwrtwaitidle(const char *what);  // waits, names the real fault, disables the layer

// Why the RT option cannot be offered, for the menu and the assistant (hwrtraison).
// The first cause recorded during bring-up wins; a later hwrtfailed reads as FAILED.
enum
{
    HWRT_WHY_NONE = 0,
    HWRT_WHY_NOVULKAN,   // no Vulkan loader, no Vulkan driver or no Vulkan device
    HWRT_WHY_DRIVER,     // driver too old: no GL/Vulkan sharing, or Vulkan below 1.2
    HWRT_WHY_NOMATCH,    // Vulkan does not list the GPU OpenGL runs on
    HWRT_WHY_NORT,       // the GPU has no hardware ray tracing (no ray query)
    HWRT_WHY_FAILED,     // it should work but failed to start: see the console
    HWRT_WHY_TIMEOUT,    // the Vulkan driver did not answer in time (probe or start-up)
    HWRT_WHY_CRASH       // the Vulkan probe process crashed (driver or overlay layer)
};
extern void hwrtunavailable(int why);
// SAUER_HWRT_SIMULATE=novulkan|driver|nort|failed|hang|hanglate|crash, read once at
// bring-up, fakes an incompatible or stuck GPU through the real fallback path.
// Returns true if MODE is simulated.
extern bool hwrtsimulating(const char *mode);

// Deferred Vulkan start-up (vkdevice.cpp). States, in order:
enum
{
    HWRT_VK_IDLE = 0,    // nothing known yet, nothing started
    HWRT_VK_PROBING,     // the probe process (sauerbraten.exe -vkprobe) is running
    HWRT_VK_PROBED,      // probe answered; hwrtvkprobe holds what it saw, nothing created here
    HWRT_VK_STARTING,    // the start-up worker thread is creating the device
    HWRT_VK_READY,       // device, NGX, FSR and trace pipelines are up in this process
    HWRT_VK_FAILED,      // start-up failed (hwrtwhy says why); Vulkan stays off
    HWRT_VK_TIMEOUT      // probe or worker did not answer in time; Vulkan stays off
};
struct hwrtvkproberesult
{
    int why;             // HWRT_WHY_* the probe found (NONE = GL's GPU can share with Vulkan)
    int rtwhy;           // HWRT_WHY_* for ray tracing alone (NONE = it can trace)
    bool rayquery;       // that GPU can trace
    bool nvidia;         // that GPU is NVIDIA (DLAA/DLSS can be offered)
    char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
};
extern int hwrtvkstate;
extern hwrtvkproberesult hwrtvkprobe;
extern bool hwrtvkabandoned;        // a timed-out worker may still be inside the driver
extern bool hwrtvkready();          // HWRT_VK_READY and never abandoned
extern bool hwrtvkchecking();       // IDLE/PROBING: the menus say "checking"
extern bool hwrtvkcanstart();       // probe said yes (or still running) and nothing failed
extern bool hwrtvkensure(const char *why); // start now (bounded wait), true if READY
extern const char *hwrtvkstallreason(); // "" or why Vulkan was given up (timeout/crash)
extern int hwrtvktimeoutsecs();
extern void hwrtvksetuuid(const uint8_t *uuid);
extern void hwrtvkschedule(int delayms); // background probe in DELAYMS (nothing wanted at start)

#define HWRTCHECK(call, what) \
    do { \
        VkResult _hwrtres = (call); \
        if(_hwrtres != VK_SUCCESS) { hwrtfail(what, _hwrtres); return false; } \
    } while(0)

#endif
