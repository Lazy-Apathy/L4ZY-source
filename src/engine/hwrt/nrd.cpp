// nrd.cpp: optional NVIDIA REBLUR_DIFFUSE path for the skyvis scalar.
// Loads bin64/nrd/sauer_nrd.dll. Failure leaves the skyage filter running.
// World pixels only: models keep hitlight lighting (envmap, studio, own MV).

#include "engine.h"
#include "hwrt/hwrt.h"
#include "nrd_gateway/sauer_nrd.h"

#ifdef WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

extern matrix4 cammatrix, projmatrix, camprojmatrix;
extern int farplane;
extern float nearplane;

VAR(hwrtnrdavailable, 1, 0, 0);
VAR(hwrtnrdready, 1, 0, 0);
VAR(hwrtnrdfailed, 1, 0, 0);
// 0 play. 1 play lighting, skip denoise. 2 magenta vis. 3 in/out/valid.
// 4 history length in R (frames/30), vis in G. 5 MV: R/G pixels, B = |dZ|*10.
// 6 full-compose identity at identical signal (skip denoise): R/G = luma of
//   the two lighting combines, B = 10*|R-G|. Not the raw skyvis scalar.
// 7 geometry: G = |v_D3D - v_pixel| (unflipped NRD UV), B = error after clip-Y
//   flip, R = deviation from a pure vertical flip of the same hit.
// 8 reject map: R = histlen/30, G = |v_D3D - v_pixel|, B = |v-0.5|*2.
// 9 reject guides: R = histlen/30, G = NoV, B = abs(viewZ)/64.
// 10 reject guides: R = histlen/30, G = |uvCurr-uvPix| in 8 px, B = |N.z|.
VAR(hwrtnrddbg, 0, 0, 10);
// Official REBLUR antilag on. 0 = disabled (README: first integration).
VAR(hwrtnrdantilag, 0, 1, 1);
// Fast history on (6 frames). 0 = disabled (>= maxAccumulatedFrameNum).
VAR(hwrtnrdfast, 0, 1, 1);

static void hwrtnrdonchanged();
// Preference is saved. Default NRD when no config has chosen yet.
// Availability and the live session are separate (DLL missing / create fail).
VARFP(hwrtnrd, 0, 1, 1, { hwrtnrdonchanged(); });

#ifdef WIN32
static HMODULE nrddll = NULL;
#else
static void *nrddll = NULL;
#endif
static PFN_sauer_nrd_abi_version p_ver = NULL;
static PFN_sauer_nrd_create p_create = NULL;
static PFN_sauer_nrd_set_images p_images = NULL;
static PFN_sauer_nrd_denoise p_denoise = NULL;
static PFN_sauer_nrd_destroy p_destroy = NULL;
static PFN_sauer_nrd_stats p_stats = NULL;
static PFN_sauer_nrd_probe_arm p_probe_arm = NULL;
static PFN_sauer_nrd_probe_read p_probe_read = NULL;
static bool nrdcreated = false;
static bool nrdfailed = false;
static int nrdmaxw = 0, nrdmaxh = 0;
static int nrdframe = 0;
static matrix4 nrdprevworldtoview, nrdprevviewtoclip;
static bool nrdhasprev = false;
static uint32_t nrdlastw = 0, nrdlasth = 0;
static uint32_t nrdsetw = 0, nrdseth = 0;
static uint64_t nrdlastimgs[5];
static int nrdlastaccum = -1;
static float nrdlastjx = 0, nrdlastjy = 0;
static float nrdlastclipyy = 0;
static float nrdmaxw2vd = 0;

#ifdef WIN32
static bool nrdfileexists(const char *p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
#endif

static const char *nrddir()
{
    static string buf;
    const char *e = getenv("SAUER_NRD_DIR");
    if(e && e[0])
    {
        copystring(buf, e);
        return buf;
    }
#ifdef WIN32
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
    if(n && n < MAX_PATH)
    {
        char *slash = strrchr(exe, '\\');
        if(!slash) slash = strrchr(exe, '/');
        if(slash) *slash = 0;
        nformatstring(buf, sizeof(buf), "%s\\nrd", exe);
        string probe;
        nformatstring(probe, sizeof(probe), "%s\\sauer_nrd.dll", buf);
        if(nrdfileexists(probe)) return buf;
    }
#endif
    copystring(buf, "bin64/nrd");
    return buf;
}

void hwrtnrdcameramats(matrix4 &worldtoview, matrix4 &viewtoclip)
{
    // Unjittered view/projection. NRD MVs and these matrices must agree:
    // both omit Halton. NGX MVs stay on their own path.
    worldtoview = cammatrix;
    viewtoclip = projmatrix;
    if(!hwrtdlaajitteractive()) return;
    matrix4 unjitcamproj = hwrttemporalworldunjit();
    matrix4 invcam;
    if(invcam.invert(cammatrix))
        viewtoclip.mul(unjitcamproj, invcam);
}

// NRD InstanceImpl decomposes viewToClip with STYLE_D3D and GetScreenUv uses
// ndc*(0.5,-0.5)+0.5 (ML_WINDOW_ORIGIN_OGL=0). hitlight writes guides at
// pixel p with NDC.y=2*v-1 (row 0 = GL bottom) and MV in that same UV.
// Unflipped: engine v=0.25 -> NDC.y=-0.5 -> NRD v=0.75. Negating clip Y
// makes NRD's D3D UV and ReconstructViewPosition match the image/MV (offline
// geom-proof.py). NRD still applies its RH->LH Z conversion on this matrix.
// Do not flip the images, MV.y, or jitter: those stay in engine UV. NGX
// keeps its own path (it flips clip Y AND the copies it submits to D3D).
static void nrdflipclipy(matrix4 &m)
{
    m.a.y = -m.a.y;
    m.b.y = -m.b.y;
    m.c.y = -m.c.y;
    m.d.y = -m.d.y;
}

static bool loadnrddll()
{
    if(nrddll) return p_create != NULL;
#ifdef WIN32
    defformatstring(path, "%s/sauer_nrd.dll", nrddir());
    nrddll = LoadLibraryA(path);
    if(!nrddll)
    {
        char full[MAX_PATH];
        nformatstring(full, sizeof(full), "%s\\sauer_nrd.dll", nrddir());
        nrddll = LoadLibraryA(full);
    }
    if(!nrddll) return false;
    p_ver = (PFN_sauer_nrd_abi_version)GetProcAddress(nrddll, "sauer_nrd_abi_version");
    p_create = (PFN_sauer_nrd_create)GetProcAddress(nrddll, "sauer_nrd_create");
    p_images = (PFN_sauer_nrd_set_images)GetProcAddress(nrddll, "sauer_nrd_set_images");
    p_denoise = (PFN_sauer_nrd_denoise)GetProcAddress(nrddll, "sauer_nrd_denoise");
    p_destroy = (PFN_sauer_nrd_destroy)GetProcAddress(nrddll, "sauer_nrd_destroy");
    p_stats = (PFN_sauer_nrd_stats)GetProcAddress(nrddll, "sauer_nrd_stats");
    p_probe_arm = (PFN_sauer_nrd_probe_arm)GetProcAddress(nrddll, "sauer_nrd_probe_arm");
    p_probe_read = (PFN_sauer_nrd_probe_read)GetProcAddress(nrddll, "sauer_nrd_probe_read");
    if(!p_ver || !p_create || !p_images || !p_denoise || !p_destroy)
    {
        FreeLibrary(nrddll);
        nrddll = NULL;
        p_create = NULL;
        p_probe_arm = NULL;
        p_probe_read = NULL;
        return false;
    }
    if(p_ver() != SAUER_NRD_ABI_VERSION)
    {
        conoutf(CON_WARN, "hwrt nrd: ABI %u != %u, ignored", unsigned(p_ver()), unsigned(SAUER_NRD_ABI_VERSION));
        FreeLibrary(nrddll);
        nrddll = NULL;
        p_create = NULL;
        p_probe_arm = NULL;
        p_probe_read = NULL;
        return false;
    }
    hwrtnrdavailable = 1;
    conoutf("hwrt nrd: loaded %s (NRD v4.17.3 REBLUR_DIFFUSE)", path);
    return true;
#else
    return false;
#endif
}

void hwrtnrdpreload()
{
    if(loadnrddll()) hwrtnrdavailable = 1;
    else hwrtnrdavailable = 0;
}

void hwrtdestroynrd()
{
    if(nrdcreated && p_destroy) p_destroy();
    nrdcreated = false;
    hwrtnrdready = 0;
    nrdmaxw = nrdmaxh = 0;
    nrdhasprev = false;
    memset(nrdlastimgs, 0, sizeof(nrdlastimgs));
    nrdsetw = nrdseth = 0;
    nrdmaxw2vd = 0;
}

bool hwrtnrdsession()
{
    return hwrtnrd && hwrtnrdavailable && nrdcreated && !nrdfailed;
}

static void copym4(float *dst, const matrix4 &m)
{
    memcpy(dst, &m, 16 * sizeof(float));
}

static void hwrtnrdonchanged()
{
    if(initing) return;
    if(hwrtnrd && nrdcreated) return;
    if(!hwrtnrd && !nrdcreated)
    {
        conoutf("hwrt nrd: off, skyage filter");
        return;
    }
    hwrtinvalidateskyhistory();
    hwrtnrdinvalidate();
    if(hwrtdev.ok()) hwrtwaitidle("nrd toggle");
    hwrtdestroynrd();
    hwrtdestroynrdguides();
    nrdfailed = false;
    hwrtnrdfailed = 0;
    if(!hwrtnrd)
        conoutf("hwrt nrd: off, skyage filter");
    else
        conoutf("hwrt nrd: on (REBLUR_DIFFUSE if the gateway comes up)");
}

bool hwrtnrdensure(int w, int h)
{
    if(!hwrtnrd) return false;
    if(nrdfailed) return false;
    if(!hwrtdev.ok() || !hwrtdev.device) return false;
    if(!loadnrddll())
    {
        nrdfailed = true;
        hwrtnrdavailable = 0;
        hwrtnrdfailed = 1;
        conoutf(CON_WARN, "hwrt nrd: sauer_nrd.dll missing, skyage filter stays");
        return false;
    }
    int aw = w, ah = h;
    if(aw < 8) aw = 8;
    if(ah < 8) ah = 8;
    if(nrdcreated && nrdmaxw == aw && nrdmaxh == ah) { hwrtnrdready = 1; return true; }
    if(nrdcreated)
    {
        conoutf("hwrt nrd: size %dx%d -> %dx%d, recreate (exact active, no larger-instance reuse)",
                nrdmaxw, nrdmaxh, aw, ah);
        hwrtwaitidle("nrd resize");
        hwrtdestroynrd();
    }
    SauerNrdInit in;
    memset(&in, 0, sizeof(in));
    in.struct_bytes = sizeof(in);
    in.instance = (uint64_t)(uintptr_t)hwrtdev.instance;
    in.physical_device = (uint64_t)(uintptr_t)hwrtdev.phys;
    in.device = (uint64_t)(uintptr_t)hwrtdev.device;
    in.gipa = (uint64_t)(uintptr_t)vkGetInstanceProcAddr;
    in.gdpa = (uint64_t)(uintptr_t)vkGetDeviceProcAddr;
    in.queue_family = hwrtdev.queuefamily;
    in.queued_frames = HWRT_FRAMES_IN_FLIGHT;
    in.max_w = uint32_t(aw);
    in.max_h = uint32_t(ah);
    int32_t r = p_create(&in);
    if(r != SAUER_NRD_OK)
    {
        SauerNrdStats st;
        memset(&st, 0, sizeof(st));
        st.struct_bytes = sizeof(st);
        if(p_stats) p_stats(&st);
        conoutf(CON_WARN, "hwrt nrd: create failed %d %s — skyage filter stays", int(r), st.message);
        nrdfailed = true;
        hwrtnrdfailed = 1;
        hwrtnrdready = 0;
        return false;
    }
    nrdcreated = true;
    nrdmaxw = aw;
    nrdmaxh = ah;
    hwrtnrdready = 1;
    nrdhasprev = false;
    nrdframe = 0;
    nrdmaxw2vd = 0;
    conoutf("hwrt nrd: REBLUR_DIFFUSE instance %dx%d (exact active, recreate on any change)", aw, ah);
    return true;
}

bool hwrtnrdsetimages(VkImage diff, VkImage viewz, VkImage normal, VkImage mv, VkImage outdiff, int w, int h)
{
    if(!hwrtnrdsession() || !p_images) return false;
    uint64_t ids[5] = {
        (uint64_t)(uintptr_t)diff, (uint64_t)(uintptr_t)viewz, (uint64_t)(uintptr_t)normal,
        (uint64_t)(uintptr_t)mv, (uint64_t)(uintptr_t)outdiff
    };
    if(nrdsetw == uint32_t(w) && nrdseth == uint32_t(h) &&
       !memcmp(nrdlastimgs, ids, sizeof(ids)))
        return true;
    SauerNrdImages im;
    memset(&im, 0, sizeof(im));
    im.struct_bytes = sizeof(im);
    im.in_diff = (uint64_t)(uintptr_t)diff;
    im.in_viewz = (uint64_t)(uintptr_t)viewz;
    im.in_normal = (uint64_t)(uintptr_t)normal;
    im.in_mv = (uint64_t)(uintptr_t)mv;
    im.out_diff = (uint64_t)(uintptr_t)outdiff;
    im.w = uint32_t(w);
    im.h = uint32_t(h);
    int32_t r = p_images(&im);
    if(r != SAUER_NRD_OK)
    {
        conoutf(CON_WARN, "hwrt nrd: set_images failed %d", int(r));
        return false;
    }
    memcpy(nrdlastimgs, ids, sizeof(ids));
    nrdsetw = uint32_t(w);
    nrdseth = uint32_t(h);
    return true;
}

bool hwrtnrddenoise(VkCommandBuffer cmd, int w, int h, int reset)
{
    if(!hwrtnrdsession() || !p_denoise || !cmd) return false;
    matrix4 worldtoview, unjitproj;
    hwrtnrdcameramats(worldtoview, unjitproj);

    matrix4 nrdclip = unjitproj;
    nrdflipclipy(nrdclip);
    matrix4 nrdclipprev = nrdhasprev ? nrdprevviewtoclip : unjitproj;
    nrdflipclipy(nrdclipprev);

    SauerNrdFrame fr;
    memset(&fr, 0, sizeof(fr));
    fr.struct_bytes = sizeof(fr);
    fr.command_buffer = (uint64_t)(uintptr_t)cmd;
    copym4(fr.view_to_clip, nrdclip);
    copym4(fr.world_to_view, worldtoview);
    if(nrdhasprev && nrdlastw == uint32_t(w) && nrdlasth == uint32_t(h) && !reset)
    {
        copym4(fr.view_to_clip_prev, nrdclipprev);
        copym4(fr.world_to_view_prev, nrdprevworldtoview);
        fr.accum = SAUER_NRD_ACCUM_CONTINUE;
        fr.w_prev = nrdlastw;
        fr.h_prev = nrdlasth;
    }
    else
    {
        copym4(fr.view_to_clip_prev, nrdclip);
        copym4(fr.world_to_view_prev, worldtoview);
        fr.accum = SAUER_NRD_ACCUM_CLEAR;
        fr.w_prev = uint32_t(w);
        fr.h_prev = uint32_t(h);
    }
    float jx = 0, jy = 0, pjx = 0, pjy = 0;
    hwrttemporaljitterpixels(jx, jy, pjx, pjy);
    // NRD validates cameraJitter in [-0.5, 0.5] pixels (Halton already is).
    fr.jitter[0] = clamp(jx, -0.5f, 0.5f);
    fr.jitter[1] = clamp(jy, -0.5f, 0.5f);
    fr.jitter_prev[0] = clamp(pjx, -0.5f, 0.5f);
    fr.jitter_prev[1] = clamp(pjy, -0.5f, 0.5f);
    // 2.5D screen-space (NRD README recommended): UV in (0;1), scale.z != 0.
    // NGX pixel MVs are a different contract; do not change those.
    fr.mv_scale[0] = 1.0f;
    fr.mv_scale[1] = 1.0f;
    fr.mv_scale[2] = 1.0f;
    fr.time_delta_ms = hwrttime.frame > 0 ? hwrttime.frame : 16.0f;
    fr.denoising_range = float(max(farplane, 1024));
    fr.frame_index = uint32_t(nrdframe++);
    fr.w = uint32_t(w);
    fr.h = uint32_t(h);
    fr.options = 0;
    if(!hwrtnrdantilag) fr.options |= SAUER_NRD_OPT_ANTILAG_OFF;
    if(!hwrtnrdfast) fr.options |= SAUER_NRD_OPT_FAST_OFF;
    nrdlastaccum = int(fr.accum);
    nrdlastjx = fr.jitter[0];
    nrdlastjy = fr.jitter[1];
    nrdlastclipyy = nrdclip.b.y;
    if(nrdhasprev)
    {
        const float *a = &worldtoview.a.x;
        const float *b = &nrdprevworldtoview.a.x;
        float md = 0;
        for(int i = 0; i < 16; i++)
        {
            float d = fabsf(a[i] - b[i]);
            if(d > md) md = d;
        }
        if(md > nrdmaxw2vd) nrdmaxw2vd = md;
    }
    int32_t r = p_denoise(&fr);
    nrdprevworldtoview = worldtoview;
    nrdprevviewtoclip = unjitproj;
    nrdhasprev = true;
    nrdlastw = uint32_t(w);
    nrdlasth = uint32_t(h);
    if(r != SAUER_NRD_OK)
    {
        conoutf(CON_WARN, "hwrt nrd: denoise failed %d, falling back", int(r));
        nrdfailed = true;
        hwrtnrdfailed = 1;
        hwrtnrdready = 0;
        return false;
    }
    static uint32_t nrdloggedw = 0, nrdloggedh = 0, nrdloggedmaxw = 0, nrdloggedmaxh = 0;
    if(nrdloggedw != uint32_t(w) || nrdloggedh != uint32_t(h) ||
       nrdloggedmaxw != uint32_t(nrdmaxw) || nrdloggedmaxh != uint32_t(nrdmaxh))
    {
        conoutf("hwrt nrd: denoise rect %dx%d instance %dx%d set_images %ux%u (resourceSize=instance, rectSize=active)",
                w, h, nrdmaxw, nrdmaxh, nrdsetw, nrdseth);
        nrdloggedw = uint32_t(w);
        nrdloggedh = uint32_t(h);
        nrdloggedmaxw = uint32_t(nrdmaxw);
        nrdloggedmaxh = uint32_t(nrdmaxh);
    }
    return true;
}

void hwrtnrdstats()
{
    if(!p_stats)
    {
        conoutf("hwrt nrd: dll %s created %d failed %d cvar %d",
                nrddll ? "loaded" : "missing", int(nrdcreated), int(nrdfailed), int(hwrtnrd));
        return;
    }
    SauerNrdStats st;
    memset(&st, 0, sizeof(st));
    st.struct_bytes = sizeof(st);
    p_stats(&st);
    conoutf("hwrt nrd: session %d ready %d failed %d pipes %u dispatches %u pool %u+%u MiB (%s)",
            int(hwrtnrdsession()), int(hwrtnrdready), int(nrdfailed), st.pipelines, st.dispatches,
            st.permanent_mb, st.transient_mb, st.message);
    conoutf("hwrt nrd: last accum %d jitter %.4f %.4f frame %u rect %ux%u instance %dx%d set_images %ux%u antilag %d fast %d clipYflip proj.b.y %.4f max|dW2V| %.6f (2.5D MV, histlen in out.w)",
            nrdlastaccum, nrdlastjx, nrdlastjy, uint32_t(nrdframe), nrdlastw, nrdlasth, nrdmaxw, nrdmaxh, nrdsetw, nrdseth,
            hwrtnrdantilag, hwrtnrdfast, nrdlastclipyy, nrdmaxw2vd);
}
COMMAND(hwrtnrdstats, "");

static const char *nrdprobenames[SAUER_NRD_PROBE_MAX] = {
    "snow", "dirt", "stair", "mix", "near", "far", "p6", "p7"
};

void hwrtnrdprobe(char *s)
{
    if(!p_probe_arm)
    {
        conoutf(CON_WARN, "hwrt nrd: probe export missing");
        return;
    }
    SauerNrdProbeIn in;
    memset(&in, 0, sizeof(in));
    in.struct_bytes = sizeof(in);
    const char *p = s ? s : "";
    while(*p && in.n < SAUER_NRD_PROBE_MAX)
    {
        while(*p == ' ' || *p == ',') p++;
        if(!*p) break;
        int x = 0, y = 0, nch = 0;
        if(sscanf(p, "%d %d%n", &x, &y, &nch) < 2) break;
        if(x < 0) x = 0;
        if(y < 0) y = 0;
        in.x[in.n] = uint16_t(x);
        in.y[in.n] = uint16_t(y);
        in.n++;
        p += nch;
    }
    int32_t r = p_probe_arm(&in);
    conoutf("NRD_PROBE_ARM n=%u r=%d (NRD/VkImage y=0 is hitlight bottom)", unsigned(in.n), int(r));
    loopi(int(in.n))
        conoutf("  %s nrd=%u,%u png=%u,%u", nrdprobenames[i], unsigned(in.x[i]), unsigned(in.y[i]),
                unsigned(in.x[i]), nrdlasth ? unsigned(nrdlasth - 1 - in.y[i]) : 0);
}
COMMAND(hwrtnrdprobe, "s");

void hwrtnrdprobedump()
{
    if(!p_probe_read)
    {
        conoutf(CON_WARN, "hwrt nrd: probe export missing");
        return;
    }
    if(hwrtdev.ok()) hwrtwaitidle("nrd probe");
    SauerNrdProbeOut o;
    memset(&o, 0, sizeof(o));
    o.struct_bytes = sizeof(o);
    int32_t r = p_probe_read(&o);
    conoutf("NRD_PROBE_DUMP r=%d ready=%u pool %ux%u user %ux%u rs %ux%u rect %ux%u n=%u",
            int(r), unsigned(o.ready), unsigned(o.pool_w), unsigned(o.pool_h),
            unsigned(o.user_w), unsigned(o.user_h), unsigned(o.rs_w), unsigned(o.rs_h),
            unsigned(o.rect_w), unsigned(o.rect_h), unsigned(o.n));
    if(!o.ready)
    {
        conoutf("NRD_PROBE_DUMP not ready (arm, wait a few frames, dump after idle)");
        return;
    }
    loopi(int(o.n))
    {
        const SauerNrdProbePx &p = o.px[i];
        int match = (p.gather_ix == p.x && p.gather_iy == p.y) ? 1 : 0;
        float dz = fabsf(p.prev_z_xy - p.prev_z_g00);
        conoutf("NRD_PROBE %s nrd=%u,%u png=%u,%u gatherTexel=%u,%u matchXY=%d",
                nrdprobenames[i], unsigned(p.x), unsigned(p.y),
                unsigned(p.x), o.rect_h ? unsigned(o.rect_h - 1 - p.y) : 0,
                unsigned(p.gather_ix), unsigned(p.gather_iy), match);
        conoutf("  write IN_Z=%.5f PREV_Z_xy=%.5f PREV_Z_after=%.5f |xy-after|=%.5f",
                p.in_z, p.prev_z_xy, p.prev_z_after, fabsf(p.in_z - p.prev_z_after));
        conoutf("  gatherZ 00=%.5f 10=%.5f 01=%.5f 11=%.5f |xy-g00|=%.5f",
                p.prev_z_g00, p.prev_z_g10, p.prev_z_g01, p.prev_z_g11, dz);
        conoutf("  hist prev=%u gather=%u data1=%.1f out.w=%.2f data2=0x%02X occ2x2=%u mv=%.5f %.5f %.5f",
                unsigned(p.prev_hist), unsigned(p.prev_hist_g), p.data1_frames, p.out_w, unsigned(p.data2),
                unsigned(p.data2 & 15), p.mv_x, p.mv_y, p.mv_z);
    }
}
COMMAND(hwrtnrdprobedump, "");

void hwrtnrdinvalidate()
{
    nrdhasprev = false;
}

ICOMMAND(hwrtnrdtoggle, "", (),
{
    hwrtnrd = hwrtnrd ? 0 : 1;
    hwrtnrdonchanged();
});

ICOMMAND(hwrtnrdsession, "", (), intret(hwrtnrdsession() ? 1 : 0));
ICOMMAND(hwrtnrdfallback, "", (), intret((hwrtnrd && (!hwrtnrdavailable || hwrtnrdfailed)) ? 1 : 0));
