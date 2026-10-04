// dlaa.cpp: NVIDIA DLAA (native 1:1) and DLSS Super Resolution Quality/Balanced/Performance,
// and the shared pipeline AMD FSR 3.1 also uses (modes 5-8, see fsr.cpp).
//
// Client (sauerbraten.exe): bin64/ngx-hdr/sauer_ngx.dll, gateway ABI 5
// (SAUER_NGX_ABI_VERSION), with nvngx_dlss.dll in the same folder; SAUER_NGX_DIR
// overrides that folder for laboratory runs. No other folder is searched.
// The gateway is an MSVC DLL around the public NVIDIA NGX Vulkan SDK
// (nvsdk_ngx_s.lib, src/ngx_gateway); this file only sees its C ABI.
// Older gateways kept for reference clients: bin64/ngx-sr (ABI 4), bin64/ngx (ABI 1).
// Present remains SDL_GL_SwapWindow. Integration notes: src/ngx_gateway/VERSIONS.txt.

#ifdef WIN32
#include <new>
#endif
#include "engine.h"
#include "hwrt/hwrt.h"

#ifdef WIN32
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include "ngx_gateway/sauer_ngx.h"
#endif

extern int hwrtngx;
extern int hwrtvelocity, hwrtveldebug, hwrtvelforce;
extern float hwrtvelspin, hwrtvelpitchspin, hwrtvelslide, hwrtvelpush;
extern int hwrtvelholdcam, hwrtvelholdplayer, hwrtvelfreezepose, hwrtvelwalk, hwrtvelsmoke;
extern vec camdir, camright, camup;
extern void saveimage(const char *filename, int format, ImageData &image, bool flip);
extern int looktaa;
extern int hwrtdlaafixedcurtime, hwrtpartlabclock;
extern int hwrtfsrsharpness;

#ifndef WIN32

bool hwrtdlaainitbeforeinstance() { return false; }
int hwrtdlaacollectinstanceexts(const char **, int) { return 0; }
int hwrtdlaacollectdeviceexts(const char **, int) { return 0; }
uint32_t hwrtdlaaextraqueues() { return 0; }
void hwrtdlaamergefeatures12(void *) {}
void hwrtdlaamergefeatures13(void *) {}
bool hwrtdlaawantsfeatures12() { return false; }
bool hwrtdlaawantsfeatures13() { return false; }
bool hwrtdlaaondevice() { return false; }
void hwrtdlaashutdown() {}
void hwrtdlaacleanup() {}
void hwrtdlaaframe() {}
void hwrtdlaaflushshot() {}
void hwrtdlaaafterhud() {}
bool hwrtdlaaneedsdata() { return false; }
bool hwrtdlaaactive() { return false; }
int hwrtfbw() { return screenw; }
int hwrtfbh() { return screenh; }
int hwrtrenderw() { return screenw; }
int hwrtrenderh() { return screenh; }
GLuint hwrtmainfbo() { return 0; }
bool hwrtsceneinternal() { return false; }
void hwrtbindscenefb() { glBindFramebuffer_(GL_FRAMEBUFFER, 0); glViewport(0, 0, screenw, screenh); }
void hwrtscenebegin() {}
void hwrtsceneend() {}
bool hwrthdractive() { return false; }
bool hwrthdrframe() { return false; }
bool hwrthdrpresented() { return false; }
void hwrthdrsetlin(bool) {}
void hwrthdrsuspend() {}
void hwrthdrstampifprobe() {}
GLuint hwrthdrscenecolor() { return 0; }
bool hwrtflamebegin() { return false; }
void hwrtflameend() {}
GLuint hwrtflametex(int &w, int &h) { w = h = 0; return 0; }
VAR(hwrthdr, 0, 0, 1);
FVARP(hwrthdrexp, -8, 0, 8);
VAR(hwrthdrprobe, 0, 0, 1);
void hwrtdlaabiascapture() {}
void hwrtdlaabiasreset() {}
bool hwrtsceneblitwindow() { return false; }
int hwrtdlaajitterseqlen() { return 16; }
int hwrtngxmoderequested() { return 0; }
int hwrtngxmodeapplied() { return 0; }

static void hwrtdlaachanged() {}
VARF(hwrtdlaa, 0, 0, 1, hwrtdlaachanged());
VAR(hwrtdlaaavailable, 1, 0, 0);
VAR(hwrtdlaaok, 1, 0, 0);

#else

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wchar.h>
#include <psapi.h>
#include <dxgi1_4.h>
#include <math.h>
#include <stddef.h>

#define GL_TEXTURE_TILING_EXT           0x9580
#define GL_DEDICATED_MEMORY_OBJECT_EXT  0x9581
#define GL_OPTIMAL_TILING_EXT           0x9584
#define GL_HANDLE_TYPE_OPAQUE_WIN32_EXT 0x9587
#define GL_LAYOUT_GENERAL_EXT           0x958D

typedef void (APIENTRYP PFNGLCREATEMEMORYOBJECTSEXTPROC)(GLsizei n, GLuint *memoryObjects);
typedef void (APIENTRYP PFNGLDELETEMEMORYOBJECTSEXTPROC)(GLsizei n, const GLuint *memoryObjects);
typedef void (APIENTRYP PFNGLMEMORYOBJECTPARAMETERIVEXTPROC)(GLuint memoryObject, GLenum pname, const GLint *params);
typedef void (APIENTRYP PFNGLTEXSTORAGEMEM2DEXTPROC)(GLenum target, GLsizei levels, GLenum internalFormat, GLsizei width, GLsizei height, GLuint memory, GLuint64 offset);
typedef void (APIENTRYP PFNGLGENSEMAPHORESEXTPROC)(GLsizei n, GLuint *semaphores);
typedef void (APIENTRYP PFNGLDELETESEMAPHORESEXTPROC)(GLsizei n, const GLuint *semaphores);
typedef void (APIENTRYP PFNGLSIGNALSEMAPHOREEXTPROC)(GLuint semaphore, GLuint numBufferBarriers, const GLuint *buffers, GLuint numTextureBarriers, const GLuint *textures, const GLenum *dstLayouts);
typedef void (APIENTRYP PFNGLWAITSEMAPHOREEXTPROC)(GLuint semaphore, GLuint numBufferBarriers, const GLuint *buffers, GLuint numTextureBarriers, const GLuint *textures, const GLenum *srcLayouts);
typedef void (APIENTRYP PFNGLIMPORTMEMORYWIN32HANDLEEXTPROC)(GLuint memory, GLuint64 size, GLenum handleType, void *handle);
typedef void (APIENTRYP PFNGLIMPORTSEMAPHOREWIN32HANDLEEXTPROC)(GLuint semaphore, GLenum handleType, void *handle);

static PFNGLCREATEMEMORYOBJECTSEXTPROC dlaaCreateMem = NULL;
static PFNGLDELETEMEMORYOBJECTSEXTPROC dlaaDeleteMem = NULL;
static PFNGLMEMORYOBJECTPARAMETERIVEXTPROC dlaaMemParam = NULL;
static PFNGLTEXSTORAGEMEM2DEXTPROC dlaaTexStorageMem = NULL;
static PFNGLGENSEMAPHORESEXTPROC dlaaGenSem = NULL;
static PFNGLDELETESEMAPHORESEXTPROC dlaaDeleteSem = NULL;
static PFNGLSIGNALSEMAPHOREEXTPROC dlaaSignalSem = NULL;
static PFNGLWAITSEMAPHOREEXTPROC dlaaWaitSem = NULL;
static PFNGLIMPORTMEMORYWIN32HANDLEEXTPROC dlaaImportMem = NULL;
static PFNGLIMPORTSEMAPHOREWIN32HANDLEEXTPROC dlaaImportSem = NULL;

static const VkExternalMemoryHandleTypeFlagBits DLAA_MEM_HANDLE = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
static const VkExternalSemaphoreHandleTypeFlagBits DLAA_SEM_HANDLE = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;

static const char *NGX_PROJECT_ID = "c3a7e9d1-4b52-4f08-9e6a-7d1c2b8f4a90";
static const char *NGX_ENGINE_VER = "cube2-hwrt-ngx-sr";
#define DLAA_SHOTDIR "shots/dlss-q"

enum { DLAA_FIF = 3, DLAA_MAX_EXT = 24, DLAA_GPUSAMPLES = 32, DLAA_SEQMAX = 90, DLAA_BENCHMAX = 24000, DLAA_BENCHWARMSEC = 4 };

static HMODULE gwmod = NULL;
static uint64_t gwhandle = 0;
static PFN_sauer_ngx_abi_version p_gwVer = NULL;
static PFN_sauer_ngx_probe_link p_gwProbe = NULL;
static PFN_sauer_ngx_set_paths p_gwPaths = NULL;
static PFN_sauer_ngx_set_log p_gwLog = NULL;
static PFN_sauer_ngx_query_instance_exts p_gwInstExt = NULL;
static PFN_sauer_ngx_query_device_exts p_gwDevExt = NULL;
static PFN_sauer_ngx_query_caps p_gwCaps = NULL;
static PFN_sauer_ngx_init p_gwInit = NULL;
static PFN_sauer_ngx_create_dlaa p_gwCreate = NULL;
static PFN_sauer_ngx_query_optimal p_gwOptimal = NULL;
static PFN_sauer_ngx_create_feature p_gwCreateFeat = NULL;
static PFN_sauer_ngx_evaluate p_gwEval = NULL;
static PFN_sauer_ngx_get_stats p_gwStats = NULL;
static PFN_sauer_ngx_release p_gwRelease = NULL;
static PFN_sauer_ngx_shutdown p_gwShutdown = NULL;
static PFN_sauer_ngx_last_error p_gwErr = NULL;
static bool slinited = false; /* true once sauer_ngx.dll is loaded (not Streamline) */
static uint32_t slextragfx = 0, slextracompute = 0;
static uint32_t slnfeat12 = 0, slnfeat13 = 0;
static int lastevalcode = 0;


static bool ngxloaded = false;
static bool ngxinited = false;
static bool ngxsupported = false;
static bool ngxfatal = false;
static bool ngxnotnvidia = false; // Vulkan device is not an NVIDIA GPU (or SAUER_UPSCALE_SIMULATE=amd)
static char ngxreason[256] = "not initialised";
static char ngxsdkpath[MAX_PATH] = "";

static char instextstore[DLAA_MAX_EXT][128];
static const char *instextptrs[DLAA_MAX_EXT];
static int ninstexts = 0;
static char devextstore[DLAA_MAX_EXT][128];
static const char *devextptrs[DLAA_MAX_EXT];
static int ndevexts = 0;

static int dlaaavailable = 0;
static int dlaaok = 0;
static bool dlaaconfigured = false;
static bool dlaaoptok = false;
static uint32_t dlaainw = 0, dlaainh = 0, dlaaoutw = 0, dlaaouth = 0;
static uint lasttframeid = 0;
static int evalokn = 0, evalfailn = 0;
static int lasteval = 0;
static bool lastblit = false;
static bool lastroundtrip = false;
static float lastcostms = 0;
static float lastgpums = 0;
static float gpuring[DLAA_GPUSAMPLES];
static int gpuringn = 0, gpuringi = 0;
static matrix4 prevunjit;
static bool hasprevunjit = false;
static int lastpackw = 0, lastpackh = 0;
static int lastcontractok = 0;
static float lastcontracterr = -1;
static char lastcontractnote[256] = "not run";
static uint64_t lastngxbytes = 0;
static SIZE_T lastws = 0, lastpriv = 0;
static uint64_t lastgpuuse = 0, lastgpubudget = 0;
static int fallbackn = 0;
static bool inchanged = false;
static bool glsignaled = false;

struct dlaabenchsample
{
    float elapsed_ms;
    float cpu_ms, gpu_ms, hwrt_frame_ms, hwrt_gpu_ms, vel_play_ms;
    int active, needs, stalls, dlaa;
};

static int benchphase = 0; /* 0 idle, 1 warm, 2 run */
static int benchn = 0, benchrunsec = 25;
static char benchname[128] = "";
static dlaabenchsample benchsamp[DLAA_BENCHMAX];
static double benchwarmuntil = 0, benchrununtil = 0, benchrunstart = 0, benchprev = 0, benchrunend = 0;
static float benchcam0[5], benchcam1[5];
static int benchcamgot = 0;

VAR(hwrtdlaaavailable, 1, 0, 0);
VAR(hwrtdlaaok, 1, 0, 0);
VAR(hwrtdlaaroundtrip, 0, 0, 1);
VAR(hwrtdlaaallowunsigned, 0, 0, 1);
// 0 none, 1 fail before vkQueueSubmit, 2 fail configure/prep, 3 fail evaluate, 4 fail blit
VAR(hwrtdlaainject, 0, 0, 4);
VAR(hwrtdlaabenchdone, 0, 1, 1);

static void hwrtdlaachanged();
VARF(hwrtdlaa, 0, 0, 1, hwrtdlaachanged());

struct dlaatex
{
    VkImage image;
    VkDeviceMemory memory;
    VkDeviceSize memsize;
    VkImageView view;
    VkFormat format;
    VkImageUsageFlags usage;
    GLuint glmem, gltex;
    HANDLE memhandle;
    GLenum glifmt;
};

static dlaatex texcolorin{}, texdepth{}, texmotion{}, texcolorout{}, texbias{}, texexposure{};
static dlaatex ngxcolorin{}, ngxdepth{}, ngxmotion{}, ngxcoloroutn{};
static VkSemaphore vkglready = VK_NULL_HANDLE, vkvkdone = VK_NULL_HANDLE;
static GLuint glglready = 0, glvkdone = 0;
static HANDLE glreadyhandle = NULL, vkdonehandle = NULL;
static GLuint packfbo = 0;
static GLuint biasfbo = 0, biasgl[2] = {0, 0}, biasds = 0;
static int biasglw = 0, biasglh = 0, biascurr = 0;
static GLuint lastbiasgl = 0;
static int lastbiasbound = 0, lastbiasmode = 0;
static int lastbiassrcfb = 0, lastbiasw = 0, lastbiash = 0;
static int lastbiasrecreate = 0, lastbiaspersistused = 0, lastbiasneeds = 0;
static vec biasprevpos(0, 0, 0);
static float biasprevyaw = 0, biasprevpitch = 0;
static int biasprevcamvalid = 0;
static GLuint biascostq = 0;
static bool biascostpending = false;
static float lastbiasgpums = 0;
static void destroybiasgl();
static int resw = 0, resh = 0;
static int resinw = 0, resinh = 0, resoutw = 0, resouth = 0;
static int reshdr = -1;
static int ngxcfghdr = -1, ngxcfgautoexp = -1;
static int ngxmodereq = 0, ngxmodeapp = 0, ngxmodehave = 0;
static bool ngxblocked = false;
static uint32_t ngxoptw = 0, ngxopth = 0, ngxoptminw = 0, ngxoptminh = 0, ngxoptmaxw = 0, ngxoptmaxh = 0;
static uint32_t ngxoptok = 0, ngxusedoptimal = 0;
static uint32_t lastngxinw = 0, lastngxinh = 0, lastngxoutw = 0, lastngxouth = 0, lastngxoptw = 0, lastngxopth = 0;
static GLuint scenefbo = 0, scenecolor = 0, sceneds = 0;
static int scenew = 0, sceneh = 0;
static GLenum scenecolorfmt = 0;
static bool scenebound = false, scenehdr = false;
static bool hdrframe = false, hdrpresented = false;
static bool scenefail = false;
static int lastscenew = 0, lastsceneh = 0;
static int lastfbw = 0, lastfbh = 0;
static char ngxshotsub[64] = "";

static void hwrtngxmodechanged();
static const char *dlssreason();
static void destroyscenefb();
static void applyngxmode(int m);
static bool ngxautoapply = false;
static void refreshlookaa()
{
    if(initing) return;
    if(identexists("LOOK_apply")) execute("LOOK_apply");
}
// 0 Native, 1 DLAA, 2-4 DLSS Quality/Balanced/Performance,
// 5 FSR Native, 6-8 FSR Quality/Balanced/Performance (fsr.cpp).
VARFP(hwrtngxmode, 0, 0, 8, hwrtngxmodechanged());
// hwrthdr is the effective internal HDR state, set every frame by hdrout.cpp
// from the saved hdroutpref; never saved. hwrthdrexp is the saved exposure (EV).
// The other switches are laboratory only and never saved.
VAR(hwrthdr, 0, 0, 1);
FVARP(hwrthdrexp, -8, 0, 8);
VAR(hwrthdrprobe, 0, 0, 1);
VAR(hwrthdrfam, 0, 0, 1);
// Lab only: pretend the FP16 scene target failed. Preference hwrthdr stays set.
VAR(hwrthdrfaillab, 0, 0, 1);
static int hdrreadpending = 0;
VAR(hwrtdlaacapturefallback, 0, 0, 1);
VAR(hwrtdlaacapturepresent, 0, 0, 1);
// Explosion/smoke coverage mask, used only as the FSR 3.1 reactive mask (modes 5-8).
// It is not given to DLSS: the NVIDIA DLSS Programming Guide (310.6.0, 3.15) says
// BiasCurrentColor "should not be used" with the current models (only the
// deprecated preset F supported it), and we use presets K and M. With DLAA/DLSS
// the mask is neither drawn nor packed. 0 = no mask. 1 = coverage that follows the
// visible texture and fade, plus a short persist only while the camera is still.
// 2 = all-ones lab test.
static void hwrtdlaabiaschanged();
VARF(hwrtdlaabias, 0, 1, 2, hwrtdlaabiaschanged());
VAR(hwrtdlaabiaspersist, 0, 50, 100);
// Lab: dump packed NGX colour in/out (or temporal colour when Off) every N sequence frames. 0 = off.
VAR(hwrtdlaaiostep, 0, 8, 64);
static bool presentpending = false;
static char presentname[128] = "fallback_hud";
static int qcacheoutw = 0, qcacheouth = 0, qcacheinw = 0, qcacheinh = 0, qcachemode = -1;
static bool qcacheok = false;
static int ngxfeatfresh = 0;
static int lastfeatfresh = 0;
static float lastinmean = -1, lastoutmean = -1;

static VkCommandPool cmdpool = VK_NULL_HANDLE;
static VkCommandBuffer cmdbuf[DLAA_FIF];
static VkFence fences[DLAA_FIF];
static int fifslot = 0;
static bool cmdok = false;

static GLuint costq[DLAA_FIF];
static bool costqok = false, costpending[DLAA_FIF];
static int costslot = 0;

static unsigned char *seqpixels = NULL;
static int seqn = 0, seqi = 0, seqw = 0, seqh = 0;
static char seqname[128] = "";

#define DLAA_IOMAX 20
struct seqioshot
{
    int index, inw, inh, outw, outh, mode, src;
    unsigned char *inrgb, *outrgb, *depthvis, *mvvis, *biasrgb;
};
static seqioshot seqios[DLAA_IOMAX];
static int seqion = 0;
static FILE *seqlog = NULL;


static void setreason(const char *s)
{
    copystring(ngxreason, s ? s : "", sizeof(ngxreason));
}


static bool fileexists_a(const char *p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool resolvedir(char *out, int outlen)
{
    const char *env = getenv("SAUER_NGX_DIR");
    if(env && env[0])
    {
        // Lab override is exclusive: do not silently pick bin64/ngx-sr.
        copystring(out, env, outlen);
        string probe;
        nformatstring(probe, sizeof(probe), "%s\\nvngx_dlss.dll", out);
        return fileexists_a(probe);
    }
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
    if(!n || n >= MAX_PATH) return false;
    char *slash = strrchr(exe, '\\');
    if(!slash) slash = strrchr(exe, '/');
    if(slash) *slash = 0;
    string probe;
    // Only the ABI 5 folder. bin64\ngx-sr (ABI 4) and bin64\ngx (ABI 1) belong
    // to older reference clients; no other folder is a fallback.
    nformatstring(out, outlen, "%s\\ngx-hdr", exe);
    nformatstring(probe, sizeof(probe), "%s\\nvngx_dlss.dll", out);
    return fileexists_a(probe);
}


static bool loadglinterop()
{
    if(dlaaCreateMem) return true;
    dlaaCreateMem = (PFNGLCREATEMEMORYOBJECTSEXTPROC)getprocaddress("glCreateMemoryObjectsEXT");
    dlaaDeleteMem = (PFNGLDELETEMEMORYOBJECTSEXTPROC)getprocaddress("glDeleteMemoryObjectsEXT");
    dlaaMemParam = (PFNGLMEMORYOBJECTPARAMETERIVEXTPROC)getprocaddress("glMemoryObjectParameterivEXT");
    dlaaTexStorageMem = (PFNGLTEXSTORAGEMEM2DEXTPROC)getprocaddress("glTexStorageMem2DEXT");
    dlaaGenSem = (PFNGLGENSEMAPHORESEXTPROC)getprocaddress("glGenSemaphoresEXT");
    dlaaDeleteSem = (PFNGLDELETESEMAPHORESEXTPROC)getprocaddress("glDeleteSemaphoresEXT");
    dlaaSignalSem = (PFNGLSIGNALSEMAPHOREEXTPROC)getprocaddress("glSignalSemaphoreEXT");
    dlaaWaitSem = (PFNGLWAITSEMAPHOREEXTPROC)getprocaddress("glWaitSemaphoreEXT");
    dlaaImportMem = (PFNGLIMPORTMEMORYWIN32HANDLEEXTPROC)getprocaddress("glImportMemoryWin32HandleEXT");
    dlaaImportSem = (PFNGLIMPORTSEMAPHOREWIN32HANDLEEXTPROC)getprocaddress("glImportSemaphoreWin32HandleEXT");
    return dlaaCreateMem && dlaaDeleteMem && dlaaMemParam && dlaaTexStorageMem &&
           dlaaGenSem && dlaaDeleteSem && dlaaSignalSem && dlaaWaitSem &&
           dlaaImportMem && dlaaImportSem;
}

static int collectnamed(const char **dst, int maxn, const char **src, int nsrc)
{
    int n = 0;
    loopi(nsrc)
    {
        if(n >= maxn) break;
        dst[n++] = src[i];
    }
    return n;
}


static void gwlog(const char *message)
{
    if(message && message[0]) conoutf(CON_INIT, "ngx: %s", message);
}

static void gwcopyerr()
{
    if(!p_gwErr) return;
    SauerNgxError e;
    memset(&e, 0, sizeof(e));
    e.struct_bytes = sizeof(e);
    p_gwErr(&e, sizeof(e));
    lasteval = int(e.ngx_result);
    lastevalcode = e.code;
    if(e.message[0]) setreason(e.message);
}

static const char *lastevalstr()
{
    static char buf[96];
    nformatstring(buf, sizeof(buf), "ngx 0x%08X gw %d", (unsigned)lasteval, lastevalcode);
    return buf;
}

static bool gwresolvefuns()
{
    p_gwVer = (PFN_sauer_ngx_abi_version)GetProcAddress(gwmod, "sauer_ngx_abi_version");
    p_gwProbe = (PFN_sauer_ngx_probe_link)GetProcAddress(gwmod, "sauer_ngx_probe_link");
    p_gwPaths = (PFN_sauer_ngx_set_paths)GetProcAddress(gwmod, "sauer_ngx_set_paths");
    p_gwLog = (PFN_sauer_ngx_set_log)GetProcAddress(gwmod, "sauer_ngx_set_log");
    p_gwInstExt = (PFN_sauer_ngx_query_instance_exts)GetProcAddress(gwmod, "sauer_ngx_query_instance_exts");
    p_gwDevExt = (PFN_sauer_ngx_query_device_exts)GetProcAddress(gwmod, "sauer_ngx_query_device_exts");
    p_gwCaps = (PFN_sauer_ngx_query_caps)GetProcAddress(gwmod, "sauer_ngx_query_caps");
    p_gwInit = (PFN_sauer_ngx_init)GetProcAddress(gwmod, "sauer_ngx_init");
    p_gwCreate = (PFN_sauer_ngx_create_dlaa)GetProcAddress(gwmod, "sauer_ngx_create_dlaa");
    p_gwOptimal = (PFN_sauer_ngx_query_optimal)GetProcAddress(gwmod, "sauer_ngx_query_optimal");
    p_gwCreateFeat = (PFN_sauer_ngx_create_feature)GetProcAddress(gwmod, "sauer_ngx_create_feature");
    p_gwEval = (PFN_sauer_ngx_evaluate)GetProcAddress(gwmod, "sauer_ngx_evaluate");
    p_gwStats = (PFN_sauer_ngx_get_stats)GetProcAddress(gwmod, "sauer_ngx_get_stats");
    p_gwRelease = (PFN_sauer_ngx_release)GetProcAddress(gwmod, "sauer_ngx_release");
    p_gwShutdown = (PFN_sauer_ngx_shutdown)GetProcAddress(gwmod, "sauer_ngx_shutdown");
    p_gwErr = (PFN_sauer_ngx_last_error)GetProcAddress(gwmod, "sauer_ngx_last_error");
    return p_gwVer && p_gwProbe && p_gwPaths && p_gwInit && p_gwCreateFeat && p_gwEval && p_gwRelease && p_gwShutdown && p_gwErr && p_gwOptimal;
}

static bool gwloaddll(char *dir, int dirlen)
{
    if(!resolvedir(dir, dirlen))
    {
        setreason("nvngx_dlss.dll folder missing (bin64\\ngx-hdr ABI 5 gateway, or SAUER_NGX_DIR)");
        return false;
    }
    copystring(ngxsdkpath, dir, sizeof(ngxsdkpath));
    string dll;
    nformatstring(dll, sizeof(dll), "%s\\sauer_ngx.dll", dir);
    if(!fileexists_a(dll))
    {
        setreason("sauer_ngx.dll missing (MSVC gateway: src\\ngx_gateway\\build-dll-hdr.bat)");
        conoutf(CON_WARN, "hwrt dlaa: %s", ngxreason);
        return false;
    }
    gwmod = LoadLibraryA(dll);
    if(!gwmod)
    {
        defformatstring(msg, "LoadLibrary sauer_ngx.dll failed (%u)", (unsigned)GetLastError());
        setreason(msg);
        return false;
    }
    if(!gwresolvefuns())
    {
        setreason("sauer_ngx.dll missing required C exports");
        FreeLibrary(gwmod);
        gwmod = NULL;
        return false;
    }
    if(p_gwVer() != SAUER_NGX_ABI_VERSION)
    {
        defformatstring(msg, "sauer_ngx ABI %u != header %u (this client needs the ABI 5 gateway in bin64\\ngx-hdr; bin64\\ngx-sr is ABI 4)", (unsigned)p_gwVer(), SAUER_NGX_ABI_VERSION);
        setreason(msg);
        FreeLibrary(gwmod);
        gwmod = NULL;
        return false;
    }
    return true;
}

static void fillgwimg(SauerNgxVkImage &o, const dlaatex &t, int w, int h, int rw)
{
    memset(&o, 0, sizeof(o));
    o.image = (uint64_t)(uintptr_t)t.image;
    o.image_view = (uint64_t)(uintptr_t)t.view;
    o.format = uint32_t(t.format);
    o.width = uint32_t(w);
    o.height = uint32_t(h);
    o.aspect_mask = VK_IMAGE_ASPECT_COLOR_BIT;
    o.read_write = rw;
}

bool hwrtdlaainitbeforeinstance()
{
    if(ngxloaded) return true;
    if(ngxfatal) return false;
    char ngxdir[MAX_PATH];
    if(!gwloaddll(ngxdir, sizeof(ngxdir))) return false;
    if(p_gwLog) p_gwLog(gwlog);
    SauerNgxPaths paths;
    memset(&paths, 0, sizeof(paths));
    paths.struct_bytes = sizeof(paths);
    copystring(paths.dll_dir, ngxdir, SAUER_NGX_PATH);
    nformatstring(paths.log_dir, SAUER_NGX_PATH, "%s\\logs", ngxdir);
    CreateDirectoryA(paths.log_dir, NULL);
    if(p_gwPaths(&paths, sizeof(paths)) != SAUER_NGX_OK)
    {
        gwcopyerr();
        return false;
    }
    char probe[768];
    memset(probe, 0, sizeof(probe));
    if(p_gwProbe(probe, sizeof(probe)) != SAUER_NGX_OK)
    {
        gwcopyerr();
        return false;
    }
    conoutf(CON_INIT, "hwrt dlaa: %s", probe);
    ninstexts = 0;
    ndevexts = 0;
    if(p_gwInstExt)
    {
        SauerNgxExtList exts;
        memset(&exts, 0, sizeof(exts));
        exts.struct_bytes = sizeof(exts);
        if(p_gwInstExt(&exts, sizeof(exts)) == SAUER_NGX_OK)
        {
            loopi(int(exts.count))
            {
                if(ninstexts >= DLAA_MAX_EXT) break;
                if(!exts.names[i][0]) continue;
                copystring(instextstore[ninstexts], exts.names[i], 128);
                instextptrs[ninstexts] = instextstore[ninstexts];
                ninstexts++;
            }
        }
        else
        {
            gwcopyerr();
            conoutf(CON_WARN, "hwrt dlaa: instance ext query %s (interop-only instance; NGX Init may still succeed)", ngxreason);
        }
    }
    ngxloaded = true;
    slinited = true;
    setreason("NGX gateway loaded, waiting for Vulkan device");
    conoutf(CON_INIT, "hwrt dlaa: public NGX Vulkan SDK via sauer_ngx.dll from %s. Streamline is not loaded. Present remains SDL_GL_SwapWindow.", ngxdir);
    return true;
}

int hwrtdlaacollectinstanceexts(const char **names, int maxnames)
{
    if(!ngxloaded || !names || maxnames <= 0 || !ninstexts) return 0;
    if(!vkEnumerateInstanceExtensionProperties) return collectnamed(names, maxnames, instextptrs, ninstexts);
    uint32_t nprops = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &nprops, NULL);
    if(!nprops) return 0;
    VkExtensionProperties *props = new VkExtensionProperties[nprops];
    vkEnumerateInstanceExtensionProperties(NULL, &nprops, props);
    int n = 0;
    loopi(ninstexts)
    {
        if(n >= maxnames) break;
        bool ok = false;
        loopj(int(nprops)) if(!strcmp(props[j].extensionName, instextptrs[i])) { ok = true; break; }
        if(!ok)
        {
            conoutf(CON_INIT, "hwrt dlaa: instance extension %s not present, skipping", instextptrs[i]);
            continue;
        }
        names[n++] = instextptrs[i];
    }
    delete[] props;
    return n;
}

int hwrtdlaacollectdeviceexts(const char **names, int maxnames)
{
    if(!ndevexts && ngxloaded && p_gwDevExt && hwrtdev.instance && hwrtdev.phys)
    {
        SauerNgxExtList exts;
        memset(&exts, 0, sizeof(exts));
        exts.struct_bytes = sizeof(exts);
        if(p_gwDevExt((uint64_t)(uintptr_t)hwrtdev.instance, (uint64_t)(uintptr_t)hwrtdev.phys, &exts, sizeof(exts)) == SAUER_NGX_OK)
        {
            loopi(int(exts.count))
            {
                if(ndevexts >= DLAA_MAX_EXT) break;
                if(!exts.names[i][0]) continue;
                copystring(devextstore[ndevexts], exts.names[i], 128);
                devextptrs[ndevexts] = devextstore[ndevexts];
                ndevexts++;
            }
            conoutf(CON_INIT, "hwrt dlaa: NGX device extensions: %d", ndevexts);
            loopi(ndevexts) conoutf(CON_INIT, "hwrt dlaa: device ext %s", devextptrs[i]);
        }
        else
        {
            gwcopyerr();
            conoutf(CON_WARN, "hwrt dlaa: device ext query %s", ngxreason);
        }
    }
    if(!ngxloaded || !names || maxnames <= 0 || !ndevexts) return 0;
    return collectnamed(names, maxnames, devextptrs, ndevexts);
}

uint32_t hwrtdlaaextraqueues() { return slextragfx + slextracompute; }
bool hwrtdlaawantsfeatures12() { return slnfeat12 > 0; }
bool hwrtdlaawantsfeatures13() { return slnfeat13 > 0; }

void hwrtdlaamergefeatures12(void *) {}
void hwrtdlaamergefeatures13(void *) {}

bool hwrtdlaaondevice()
{
    if(!ngxloaded || !hwrtdev.device || !p_gwInit) return false;
    // NGX only runs on NVIDIA: do not even start it elsewhere. The menu then
    // greys DLAA/DLSS with this reason; FSR stays offered.
    VkPhysicalDeviceProperties devprops;
    memset(&devprops, 0, sizeof(devprops));
    vkGetPhysicalDeviceProperties(hwrtdev.phys, &devprops);
    ngxnotnvidia = devprops.vendorID != 0x10DE || hwrtfsrsimulating("amd");
    if(ngxnotnvidia)
    {
        setreason(devprops.vendorID != 0x10DE ? "not an NVIDIA GPU: NGX not started" : "not an NVIDIA GPU (simulated: SAUER_UPSCALE_SIMULATE=amd): NGX not started");
        conoutf(CON_INIT, "hwrt dlaa: %s (%s, vendor 0x%04X)", ngxreason, hwrtdev.name, devprops.vendorID);
        return false;
    }
    SauerNgxInit in;
    memset(&in, 0, sizeof(in));
    in.struct_bytes = sizeof(in);
    in.instance = (uint64_t)(uintptr_t)hwrtdev.instance;
    in.physical_device = (uint64_t)(uintptr_t)hwrtdev.phys;
    in.device = (uint64_t)(uintptr_t)hwrtdev.device;
    in.gipa = (uint64_t)(uintptr_t)vkGetInstanceProcAddr;
    in.gdpa = (uint64_t)(uintptr_t)vkGetDeviceProcAddr;
    if(p_gwInit(&in, sizeof(in)) != SAUER_NGX_OK)
    {
        gwcopyerr();
        conoutf(CON_WARN, "hwrt dlaa: %s", ngxreason);
        return false;
    }
    if(p_gwCaps)
    {
        SauerNgxCaps caps;
        memset(&caps, 0, sizeof(caps));
        caps.struct_bytes = sizeof(caps);
        p_gwCaps(in.instance, in.physical_device, &caps, sizeof(caps));
        conoutf(CON_INIT, "hwrt dlaa: NGX SuperSampling available=%u featureSupported=%d minHW=%u",
                caps.super_sampling_available, int(caps.feature_supported), caps.min_hw_arch);
    }
    ngxinited = true;
    ngxsupported = true;
    dlaaavailable = 1;
    hwrtdlaaavailable = 1;
    hwrtngx = 1;
    setreason("available (inactive until hwrtngxmode 1 DLAA, 2 Quality, 3 Balanced or 4 Performance)");
    conoutf(CON_INIT, "hwrt dlaa: NGX Vulkan Init_with_ProjectID ok on %s. Evaluate is NGX_VULKAN_EVALUATE_DLSS_EXT via sauer_ngx.dll. Streamline is not loaded. Present remains SDL_GL_SwapWindow.",
            hwrtdev.name);
    return true;
}

static void destroytex(dlaatex &t)
{
    if(t.gltex) { glDeleteTextures(1, &t.gltex); t.gltex = 0; }
    if(t.glmem && dlaaDeleteMem) { dlaaDeleteMem(1, &t.glmem); t.glmem = 0; }
    if(hwrtdev.device)
    {
        if(t.view) vkDestroyImageView(hwrtdev.device, t.view, NULL);
        if(t.image) vkDestroyImage(hwrtdev.device, t.image, NULL);
        if(t.memory) vkFreeMemory(hwrtdev.device, t.memory, NULL);
    }
    t.view = VK_NULL_HANDLE;
    t.image = VK_NULL_HANDLE;
    t.memory = VK_NULL_HANDLE;
    t.memsize = 0;
    if(t.memhandle) { CloseHandle(t.memhandle); t.memhandle = NULL; }
}

static bool createtex(dlaatex &t, int w, int h, VkFormat fmt, GLenum glifmt, VkImageUsageFlags usage, GLenum filt, bool sharegl = true)
{
    memset(&t, 0, sizeof(t));
    t.format = fmt;
    t.usage = usage;
    t.glifmt = glifmt;

    VkExternalMemoryImageCreateInfo extinfo = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
    extinfo.handleTypes = DLAA_MEM_HANDLE;
    VkImageCreateInfo imginfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    if(sharegl) imginfo.pNext = &extinfo;
    imginfo.imageType = VK_IMAGE_TYPE_2D;
    imginfo.format = fmt;
    imginfo.extent.width = uint32_t(w);
    imginfo.extent.height = uint32_t(h);
    imginfo.extent.depth = 1;
    imginfo.mipLevels = 1;
    imginfo.arrayLayers = 1;
    imginfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imginfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imginfo.usage = usage;
    imginfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imginfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult r = vkCreateImage(hwrtdev.device, &imginfo, NULL, &t.image);
    if(r != VK_SUCCESS) { conoutf(CON_WARN, "hwrt dlaa: vkCreateImage failed (%s)", hwrtresultstr(r)); return false; }

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(hwrtdev.device, t.image, &req);
    int memtype = hwrtfindmemtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if(memtype < 0) { conoutf(CON_WARN, "hwrt dlaa: no device-local memory type"); return false; }

    VkMemoryDedicatedAllocateInfo dedicated = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    dedicated.image = t.image;
    VkExportMemoryWin32HandleInfoKHR win32info = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
    win32info.dwAccess = GENERIC_ALL;
    win32info.pNext = &dedicated;
    VkExportMemoryAllocateInfo exportinfo = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
    exportinfo.handleTypes = DLAA_MEM_HANDLE;
    exportinfo.pNext = &win32info;
    VkMemoryAllocateInfo allocinfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocinfo.pNext = sharegl ? (void *)&exportinfo : (void *)&dedicated;
    allocinfo.allocationSize = req.size;
    allocinfo.memoryTypeIndex = uint32_t(memtype);
    r = vkAllocateMemory(hwrtdev.device, &allocinfo, NULL, &t.memory);
    if(r != VK_SUCCESS) { conoutf(CON_WARN, "hwrt dlaa: vkAllocateMemory failed (%s)", hwrtresultstr(r)); return false; }
    t.memsize = req.size;
    r = vkBindImageMemory(hwrtdev.device, t.image, t.memory, 0);
    if(r != VK_SUCCESS) { conoutf(CON_WARN, "hwrt dlaa: vkBindImageMemory failed (%s)", hwrtresultstr(r)); return false; }

    VkImageViewCreateInfo viewinfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    viewinfo.image = t.image;
    viewinfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewinfo.format = fmt;
    viewinfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewinfo.subresourceRange.levelCount = 1;
    viewinfo.subresourceRange.layerCount = 1;
    r = vkCreateImageView(hwrtdev.device, &viewinfo, NULL, &t.view);
    if(r != VK_SUCCESS) { conoutf(CON_WARN, "hwrt dlaa: vkCreateImageView failed (%s)", hwrtresultstr(r)); return false; }

    if(!sharegl) return true;

    VkMemoryGetWin32HandleInfoKHR getinfo = { VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR };
    getinfo.memory = t.memory;
    getinfo.handleType = DLAA_MEM_HANDLE;
    HANDLE handle = NULL;
    r = vkGetMemoryWin32HandleKHR(hwrtdev.device, &getinfo, &handle);
    if(r != VK_SUCCESS) { conoutf(CON_WARN, "hwrt dlaa: vkGetMemoryWin32HandleKHR failed (%s)", hwrtresultstr(r)); return false; }

    while(glGetError() != GL_NO_ERROR);
    dlaaCreateMem(1, &t.glmem);
    GLint dedicatedflag = GL_TRUE;
    dlaaMemParam(t.glmem, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicatedflag);
    dlaaImportMem(t.glmem, t.memsize, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, handle);
    t.memhandle = handle;
    glActiveTexture_(GL_TEXTURE0);
    glGenTextures(1, &t.gltex);
    glBindTexture(GL_TEXTURE_2D, t.gltex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_TILING_EXT, GL_OPTIMAL_TILING_EXT);
    dlaaTexStorageMem(GL_TEXTURE_2D, 1, glifmt, w, h, t.glmem, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    GLenum err = glGetError();
    if(err != GL_NO_ERROR)
    {
        conoutf(CON_WARN, "hwrt dlaa: GL import failed (0x%x)", int(err));
        return false;
    }
    return true;
}

static void destroysems()
{
    if(glvkdone && dlaaDeleteSem) { dlaaDeleteSem(1, &glvkdone); glvkdone = 0; }
    if(glglready && dlaaDeleteSem) { dlaaDeleteSem(1, &glglready); glglready = 0; }
    if(hwrtdev.device)
    {
        if(vkvkdone) vkDestroySemaphore(hwrtdev.device, vkvkdone, NULL);
        if(vkglready) vkDestroySemaphore(hwrtdev.device, vkglready, NULL);
    }
    vkvkdone = vkglready = VK_NULL_HANDLE;
    if(glreadyhandle) { CloseHandle(glreadyhandle); glreadyhandle = NULL; }
    if(vkdonehandle) { CloseHandle(vkdonehandle); vkdonehandle = NULL; }
    glsignaled = false;
}

static bool createsem(VkSemaphore &vksem, GLuint &glsem, HANDLE &keep)
{
    VkExportSemaphoreWin32HandleInfoKHR win32info = { VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR };
    win32info.dwAccess = GENERIC_ALL;
    VkExportSemaphoreCreateInfo exportinfo = { VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO };
    exportinfo.handleTypes = DLAA_SEM_HANDLE;
    exportinfo.pNext = &win32info;
    VkSemaphoreCreateInfo info = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    info.pNext = &exportinfo;
    VkResult r = vkCreateSemaphore(hwrtdev.device, &info, NULL, &vksem);
    if(r != VK_SUCCESS) return false;
    VkSemaphoreGetWin32HandleInfoKHR getinfo = { VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR };
    getinfo.semaphore = vksem;
    getinfo.handleType = DLAA_SEM_HANDLE;
    HANDLE handle = NULL;
    r = vkGetSemaphoreWin32HandleKHR(hwrtdev.device, &getinfo, &handle);
    if(r != VK_SUCCESS) return false;
    dlaaGenSem(1, &glsem);
    dlaaImportSem(glsem, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, handle);
    keep = handle;
    return glGetError() == GL_NO_ERROR;
}

static void destroycmd()
{
    if(hwrtdev.device)
    {
        loopi(DLAA_FIF) if(fences[i]) vkDestroyFence(hwrtdev.device, fences[i], NULL);
        if(cmdpool) vkDestroyCommandPool(hwrtdev.device, cmdpool, NULL);
    }
    memset(cmdbuf, 0, sizeof(cmdbuf));
    memset(fences, 0, sizeof(fences));
    cmdpool = VK_NULL_HANDLE;
    cmdok = false;
}

static bool ensurecmd()
{
    if(cmdok) return true;
    if(!hwrtdev.device) return false;
    VkCommandPoolCreateInfo poolinfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolinfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolinfo.queueFamilyIndex = hwrtdev.queuefamily;
    if(vkCreateCommandPool(hwrtdev.device, &poolinfo, NULL, &cmdpool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo alloc = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    alloc.commandPool = cmdpool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = DLAA_FIF;
    if(vkAllocateCommandBuffers(hwrtdev.device, &alloc, cmdbuf) != VK_SUCCESS) return false;
    loopi(DLAA_FIF)
    {
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if(vkCreateFence(hwrtdev.device, &fi, NULL, &fences[i]) != VK_SUCCESS) return false;
    }
    cmdok = true;
    fifslot = 0;
    return true;
}

static bool recreatesignaledfence(int slot)
{
    if(!hwrtdev.device || slot < 0 || slot >= DLAA_FIF) return false;
    if(fences[slot]) vkDestroyFence(hwrtdev.device, fences[slot], NULL);
    fences[slot] = VK_NULL_HANDLE;
    VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    return vkCreateFence(hwrtdev.device, &fi, NULL, &fences[slot]) == VK_SUCCESS;
}

static bool draininterop();

static void waitglcomplete()
{
    if(glsignaled && cmdok && hwrtdev.queue) draininterop();
    glFinish();
}

static void transitionall(VkImageLayout dst)
{
    if(!cmdok || !hwrtdev.queue) return;
    vkWaitForFences(hwrtdev.device, 1, &fences[0], VK_TRUE, UINT64_MAX);
    vkResetCommandBuffer(cmdbuf[0], 0);
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if(vkBeginCommandBuffer(cmdbuf[0], &begin) != VK_SUCCESS) return;
    VkImage images[6];
    images[0] = texcolorin.image;
    images[1] = texdepth.image;
    images[2] = texmotion.image;
    images[3] = texcolorout.image;
    int nimg = 4;
    if(texbias.image) images[nimg++] = texbias.image;
    if(texexposure.image) images[nimg++] = texexposure.image;
    VkImageMemoryBarrier b[6];
    loopi(nimg)
    {
        memset(&b[i], 0, sizeof(b[i]));
        b[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b[i].newLayout = dst;
        b[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b[i].image = images[i];
        b[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b[i].subresourceRange.levelCount = 1;
        b[i].subresourceRange.layerCount = 1;
        b[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    vkCmdPipelineBarrier(cmdbuf[0], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, NULL, 0, NULL, nimg, b);
    if(vkEndCommandBuffer(cmdbuf[0]) != VK_SUCCESS) return;
    vkResetFences(hwrtdev.device, 1, &fences[0]);
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmdbuf[0];
    if(vkQueueSubmit(hwrtdev.queue, 1, &submit, fences[0]) != VK_SUCCESS)
    {
        recreatesignaledfence(0);
        return;
    }
    vkWaitForFences(hwrtdev.device, 1, &fences[0], VK_TRUE, UINT64_MAX);
}

static void releasefeature()
{
    if(!dlaaconfigured && !gwhandle && !hwrtfsrconfigured()) return;
    if(hwrtdev.device) vkDeviceWaitIdle(hwrtdev.device);
    if(gwhandle && p_gwRelease)
    {
        if(p_gwRelease(gwhandle) != SAUER_NGX_OK) gwcopyerr();
        gwhandle = 0;
    }
    hwrtfsrrelease();
    dlaaconfigured = false;
    if(dlaainw || dlaainh || dlaaoutw || dlaaouth)
    {
        lastngxinw = dlaainw;
        lastngxinh = dlaainh;
        lastngxoutw = dlaaoutw;
        lastngxouth = dlaaouth;
        lastngxoptw = ngxoptw;
        lastngxopth = ngxopth;
    }
    dlaainw = dlaainh = dlaaoutw = dlaaouth = 0;
    ngxusedoptimal = 0;
    ngxoptw = ngxopth = ngxoptminw = ngxoptminh = ngxoptmaxw = ngxoptmaxh = 0;
    ngxoptok = 0;
}

void hwrtdlaacleanup()
{
    waitglcomplete();
    if(hwrtdev.device) vkDeviceWaitIdle(hwrtdev.device);
    releasefeature();
    if(packfbo) { glDeleteFramebuffers_(1, &packfbo); packfbo = 0; }
    if(costqok)
    {
        glDeleteQueries_(DLAA_FIF, costq);
        costqok = false;
        memset(costpending, 0, sizeof(costpending));
    }
    if(biascostq)
    {
        glDeleteQueries_(1, &biascostq);
        biascostq = 0;
        biascostpending = false;
        lastbiasgpums = 0;
    }
    destroytex(texcolorin);
    destroytex(texdepth);
    destroytex(texmotion);
    destroytex(texcolorout);
    destroytex(texbias);
    destroytex(texexposure);
    destroytex(ngxcolorin);
    destroytex(ngxdepth);
    destroytex(ngxmotion);
    destroytex(ngxcoloroutn);
    destroybiasgl();
    destroysems();
    destroycmd();
    resw = resh = 0;
    resinw = resinh = resoutw = resouth = 0;
    reshdr = -1;
    ngxcfghdr = -1;
    qcacheok = false;
    destroyscenefb();
    ngxmodehave = 0;
    ngxmodeapp = 0;
    ngxblocked = false;
    dlaaok = 0;
    hwrtdlaaok = 0;
    lastblit = false;
    hasprevunjit = false;
    glsignaled = false;
}

static VkImageUsageFlags texusage()
{
    return VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
}

static const char *ngxmodename(int m)
{
    if(m == 8) return "FSR Performance";
    if(m == 7) return "FSR Equilibre";
    if(m == 6) return "FSR Qualite";
    if(m == 5) return "FSR Native";
    if(m == 4) return "DLSS Performance";
    if(m == 3) return "DLSS Equilibre";
    if(m == 2) return "DLSS Qualite";
    if(m == 1) return "DLAA";
    return "OFF";
}

// "SR" = the scene is rendered below the output size (DLSS or FSR upscaling).
static bool ngxissrmode(int m) { return (m >= 2 && m <= 4) || (m >= 6 && m <= 8); }
static bool ngxneedsfeature(int m) { return m >= 1 && m <= 8; }
static bool fsrmode(int m) { return m >= 5 && m <= 8; }
// The upscaler behind mode m can run on this machine.
static bool modeready(int m)
{
    // Never on a device a timed-out start-up worker may still be creating.
    if(!hwrtvkready()) return false;
    if(fsrmode(m)) return hwrtfsravailable();
    return dlaaavailable && ngxsupported && !ngxfatal;
}

static uint32_t gwmodefromengine(int m)
{
    if(m == 2) return SAUER_NGX_MODE_QUALITY;
    if(m == 3) return SAUER_NGX_MODE_BALANCED;
    if(m == 4) return SAUER_NGX_MODE_PERFORMANCE;
    return SAUER_NGX_MODE_DLAA;
}

static int enginefromgwmode(uint32_t m)
{
    if(m == SAUER_NGX_MODE_QUALITY) return 2;
    if(m == SAUER_NGX_MODE_BALANCED) return 3;
    if(m == SAUER_NGX_MODE_PERFORMANCE) return 4;
    return 1;
}

static const char *ngxpresetname(int enginemode)
{
    if(enginemode == 4) return "M";
    return "K";
}

int hwrtfbw() { return (scenebound && scenew > 0) ? scenew : screenw; }
int hwrtfbh() { return (scenebound && sceneh > 0) ? sceneh : screenh; }
int hwrtrenderw() { return scenew > 0 ? scenew : screenw; }
int hwrtrenderh() { return sceneh > 0 ? sceneh : screenh; }
GLuint hwrtmainfbo() { return scenebound ? scenefbo : 0; }
bool hwrtsceneinternal() { return scenebound; }
int hwrtngxmoderequested() { return ngxmodereq; }
int hwrtngxmodeapplied() { return ngxmodeapp; }

extern int hdrout_lightfix_level();
extern int hdrout_skyfix_level();
extern int hdrout_colorfix_level();
extern float hdrout_colorfix_scale();
extern void hdrout_jour_params(float p[4], float tab[24]);
extern int hdrout_lightdbg();

void hwrthdrsetlin(bool targetlinear)
{
    bool lin = hdrframe && targetlinear;
    GLOBALPARAMF(hwrthdrlin, lin ? 1.0f : 0.0f);
    GLOBALPARAMF(hdrlightfix, lin ? float(hdrout_lightfix_level()) : 0.0f);
    GLOBALPARAMF(hdrskyfix, lin ? float(hdrout_skyfix_level()) : 0.0f);
    GLOBALPARAMF(hdrmatcol, lin ? hdrout_colorfix_scale() : 0.0f);
    float jp[4], jt[24];
    hdrout_jour_params(jp, jt);
    GLOBALPARAMF(hdrjour, lin ? jp[0] : 0.0f, jp[1], jp[2], jp[3]);
    GLOBALPARAMV(hdrjourt, jt, 24);
    GLOBALPARAMF(hdrlightdbg, float(hdrout_lightdbg()));
}

bool hwrthdrframe()
{
    return hdrframe;
}

bool hwrthdrpresented()
{
    return hdrpresented;
}

void hwrthdrsuspend()
{
    hwrthdrsetlin(false);
    hdrpresented = false;
}

void hwrtbindscenefb()
{
    glBindFramebuffer_(GL_FRAMEBUFFER, hwrtmainfbo());
    glViewport(0, 0, hwrtfbw(), hwrtfbh());
    hwrthdrsetlin(scenehdr && scenebound);
}

int hwrtdlaajitterseqlen()
{
    int rw = hwrtrenderw(), rh = hwrtrenderh();
    if(rw < 1 || rh < 1 || screenw < 1 || screenh < 1) return 16;
    float sx = float(screenw) / float(rw);
    float sy = float(screenh) / float(rh);
    float s = sx > sy ? sx : sy;
    if(s < 1.01f) return 16;
    int n = int(ceilf(8.0f * s * s));
    if(n < 8) n = 8;
    if(n > 32) n = 32;
    return n;
}

static void bindnativewindow()
{
    glBindFramebuffer_(GL_FRAMEBUFFER, 0);
    if(screenw >= 1 && screenh >= 1) glViewport(0, 0, screenw, screenh);
}

// Lot 5. Additive flames accumulate alone in an RGBA16F target that shares the
// scene depth-stencil, so depth test and stencil marks are those of the scene.
// renderparticles composes the target into the linear scene right after.
static GLuint flamefbo = 0, flamecolor = 0, flameds = 0;
static int flamew = 0, flameh = 0;
static bool flamevalid = false;

static void destroyflamefb()
{
    if(flamefbo) { glDeleteFramebuffers_(1, &flamefbo); flamefbo = 0; }
    if(flamecolor) { glDeleteTextures(1, &flamecolor); flamecolor = 0; }
    flameds = 0;
    flamew = flameh = 0;
    flamevalid = false;
}

bool hwrtflamebegin()
{
    flamevalid = false;
    if(!scenehdr || !scenebound || !scenefbo || !sceneds || scenew < 8 || sceneh < 8) return false;
    GLint cur = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &cur);
    if(GLuint(cur) != scenefbo) return false;
    if(!flamefbo || !flamecolor || flamew != scenew || flameh != sceneh || flameds != sceneds)
    {
        destroyflamefb();
        glGenTextures(1, &flamecolor);
        glBindTexture(GL_TEXTURE_2D, flamecolor);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, scenew, sceneh, 0, GL_RGBA, GL_FLOAT, NULL);
        glBindTexture(GL_TEXTURE_2D, 0);
        glGenFramebuffers_(1, &flamefbo);
        glBindFramebuffer_(GL_FRAMEBUFFER, flamefbo);
        glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, flamecolor, 0);
        glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, sceneds);
        GLenum st = glCheckFramebufferStatus_(GL_FRAMEBUFFER);
        glBindFramebuffer_(GL_FRAMEBUFFER, scenefbo);
        if(st != GL_FRAMEBUFFER_COMPLETE)
        {
            destroyflamefb();
            static int warned = 0;
            if(!warned) { warned = 1; conoutf(CON_ERROR, "hdr lot5: cible flammes incomplete (0x%x), flammes du lot4", unsigned(st)); }
            return false;
        }
        flamew = scenew;
        flameh = sceneh;
        flameds = sceneds;
        conoutf(CON_INIT, "hdr lot5: cible flammes RGBA16F %dx%d, profondeur de la scene", flamew, flameh);
    }
    glBindFramebuffer_(GL_FRAMEBUFFER, flamefbo);
    GLfloat cc[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    if(scissor) glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(cc[0], cc[1], cc[2], cc[3]);
    if(scissor) glEnable(GL_SCISSOR_TEST);
    flamevalid = true;
    return true;
}

void hwrtflameend()
{
    glBindFramebuffer_(GL_FRAMEBUFFER, scenefbo);
}

// Valid until the next hwrtflamebegin: the lab capture reads it after the compose.
GLuint hwrtflametex(int &w, int &h)
{
    w = flamevalid ? flamew : 0;
    h = flamevalid ? flameh : 0;
    return flamevalid ? flamecolor : 0;
}

static void destroyscenefb()
{
    destroyflamefb();
    // Quality binds a reduced FBO + viewport. Blitting to the window does not
    // restore glViewport. If we only clear scenebound here, hwrtsceneend
    // returns immediately and the HUD of this frame is drawn in the reduced
    // rectangle unless a later postfx pass happens to reset it.
    bindnativewindow();
    scenebound = false;
    scenehdr = false;
    hwrthdrsetlin(false);
    if(scenefbo) { glDeleteFramebuffers_(1, &scenefbo); scenefbo = 0; }
    if(scenecolor) { glDeleteTextures(1, &scenecolor); scenecolor = 0; }
    if(sceneds) { glDeleteRenderbuffers_(1, &sceneds); sceneds = 0; }
    scenew = sceneh = 0;
    scenecolorfmt = 0;
    qcacheok = false;
}

static bool ensurescenefb(int w, int h, GLenum fmt)
{
    if(scenefbo && scenecolor && sceneds && scenew == w && sceneh == h && scenecolorfmt == fmt) return true;
    if(w < 8 || h < 8 || !hasFBO) return false;
    destroyscenefb();
    if(!scenecolor) glGenTextures(1, &scenecolor);
    glBindTexture(GL_TEXTURE_2D, scenecolor);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLenum srcType = (fmt == GL_RGBA16F) ? GL_FLOAT : GL_UNSIGNED_BYTE;
    glTexImage2D(GL_TEXTURE_2D, 0, fmt, w, h, 0, GL_RGBA, srcType, NULL);
    if(!sceneds) glGenRenderbuffers_(1, &sceneds);
    glBindRenderbuffer_(GL_RENDERBUFFER, sceneds);
    glRenderbufferStorage_(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glBindRenderbuffer_(GL_RENDERBUFFER, 0);
    if(!scenefbo) glGenFramebuffers_(1, &scenefbo);
    glBindFramebuffer_(GL_FRAMEBUFFER, scenefbo);
    glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, scenecolor, 0);
    glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, sceneds);
    GLenum st = glCheckFramebufferStatus_(GL_FRAMEBUFFER);
    if(st != GL_FRAMEBUFFER_COMPLETE)
    {
        glBindFramebuffer_(GL_FRAMEBUFFER, 0);
        destroyscenefb();
        setreason("scene FBO incomplete");
        return false;
    }
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glBindFramebuffer_(GL_FRAMEBUFFER, 0);
    scenew = w;
    sceneh = h;
    scenecolorfmt = fmt;
    conoutf(CON_INIT, "hwrt scene: internal colour %s + depth-stencil %dx%d (output %dx%d)",
            fmt == GL_RGBA16F ? "RGBA16F" : "RGBA8", w, h, screenw, screenh);
    return true;
}

static bool querysrsize(int mode, int outw, int outh, int &inw, int &inh)
{
    inw = outw;
    inh = outh;
    if(qcacheok && qcacheoutw == outw && qcacheouth == outh && qcachemode == mode && qcacheinw >= 8 && qcacheinh >= 8)
    {
        inw = qcacheinw;
        inh = qcacheinh;
        return inw < outw || inh < outh;
    }
    if(fsrmode(mode))
    {
        if(!hwrtfsrquerysize(mode, outw, outh, inw, inh))
        {
            qcacheok = false;
            return false;
        }
        ngxoptw = ngxoptminw = ngxoptmaxw = uint32_t(inw);
        ngxopth = ngxoptminh = ngxoptmaxh = uint32_t(inh);
        ngxoptok = 1;
        qcacheoutw = outw;
        qcacheouth = outh;
        qcacheinw = inw;
        qcacheinh = inh;
        qcachemode = mode;
        qcacheok = inw >= 8 && inh >= 8 && (inw < outw || inh < outh);
        return qcacheok;
    }
    if(!p_gwOptimal || !ngxsupported) return false;
    SauerNgxOptimal q;
    memset(&q, 0, sizeof(q));
    q.struct_bytes = sizeof(q);
    q.out_w = uint32_t(outw);
    q.out_h = uint32_t(outh);
    q.mode = gwmodefromengine(mode);
    if(p_gwOptimal(&q, sizeof(q)) != SAUER_NGX_OK || !q.callback_ok || !q.opt_w || !q.opt_h)
    {
        gwcopyerr();
        qcacheok = false;
        return false;
    }
    ngxoptw = q.opt_w;
    ngxopth = q.opt_h;
    ngxoptminw = q.min_w;
    ngxoptminh = q.min_h;
    ngxoptmaxw = q.max_w;
    ngxoptmaxh = q.max_h;
    ngxoptok = 1;
    inw = int(q.opt_w);
    inh = int(q.opt_h);
    qcacheoutw = outw;
    qcacheouth = outh;
    qcacheinw = inw;
    qcacheinh = inh;
    qcachemode = mode;
    qcacheok = inw >= 8 && inh >= 8 && (inw < outw || inh < outh);
    return qcacheok;
}

extern int hwrt;

static bool hwrthdreligible()
{
    return hwrthdr != 0;
}

static bool hdrpresentready()
{
    // lookupshaderbyname hides a deferred shader. useshaderbyname compiles it.
    Shader *s = useshaderbyname("hdrpresent");
    return s && !s->invalid();
}

bool hwrthdractive()
{
    return hdrframe;
}

static void hwrthdrstatuscmd()
{
    conoutf("hdr lot2 status req %d frame %d presented %d faillab %d fam %d probe %d ngx_demande %d ngx_effectif %d IsHDR %d push 0x%x scenehdr %d scenecolorfmt 0x%x scene %dx%d",
            hwrthdr, hdrframe ? 1 : 0, hdrpresented ? 1 : 0, hwrthdrfaillab, hwrthdrfam, hwrthdrprobe,
            hwrtngxmode, ngxmodeapp, ngxcfghdr, hwrthdrpushmode, scenehdr ? 1 : 0, int(scenecolorfmt), scenew, sceneh);
}
ICOMMAND(hwrthdrstatus, "", (), hwrthdrstatuscmd());

GLuint hwrthdrscenecolor()
{
    // Still the linear scene after the SDR present. A later glare pass can
    // sample it; the window already holds the tone-mapped image.
    return (scenecolor && scenecolorfmt == GL_RGBA16F) ? scenecolor : 0;
}

static void hwrthdrreadcmd()
{
    hdrreadpending = 1;
    conoutf("hdr lot2: lecture GPU demandee");
}
ICOMMAND(hwrthdrread, "", (), hwrthdrreadcmd());

static const int HDR_STAMP_DX[4] = { 32, 88, 144, 200 };
static const char *HDR_STAMP_NAME[4] = { "0.18", "1", "4", "16" };

static void hdrstamporigin(int &x0, int &y0, int &yc)
{
    x0 = scenew - 240;
    y0 = sceneh - 70;
    yc = y0 + 24;
}

static void hwrthdrstampgl()
{
    if(!hwrthdrprobe || hwrt) return;
    Shader *s = useshaderbyname("hdrstamp");
    if(!s || s->invalid())
    {
        conoutf(CON_WARN, "hdr lot2: shader hdrstamp absent, sondes GL non ecrites");
        return;
    }
    int x0, y0, yc;
    hdrstamporigin(x0, y0, yc);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    s->set();
    LOCALPARAMF(hdrstamp, float(x0), float(y0), 0.0f, 0.0f);
    screenquad(1, 1);
}

void hwrthdrstampifprobe()
{
    if(!scenehdr || !scenefbo || !hwrthdrprobe || hwrt) return;
    GLboolean depth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean blend = glIsEnabled(GL_BLEND);
    GLboolean cull = glIsEnabled(GL_CULL_FACE);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    hwrthdrstampgl();
    if(depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if(blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if(cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if(scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
}

static void hwrthdrlogpixel(const char *where, int i, int x, int y, const float *rgba, int err, bool ok)
{
    conoutf("hdr lot2 %s %s (x %d y %d) %s %.6f %.6f %.6f %.6f err 0x%x",
            where, HDR_STAMP_NAME[i], x, y,
            ok ? "ok" : "FAIL", rgba[0], rgba[1], rgba[2], rgba[3], err);
}

static void hwrthdrsample(bool aftertonemap)
{
    if(!aftertonemap)
    {
        GLint scenefmt = 0;
        glBindTexture(GL_TEXTURE_2D, scenecolor);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &scenefmt);
        glBindTexture(GL_TEXTURE_2D, 0);
        int rtfmt = hwrtresultglformat();
        conoutf("hdr lot2 formats scene_gl 0x%x (RGBA16F=0x881A) rt_gl 0x%x vk R16G16B16A16_SFLOAT exp_stops %.3f mul %.5f hwrt %d ngx %d probe %d push 0x%x",
                int(scenefmt), rtfmt, hwrthdrexp, powf(2.0f, hwrthdrexp), hwrt, hwrtngxmode, hwrthdrprobe, hwrthdrpushmode);
        int x0, y0, yc;
        hdrstamporigin(x0, y0, yc);
        int rtw = 0, rth = 0;
        hwrtresultsize(&rtw, &rth);
        if(hwrt && rtw >= 8 && rth >= 8 && rtw <= 4096 && rth <= 4096)
        {
            int rw = 0, rh = 0, err = 0;
            float *rt = new float[size_t(rtw) * size_t(rth) * 4];
            bool ok = hwrtcopyresult(rt, &rw, &rh, &err);
            if(ok && (rw != rtw || rh != rth)) ok = false;
            loopi(4)
            {
                float rgba[4] = { -1, -1, -1, -1 };
                int x = x0 + HDR_STAMP_DX[i];
                if(ok && x >= 0 && yc >= 0 && x < rw && yc < rh)
                {
                    float *p = &rt[(size_t(yc) * rw + x) * 4];
                    rgba[0] = p[0]; rgba[1] = p[1]; rgba[2] = p[2]; rgba[3] = p[3];
                }
                hwrthdrlogpixel(ok ? "rt" : "rt_copy", i, x, yc, rgba, err, ok);
            }
            if(ok)
            {
                int fx = -1, fy = -1;
                float best = 1e9f, br = 0, bg = 0, bb = 0;
                for(int y = 0; y < rh; y += 2)
                for(int x = 0; x < rw; x += 2)
                {
                    float *p = &rt[(size_t(y) * rw + x) * 4];
                    float d = fabsf(p[0] - 16.f) + fabsf(p[1] - 16.f) + fabsf(p[2] - 16.f);
                    if(d < best) { best = d; fx = x; fy = y; br = p[0]; bg = p[1]; bb = p[2]; }
                }
                conoutf("hdr lot2 rt_scan closest_to_16 at %d %d value %.4f %.4f %.4f dist %.4f", fx, fy, br, bg, bb, best);
            }
            delete[] rt;
        }
        else
        {
            float miss[4] = { -1, -1, -1, -1 };
            loopi(4) hwrthdrlogpixel("rt_off", i, x0 + HDR_STAMP_DX[i], yc, miss, 0, false);
        }
        glBindFramebuffer_(GL_FRAMEBUFFER, scenefbo);
        while(glGetError() != GL_NO_ERROR);
        loopi(4)
        {
            float rgba[4] = { -1, -1, -1, -1 };
            glReadPixels(x0 + HDR_STAMP_DX[i], yc, 1, 1, GL_RGBA, GL_FLOAT, rgba);
            int err = int(glGetError());
            hwrthdrlogpixel("scene", i, x0 + HDR_STAMP_DX[i], yc, rgba, err, err == 0);
        }
        if(scenew >= 8 && sceneh >= 8 && scenew <= 4096 && sceneh <= 4096)
        {
            float *buf = new float[size_t(scenew) * size_t(sceneh) * 4];
            while(glGetError() != GL_NO_ERROR);
            glReadPixels(0, 0, scenew, sceneh, GL_RGBA, GL_FLOAT, buf);
            int err = int(glGetError());
            float maxl = -1.0f, maxr = 0, maxg = 0, maxb = 0;
            int maxx = -1, maxy = -1;
            if(err == 0)
            {
                for(int y = 0; y < sceneh; y++)
                for(int x = 0; x < scenew; x++)
                {
                    if(x >= scenew - 240 && y >= sceneh - 70) continue;
                    if(y < 24 && x < 48) continue;
                    const float *p = &buf[(size_t(y) * scenew + x) * 4];
                    float l = p[0] > p[1] ? p[0] : p[1];
                    if(p[2] > l) l = p[2];
                    if(l > maxl) { maxl = l; maxr = p[0]; maxg = p[1]; maxb = p[2]; maxx = x; maxy = y; }
                }
            }
            delete[] buf;
            conoutf("hdr lot2 scene_max %s %.6f %.6f %.6f luma %.6f at %d %d (hors rectangle sondes) err 0x%x",
                    err == 0 ? "ok" : "FAIL", maxr, maxg, maxb, maxl, maxx, maxy, err);
        }
    }
    else
    {
        int x0, y0, yc;
        hdrstamporigin(x0, y0, yc);
        glBindFramebuffer_(GL_FRAMEBUFFER, 0);
        while(glGetError() != GL_NO_ERROR);
        loopi(4)
        {
            float rgba[4] = { -1, -1, -1, -1 };
            glReadPixels(x0 + HDR_STAMP_DX[i], yc, 1, 1, GL_RGBA, GL_FLOAT, rgba);
            int err = int(glGetError());
            hwrthdrlogpixel("window", i, x0 + HDR_STAMP_DX[i], yc, rgba, err, err == 0);
        }
        if(getlogfile()) fflush(getlogfile());
        conoutf("hdr lot2 read_done");
    }
}

static float srgb_eotf(float c)
{
    return c <= 0.04045f ? c * (1.0f / 12.92f) : powf((c + 0.055f) * (1.0f / 1.055f), 2.4f);
}

static void hwrthdrfamsrc(const char *name)
{
    Shader *s = useshaderbyname(name);
    bool ok = s && !s->invalid() && s->psstr && strstr(s->psstr, "hwrthdrlin") && strstr(s->psstr, "1.055");
    conoutf("hdr lot2 inspection %s %s", name, ok ? "texte contient hwrthdrlin et 1.055" : "ABSENT");
}

static void hwrthdrfamdraw(int x, int y, float op, float ambient, float scale, float sr, float sg, float sb, float sa, float lm, float ndotl)
{
    glScissor(x, y, 1, 1);
    LOCALPARAMF(hdrfam, op, ambient, scale, 0.0f);
    LOCALPARAMF(hdrsrc, sr, sg, sb, sa);
    LOCALPARAMF(hdrlight, lm, lm, lm, ndotl);
    screenquad(1, 1);
}

static void hwrthdrfamread(const char *tag, int x, int y, float expect)
{
    float rgba[4] = { -1, -1, -1, -1 };
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_FLOAT, rgba);
    int err = int(glGetError());
    float e = fabsf(rgba[0] - expect);
    bool ok = err == 0 && e <= 0.002f && fabsf(rgba[1] - expect) <= 0.002f && fabsf(rgba[2] - expect) <= 0.002f;
    conoutf("hdr lot2 probe %s (helper hdrfamprobe, pas stdworld) (x %d y %d) %s got %.6f %.6f %.6f expect %.6f errabs %.6f gl 0x%x",
            tag, x, y, ok ? "ok" : "FAIL", rgba[0], rgba[1], rgba[2], expect, e, err);
}

static void hwrthdrfamrun()
{
    hwrthdrfamsrc("stdworld");
    hwrthdrfamsrc("alphaworld");
    hwrthdrfamsrc("bumpspecmapworld");
    hwrthdrfamsrc("bumpspecmapparallaxworld");
    hwrthdrfamsrc("particle");
    hwrthdrfamsrc("water");
    hwrthdrfamsrc("fogged");
    hwrthdrfamsrc("grass");
    Shader *s = useshaderbyname("hdrfamprobe");
    if(!s || s->invalid())
    {
        conoutf(CON_WARN, "hdr lot2 fam_src hdrfamprobe MISSING");
        return;
    }
    GLboolean depth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean blend = glIsEnabled(GL_BLEND);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLint bsrc = GL_ONE, bdst = GL_ZERO;
    glGetIntegerv(GL_BLEND_SRC, &bsrc);
    glGetIntegerv(GL_BLEND_DST, &bdst);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glEnable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    s->set();
    float c128 = 128.0f / 255.0f, c186 = 186.0f / 255.0f;
    float l128 = srgb_eotf(c128), l186 = srgb_eotf(c186);
    hwrthdrfamdraw(8, 8, 0.0f, 0.0f, 2.0f, c128, c128, c128, 1.0f, 1.0f, 1.0f);
    hwrthdrfamdraw(16, 8, 1.0f, 0.0f, 2.0f, c128, c128, c128, 1.0f, 1.0f, 1.0f);
    hwrthdrfamdraw(24, 8, 0.0f, 0.0f, 2.0f, c128, c128, c128, 1.0f, 0.5f, 1.0f);
    hwrthdrfamdraw(32, 8, 1.0f, 0.25f, 2.0f, c186, c186, c186, 1.0f, 0.5f, 0.0f);
    hwrthdrfamdraw(40, 8, 0.0f, 0.0f, 2.0f, c186, c186, c186, 1.0f, 1.0f, 1.0f);
    glDisable(GL_BLEND);
    hwrthdrfamdraw(8, 16, 4.0f, 0.0f, 1.0f, 0.25f, 0.25f, 0.25f, 1.0f, 1.0f, 1.0f);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    hwrthdrfamdraw(8, 16, 2.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.5f, 1.0f, 1.0f);
    glDisable(GL_BLEND);
    hwrthdrfamdraw(16, 16, 4.0f, 0.0f, 1.0f, 0.25f, 0.25f, 0.25f, 1.0f, 1.0f, 1.0f);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    hwrthdrfamdraw(16, 16, 2.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.5f, 1.0f, 1.0f);
    glDisable(GL_BLEND);
    hwrthdrfamdraw(24, 16, 4.0f, 0.0f, 1.0f, 0.25f, 0.25f, 0.25f, 1.0f, 1.0f, 1.0f);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    hwrthdrfamdraw(24, 16, 3.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.5f, 1.0f, 1.0f);
    glBindFramebuffer_(GL_FRAMEBUFFER, scenefbo);
    while(glGetError() != GL_NO_ERROR);
    hwrthdrfamread("probe_std_128", 8, 8, l128 * 2.0f);
    hwrthdrfamread("probe_bump_128", 16, 8, l128 * 2.0f);
    hwrthdrfamread("probe_std_lmhalf", 24, 8, l128 * 2.0f * 0.5f);
    hwrthdrfamread("probe_bump_ambient", 32, 8, l186 * 2.0f * 0.25f);
    hwrthdrfamread("probe_std_186", 40, 8, l186 * 2.0f);
    hwrthdrfamread("straight", 8, 16, 0.625f);
    hwrthdrfamread("additive", 16, 16, 0.75f);
    hwrthdrfamread("premul", 24, 16, 0.625f);
    if(scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if(blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    glBlendFunc(bsrc, bdst);
    if(depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
}

extern Shader *particleshader;

static GLuint hdrrealtex[5];

static void hwrthdrrealtexensure()
{
    if(hdrrealtex[0]) return;
    glGenTextures(5, hdrrealtex);
    const uchar pix[5][4] = {
        { 128, 128, 128, 0 },
        { 255, 255, 255, 255 },
        { 128, 128, 255, 255 },
        { 128, 128, 255, 255 },
        { 128, 128, 128, 128 }
    };
    loopi(5)
    {
        glBindTexture(GL_TEXTURE_2D, hdrrealtex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pix[i]);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

static void hwrthdrrealoverride(GLuint prog, int kind)
{
    float id[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    GLint loc = glGetUniformLocation_(prog, "camprojmatrix");
    if(loc >= 0) glUniformMatrix4fv_(loc, 1, GL_FALSE, id);
    loc = glGetUniformLocation_(prog, "fogparams");
    if(loc >= 0) glUniform2f_(loc, 0.0f, 1.0f);
    loc = glGetUniformLocation_(prog, "texgenscroll");
    if(loc >= 0) glUniform2f_(loc, 0.0f, 0.0f);
    if(kind == 2)
    {
        loc = glGetUniformLocation_(prog, "colorscale");
        if(loc >= 0) glUniform4f_(loc, 1.0f, 1.0f, 1.0f, 1.0f);
        loc = glGetUniformLocation_(prog, "particlemod");
        if(loc >= 0) glUniform4f_(loc, 0.0f, 0.0f, 0.0f, 0.0f);
        loc = glGetUniformLocation_(prog, "tex0");
        if(loc >= 0) glUniform1i_(loc, 0);
    }
    else
    {
        loc = glGetUniformLocation_(prog, "colorparams");
        if(loc >= 0) glUniform4f_(loc, 2.0f, 2.0f, 2.0f, 1.0f);
        loc = glGetUniformLocation_(prog, "diffusemap");
        if(loc >= 0) glUniform1i_(loc, 0);
        if(kind == 0)
        {
            loc = glGetUniformLocation_(prog, "lightmap");
            if(loc >= 0) glUniform1i_(loc, 1);
        }
        else
        {
            loc = glGetUniformLocation_(prog, "lmcolor");
            if(loc >= 0) glUniform1i_(loc, 1);
            loc = glGetUniformLocation_(prog, "lmdir");
            if(loc >= 0) glUniform1i_(loc, 2);
            loc = glGetUniformLocation_(prog, "normalmap");
            if(loc >= 0) glUniform1i_(loc, 3);
            loc = glGetUniformLocation_(prog, "specscale");
            if(loc >= 0) glUniform4f_(loc, 0.0f, 0.0f, 0.0f, 0.0f);
            loc = glGetUniformLocation_(prog, "ambient");
            if(loc >= 0) glUniform4f_(loc, 0.0f, 0.0f, 0.0f, 0.0f);
        }
    }
}

static void hwrthdrrealquad()
{
    gle::defvertex(2);
    gle::deftexcoord0();
    gle::deftexcoord1();
    gle::begin(GL_TRIANGLE_STRIP);
    gle::attribf(1, -1); gle::attribf(0.5f, 0.5f); gle::attribf(0.5f, 0.5f);
    gle::attribf(-1, -1); gle::attribf(0.5f, 0.5f); gle::attribf(0.5f, 0.5f);
    gle::attribf(1, 1); gle::attribf(0.5f, 0.5f); gle::attribf(0.5f, 0.5f);
    gle::attribf(-1, 1); gle::attribf(0.5f, 0.5f); gle::attribf(0.5f, 0.5f);
    gle::end();
}

static void hwrthdrreallog(const char *tag, Shader *s, int x, int y, float expect)
{
    GLint cur = 0;
    glGetIntegerv(0x8B8D, &cur); /* GL_CURRENT_PROGRAM */
    Shader *detail = s && s->detailshader ? s->detailshader : s;
    int linloc = -1;
    float lin = -1.0f;
    if(cur)
    {
        linloc = glGetUniformLocation_(GLuint(cur), "hwrthdrlin");
        if(linloc >= 0)
        {
            typedef void (APIENTRY *hdrgetuniformfv)(GLuint program, GLint location, GLfloat *params);
            static hdrgetuniformfv getu = NULL;
            if(!getu) getu = (hdrgetuniformfv)getprocaddress("glGetUniformfv");
            if(getu) getu(GLuint(cur), linloc, &lin);
        }
    }
    GLint bound[4] = { 0, 0, 0, 0 };
    GLint fmt0 = 0;
    loopi(4)
    {
        glActiveTexture_(GL_TEXTURE0 + i);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound[i]);
    }
    if(bound[0])
    {
        glBindTexture(GL_TEXTURE_2D, bound[0]);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &fmt0);
    }
    glActiveTexture_(GL_TEXTURE0);
    float rgba[4] = { -1, -1, -1, -1 };
    while(glGetError() != GL_NO_ERROR);
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_FLOAT, rgba);
    int err = int(glGetError());
    float e = fabsf(rgba[0] - expect);
    bool ok = err == 0 && e <= 0.002f && fabsf(rgba[1] - expect) <= 0.002f && fabsf(rgba[2] - expect) <= 0.002f;
    conoutf("hdr lot2 real %s programme %u detail %u courant %d shader %s hwrthdrlin loc %d val %.3f tex %d %d %d %d fmt0 0x%x (x %d y %d) %s got %.6f %.6f %.6f expect %.6f errabs %.6f gl 0x%x",
            tag,
            s ? (unsigned)s->program : 0u,
            detail ? (unsigned)detail->program : 0u,
            int(cur),
            s && s->name ? s->name : "?",
            linloc, lin,
            int(bound[0]), int(bound[1]), int(bound[2]), int(bound[3]), int(fmt0),
            x, y, ok ? "ok" : "FAIL",
            rgba[0], rgba[1], rgba[2], expect, e, err);
    if(tag[0] == 'p')
        conoutf("hdr lot2 real particle pointeur_jeu %s", (particleshader && s == particleshader) ? "identique" : "different");
}

static void hwrthdrrealrun()
{
    if(!scenefbo || scenew < 32 || sceneh < 40) return;
    hwrthdrrealtexensure();
    hwrthdrsetlin(true);
    Shader *stds = stdworldshader ? stdworldshader : lookupshaderbyname("stdworld");
    Shader *bumps = lookupshaderbyname("bumpspecmapworld");
    Shader *parts = particleshader ? particleshader : lookupshaderbyname("particle");
    if(stds) stds = useshaderbyname("stdworld");
    if(bumps) bumps = useshaderbyname("bumpspecmapworld");
    if(parts) parts = useshaderbyname(parts->name ? parts->name : "particle");
    GLboolean depth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean blend = glIsEnabled(GL_BLEND);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLint bsrc = GL_ONE, bdst = GL_ZERO;
    glGetIntegerv(GL_BLEND_SRC, &bsrc);
    glGetIntegerv(GL_BLEND_DST, &bdst);
    glBindFramebuffer_(GL_FRAMEBUFFER, scenefbo);
    glViewport(0, 0, scenew, sceneh);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glEnable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    float c128 = 128.0f / 255.0f;
    float l128 = srgb_eotf(c128);
    float stdexpect = l128 * 2.0f;
    if(stds && !stds->invalid())
    {
        stds->set();
        GLint cur = 0;
        glGetIntegerv(0x8B8D, &cur);
        hwrthdrrealoverride(GLuint(cur), 0);
        glActiveTexture_(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, hdrrealtex[0]);
        glActiveTexture_(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, hdrrealtex[1]);
        glActiveTexture_(GL_TEXTURE0);
        glScissor(8, 32, 1, 1);
        hwrthdrrealquad();
        hwrthdrreallog("stdworld", stds, 8, 32, stdexpect);
    }
    else conoutf(CON_WARN, "hdr lot2 real stdworld absent");
    if(bumps && !bumps->invalid())
    {
        bumps->set();
        GLint cur = 0;
        glGetIntegerv(0x8B8D, &cur);
        hwrthdrrealoverride(GLuint(cur), 1);
        glActiveTexture_(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, hdrrealtex[0]);
        glActiveTexture_(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, hdrrealtex[1]);
        glActiveTexture_(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, hdrrealtex[2]);
        glActiveTexture_(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, hdrrealtex[3]);
        glActiveTexture_(GL_TEXTURE0);
        glScissor(16, 32, 1, 1);
        hwrthdrrealquad();
        hwrthdrreallog("bumpspecmapworld", bumps, 16, 32, stdexpect);
    }
    else conoutf(CON_WARN, "hdr lot2 real bumpspecmapworld absent");
    float a = 128.0f / 255.0f;
    float src = l128 * l128;
    float partexpect = 0.25f * (1.0f - a) + src * a;
    if(parts && !parts->invalid())
    {
        glScissor(24, 32, 1, 1);
        glClearColor(0.25f, 0.25f, 0.25f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        parts->set();
        GLint cur = 0;
        glGetIntegerv(0x8B8D, &cur);
        hwrthdrrealoverride(GLuint(cur), 2);
        glActiveTexture_(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, hdrrealtex[4]);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        gle::color(bvec(128, 128, 128), 255);
        hwrthdrrealquad();
        hwrthdrreallog("particle", parts, 24, 32, partexpect);
    }
    else conoutf(CON_WARN, "hdr lot2 real particle absent");
    glActiveTexture_(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture_(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture_(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture_(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture_(GL_TEXTURE0);
    if(scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if(blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    glBlendFunc(bsrc, bdst);
    if(depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glBindFramebuffer_(GL_FRAMEBUFFER, scenefbo);
}

static void hwrthdrscheduleread()
{
    static int hdrframes = 0;
    hdrframes++;
    if(hwrthdrprobe && (hdrframes == 90 || hdrframes == 150)) hdrreadpending = 1;
}

static bool hwrthdrtonemap(GLuint tex, bool yflip)
{
    static int sdrnoted = 0;
    if(hdrout_compose(tex, yflip))
    {
        sdrnoted = 0;
        return true;
    }
    if(!sdrnoted)
    {
        sdrnoted = 1;
        conoutf("hdrout repli: image SDR composee par hdrpresent (ACES puis sRGB) dans le framebuffer fenetre. Pas de copie des valeurs scRGB.");
    }
    glBindFramebuffer_(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, screenw, screenh);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    Shader *s = useshaderbyname("hdrpresent");
    if(!s || s->invalid() || !tex)
    {
        conoutf(CON_ERROR, "hdr lot2: shader hdrpresent absent, scene non affichee");
        glClearColor(0.4f, 0.0f, 0.2f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        return false;
    }
    s->set();
    glActiveTexture_(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    LOCALPARAMF(hdrexp, powf(2.0f, hwrthdrexp), yflip ? 1.0f : 0.0f, 0.0f, 0.0f);
    screenquad(1, 1);
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

static void hwrthdrpresentedone()
{
    scenebound = false;
    scenehdr = false;
    hdrpresented = true;
    hwrthdrsetlin(false);
    if(screenw >= 1 && screenh >= 1) glViewport(0, 0, screenw, screenh);
}

static void hwrthdrpresent()
{
    if(!scenehdr || !scenefbo || !scenecolor) return;
    hwrthdrscheduleread();
    hwrthdrstampgl();
    bool doread = hdrreadpending != 0;
    if(doread && hwrthdrfam) hwrthdrfamrun();
    if(doread) hwrthdrrealrun();
    if(doread) hwrthdrsample(false);
    hwrthdrtonemap(scenecolor, false);
    if(doread)
    {
        hwrthdrsample(true);
        hdrreadpending = 0;
    }
    hwrthdrpresentedone();
}

// NVIDIA DLSS Programming Guide 3.5: while DLAA/DLSS renders the scene, scene
// textures get the mip-map bias log2(render width / display width) - 1 (DLAA -1,
// Quality about -1.58, Performance -2). FSR and Native keep 0. Off by default
// since 2026-10-04 (tester report: DLSS less stable, aliasing even in DLAA with it):
// 0 leaves every texture at bias 0 (no GL call), as before the guide pass.
// Not saved; 1 stays available to compare.
VAR(hwrtdlssmipbias, 0, 0, 1);
static void updatescenelodbias()
{
    float bias = 0;
    int m = hwrtngxmode;
    if(hwrtdlssmipbias && m >= 1 && m <= 4 && !ngxblocked && modeready(m) && screenw >= 8 && screenh >= 8)
    {
        int inw = screenw, inh = screenh;
        if(ngxissrmode(m) && !querysrsize(m, screenw, screenh, inw, inh)) inw = 0;
        if(inw > 0) bias = log2f(float(inw) / float(screenw)) - 1.0f;
    }
    setscenelodbias(bias);
}

void hwrtscenebegin()
{
    hdrframe = false;
    hdrpresented = false;
    flamevalid = false;
    scenebound = false;
    scenehdr = false;
    scenefail = false;
    hwrthdrsetlin(false);
    if(!initing && !inchanged && !ngxblocked && ngxmodehave != hwrtngxmode)
    {
        ngxautoapply = true;   // saved choice applied on the first frame, not a new request
        applyngxmode(hwrtngxmode);
        ngxautoapply = false;
    }
    ngxmodereq = hwrtngxmode;
    updatescenelodbias();
    int wantw = screenw, wanth = screenh;
    if(hwrthdreligible() && screenw >= 8 && screenh >= 8)
    {
        int fw = screenw, fh = screenh;
        if(!ngxblocked && ngxissrmode(hwrtngxmode) && modeready(hwrtngxmode))
        {
            int inw = 0, inh = 0;
            if(querysrsize(hwrtngxmode, screenw, screenh, inw, inh))
            {
                fw = inw;
                fh = inh;
            }
        }
        int cause = 0;
        if(hwrthdrfaillab) cause = 1;
        else if(!hdrpresentready()) cause = 2;
        else if(!ensurescenefb(fw, fh, GL_RGBA16F)) cause = 3;
        static int loggedcause = 0;
        if(cause)
        {
            if(loggedcause != cause)
            {
                loggedcause = cause;
                const char *why = cause == 1 ? "injection laboratoire" : cause == 2 ? "shader hdrpresent absent" : "FBO scene RGBA16F incomplet";
                conoutf(CON_ERROR, "hdr lot2: repli LDR coherent (%s). hwrthdr %d conserve.", why, hwrthdr);
            }
        }
        else
        {
            loggedcause = 0;
            glBindFramebuffer_(GL_FRAMEBUFFER, scenefbo);
            glViewport(0, 0, fw, fh);
            scenebound = true;
            scenehdr = true;
            hdrframe = true;
            hwrthdrsetlin(true);
            lastscenew = fw;
            lastsceneh = fh;
            lastfbw = fw;
            lastfbh = fh;
            return;
        }
    }
    if(ngxissrmode(hwrtngxmode) && !ngxblocked && modeready(hwrtngxmode) && screenw >= 8 && screenh >= 8)
    {
        int inw = 0, inh = 0;
        if(!querysrsize(hwrtngxmode, screenw, screenh, inw, inh))
        {
            scenefail = true;
            if(!ngxreason[0]) setreason("SR GET_OPTIMAL_SETTINGS failed");
        }
        else if(!ensurescenefb(inw, inh, GL_RGBA8))
            scenefail = true;
        else
        {
            glBindFramebuffer_(GL_FRAMEBUFFER, scenefbo);
            glViewport(0, 0, inw, inh);
            scenebound = true;
            lastscenew = inw;
            lastsceneh = inh;
            lastfbw = inw;
            lastfbh = inh;
            return;
        }
    }
    destroyscenefb();
    scenew = wantw;
    sceneh = wanth;
    glBindFramebuffer_(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, screenw, screenh);
    lastscenew = scenew;
    lastsceneh = sceneh;
    lastfbw = screenw;
    lastfbh = screenh;
}

void hwrtsceneend()
{
    if(scenehdr && !hdrpresented) hwrthdrpresent();
    else if(scenebound)
    {
        glBindFramebuffer_(GL_FRAMEBUFFER, 0);
        scenebound = false;
    }
    if(screenw >= 1 && screenh >= 1) glViewport(0, 0, screenw, screenh);
}

static int biaspersistgen = -1;

static void biascaminvalidate()
{
    biasprevcamvalid = 0;
}

static bool biascamerastill()
{
    if(!biasprevcamvalid || !camera1) return false;
    float dy = camera1->yaw - biasprevyaw;
    while(dy > 180) dy -= 360;
    while(dy < -180) dy += 360;
    float dp = camera1->pitch - biasprevpitch;
    // Screen-space persist is only valid if this frame's view matches the
    // previous mask. 0.45 deg/frame lab spin is already above this gate.
    return fabsf(dy) < 0.2f && fabsf(dp) < 0.2f && camera1->o.dist(biasprevpos) < 0.15f;
}

static void biascamstore()
{
    if(!camera1)
    {
        biascaminvalidate();
        return;
    }
    biasprevpos = camera1->o;
    biasprevyaw = camera1->yaw;
    biasprevpitch = camera1->pitch;
    biasprevcamvalid = 1;
}

static void harvestbiascost()
{
    if(!biascostq || !biascostpending || !glGetQueryObjectiv_ || !glGetQueryObjectuiv_) return;
    GLint avail = 0;
    glGetQueryObjectiv_(biascostq, GL_QUERY_RESULT_AVAILABLE, &avail);
    if(!avail) return;
    GLuint ns = 0;
    glGetQueryObjectuiv_(biascostq, GL_QUERY_RESULT, &ns);
    lastbiasgpums = ns / 1.0e6f;
    biascostpending = false;
}

struct biasglprev
{
    GLint fb, readfb, drawfb, vp[4], drawbuf, depthfunc, blendeq, active, tex0;
    GLboolean depth, blend, cull, scissor, depthmask, colormask[4];
};

static void savebiasglprev(biasglprev &p)
{
    memset(&p, 0, sizeof(p));
    p.drawbuf = GL_BACK;
    p.depthfunc = GL_LESS;
    p.blendeq = GL_FUNC_ADD;
    p.active = GL_TEXTURE0;
    p.depthmask = GL_TRUE;
    loopi(4) p.colormask[i] = GL_TRUE;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &p.fb);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &p.readfb);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &p.drawfb);
    glGetIntegerv(GL_VIEWPORT, p.vp);
    glGetIntegerv(GL_DRAW_BUFFER, &p.drawbuf);
    glGetIntegerv(GL_DEPTH_FUNC, &p.depthfunc);
    glGetIntegerv(GL_BLEND_EQUATION, &p.blendeq);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &p.active);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &p.tex0);
    p.depth = glIsEnabled(GL_DEPTH_TEST);
    p.blend = glIsEnabled(GL_BLEND);
    p.cull = glIsEnabled(GL_CULL_FACE);
    p.scissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &p.depthmask);
    glGetBooleanv(GL_COLOR_WRITEMASK, p.colormask);
}

static void restorebiasglprev(const biasglprev &p)
{
    glBindFramebuffer_(GL_FRAMEBUFFER, p.fb);
    if(p.drawbuf) glDrawBuffer(GLenum(p.drawbuf));
    else glDrawBuffer(p.fb ? GL_COLOR_ATTACHMENT0 : GL_BACK);
    glBindFramebuffer_(GL_READ_FRAMEBUFFER, p.readfb);
    glBindFramebuffer_(GL_DRAW_FRAMEBUFFER, p.drawfb);
    glViewport(p.vp[0], p.vp[1], p.vp[2], p.vp[3]);
    glDepthMask(p.depthmask);
    glDepthFunc(GLenum(p.depthfunc));
    glColorMask(p.colormask[0], p.colormask[1], p.colormask[2], p.colormask[3]);
    if(glBlendEquation_) glBlendEquation_(GLenum(p.blendeq ? p.blendeq : GL_FUNC_ADD));
    if(p.depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if(p.blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if(p.cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if(p.scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if(glActiveTexture_)
    {
        glActiveTexture_(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, p.tex0);
        glActiveTexture_(GLenum(p.active ? p.active : GL_TEXTURE0));
    }
}

static void clearbiasgl()
{
    if(!biasfbo || !hasFBO) return;
    biasglprev prev;
    savebiasglprev(prev);
    glBindFramebuffer_(GL_FRAMEBUFFER, biasfbo);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    loopi(2)
    {
        if(!biasgl[i]) continue;
        glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, biasgl[i], 0);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, biasgl[biascurr], 0);
    restorebiasglprev(prev);
}

void hwrtdlaabiasreset()
{
    clearbiasgl();
    biaspersistgen = int(hwrtdlaabias);
    biascaminvalidate();
    lastbiasgl = 0;
    lastbiasbound = 0;
    lastbiaspersistused = 0;
}

static void hwrtdlaabiaschanged()
{
    if(initing) return;
    hwrtdlaabiasreset();
}

static void destroybiasgl()
{
    if(biasfbo) { glDeleteFramebuffers_(1, &biasfbo); biasfbo = 0; }
    loopi(2) if(biasgl[i]) { glDeleteTextures(1, &biasgl[i]); biasgl[i] = 0; }
    if(biasds) { glDeleteRenderbuffers_(1, &biasds); biasds = 0; }
    biasglw = biasglh = 0;
    biascurr = 0;
    lastbiasgl = 0;
    lastbiasbound = 0;
    biaspersistgen = -1;
    biascaminvalidate();
}

static bool ensurebiasgl(int w, int h, int *recreated)
{
    if(recreated) *recreated = 0;
    if(biasfbo && biasgl[0] && biasgl[1] && biasds && biasglw == w && biasglh == h) return true;
    GLint keepfb = 0, keepread = 0, keepdraw = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &keepfb);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &keepread);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &keepdraw);
    destroybiasgl();
    if(w < 8 || h < 8 || !hasFBO)
    {
        glBindFramebuffer_(GL_FRAMEBUFFER, keepfb);
        return false;
    }
    loopi(2)
    {
        glGenTextures(1, &biasgl[i]);
        glBindTexture(GL_TEXTURE_2D, biasgl[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenRenderbuffers_(1, &biasds);
    glBindRenderbuffer_(GL_RENDERBUFFER, biasds);
    glRenderbufferStorage_(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glBindRenderbuffer_(GL_RENDERBUFFER, 0);
    glGenFramebuffers_(1, &biasfbo);
    glBindFramebuffer_(GL_FRAMEBUFFER, biasfbo);
    glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, biasds);
    loopi(2)
    {
        glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, biasgl[i], 0);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, biasgl[0], 0);
    GLenum st = glCheckFramebufferStatus_(GL_FRAMEBUFFER);
    if(st != GL_FRAMEBUFFER_COMPLETE)
    {
        destroybiasgl();
        glBindFramebuffer_(GL_FRAMEBUFFER, keepfb);
        glBindFramebuffer_(GL_READ_FRAMEBUFFER, keepread);
        glBindFramebuffer_(GL_DRAW_FRAMEBUFFER, keepdraw);
        return false;
    }
    // Leave the caller's FBO bound. Binding only READ/DRAW here used to
    // disagree with GL_FRAMEBUFFER on the recreate path.
    glBindFramebuffer_(GL_FRAMEBUFFER, keepfb);
    glBindFramebuffer_(GL_READ_FRAMEBUFFER, keepread);
    glBindFramebuffer_(GL_DRAW_FRAMEBUFFER, keepdraw);
    biasglw = w;
    biasglh = h;
    biascurr = 0;
    if(recreated) *recreated = 1;
    return true;
}

void hwrtdlaabiascapture()
{
    lastbiasgl = 0;
    lastbiasbound = 0;
    lastbiasmode = int(hwrtdlaabias);
    lastbiasrecreate = 0;
    lastbiaspersistused = 0;
    lastbiasneeds = hwrtdlaaneedsdata() ? 1 : 0;
    lastbiassrcfb = 0;
    lastbiasw = 0;
    lastbiash = 0;
    if(hwrtdlaabias <= 0 || !hwrtdlaaneedsdata() || !fsrmode(hwrtngxmode))
    {
        lastbiasgpums = 0;
        return;
    }
    if(drawtex)
    {
        lastbiasgpums = 0;
        return;
    }

    // Save the scene FBO before any allocation. ensurebiasgl used to finish
    // on framebuffer 0, so a recreate copied window depth and restored the
    // wrong target. hwrtgenvelocity rebinds the scene FBO later; that does
    // not fix the depth already used for this mask.
    biasglprev prev;
    savebiasglprev(prev);
    lastbiassrcfb = int(prev.fb);

    int w = hwrtrenderw(), h = hwrtrenderh();
    lastbiasw = w;
    lastbiash = h;
    if(w < 8 || h < 8)
    {
        restorebiasglprev(prev);
        return;
    }

    harvestbiascost();
    int recreated = 0;
    if(!ensurebiasgl(w, h, &recreated))
    {
        restorebiasglprev(prev);
        return;
    }
    lastbiasrecreate = recreated;
    if(recreated)
        conoutf("hwrt ngx: bias mask alloc %dx%d src_fb %u (depth blit from this FBO)", w, h, (unsigned)prev.fb);

    if(biaspersistgen != int(hwrtdlaabias))
    {
        clearbiasgl();
        biaspersistgen = int(hwrtdlaabias);
        biascaminvalidate();
    }

    bool query = glBeginQuery_ && glEndQuery_ && glGenQueries_ && !biascostpending;
    if(query && !biascostq) glGenQueries_(1, &biascostq);
    if(query && biascostq)
    {
        glBeginQuery_(GL_TIME_ELAPSED, biascostq);
        biascostpending = true;
    }

    int dst = biascurr;
    int src = biascurr ^ 1;
    glBindFramebuffer_(GL_FRAMEBUFFER, biasfbo);
    glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, biasgl[dst], 0);
    glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, biasds);
    glViewport(0, 0, w, h);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_CULL_FACE);
    if(prev.scissor) glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glClearColor(0, 0, 0, 0);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDepthMask(GL_FALSE);

    glBindFramebuffer_(GL_READ_FRAMEBUFFER, GLuint(prev.fb));
    glBindFramebuffer_(GL_DRAW_FRAMEBUFFER, biasfbo);
    if(glBlitFramebuffer_) glBlitFramebuffer_(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);

    glBindFramebuffer_(GL_FRAMEBUFFER, biasfbo);

    if(hwrtdlaabias >= 2)
    {
        glClearColor(1, 1, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    else
    {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glEnable(GL_BLEND);
        if(glBlendEquation_) glBlendEquation_(GL_MAX);
        glBlendFunc(GL_ONE, GL_ONE);
        hwrtpartbiasdraw();
        if(glBlendEquation_) glBlendEquation_(GL_FUNC_ADD);
    }

    bool persist = hwrtdlaabias == 1 && hwrtdlaabiaspersist > 0 && biascamerastill();
    if(persist)
    {
        Shader *s = useshaderbyname("hwrtdlaabiasfade");
        if(s && !s->invalid())
        {
            glDisable(GL_DEPTH_TEST);
            glEnable(GL_BLEND);
            if(glBlendEquation_) glBlendEquation_(GL_MAX);
            glBlendFunc(GL_ONE, GL_ONE);
            if(glActiveTexture_) glActiveTexture_(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, biasgl[src]);
            s->set();
            LOCALPARAMF(biasfade, float(hwrtdlaabiaspersist) / 100.0f, 0, 0, 0);
            screenquad(1, 1);
            if(glBlendEquation_) glBlendEquation_(GL_FUNC_ADD);
            lastbiaspersistused = 1;
        }
    }

    lastbiasgl = biasgl[dst];
    lastbiasbound = 1;
    biascurr = src;
    biascamstore();

    if(query && biascostq && glEndQuery_) glEndQuery_(GL_TIME_ELAPSED);
    restorebiasglprev(prev);
}

static bool biaswanted()
{
    // FSR reactive mask only (see hwrtdlaabias): never for DLAA/DLSS.
    return fsrmode(ngxmodeapp) && hwrtdlaabias > 0 && lastbiasbound && lastbiasgl && texbias.gltex;
}

bool hwrtsceneblitwindow()
{
    if(!scenefbo || scenew < 8 || sceneh < 8 || screenw < 8 || screenh < 8) return false;
    if(!glBlitFramebuffer_) return false;
    GLint prevread = 0, prevdraw = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevread);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevdraw);
    glBindFramebuffer_(GL_READ_FRAMEBUFFER, scenefbo);
    glBindFramebuffer_(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer_(0, 0, scenew, sceneh, 0, 0, screenw, screenh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer_(GL_READ_FRAMEBUFFER, prevread);
    glBindFramebuffer_(GL_DRAW_FRAMEBUFFER, prevdraw);
    return true;
}

static bool ensureres(int inw, int inh, int outw, int outh)
{
    int wantdrhdr = (scenehdr && scenecolorfmt == GL_RGBA16F) ? 1 : 0;
    if(resinw == inw && resinh == inh && resoutw == outw && resouth == outh && reshdr == wantdrhdr &&
       texcolorin.gltex && texcolorout.gltex && texdepth.gltex && texmotion.gltex && texbias.gltex && cmdok &&
       (!wantdrhdr || texexposure.gltex))
        return true;
    if(!hwrtdev.device || !loadglinterop()) return false;
    if(inw < 8 || inh < 8 || outw < 8 || outh < 8) return false;
    waitglcomplete();
    if(hwrtdev.device) vkDeviceWaitIdle(hwrtdev.device);
    releasefeature();
    destroytex(texcolorin);
    destroytex(texdepth);
    destroytex(texmotion);
    destroytex(texcolorout);
    destroytex(texbias);
    destroytex(texexposure);
    // Coverage FBO is independent of the packed NGX images. Destroying it here
    // on the first Quality frame threw away the mask just captured from the
    // reduced scene FBO. Keep it when the size already matches.
    if(biasglw != inw || biasglh != inh) destroybiasgl();
    destroysems();
    if(packfbo) { glDeleteFramebuffers_(1, &packfbo); packfbo = 0; }
    if(!ensurecmd()) return false;
    VkImageUsageFlags usage = texusage();
    VkFormat colorvk = wantdrhdr ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
    GLenum colorgl = wantdrhdr ? GL_RGBA16F : GL_RGBA8;
    if(!createtex(texcolorin, inw, inh, colorvk, colorgl, usage, GL_LINEAR) ||
       !createtex(texcolorout, outw, outh, colorvk, colorgl, usage, GL_LINEAR) ||
       !createtex(texdepth, inw, inh, VK_FORMAT_R32_SFLOAT, GL_R32F, usage, GL_NEAREST) ||
       !createtex(texmotion, inw, inh, VK_FORMAT_R16G16B16A16_SFLOAT, GL_RGBA16F, usage, GL_NEAREST) ||
       !createtex(texbias, inw, inh, VK_FORMAT_R8G8B8A8_UNORM, GL_RGBA8, usage, GL_NEAREST) ||
       (wantdrhdr && !createtex(texexposure, 1, 1, VK_FORMAT_R32_SFLOAT, GL_R32F, usage, GL_NEAREST)) ||
       !createsem(vkglready, glglready, glreadyhandle) ||
       !createsem(vkvkdone, glvkdone, vkdonehandle))
    {
        hwrtdlaacleanup();
        return false;
    }
    glGenFramebuffers_(1, &packfbo);
    transitionall(VK_IMAGE_LAYOUT_GENERAL);
    if(!costqok)
    {
        glGenQueries_(DLAA_FIF, costq);
        costqok = true;
        memset(costpending, 0, sizeof(costpending));
        costslot = 0;
    }
    resinw = resw = inw;
    resinh = resh = inh;
    resoutw = outw;
    resouth = outh;
    reshdr = wantdrhdr;
    hasprevunjit = false;
    conoutf(CON_INIT, "hwrt ngx: shared images in %dx%d %s+R32F+RGBA16F+biasRGBA8, out %dx%d %s%s (Y-flipped for DLSS)",
            inw, inh, wantdrhdr ? "RGBA16F" : "RGBA8", outw, outh, wantdrhdr ? "RGBA16F" : "RGBA8",
            wantdrhdr ? " +exposureR32F" : "");
    return true;
}

// FSR context for the same shared images. No command buffer: the FidelityFX
// backend uploads its own initial data at the first dispatch.
static bool configurefsr(int inw, int inh, int outw, int outh, int mode)
{
    if(hwrtdlaainject == 2)
    {
        setreason("injected: configure/prep failure");
        return false;
    }
    int wantHdr = (scenehdr && scenecolorfmt == GL_RGBA16F && texcolorin.format == VK_FORMAT_R16G16B16A16_SFLOAT && texcolorout.format == VK_FORMAT_R16G16B16A16_SFLOAT) ? 1 : 0;
    if(dlaaconfigured && int(dlaainw) == inw && int(dlaainh) == inh && int(dlaaoutw) == outw && int(dlaaouth) == outh && ngxmodeapp == mode && ngxcfghdr == wantHdr)
        return dlaaoptok;
    if(hwrtdev.device) vkDeviceWaitIdle(hwrtdev.device);
    releasefeature();
    ngxmodeapp = 0;
    ngxusedoptimal = 0;
    bool sr = ngxissrmode(mode);
    if(sr ? (inw >= outw && inh >= outh) : (inw != outw || inh != outh))
    {
        setreason(sr ? "FSR upscale scene is not below the output" : "FSR Native is not 1:1");
        return false;
    }
    char err[256];
    if(!hwrtfsrcreate(mode, inw, inh, outw, outh, wantHdr != 0, err, sizeof(err)))
    {
        setreason(err[0] ? err : "FSR create failed");
        return false;
    }
    dlaainw = uint32_t(inw);
    dlaainh = uint32_t(inh);
    dlaaoutw = uint32_t(outw);
    dlaaouth = uint32_t(outh);
    ngxusedoptimal = sr && uint32_t(inw) == ngxoptw && uint32_t(inh) == ngxopth ? 1u : 0u;
    ngxmodeapp = mode;
    ngxcfghdr = wantHdr;
    ngxfeatfresh = 1;
    dlaaoptok = true;
    dlaaconfigured = true;
    lastngxinw = dlaainw;
    lastngxinh = dlaainh;
    lastngxoutw = dlaaoutw;
    lastngxouth = dlaaouth;
    lastngxoptw = ngxoptw;
    lastngxopth = ngxopth;
    conoutf(CON_INIT, "hwrt fsr: %s  in %ux%u  out %ux%u  HDR %d  sharpness %d",
            ngxmodename(ngxmodeapp), dlaainw, dlaainh, dlaaoutw, dlaaouth, wantHdr, hwrtfsrsharpness);
    refreshlookaa();
    return true;
}

static bool configure(int inw, int inh, int outw, int outh, int mode)
{
    if(fsrmode(mode)) return configurefsr(inw, inh, outw, outh, mode);
    if(!ngxsupported || !p_gwCreateFeat) return false;
    if(hwrtdlaainject == 2)
    {
        setreason("injected: configure/prep failure");
        return false;
    }
    int wantHdr = (scenehdr && scenecolorfmt == GL_RGBA16F && texcolorin.format == VK_FORMAT_R16G16B16A16_SFLOAT && texcolorout.format == VK_FORMAT_R16G16B16A16_SFLOAT) ? 1 : 0;
    if(dlaaconfigured && int(dlaainw) == inw && int(dlaainh) == inh && int(dlaaoutw) == outw && int(dlaaouth) == outh && ngxmodeapp == mode && ngxcfghdr == wantHdr)
        return dlaaoptok;
    if((ngxcfghdr == 0 || ngxcfghdr == 1) && ngxcfghdr != wantHdr)
        logoutf("comparateur ngx historique recree IsHDR %d -> %d, carte non rechargee", ngxcfghdr, wantHdr);
    if(hwrtdev.device) vkDeviceWaitIdle(hwrtdev.device);
    releasefeature();
    ngxmodeapp = 0;
    ngxusedoptimal = 0;
    if(!ensurecmd()) return false;
    vkWaitForFences(hwrtdev.device, 1, &fences[0], VK_TRUE, UINT64_MAX);
    vkResetCommandBuffer(cmdbuf[0], 0);
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if(vkBeginCommandBuffer(cmdbuf[0], &begin) != VK_SUCCESS)
    {
        setreason("configure vkBeginCommandBuffer failed");
        return false;
    }
    SauerNgxCreateFeature c;
    memset(&c, 0, sizeof(c));
    c.struct_bytes = sizeof(c);
    c.device = (uint64_t)(uintptr_t)hwrtdev.device;
    c.command_buffer = (uint64_t)(uintptr_t)cmdbuf[0];
    c.out_w = uint32_t(outw);
    c.out_h = uint32_t(outh);
    c.in_w = uint32_t(inw);
    c.in_h = uint32_t(inh);
    c.mode = gwmodefromengine(mode);
    c.preset_k = 1;
    c.hdr = uint32_t(wantHdr);
    c.auto_exposure = wantHdr ? 0u : 1u;
    SauerNgxCreateInfo info;
    memset(&info, 0, sizeof(info));
    info.struct_bytes = sizeof(info);
    int32_t rc = p_gwCreateFeat(&c, sizeof(c), &gwhandle, &info, sizeof(info));
    if(vkEndCommandBuffer(cmdbuf[0]) != VK_SUCCESS)
    {
        if(gwhandle && p_gwRelease) { p_gwRelease(gwhandle); gwhandle = 0; }
        setreason("configure vkEndCommandBuffer failed");
        return false;
    }
    vkResetFences(hwrtdev.device, 1, &fences[0]);
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmdbuf[0];
    if(vkQueueSubmit(hwrtdev.queue, 1, &submit, fences[0]) != VK_SUCCESS)
    {
        recreatesignaledfence(0);
        if(gwhandle && p_gwRelease) { p_gwRelease(gwhandle); gwhandle = 0; }
        setreason("configure vkQueueSubmit failed");
        return false;
    }
    vkWaitForFences(hwrtdev.device, 1, &fences[0], VK_TRUE, UINT64_MAX);
    gwcopyerr();
    conoutf(CON_INIT, "hdr lot2 ngx create dll: %s", ngxreason);
    if(rc != SAUER_NGX_OK || !gwhandle)
        return false;
    dlaainw = info.in_w ? info.in_w : uint32_t(inw);
    dlaainh = info.in_h ? info.in_h : uint32_t(inh);
    dlaaoutw = info.out_w ? info.out_w : uint32_t(outw);
    dlaaouth = info.out_h ? info.out_h : uint32_t(outh);
    ngxoptw = info.optimal_w;
    ngxopth = info.optimal_h;
    ngxusedoptimal = info.used_optimal;
    ngxmodeapp = enginefromgwmode(info.mode_applied);
    ngxcfghdr = int(info.hdr);
    ngxcfgautoexp = int(info.auto_exposure);
    ngxfeatfresh = 1;
    conoutf(CON_INIT, "hdr lot2 ngx contrat IsHDR=%u AutoExposure=%u MVLowRes=1 DepthInverted=0 MVJittered=0 flags=0x%x in %ux%u out %ux%u couleur %s",
            (unsigned)info.hdr, (unsigned)info.auto_exposure, (unsigned)info.flags,
            dlaainw, dlaainh, dlaaoutw, dlaaouth,
            info.hdr ? "VK_FORMAT_R16G16B16A16_SFLOAT" : "VK_FORMAT_R8G8B8A8_UNORM");
    dlaaoptok = ngxissrmode(mode) ? (dlaainw < dlaaoutw || dlaainh < dlaaouth) : (dlaainw == dlaaoutw && dlaainh == dlaaouth);
    if(int(dlaainw) != inw || int(dlaainh) != inh || int(dlaaoutw) != outw || int(dlaaouth) != outh || !dlaaoptok)
    {
        releasefeature();
        ngxmodeapp = 0;
        ngxfeatfresh = 0;
        dlaaconfigured = false;
        setreason(ngxissrmode(mode) ? "SR create size disagrees with GET_OPTIMAL scene" : "DLAA create is not native 1:1");
        return false;
    }
    dlaaconfigured = true;
    lastngxinw = dlaainw;
    lastngxinh = dlaainh;
    lastngxoutw = dlaaoutw;
    lastngxouth = dlaaouth;
    lastngxoptw = ngxoptw;
    lastngxopth = ngxopth;
    conoutf(CON_INIT, "hwrt ngx: CREATE_DLSS_EXT1 %s preset %s  in %ux%u  out %ux%u  optimal %ux%u used_optimal=%u  IsHDR=%d  AutoExposure=%d  MVLowRes  mip bias %.3f",
            ngxmodename(ngxmodeapp), ngxpresetname(ngxmodeapp), dlaainw, dlaainh, dlaaoutw, dlaaouth, ngxoptw, ngxopth, (unsigned)ngxusedoptimal,
            ngxcfghdr, ngxcfgautoexp, getscenelodbias());
    refreshlookaa();
    return true;
}

// Row-major multiply copied from Streamline 2.12 sl_matrix_helpers.h::matrixMul
// (result = a * b, column vectors). Used only to audit tomat/fillconstants.
static void slrowmul(float r[16], const float a[16], const float b[16])
{
    r[0]  = a[0]*b[0]  + a[1]*b[4]  + a[2]*b[8]  + a[3]*b[12];
    r[1]  = a[0]*b[1]  + a[1]*b[5]  + a[2]*b[9]  + a[3]*b[13];
    r[2]  = a[0]*b[2]  + a[1]*b[6]  + a[2]*b[10] + a[3]*b[14];
    r[3]  = a[0]*b[3]  + a[1]*b[7]  + a[2]*b[11] + a[3]*b[15];
    r[4]  = a[4]*b[0]  + a[5]*b[4]  + a[6]*b[8]  + a[7]*b[12];
    r[5]  = a[4]*b[1]  + a[5]*b[5]  + a[6]*b[9]  + a[7]*b[13];
    r[6]  = a[4]*b[2]  + a[5]*b[6]  + a[6]*b[10] + a[7]*b[14];
    r[7]  = a[4]*b[3]  + a[5]*b[7]  + a[6]*b[11] + a[7]*b[15];
    r[8]  = a[8]*b[0]  + a[9]*b[4]  + a[10]*b[8] + a[11]*b[12];
    r[9]  = a[8]*b[1]  + a[9]*b[5]  + a[10]*b[9] + a[11]*b[13];
    r[10] = a[8]*b[2]  + a[9]*b[6]  + a[10]*b[10]+ a[11]*b[14];
    r[11] = a[8]*b[3]  + a[9]*b[7]  + a[10]*b[11]+ a[11]*b[15];
    r[12] = a[12]*b[0] + a[13]*b[4] + a[14]*b[8] + a[15]*b[12];
    r[13] = a[12]*b[1] + a[13]*b[5] + a[14]*b[9] + a[15]*b[13];
    r[14] = a[12]*b[2] + a[13]*b[6] + a[14]*b[10]+ a[15]*b[14];
    r[15] = a[12]*b[3] + a[13]*b[7] + a[14]*b[11]+ a[15]*b[15];
}

static bool slrowinvert(float r[16], const float m[16])
{
    const float *pMat = m;
    float *pResult = r;
    pResult[0] = pMat[5]*pMat[10]*pMat[15] - pMat[5]*pMat[11]*pMat[14] - pMat[9]*pMat[6]*pMat[15] + pMat[9]*pMat[7]*pMat[14] + pMat[13]*pMat[6]*pMat[11] - pMat[13]*pMat[7]*pMat[10];
    pResult[4] = -pMat[4]*pMat[10]*pMat[15] + pMat[4]*pMat[11]*pMat[14] + pMat[8]*pMat[6]*pMat[15] - pMat[8]*pMat[7]*pMat[14] - pMat[12]*pMat[6]*pMat[11] + pMat[12]*pMat[7]*pMat[10];
    pResult[8] = pMat[4]*pMat[9]*pMat[15] - pMat[4]*pMat[11]*pMat[13] - pMat[8]*pMat[5]*pMat[15] + pMat[8]*pMat[7]*pMat[13] + pMat[12]*pMat[5]*pMat[11] - pMat[12]*pMat[7]*pMat[9];
    pResult[12] = -pMat[4]*pMat[9]*pMat[14] + pMat[4]*pMat[10]*pMat[13] + pMat[8]*pMat[5]*pMat[14] - pMat[8]*pMat[6]*pMat[13] - pMat[12]*pMat[5]*pMat[10] + pMat[12]*pMat[6]*pMat[9];
    pResult[1] = -pMat[1]*pMat[10]*pMat[15] + pMat[1]*pMat[11]*pMat[14] + pMat[9]*pMat[2]*pMat[15] - pMat[9]*pMat[3]*pMat[14] - pMat[13]*pMat[2]*pMat[11] + pMat[13]*pMat[3]*pMat[10];
    pResult[5] = pMat[0]*pMat[10]*pMat[15] - pMat[0]*pMat[11]*pMat[14] - pMat[8]*pMat[2]*pMat[15] + pMat[8]*pMat[3]*pMat[14] + pMat[12]*pMat[2]*pMat[11] - pMat[12]*pMat[3]*pMat[10];
    pResult[9] = -pMat[0]*pMat[9]*pMat[15] + pMat[0]*pMat[11]*pMat[13] + pMat[8]*pMat[1]*pMat[15] - pMat[8]*pMat[3]*pMat[13] - pMat[12]*pMat[1]*pMat[11] + pMat[12]*pMat[3]*pMat[9];
    pResult[13] = pMat[0]*pMat[9]*pMat[14] - pMat[0]*pMat[10]*pMat[13] - pMat[8]*pMat[1]*pMat[14] + pMat[8]*pMat[2]*pMat[13] + pMat[12]*pMat[1]*pMat[10] - pMat[12]*pMat[2]*pMat[9];
    pResult[2] = pMat[1]*pMat[6]*pMat[15] - pMat[1]*pMat[7]*pMat[14] - pMat[5]*pMat[2]*pMat[15] + pMat[5]*pMat[3]*pMat[14] + pMat[13]*pMat[2]*pMat[7] - pMat[13]*pMat[3]*pMat[6];
    pResult[6] = -pMat[0]*pMat[6]*pMat[15] + pMat[0]*pMat[7]*pMat[14] + pMat[4]*pMat[2]*pMat[15] - pMat[4]*pMat[3]*pMat[14] - pMat[12]*pMat[2]*pMat[7] + pMat[12]*pMat[3]*pMat[6];
    pResult[10] = pMat[0]*pMat[5]*pMat[15] - pMat[0]*pMat[7]*pMat[13] - pMat[4]*pMat[1]*pMat[15] + pMat[4]*pMat[3]*pMat[13] + pMat[12]*pMat[1]*pMat[7] - pMat[12]*pMat[3]*pMat[5];
    pResult[14] = -pMat[0]*pMat[5]*pMat[14] + pMat[0]*pMat[6]*pMat[13] + pMat[4]*pMat[1]*pMat[14] - pMat[4]*pMat[2]*pMat[13] - pMat[12]*pMat[1]*pMat[6] + pMat[12]*pMat[2]*pMat[5];
    pResult[3] = -pMat[1]*pMat[6]*pMat[11] + pMat[1]*pMat[7]*pMat[10] + pMat[5]*pMat[2]*pMat[11] - pMat[5]*pMat[3]*pMat[10] - pMat[9]*pMat[2]*pMat[7] + pMat[9]*pMat[3]*pMat[6];
    pResult[7] = pMat[0]*pMat[6]*pMat[11] - pMat[0]*pMat[7]*pMat[10] - pMat[4]*pMat[2]*pMat[11] + pMat[4]*pMat[3]*pMat[10] + pMat[8]*pMat[2]*pMat[7] - pMat[8]*pMat[3]*pMat[6];
    pResult[11] = -pMat[0]*pMat[5]*pMat[11] + pMat[0]*pMat[7]*pMat[9] + pMat[4]*pMat[1]*pMat[11] - pMat[4]*pMat[3]*pMat[9] - pMat[8]*pMat[1]*pMat[7] + pMat[8]*pMat[3]*pMat[5];
    pResult[15] = pMat[0]*pMat[5]*pMat[10] - pMat[0]*pMat[6]*pMat[9] - pMat[4]*pMat[1]*pMat[10] + pMat[4]*pMat[2]*pMat[9] + pMat[8]*pMat[1]*pMat[6] - pMat[8]*pMat[2]*pMat[5];
    float det = pMat[0]*pResult[0] + pMat[1]*pResult[4] + pMat[2]*pResult[8] + pMat[3]*pResult[12];
    if(det == 0) return false;
    det = 1.0f / det;
    loopi(16) pResult[i] *= det;
    return true;
}

static void tomat(const matrix4 &m, float o[16])
{
    o[0] = m.a.x; o[1] = m.b.x; o[2] = m.c.x; o[3] = m.d.x;
    o[4] = m.a.y; o[5] = m.b.y; o[6] = m.c.y; o[7] = m.d.y;
    o[8] = m.a.z; o[9] = m.b.z; o[10] = m.c.z; o[11] = m.d.z;
    o[12] = m.a.w; o[13] = m.b.w; o[14] = m.c.w; o[15] = m.d.w;
}

static void flipclipy(matrix4 &m)
{
    m.a.y = -m.a.y;
    m.b.y = -m.b.y;
    m.c.y = -m.c.y;
    m.d.y = -m.d.y;
}


static float matmaxabsdiff(const float a[16], const float b[16])
{
    float e = 0;
    loopi(16) { float d = fabsf(a[i] - b[i]); if(d > e) e = d; }
    return e;
}

static void xform4(const float m[16], float x, float y, float z, float w, float o[4])
{
    o[0] = m[0]*x + m[1]*y + m[2]*z + m[3]*w;
    o[1] = m[4]*x + m[5]*y + m[6]*z + m[7]*w;
    o[2] = m[8]*x + m[9]*y + m[10]*z + m[11]*w;
    o[3] = m[12]*x + m[13]*y + m[14]*z + m[15]*w;
}

static void runcontract(FILE *f)
{
    lastcontractok = 0;
    lastcontracterr = -1;
    copystring(lastcontractnote, "failed", sizeof(lastcontractnote));

    matrix4 trans;
    trans.identity();
    trans.d = vec4(10, 20, 30, 1);
    matrix4 rot;
    rot.identity();
    rot.rotate_around_z(30 * RAD);
    matrix4 world;
    world.mul(trans, rot);

    float row[16], rowinv[16], engineinvrow[16];
    tomat(world, row);
    if(!slrowinvert(rowinv, row))
    {
        copystring(lastcontractnote, "slrowinvert failed on known T*R", sizeof(lastcontractnote));
        if(f) fprintf(f, "contract FAIL slrowinvert\n");
        return;
    }
    matrix4 engineinv;
    if(!engineinv.invert(world))
    {
        copystring(lastcontractnote, "engine invert failed on known T*R", sizeof(lastcontractnote));
        if(f) fprintf(f, "contract FAIL engine invert\n");
        return;
    }
    tomat(engineinv, engineinvrow);
    float err_inv = matmaxabsdiff(rowinv, engineinvrow);

    vec4 p(1, 0, 0, 1), tp, back;
    world.transform(p, tp);
    engineinv.transform(tp, back);
    float err_round = max(fabsf(back.x - p.x), max(fabsf(back.y - p.y), max(fabsf(back.z - p.z), fabsf(back.w - p.w))));

    float slp[4], slback[4];
    xform4(row, 1, 0, 0, 1, slp);
    xform4(rowinv, slp[0], slp[1], slp[2], slp[3], slback);
    float err_sl = max(fabsf(slback[0] - 1), max(fabsf(slback[1]), max(fabsf(slback[2]), fabsf(slback[3] - 1))));
    float err_path = max(fabsf(slp[0] - tp.x), max(fabsf(slp[1] - tp.y), max(fabsf(slp[2] - tp.z), fabsf(slp[3] - tp.w))));

    matrix4 viewtoclip;
    viewtoclip.identity();
    viewtoclip.d.z = -1;
    viewtoclip.c.w = -1;
    viewtoclip.d.w = 0;
    matrix4 viewtoclip_d3d = viewtoclip;
    flipclipy(viewtoclip_d3d);
    float pgl[16], pd3d[16];
    tomat(viewtoclip, pgl);
    tomat(viewtoclip_d3d, pd3d);
    float clip_y_sign = pd3d[5] * pgl[5];
    bool yflipped = fabsf(pd3d[5] + pgl[5]) < 1e-5f && fabsf(pgl[5]) > 0.5f;

    matrix4 curr = world;
    matrix4 prev = world;
    prev.d.x += 5;
    matrix4 cliptoworld;
    if(!cliptoworld.invert(curr)) { copystring(lastcontractnote, "invert curr failed", sizeof(lastcontractnote)); return; }
    matrix4 cliptoprev;
    cliptoprev.mul(prev, cliptoworld);
    float arow[16], brow[16], prod[16], engineprod[16];
    tomat(prev, arow);
    tomat(cliptoworld, brow);
    slrowmul(prod, arow, brow);
    tomat(cliptoprev, engineprod);
    float err_mul = matmaxabsdiff(prod, engineprod);

    vec4 clip(0.1f, -0.2f, 0.3f, 1), worldh, prevclip_e;
    cliptoworld.transform(clip, worldh);
    prev.transform(worldh, prevclip_e);
    float clipr[16], prevr[16], c2p[16];
    tomat(cliptoworld, clipr);
    tomat(prev, prevr);
    slrowmul(c2p, prevr, clipr);
    float prevclip_s[4];
    xform4(c2p, clip.x, clip.y, clip.z, clip.w, prevclip_s);
    float err_pt = max(fabsf(prevclip_s[0] - prevclip_e.x), max(fabsf(prevclip_s[1] - prevclip_e.y), max(fabsf(prevclip_s[2] - prevclip_e.z), fabsf(prevclip_s[3] - prevclip_e.w))));

    lastcontracterr = max(err_inv, max(err_round, max(err_sl, max(err_path, max(err_mul, err_pt)))));
    lastcontractok = lastcontracterr < 1e-4f ? 1 : 0;
    nformatstring(lastcontractnote, sizeof(lastcontractnote),
                  "%s maxabs %.6g invert %.6g roundtrip %.6g sl %.6g tomat_vs_engine_xform %.6g mul %.6g clipToPrev point %.6g",
                  lastcontractok ? "PASS" : "FAIL", lastcontracterr, err_inv, err_round, err_sl, err_path, err_mul, err_pt);

    if(f)
    {
        fprintf(f, "contract %s\n", lastcontractnote);
        fprintf(f, "tomat: engine columns a,b,c,d -> NGX/SL row-major rows (transpose of column-major). Multiplication compared to sl_matrix_helpers.h matrixMul (row-major, column vectors).\n");
        fprintf(f, "known transform: T(10,20,30) * RotZ(30deg). Point (1,0,0,1).\n");
        fprintf(f, "engine T*R*(1,0,0,1) = %.6f %.6f %.6f %.6f\n", tp.x, tp.y, tp.z, tp.w);
        fprintf(f, "slrow same point = %.6f %.6f %.6f %.6f\n", slp[0], slp[1], slp[2], slp[3]);
        fprintf(f, "clipToPrevClip = prevWorldToClip * clipToWorld (engine mul(prev, cliptoworld)); matches sl_consts.h clipToPrevClip = clipToView * viewToViewPrev * viewToClipPrev for this camera-only case.\n");
        fprintf(f, "clip Y flip: GL clip +Y up -> D3D +Y down by negating matrix row Y (columns' .y). GL rowY.y=%.6f D3D rowY.y=%.6f product %.6f (expect -1)\n",
                pgl[5], pd3d[5], clip_y_sign);
        fprintf(f, "yflip_applied %s\n", yflipped ? "yes" : "no");
        fprintf(f, "depth: window 0 near / 1 far packed as R32F. NGX DepthInverted is NOT set. GL clip used only in the pack reconstruction is z*2-1 with GL Y-up UVs. NGX sees the 0-1 window values, same convention as uninverted D3D depth.\n");
        fprintf(f, "jitter: engine +Y up pixels; NGX InJitterOffsetY = -engine.y (D3D +Y down). MVs packed with Y negated; InMVScale 1,1 (pixel space). motionVectorsJittered false.\n");
        fprintf(f, "NGX_VULKAN_EVALUATE_DLSS_EXT consumes Color, Depth, MotionVectors, Output via NVSDK_NGX_Create_ImageView_Resource_VK. Uncovered (B<0.5) pack reconstruction still uses inv(camproj_used) for camera-only MVs.\n");
        fprintf(f, "HDR exposure: colour is scene-linear and not pre-multiplied. pre_exposure=1 exposure_scale=1. The 1x1 R32F texture holds 2^hwrthdrexp; DLSS normalises with it and inverts it, so the output stays scene-linear. hdrpresent multiplies that same value once. AutoExposure is off in HDR. This is not camera auto-exposure.\n");
        fprintf(f, "weapon: mixed window depth after renderavatar. B>=0.5 keeps avatar MVs. World unproject of gun pixels is not used. Mixed Z is the framebuffer occlusion value (0-1, not inverted), which is what DLAA is given for disocclusion along with those MVs.\n");
    }
}

static bool packinputs(const hwrttemporalinput *tin)
{
    if(hwrtdlaainject == 2)
    {
        setreason("injected: configure/prep failure");
        return false;
    }
    Shader *s = useshaderbyname("hwrtdlaapack");
    if(!s || s->invalid())
    {
        setreason("missing shader hwrtdlaapack");
        return false;
    }
    GLint prevfb = 0, prevvp[4], prevactive = 0, prevtex0 = 0, prevtex1 = 0, prevtex2 = 0;
    GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST), cull = glIsEnabled(GL_CULL_FACE);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevfb);
    glGetIntegerv(GL_VIEWPORT, prevvp);
    GLint prevdraw = 0;
    glGetIntegerv(GL_DRAW_BUFFER, &prevdraw);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevactive);
    glActiveTexture_(GL_TEXTURE0); glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevtex0);
    glActiveTexture_(GL_TEXTURE1); glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevtex1);
    glActiveTexture_(GL_TEXTURE2); glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevtex2);

    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    if(scissor) glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, tin->width, tin->height);
    glBindFramebuffer_(GL_FRAMEBUFFER, packfbo);

    glActiveTexture_(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tin->colortex);
    glActiveTexture_(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, tin->depthtex);
    glActiveTexture_(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, tin->velocitytex ? tin->velocitytex : tin->colortex);
    glActiveTexture_(GL_TEXTURE0);
    s->set();
    LOCALPARAM(dlaainvused, tin->invcamproj_used);
    LOCALPARAM(dlaacurrunjit, tin->camproj_unjit);
    LOCALPARAM(dlaaprevunjit, hasprevunjit ? prevunjit : tin->camproj_unjit);
    LOCALPARAMF(dlaaparam, float(tin->width), float(tin->height), hasprevunjit && !tin->reset ? 1.0f : 0.0f, tin->velocitytex ? 1.0f : 0.0f);

    GLenum buf = GL_COLOR_ATTACHMENT0;
    glDrawBuffers_(1, &buf);
    GLuint targets[4] = { texcolorin.gltex, texdepth.gltex, texmotion.gltex, texbias.gltex };
    int npass = biaswanted() ? 4 : 3;
    bool packok = true;
    loopi(npass)
    {
        glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, targets[i], 0);
        GLenum fbstatus = glCheckFramebufferStatus_(GL_FRAMEBUFFER);
        if(fbstatus != GL_FRAMEBUFFER_COMPLETE)
        {
            defformatstring(msg, "pack FBO incomplete 0x%x pass %d", int(fbstatus), i);
            setreason(msg);
            packok = false;
            break;
        }
        if(i == 3)
        {
            glActiveTexture_(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, lastbiasgl);
        }
        LOCALPARAMF(dlaapass, float(i), 0, 0, 0);
        screenquad(1, 1);
    }

    glBindFramebuffer_(GL_FRAMEBUFFER, prevfb);
    if(prevdraw) glDrawBuffer(GLenum(prevdraw));
    else glDrawBuffer(GL_BACK);
    glViewport(prevvp[0], prevvp[1], prevvp[2], prevvp[3]);
    glActiveTexture_(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, prevtex0);
    glActiveTexture_(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, prevtex1);
    glActiveTexture_(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, prevtex2);
    glActiveTexture_(GLenum(prevactive));
    if(blend) glEnable(GL_BLEND);
    if(depth) glEnable(GL_DEPTH_TEST);
    if(cull) glEnable(GL_CULL_FACE);
    if(scissor) glEnable(GL_SCISSOR_TEST);
    while(glGetError() != GL_NO_ERROR);
    return packok;
}

static bool blitoutput()
{
    if(hwrtdlaainject == 4)
    {
        setreason("injected: blit failure");
        return false;
    }
    Shader *s = useshaderbyname("hwrtdlaablit");
    if(!s || s->invalid())
    {
        setreason("missing shader hwrtdlaablit");
        return false;
    }
    GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST), cull = glIsEnabled(GL_CULL_FACE);
    GLint prevfb = 0, prevvp[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevfb);
    glGetIntegerv(GL_VIEWPORT, prevvp);
    glBindFramebuffer_(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, screenw, screenh);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glActiveTexture_(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texcolorout.gltex);
    s->set();
    screenquad(1, 1);
    glBindFramebuffer_(GL_FRAMEBUFFER, prevfb);
    glViewport(prevvp[0], prevvp[1], prevvp[2], prevvp[3]);
    if(blend) glEnable(GL_BLEND);
    if(depth) glEnable(GL_DEPTH_TEST);
    if(cull) glEnable(GL_CULL_FACE);
    return true;
}

static void harvestcost()
{
    harvestbiascost();
    if(!costqok) return;
    int slot = costslot;
    if(!costpending[slot]) return;
    GLint avail = 0;
    glGetQueryObjectiv_(costq[slot], GL_QUERY_RESULT_AVAILABLE, &avail);
    if(!avail) return;
    GLuint ns = 0;
    glGetQueryObjectuiv_(costq[slot], GL_QUERY_RESULT, &ns);
    lastgpums = ns / 1.0e6f;
    costpending[slot] = false;
    gpuring[gpuringi] = lastgpums;
    gpuringi = (gpuringi + 1) % DLAA_GPUSAMPLES;
    if(gpuringn < DLAA_GPUSAMPLES) gpuringn++;
}

static float gpumean()
{
    if(gpuringn <= 0) return 0;
    float s = 0;
    loopi(gpuringn) s += gpuring[i];
    return s / float(gpuringn);
}

static bool draininterop()
{
    if(!cmdok || !hwrtdev.queue || !vkglready || !vkvkdone) return false;
    vkWaitForFences(hwrtdev.device, 1, &fences[fifslot], VK_TRUE, UINT64_MAX);
    vkResetCommandBuffer(cmdbuf[fifslot], 0);
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if(vkBeginCommandBuffer(cmdbuf[fifslot], &begin) != VK_SUCCESS) return false;
    if(vkEndCommandBuffer(cmdbuf[fifslot]) != VK_SUCCESS) return false;
    vkResetFences(hwrtdev.device, 1, &fences[fifslot]);
    VkPipelineStageFlags waitstage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &vkglready;
    submit.pWaitDstStageMask = &waitstage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmdbuf[fifslot];
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &vkvkdone;
    VkResult sr = vkQueueSubmit(hwrtdev.queue, 1, &submit, fences[fifslot]);
    if(sr != VK_SUCCESS)
    {
        recreatesignaledfence(fifslot);
        setreason("drain vkQueueSubmit failed after GL semaphore was signaled");
        return false;
    }
    GLenum outlayout = GL_LAYOUT_GENERAL_EXT;
    dlaaWaitSem(glvkdone, 0, NULL, 1, &texcolorout.gltex, &outlayout);
    glsignaled = false;
    vkWaitForFences(hwrtdev.device, 1, &fences[fifslot], VK_TRUE, UINT64_MAX);
    return true;
}

static bool submitvk(bool roundtrip, const hwrttemporalinput *tin, bool reset, bool &submitted)
{
    submitted = false;
    if(!cmdok || !vkCmdCopyImage) { setreason("vkCmdCopyImage missing"); return false; }
    VkResult wr = vkWaitForFences(hwrtdev.device, 1, &fences[fifslot], VK_TRUE, 0);
    if(wr == VK_TIMEOUT)
    {
        double t0 = latency_watchblocks() ? latency_now() : 0;
        vkWaitForFences(hwrtdev.device, 1, &fences[fifslot], VK_TRUE, UINT64_MAX);
        if(t0) latency_blocked(t0, latency_now());
        hwrtfencestalls++;
    }
    vkResetCommandBuffer(cmdbuf[fifslot], 0);
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if(vkBeginCommandBuffer(cmdbuf[fifslot], &begin) != VK_SUCCESS)
    {
        setreason("vkBeginCommandBuffer failed");
        return false;
    }

    VkImage images[6];
    images[0] = texcolorin.image;
    images[1] = texdepth.image;
    images[2] = texmotion.image;
    images[3] = texcolorout.image;
    int nimg = 4;
    if(texbias.image) images[nimg++] = texbias.image;
    if(texexposure.image) images[nimg++] = texexposure.image;
    VkImageMemoryBarrier b[6];
    loopi(nimg)
    {
        memset(&b[i], 0, sizeof(b[i]));
        b[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b[i].srcAccessMask = 0;
        b[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        b[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        b[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b[i].image = images[i];
        b[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b[i].subresourceRange.levelCount = 1;
        b[i].subresourceRange.layerCount = 1;
    }
    vkCmdPipelineBarrier(cmdbuf[fifslot], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, NULL, 0, NULL, nimg, b);

    bool evalok = false;
    if(roundtrip)
    {
        VkImageCopy region;
        memset(&region, 0, sizeof(region));
        region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.srcSubresource.layerCount = 1;
        region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.dstSubresource.layerCount = 1;
        region.extent.width = uint32_t(tin->width);
        region.extent.height = uint32_t(tin->height);
        region.extent.depth = 1;
        vkCmdCopyImage(cmdbuf[fifslot], texcolorin.image, VK_IMAGE_LAYOUT_GENERAL,
                       texcolorout.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        evalok = true;
        lasteval = 1;
        lastevalcode = 0;
        lastroundtrip = true;
    }
    else
    {
        lastroundtrip = false;
        if(hwrtdlaainject == 3)
        {
            setreason("injected: evaluate failure");
            lasteval = 0;
            lastevalcode = SAUER_NGX_ERR_NGX;
            vkEndCommandBuffer(cmdbuf[fifslot]);
            return false;
        }
        if(fsrmode(ngxmodeapp))
        {
            // AMD FSR 3.1: the same packed images, jitter and reset as NGX.
            if(!dlaaconfigured || !hwrtfsrconfigured())
            {
                setreason("FSR dispatch path missing");
                vkEndCommandBuffer(cmdbuf[fifslot]);
                return false;
            }
            hwrtfsrdispatchargs fa;
            memset(&fa, 0, sizeof(fa));
            fa.cmd = cmdbuf[fifslot];
            fa.color.image = texcolorin.image; fa.color.format = texcolorin.format; fa.color.w = tin->width; fa.color.h = tin->height;
            fa.depth.image = texdepth.image; fa.depth.format = texdepth.format; fa.depth.w = tin->width; fa.depth.h = tin->height;
            fa.motion.image = texmotion.image; fa.motion.format = texmotion.format; fa.motion.w = tin->width; fa.motion.h = tin->height;
            fa.output.image = texcolorout.image; fa.output.format = texcolorout.format; fa.output.w = int(dlaaoutw); fa.output.h = int(dlaaouth);
            // explosion/smoke coverage (the DLSS bias mask) drives FSR reactivity
            if(biaswanted()) { fa.reactive.image = texbias.image; fa.reactive.format = texbias.format; fa.reactive.w = tin->width; fa.reactive.h = tin->height; }
            if(ngxcfghdr == 1) { fa.exposure.image = texexposure.image; fa.exposure.format = texexposure.format; fa.exposure.w = 1; fa.exposure.h = 1; }
            fa.jitterx = tin->jitter_px;
            fa.jittery = -tin->jitter_py;
            fa.renderw = tin->width;
            fa.renderh = tin->height;
            fa.reset = reset;
            char err[256];
            if(!hwrtfsrdispatch(fa, err, sizeof(err)))
            {
                setreason(err[0] ? err : "FSR dispatch failed");
                lasteval = 0;
                lastevalcode = SAUER_NGX_ERR_NGX;
                vkEndCommandBuffer(cmdbuf[fifslot]);
                return false;
            }
            lasteval = 1;
            lastevalcode = 0;
            evalok = true;
        }
        else
        {
            if(!dlaaconfigured || !p_gwEval || !gwhandle)
            {
                setreason("NGX evaluate path missing");
                vkEndCommandBuffer(cmdbuf[fifslot]);
                return false;
            }
            SauerNgxEval ev;
            memset(&ev, 0, sizeof(ev));
            ev.struct_bytes = sizeof(ev);
            ev.command_buffer = (uint64_t)(uintptr_t)cmdbuf[fifslot];
            fillgwimg(ev.color, texcolorin, tin->width, tin->height, 0);
            fillgwimg(ev.depth, texdepth, tin->width, tin->height, 0);
            fillgwimg(ev.motion, texmotion, tin->width, tin->height, 0);
            fillgwimg(ev.output, texcolorout, int(dlaaoutw ? dlaaoutw : uint32_t(screenw)), int(dlaaouth ? dlaaouth : uint32_t(screenh)), 1);
            // ev.bias stays empty: no BiasCurrentColorMask with presets K/M (guide 3.15).
            ev.pre_exposure = 1.0f;
            ev.exposure_scale = 1.0f;
            ev.hdr = ngxcfghdr == 1 ? 1u : 0u;
            if(ev.hdr) fillgwimg(ev.exposure, texexposure, 1, 1, 0);
            ev.jitter_x = tin->jitter_px;
            ev.jitter_y = -tin->jitter_py;
            ev.mv_scale_x = 1.0f;
            ev.mv_scale_y = 1.0f;
            ev.reset = reset ? 1 : 0;
            ev.render_w = uint32_t(tin->width);
            ev.render_h = uint32_t(tin->height);
            int32_t rc = p_gwEval(gwhandle, &ev, sizeof(ev));
            gwcopyerr();
            if(rc != SAUER_NGX_OK)
            {
                vkEndCommandBuffer(cmdbuf[fifslot]);
                return false;
            }
            evalok = true;
        }
    }

    loopi(nimg)
    {
        b[i].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        b[i].dstAccessMask = 0;
    }
    vkCmdPipelineBarrier(cmdbuf[fifslot], VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, NULL, 0, NULL, nimg, b);
    if(vkEndCommandBuffer(cmdbuf[fifslot]) != VK_SUCCESS)
    {
        setreason("vkEndCommandBuffer failed");
        return false;
    }
    if(hwrtdlaainject == 1)
    {
        setreason("injected: failure before vkQueueSubmit");
        return false;
    }

    vkResetFences(hwrtdev.device, 1, &fences[fifslot]);
    VkPipelineStageFlags waitstage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &vkglready;
    submit.pWaitDstStageMask = &waitstage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmdbuf[fifslot];
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &vkvkdone;
    VkResult sr = vkQueueSubmit(hwrtdev.queue, 1, &submit, fences[fifslot]);
    if(sr != VK_SUCCESS)
    {
        recreatesignaledfence(fifslot);
        defformatstring(msg, "vkQueueSubmit DLAA failed (%s)", hwrtresultstr(sr));
        setreason(msg);
        return false;
    }
    submitted = true;
    fifslot = (fifslot + 1) % DLAA_FIF;
    return evalok;
}

static void deactivate(const char *why)
{
    dlaaok = 0;
    hwrtdlaaok = 0;
    lastblit = false;
    ngxmodeapp = 0;
    if(why) setreason(why);
}

static void applyngxmode(int m);

static void dlaashotsdir();
static void dlaashotrel(char *dst, int len, const char *file);

static bool readwindowback(unsigned char *dst, int w, int h, GLint vp[4], int packalign)
{
    if(!dst || w < 1 || h < 1) return false;
    GLint prevreadfb = 0, prevread = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevreadfb);
    glGetIntegerv(GL_READ_BUFFER, &prevread);
    if(vp) glGetIntegerv(GL_VIEWPORT, vp);
    glBindFramebuffer_(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, packalign);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, dst);
    glBindFramebuffer_(GL_READ_FRAMEBUFFER, prevreadfb);
    if(prevread) glReadBuffer(GLenum(prevread));
    else glReadBuffer(GL_BACK);
    return true;
}

static void freeseqio()
{
    loopi(seqion)
    {
        delete[] seqios[i].inrgb;
        delete[] seqios[i].outrgb;
        delete[] seqios[i].depthvis;
        delete[] seqios[i].mvvis;
        delete[] seqios[i].biasrgb;
        seqios[i].inrgb = seqios[i].outrgb = seqios[i].depthvis = seqios[i].mvvis = seqios[i].biasrgb = NULL;
    }
    seqion = 0;
}

static void closeseqlog()
{
    if(seqlog) { fclose(seqlog); seqlog = NULL; }
}

static GLuint iofbo = 0;

static bool readtexfbo(GLuint tex, int w, int h, GLenum fmt, GLenum type, void *dst)
{
    if(!tex || !dst || w < 1 || h < 1) return false;
    if(!iofbo) glGenFramebuffers_(1, &iofbo);
    GLint prevread = 0, prevdraw = 0, prevreadbuf = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevread);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevdraw);
    glGetIntegerv(GL_READ_BUFFER, &prevreadbuf);
    glBindFramebuffer_(GL_READ_FRAMEBUFFER, iofbo);
    glFramebufferTexture2D_(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    GLenum st = glCheckFramebufferStatus_(GL_READ_FRAMEBUFFER);
    bool ok = false;
    if(st == GL_FRAMEBUFFER_COMPLETE)
    {
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        while(glGetError() != GL_NO_ERROR);
        glReadPixels(0, 0, w, h, fmt, type, dst);
        ok = glGetError() == GL_NO_ERROR;
    }
    glBindFramebuffer_(GL_READ_FRAMEBUFFER, prevread);
    if(prevdraw) glBindFramebuffer_(GL_DRAW_FRAMEBUFFER, prevdraw);
    if(prevreadbuf) glReadBuffer(GLenum(prevreadbuf));
    else glReadBuffer(GL_BACK);
    return ok;
}

static bool readgltexrgb(GLuint tex, int w, int h, unsigned char *dst)
{
    return readtexfbo(tex, w, h, GL_RGB, GL_UNSIGNED_BYTE, dst);
}

static unsigned char *readgltexgrey32f(GLuint tex, int w, int h)
{
    if(!tex || w < 1 || h < 1) return NULL;
    float *raw = new float[size_t(w) * size_t(h)];
    if(!readtexfbo(tex, w, h, GL_RED, GL_FLOAT, raw)) { delete[] raw; return NULL; }
    unsigned char *rgb = new unsigned char[size_t(w) * size_t(h) * 3];
    loopi(w * h)
    {
        float v = raw[i];
        if(!(v >= 0.0f)) v = 0.0f;
        if(v > 1.0f) v = 1.0f;
        int g = int(v * 255.0f + 0.5f);
        rgb[i*3] = rgb[i*3+1] = rgb[i*3+2] = (unsigned char)g;
    }
    delete[] raw;
    return rgb;
}

static unsigned char *readgltexmvvis(GLuint tex, int w, int h)
{
    if(!tex || w < 1 || h < 1) return NULL;
    float *raw = new float[size_t(w) * size_t(h) * 4];
    if(!readtexfbo(tex, w, h, GL_RGBA, GL_FLOAT, raw)) { delete[] raw; return NULL; }
    unsigned char *rgb = new unsigned char[size_t(w) * size_t(h) * 3];
    loopi(w * h)
    {
        float mx = raw[i*4], my = raw[i*4+1];
        int r = int(128.0f + mx * 8.0f);
        int g = int(128.0f + my * 8.0f);
        float mag = sqrtf(mx*mx + my*my);
        int b = int(mag * 16.0f);
        if(r < 0) r = 0; if(r > 255) r = 255;
        if(g < 0) g = 0; if(g > 255) g = 255;
        if(b < 0) b = 0; if(b > 255) b = 255;
        rgb[i*3] = (unsigned char)r;
        rgb[i*3+1] = (unsigned char)g;
        rgb[i*3+2] = (unsigned char)b;
    }
    delete[] raw;
    return rgb;
}

static bool seqiowanted(int idx)
{
    if(!seqpixels || idx < 0 || idx >= seqn) return false;
    if(hwrtdlaaiostep <= 0) return false;
    if(seqion >= DLAA_IOMAX) return false;
    if(idx == 0 || idx == seqn - 1) return true;
    return (idx % hwrtdlaaiostep) == 0;
}

static float rgbmean(const unsigned char *rgb, int w, int h)
{
    if(!rgb || w < 1 || h < 1) return -1;
    double s = 0;
    size_t n = size_t(w) * size_t(h);
    for(size_t i = 0; i < n; i++)
    {
        const unsigned char *p = rgb + i * 3;
        s += (double(p[0]) + double(p[1]) + double(p[2])) / 3.0;
    }
    return float(s / double(n));
}

static void captureseqio(int src)
{
    if(!seqiowanted(seqi)) return;
    int inw = 0, inh = 0, outw = 0, outh = 0;
    GLuint intex = 0, outtex = 0, ztex = 0, mvtex = 0;
    if(src == 1)
    {
        if(!texcolorin.gltex || !texcolorout.gltex) return;
        inw = lastpackw > 0 ? lastpackw : resinw;
        inh = lastpackh > 0 ? lastpackh : resinh;
        outw = resoutw > 0 ? resoutw : screenw;
        outh = resouth > 0 ? resouth : screenh;
        intex = texcolorin.gltex;
        outtex = texcolorout.gltex;
        ztex = texdepth.gltex;
        mvtex = texmotion.gltex;
    }
    else
    {
        const hwrttemporalinput *tin = hwrttemporalcurrent();
        if(!tin || !tin->valid || !tin->colortex) return;
        inw = tin->width;
        inh = tin->height;
        outw = screenw;
        outh = screenh;
        intex = tin->colortex;
        ztex = tin->depthtex;
        mvtex = tin->velocitytex;
    }
    if(inw < 8 || inh < 8) return;
    seqioshot &s = seqios[seqion];
    memset(&s, 0, sizeof(s));
    s.index = seqi;
    s.inw = inw;
    s.inh = inh;
    s.outw = outw;
    s.outh = outh;
    s.mode = int(hwrtngxmode);
    s.src = src;
    s.inrgb = new unsigned char[size_t(inw) * size_t(inh) * 3];
    if(!readgltexrgb(intex, inw, inh, s.inrgb)) { delete[] s.inrgb; s.inrgb = NULL; }
    lastinmean = s.inrgb ? rgbmean(s.inrgb, inw, inh) : -1;
    if(src == 1 && outtex && outw >= 8 && outh >= 8)
    {
        s.outrgb = new unsigned char[size_t(outw) * size_t(outh) * 3];
        if(!readgltexrgb(outtex, outw, outh, s.outrgb))
        {
            if(outw == screenw && outh == screenh) readwindowback(s.outrgb, outw, outh, NULL, 1);
            else { delete[] s.outrgb; s.outrgb = NULL; }
        }
        lastoutmean = s.outrgb ? rgbmean(s.outrgb, outw, outh) : -1;
    }
    else lastoutmean = -1;
    // Packed NGX depth is R32F; packed motion is RGBA16F. Off temporal
    // depth is D24 and must not be read as RED/FLOAT (GL_INVALID_OPERATION).
    if(src == 1)
    {
        if(ztex) s.depthvis = readgltexgrey32f(ztex, inw, inh);
        if(mvtex) s.mvvis = readgltexmvvis(mvtex, inw, inh);
        if(texbias.gltex && lastbiasbound)
        {
            s.biasrgb = new unsigned char[size_t(inw) * size_t(inh) * 3];
            if(!readgltexrgb(texbias.gltex, inw, inh, s.biasrgb))
            {
                delete[] s.biasrgb;
                s.biasrgb = NULL;
            }
        }
    }
    else if(mvtex) s.mvvis = readgltexmvvis(mvtex, inw, inh);
    if(s.inrgb || s.outrgb) seqion++;
    else
    {
        delete[] s.depthvis;
        delete[] s.mvvis;
        delete[] s.biasrgb;
        s.depthvis = s.mvvis = s.biasrgb = NULL;
    }
}

static void capturefallbackframe()
{
    if(!hwrtdlaacapturefallback || screenw < 8 || screenh < 8) return;
    string savedhome;
    copystring(savedhome, homedir);
    homedir[0] = '\0';
    dlaashotsdir();
    defformatstring(leaf, "fallback_%02d.png", fallbackn);
    string rel;
    dlaashotrel(rel, sizeof(rel), leaf);
    GLint vp[4] = {0, 0, 0, 0};
    ImageData image(screenw, screenh, 3);
    // Photograph only. The caller already stretched the reduced scene into the
    // window. Do not blit again and do not change the draw viewport — that
    // would either hide or reintroduce the HUD-size bug.
    readwindowback(image.data, screenw, screenh, vp, texalign(image.data, screenw, 3));
    saveimage(rel, 2, image, true);
    copystring(homedir, savedhome);
    conoutf("hwrt ngx: fallback frame captured %s  %dx%d (window before HUD, viewport %d %d %d %d, no blit)",
            rel, screenw, screenh, vp[0], vp[1], vp[2], vp[3]);
}

static void queuepresentshot(const char *name)
{
    presentpending = true;
    if(name && name[0]) copystring(presentname, name, sizeof(presentname));
    else copystring(presentname, "present", sizeof(presentname));
    for(char *s = presentname; *s; s++) if(*s == '/' || *s == '\\' || *s == ':' || iscubespace(*s)) *s = '_';
}

static void writepresentshot()
{
    if(!presentpending || screenw < 8 || screenh < 8)
    {
        presentpending = false;
        return;
    }
    presentpending = false;
    string savedhome;
    copystring(savedhome, homedir);
    homedir[0] = '\0';
    dlaashotsdir();
    defformatstring(leaf, "%s.png", presentname);
    string rel;
    dlaashotrel(rel, sizeof(rel), leaf);
    GLint vp[4] = {0, 0, 0, 0};
    ImageData image(screenw, screenh, 3);
    readwindowback(image.data, screenw, screenh, vp, texalign(image.data, screenw, 3));
    saveimage(rel, 2, image, true);
    defformatstring(txtleaf, "%s.txt", presentname);
    string txt;
    dlaashotrel(txt, sizeof(txt), txtleaf);
    FILE *f = fopen(findfile(txt, "w"), "w");
    if(f)
    {
        int match = (vp[0] == 0 && vp[1] == 0 && vp[2] == screenw && vp[3] == screenh) ? 1 : 0;
        fprintf(f, "present_after_hud %s size %dx%d\n", presentname, screenw, screenh);
        fprintf(f, "viewport %d %d %d %d  window %dx%d  matches_window %s\n",
                vp[0], vp[1], vp[2], vp[3], screenw, screenh, match ? "yes" : "no");
        fprintf(f, "observation READ_FRAMEBUFFER 0 only  blit no  viewport_modified no\n");
        fprintf(f, "capturefallback %d capturepresent %d inject %d fallbacks %d scenebound %s blocked %s\n",
                int(hwrtdlaacapturefallback), int(hwrtdlaacapturepresent), int(hwrtdlaainject),
                fallbackn, scenebound ? "yes" : "no", ngxblocked ? "yes" : "no");
        fprintf(f, "preference %d (%s) applied %d (%s) reason %s\n",
                int(hwrtngxmode), ngxmodename(hwrtngxmode), ngxmodeapp, ngxmodename(ngxmodeapp), ngxreason);
        fprintf(f, "hidehud %d showfps %d hudgun %d thirdperson %d player_state %d\n",
                getvar("hidehud"), getvar("showfps"), getvar("hudgun"), getvar("thirdperson"),
                player ? player->state : -1);
        fprintf(f, "zoom %d zoomprogress %.3f curfov %.3f curavatarfov %.3f\n",
                getvar("zoom"), getzoomprogress(), curfov, curavatarfov);
        fprintf(f, "current_scene %dx%d ngx_active %ux%u -> %ux%u\n",
                lastscenew, lastsceneh, dlaainw, dlaainh, dlaaoutw, dlaaouth);
        fclose(f);
    }
    copystring(homedir, savedhome);
    conoutf("hwrt ngx: present-after-hud %s  %dx%d viewport %d %d %d %d  match %s",
            rel, screenw, screenh, vp[0], vp[1], vp[2], vp[3],
            (vp[0] == 0 && vp[1] == 0 && vp[2] == screenw && vp[3] == screenh) ? "yes" : "no");
}

static void requestoff(const char *why)
{
    fallbackn++;
    deactivate(why);
    conoutf(CON_WARN, "hwrt %s: fallback (%s)  preference %s (%d) kept, applied OFF",
            fsrmode(hwrtngxmode) ? "fsr" : "ngx", ngxreason, ngxmodename(hwrtngxmode), int(hwrtngxmode));
    capturefallbackframe();
    if(hwrtdlaacapturepresent)
    {
        defformatstring(hudname, "fallback_hud_%02d", fallbackn);
        queuepresentshot(hudname);
    }
    ngxblocked = true;
    ngxmodeapp = 0;
    waitglcomplete();
    if(hwrtdev.device) vkDeviceWaitIdle(hwrtdev.device);
    releasefeature();
    qcacheok = false;
    hasprevunjit = false;
    destroyscenefb();
    hwrttemporalreset("ngx fallback");
    hwrtresetvelocity();
    ngxmodehave = hwrtngxmode;
    refreshlookaa();
}

// Why mode m runs as Native. Vulkan not started because it did not answer
// during an earlier start: the menu keeps the mode selectable (choosing it again
// retries), so the explanation is given here rather than as a lock reason.
static const char *ngxfallbackreason(int m)
{
    extern int hwrtvkstall;
    if(!hwrtvkready() && hwrtvkstall && hwrtvkcanstart() && !hwrtvkchecking())
        return "Vulkan did not answer during an earlier start, so it was not started this time. Choose this mode again to retry.";
    return fsrmode(m) ? hwrtfsrreason() : dlssreason();
}

// The player (menu, console, script) chose a Vulkan mode: start Vulkan now,
// with the bounded wait, if it is not up yet. Never from the per-frame path.
static void ngxensurevulkan(int m)
{
    if(m && ngxneedsfeature(m) && !hwrtvkready() && hwrtvkcanstart())
        hwrtvkensure(fsrmode(m) ? "FSR chosen" : (m == 1 ? "DLAA chosen" : "DLSS chosen"));
}

static void applyngxmode(int m)
{
    if(m < 0) m = 0;
    if(m > 8) m = 8;
    bool wasblocked = ngxblocked;
    ngxblocked = false;
    if(m && !modeready(m))
    {
        setreason(ngxfallbackreason(m));
        logoutf("%s: not available (%s)  preference %s kept, applied OFF",
                fsrmode(m) ? "FSR" : "NGX", ngxreason, ngxmodename(m));
        // A Vulkan give-up already told the player at start-up; say it again only when he asks.
        if(ngxreason[0] && !(ngxautoapply && hwrtvkstate == HWRT_VK_TIMEOUT)) conoutf(CON_WARN, "%s", ngxreason);
        ngxmodereq = m;
        ngxmodeapp = 0;
        ngxblocked = true;
        ngxmodehave = m;
        inchanged = true;
        hwrtngxmode = m;
        hwrtdlaa = 0;
        inchanged = false;
        releasefeature();
        destroyscenefb();
        hwrttemporalreset("ngx unavailable");
        hwrtresetvelocity();
        refreshlookaa();
        return;
    }
    int old = ngxmodehave;
    inchanged = true;
    hwrtngxmode = m;
    ngxmodereq = m;
    hwrtdlaa = (m == 1) ? 1 : 0;
    inchanged = false;
    if(old == m && !wasblocked) return;
    ngxmodehave = m;
    waitglcomplete();
    if(hwrtdev.device) vkDeviceWaitIdle(hwrtdev.device);
    releasefeature();
    ngxmodeapp = 0;
    ngxfeatfresh = 0;
    qcacheok = false;
    hasprevunjit = false;
    if(m == 0 || !ngxissrmode(m)) destroyscenefb();
    hwrttemporalreset(m == 0 ? "ngx off" : ngxmodename(m));
    hwrtresetvelocity();
    if(m == 0)
    {
        if(!ngxreason[0] || !strncmp(ngxreason, "active ", 7) || strstr(ngxreason, "inactive until"))
            deactivate("disabled");
        logoutf("NGX: OFF");
    }
    else logoutf("%s: %s", fsrmode(m) ? "FSR" : "NGX", ngxmodename(m));
    refreshlookaa();
}

static void hwrtdlaachanged()
{
    if(initing || inchanged) return;
    if(hwrtdlaa) { ngxensurevulkan(1); applyngxmode(1); }
    else if(hwrtngxmode == 1) applyngxmode(0);
}

static void hwrtngxmodechanged()
{
    if(initing || inchanged) return;
    ngxensurevulkan(hwrtngxmode);
    applyngxmode(hwrtngxmode);
}

bool hwrtdlaaneedsdata()
{
    return !ngxblocked && ngxneedsfeature(hwrtngxmode) && modeready(hwrtngxmode) && screenw > 0 && screenh > 0;
}

bool hwrtdlaaactive()
{
    return hwrtdlaaok != 0 && lastblit;
}

void hwrtdlaashutdown()
{
    waitglcomplete();
    if(hwrtdev.device) vkDeviceWaitIdle(hwrtdev.device);
    releasefeature();
    if(p_gwShutdown) p_gwShutdown();
    p_gwVer = NULL; p_gwProbe = NULL; p_gwPaths = NULL; p_gwLog = NULL;
    p_gwInstExt = NULL; p_gwDevExt = NULL; p_gwCaps = NULL; p_gwInit = NULL;
    p_gwCreate = NULL; p_gwCreateFeat = NULL; p_gwOptimal = NULL; p_gwEval = NULL; p_gwStats = NULL; p_gwRelease = NULL;
    p_gwShutdown = NULL; p_gwErr = NULL;
    gwhandle = 0;
    if(gwmod)
    {
        FreeLibrary(gwmod);
        gwmod = NULL;
    }
    slinited = false;
    ngxinited = false;
    ngxloaded = false;
    ngxsupported = false;
    dlaaavailable = 0;
    hwrtdlaaavailable = 0;
    hwrtngx = 0;
}

static void endcostifpending()
{
    if(costqok && costpending[costslot])
    {
        glEndQuery_(GL_TIME_ELAPSED);
        costpending[costslot] = false;
    }
}

static void hwrthdrkeepwindow(const char *why)
{
    if(scenehdr && scenecolorfmt == GL_RGBA16F && scenefbo && scenecolor && !hdrpresented)
        hwrthdrpresent();
    else if(scenefbo && !hdrpresented && !scenehdr)
        hwrtsceneblitwindow();
    requestoff(why);
}

static void writeexposuretex()
{
    if(!scenehdr || !texexposure.gltex || !packfbo) return;
    float stops = hwrthdrexp;
    if(stops < -8.0f) stops = -8.0f;
    if(stops > 8.0f) stops = 8.0f;
    float mul = powf(2.0f, stops);
    GLint prevfb = 0, prevdraw = 0, prevvp[4];
    GLfloat prevclear[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevfb);
    glGetIntegerv(GL_DRAW_BUFFER, &prevdraw);
    glGetIntegerv(GL_VIEWPORT, prevvp);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, prevclear);
    glBindFramebuffer_(GL_FRAMEBUFFER, packfbo);
    glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texexposure.gltex, 0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glViewport(0, 0, 1, 1);
    glClearColor(mul, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    GLenum expst = glCheckFramebufferStatus_(GL_FRAMEBUFFER);
    static int explog = 0;
    static float explogmul = -999.0f;
    if(!explog || expst != GL_FRAMEBUFFER_COMPLETE || explogmul != mul)
    {
        explog = 1;
        explogmul = mul;
        conoutf("hdr lot2 exposition FBO 0x%x stops %.3f valeur %.6f pre_exposure=1 exposure_scale=1 texture_R32F=%.6f AutoExposure=0 IsHDR=%d",
                int(expst), stops, mul, mul, ngxcfghdr);
    }
    glBindFramebuffer_(GL_FRAMEBUFFER, prevfb);
    if(prevdraw) glDrawBuffer(GLenum(prevdraw));
    glViewport(prevvp[0], prevvp[1], prevvp[2], prevvp[3]);
    glClearColor(prevclear[0], prevclear[1], prevclear[2], prevclear[3]);
}

static int hdrscale(int p, int src, int dst)
{
    if(src < 1 || dst < 1) return 0;
    int q = int((float(p) + 0.5f) * float(dst) / float(src));
    if(q < 0) q = 0;
    if(q >= dst) q = dst - 1;
    return q;
}

static bool hwrthdrreadtexel(GLuint tex, int x, int y, float rgba[4])
{
    static GLuint fb = 0;
    if(!fb) glGenFramebuffers_(1, &fb);
    GLint prev = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
    glBindFramebuffer_(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    while(glGetError() != GL_NO_ERROR);
    GLenum st = glCheckFramebufferStatus_(GL_FRAMEBUFFER);
    bool ok = st == GL_FRAMEBUFFER_COMPLETE && x >= 0 && y >= 0;
    if(ok) glReadPixels(x, y, 1, 1, GL_RGBA, GL_FLOAT, rgba);
    int err = int(glGetError());
    glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer_(GL_FRAMEBUFFER, prev);
    return ok && err == 0;
}

static const float HDR_STAMP_VAL[4] = { 0.18f, 1.0f, 4.0f, 16.0f };

static void hwrthdrngxread(int inw, int inh, int outw, int outh)
{
    int x0, y0, yc;
    hdrstamporigin(x0, y0, yc);
    bool sr = ngxissrmode(hwrtngxmode);
    conoutf("hdr lot2 ngx lecture in %dx%d vk 0x%x out %dx%d vk 0x%x IsHDR %d pre_exposure=1 exposure_scale=1",
            inw, inh, int(texcolorin.format), outw, outh, int(texcolorout.format), ngxcfghdr);
    loopi(4)
    {
        int sx = x0 + HDR_STAMP_DX[i];
        int sy = inh - 1 - yc;
        int ox = hdrscale(sx, inw, outw);
        int oy = hdrscale(sy, inh, outh);
        float tol = sr ? max(0.05f, 0.12f * HDR_STAMP_VAL[i]) : max(0.02f, 0.05f * HDR_STAMP_VAL[i]);
        float inrgba[4] = { -1, -1, -1, -1 };
        float outrgba[4] = { -1, -1, -1, -1 };
        bool inok = hwrthdrreadtexel(texcolorin.gltex, sx, sy, inrgba);
        bool outok = hwrthdrreadtexel(texcolorout.gltex, ox, oy, outrgba);
        float ein = fabsf(inrgba[0] - HDR_STAMP_VAL[i]);
        float eout = fabsf(outrgba[0] - HDR_STAMP_VAL[i]);
        bool pin = inok && ein <= tol && fabsf(inrgba[1] - HDR_STAMP_VAL[i]) <= tol && fabsf(inrgba[2] - HDR_STAMP_VAL[i]) <= tol;
        bool pout = outok && eout <= tol && fabsf(outrgba[1] - HDR_STAMP_VAL[i]) <= tol && fabsf(outrgba[2] - HDR_STAMP_VAL[i]) <= tol;
        conoutf("hdr lot2 ngx in %s (x %d y %d) %s got %.6f %.6f %.6f expect %.6f tol %.4f errabs %.6f",
                HDR_STAMP_NAME[i], sx, sy, pin ? "ok" : "FAIL", inrgba[0], inrgba[1], inrgba[2], HDR_STAMP_VAL[i], tol, ein);
        conoutf("hdr lot2 ngx out %s (x %d y %d) %s got %.6f %.6f %.6f expect %.6f tol %.4f errabs %.6f",
                HDR_STAMP_NAME[i], ox, oy, pout ? "ok" : "FAIL", outrgba[0], outrgba[1], outrgba[2], HDR_STAMP_VAL[i], tol, eout);
    }
}

static void hwrthdrngxwindow()
{
    int x0, y0, yc;
    hdrstamporigin(x0, y0, yc);
    glBindFramebuffer_(GL_FRAMEBUFFER, 0);
    loopi(4)
    {
        int x = hdrscale(x0 + HDR_STAMP_DX[i], scenew, screenw);
        int y = hdrscale(yc, sceneh, screenh);
        float rgba[4] = { -1, -1, -1, -1 };
        while(glGetError() != GL_NO_ERROR);
        glReadPixels(x, y, 1, 1, GL_RGBA, GL_FLOAT, rgba);
        int err = int(glGetError());
        conoutf("hdr lot2 ngx window %s (x %d y %d) %.6f %.6f %.6f gl 0x%x",
                HDR_STAMP_NAME[i], x, y, rgba[0], rgba[1], rgba[2], err);
    }
}

void hwrtdlaaframe()
{
    lastblit = false;
    dlaaok = 0;
    hwrtdlaaok = 0;
    if(!hwrtdlaaneedsdata()) return;
    if(drawtex) return;
    if(ngxissrmode(hwrtngxmode) && (scenefail || !scenebound || scenew < 8 || sceneh < 8))
    {
        hwrthdrkeepwindow(ngxreason[0] ? ngxreason : "SR internal scene missing");
        return;
    }
    const hwrttemporalinput *tin = hwrttemporalcurrent();
    int expectw = hwrtrenderw(), expecth = hwrtrenderh();
    if(!tin || !tin->valid || tin->width != expectw || tin->height != expecth)
    {
        hwrthdrkeepwindow("temporal packet invalid or size mismatch");
        return;
    }
    lasttframeid = tin->frameid;
    int outw = screenw, outh = screenh;
    if(ngxissrmode(hwrtngxmode) && tin->width >= outw && tin->height >= outh)
    {
        hwrthdrkeepwindow("SR internal size is not below the output");
        return;
    }
    if(!ensureres(tin->width, tin->height, outw, outh))
    {
        hwrthdrkeepwindow("could not create shared NGX images");
        return;
    }
    bool roundtrip = hwrtdlaaroundtrip != 0 && tin->width == outw && tin->height == outh;
    if(!roundtrip && !configure(tin->width, tin->height, outw, outh, hwrtngxmode))
    {
        hwrthdrkeepwindow(ngxreason);
        return;
    }
    harvestcost();
    double t0 = hwrtnow();
    if(costqok && !costpending[costslot])
    {
        glBeginQuery_(GL_TIME_ELAPSED, costq[costslot]);
        costpending[costslot] = true;
    }

    if(!packinputs(tin))
    {
        endcostifpending();
        hwrthdrkeepwindow(ngxreason);
        lastcostms = float((hwrtnow() - t0) * 1000.0);
        return;
    }
    writeexposuretex();

    GLuint texs[6];
    GLenum layouts[6];
    texs[0] = texcolorin.gltex;
    texs[1] = texdepth.gltex;
    texs[2] = texmotion.gltex;
    texs[3] = texcolorout.gltex;
    int ntex = 4;
    if(texbias.gltex)
    {
        texs[ntex] = texbias.gltex;
        layouts[ntex] = GL_LAYOUT_GENERAL_EXT;
        ntex++;
    }
    if(texexposure.gltex)
    {
        texs[ntex] = texexposure.gltex;
        layouts[ntex] = GL_LAYOUT_GENERAL_EXT;
        ntex++;
    }
    loopi(4) layouts[i] = GL_LAYOUT_GENERAL_EXT;
    dlaaSignalSem(glglready, 0, NULL, ntex, texs, layouts);
    glFlush();
    glsignaled = true;

    bool reset = tin->reset || !hasprevunjit;
    bool submitted = false;
    bool ok = submitvk(roundtrip, tin, reset, submitted);
    if(!ok)
    {
        if(glsignaled && !submitted)
        {
            if(!draininterop())
                conoutf(CON_WARN, "hwrt ngx: drain after failed submit: %s", ngxreason);
        }
        endcostifpending();
        evalfailn++;
        hwrthdrkeepwindow(ngxreason);
        lastcostms = float((hwrtnow() - t0) * 1000.0);
        return;
    }
    glsignaled = false;
    GLenum outlayout = GL_LAYOUT_GENERAL_EXT;
    dlaaWaitSem(glvkdone, 0, NULL, 1, &texcolorout.gltex, &outlayout);

    bool blitok;
    if(scenehdr && scenecolorfmt == GL_RGBA16F)
    {
        hwrthdrscheduleread();
        bool doread = hdrreadpending != 0;
        if(doread)
        {
            if(hwrthdrfam) hwrthdrfamrun();
            hwrthdrrealrun();
            hwrthdrsample(false);
            hwrthdrngxread(tin->width, tin->height, int(dlaaoutw ? dlaaoutw : uint32_t(outw)), int(dlaaouth ? dlaaouth : uint32_t(outh)));
        }
        bool useout = !ngxfeatfresh && !roundtrip && texcolorout.gltex;
        if(!useout)
            conoutf("hdr lot2 ngx premiere image: tone map de la scene lineaire (sortie de reset non presentee)");
        blitok = hwrthdrtonemap(useout ? texcolorout.gltex : scenecolor, useout);
        if(doread)
        {
            hwrthdrngxwindow();
            hdrreadpending = 0;
            if(getlogfile()) fflush(getlogfile());
            conoutf("hdr lot2 read_done");
        }
        if(blitok) hwrthdrpresentedone();
    }
    else if(ngxfeatfresh && !scenefbo)
    {
        // DLAA create frame: native colour is already in the window. A reset
        // evaluate can write black; keep that raster this one frame. NGX still
        // ran so the next evaluate has history.
        blitok = true;
    }
    else
    {
        blitok = blitoutput();
        if(blitok && ngxfeatfresh && scenefbo)
            hwrtsceneblitwindow();
    }
    lastfeatfresh = ngxfeatfresh;
    ngxfeatfresh = 0;
    if(costpending[costslot]) glEndQuery_(GL_TIME_ELAPSED);
    costslot = (costslot + 1) % DLAA_FIF;

    lastpackw = tin->width;
    lastpackh = tin->height;
    lastcostms = float((hwrtnow() - t0) * 1000.0);
    if(!blitok)
    {
        evalfailn++;
        hwrthdrkeepwindow(ngxreason[0] ? ngxreason : "present failed");
        return;
    }
    lastblit = true;
    prevunjit = tin->camproj_unjit;
    hasprevunjit = true;
    captureseqio(1);
    if(roundtrip)
    {
        dlaaok = 0;
        hwrtdlaaok = 0;
        ngxmodeapp = 0;
        setreason("round-trip copy (no NVIDIA evaluate)");
    }
    else
    {
        if(evalokn == 0)
            conoutf(CON_INIT, "hwrt %s ok  %ux%u -> %ux%u  mode %s used_optimal=%u",
                    fsrmode(ngxmodeapp) ? "fsr: ffxFsr3UpscalerContextDispatch" : "ngx: NGX_VULKAN_EVALUATE_DLSS_EXT",
                    dlaainw, dlaainh, dlaaoutw, dlaaouth, ngxmodename(ngxmodeapp), (unsigned)ngxusedoptimal);
        evalokn++;
        dlaaok = 1;
        hwrtdlaaok = 1;
        defformatstring(msg, "active %s %s %ux%u -> %ux%u eval ok", fsrmode(ngxmodeapp) ? "FSR" : "NGX", ngxmodename(ngxmodeapp ? ngxmodeapp : hwrtngxmode), dlaainw, dlaainh, dlaaoutw, dlaaouth);
        setreason(msg);
    }
}

static void samplemem()
{
    typedef BOOL (WINAPI *PFN_GPMI)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
    PFN_GPMI gpmi = (PFN_GPMI)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32GetProcessMemoryInfo");
    if(!gpmi)
    {
        HMODULE psapi = LoadLibraryA("psapi.dll");
        if(psapi) gpmi = (PFN_GPMI)GetProcAddress(psapi, "GetProcessMemoryInfo");
    }
    PROCESS_MEMORY_COUNTERS_EX pmc;
    memset(&pmc, 0, sizeof(pmc));
    pmc.cb = sizeof(pmc);
    if(gpmi && gpmi(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof(pmc)))
    {
        lastws = pmc.WorkingSetSize;
        lastpriv = pmc.PrivateUsage ? pmc.PrivateUsage : pmc.PagefileUsage;
    }
    lastgpuuse = lastgpubudget = 0;
    HMODULE dxgi = LoadLibraryA("dxgi.dll");
    if(dxgi)
    {
        typedef HRESULT (WINAPI *PFN_CREATE)(REFIID, void **);
        PFN_CREATE create = (PFN_CREATE)GetProcAddress(dxgi, "CreateDXGIFactory1");
        static const GUID iid_factory1 = { 0x770aae78, 0xf26f, 0x4dba, { 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 } };
        static const GUID iid_adapter3 = { 0x645967a4, 0x1392, 0x4310, { 0xa7, 0x98, 0x80, 0x53, 0x43, 0x4c, 0xc1, 0x4b } };
        IDXGIFactory1 *factory = NULL;
        if(create && SUCCEEDED(create(iid_factory1, (void **)&factory)) && factory)
        {
            IDXGIAdapter1 *ad = NULL;
            for(UINT i = 0; factory->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; i++)
            {
                IDXGIAdapter3 *ad3 = NULL;
                if(SUCCEEDED(ad->QueryInterface(iid_adapter3, (void **)&ad3)) && ad3)
                {
                    DXGI_QUERY_VIDEO_MEMORY_INFO inf{};
                    if(SUCCEEDED(ad3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &inf)))
                    {
                        if(inf.CurrentUsage > lastgpuuse)
                        {
                            lastgpuuse = inf.CurrentUsage;
                            lastgpubudget = inf.Budget;
                        }
                    }
                    ad3->Release();
                }
                ad->Release();
                ad = NULL;
            }
            factory->Release();
        }
        FreeLibrary(dxgi);
    }
    lastngxbytes = 0;
    if(p_gwStats && dlaaconfigured && gwhandle)
    {
        SauerNgxStats st;
        memset(&st, 0, sizeof(st));
        st.struct_bytes = sizeof(st);
        if(p_gwStats(gwhandle, &st, sizeof(st)) == SAUER_NGX_OK)
            lastngxbytes = st.vram_bytes;
    }
}

static void dlaashotsdir()
{
    string d1, d2, d3;
    copystring(d1, "shots");
    path(d1);
    createdir(d1);
    copystring(d2, "shots/dlss-q");
    path(d2);
    createdir(d2);
    if(ngxshotsub[0])
    {
        nformatstring(d3, sizeof(d3), "shots/dlss-q/%s", ngxshotsub);
        path(d3);
        createdir(d3);
    }
}

static void dlaashotrel(char *dst, int len, const char *file)
{
    if(ngxshotsub[0]) nformatstring(dst, len, "shots/dlss-q/%s/%s", ngxshotsub, file);
    else nformatstring(dst, len, "shots/dlss-q/%s", file);
    path(dst);
}

static void hwrtdlaashotdir(char *name)
{
    ngxshotsub[0] = 0;
    if(name && name[0])
    {
        char *d = ngxshotsub;
        const char *s = name;
        for(; *s && d < ngxshotsub + int(sizeof(ngxshotsub)) - 1; s++)
        {
            if((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '-' || *s == '_')
                *d++ = *s;
        }
        *d = 0;
    }
    dlaashotsdir();
    conoutf("hwrt ngx: proof dir shots/dlss-q/%s", ngxshotsub[0] ? ngxshotsub : "(root)");
}
COMMAND(hwrtdlaashotdir, "s");

static void hwrtdlaastats()
{
    conoutf("hwrt ngx: preference %s (%d)  applied %s (%d)  blocked %s  available %d  active %d  reason: %s",
            ngxmodename(hwrtngxmode), int(hwrtngxmode), ngxmodename(ngxmodeapp), ngxmodeapp,
            ngxblocked ? "yes" : "no", int(hwrtdlaaavailable), int(hwrtdlaaok), ngxreason);
    conoutf("  SDK NGX Vulkan public (sauer_ngx.dll)  inited %s  supported %s  configured %s  Streamline loaded %s",
            slinited ? "yes" : "no", ngxsupported ? "yes" : "no", dlaaconfigured ? "yes" : "no",
            GetModuleHandleA("sl.interposer.dll") ? "YES-BUG" : "no");
    conoutf("  current window %dx%d  current scene/RT %dx%d bound %s  (native when Off)",
            screenw, screenh, lastscenew, lastsceneh, hwrtsceneinternal() ? "yes" : "no");
    conoutf("  NGX active in %ux%u out %ux%u  (0x0 when feature released)  last NGX resources in %ux%u out %ux%u",
            dlaainw, dlaainh, dlaaoutw, dlaaouth, lastngxinw, lastngxinh, lastngxoutw, lastngxouth);
    conoutf("  allocated shared in %dx%d out %dx%d pack %dx%d  GET_OPTIMAL active %ux%u last %ux%u used_optimal %u  jitter samples %d",
            resinw, resinh, resoutw, resouth, lastpackw, lastpackh,
            ngxmodeapp ? ngxoptw : 0, ngxmodeapp ? ngxopth : 0,
            lastngxoptw, lastngxopth, (unsigned)ngxusedoptimal, hwrtdlaajitterseqlen());
    conoutf("  evaluate ok %d  fail %d  last %s  blit %s  fallbacks %d  inject %d",
            evalokn, evalfailn, lastevalstr(), lastblit ? "yes" : "no", fallbackn, int(hwrtdlaainject));
    conoutf("  CPU sandwich %.3f ms. GPU pack→blit %.3f ms last, %.3f ms mean of %d samples. Bias mask GPU %.3f ms last (before sandwich).",
            lastcostms, lastgpums, gpumean(), gpuringn, lastbiasgpums);
    conoutf("  present OpenGL SDL_GL_SwapWindow. NGX has no presentCommon. Create/ReleaseFeature/Shutdown1 through sauer_ngx.dll.");
    conoutf("  colour LDR UNORM RGBA8  depth R32F window 0-1 not inverted  MV prev-curr pixels Y-flipped for D3D  NGX InMVScale 1,1 (pixel space; not Streamline 1/w,1/h)");
    conoutf("  autonomy drivers: hwrtvelocity %d hwrtveldebug %d hwrtdlaajitter %d hwrtdlaaneedsdata %s",
            int(hwrtvelocity), int(hwrtveldebug), int(hwrtdlaajitter), hwrtdlaaneedsdata() ? "yes" : "no");
    conoutf("  contract %s err %.6g  (%s)", lastcontractok ? "PASS" : "n/a-or-FAIL", lastcontracterr, lastcontractnote);
    const hwrttemporalinput *t = hwrttemporalcurrent();
    if(t)
        conoutf("  packet valid %s frame %u reset %s  jitter px %.4f %.4f  looktaa %d skipped-if-dlaa",
                t->valid ? "yes" : "no", t->frameid, t->reset ? "yes" : "no",
                t->jitter_px, t->jitter_py, int(looktaa));
    conoutf("  bias mode %d persist %d bound %d src_fb %u %dx%d recreate %d persist_used %d needs %d gpu_ms %.3f",
            int(hwrtdlaabias), int(hwrtdlaabiaspersist), lastbiasbound,
            (unsigned)lastbiassrcfb, lastbiasw, lastbiash, lastbiasrecreate, lastbiaspersistused, lastbiasneeds, lastbiasgpums);
}
COMMAND(hwrtdlaastats, "");

static void hwrtngxstats() { hwrtdlaastats(); }
COMMAND(hwrtngxstats, "");

static void hwrtdlaatoggle()
{
    int next = hwrtngxmode + 1;
    if(next > 4) next = 0;
    applyngxmode(next);
}
COMMAND(hwrtdlaatoggle, "");

static void hwrtngxmodetoggle()
{
    hwrtdlaatoggle();
}
COMMAND(hwrtngxmodetoggle, "");

ICOMMAND(hwrtngxmodeapplied, "", (), intret(ngxmodeapp));
ICOMMAND(hwrtngxmodeblocked, "", (), intret(ngxblocked ? 1 : 0));
ICOMMAND(hwrtngxmodename, "i", (int *m), result(ngxmodename(*m)));
// Preference without a live feature is normal on the start screen.
// Only report fallback when NGX is actually blocked or missing.
ICOMMAND(hwrtngxfallback, "", (), intret((hwrtngxmode != 0 && ngxmodeapp == 0 && (ngxblocked || !modeready(hwrtngxmode))) ? 1 : 0));
ICOMMAND(hwrtngxreason, "", (), result(ngxreason));

// Short English line for the menu, the settings search and the assistant:
// why DLAA/DLSS cannot be chosen on this machine, "" when they can.
static const char *dlssreason()
{
    if(hwrtvkready() && dlaaavailable && ngxsupported && !ngxfatal) return "";
    if(!hwrtvkready())
    {
        static string msg;
        if(hwrtvkchecking()) return "DLAA/DLSS: checking the graphics card and driver...";
        if(hwrtvkstate == HWRT_VK_TIMEOUT)
        {
            formatstring(msg, "DLAA/DLSS are not available: %s", hwrtvkstallreason());
            return msg;
        }
        // Probe answered, Vulkan not started in the game yet: offered on NVIDIA.
        if(hwrtvkstate == HWRT_VK_PROBED && hwrtvkprobe.why == HWRT_WHY_NONE)
        {
            if(hwrtvkprobe.nvidia && !hwrtfsrsimulating("amd")) return "";
            if(hwrtfsrsimulating("amd")) return "DLAA/DLSS are not available: they need an NVIDIA RTX graphics card (simulated non-NVIDIA card).";
            formatstring(msg, "DLAA/DLSS are not available: they need an NVIDIA RTX graphics card (this one: %s).", hwrtvkprobe.name);
            return msg;
        }
    }
    if(!hwrtdev.device) return "DLAA/DLSS are not available: the graphics driver cannot share images between OpenGL and Vulkan.";
    if(ngxnotnvidia)
    {
        static string msg;
        if(hwrtfsrsimulating("amd")) copystring(msg, "DLAA/DLSS are not available: they need an NVIDIA RTX graphics card (simulated non-NVIDIA card).");
        else formatstring(msg, "DLAA/DLSS are not available: they need an NVIDIA RTX graphics card (this one: %s).", hwrtdev.name);
        return msg;
    }
    return "DLAA/DLSS are not available: they failed to start (NVIDIA driver or DLSS files, details in log.txt).";
}
ICOMMAND(hwrtdlssraison, "", (), result(dlssreason()));

static void hwrtdlaadump(char *name)
{
    string fname;
    if(name && name[0]) copystring(fname, name);
    else copystring(fname, "dlaa_dump");
    string savedhome;
    copystring(savedhome, homedir);
    homedir[0] = '\0';
    dlaashotsdir();
    string rel, leaf;
    nformatstring(leaf, sizeof(leaf), "%s.txt", fname);
    dlaashotrel(rel, sizeof(rel), leaf);
    FILE *f = fopen(findfile(rel, "w"), "w");
    if(!f)
    {
        copystring(homedir, savedhome);
        conoutf(CON_WARN, "hwrt dlaa: cannot write %s", rel);
        return;
    }
    fprintf(f, "preference %d (%s) applied %d (%s) intended_have %d blocked %s\n",
            int(hwrtngxmode), ngxmodename(hwrtngxmode), ngxmodeapp, ngxmodename(ngxmodeapp),
            ngxmodehave, ngxblocked ? "yes" : "no");
    fprintf(f, "hwrtdlaa requested %d available %d active %d\n", int(hwrtdlaa), int(hwrtdlaaavailable), int(hwrtdlaaok));
    fprintf(f, "hwrtdlaabias %d persist %d bound %d src_fb %u %dx%d recreate %d persist_used %d needs %d gpu_ms %.3f (0=off 1=visible explosion+smoke 2=all-ones consumption test)\n",
            int(hwrtdlaabias), int(hwrtdlaabiaspersist), lastbiasbound,
            (unsigned)lastbiassrcfb, lastbiasw, lastbiash, lastbiasrecreate, lastbiaspersistused, lastbiasneeds, lastbiasgpums);
    fprintf(f, "reason %s\n", ngxreason);
    fprintf(f, "sdk NVIDIA NGX Vulkan public nvsdk_ngx_s.lib via sauer_ngx.dll ABI %u\n", SAUER_NGX_ABI_VERSION);
    fprintf(f, "projectId %s  engine %s  EngineType CUSTOM\n", NGX_PROJECT_ID, NGX_ENGINE_VER);
    fprintf(f, "lifecycle Init_with_ProjectID / CREATE_DLSS_EXT1 / EVALUATE_DLSS_EXT Color+Depth+MV+Output[+exposure 1x1 in HDR with preset K] / ReleaseFeature / Shutdown1\n");
    fprintf(f, "present OpenGL SDL_GL_SwapWindow. NGX does not use presentCommon. Streamline loaded=%s\n",
            GetModuleHandleA("sl.interposer.dll") ? "yes" : "no");
    fprintf(f, "mode %s  preset %s  IsHDR %d  AutoExposure %d  depthInverted false  InMVScale 1,1 (render-pixel space)  MVLowRes  mip bias %.3f\n", ngxmodename(ngxmodeapp ? ngxmodeapp : hwrtngxmode), ngxpresetname(ngxmodeapp ? ngxmodeapp : hwrtngxmode),
            ngxcfghdr, ngxcfgautoexp, getscenelodbias());
    extern int windowmode, scr_w, scr_h, thirdperson, hwrtfreezeself;
    SDL_Rect bounds;
    bounds.w = bounds.h = 0;
    if(SDL_GetDisplayBounds(0, &bounds) < 0) bounds.w = bounds.h = 0;
    fprintf(f, "current_window %dx%d  current_scene_rt %dx%d  scene_bound %s  (native when applied OFF)\n",
            screenw, screenh, lastscenew, lastsceneh, hwrtsceneinternal() ? "yes" : "no");
    fprintf(f, "ngx_active_in %ux%u ngx_active_out %ux%u  (0x0 if feature released)\n", dlaainw, dlaainh, dlaaoutw, dlaaouth);
    fprintf(f, "ngx_last_in %ux%u ngx_last_out %ux%u ngx_last_GET_OPTIMAL %ux%u  (last configured resources, not current scene)\n",
            lastngxinw, lastngxinh, lastngxoutw, lastngxouth, lastngxoptw, lastngxopth);
    fprintf(f, "used_optimal %u GET_OPTIMAL_active %ux%u GET_OPTIMAL_last %ux%u size_ok %s  (active 0x0 when feature released; last is leftover, not the current scene)\n",
            (unsigned)ngxusedoptimal,
            ngxmodeapp ? ngxoptw : 0, ngxmodeapp ? ngxopth : 0,
            lastngxoptw, lastngxopth,
            (ngxmodeapp && dlaaoptok) ? "yes" : "n/a");
    fprintf(f, "windowmode %d scr_wh %dx%d desktop %dx%d  (windowed title bar shrinks the client; borderless uses desktop)\n",
            windowmode, scr_w, scr_h, bounds.w, bounds.h);
    fprintf(f, "player_state %d thirdperson %d hudgun %d hidehud %d hwrtfreezeself %d  (0=alive 1=dead 4=editing 5=spectator)\n",
            player ? player->state : -1, thirdperson, getvar("hudgun"), getvar("hidehud"), hwrtfreezeself);
    fprintf(f, "jitter_seqlen %d  mv_units render_pixels +Y up  packed_y_negated_for_d3d  InMVScale 1,1\n", hwrtdlaajitterseqlen());
    fprintf(f, "allocated_shared in %dx%d out %dx%d pack %dx%d  (may linger after Off; not the current scene size)\n",
            resinw, resinh, resoutw, resouth, lastpackw, lastpackh);
    fprintf(f, "evaluate ok %d fail %d last %s blit %s roundtrip %s fallbacks %d inject %d\n",
            evalokn, evalfailn, lastevalstr(), lastblit ? "yes" : "no", lastroundtrip ? "yes" : "no", fallbackn, int(hwrtdlaainject));
    fprintf(f, "cost_cpu_sandwich_ms %.4f  cost_gpu_gl_elapsed_ms_last %.4f  cost_gpu_gl_elapsed_ms_mean %.4f  samples %d  bias_gpu_ms %.4f (mask+depth blit, before sandwich)\n",
            lastcostms, lastgpums, gpumean(), gpuringn, lastbiasgpums);
    fprintf(f, "cost_scope GL_TIME_ELAPSED from pack through blit, including the GL wait on the Vulkan semaphore. Does not claim a dedicated NGX Evaluate GPU timestamp.\n");
    fprintf(f, "formats colorin/out VK_FORMAT_R8G8B8A8_UNORM GL_RGBA8  depth VK_FORMAT_R32_SFLOAT GL_R32F  motion VK_FORMAT_R16G16B16A16_SFLOAT\n");
    fprintf(f, "orientation pack flips V so memory row 0 is top of screen; blit flips back to GL window\n");
    fprintf(f, "motion engine prev-curr pixels +Y up; packed MV.y negated; NGX InMVScale 1,1 (already pixel space, not Streamline 1/w,1/h); motionVectorsJittered false\n");
    fprintf(f, "jitterOffset pixel D3D (engine.y negated)\n");
    fprintf(f, "depth mixed world/avatar after gun, 0 near 1 far, depthInverted false. Gun pixels keep avatar MVs (B=1), not world unproject.\n");
    fprintf(f, "uncovered B<0.5: camera-only MVs from mixed depth via world inv(camproj_used)->unjit curr/prev. Sky rotation covered. Water/particles/unvalidated: limitation.\n");
    fprintf(f, "B/A coverage is NOT a Streamline mask.\n");
    fprintf(f, "matrices cameraViewToClip = unjittered proj with clip Y negated (GL->D3D). clipToPrevClip = prev_camproj_Yflip * inv(curr_camproj_Yflip). tomat is transpose of engine columns, same as sl row-major.\n");
    fprintf(f, "autonomy hwrtvelocity %d hwrtveldebug %d hwrtdlaajitter %d needsdata %s\n",
            int(hwrtvelocity), int(hwrtveldebug), int(hwrtdlaajitter), hwrtdlaaneedsdata() ? "yes" : "no");
    runcontract(f);
    samplemem();
    fprintf(f, "mem_cpu_working_set_bytes %llu  mem_cpu_private_bytes %llu  mem_gpu_dxgi_current_bytes %llu  mem_gpu_dxgi_budget_bytes %llu  mem_ngx_dlss_bytes %llu\n",
            (unsigned long long)lastws, (unsigned long long)lastpriv, (unsigned long long)lastgpuuse,
            (unsigned long long)lastgpubudget, (unsigned long long)lastngxbytes);
    hwrttemporalfprint(f);
    fclose(f);
    copystring(homedir, savedhome);
    conoutf("hwrt dlaa: wrote %s", rel);
}
COMMAND(hwrtdlaadump, "s");

static void writeseq()
{
    if(!seqpixels) { seqn = seqi = seqw = seqh = 0; return; }
    if(seqi <= 0)
    {
        delete[] seqpixels;
        seqpixels = NULL;
        seqn = seqi = seqw = seqh = 0;
        closeseqlog();
        freeseqio();
        return;
    }
    string savedhome;
    copystring(savedhome, homedir);
    homedir[0] = '\0';
    dlaashotsdir();
    loopi(seqi)
    {
        defformatstring(leaf, "%s_%03d.png", seqname, i);
        string rel;
        dlaashotrel(rel, sizeof(rel), leaf);
        ImageData image(seqw, seqh, 3);
        memcpy(image.data, seqpixels + size_t(i) * size_t(seqw) * size_t(seqh) * 3, size_t(seqw) * size_t(seqh) * 3);
        saveimage(rel, 2, image, true);
    }
    loopi(seqion)
    {
        seqioshot &s = seqios[i];
        if(s.inrgb)
        {
            defformatstring(leaf, "io_%s_%03d_in_%dx%d_m%d_%s.png", seqname, s.index, s.inw, s.inh, s.mode, s.src ? "ngx" : "temporal");
            string rel;
            dlaashotrel(rel, sizeof(rel), leaf);
            ImageData image(s.inw, s.inh, 3);
            memcpy(image.data, s.inrgb, size_t(s.inw) * size_t(s.inh) * 3);
            saveimage(rel, 2, image, true);
        }
        if(s.outrgb)
        {
            defformatstring(leaf, "io_%s_%03d_out_%dx%d_m%d_ngx.png", seqname, s.index, s.outw, s.outh, s.mode);
            string rel;
            dlaashotrel(rel, sizeof(rel), leaf);
            ImageData image(s.outw, s.outh, 3);
            memcpy(image.data, s.outrgb, size_t(s.outw) * size_t(s.outh) * 3);
            saveimage(rel, 2, image, true);
        }
        if(s.depthvis)
        {
            defformatstring(leaf, "io_%s_%03d_depth_%dx%d_m%d.png", seqname, s.index, s.inw, s.inh, s.mode);
            string rel;
            dlaashotrel(rel, sizeof(rel), leaf);
            ImageData image(s.inw, s.inh, 3);
            memcpy(image.data, s.depthvis, size_t(s.inw) * size_t(s.inh) * 3);
            saveimage(rel, 2, image, true);
        }
        if(s.mvvis)
        {
            defformatstring(leaf, "io_%s_%03d_mv_%dx%d_m%d.png", seqname, s.index, s.inw, s.inh, s.mode);
            string rel;
            dlaashotrel(rel, sizeof(rel), leaf);
            ImageData image(s.inw, s.inh, 3);
            memcpy(image.data, s.mvvis, size_t(s.inw) * size_t(s.inh) * 3);
            saveimage(rel, 2, image, true);
        }
        if(s.biasrgb)
        {
            defformatstring(leaf, "io_%s_%03d_bias_%dx%d_m%d.png", seqname, s.index, s.inw, s.inh, s.mode);
            string rel;
            dlaashotrel(rel, sizeof(rel), leaf);
            ImageData image(s.inw, s.inh, 3);
            memcpy(image.data, s.biasrgb, size_t(s.inw) * size_t(s.inh) * 3);
            saveimage(rel, 2, image, true);
        }
    }
    defformatstring(txtleaf, "%s.txt", seqname);
    string txt;
    dlaashotrel(txt, sizeof(txt), txtleaf);
    FILE *f = fopen(findfile(txt, "w"), "w");
    if(f)
    {
        fprintf(f, "sequence %s frames %d size %dx%d consecutive glReadPixels after HUD compose, PNG written after the last frame (no mid-sequence PNG stall)\n",
                seqname, seqi, seqw, seqh);
        fprintf(f, "preference %d (%s) applied %d (%s) blocked %s scene %dx%d window %dx%d ngx %ux%u -> %ux%u\n",
                int(hwrtngxmode), ngxmodename(hwrtngxmode), ngxmodeapp, ngxmodename(ngxmodeapp),
                ngxblocked ? "yes" : "no", lastscenew, lastsceneh, screenw, screenh,
                dlaainw, dlaainh, dlaaoutw, dlaaouth);
        fprintf(f, "hwrtdlaa %d available %d active %d reason %s\n", int(hwrtdlaa), int(hwrtdlaaavailable), int(hwrtdlaaok), ngxreason);
        fprintf(f, "lab_fixedcurtime %d iostep %d io_frames %d hwrtpartlabclock %d hwrtdlaabias %d persist %d bound %d src_fb %u %dx%d recreate %d persist_used %d needs %d timeline %s_timeline.csv\n",
                hwrtdlaafixedcurtime, int(hwrtdlaaiostep), seqion, hwrtpartlabclock, lastbiasmode, int(hwrtdlaabiaspersist), lastbiasbound,
                (unsigned)lastbiassrcfb, lastbiasw, lastbiash, lastbiasrecreate, lastbiaspersistused, lastbiasneeds, seqname);
        fprintf(f, "io_note packed NGX colour is V-flipped vs GL window; Off dumps temporal HUD-less colour as 'in' (no NGX out). Depth is R32F->grey. MV vis is packed RG scaled, not a game overlay. bias is packed BiasCurrentColorMask (R).\n");
        if(seqi > 0 && seqw > 16 && seqh > 16)
        {
            int skyish = 0, n = 0;
            int y0 = seqh * 2 / 3;
            for(int y = y0; y < seqh; y += 8)
                for(int x = 0; x < seqw; x += 8)
                {
                    unsigned char *p = seqpixels + (size_t(seqh - 1 - y) * seqw + x) * 3;
                    n++;
                    if(p[2] > p[0] + 8 && p[2] > 40) skyish++;
                }
            fprintf(f, "upper_third_blueish_samples %d / %d (heuristic; inspect the PNGs)\n", skyish, n);
        }
        fclose(f);
    }
    copystring(homedir, savedhome);
    conoutf("hwrt dlaa: wrote %d consecutive frames %s", seqi, txt);
    delete[] seqpixels;
    seqpixels = NULL;
    seqn = seqi = seqw = seqh = 0;
    closeseqlog();
    freeseqio();
}

static int cmpfloatasc(const void *a, const void *b)
{
    float fa = *(const float *)a, fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

static float benchpctsorted(float *sorted, int n, float p)
{
    if(n <= 0) return 0;
    int idx = int(p * float(n - 1) + 0.5f);
    if(idx < 0) idx = 0;
    if(idx >= n) idx = n - 1;
    return sorted[idx];
}

static void benchcapturecam(float *o)
{
    if(!camera1)
    {
        loopi(5) o[i] = 0;
        return;
    }
    o[0] = camera1->o.x;
    o[1] = camera1->o.y;
    o[2] = camera1->o.z;
    o[3] = camera1->yaw;
    o[4] = camera1->pitch;
}

static void dlaabenchwrite()
{
    extern int vsync, maxfps, hwrt, hwrtstalls, hwrtdlaajitter;
    string savedhome;
    copystring(savedhome, homedir);
    homedir[0] = '\0';
    dlaashotsdir();
    string rel, leaf;
    nformatstring(leaf, sizeof(leaf), "%s.txt", benchname[0] ? benchname : "bench");
    dlaashotrel(rel, sizeof(rel), leaf);
    FILE *f = fopen(findfile(rel, "w"), "w");
    if(!f)
    {
        copystring(homedir, savedhome);
        conoutf(CON_WARN, "hwrt dlaa: cannot write bench %s", rel);
        benchphase = 0;
        hwrtdlaabenchdone = 1;
        return;
    }
    double wall = benchrunend > benchrunstart ? benchrunend - benchrunstart : 0;
    fprintf(f, "protocol timed-still-v2  (replaces 300-frame ~1s integer-ms benches; those files live in shots/dlaa-ngx/bench-300frames)\n");
    fprintf(f, "bench %s frames %d warmup_s %d measure_s %d output %dx%d render %dx%d ngx_active_in %ux%u ngx_active_out %ux%u ngx_last_in %ux%u ngx_last_out %ux%u mode_pref %d mode_app %d blocked %s\n",
            benchname, benchn, DLAA_BENCHWARMSEC, benchrunsec, screenw, screenh, lastscenew, lastsceneh,
            dlaainw, dlaainh, dlaaoutw, dlaaouth, lastngxinw, lastngxinh, lastngxoutw, lastngxouth,
            int(hwrtngxmode), ngxmodeapp, ngxblocked ? "yes" : "no");
    fprintf(f, "wall_s %.4f  global_fps frames/wall  clock SDL_GetPerformanceCounter via hwrtnow()\n", wall);
    fprintf(f, "hwrtngxmode %d applied %d hwrtdlaa %d available %d active %d needsdata %s used_optimal %u hwrtdlaabias %d persist %d bias_gpu_ms %.3f\n",
            int(hwrtngxmode), ngxmodeapp, int(hwrtdlaa), int(hwrtdlaaavailable), int(hwrtdlaaok),
            hwrtdlaaneedsdata() ? "yes" : "no", (unsigned)ngxusedoptimal, int(hwrtdlaabias), int(hwrtdlaabiaspersist), lastbiasgpums);
    fprintf(f, "hwrt %d hwrtvelocity %d hwrtdlaajitter %d hwrtveldebug %d looktaa %d\n",
            int(hwrt), int(hwrtvelocity), int(hwrtdlaajitter), int(hwrtveldebug), int(looktaa));
    fprintf(f, "hwrtvelspin %.4f hwrtvelforce %d  camera still, independent of DLAA/velocity\n",
            hwrtvelspin, int(hwrtvelforce));
    fprintf(f, "vsync %d maxfps %d stalls %d\n", int(vsync), int(maxfps), int(hwrtstalls));
    fprintf(f, "reason %s\n", ngxreason);
    fprintf(f, "cam_start %.3f %.3f %.3f yaw %.3f pitch %.3f\n",
            benchcam0[0], benchcam0[1], benchcam0[2], benchcam0[3], benchcam0[4]);
    fprintf(f, "cam_end %.3f %.3f %.3f yaw %.3f pitch %.3f\n",
            benchcam1[0], benchcam1[1], benchcam1[2], benchcam1[3], benchcam1[4]);
    float dyaw = benchcam1[3] - benchcam0[3], dpitch = benchcam1[4] - benchcam0[4];
    while(dyaw > 180) dyaw -= 360;
    while(dyaw < -180) dyaw += 360;
    fprintf(f, "cam_delta pos %.4f yaw %.4f pitch %.4f  (expect ~0: same still view in every state)\n",
            sqrtf((benchcam1[0]-benchcam0[0])*(benchcam1[0]-benchcam0[0]) +
                  (benchcam1[1]-benchcam0[1])*(benchcam1[1]-benchcam0[1]) +
                  (benchcam1[2]-benchcam0[2])*(benchcam1[2]-benchcam0[2])),
            dyaw, dpitch);
    fprintf(f, "elapsed_ms is high-res full-frame dt (hwrtnow between gl_drawframe samples, includes swap). Not getclockmillis 1ms ticks.\n");
    fprintf(f, "gpu_dlaa_ms is GL_TIME_ELAPSED pack-through-blit, 0 when DLAA is off. hwrt_gpu_ms is RT interop timestamps, not including DLAA. vel_play_ms is velocity GPU copy+gen+obj+skel.\n");
    fprintf(f, "i elapsed_ms cpu_dlaa_ms gpu_dlaa_ms hwrt_frame_ms hwrt_gpu_ms vel_play_ms active needs stalls dlaa\n");
    loopi(benchn)
    {
        const dlaabenchsample &s = benchsamp[i];
        fprintf(f, "%d %.4f %.4f %.4f %.4f %.4f %.4f %d %d %d %d\n",
                i, s.elapsed_ms, s.cpu_ms, s.gpu_ms, s.hwrt_frame_ms, s.hwrt_gpu_ms, s.vel_play_ms,
                s.active, s.needs, s.stalls, s.dlaa);
    }
    if(benchn > 0)
    {
        float *el = new float[benchn];
        double sume = 0, suminv = 0, sumg = 0, sumc = 0, sumh = 0, sumv = 0;
        float mine = benchsamp[0].elapsed_ms, maxe = benchsamp[0].elapsed_ms;
        int hitches50 = 0;
        loopi(benchn)
        {
            float e = benchsamp[i].elapsed_ms;
            if(e < 0.001f) e = 0.001f;
            el[i] = e;
            sume += e;
            suminv += 1000.0 / double(e);
            sumg += benchsamp[i].gpu_ms;
            sumc += benchsamp[i].cpu_ms;
            sumh += benchsamp[i].hwrt_gpu_ms;
            sumv += benchsamp[i].vel_play_ms;
            if(e < mine) mine = e;
            if(e > maxe) maxe = e;
            if(e > 50.0f) hitches50++;
        }
        qsort(el, benchn, sizeof(float), cmpfloatasc);
        float p50 = benchpctsorted(el, benchn, 0.50f);
        float p95 = benchpctsorted(el, benchn, 0.95f);
        float p99 = benchpctsorted(el, benchn, 0.99f);
        double globalfps = wall > 0 ? double(benchn) / wall : 0;
        double meanms = wall > 0 ? 1000.0 * wall / double(benchn) : (benchn ? sume / benchn : 0);
        fprintf(f, "summary_global_fps %.2f  (frames %d / wall_s %.4f)  THIS is the overall average FPS\n",
                globalfps, benchn, wall);
        fprintf(f, "summary_mean_frame_ms %.4f  (wall_s / frames, same information as global_fps)\n", meanms);
        fprintf(f, "summary_frame_ms min %.4f p50 %.4f p95 %.4f p99 %.4f max %.4f\n",
                mine, p50, p95, p99, maxe);
        fprintf(f, "summary_mean_of_instant_fps %.2f  (mean of 1/dt per frame; NOT overall FPS; was the old summary_fps avg)\n",
                suminv / benchn);
        fprintf(f, "summary_dlaa_gpu_ms avg %.4f  dlaa_cpu_ms avg %.4f  hwrt_gpu_ms avg %.4f  vel_play_ms avg %.4f\n",
                sumg / benchn, sumc / benchn, sumh / benchn, sumv / benchn);
        fprintf(f, "hitches_over_50ms %d of %d  (kept in raw rows above; a normal gpu_dlaa_ms does not explain a long pause)\n",
                hitches50, benchn);
        int nfilt = 0;
        double wallfilt = 0;
        loopi(benchn) if(benchsamp[i].elapsed_ms <= 50.0f)
        {
            nfilt++;
            wallfilt += benchsamp[i].elapsed_ms / 1000.0;
        }
        if(nfilt > 0 && wallfilt > 0)
            fprintf(f, "filtered_drop_frames_over_50ms  frames %d  wall_s %.4f  global_fps %.2f  mean_ms %.4f  (exactly: drop dt>50ms, then frames/sum(dt); hides hitches, does not prove NGX innocence)\n",
                    nfilt, wallfilt, double(nfilt) / wallfilt, 1000.0 * wallfilt / double(nfilt));
        else
            fprintf(f, "filtered_drop_frames_over_50ms  none kept\n");
        delete[] el;
    }
    fclose(f);
    copystring(homedir, savedhome);
    conoutf("hwrt dlaa: bench wrote %d frames in %.2fs %s", benchn, wall, rel);
    benchphase = 0;
    hwrtdlaabenchdone = 1;
}

static void dlaabenchstep()
{
    if(!benchphase) return;
    extern int hwrtstalls;
    extern float hwrtvelcopycost, hwrtvelgencost, hwrtvelobjcost, hwrtvelskelcost;
    double now = hwrtnow();
    if(benchphase == 1)
    {
        if(now < benchwarmuntil) return;
        benchphase = 2;
        benchn = 0;
        benchrunstart = now;
        benchrununtil = now + double(benchrunsec);
        benchprev = now;
        benchcapturecam(benchcam0);
        benchcamgot = 1;
        conoutf("hwrt dlaa: bench measure %d s (%s)", benchrunsec, benchname);
        return;
    }
    float dtms = float((now - benchprev) * 1000.0);
    benchprev = now;
    if(benchn < DLAA_BENCHMAX)
    {
        dlaabenchsample &s = benchsamp[benchn];
        s.elapsed_ms = dtms > 0 ? dtms : 0.001f;
        s.cpu_ms = hwrtdlaaneedsdata() ? lastcostms : 0;
        s.gpu_ms = hwrtdlaaneedsdata() ? lastgpums : 0;
        s.hwrt_frame_ms = hwrttime.frame;
        s.hwrt_gpu_ms = hwrttime.gptotal;
        s.vel_play_ms = hwrtvelcopycost + hwrtvelgencost + hwrtvelobjcost + hwrtvelskelcost;
        s.active = hwrtdlaaok && lastblit ? 1 : 0;
        s.needs = hwrtdlaaneedsdata() ? 1 : 0;
        s.stalls = hwrtstalls;
        s.dlaa = int(hwrtngxmode);
        benchn++;
    }
    if(now >= benchrununtil || benchn >= DLAA_BENCHMAX)
    {
        benchrunend = now;
        benchcapturecam(benchcam1);
        dlaabenchwrite();
    }
}

void hwrtdlaaflushshot()
{
    dlaabenchstep();
}

void hwrtdlaaafterhud()
{
    if(seqpixels && seqi < seqn && screenw >= 8 && screenh >= 8)
    {
        if(seqw != screenw || seqh != screenh) writeseq();
        else
        {
            if(hwrtngxmode == 0) captureseqio(0);
            if(seqlog)
            {
                hwrtpartlabframe st;
                hwrtpartlabstats(st);
                const hwrttemporalinput *tin = hwrttemporalcurrent();
                fprintf(seqlog, "%d %d %d %d %d %s %d %d %d %d %.6f %.6f %d %d %d %d %d %d %d %.4f %d %.3f %.3f %.3f %d %d %s %u %d %d %d %d %d %d %.3f %.3f %d\n",
                        seqi, lastmillis, curtime, elapsedtime, int(hwrtngxmode), ngxmodename(hwrtngxmode),
                        tin && tin->valid ? tin->width : 0, tin && tin->valid ? tin->height : 0,
                        screenw, screenh,
                        tin && tin->valid ? tin->jitter_px : 0, tin && tin->valid ? tin->jitter_py : 0,
                        tin && tin->valid && tin->reset ? 1 : 0,
                        st.nexp, st.ageexp_min, st.ageexp_max,
                        st.nsmoke, st.agesmoke_min, st.agesmoke_max,
                        st.growth, st.inside, st.ox, st.oy, st.oz, st.kind, st.dist,
                        lastblit ? "ngxblit" : "native",
                        (unsigned)lastbiassrcfb, lastbiasw, lastbiash, lastbiasrecreate, lastbiaspersistused, lastbiasneeds, lastbiasbound,
                        lastinmean, lastoutmean, lastfeatfresh);
                fflush(seqlog);
            }
            readwindowback(seqpixels + size_t(seqi) * size_t(seqw) * size_t(seqh) * 3, seqw, seqh, NULL, 1);
            seqi++;
            if(seqi >= seqn) writeseq();
        }
    }
    if(presentpending) writepresentshot();
}

static void hwrtdlaaseq(char *name, int *n)
{
    int want = *n;
    if(want < 1) want = 30;
    if(want > DLAA_SEQMAX) want = DLAA_SEQMAX;
    if(seqpixels) writeseq();
    if(screenw < 8 || screenh < 8) return;
    copystring(seqname, (name && name[0]) ? name : "seq", sizeof(seqname));
    for(char *s = seqname; *s; s++) if(*s == '/' || *s == '\\' || *s == ':' || iscubespace(*s)) *s = '_';
    seqw = screenw;
    seqh = screenh;
    seqn = want;
    seqi = 0;
    seqpixels = new unsigned char[size_t(seqn) * size_t(seqw) * size_t(seqh) * 3];
    {
        string savedhome;
        copystring(savedhome, homedir);
        homedir[0] = '\0';
        dlaashotsdir();
        defformatstring(leaf, "%s_timeline.csv", seqname);
        string rel;
        dlaashotrel(rel, sizeof(rel), leaf);
        seqlog = fopen(findfile(rel, "w"), "w");
        copystring(homedir, savedhome);
        if(seqlog)
        {
            fprintf(seqlog, "frame lastmillis curtime elapsed_ms mode mode_name inw inh outw outh jitter_px jitter_py reset nexp ageexp_min ageexp_max nsmoke agesmoke_min agesmoke_max growth inside ox oy oz kind dist path src_fb bias_w bias_h recreate persist_used needs bound in_mean out_mean featfresh\n");
            fflush(seqlog);
        }
    }
    conoutf("hwrt dlaa: capturing %d consecutive frames as %s (no PNG until complete, timeline+io after last frame)", seqn, seqname);
}
COMMAND(hwrtdlaaseq, "si");

static void hwrtdlaalabalign()
{
    // Same initial temporal/jitter/bias persist for A/B. This sets tin->reset
    // so NGX history starts empty on both sides. Lab only.
    hwrttemporalreset("lab align");
    hwrtresetvelocity();
    hwrtdlaabiasreset();
    conoutf("hwrt ngx: lab align (jitter sequence, velocity, bias persist; NGX reset flag)");
}
COMMAND(hwrtdlaalabalign, "");
ICOMMAND(hwrtdlaabiassrcfb, "", (), intret(lastbiassrcfb));
ICOMMAND(hwrtdlaabiasrecreate, "", (), intret(lastbiasrecreate));
ICOMMAND(hwrtdlaabiaspersistused, "", (), intret(lastbiaspersistused));

static void hwrtdlaashot(char *name)
{
    string fname;
    if(name && name[0]) copystring(fname, name);
    else copystring(fname, "dlaa");
    for(char *s = fname; *s; s++) if(*s == '/' || *s == '\\' || *s == ':' || iscubespace(*s)) *s = '_';
    hwrtdlaadump(fname);
    if(screenw < 8 || screenh < 8) return;
    string savedhome;
    copystring(savedhome, homedir);
    homedir[0] = '\0';
    dlaashotsdir();
    string rel, leaf;
    nformatstring(leaf, sizeof(leaf), "%s.png", fname);
    dlaashotrel(rel, sizeof(rel), leaf);
    GLint prevread = 0;
    glGetIntegerv(GL_READ_BUFFER, &prevread);
    glReadBuffer(GL_FRONT);
    ImageData image(screenw, screenh, 3);
    glPixelStorei(GL_PACK_ALIGNMENT, texalign(image.data, screenw, 3));
    glReadPixels(0, 0, screenw, screenh, GL_RGB, GL_UNSIGNED_BYTE, image.data);
    if(prevread) glReadBuffer(GLenum(prevread));
    else glReadBuffer(GL_BACK);
    saveimage(rel, 2, image, true);
    copystring(homedir, savedhome);
    conoutf("hwrt dlaa: shot %s  requested %d available %d active %d  %s",
            rel, int(hwrtdlaa), int(hwrtdlaaavailable), int(hwrtdlaaok), ngxreason);
}
COMMAND(hwrtdlaashot, "s");

static void hwrtdlaapresentshot(char *name)
{
    queuepresentshot((name && name[0]) ? name : "present");
    conoutf("hwrt ngx: next presented frame after HUD will be saved as %s (read-only, no blit)", presentname);
}
COMMAND(hwrtdlaapresentshot, "s");

static void hwrtdlaacontract()
{
    string savedhome;
    copystring(savedhome, homedir);
    homedir[0] = '\0';
    dlaashotsdir();
    string rel;
    dlaashotrel(rel, sizeof(rel), "contract.txt");
    FILE *f = fopen(findfile(rel, "w"), "w");
    if(f)
    {
        runcontract(f);
        fclose(f);
    }
    else runcontract(NULL);
    copystring(homedir, savedhome);
    conoutf("hwrt dlaa: contract %s", lastcontractnote);
}
COMMAND(hwrtdlaacontract, "");

static void hwrtdlaamem()
{
    samplemem();
    string savedhome;
    copystring(savedhome, homedir);
    homedir[0] = '\0';
    dlaashotsdir();
    string rel;
    dlaashotrel(rel, sizeof(rel), "stability-mem.txt");
    const char *pathw = findfile(rel, "w");
    FILE *exist = fopen(findfile(rel, "r"), "r");
    bool header = exist == NULL;
    if(exist) fclose(exist);
    FILE *f = fopen(pathw, header ? "w" : "a");
    if(!f)
    {
        copystring(homedir, savedhome);
        conoutf(CON_WARN, "hwrt dlaa: cannot write %s", rel);
        return;
    }
    if(header)
        fprintf(f, "time_ms requested active eval_ok eval_fail fallbacks cpu_ws cpu_priv gpu_dxgi ngx_dlss reason\n");
    fprintf(f, "%.0f %d %d %d %d %d %llu %llu %llu %llu %s\n",
            hwrtnow() * 1000.0, int(hwrtdlaa), int(hwrtdlaaok), evalokn, evalfailn, fallbackn,
            (unsigned long long)lastws, (unsigned long long)lastpriv,
            (unsigned long long)lastgpuuse, (unsigned long long)lastngxbytes, ngxreason);
    fclose(f);
    copystring(homedir, savedhome);
    conoutf("hwrt dlaa: mem CPU ws %.2f MiB  private %.2f MiB  GPU DXGI %.2f MiB  NGX %.2f MiB  eval %d/%d  %s",
            lastws / (1024.0*1024.0), lastpriv / (1024.0*1024.0), lastgpuuse / (1024.0*1024.0),
            lastngxbytes / (1024.0*1024.0), evalokn, evalfailn, ngxreason);
}
COMMAND(hwrtdlaamem, "");

static void hwrtdlaabench(char *name, int *n)
{
    int sec = *n;
    if(sec < 5) sec = 25;
    if(sec > 60) sec = 60;
    if(benchphase)
    {
        conoutf(CON_WARN, "hwrt dlaa: bench already running (%s)", benchname);
        return;
    }
    copystring(benchname, (name && name[0]) ? name : "bench", sizeof(benchname));
    for(char *s = benchname; *s; s++) if(*s == '/' || *s == '\\' || *s == ':' || iscubespace(*s)) *s = '_';
    benchrunsec = sec;
    benchn = 0;
    benchcamgot = 0;
    benchrunstart = benchrunend = benchprev = 0;
    benchwarmuntil = hwrtnow() + double(DLAA_BENCHWARMSEC);
    benchphase = 1;
    hwrtdlaabenchdone = 0;
    extern int maxfps, vsync;
    conoutf("hwrt dlaa: bench warmup %ds then %ds as %s (maxfps %d vsync %d, high-res hwrtnow, still camera)",
            DLAA_BENCHWARMSEC, benchrunsec, benchname, int(maxfps), int(vsync));
}
COMMAND(hwrtdlaabench, "si");

static void holdcam(const vec &o, float yaw, float pitch)
{
    execute("paused 0");
    execute("spectator 0");
    if(editmode) toggleedit(false);
    setvar("hudgun", 1);
    setvar("hudgunsway", 0);
    setvar("hwrtvelholdcam", 0);
    setvar("hwrtvelholdplayer", 0);
    extern int thirdperson;
    thirdperson = 0;
    if(player)
    {
        player->o = o;
        player->yaw = yaw;
        player->pitch = pitch;
        player->vel = vec(0, 0, 0);
        player->falling = vec(0, 0, 0);
        player->move = 0;
        player->strafe = 0;
        player->resetinterp();
    }
    if(camera1)
    {
        camera1->o = o;
        camera1->yaw = yaw;
        camera1->pitch = pitch;
    }
}

static void hwrtdlaabenchpose()
{
    hwrtvelspin = 0;
    hwrtvelpitchspin = 0;
    hwrtvelslide = 0;
    hwrtvelpush = 0;
    hwrtvelwalk = 0;
    hwrtvelsmoke = 0;
    hwrtvelfreezepose = 0;
    hwrtvelforce = 0;
    hwrtveldebug = 0;
    setvar("hwrtvelocity", 0, true);
    setvar("hwrtdlaajitter", 0, true);
    execute("hwrtvelsetpos 668 591 566 180 -8");
    conoutf("hwrt dlaa: still bench pose held at 668 591 566 yaw 180 pitch -8 (hwrtvelholdcam/player 1, no spin)");
}
COMMAND(hwrtdlaabenchpose, "");

static float identfloat(const char *name, float fallback)
{
    ident *id = getident(name);
    if(!id) return fallback;
    if(id->type == ID_FVAR) return *id->storage.f;
    if(id->type == ID_VAR) return float(*id->storage.i);
    return fallback;
}

static void hwrtdlaaplaycheck(char *name)
{
    string savedhome;
    copystring(savedhome, homedir);
    homedir[0] = '\0';
    dlaashotsdir();
    string fname;
    if(name && name[0]) copystring(fname, name);
    else copystring(fname, "play-check");
    for(char *s = fname; *s; s++) if(*s == '/' || *s == '\\' || *s == ':' || iscubespace(*s)) *s = '_';
    string rel, leaf;
    nformatstring(leaf, sizeof(leaf), "%s.txt", fname);
    dlaashotrel(rel, sizeof(rel), leaf);
    FILE *f = fopen(findfile(rel, "w"), "w");
    if(!f)
    {
        copystring(homedir, savedhome);
        conoutf(CON_WARN, "hwrt dlaa: cannot write %s", rel);
        return;
    }
    ident *sens = getident("sensitivity");
    fprintf(f, "playcheck %s size %dx%d\n", name && name[0] ? name : "play-check", screenw, screenh);
    fprintf(f, "hidehud %d showfps %d hudgun %d hudgunsway %d thirdperson %d zoom %d zoomprogress %.3f curfov %.3f\n",
            getvar("hidehud"), getvar("showfps"), getvar("hudgun"), getvar("hudgunsway"), getvar("thirdperson"),
            getvar("zoom"), getzoomprogress(), curfov);
    fprintf(f, "sensitivity %.4f fov %d maxfps %d vsync %d looktaa %d\n",
            sens && sens->type == ID_FVAR ? *sens->storage.f : identfloat("sensitivity", -1),
            getvar("fov"), getvar("maxfps"), getvar("vsync"), int(looktaa));
    fprintf(f, "hwrt %d hwrtdebug %d hwrtlight %d hwrtshade %d hwrtshadowself %d lookselfshadow %d hwrtdlights %d hwrtdlaajitterrt %d\n",
            getvar("hwrt"), getvar("hwrtdebug"), getvar("hwrtlight"), getvar("hwrtshade"),
            getvar("hwrtshadowself"), getvar("lookselfshadow"), getvar("hwrtdlights"), int(hwrtdlaajitterrt));
    // Same winner rules as hwrtrender: debug 7 / hwrtlight => hitlight composite.
    // hwrt 1 alone does not certify RT lighting on screen.
    const char *displayed = "gl-or-overlay";
    if(getvar("hwrtdebug") == 7 || getvar("hwrtlight")) displayed = "rt-lighting";
    else if(getvar("hwrtdebug") == 6 || getvar("hwrtshade")) displayed = "rt-shade";
    else if(getvar("hwrtdebug") > 0) displayed = "rt-diagnostic";
    fprintf(f, "displayed_rt %s hwrtdiffmip %d hwrtdiffvis %d hwrtskystable %d hwrtskyhold %d hwrtskyhistfilter %d hwrtskyage %d hwrtskyvisdbg %d hwrtnrd %d nrd_session %d nrd_dbg %d\n",
            displayed, getvar("hwrtdiffmip"), getvar("hwrtdiffvis"), getvar("hwrtskystable"),
            getvar("hwrtskyhold"), getvar("hwrtskyhistfilter"), getvar("hwrtskyage"), getvar("hwrtskyvisdbg"),
            getvar("hwrtnrd"), hwrtnrdsession() ? 1 : 0, getvar("hwrtnrddbg"));
    {
        int hw = 0, hh = 0;
        hwrtskyhistsize(hw, hh);
        fprintf(f, "sky packed_rays %.2f cvar_rays %d eff_rays %d filter %d temporal %d hist %dx%d io %dx%d ready %d\n",
                float(hwrtskyrayseffective()) + (getvar("hwrtskyfilter") ? 0.25f : 0.0f),
                getvar("hwrtskyrays"), hwrtskyrayseffective(), getvar("hwrtskyfilter"), getvar("hwrtskytemporal"),
                hw, hh, hwrtio.w, hwrtio.h, hwrtskyhistready() ? 1 : 0);
    }
    fprintf(f, "hwrtngxmode preference %d applied %d blocked %s hwrtdlaa %d available %d active %d needsdata %s reason %s\n",
            int(hwrtngxmode), ngxmodeapp, ngxblocked ? "yes" : "no", int(hwrtdlaa), int(hwrtdlaaavailable), int(hwrtdlaaok),
            hwrtdlaaneedsdata() ? "yes" : "no", ngxreason);
    fprintf(f, "current_scene %dx%d current_window %dx%d ngx_active %ux%u -> %ux%u ngx_last %ux%u -> %ux%u used_optimal %u\n",
            lastscenew, lastsceneh, screenw, screenh, dlaainw, dlaainh, dlaaoutw, dlaaouth,
            lastngxinw, lastngxinh, lastngxoutw, lastngxouth, (unsigned)ngxusedoptimal);
    fprintf(f, "player_state %d hwrtfreezeself %d\n", player ? player->state : -1, getvar("hwrtfreezeself"));
    fprintf(f, "hwrtvelocity %d hwrtdlaajitter %d hwrtveldebug %d hwrtvelforce %d\n",
            int(hwrtvelocity), int(hwrtdlaajitter), int(hwrtveldebug), int(hwrtvelforce));
    fprintf(f, "hwrtvelspin %.4f hwrtvelslide %.4f hwrtvelpush %.4f hwrtvelfreezepose %d hwrtvelholdcam %d\n",
            hwrtvelspin, hwrtvelslide, hwrtvelpush, int(hwrtvelfreezepose), int(hwrtvelholdcam));
    fprintf(f, "playermodel %d forceplayermodels %d\n", getvar("playermodel"), getvar("forceplayermodels"));
    if(camera1)
        fprintf(f, "camera %.3f %.3f %.3f yaw %.3f pitch %.3f\n",
                camera1->o.x, camera1->o.y, camera1->o.z, camera1->yaw, camera1->pitch);
    fclose(f);
    copystring(homedir, savedhome);
    conoutf("hwrt ngx: playcheck %s  preference %s applied %s blocked %s  shadowself %d  sensitivity %.2f  hidehud %d showfps %d",
            rel, ngxmodename(hwrtngxmode), ngxmodename(ngxmodeapp), ngxblocked ? "yes" : "no",
            getvar("hwrtshadowself"), identfloat("sensitivity", 0), getvar("hidehud"), getvar("showfps"));
}
COMMAND(hwrtdlaaplaycheck, "s");

static void hwrtdlaaplacesky()
{
    execute("paused 0");
    execute("spectator 1");
    execute("hudgun 0");
    execute("thirdperson 0");
    execute("hwrtvelsetpos 512 512 900 0 70");
    conoutf("hwrt dlaa: sky framing 512 512 900 yaw 0 pitch 70 (sun + blue skybox + clouds; verified by skyprobe_a)");
}
COMMAND(hwrtdlaaplacesky, "");

static void hwrtdlaaplacegunnear()
{
    execute("hwrtvelplacehud");
    holdcam(vec(668, 575, 566), 180, -6);
    conoutf("hwrt dlaa: shotgun against nearby academy geo at 668 575 566. Appearance unchanged; mixed depth + avatar MVs.");
}
COMMAND(hwrtdlaaplacegunnear, "");

#endif // WIN32
