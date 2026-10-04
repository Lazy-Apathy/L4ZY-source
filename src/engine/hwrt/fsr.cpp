// fsr.cpp: AMD FSR 3.1 upscaler (FidelityFX SDK v1.1.4, Vulkan backend) next to
// NVIDIA DLAA/DLSS. Upscaling only: frame generation is not in the gateway and
// never will be.
//
// The gateway is its own MSVC DLL with a versioned C ABI, in its own folder:
// bin64\fsr\sauer_fsr.dll (ABI 1, include/fsr_gateway/sauer_fsr.h). It never
// shares a folder or an ABI with the NGX gateways (bin64\ngx-hdr ...).
//
// dlaa.cpp keeps the whole shared pipeline for both upscalers: internal scene
// FBO at the render size, packed colour/depth/motion/mask images shared with
// Vulkan, one submit per frame, blit or HDR compose, fallback to Native. This
// file only loads the gateway, says whether FSR can run (and why not), and turns
// those images into one FSR dispatch. Present remains SDL_GL_SwapWindow.
//
// Engine modes (hwrtngxmode): 5 FSR Native (1:1), 6 Quality (1.5x),
// 7 Balanced (1.7x), 8 Performance (2x).
//
// SAUER_UPSCALE_SIMULATE (diagnostic, read at start-up, like SAUER_HWRT_SIMULATE):
//   amd      DLAA/DLSS refused as on a non-NVIDIA card; FSR normal
//   nofsr    FSR refused at start-up (greyed in the menu)
//   fsrfail  FSR offered, but creating it fails: fallback to Native at first use
// The "no OpenGL/Vulkan sharing" case is SAUER_HWRT_SIMULATE=driver.

#include "engine.h"
#include "hwrt/hwrt.h"

#ifndef WIN32

bool hwrtfsrloadgateway() { return false; }
bool hwrtfsrgatewayloaded() { return false; }
void hwrtfsrondevice(uint32_t) {}
void hwrtfsrshutdown() {}
bool hwrtfsravailable() { return false; }
const char *hwrtfsrreason() { return "FSR is only built for Windows."; }
bool hwrtfsrsimulating(const char *) { return false; }
bool hwrtfsrquerysize(int, int, int, int &inw, int &inh) { inw = inh = 0; return false; }
bool hwrtfsrcreate(int, int, int, int, int, bool, char *err, int errlen) { copystring(err, "FSR is only built for Windows.", errlen); return false; }
bool hwrtfsrdispatch(const hwrtfsrdispatchargs &, char *err, int errlen) { copystring(err, "FSR is only built for Windows.", errlen); return false; }
void hwrtfsrrelease() {}
bool hwrtfsrconfigured() { return false; }
uint64_t hwrtfsrvram() { return 0; }
ICOMMAND(hwrtfsrraison, "", (), result(hwrtfsrreason()));
VARP(hwrtfsrsharpness, 0, 10, 100);

#else

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "fsr_gateway/sauer_fsr.h"

static_assert(unsigned(HWRT_FSR_DEV_FORMATLESS) == unsigned(SAUER_FSR_DEV_FORMATLESS_STORAGE) && unsigned(HWRT_FSR_DEV_FP16) == unsigned(SAUER_FSR_DEV_FP16),
              "hwrt.h and sauer_fsr.h device feature bits");

// Mode reasons for the menu and the assistant ("" = FSR can run).
enum
{
    FSR_WHY_NONE = 0,
    FSR_WHY_NODEVICE,   // no Vulkan device next to GL (no Vulkan, or no GL/Vulkan sharing)
    FSR_WHY_MISSING,    // bin64\fsr\sauer_fsr.dll missing or wrong ABI
    FSR_WHY_FEATURES,   // the driver lacks a Vulkan feature the FSR shaders need
    FSR_WHY_FAILED,     // it should work but failed to start: see the console
};

static HMODULE fsrmod = NULL;
static PFN_sauer_fsr_abi_version p_fsrVer = NULL;
static PFN_sauer_fsr_set_log p_fsrLog = NULL;
static PFN_sauer_fsr_init p_fsrInit = NULL;
static PFN_sauer_fsr_query_optimal p_fsrOptimal = NULL;
static PFN_sauer_fsr_create p_fsrCreate = NULL;
static PFN_sauer_fsr_dispatch p_fsrDispatch = NULL;
static PFN_sauer_fsr_get_stats p_fsrStats = NULL;
static PFN_sauer_fsr_release p_fsrRelease = NULL;
static PFN_sauer_fsr_shutdown p_fsrShutdown = NULL;
static PFN_sauer_fsr_last_error p_fsrErr = NULL;

static bool fsrloaded = false, fsrinited = false;
static int fsrwhy = FSR_WHY_NODEVICE;
static string fsrdetail = "";
static string fsrsimulate = "";
static bool fsrsimread = false;
static uint64_t fsrhandle = 0;
static bool fsrhandlenative = false;
static SauerFsrInfo fsrinfo;
static int fsrlastms = 0;

// RCAS sharpening after an FSR upscale (Quality/Balanced/Performance), 0 = off.
// Saved. Default 10: measured on triforts, academy and haste (1600x900), it brings
// FSR Quality to the detail of DLSS Quality at the same ratio without the isolated
// over-bright/over-dark pixels a stronger value adds. Never applied to FSR Native,
// which is already sharper than DLAA without it.
VARP(hwrtfsrsharpness, 0, 10, 100);

static void readsimulate()
{
    if(fsrsimread) return;
    fsrsimread = true;
    const char *sim = getenv("SAUER_UPSCALE_SIMULATE");
    copystring(fsrsimulate, sim ? sim : "");
    if(!fsrsimulate[0]) return;
    if(!strcmp(fsrsimulate, "amd") || !strcmp(fsrsimulate, "nofsr") || !strcmp(fsrsimulate, "fsrfail"))
        conoutf(CON_WARN, "upscale: SAUER_UPSCALE_SIMULATE=%s (diagnostic)", fsrsimulate);
    else
    {
        conoutf(CON_WARN, "upscale: SAUER_UPSCALE_SIMULATE=%s is not amd, nofsr or fsrfail; ignored", fsrsimulate);
        fsrsimulate[0] = 0;
    }
}

bool hwrtfsrsimulating(const char *mode)
{
    readsimulate();
    return fsrsimulate[0] && !strcmp(fsrsimulate, mode);
}
ICOMMAND(hwrtupscalesimulated, "", (), { readsimulate(); result(fsrsimulate); });

static void fsrlog(const char *message)
{
    if(message && message[0]) conoutf(CON_INIT, "fsr: %s", message);
}

static void fsrcopyerr(char *dst, int len)
{
    if(!dst || len <= 0) return;
    dst[0] = 0;
    if(!p_fsrErr) return;
    SauerFsrError e;
    memset(&e, 0, sizeof(e));
    e.struct_bytes = sizeof(e);
    p_fsrErr(&e, sizeof(e));
    copystring(dst, e.message, len);
}

static bool resolvefuns()
{
    p_fsrVer = (PFN_sauer_fsr_abi_version)GetProcAddress(fsrmod, "sauer_fsr_abi_version");
    p_fsrLog = (PFN_sauer_fsr_set_log)GetProcAddress(fsrmod, "sauer_fsr_set_log");
    p_fsrInit = (PFN_sauer_fsr_init)GetProcAddress(fsrmod, "sauer_fsr_init");
    p_fsrOptimal = (PFN_sauer_fsr_query_optimal)GetProcAddress(fsrmod, "sauer_fsr_query_optimal");
    p_fsrCreate = (PFN_sauer_fsr_create)GetProcAddress(fsrmod, "sauer_fsr_create");
    p_fsrDispatch = (PFN_sauer_fsr_dispatch)GetProcAddress(fsrmod, "sauer_fsr_dispatch");
    p_fsrStats = (PFN_sauer_fsr_get_stats)GetProcAddress(fsrmod, "sauer_fsr_get_stats");
    p_fsrRelease = (PFN_sauer_fsr_release)GetProcAddress(fsrmod, "sauer_fsr_release");
    p_fsrShutdown = (PFN_sauer_fsr_shutdown)GetProcAddress(fsrmod, "sauer_fsr_shutdown");
    p_fsrErr = (PFN_sauer_fsr_last_error)GetProcAddress(fsrmod, "sauer_fsr_last_error");
    return p_fsrVer && p_fsrLog && p_fsrInit && p_fsrOptimal && p_fsrCreate && p_fsrDispatch &&
           p_fsrStats && p_fsrRelease && p_fsrShutdown && p_fsrErr;
}

static void unloadgateway()
{
    if(fsrmod) FreeLibrary(fsrmod);
    fsrmod = NULL;
    p_fsrVer = NULL; p_fsrLog = NULL; p_fsrInit = NULL; p_fsrOptimal = NULL; p_fsrCreate = NULL;
    p_fsrDispatch = NULL; p_fsrStats = NULL; p_fsrRelease = NULL; p_fsrShutdown = NULL; p_fsrErr = NULL;
    fsrloaded = false;
}

// Before the Vulkan device: the device must enable the features the FSR shaders
// use, and only when the gateway is there. SAUER_FSR_DIR overrides the folder
// (laboratory only), exclusively like SAUER_NGX_DIR.
bool hwrtfsrloadgateway()
{
    readsimulate();
    if(fsrloaded) return true;
    fsrwhy = FSR_WHY_MISSING;
    string dll;
    const char *env = getenv("SAUER_FSR_DIR");
    if(env && env[0]) nformatstring(dll, sizeof(dll), "%s\\sauer_fsr.dll", env);
    else
    {
        char exe[MAX_PATH];
        DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
        if(!n || n >= MAX_PATH) return false;
        char *slash = strrchr(exe, '\\');
        if(!slash) slash = strrchr(exe, '/');
        if(slash) *slash = 0;
        nformatstring(dll, sizeof(dll), "%s\\fsr\\sauer_fsr.dll", exe);
    }
    DWORD attr = GetFileAttributesA(dll);
    if(attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY))
    {
        copystring(fsrdetail, "bin64\\fsr\\sauer_fsr.dll missing");
        conoutf(CON_INIT, "fsr: %s, FSR unavailable", fsrdetail);
        return false;
    }
    fsrmod = LoadLibraryA(dll);
    if(!fsrmod)
    {
        formatstring(fsrdetail, "LoadLibrary sauer_fsr.dll failed (%u)", (unsigned)GetLastError());
        conoutf(CON_WARN, "fsr: %s", fsrdetail);
        return false;
    }
    if(!resolvefuns())
    {
        copystring(fsrdetail, "sauer_fsr.dll is missing required C exports");
        conoutf(CON_WARN, "fsr: %s", fsrdetail);
        unloadgateway();
        return false;
    }
    if(p_fsrVer() != SAUER_FSR_ABI_VERSION)
    {
        formatstring(fsrdetail, "sauer_fsr ABI %u != header %u", (unsigned)p_fsrVer(), SAUER_FSR_ABI_VERSION);
        conoutf(CON_WARN, "fsr: %s", fsrdetail);
        unloadgateway();
        return false;
    }
    p_fsrLog(fsrlog);
    fsrloaded = true;
    fsrwhy = FSR_WHY_NODEVICE;
    copystring(fsrdetail, "gateway loaded, waiting for the Vulkan device");
    conoutf(CON_INIT, "fsr: AMD FSR 3.1 gateway loaded from %s (ABI %u)", dll, SAUER_FSR_ABI_VERSION);
    return true;
}

bool hwrtfsrgatewayloaded() { return fsrloaded; }

void hwrtfsrondevice(uint32_t enabledfeatures)
{
    if(!fsrloaded || !hwrtdev.device) return;
    fsrinited = false;
    if(hwrtfsrsimulating("nofsr"))
    {
        fsrwhy = FSR_WHY_FAILED;
        copystring(fsrdetail, "simulated start-up failure (SAUER_UPSCALE_SIMULATE=nofsr)");
        conoutf(CON_WARN, "fsr: %s", fsrdetail);
        return;
    }
    if(!(enabledfeatures & SAUER_FSR_DEV_FORMATLESS_STORAGE))
    {
        fsrwhy = FSR_WHY_FEATURES;
        copystring(fsrdetail, "the driver has no storage images without format (read and write)");
        conoutf(CON_WARN, "fsr: %s, FSR unavailable", fsrdetail);
        return;
    }
    SauerFsrInit in;
    memset(&in, 0, sizeof(in));
    in.struct_bytes = sizeof(in);
    in.instance = (uint64_t)(uintptr_t)hwrtdev.instance;
    in.physical_device = (uint64_t)(uintptr_t)hwrtdev.phys;
    in.device = (uint64_t)(uintptr_t)hwrtdev.device;
    in.gipa = (uint64_t)(uintptr_t)vkGetInstanceProcAddr;
    in.gdpa = (uint64_t)(uintptr_t)vkGetDeviceProcAddr;
    in.device_features = enabledfeatures;
    memset(&fsrinfo, 0, sizeof(fsrinfo));
    fsrinfo.struct_bytes = sizeof(fsrinfo);
    int32_t rc = p_fsrInit(&in, sizeof(in), &fsrinfo, sizeof(fsrinfo));
    fsrcopyerr(fsrdetail, sizeof(fsrdetail));
    if(rc != SAUER_FSR_OK)
    {
        fsrwhy = rc == SAUER_FSR_ERR_UNSUPPORTED ? FSR_WHY_FEATURES : FSR_WHY_FAILED;
        conoutf(CON_WARN, "fsr: init failed: %s", fsrdetail);
        return;
    }
    fsrinited = true;
    fsrwhy = FSR_WHY_NONE;
    conoutf(CON_INIT, "fsr: %s", fsrdetail);
}

void hwrtfsrshutdown()
{
    if(p_fsrShutdown) p_fsrShutdown();
    fsrhandle = 0;
    fsrinited = false;
    unloadgateway();
    fsrwhy = FSR_WHY_NODEVICE;
}

bool hwrtfsravailable()
{
    return hwrtvkready() && fsrloaded && fsrinited && hwrtdev.device && fsrwhy == FSR_WHY_NONE;
}

// Short English line for the menu and the assistant, "" when FSR can run.
const char *hwrtfsrreason()
{
    if(hwrtfsravailable()) return "";
    if(!hwrtvkready())
    {
        if(hwrtvkchecking()) return "FSR: checking the graphics card and driver...";
        if(hwrtvkstate == HWRT_VK_TIMEOUT)
        {
            static string msg;
            formatstring(msg, "FSR is not available: %s", hwrtvkstallreason());
            return msg;
        }
        // Probe answered, Vulkan not started in the game yet: offered, started when chosen.
        if(hwrtvkstate == HWRT_VK_PROBED && hwrtvkprobe.why == HWRT_WHY_NONE) return "";
    }
    int why = fsrwhy;
    if(!hwrtdev.device && why != FSR_WHY_MISSING) why = FSR_WHY_NODEVICE;
    switch(why)
    {
        case FSR_WHY_NODEVICE: return "FSR is not available: the graphics driver cannot share images between OpenGL and Vulkan.";
        case FSR_WHY_MISSING: return "FSR is not available: its files are missing (bin64\\fsr).";
        case FSR_WHY_FEATURES: return "FSR is not available: the graphics driver lacks a Vulkan feature FSR needs.";
        default: return "FSR is not available: it failed to start (details in log.txt).";
    }
}
ICOMMAND(hwrtfsrraison, "", (), result(hwrtfsrreason()));

static uint32_t gwmode(int enginemode)
{
    switch(enginemode)
    {
        case 6: return SAUER_FSR_MODE_QUALITY;
        case 7: return SAUER_FSR_MODE_BALANCED;
        case 8: return SAUER_FSR_MODE_PERFORMANCE;
        default: return SAUER_FSR_MODE_NATIVE;
    }
}

bool hwrtfsrquerysize(int enginemode, int outw, int outh, int &inw, int &inh)
{
    inw = outw;
    inh = outh;
    if(!hwrtfsravailable() || outw < 8 || outh < 8) return false;
    SauerFsrOptimal q;
    memset(&q, 0, sizeof(q));
    q.struct_bytes = sizeof(q);
    q.out_w = uint32_t(outw);
    q.out_h = uint32_t(outh);
    q.mode = gwmode(enginemode);
    if(p_fsrOptimal(&q, sizeof(q)) != SAUER_FSR_OK || q.render_w < 8 || q.render_h < 8) return false;
    inw = int(q.render_w);
    inh = int(q.render_h);
    return true;
}

bool hwrtfsrcreate(int enginemode, int inw, int inh, int outw, int outh, bool hdr, char *err, int errlen)
{
    if(err && errlen > 0) err[0] = 0;
    hwrtfsrrelease();
    if(!hwrtfsravailable())
    {
        copystring(err, hwrtfsrreason(), errlen);
        return false;
    }
    if(hwrtfsrsimulating("fsrfail"))
    {
        copystring(err, "FSR failed to start (simulated: SAUER_UPSCALE_SIMULATE=fsrfail)", errlen);
        return false;
    }
    SauerFsrCreate c;
    memset(&c, 0, sizeof(c));
    c.struct_bytes = sizeof(c);
    c.in_w = uint32_t(inw);
    c.in_h = uint32_t(inh);
    c.out_w = uint32_t(outw);
    c.out_h = uint32_t(outh);
    c.mode = gwmode(enginemode);
    c.hdr = hdr ? 1u : 0u;
    // SDR colour is already display-referred: FSR's default exposure of 1 is
    // the right one. HDR passes the 2^EV texture that hdrpresent also applies.
    c.auto_exposure = 0;
    c.depth_inverted = 0;
    SauerFsrCreateInfo info;
    memset(&info, 0, sizeof(info));
    info.struct_bytes = sizeof(info);
    uint64_t h = 0;
    int32_t rc = p_fsrCreate(&c, sizeof(c), &h, &info, sizeof(info));
    fsrcopyerr(err, errlen);
    if(rc != SAUER_FSR_OK || !h)
    {
        conoutf(CON_WARN, "fsr: create failed: %s", err ? err : "");
        return false;
    }
    fsrhandle = h;
    fsrhandlenative = c.mode == SAUER_FSR_MODE_NATIVE;
    fsrlastms = 0;
    conoutf(CON_INIT, "fsr: %s", err ? err : "created");
    return true;
}

static void fillimg(SauerFsrVkImage &o, const hwrtfsrimage &i)
{
    memset(&o, 0, sizeof(o));
    o.image = (uint64_t)(uintptr_t)i.image;
    o.format = uint32_t(i.format);
    o.width = uint32_t(i.w);
    o.height = uint32_t(i.h);
}

bool hwrtfsrdispatch(const hwrtfsrdispatchargs &a, char *err, int errlen)
{
    if(err && errlen > 0) err[0] = 0;
    if(!fsrhandle || !p_fsrDispatch)
    {
        copystring(err, "FSR dispatch without a context", errlen);
        return false;
    }
    SauerFsrDispatch d;
    memset(&d, 0, sizeof(d));
    d.struct_bytes = sizeof(d);
    d.command_buffer = (uint64_t)(uintptr_t)a.cmd;
    fillimg(d.color, a.color);
    fillimg(d.depth, a.depth);
    fillimg(d.motion, a.motion);
    fillimg(d.reactive, a.reactive);
    fillimg(d.exposure, a.exposure);
    fillimg(d.output, a.output);
    // Same values NGX receives: engine jitter is the projection offset in render
    // pixels, +Y up; both SDKs take it with +Y down.
    d.jitter_x = a.jitterx;
    d.jitter_y = a.jittery;
    // Packed MVs: previous - current, render pixels, +Y down (same as NGX).
    d.mv_scale_x = 1.0f;
    d.mv_scale_y = 1.0f;
    d.render_w = uint32_t(a.renderw);
    d.render_h = uint32_t(a.renderh);
    d.reset = a.reset ? 1u : 0u;
    d.sharpen = hwrtfsrsharpness > 0 && !fsrhandlenative ? 1u : 0u;
    d.sharpness = hwrtfsrsharpness / 100.0f;
    int ms = fsrlastms ? totalmillis - fsrlastms : 16;
    fsrlastms = totalmillis;
    d.frame_ms = float(clamp(ms, 1, 100));
    d.pre_exposure = 1.0f;
    extern float fovy;
    extern int farplane;
    extern float nearplane;
    d.camera_near = nearplane;
    d.camera_far = float(max(farplane, 1024));
    d.camera_fov_v = fovy * RAD;
    // Cube 2 units: a player is 15.5 units tall for about 1.8 m, so ~8.6 per metre.
    d.view_to_meters = 1.0f / 8.6f;
    d.reactive_scale = 1.0f;
    int32_t rc = p_fsrDispatch(fsrhandle, &d, sizeof(d));
    if(rc != SAUER_FSR_OK)
    {
        fsrcopyerr(err, errlen);
        return false;
    }
    return true;
}

void hwrtfsrrelease()
{
    if(fsrhandle && p_fsrRelease) p_fsrRelease(fsrhandle);
    fsrhandle = 0;
}

bool hwrtfsrconfigured() { return fsrhandle != 0; }

uint64_t hwrtfsrvram()
{
    if(!fsrhandle || !p_fsrStats) return 0;
    SauerFsrStats st;
    memset(&st, 0, sizeof(st));
    st.struct_bytes = sizeof(st);
    return p_fsrStats(fsrhandle, &st, sizeof(st)) == SAUER_FSR_OK ? st.vram_bytes : 0;
}

static void hwrtfsrstats()
{
    conoutf("fsr: loaded %s  inited %s  available %s  context %s  reason: %s",
            fsrloaded ? "yes" : "no", fsrinited ? "yes" : "no", hwrtfsravailable() ? "yes" : "no",
            fsrhandle ? "yes" : "no", hwrtfsrreason()[0] ? hwrtfsrreason() : "none");
    if(fsrinited)
        conoutf("  FSR %u.%u.%u  %s  fp16 %u  subgroup %u  sharpness %d  VRAM %.1f MB  detail: %s",
                fsrinfo.version_major, fsrinfo.version_minor, fsrinfo.version_patch, fsrinfo.sdk,
                fsrinfo.fp16, fsrinfo.wave_min, int(hwrtfsrsharpness), hwrtfsrvram() / 1048576.0, fsrdetail);
    else conoutf("  detail: %s", fsrdetail);
}
COMMAND(hwrtfsrstats, "");

#endif
