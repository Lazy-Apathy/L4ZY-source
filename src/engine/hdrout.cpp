// Native HDR presentation for the isolated prototype.
// OpenGL keeps the scene. A flip-model DXGI swap chain in FP16 scRGB
// (1.0 = 80 nits) is the only image presented when Windows HDR is active.
// The frame is not read back to the CPU. SDL_GL_SwapWindow is not called
// on that path.

#include "engine.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <initguid.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include "SDL_syswm.h"

#undef min
#undef max

#ifndef WGL_ACCESS_READ_WRITE_NV
#define WGL_ACCESS_READ_ONLY_NV 0x0000
#define WGL_ACCESS_READ_WRITE_NV 0x0001
#define WGL_ACCESS_WRITE_DISCARD_NV 0x0002
#endif

typedef HANDLE (WINAPI *PFN_wglDXOpenDeviceNV)(void *);
typedef BOOL (WINAPI *PFN_wglDXCloseDeviceNV)(HANDLE);
typedef HANDLE (WINAPI *PFN_wglDXRegisterObjectNV)(HANDLE, void *, GLuint, GLenum, GLenum);
typedef BOOL (WINAPI *PFN_wglDXUnregisterObjectNV)(HANDLE, HANDLE);
typedef BOOL (WINAPI *PFN_wglDXLockObjectsNV)(HANDLE, GLint, HANDLE *);
typedef BOOL (WINAPI *PFN_wglDXUnlockObjectsNV)(HANDLE, GLint, HANDLE *);
typedef HRESULT (WINAPI *PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);

extern int hwrthdr;
extern float hwrthdrexp;
extern int curvsync;
extern bool minimized;
extern int hdrout, hdroutref, hdroutmax, hdrouthud, hdroutmire, hdroutread, hdroutcurve;
extern int hdroutlumiere, hdrlightdbg, hdroutflamme, hdroutciel, hdroutcouleur, hdroutjour, hdroutjourcouleur;
extern int hwrt, hwrtavailable;
extern bool hwrtfailed;
extern Shader *newshader(int type, const char *name, const char *vs, const char *ps, Shader *variant, int row);
bool hdrout_lightfix_wanted();
int hdrout_skyfix_level();
int hdrout_colorfix_level();
int hdrout_dayfix_level();
// Lot 8 proposal: dose of the lot 7 colour gain (percent) with the daylight response.
#define HDRJOUR_COULEUR 25
static void lot5_params(float ref, float peak, float &la, float &sa, float &span);
static void lot6_params(float ref, float peak, float &b, float &span);
static void perf_gl_stamp(int which);
static void perf_add(int field, double sec);
static double perf_now();
static void sync_drop_fences();
static void perf_d3d_begin();
static void perf_d3d_end();
static void fresh_mark();
static void fresh_check();
static void hdrout_resolve();
// Last frame actually shown: 0 = SDR (SDL), 1 = HDR preview in SDR, 2 = native HDR. -1 = none yet.
static int g_lastkind = -1;

// Cube font codes (shared/stream.cpp cube2unichars). The font has no long dash.
#define CUBE_E_AIGU "\x8B"
#define CUBE_E_MAJ "\x0F"

static PFN_wglDXOpenDeviceNV pOpen = NULL;
static PFN_wglDXCloseDeviceNV pClose = NULL;
static PFN_wglDXRegisterObjectNV pReg = NULL;
static PFN_wglDXUnregisterObjectNV pUnreg = NULL;
static PFN_wglDXLockObjectsNV pLock = NULL;
static PFN_wglDXUnlockObjectsNV pUnlock = NULL;

static IDXGIFactory2 *g_factory = NULL;
static ID3D11Device *g_dev = NULL;
static ID3D11DeviceContext *g_ctx = NULL;
static IDXGISwapChain1 *g_swap = NULL;
static IDXGISwapChain3 *g_swap3 = NULL;
static ID3D11Texture2D *g_back = NULL;
static ID3D11RenderTargetView *g_rtv = NULL;
static ID3D11Texture2D *g_shared = NULL;
static ID3D11ShaderResourceView *g_srv = NULL;
static ID3D11Texture2D *g_staging = NULL;
static ID3D11VertexShader *g_vs = NULL;
static ID3D11PixelShader *g_ps = NULL;
static ID3D11SamplerState *g_samp = NULL;
static ID3D11RasterizerState *g_rast = NULL;
static ID3D11BlendState *g_blend = NULL;
static ID3D11DepthStencilState *g_depth = NULL;

static HANDLE g_dx = NULL;
static HANDLE g_dxobj = NULL;
static GLuint g_gltex = 0;
static GLuint g_fbo = 0;
static HWND g_parent = NULL;
static HWND g_child = NULL;
static Shader *g_map = NULL;
static Shader *g_mire = NULL;

static bool g_locked = false;
static bool g_ui = false;
static bool g_ready = false;
static bool g_tried = false;
// The presenter (D3D11 device, GL-D3D interop, child window, swap chain) is
// opened only once the saved output choice is known and asks for native HDR.
// g_open_failed: the last opening or presentation failed; no new attempt
// until Native HDR is chosen again or the GL context is recreated.
// g_disabled: hdrout-off.txt or no Windows window, never opened.
static bool g_open_failed = false;
static bool g_disabled = false;
static bool g_child_on = false;
// 0 = base SDR, 1 = HDR interne apercu SDR, 2 = HDR natif. -1 = pas compose cette image.
static int g_frame_kind = -1;
static bool g_in_frame = false;
static int g_sonde = 0;
static char g_effective[80] = "";
static int g_w = 0, g_h = 0;
static int g_readn = 0;
static int g_okpresents = 0;
static unsigned g_lastpresent = 0xFFFFFFFFu;

static int g_hdr = 0;
static int g_cs = -1;
static unsigned g_support = 0;
static unsigned g_sethr = 0;
static float g_min_nits = 0, g_max_nits = 0, g_full_nits = 0;
static unsigned g_bits = 0;
static int g_ac_sup = -1, g_ac_on = -1;
static unsigned g_sdr_raw = 0;
static float g_sdr_nits = -1.f;
static char g_adapter[160], g_output[160], g_gl[160], g_luid[40], g_reason[180];

static const float MIRE_V[5] = { 0.f, 1.f, 2.f, 4.f, 8.f };
static const int MIRE_X[5] = { 16, 40, 64, 88, 112 };
static const int MIRE_GY = 8;

// Mire injected into the linear scene, before exposure and the output curve.
// Centers: x = 40 + i*36 + 14, y = 238. Not the old post-compose transport mire.
enum { PATCH_N = 13, PATCH_X0 = 40, PATCH_STRIDE = 36, PATCH_W = 28, PATCH_Y0 = 220, PATCH_H = 36 };
static const float PATCH_RGB[PATCH_N][3] = {
    { 0.f, 0.f, 0.f }, { 0.02f, 0.02f, 0.02f }, { 0.18f, 0.18f, 0.18f }, { 0.50f, 0.50f, 0.50f },
    { 1.f, 1.f, 1.f }, { 2.f, 2.f, 2.f }, { 4.f, 4.f, 4.f }, { 8.f, 8.f, 8.f },
    { 1.f, 0.05f, 0.05f }, { 0.05f, 1.f, 0.05f }, { 0.05f, 0.05f, 1.f },
    { 1.f, 0.45f, 0.15f }, { 4.f, 1.f, 0.25f }
};
static const char *const PATCH_NAME[PATCH_N] = {
    "noir", "noir_proche", "gris_18", "gris_50", "scene_1", "hdr_2", "hdr_4", "hdr_8",
    "rouge", "vert", "bleu", "chaud", "chaud_hdr"
};
static int g_mesure = 0, g_present_patches = 0;
static float g_patch_gl[PATCH_N][3];
static float g_patch_expect[PATCH_N][3];
static int g_patch_mode = 0;
static int patch_cx(int i);
static int patch_cy();

static void hflush()
{
    FILE *f = getlogfile();
    if(f) fflush(f);
}

static void narrow(const wchar_t *w, char *out, int n)
{
    if(!out || n < 1) return;
    out[0] = 0;
    if(!w) return;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL);
    out[n - 1] = 0;
}

static const char *cs_name(int cs)
{
    if(cs == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) return "G22_SDR";
    if(cs == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709) return "G10_scRGB";
    if(cs == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) return "G2084_HDR";
    return "autre";
}

static void set_reason(const char *s)
{
    copystring(g_reason, s ? s : "", sizeof(g_reason));
}

static float used_max_nits(int *incomplete)
{
    if(incomplete) *incomplete = 0;
    if(hdroutmax > 0) return float(hdroutmax);
    if(g_max_nits >= 80.f) return g_max_nits;
    if(incomplete) *incomplete = 1;
    return 1000.f;
}

static float hud_nits()
{
    float n = hdrouthud > 0 ? float(hdrouthud) : float(hdroutref);
    if(n < 40.f) n = 40.f;
    return n;
}

static const char *mode_name()
{
    if(!hwrthdr) return "sdr_base";
    if(!g_ready) return "repli_sdr";
    if(hdrout) return "hdr_natif";
    return "apercu_sdr_surface_hdr";
}

// RT lighting composes the world and opaque models. The lot 4 light colour
// correction is for the classic lightmap lighting only.
static bool rt_lighting()
{
    return hwrt && hwrtavailable && !hwrtfailed;
}

// 8 = lot 8 proposal (lot 7 chain + daylight response of lit materials, hdroutjour, with
// the lot 7 colour at HDRJOUR_COULEUR percent), 82 / 83 = lot 8 diagnostics: daylight
// response without the lot 7 colour / with another colour dose (hdroutjourcouleur),
// 7 = lot 7 (lot 6 chain + daylight material colours, hdroutcouleur),
// 6 = lot 6 proposal (curve 4 + sky colours and exposed sky, on the lot 5 scene),
// 5 = lot 5 reference (curve 3 + lot 5 light response + flame target),
// 4 = lot 4 (curve 2 + classic light colour), 3 = lot 3 (curve 2 alone),
// 1 = restitution lot 1 correction, 0 = first native output. -1 = console-only mix.
// Lot 6 diagnostics on the lot 5 chain: 61 = curve 4 alone, 62 = sky alone
// (colours + exposed sky), 63 = sky colours alone, 64 = curve 4 + sky colours.
// Lot 5 diagnostics on the lot 4 chain: 51 = flames alone, 52 = bright tones alone
// (light response + curve 3), 53 = curve 3 alone, 54 = light response alone.
static int variant_lot()
{
    int c = hdroutcurve, l = hdroutlumiere, f = hdroutflamme, s = hdroutciel;
    if(hdroutjour)
    {
        if(c != 4 || l != 2 || !f || s != 2) return -1;
        if(!hdroutcouleur || !hdroutjourcouleur) return 82;
        return hdroutjourcouleur == HDRJOUR_COULEUR ? 8 : 83;
    }
    if(hdroutcouleur) return (c == 4 && l == 2 && f && s == 2) ? 7 : -1;
    if(l == 2 && f)
    {
        if(c == 4) return s == 2 ? 6 : s == 1 ? 64 : 61;
        if(c == 3 && s) return s == 2 ? 62 : 63;
    }
    if(s || c == 4) return -1;
    if(c == 3 && l == 2) return f ? 5 : 52;
    if(c == 2 && l == 1 && f) return 51;
    if(c == 3 && l == 1 && !f) return 53;
    if(c == 2 && l == 2 && !f) return 54;
    if(f || c == 3 || l == 2) return -1;
    if(c >= 2) return l ? 4 : 3;
    if(l) return -1;
    return c == 1 ? 1 : 0;
}

static const char *curve_name()
{
    switch(variant_lot())
    {
        case 8: return "lot8_proposition";
        case 82: return "lot8_diag_jour_sans_couleur";
        case 83: return "lot8_diag_jour_autre_couleur";
        case 7: return "lot7_reference";
        case 6: return "lot6_reference";
        case 61: return "lot6_diag_courbe_seule";
        case 62: return "lot6_diag_ciel_seul";
        case 63: return "lot6_diag_teinte_ciel_seule";
        case 64: return "lot6_diag_courbe_teinte_ciel";
        case 5: return "lot5_historique";
        case 51: return "lot5_diag_flammes_seules";
        case 52: return "lot5_diag_tons_clairs_seuls";
        case 53: return "lot5_diag_courbe_seule";
        case 54: return "lot5_diag_lumiere_seule";
        case 4: return "lot4_reference";
        case 3: return "lot3_historique";
        case 1: return "lot1_ancienne_correction";
        case 0: return "origine_sortie_lot1";
    }
    return "combinaison_diagnostic";
}

static const char LABEL_LOT8[] = "HDR natif - Nouvelle proposition (lot8)";
static const char LABEL_D82[] = "HDR natif - Diagnostic lot8 : r" CUBE_E_AIGU "ponse de jour sans couleur lot7";
static const char LABEL_D83[] = "HDR natif - Diagnostic lot8 : r" CUBE_E_AIGU "ponse de jour, autre dosage de couleur";
static const char LABEL_LOT7[] = "HDR natif - R" CUBE_E_AIGU "f" CUBE_E_AIGU "rence pr" CUBE_E_AIGU "c" CUBE_E_AIGU "dente (lot7)";
static const char LABEL_LOT6[] = "HDR natif - Historique (lot6)";
static const char LABEL_D61[] = "HDR natif - Diagnostic lot6 : courbe seule";
static const char LABEL_D62[] = "HDR natif - Diagnostic lot6 : ciel seul";
static const char LABEL_D63[] = "HDR natif - Diagnostic lot6 : teinte du ciel seule";
static const char LABEL_D64[] = "HDR natif - Diagnostic lot6 : courbe et teinte du ciel";
static const char LABEL_LOT5[] = "HDR natif - Historique (lot5)";
static const char LABEL_LOT4[] = "HDR natif - Historique (lot4)";
static const char LABEL_D51[] = "HDR natif - Diagnostic lot5 : flammes seules";
static const char LABEL_D52[] = "HDR natif - Diagnostic lot5 : tons clairs seuls";
static const char LABEL_D53[] = "HDR natif - Diagnostic lot5 : courbe seule";
static const char LABEL_D54[] = "HDR natif - Diagnostic lot5 : lumi" "\x8A" "re seule";
static const char LABEL_LOT3[] = "HDR natif - Historique (lot3)";
static const char LABEL_LOT1[] = "HDR natif - Ancienne correction (lot1)";
static const char LABEL_ORIG[] = "HDR natif - Origine (sortie lot1)";
static const char LABEL_MIX[] = "HDR natif - Combinaison de diagnostic";
static const char LABEL_APERCU5[] = "HDR interne pr" CUBE_E_AIGU "sent" CUBE_E_AIGU " en SDR (lot5)";
static const char LABEL_APERCU4[] = "HDR interne pr" CUBE_E_AIGU "sent" CUBE_E_AIGU " en SDR (lot4)";
static const char LABEL_APERCU3[] = "HDR interne pr" CUBE_E_AIGU "sent" CUBE_E_AIGU " en SDR (lot3)";
static const char LABEL_SDR[] = "SDR de base";
static const char LABEL_REPLI[] = "SDR - sortie HDR indisponible";

static const char *native_label()
{
    switch(variant_lot())
    {
        case 8: return LABEL_LOT8;
        case 82: return LABEL_D82;
        case 83: return LABEL_D83;
        case 7: return LABEL_LOT7;
        case 6: return LABEL_LOT6;
        case 61: return LABEL_D61;
        case 62: return LABEL_D62;
        case 63: return LABEL_D63;
        case 64: return LABEL_D64;
        case 5: return LABEL_LOT5;
        case 51: return LABEL_D51;
        case 52: return LABEL_D52;
        case 53: return LABEL_D53;
        case 54: return LABEL_D54;
        case 4: return LABEL_LOT4;
        case 3: return LABEL_LOT3;
        case 1: return LABEL_LOT1;
        case 0: return LABEL_ORIG;
    }
    return LABEL_MIX;
}

// The preview has its own per-channel curve: only the scene changes apply. The lot 6
// sky changes are native-only (hdrout_skyfix_level), so the preview stays the lot 5 one.
static const char *apercu_label()
{
    if(hdroutflamme || hdroutlumiere >= 2) return LABEL_APERCU5;
    return hdroutlumiere ? LABEL_APERCU4 : LABEL_APERCU3;
}

static const char *kind_label(int kind)
{
    if(kind == 2) return native_label();
    if(kind == 1) return apercu_label();
    return LABEL_REPLI;
}

static const char *label_key(const char *label)
{
    if(!label) return "autre";
    if(!strcmp(label, LABEL_SDR)) return "sdr_base";
    if(!strcmp(label, LABEL_APERCU5)) return "apercu_sdr_lot5";
    if(!strcmp(label, LABEL_APERCU4)) return "apercu_sdr_lot4";
    if(!strcmp(label, LABEL_APERCU3)) return "apercu_sdr_lot3";
    if(!strcmp(label, LABEL_LOT8)) return "hdr_lot8_proposition";
    if(!strcmp(label, LABEL_D82)) return "hdr_lot8_diag_jour_sans_couleur";
    if(!strcmp(label, LABEL_D83)) return "hdr_lot8_diag_jour_autre_couleur";
    if(!strcmp(label, LABEL_LOT7)) return "hdr_lot7_reference";
    if(!strcmp(label, LABEL_LOT6)) return "hdr_lot6_historique";
    if(!strcmp(label, LABEL_D61)) return "hdr_lot6_diag_courbe";
    if(!strcmp(label, LABEL_D62)) return "hdr_lot6_diag_ciel";
    if(!strcmp(label, LABEL_D63)) return "hdr_lot6_diag_teinte_ciel";
    if(!strcmp(label, LABEL_D64)) return "hdr_lot6_diag_courbe_teinte_ciel";
    if(!strcmp(label, LABEL_LOT5)) return "hdr_lot5_historique";
    if(!strcmp(label, LABEL_D51)) return "hdr_lot5_diag_flammes";
    if(!strcmp(label, LABEL_D52)) return "hdr_lot5_diag_tons_clairs";
    if(!strcmp(label, LABEL_D53)) return "hdr_lot5_diag_courbe";
    if(!strcmp(label, LABEL_D54)) return "hdr_lot5_diag_lumiere";
    if(!strcmp(label, LABEL_LOT4)) return "hdr_lot4_historique";
    if(!strcmp(label, LABEL_LOT3)) return "hdr_lot3_historique";
    if(!strcmp(label, LABEL_LOT1)) return "hdr_lot1_ancienne_correction";
    if(!strcmp(label, LABEL_ORIG)) return "hdr_origine_sortie_lot1";
    if(!strcmp(label, LABEL_MIX)) return "hdr_combinaison";
    if(!strcmp(label, LABEL_REPLI)) return "repli_sdr";
    return "autre";
}

static const char *lighting_label()
{
    if(rt_lighting())
        return (hdroutlumiere || hdroutcouleur || hdroutjour) ? CUBE_E_MAJ "clairage : RT (corrections classiques de lumi" "\x8A" "re et de couleur inactives en RT)" : CUBE_E_MAJ "clairage : RT";
    return CUBE_E_MAJ "clairage : classique, sans RT";
}

// Effective image of this frame. Voluntary base SDR is not an error fallback.
static const char *current_label()
{
    if(!g_tried) return "";
    if(!hwrthdr) return LABEL_SDR;
    int kind = g_frame_kind;
    if(kind < 0 && g_ready && g_locked) kind = hdrout ? 2 : 1;
    if(kind == 1 || kind == 2) return kind_label(kind);
    if(g_in_frame) return LABEL_REPLI;
    if(g_effective[0]) return g_effective;
    return LABEL_REPLI;
}

const char *hdrout_label_text()
{
    return current_label();
}

static void remember_effective()
{
    const char *s = current_label();
    if(s && s[0]) copystring(g_effective, s, sizeof(g_effective));
}

static void note_effective(const char *presentation)
{
    const char *eff = current_label();
    int ngx = hwrtngxmodeapplied();
    int rt = rt_lighting() ? 1 : 0;
    int fix = (hwrthdr && hdrout_lightfix_wanted()) ? 1 : 0;
    int flame = (hwrthdr && hdroutflamme) ? 1 : 0;
    int sky = hdrout_skyfix_level();
    int col = hdrout_colorfix_level();
    int day = hdrout_dayfix_level();
    defformatstring(sig, "%s|%s|%d|%d|%.3f|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d",
            label_key(eff), presentation ? presentation : "",
            hwrthdr, hdrout, hwrthdrexp, hdroutref, hdroutmax, hdrouthud,
            ngx, g_child_on ? 1 : 0, hdroutcurve, hdroutlumiere, rt, fix, flame, sky, col, day, hdroutjourcouleur);
    static string lastsig;
    if(!strcmp(lastsig, sig)) return;
    copystring(lastsig, sig);
    logoutf("hdrout compare effective key=%s text=%s presentation=%s child=%d hdr=%d out=%d exp=%.3f ref=%d max=%d hud=%d ngx=%d ready=%d curve=%d light_lot4=%d rt_lighting=%d light_fix_active=%d flames_lot5=%d sky_lot6_active=%d colour_lot7_active=%d day_lot8_active=%d colour_dose_lot8=%d",
            label_key(eff), eff, presentation ? presentation : "", g_child_on ? 1 : 0,
            hwrthdr, hdrout, hwrthdrexp, hdroutref, hdroutmax, hdrouthud, ngx, g_ready ? 1 : 0, hdroutcurve,
            hdroutlumiere, rt, fix, flame, sky, col, day, hdroutjourcouleur);
    hflush();
}

static void read_center(float rgb[3])
{
    rgb[0] = rgb[1] = rgb[2] = -1.f;
    GLint vp[4] = { 0, 0, 0, 0 };
    glGetIntegerv(GL_VIEWPORT, vp);
    if(vp[2] < 2 || vp[3] < 2) return;
    float px[4] = { 0.f, 0.f, 0.f, 0.f };
    glReadPixels(vp[2] / 2, vp[3] / 2, 1, 1, GL_RGBA, GL_FLOAT, px);
    rgb[0] = px[0];
    rgb[1] = px[1];
    rgb[2] = px[2];
}

static void take_sonde(const char *presentation)
{
    if(g_sonde <= 0) return;
    g_sonde--;
    float rgb[3];
    read_center(rgb);
    const char *eff = current_label();
    float cx = camera1 ? camera1->o.x : 0.f;
    float cy = camera1 ? camera1->o.y : 0.f;
    float cz = camera1 ? camera1->o.z : 0.f;
    float yaw = camera1 ? camera1->yaw : 0.f;
    float pitch = camera1 ? camera1->pitch : 0.f;
    logoutf("hdrout compare probe key=%s text=%s presentation=%s child=%d hdr=%d out=%d exp=%.3f ref=%d max=%d hud=%d rt=%d nrd=%d ngx=%d cam=%.3f %.3f %.3f yaw=%.3f pitch=%.3f rgb=%.5f %.5f %.5f",
            label_key(eff), eff, presentation ? presentation : "", g_child_on ? 1 : 0,
            hwrthdr, hdrout, hwrthdrexp, hdroutref, hdroutmax, hdrouthud,
            getvar("hwrt"), getvar("hwrtnrd"), hwrtngxmodeapplied(),
            cx, cy, cz, yaw, pitch, rgb[0], rgb[1], rgb[2]);
    hflush();
}

void hdrout_framebegin()
{
    hdrout_resolve();
    perf_gl_stamp(0);
    g_in_frame = true;
    g_frame_kind = -1;
}

static void frameend()
{
    if(g_in_frame) remember_effective();
    g_in_frame = false;
}

static void log_state(const char *why)
{
    int incomplete = 0;
    float mx = used_max_nits(&incomplete);
    logoutf("hdrout mode why=%s hdr=%d out=%d effective=%s reason=%s",
            why ? why : "", hwrthdr, hdrout, mode_name(), g_reason[0] ? g_reason : "ok");
    logoutf("hdrout gpu gl=%s d3d=%s luid=%s", g_gl, g_adapter, g_luid);
    logoutf("hdrout screen %s hdr_dxgi=%d colourspace=%d %s bits=%u",
            g_output, g_hdr, g_cs, cs_name(g_cs), g_bits);
    logoutf("hdrout caps min=%.3f max=%.3f full_frame=%.3f nits sdr_white_raw=%u sdr_white_nits=%.1f ac_supported=%d ac_on=%d",
            g_min_nits, g_max_nits, g_full_nits, g_sdr_raw, g_sdr_nits, g_ac_sup, g_ac_on);
    logoutf("hdrout format RGBA16F present_colourspace=G10_scRGB support=0x%X set=0x%08X size=%dx%d",
            g_support, g_sethr, g_w, g_h);
    logoutf("hdrout cal ref=%d max_used=%.1f incomplete=%d hud=%.1f exp=%.3f curve=%s curve_num=%d light_lot4=%d flames_lot5=%d sky_lot6=%d sky_lot6_active=%d colour_lot7=%d colour_lot7_active=%d day_lot8=%d day_lot8_active=%d colour_dose_lot8=%d rt_lighting=%d",
            hdroutref, mx, incomplete, hud_nits(), hwrthdrexp, curve_name(), hdroutcurve, hdroutlumiere, hdroutflamme, hdroutciel, hdrout_skyfix_level(), hdroutcouleur, hdrout_colorfix_level(), hdroutjour, hdrout_dayfix_level(), hdroutjourcouleur, rt_lighting() ? 1 : 0);
    logoutf("hdrout Windows SDR white %.1f nits is read, not used as a gain. scRGB 1 = 80 nits does not mean scene 1 = 80 nits.",
            g_sdr_nits);
    if(g_ready && hwrthdr && !hdrout)
        logoutf("hdrout comparison: SDR preview in the HDR surface. The physical screen mode is not changed.");
    static int fallback_logged = 0;
    if(!hwrthdr || g_ready) fallback_logged = 0;
    else if(g_tried && !fallback_logged)
    {
        fallback_logged = 1;
        conoutf("hdrout SDR: native HDR output unavailable (%s)", g_reason[0] ? g_reason : "?");
    }
    hflush();
}

static void hdrout_on_pref()
{
    if(!g_tried) return;
    log_state("setting");
}

static void hdrout_on_curve()
{
    if(!g_tried) return;
    logoutf("hdrout variant %s curve %d light_lot4 %d flames_lot5 %d sky_lot6 %d colour_lot7 %d day_lot8 %d colour_dose %d. Exposure %.3f, white %d, peak %d and HUD %d are unchanged. Not a calibration setting.",
            curve_name(), hdroutcurve, hdroutlumiere, hdroutflamme, hdroutciel, hdroutcouleur, hdroutjour, hdroutjourcouleur, hwrthdrexp, hdroutref, hdroutmax, hdrouthud);
    conoutf("%s", native_label());
    log_state("curve");
}

// The light colour and the flame target change the scene before DLAA/DLSS:
// their history must not blend the previous chain into the new one.
static void hdrout_scene_changed(const char *why)
{
    if(hwrthdr) hwrttemporalreset(why);
}

static void hdrout_on_scene()
{
    hdrout_scene_changed("hdrout chaine");
    hdrout_on_curve();
}

VARF(hdrout, 0, 1, 1, hdrout_on_pref());
// Personal calibration, saved in config.cfg. Unchanged by output, map, AA or RT.
// hdroutmax 0 = peak reported by the display (automatic). hdrouthud 0 = HUD follows the reference white.
VARP(hdroutref, 40, 80, 480);
VARP(hdroutmax, 0, 0, 10000);
VARP(hdrouthud, 0, 0, 480);
VAR(hdroutmire, 0, 0, 1);
VAR(hdroutread, 0, 0, 1);
// 0 = linear native path (first native output). 1 = restitution lot 1 curve.
// 2 = lot 3 curve: same white and same shoulder, body opened below reference white.
// 3 = lot 5 curve: curve 2 up to scene 0.12, then a local contrast that eases from
// the curve 2 one down to 0.55 instead of dipping near 0.2, then a soft limit at the peak.
// 4 = lot 6 curve: curve 3 up to scene LOT6_Y0; above, a bright-tone gain that grows
// smoothly in log up to LOT6_GAIN at LOT6_Y1, then the soft limit at the peak.
VARF(hdroutcurve, 0, 4, 4, hdrout_on_curve());
// 1 = lot 4: lightmap / dynamic / model light colours take the chromaticity they had
// on the historical display, at the lot 3 luminance (glsl.cfg hdrlightchroma).
// 2 = lot 5: same colour, and overbright light (albedo multiplier above 1) takes back
// its historical luminance response. Classic lighting only. 0 = the lot 3 scene. Not saved.
VARF(hdroutlumiere, 0, 2, 2, hdrout_on_scene());
// 1 = lot 5: additive flames summed apart, encoded, then composed with the hue of
// the 8-bit clipped sum (renderparticles.cpp). HDR scene, classic and RT. Not saved.
VARF(hdroutflamme, 0, 1, 1, hdrout_on_scene());
// 1 = lot 6: sky tint colours (skybox, cloud box, cloud layer, fog dome) decoded as the
// 8-bit display colours they are. 2 = same, and the saturated blue sky, whose luminance
// is that of a shadow, takes the lot 6 gain (glsl.cfg skybox). Native HDR only: the base
// SDR and preview witnesses keep lot 5.
VARF(hdroutciel, 0, 2, 2, hdrout_on_scene());
// 1 = lot 7: daylight material colours. In the classic world, grass and model shaders
// (glsl.cfg hdrmatcouleur), the decoded albedo of moderately coloured materials moves
// away from its own luminance (luminance kept, +15 % at most) when the light on it is
// not strongly coloured and as strong as direct daylight, and the lit surface is out of
// the shadows. Neutral and already vivid albedos, torch-coloured light, moonlight and
// shade, sky, particles and flames are left alone.
// Native HDR and classic lighting only. Not saved.
VARF(hdroutcouleur, 0, 1, 1, hdrout_on_scene());
// 1 = lot 8: daylight response of lit materials (glsl.cfg hdrjourmat). In the same
// shaders, a surface lit by strong, weakly coloured light takes, through the common
// curve, the output k.Y of the line tangent to that curve (log slope 1: the contrast of
// the scene), instead of its steep foot and flat top. Never darker than lot 7. Shade,
// moon, torch light, sky, particles and flames keep lot 7.
// Native HDR and classic lighting only. Not saved.
VARF(hdroutjour, 0, 1, 1, hdrout_on_scene());
// Lot 8 dose of the lot 7 colour gain, in percent, when hdroutjour is on. The lot 7 gain
// partly made up for sunlit walls drawn at ~0.7 of their base SDR luminance; the
// daylight response gives them that luminance back. HDRJOUR_COULEUR = the proposal.
VARF(hdroutjourcouleur, 0, HDRJOUR_COULEUR, 100, hdrout_on_scene());

bool hdrout_flamefix_wanted()
{
    return hdroutflamme != 0;
}

int hdrout_skyfix_level()
{
    return (hwrthdr && hdrout) ? hdroutciel : 0;
}

// Native only, like the lot 6 sky: the base SDR and preview witnesses stay the lot 5 ones.
// Classic only, like the lot 4/5 light: in RT these shaders do not light the albedo.
int hdrout_colorfix_level()
{
    return (hwrthdr && hdrout && hdroutcouleur && !rt_lighting()) ? 1 : 0;
}

// Value of the hdrmatcol uniform: 1 = the lot 7 gain; with the lot 8 daylight response,
// hdroutjourcouleur percent of it.
float hdrout_colorfix_scale()
{
    if(!hdrout_colorfix_level()) return 0.f;
    return hdroutjour ? float(hdroutjourcouleur) / 100.f : 1.f;
}

int hdrout_dayfix_level()
{
    return (hwrthdr && hdrout && hdroutjour && !rt_lighting()) ? 1 : 0;
}
// Lab only, not saved. World shaders: 1 = light alone, 2 = texture alone.
VAR(hdrlightdbg, 0, 0, 2);

bool hdrout_lightfix_wanted()
{
    return hdroutlumiere && !rt_lighting();
}

int hdrout_lightfix_level()
{
    return hdrout_lightfix_wanted() ? hdroutlumiere : 0;
}

int hdrout_lightdbg()
{
    return hdrlightdbg;
}
// Lab only, not saved. 1 = after one real Present, fail the next one in software.
VAR(hdroutsim, 0, 0, 1);

static bool load_wgl()
{
    if(pOpen) return true;
    pOpen = (PFN_wglDXOpenDeviceNV)wglGetProcAddress("wglDXOpenDeviceNV");
    pClose = (PFN_wglDXCloseDeviceNV)wglGetProcAddress("wglDXCloseDeviceNV");
    pReg = (PFN_wglDXRegisterObjectNV)wglGetProcAddress("wglDXRegisterObjectNV");
    pUnreg = (PFN_wglDXUnregisterObjectNV)wglGetProcAddress("wglDXUnregisterObjectNV");
    pLock = (PFN_wglDXLockObjectsNV)wglGetProcAddress("wglDXLockObjectsNV");
    pUnlock = (PFN_wglDXUnlockObjectsNV)wglGetProcAddress("wglDXUnlockObjectsNV");
    if(!pOpen || !pClose || !pReg || !pUnreg || !pLock || !pUnlock)
    {
        set_reason("WGL_NV_DX_interop2 missing");
        return false;
    }
    return true;
}

static void release_views()
{
    if(g_ctx)
    {
        ID3D11ShaderResourceView *ns = NULL;
        g_ctx->PSSetShaderResources(0, 1, &ns);
        g_ctx->OMSetRenderTargets(0, NULL, NULL);
    }
    if(g_locked && pUnlock && g_dx && g_dxobj)
    {
        if(!pUnlock(g_dx, 1, &g_dxobj))
            logoutf("hdrout api wglDXUnlockObjectsNV failed");
    }
    g_locked = false;
    g_ui = false;
    if(g_dxobj && pUnreg && g_dx) pUnreg(g_dx, g_dxobj);
    g_dxobj = NULL;
    if(g_fbo) { glDeleteFramebuffers_(1, &g_fbo); g_fbo = 0; }
    if(g_gltex) { glDeleteTextures(1, &g_gltex); g_gltex = 0; }
    if(g_srv) { g_srv->Release(); g_srv = NULL; }
    if(g_shared) { g_shared->Release(); g_shared = NULL; }
    if(g_rtv) { g_rtv->Release(); g_rtv = NULL; }
    if(g_back) { g_back->Release(); g_back = NULL; }
    if(g_staging) { g_staging->Release(); g_staging = NULL; }
}

// Borderless at a chosen resolution (main.cpp): the swap chain keeps the
// render size and DXGI_SCALING_STRETCH stretches it over the child window,
// which covers the rectangle the picture is shown in. With kept proportions a
// black child below it fills the bars, so the 8-bit GL window never shows there.
extern void screendisplayrect(int &x, int &y, int &w, int &h);
static HWND g_bars = NULL;
static bool g_bars_want = false, g_bars_on = false;
static int g_cx = -1, g_cy = -1, g_cw = -1, g_ch = -1;

static LRESULT CALLBACK bars_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
    if(msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(hwnd, msg, w, l);
}

static void show_bars(bool on)
{
    if(on && !g_bars && g_parent)
    {
        HINSTANCE inst = GetModuleHandle(NULL);
        WNDCLASSW wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = bars_proc;
        wc.hInstance = inst;
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = L"SauerHDROutBars";
        RegisterClassW(&wc);
        g_bars = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_NOPARENTNOTIFY | WS_EX_TRANSPARENT,
                                 L"SauerHDROutBars", L"", WS_CHILD | WS_CLIPSIBLINGS,
                                 0, 0, 64, 64, g_parent, NULL, inst, NULL);
        if(!g_bars) logoutf("hdrout black bars: no window");
    }
    if(!g_bars) return;
    if(on)
    {
        RECT rc;
        if(!GetClientRect(g_parent, &rc)) return;
        // Below the presenting child, over the whole client area.
        SetWindowPos(g_bars, g_child ? g_child : HWND_BOTTOM, 0, 0, int(rc.right - rc.left), int(rc.bottom - rc.top),
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    else if(g_bars_on) ShowWindow(g_bars, SW_HIDE);
    g_bars_on = on;
}

static void place_child(bool force)
{
    if(!g_child) return;
    int x, y, w, h;
    screendisplayrect(x, y, w, h);
    if(w < 1 || h < 1) { x = y = 0; w = screenw; h = screenh; }
    if(!force && x == g_cx && y == g_cy && w == g_cw && h == g_ch) return;
    MoveWindow(g_child, x, y, w, h, FALSE);
    g_cx = x; g_cy = y; g_cw = w; g_ch = h;
    g_bars_want = x > 0 || y > 0;
    if(g_child_on) show_bars(g_bars_want);
    if(w != screenw || h != screenh)
        logoutf("hdrout child %d,%d %dx%d: picture %dx%d stretched (bars %d)", x, y, w, h, screenw, screenh, g_bars_want ? 1 : 0);
}

static void hide_child()
{
    if(g_bars_on) show_bars(false);
    if(g_child && g_child_on)
    {
        ShowWindow(g_child, SW_HIDE);
        g_child_on = false;
    }
}

// HDR is no longer functional. The shared scRGB target is not copied into
// the 8-bit window. The next frame takes the SDR compose path.
static void abandon_hdr(const char *why)
{
    bool live = g_ready || g_locked;
    if(g_locked && pUnlock && g_dx && g_dxobj)
    {
        glBindFramebuffer_(GL_FRAMEBUFFER, 0);
        if(!pUnlock(g_dx, 1, &g_dxobj))
            logoutf("hdrout api wglDXUnlockObjectsNV failed");
    }
    g_locked = false;
    g_ui = false;
    g_ready = false;
    g_open_failed = true;
    g_frame_kind = -1;
    hide_child();
    set_reason(why ? why : "presentation failed");
    if(live) log_state("fallback");
    else hflush();
}

// Shown only after a successful Present, so a child window holding no picture
// never covers the game. SW_SHOWNA does not activate it; the child has
// WS_EX_NOACTIVATE and ignores hit tests, so focus stays on the SDL window
// without a SetFocus call (which only produced extra focus events).
static void show_child()
{
    if(!g_child || g_child_on) return;
    ShowWindow(g_child, SW_SHOWNA);
    g_child_on = true;
    if(g_bars_want) show_bars(true);
    logoutf("hdrout child shown after a successful Present t=%u ms", SDL_GetTicks());
    hflush();
}

static LRESULT CALLBACK child_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
    if(msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(hwnd, msg, w, l);
}

static bool ensure_child()
{
    if(g_child) return true;
    HINSTANCE inst = GetModuleHandle(NULL);
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = child_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"SauerHDROut";
    RegisterClassW(&wc);
    g_child = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_NOPARENTNOTIFY | WS_EX_TRANSPARENT,
                               L"SauerHDROut", L"",
                               WS_CHILD,
                               0, 0, g_w > 0 ? g_w : 64, g_h > 0 ? g_h : 64,
                               g_parent, NULL, inst, NULL);
    if(!g_child)
    {
        set_reason("no presentation window");
        return false;
    }
    // Created hidden: show_child() after the first successful Present.
    g_child_on = false;
    logoutf("hdrout child created hidden t=%u ms", SDL_GetTicks());
    return true;
}

static void read_display_path(const wchar_t *gdi)
{
    g_ac_sup = g_ac_on = -1;
    g_sdr_raw = 0;
    g_sdr_nits = -1.f;
    UINT32 pc = 0, mc = 0;
    LONG st = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pc, &mc);
    if(st != ERROR_SUCCESS || pc < 1 || mc < 1 || pc > 32 || mc > 64)
    {
        logoutf("hdrout GetDisplayConfigBufferSizes hr %ld paths %u modes %u", st, pc, mc);
        return;
    }
    DISPLAYCONFIG_PATH_INFO paths[32];
    DISPLAYCONFIG_MODE_INFO modes[64];
    st = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pc, paths, &mc, modes, NULL);
    if(st != ERROR_SUCCESS) 
    {
        logoutf("hdrout QueryDisplayConfig hr %ld", st);
        return;
    }
    UINT32 i;
    for(i = 0; i < pc; i++)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src;
        memset(&src, 0, sizeof(src));
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof(src);
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;
        if(DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
        if(gdi && wcscmp(src.viewGdiDeviceName, gdi) != 0) continue;

        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO ac;
        memset(&ac, 0, sizeof(ac));
        ac.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
        ac.header.size = sizeof(ac);
        ac.header.adapterId = paths[i].targetInfo.adapterId;
        ac.header.id = paths[i].targetInfo.id;
        if(DisplayConfigGetDeviceInfo(&ac.header) == ERROR_SUCCESS)
        {
            g_ac_sup = ac.advancedColorSupported ? 1 : 0;
            g_ac_on = ac.advancedColorEnabled ? 1 : 0;
        }
        DISPLAYCONFIG_SDR_WHITE_LEVEL sw;
        memset(&sw, 0, sizeof(sw));
        sw.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        sw.header.size = sizeof(sw);
        sw.header.adapterId = paths[i].targetInfo.adapterId;
        sw.header.id = paths[i].targetInfo.id;
        if(DisplayConfigGetDeviceInfo(&sw.header) == ERROR_SUCCESS)
        {
            g_sdr_raw = sw.SDRWhiteLevel;
            g_sdr_nits = float(sw.SDRWhiteLevel) / 1000.f * 80.f;
        }
        return;
    }
    logoutf("hdrout display path not matched");
}

static bool query_output(IDXGIAdapter *adapter, HMONITOR mon)
{
    g_hdr = 0;
    g_cs = -1;
    g_min_nits = g_max_nits = g_full_nits = 0;
    g_bits = 0;
    g_output[0] = 0;
    IDXGIOutput *best = NULL;
    UINT i;
    for(i = 0; ; i++)
    {
        IDXGIOutput *out = NULL;
        if(adapter->EnumOutputs(i, &out) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_OUTPUT_DESC od;
        memset(&od, 0, sizeof(od));
        out->GetDesc(&od);
        if(!best) { best = out; }
        else if(mon && od.Monitor == mon)
        {
            best->Release();
            best = out;
            break;
        }
        else out->Release();
    }
    if(!best)
    {
        set_reason("DXGI output not found");
        return false;
    }
    DXGI_OUTPUT_DESC od;
    memset(&od, 0, sizeof(od));
    best->GetDesc(&od);
    narrow(od.DeviceName, g_output, sizeof(g_output));
    read_display_path(od.DeviceName);
    IDXGIOutput6 *o6 = NULL;
    HRESULT hr = best->QueryInterface(IID_IDXGIOutput6, (void **)&o6);
    best->Release();
    if(FAILED(hr) || !o6)
    {
        set_reason("IDXGIOutput6 unavailable");
        logoutf("hdrout api Output6 hr 0x%08X", (unsigned)hr);
        return false;
    }
    DXGI_OUTPUT_DESC1 d;
    memset(&d, 0, sizeof(d));
    hr = o6->GetDesc1(&d);
    o6->Release();
    if(FAILED(hr))
    {
        set_reason("GetDesc1 failed");
        logoutf("hdrout api GetDesc1 hr 0x%08X", (unsigned)hr);
        return false;
    }
    g_cs = int(d.ColorSpace);
    g_bits = d.BitsPerColor;
    g_min_nits = d.MinLuminance;
    g_max_nits = d.MaxLuminance;
    g_full_nits = d.MaxFullFrameLuminance;
    g_hdr = d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ? 1 : 0;
    if(!g_hdr) set_reason("Windows HDR off on this screen");
    return true;
}

static bool make_shaders()
{
    static const char *vs =
        "attribute vec4 vvertex;\n"
        "uniform vec4 screentexcoord0;\n"
        "varying vec2 texcoord0;\n"
        "void main(void) {\n"
        "    gl_Position = vvertex;\n"
        "    texcoord0 = vvertex.xy * screentexcoord0.xy + screentexcoord0.zw;\n"
        "}\n";
    static const char *mapps =
        "uniform sampler2D tex0;\n"
        "varying vec2 texcoord0;\n"
        "uniform vec4 hdroutp;\n"
        "uniform vec4 hdroutq;\n"
        "uniform vec4 hdroutr;\n"
        "uniform vec4 hdrouts;\n"
        "float hdr_lot5raw(float Y) {\n"
        "    float x = log(Y / 0.12);\n"
        "    return hdroutr.x * exp(0.55 * x + (hdroutr.y - 0.55) * 0.5 * (1.0 - exp(-x / 0.5)));\n"
        "}\n"
        "float hdr_lot5(float Y) {\n"
        "    float t = (hdr_lot5raw(Y) - hdroutr.x) / hdroutr.z;\n"
        "    return hdroutr.x + hdroutr.z * t * inversesqrt(1.0 + t * t);\n"
        "}\n"
        "float hdr_nark(float v) {\n"
        "    return clamp((v * (2.51 * v + 0.03)) / (v * (2.43 * v + 0.59) + 0.14), 0.0, 1.0);\n"
        "}\n"
        "float hdr_luma(vec3 c) {\n"
        "    return dot(c, vec3(0.2126, 0.7152, 0.0722));\n"
        "}\n"
        "float hdr_body(float Y) {\n"
        "    float y;\n"
        "    float lifted;\n"
        "    float t;\n"
        "    float u;\n"
        "    y = clamp(Y, 0.0, 1.0);\n"
        "    lifted = (1.80 * y) / (1.0 + y * 0.80);\n"
        "    t = clamp((y - 0.40) / 0.52, 0.0, 1.0);\n"
        "    u = t * t * (3.0 - 2.0 * t);\n"
        "    return hdr_nark((1.0 - u) * lifted + u * y);\n"
        "}\n"
        "float hdr_lot6(float Y) {\n"
        "    float raw = Y <= 0.12 ? hdr_body(Y) : hdr_lot5raw(Y);\n"
        "    float u = clamp(log(Y / hdrouts.z) * hdrouts.w, 0.0, 1.0);\n"
        "    raw *= exp(hdroutr.w * u * u * (3.0 - 2.0 * u));\n"
        "    float t = (raw - hdrouts.x) / hdrouts.y;\n"
        "    return hdrouts.x + hdrouts.y * t * inversesqrt(1.0 + t * t);\n"
        "}\n"
        "vec4 hdr_patch(vec2 frag) {\n"
        "    float col;\n"
        "    float local;\n"
        "    if(frag.y < 220.0 || frag.y >= 256.0) return vec4(0.0, 0.0, 0.0, -1.0);\n"
        "    col = floor((frag.x - 40.0) / 36.0);\n"
        "    local = frag.x - 40.0 - col * 36.0;\n"
        "    if(local < 0.0 || local >= 28.0 || col < 0.0 || col >= 13.0) return vec4(0.0, 0.0, 0.0, -1.0);\n"
        "    if(col < 0.5) return vec4(0.0, 0.0, 0.0, 1.0);\n"
        "    if(col < 1.5) return vec4(0.02, 0.02, 0.02, 1.0);\n"
        "    if(col < 2.5) return vec4(0.18, 0.18, 0.18, 1.0);\n"
        "    if(col < 3.5) return vec4(0.50, 0.50, 0.50, 1.0);\n"
        "    if(col < 4.5) return vec4(1.0, 1.0, 1.0, 1.0);\n"
        "    if(col < 5.5) return vec4(2.0, 2.0, 2.0, 1.0);\n"
        "    if(col < 6.5) return vec4(4.0, 4.0, 4.0, 1.0);\n"
        "    if(col < 7.5) return vec4(8.0, 8.0, 8.0, 1.0);\n"
        "    if(col < 8.5) return vec4(1.0, 0.05, 0.05, 1.0);\n"
        "    if(col < 9.5) return vec4(0.05, 1.0, 0.05, 1.0);\n"
        "    if(col < 10.5) return vec4(0.05, 0.05, 1.0, 1.0);\n"
        "    if(col < 11.5) return vec4(1.0, 0.45, 0.15, 1.0);\n"
        "    return vec4(4.0, 1.0, 0.25, 1.0);\n"
        "}\n"
        "void main(void) {\n"
        "    vec2 uv = texcoord0;\n"
        "    vec3 scene;\n"
        "    vec4 inj;\n"
        "    vec3 x;\n"
        "    vec3 sc;\n"
        "    vec3 y;\n"
        "    vec3 nits;\n"
        "    float Y;\n"
        "    float n1;\n"
        "    float L1;\n"
        "    float ceiling;\n"
        "    float L;\n"
        "    float span;\n"
        "    float kk;\n"
        "    float m;\n"
        "    uv = texcoord0;\n"
        "    if(hdroutq.x > 0.5) uv.y = 1.0 - uv.y;\n"
        "    scene = max(texture2D(tex0, uv).rgb, vec3(0.0));\n"
        "    if(hdroutq.y > 0.5) {\n"
        "        inj = hdr_patch(gl_FragCoord.xy);\n"
        "        if(inj.a >= 0.0) scene = inj.rgb;\n"
        "    }\n"
        "    x = scene * hdroutp.x;\n"
        "    if(hdroutp.w > 0.5) {\n"
        "        y = clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);\n"
        "        sc = y * (hdroutp.y / 80.0);\n"
        "    } else if(hdroutq.z > 0.5) {\n"
        "        Y = hdr_luma(x);\n"
        "        n1 = hdr_nark(1.0);\n"
        "        L1 = n1 * hdroutp.y;\n"
        "        ceiling = max(hdroutp.z, 1.0);\n"
        "        if(Y <= 0.0) L = 0.0;\n"
        "        else if(hdroutq.z > 3.5 && Y > hdrouts.z) L = hdr_lot6(Y) * hdroutp.y;\n"
        "        else if(hdroutq.z > 2.5 && hdroutq.z < 3.5 && Y > 0.12) L = hdr_lot5(Y) * hdroutp.y;\n"
        "        else if(Y <= 1.0) L = (hdroutq.z > 1.5 ? hdr_body(Y) : hdr_nark(Y)) * hdroutp.y;\n"
        "        else {\n"
        "            span = max(ceiling - L1, 0.0);\n"
        "            kk = max(hdroutq.w, 0.0001);\n"
        "            L = L1 + span * (1.0 - exp(-(Y - 1.0) / kk));\n"
        "        }\n"
        "        if(L > ceiling) L = ceiling;\n"
        "        nits = (Y > 0.000001) ? x * (L / Y) : vec3(0.0);\n"
        "        m = max(nits.r, max(nits.g, nits.b));\n"
        "        if(m > ceiling) nits *= ceiling / m;\n"
        "        sc = nits / 80.0;\n"
        "    } else {\n"
        "        nits = x * hdroutp.y;\n"
        "        Y = hdr_luma(nits);\n"
        "        if(Y > hdroutp.z && Y > 0.0) nits *= hdroutp.z / Y;\n"
        "        sc = nits / 80.0;\n"
        "    }\n"
        "    gl_FragColor = vec4(sc, 1.0);\n"
        "}\n";
    static const char *mireps =
        "void main(void) {\n"
        "    float x = gl_FragCoord.x;\n"
        "    float y = gl_FragCoord.y;\n"
        "    float v = -1.0;\n"
        "    if(y >= 2.0 && y < 14.0) {\n"
        "        if(x >= 8.0 && x < 24.0) v = 0.0;\n"
        "        else if(x >= 32.0 && x < 48.0) v = 1.0;\n"
        "        else if(x >= 56.0 && x < 72.0) v = 2.0;\n"
        "        else if(x >= 80.0 && x < 96.0) v = 4.0;\n"
        "        else if(x >= 104.0 && x < 120.0) v = 8.0;\n"
        "    }\n"
        "    if(v < 0.0) discard;\n"
        "    gl_FragColor = vec4(v, v, v, 1.0);\n"
        "}\n";
    if(!g_map || !g_map->loaded() || !g_map->program)
        g_map = newshader(0, "hdroutmap", vs, mapps, NULL, 0);
    if(!g_mire || !g_mire->loaded() || !g_mire->program)
        g_mire = newshader(0, "hdroutmire", vs, mireps, NULL, 0);
    if(!g_map || !g_mire || g_map->invalid() || g_mire->invalid() || !g_map->program || !g_mire->program)
    {
        set_reason("presentation shaders missing");
        return false;
    }
    return true;
}

static bool make_flip_shader()
{
    if(g_vs && g_ps) return true;
    HMODULE mod = LoadLibraryA("d3dcompiler_47.dll");
    if(!mod)
    {
        set_reason("d3dcompiler_47 missing");
        return false;
    }
    PFN_D3DCompile compile = (PFN_D3DCompile)GetProcAddress(mod, "D3DCompile");
    if(!compile)
    {
        set_reason("D3DCompile missing");
        return false;
    }
    static const char *src =
        "Texture2D t : register(t0);\n"
        "SamplerState s : register(s0);\n"
        "struct V { float4 p : SV_Position; float2 u : TEXCOORD0; };\n"
        "V vs(uint id : SV_VertexID) {\n"
        "    V o; float2 uv = float2((id << 1) & 2, id & 2);\n"
        "    o.p = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
        "    o.u = uv; return o;\n"
        "}\n"
        "float4 ps(V i) : SV_Target { return t.Sample(s, float2(i.u.x, 1.0 - i.u.y)); }\n";
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = compile(src, strlen(src), "hdrout", NULL, NULL, "vs", "vs_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &err);
    if(FAILED(hr) || !code)
    {
        if(err) logoutf("hdrout hlsl vs %s", (const char *)err->GetBufferPointer());
        if(err) err->Release();
        set_reason("D3D pass compilation failed");
        logoutf("hdrout api D3DCompile vs hr 0x%08X", (unsigned)hr);
        return false;
    }
    hr = g_dev->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), NULL, &g_vs);
    code->Release();
    if(err) err->Release();
    err = NULL;
    code = NULL;
    if(FAILED(hr)) { set_reason("D3D vertex shader failed"); return false; }
    hr = compile(src, strlen(src), "hdrout", NULL, NULL, "ps", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &err);
    if(FAILED(hr) || !code)
    {
        if(err) logoutf("hdrout hlsl ps %s", (const char *)err->GetBufferPointer());
        if(err) err->Release();
        set_reason("D3D pass compilation failed");
        return false;
    }
    hr = g_dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), NULL, &g_ps);
    code->Release();
    if(err) err->Release();
    if(FAILED(hr)) { set_reason("D3D pixel shader failed"); return false; }

    D3D11_SAMPLER_DESC sd;
    memset(&sd, 0, sizeof(sd));
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    hr = g_dev->CreateSamplerState(&sd, &g_samp);
    if(FAILED(hr)) { set_reason("D3D sampler failed"); return false; }

    D3D11_RASTERIZER_DESC rd;
    memset(&rd, 0, sizeof(rd));
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    hr = g_dev->CreateRasterizerState(&rd, &g_rast);
    if(FAILED(hr)) { set_reason("D3D rasterizer failed"); return false; }

    D3D11_BLEND_DESC bd;
    memset(&bd, 0, sizeof(bd));
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = g_dev->CreateBlendState(&bd, &g_blend);
    if(FAILED(hr)) { set_reason("D3D blend failed"); return false; }

    D3D11_DEPTH_STENCIL_DESC dd;
    memset(&dd, 0, sizeof(dd));
    hr = g_dev->CreateDepthStencilState(&dd, &g_depth);
    if(FAILED(hr)) { set_reason("D3D depth failed"); return false; }
    return true;
}

static bool make_share(int w, int h)
{
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof(td));
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    HRESULT hr = g_dev->CreateTexture2D(&td, NULL, &g_shared);
    if(FAILED(hr) || !g_shared)
    {
        set_reason("shared FP16 texture failed");
        logoutf("hdrout api CreateTexture2D hr 0x%08X", (unsigned)hr);
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC sv;
    memset(&sv, 0, sizeof(sv));
    sv.Format = td.Format;
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    hr = g_dev->CreateShaderResourceView(g_shared, &sv, &g_srv);
    if(FAILED(hr))
    {
        set_reason("D3D view failed");
        logoutf("hdrout api SRV hr 0x%08X", (unsigned)hr);
        return false;
    }
    td.BindFlags = 0;
    td.MiscFlags = 0;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = g_dev->CreateTexture2D(&td, NULL, &g_staging);
    if(FAILED(hr))
    {
        set_reason("readback texture failed");
        logoutf("hdrout api staging hr 0x%08X", (unsigned)hr);
        return false;
    }
    glGenTextures(1, &g_gltex);
    g_dxobj = pReg(g_dx, g_shared, g_gltex, GL_TEXTURE_2D, WGL_ACCESS_READ_WRITE_NV);
    if(!g_dxobj)
    {
        set_reason("GL-D3D registration failed");
        logoutf("hdrout api wglDXRegisterObjectNV gl 0x%x", int(glGetError()));
        return false;
    }
    return true;
}

static bool make_chain(int w, int h)
{
    if(!ensure_child()) return false;
    place_child(true);
    if(g_parent)
    {
        RECT rc;
        if(GetClientRect(g_parent, &rc))
        {
            int cw = int(rc.right - rc.left), ch = int(rc.bottom - rc.top);
            if(cw != w || ch != h)
                logoutf("hdrout client hwnd %dx%d sdl %dx%d", cw, ch, w, h);
        }
    }
    if(g_factory) g_factory->MakeWindowAssociation(g_child, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    DXGI_SWAP_CHAIN_DESC1 sd;
    memset(&sd, 0, sizeof(sd));
    sd.Width = w;
    sd.Height = h;
    sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Scaling = DXGI_SCALING_STRETCH;
    HRESULT hr = g_factory->CreateSwapChainForHwnd(g_dev, g_child, &sd, NULL, NULL, &g_swap);
    if(FAILED(hr))
    {
        sd.Scaling = DXGI_SCALING_NONE;
        hr = g_factory->CreateSwapChainForHwnd(g_dev, g_child, &sd, NULL, NULL, &g_swap);
        if(SUCCEEDED(hr)) logoutf("hdrout swap chain without DXGI_SCALING_STRETCH: a %dx%d picture smaller than the screen will not be stretched", w, h);
    }
    if(FAILED(hr) || !g_swap)
    {
        set_reason("FP16 flip swap chain failed");
        logoutf("hdrout api CreateSwapChainForHwnd hr 0x%08X", (unsigned)hr);
        return false;
    }
    hr = g_swap->QueryInterface(IID_IDXGISwapChain3, (void **)&g_swap3);
    if(FAILED(hr) || !g_swap3)
    {
        set_reason("IDXGISwapChain3 unavailable");
        logoutf("hdrout api SwapChain3 hr 0x%08X", (unsigned)hr);
        return false;
    }
    g_support = 0;
    hr = g_swap3->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, &g_support);
    logoutf("hdrout api CheckColorSpaceSupport hr 0x%08X flags 0x%X", (unsigned)hr, g_support);
    hr = g_swap3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
    g_sethr = (unsigned)hr;
    logoutf("hdrout api SetColorSpace1 G10_scRGB hr 0x%08X", g_sethr);
    if(FAILED(hr))
    {
        set_reason("scRGB colour space refused");
        return false;
    }
    hr = g_swap->GetBuffer(0, IID_ID3D11Texture2D, (void **)&g_back);
    if(FAILED(hr) || !g_back)
    {
        set_reason("no presentation buffer");
        logoutf("hdrout api GetBuffer hr 0x%08X", (unsigned)hr);
        return false;
    }
    hr = g_dev->CreateRenderTargetView(g_back, NULL, &g_rtv);
    if(FAILED(hr) || !g_rtv)
    {
        set_reason("no buffer view");
        logoutf("hdrout api RTV hr 0x%08X", (unsigned)hr);
        return false;
    }
    return true;
}

static bool build_targets(int w, int h)
{
    release_views();
    if(g_swap)
    {
        g_swap3->Release();
        g_swap3 = NULL;
        g_swap->Release();
        g_swap = NULL;
    }
    if(!make_chain(w, h)) return false;
    if(!make_share(w, h)) return false;
    g_w = w;
    g_h = h;
    return true;
}

static bool lock_share()
{
    if(g_locked) return true;
    if(!g_dx || !g_dxobj || !pLock) return false;
    if(!pLock(g_dx, 1, &g_dxobj))
    {
        logoutf("hdrout api wglDXLockObjectsNV failed");
        hflush();
        abandon_hdr("wglDXLockObjectsNV lock failed");
        return false;
    }
    g_locked = true;
    g_ui = true;
    if(!g_fbo) glGenFramebuffers_(1, &g_fbo);
    glBindFramebuffer_(GL_FRAMEBUFFER, g_fbo);
    glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_gltex, 0);
    GLenum st = glCheckFramebufferStatus_(GL_FRAMEBUFFER);
    if(st != GL_FRAMEBUFFER_COMPLETE)
    {
        logoutf("hdrout fbo incomplete 0x%x", int(st));
        glBindFramebuffer_(GL_FRAMEBUFFER, 0);
        if(pUnlock && g_dx && g_dxobj && !pUnlock(g_dx, 1, &g_dxobj))
            logoutf("hdrout api wglDXUnlockObjectsNV failed");
        g_locked = false;
        g_ui = false;
        abandon_hdr("presentation FBO incomplete");
        return false;
    }
    glViewport(0, 0, g_w, g_h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    return true;
}

// Performance lot 1. hdroutsync 0 = lot 8 transport: glFinish before the
// unlock, swap chain buffer and view taken again every frame.
// 1 = no glFinish. WGL_NV_DX_interop (rev. 1): "The Lock/Unlock calls serve
// as synchronization points between OpenGL and DirectX. They ensure that any
// rendering operations that affect the resource on one driver are complete
// before the other driver takes ownership of it." A GL fence per frame keeps
// the CPU at most one frame ahead of the GPU (see sync_limit).
static void hdrout_on_sync();
VARF(hdroutsync, 0, 1, 1, hdrout_on_sync());

static PFNGLFENCESYNCPROC pFenceSync = NULL;
static PFNGLCLIENTWAITSYNCPROC pClientWaitSync = NULL;
static PFNGLDELETESYNCPROC pDeleteSync = NULL;
static bool g_syncload = false;
static GLsync g_fence_cur = NULL, g_fence_prev = NULL;
static int g_sync_timeouts = 0;

static bool sync_ready()
{
    if(!g_syncload)
    {
        g_syncload = true;
        pFenceSync = (PFNGLFENCESYNCPROC)wglGetProcAddress("glFenceSync");
        pClientWaitSync = (PFNGLCLIENTWAITSYNCPROC)wglGetProcAddress("glClientWaitSync");
        pDeleteSync = (PFNGLDELETESYNCPROC)wglGetProcAddress("glDeleteSync");
        logoutf("hdrout transport: glFenceSync %s glClientWaitSync %s glDeleteSync %s",
                pFenceSync ? "ok" : "missing", pClientWaitSync ? "ok" : "missing", pDeleteSync ? "ok" : "missing");
    }
    return pFenceSync && pClientWaitSync && pDeleteSync;
}

static void sync_drop_fences()
{
    if(pDeleteSync)
    {
        if(g_fence_cur) pDeleteSync(g_fence_cur);
        if(g_fence_prev) pDeleteSync(g_fence_prev);
    }
    g_fence_cur = g_fence_prev = NULL;
}

// Called after Present. Waits for the GL work of the previous frame, so the
// GPU holds at most this frame while the CPU prepares the next one.
static void sync_limit()
{
    if(!g_fence_cur && !g_fence_prev) return;
    if(g_fence_prev)
    {
        double t0 = perf_now();
        GLenum r = pClientWaitSync(g_fence_prev, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
        if(r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED)
        {
            if(g_sync_timeouts++ < 4) logoutf("hdrout transport: GL fence wait result 0x%X, fallback glFinish", unsigned(r));
            glFinish();
        }
        perf_add(8, perf_now() - t0);
        pDeleteSync(g_fence_prev);
    }
    g_fence_prev = g_fence_cur;
    g_fence_cur = NULL;
}

static bool unlock_share()
{
    if(!g_locked) return true;
    glBindFramebuffer_(GL_FRAMEBUFFER, 0);
    double t0 = perf_now();
    if(hdroutsync && sync_ready())
    {
        if(g_fence_cur) pDeleteSync(g_fence_cur);
        g_fence_cur = pFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    }
    else
    {
        glFinish();
        perf_add(3, perf_now() - t0);
    }
    double t1 = perf_now();
    BOOL ok = FALSE;
    if(pUnlock && g_dx && g_dxobj) ok = pUnlock(g_dx, 1, &g_dxobj);
    perf_add(4, perf_now() - t1);
    g_locked = false;
    g_ui = false;
    if(!ok)
    {
        logoutf("hdrout api wglDXUnlockObjectsNV failed");
        hflush();
        abandon_hdr("wglDXUnlockObjectsNV unlock failed");
        return false;
    }
    return true;
}

static bool want_surface()
{
    return g_ready && hwrthdr && screenw >= 8 && screenh >= 8 && !minimized;
}

static bool ensure_size()
{
    if(!want_surface()) return false;
    if(!make_shaders()) return false;
    if(g_w != screenw || g_h != screenh || !g_swap || !g_shared)
    {
        if(!build_targets(screenw, screenh))
        {
            g_ready = false;
            g_open_failed = true;
            hide_child();
            log_state("fallback");
            return false;
        }
        logoutf("hdrout targets %dx%d", g_w, g_h);
        hflush();
    }
    return lock_share();
}

static void plain_draw()
{
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

static float half_to_float(unsigned short h)
{
    unsigned int s = (unsigned int)(h & 0x8000u) << 16;
    int e = (h >> 10) & 0x1f;
    unsigned int m = h & 0x3ffu;
    unsigned int u;
    if(e == 0)
    {
        if(!m) u = s;
        else
        {
            while((m & 0x400u) == 0) { m <<= 1; e--; }
            e++;
            m &= 0x3ffu;
            u = s | ((unsigned)(e + (127 - 15)) << 23) | (m << 13);
        }
    }
    else if(e == 31) u = s | 0x7f800000u | (m << 13);
    else u = s | ((unsigned)(e + (127 - 15)) << 23) | (m << 13);
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

static void sample_px(const D3D11_MAPPED_SUBRESOURCE &map, int x, int y, float rgba[4])
{
    rgba[0] = rgba[1] = rgba[2] = rgba[3] = -1.f;
    if(x < 0 || y < 0 || x >= g_w || y >= g_h || !map.pData) return;
    const unsigned char *row = (const unsigned char *)map.pData + y * map.RowPitch;
    const unsigned short *px = (const unsigned short *)(row + x * 8);
    rgba[0] = half_to_float(px[0]);
    rgba[1] = half_to_float(px[1]);
    rgba[2] = half_to_float(px[2]);
    rgba[3] = half_to_float(px[3]);
}

static float enc_srgb(float y)
{
    if(y < 0.f) y = 0.f;
    if(y > 1.f) y = 1.f;
    if(y <= 0.0031308f) return y * 12.92f;
    return 1.055f * powf(y, 1.f / 2.4f) - 0.055f;
}

static void write_diag_bmp(const D3D11_MAPPED_SUBRESOURCE &map)
{
    if(g_w < 8 || g_h < 8 || g_w * g_h > 8000000) return;
    // Written in the profile (home) folder, never in the game or lab folders.
    defformatstring(rel, "captures/swap-%d-ecrete.bmp", g_readn);
    const char *path = findfile(::path(rel), "wb");
    FILE *f = fopen(path, "wb");
    if(!f)
    {
        logoutf("hdrout bmp missing %s", path);
        return;
    }
    int rowb = g_w * 3;
    int pad = (4 - (rowb & 3)) & 3;
    int stride = rowb + pad;
    unsigned filesz = 54u + (unsigned)stride * (unsigned)g_h;
    unsigned char hdr[54];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = filesz & 255; hdr[3] = (filesz >> 8) & 255; hdr[4] = (filesz >> 16) & 255; hdr[5] = (filesz >> 24) & 255;
    hdr[10] = 54;
    hdr[14] = 40;
    hdr[18] = g_w & 255; hdr[19] = (g_w >> 8) & 255; hdr[20] = (g_w >> 16) & 255; hdr[21] = (g_w >> 24) & 255;
    hdr[22] = g_h & 255; hdr[23] = (g_h >> 8) & 255; hdr[24] = (g_h >> 16) & 255; hdr[25] = (g_h >> 24) & 255;
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    float refsc = float(hdroutref) / 80.f;
    if(refsc < 0.05f) refsc = 1.f;
    vector<uchar> row;
    row.pad(stride);
    int y;
    for(y = g_h - 1; y >= 0; y--)
    {
        int x;
        for(x = 0; x < g_w; x++)
        {
            float rgba[4];
            sample_px(map, x, y, rgba);
            loopk(3)
            {
                float e = enc_srgb(rgba[2 - k] / refsc);
                int b = int(e * 255.f + 0.5f);
                if(b < 0) b = 0;
                if(b > 255) b = 255;
                row[x * 3 + k] = (uchar)b;
            }
        }
        loopk(pad) row[rowb + k] = 0;
        fwrite(row.getbuf(), 1, stride, f);
    }
    fclose(f);
    logoutf("hdrout bmp diagnostic clipped at reference white, not a measurement: %s", path);
}

static void read_presented()
{
    if(!g_ctx || !g_back || !g_staging) return;
    g_ctx->CopyResource(g_staging, g_back);
    D3D11_MAPPED_SUBRESOURCE map;
    memset(&map, 0, sizeof(map));
    HRESULT hr = g_ctx->Map(g_staging, 0, D3D11_MAP_READ, 0, &map);
    logoutf("hdrout api Map read hr 0x%08X pitch %u", (unsigned)hr, map.RowPitch);
    if(FAILED(hr))
    {
        hflush();
        return;
    }
    int py = g_h - 1 - MIRE_GY;
    int alt = MIRE_GY;
    logoutf("hdrout reading the RGBA16F scRGB swap chain target before Present. 1.0 = 80 nits. Not a panel measurement.");
    int pass = 0;
    loopi(5)
    {
        float got[4], other[4];
        sample_px(map, MIRE_X[i], py, got);
        sample_px(map, MIRE_X[i], alt, other);
        float err = fabsf(got[0] - MIRE_V[i]);
        bool ok = err <= 0.02f && fabsf(got[1] - MIRE_V[i]) <= 0.02f && fabsf(got[2] - MIRE_V[i]) <= 0.02f;
        if(ok) pass++;
        logoutf("hdrout patch %d x %d y %d expect %.3f got %.6f %.6f %.6f %s other_y %d %.6f",
                i, MIRE_X[i], py, MIRE_V[i], got[0], got[1], got[2], ok ? "PASS" : "FAIL", alt, other[0]);
    }
    logoutf("hdrout patch result %d/5", pass);
    if(pass != 5)
    {
        int y;
        for(y = 0; y < g_h && y < 20; y++)
        {
            float p[4];
            sample_px(map, 16, y, p);
            logoutf("hdrout sweep top y %d %.4f", y, p[0]);
        }
        for(y = max(0, g_h - 20); y < g_h; y++)
        {
            float p[4];
            sample_px(map, 16, y, p);
            logoutf("hdrout sweep bottom y %d %.4f", y, p[0]);
        }
    }
    g_readn++;
    write_diag_bmp(map);
    g_ctx->Unmap(g_staging, 0);
    hflush();
}

static void release_back()
{
    if(g_ctx) g_ctx->OMSetRenderTargets(0, NULL, NULL);
    if(g_rtv) { g_rtv->Release(); g_rtv = NULL; }
    if(g_back) { g_back->Release(); g_back = NULL; }
}

static bool acquire_back()
{
    release_back();
    if(!g_swap || !g_dev) return false;
    HRESULT hr = g_swap->GetBuffer(0, IID_ID3D11Texture2D, (void **)&g_back);
    if(FAILED(hr) || !g_back)
    {
        logoutf("hdrout api GetBuffer hr 0x%08X", (unsigned)hr);
        hflush();
        return false;
    }
    hr = g_dev->CreateRenderTargetView(g_back, NULL, &g_rtv);
    if(FAILED(hr) || !g_rtv)
    {
        logoutf("hdrout api RTV hr 0x%08X", (unsigned)hr);
        release_back();
        hflush();
        return false;
    }
    return true;
}

static void read_curve_swap()
{
    if(!g_ctx || !g_back || !g_staging) return;
    g_ctx->CopyResource(g_staging, g_back);
    D3D11_MAPPED_SUBRESOURCE map;
    memset(&map, 0, sizeof(map));
    HRESULT hr = g_ctx->Map(g_staging, 0, D3D11_MAP_READ, 0, &map);
    logoutf("hdrout check swapchain Map hr 0x%08X. RGBA16F scRGB before Present. 1 = 80 nits. Not a panel measurement.", (unsigned)hr);
    if(FAILED(hr))
    {
        hflush();
        return;
    }
    int cy = patch_cy();
    int yTop = g_h - 1 - cy;
    int use = yTop;
    float atTop[4], atBot[4];
    sample_px(map, patch_cx(4), yTop, atTop);
    sample_px(map, patch_cx(4), cy, atBot);
    float dTop = fabsf(atTop[0] - g_patch_gl[4][0]) + fabsf(atTop[1] - g_patch_gl[4][1]) + fabsf(atTop[2] - g_patch_gl[4][2]);
    float dBot = fabsf(atBot[0] - g_patch_gl[4][0]) + fabsf(atBot[1] - g_patch_gl[4][1]) + fabsf(atBot[2] - g_patch_gl[4][2]);
    if(dBot + 0.0001f < dTop) use = cy;
    logoutf("hdrout check swap row %d scene1_gap top %d=%.4f bottom %d=%.4f", use, yTop, dTop, cy, dBot);
    int pass = 0, i;
    for(i = 0; i < PATCH_N; i++)
    {
        int cx = patch_cx(i);
        float got[4];
        sample_px(map, cx, use, got);
        float d0 = fabsf(got[0] - g_patch_gl[i][0]);
        float d1 = fabsf(got[1] - g_patch_gl[i][1]);
        float d2 = fabsf(got[2] - g_patch_gl[i][2]);
        bool ok = d0 <= 0.02f && d1 <= 0.02f && d2 <= 0.02f;
        if(ok) pass++;
        logoutf("hdrout check swap %s x %d y %d gl %.5f %.5f %.5f got %.5f %.5f %.5f %s",
                PATCH_NAME[i], cx, use, g_patch_gl[i][0], g_patch_gl[i][1], g_patch_gl[i][2],
                got[0], got[1], got[2], ok ? "PASS" : "FAIL");
    }
    logoutf("hdrout check swapchain result %d/%d (compared with the GL readback, not the panel)", pass, PATCH_N);
    g_ctx->Unmap(g_staging, 0);
    hflush();
}

// Low latency: DXGI lets up to three presents wait for the screen by default;
// with V-Sync that is up to three refreshes between the frame and the screen.
// One keeps the GPU fed. Without V-Sync the flip model already drops stale
// frames and a shorter queue only costs frame rate, so it is left alone, as
// it is with Off.
static int g_framelatency = 0;
static void apply_framelatency()
{
    extern int lowlatency;
    int want = lowlatency && curvsync > 0 ? 1 : 3;
    if(!g_dev || want == g_framelatency || (!g_framelatency && want == 3)) return;
    IDXGIDevice1 *d1 = NULL;
    if(SUCCEEDED(g_dev->QueryInterface(IID_IDXGIDevice1, (void **)&d1)) && d1)
    {
        HRESULT hr = d1->SetMaximumFrameLatency(want);
        d1->Release();
        logoutf("hdrout api SetMaximumFrameLatency %d hr 0x%08X", want, (unsigned)hr);
    }
    g_framelatency = want;
}

static bool present_dx()
{
    if(!g_ctx || !g_srv || !g_swap || !g_vs)
    {
        abandon_hdr("presenter incomplete");
        return false;
    }
    double t0 = perf_now();
    // Flip model, D3D11: buffer 0 always names the current back buffer, so
    // the optimised path keeps the view made with the swap chain.
    if((!hdroutsync || !g_rtv || !g_back) && !acquire_back())
    {
        abandon_hdr("no presentation buffer");
        return false;
    }
    perf_d3d_begin();
    D3D11_VIEWPORT vp;
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    vp.Width = float(g_w);
    vp.Height = float(g_h);
    vp.MinDepth = 0;
    vp.MaxDepth = 1;
    g_ctx->OMSetRenderTargets(1, &g_rtv, NULL);
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->RSSetState(g_rast);
    g_ctx->OMSetBlendState(g_blend, NULL, 0xffffffffu);
    g_ctx->OMSetDepthStencilState(g_depth, 0);
    g_ctx->IASetInputLayout(NULL);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_vs, NULL, 0);
    g_ctx->PSSetShader(g_ps, NULL, 0);
    g_ctx->PSSetShaderResources(0, 1, &g_srv);
    g_ctx->PSSetSamplers(0, 1, &g_samp);
    g_ctx->Draw(3, 0);
    ID3D11ShaderResourceView *ns = NULL;
    g_ctx->PSSetShaderResources(0, 1, &ns);
    g_ctx->OMSetRenderTargets(0, NULL, NULL);
    if(hdroutread)
    {
        hdroutread = 0;
        read_presented();
    }
    if(g_present_patches)
    {
        g_present_patches = 0;
        read_curve_swap();
    }
    perf_d3d_end();
    fresh_check();
    if(!hdroutsync) release_back();
    perf_add(5, perf_now() - t0);
    // Lab only. After at least one real Present, skip the call and take the
    // same fallback as a failed Present. This is not a device-removal.
    if(hdroutsim == 1 && g_okpresents >= 1)
    {
        hdroutsim = 0;
        logoutf("hdrout SIMULATION: controlled Present failure after %d successful Presents. Present is not called. No device loss. Windows HDR, VRR, gamma and the driver are not changed. Not proof of a hardware fault.", g_okpresents);
        hflush();
        abandon_hdr("simulation: Present failed");
        return false;
    }
    apply_framelatency();
    UINT sync = curvsync > 0 ? 1u : 0u;
    double t1 = perf_now();
    HRESULT hr = g_swap->Present(sync, 0);
    perf_add(6, perf_now() - t1);
    if((unsigned)hr != g_lastpresent || hr == DXGI_STATUS_OCCLUDED || FAILED(hr))
    {
        g_lastpresent = (unsigned)hr;
        logoutf("hdrout api Present hr 0x%08X sync %u", g_lastpresent, sync);
        hflush();
    }
    if(FAILED(hr))
    {
        defformatstring(why, "Present failed 0x%08X", (unsigned)hr);
        abandon_hdr(why);
        return false;
    }
    g_okpresents++;
    return true;
}

void hdrout_applyshader(Shader *s)
{
    if(!s || !s->name || !s->program || Shader::lastshader != s) return;
    const char *n = s->name;
    bool hud = !strcmp(n, "hud") || !strcmp(n, "hudnotexture") || !strcmp(n, "hudrgb")
            || !strcmp(n, "<init>hud") || !strcmp(n, "<init>hudnotexture");
    if(!hud) return;
    float on = 0.f, scale = 1.f, premul = 0.f;
    if(g_ui)
    {
        on = 1.f;
        scale = hud_nits() / 80.f;
        GLint blend = 0, src = 0, dst = 0;
        glGetIntegerv(GL_BLEND, &blend);
        glGetIntegerv(GL_BLEND_SRC_RGB, &src);
        glGetIntegerv(GL_BLEND_DST_RGB, &dst);
        if(blend && src == GL_ONE && dst == GL_ONE_MINUS_SRC_ALPHA) premul = 1.f;
    }
    LOCALPARAMF(hdroutui, on, scale, premul, 0.f);
    static int logged = 0;
    if(!logged)
    {
        GLint loc = glGetUniformLocation_(s->program, "hdroutui");
        logoutf("hdrout ui shader %s loc %d", n, int(loc));
        logged = 1;
        hflush();
    }
}

// Output and lighting are two separate choices (F4/F5 and F3), so two lines.
// Lab overlay only. Off in normal play; the Graphics menu shows the output state.
VAR(hdroutlabel, 0, 0, 1);
void hdrout_drawlabel()
{
    if(!hdroutlabel) return;
    const char *s = hdrout_label_text();
    if(!s || !s[0]) return;
    draw_text(s, 12, 28, 255, 255, 255, 255);
    draw_text(lighting_label(), 12, 28 + FONTH, 200, 200, 200, 255);
    // Performance lot 1: third line, the HDR transport chosen by F5.
    defformatstring(tr, "Transport HDR : %s%s", hdroutsync ? "Version optimis" CUBE_E_AIGU "e" : "R" CUBE_E_AIGU "f" CUBE_E_AIGU "rence lot8",
                    g_locked || (g_ready && hwrthdr) ? "" : " (sans effet en SDR de base)");
    draw_text(tr, 12, 28 + 2 * FONTH, 200, 200, 200, 255);
}

static float nark_f(float v)
{
    if(v < 0.f) v = 0.f;
    float y = (v * (2.51f * v + 0.03f)) / (v * (2.43f * v + 0.59f) + 0.14f);
    if(y < 0.f) y = 0.f;
    if(y > 1.f) y = 1.f;
    return y;
}

static float smooth01(float e0, float e1, float x)
{
    float t;
    if(x <= e0) return 0.f;
    if(x >= e1) return 1.f;
    t = (x - e0) / (e1 - e0);
    return t * t * (3.f - 2.f * t);
}

// Opens the input of the lot1 curve below reference white, then blends back
// so Y = 1 and the shoulder stay on that curve. Not an exposure and not a saturation.
static float proposal_f(float Y)
{
    const float k = 1.80f;
    float lifted, u, z;
    if(Y <= 0.f) return 0.f;
    if(Y > 1.f) Y = 1.f;
    lifted = (k * Y) / (1.f + Y * (k - 1.f));
    u = smooth01(0.40f, 0.92f, Y);
    z = (1.f - u) * lifted + u * Y;
    return nark_f(z);
}

static float nark_slope1()
{
    const float x = 1.f, a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    float num = x * (a * x + b);
    float den = x * (c * x + d) + e;
    float dnum = 2.f * a * x + b;
    float dden = 2.f * c * x + d;
    return (dnum * den - num * dden) / (den * den);
}

static float luma3(float r, float g, float b)
{
    return r * 0.2126f + g * 0.7152f + b * 0.0722f;
}

static float shoulder_k(float ref, float peak)
{
    float L1 = nark_f(1.f) * ref;
    float ceiling = peak > 1.f ? peak : 1.f;
    float span = ceiling - L1;
    float slope = nark_slope1();
    float k = 1.f;
    if(span > 1.f && slope > 1e-6f && ref > 1.f) k = span / (slope * ref);
    if(k < 0.05f) k = 0.05f;
    if(k > 1000.f) k = 1000.f;
    return k;
}

// Lot 5 curve, in units of the reference white. Up to scene LOT5_YA it is curve 2.
// Above, in log/log, the local contrast starts at the curve 2 one at LOT5_YA and
// eases to LOT5_SM with scale LOT5_W; then la + span*t/sqrt(1+t^2) reaches the
// peak softly. Value and slope are continuous at LOT5_YA; monotone.
static const float LOT5_YA = 0.12f, LOT5_SM = 0.55f, LOT5_W = 0.5f;

static void lot5_params(float ref, float peak, float &la, float &sa, float &span)
{
    const float h = 1e-3f;
    la = proposal_f(LOT5_YA);
    sa = (logf(proposal_f(LOT5_YA * expf(h))) - logf(proposal_f(LOT5_YA * expf(-h)))) / (2.f * h);
    float P = (peak > 1.f ? peak : 1.f) / (ref > 1.f ? ref : 1.f);
    span = P - la;
    if(span < 1e-4f) span = 1e-4f;
}

static float lot5_raw(float Y, float la, float sa)
{
    if(Y <= LOT5_YA) return proposal_f(Y);
    float x = logf(Y / LOT5_YA);
    return la * expf(LOT5_SM * x + (sa - LOT5_SM) * LOT5_W * (1.f - expf(-x / LOT5_W)));
}

static float lot5_f(float Y, float la, float sa, float span)
{
    if(Y <= LOT5_YA) return proposal_f(Y);
    float t = (lot5_raw(Y, la, sa) - la) / span;
    return la + span * t / sqrtf(1.f + t * t);
}

// Lot 6 curve, in units of the reference white. Up to scene LOT6_Y0 it is curve 3
// (curve 2 there). Above, the curve 3 growth before its soft limit is multiplied by a
// gain whose log is LOT6_GAIN's times smoothstep(ln Y between LOT6_Y0 and LOT6_Y1),
// then b + span*t/sqrt(1+t^2) reaches the peak softly from b = curve 3 at LOT6_Y0.
// Value continuous at LOT6_Y0, slope continuous to 0.2 %; monotone (courbe6.py).
static const float LOT6_Y0 = 0.05f, LOT6_Y1 = 0.45f, LOT6_GAIN = 1.70f;

static void lot6_params(float ref, float peak, float &b, float &span)
{
    float la, sa, span5;
    lot5_params(ref, peak, la, sa, span5);
    b = lot5_f(LOT6_Y0, la, sa, span5);
    float P = (peak > 1.f ? peak : 1.f) / (ref > 1.f ? ref : 1.f);
    span = P - b;
    if(span < 1e-4f) span = 1e-4f;
}

static float lot6_f(float Y, float ref, float peak)
{
    float la, sa, span5, b, span;
    lot5_params(ref, peak, la, sa, span5);
    if(Y <= LOT6_Y0) return lot5_f(Y, la, sa, span5);
    lot6_params(ref, peak, b, span);
    float u = logf(Y / LOT6_Y0) / logf(LOT6_Y1 / LOT6_Y0);
    if(u > 1.f) u = 1.f;
    float raw = lot5_raw(Y, la, sa) * expf(logf(LOT6_GAIN) * u * u * (3.f - 2.f * u));
    float t = (raw - b) / span;
    return b + span * t / sqrtf(1.f + t * t);
}

// Lot 8 daylight response (glsl.cfg hdrjourmat). Output target for a daylit material:
// T(Y) = P u / sqrt(1 + u^2), u = k Y (Y / (Y + JOUR_YT))^JOUR_TOE / P, P = peak, where
// k = max C(Y) / Y is the slope of the line from the origin tangent to the common curve C
// (lot 6, in nits). T is kept >= C. ln G(Y) = ln(C^-1(T(Y)) / Y) on JOUR_N points of ln Y
// in [JOUR_Y0, JOUR_Y1], constant outside. Same arithmetic as diag/courbe8.py.
static const int JOUR_N = 24;
static const float JOUR_Y0 = 0.001f, JOUR_Y1 = 16.f, JOUR_YT = 0.01f, JOUR_TOE = 0.5f;

static float jour_c(float Y, float ref, float peak)
{
    return lot6_f(Y, ref, peak) * ref;
}

static float jour_cinv(float L, float ref, float peak)
{
    float lo = logf(1e-6f), hi = logf(1e4f);
    for(int i = 0; i < 60; i++)
    {
        float mid = 0.5f * (lo + hi);
        if(jour_c(expf(mid), ref, peak) < L) lo = mid; else hi = mid;
    }
    return expf(0.5f * (lo + hi));
}

static float jour_k(float ref, float peak)
{
    float best = 0.f;
    for(int i = 0; i <= 800; i++)
    {
        float Y = expf(logf(0.01f) + (logf(4.f) - logf(0.01f)) * float(i) / 800.f);
        float r = jour_c(Y, ref, peak) / Y;
        if(r > best) best = r;
    }
    return best;
}

static float g_jour_tab[JOUR_N];
static float g_jour_ref = -1.f, g_jour_peak = -1.f, g_jour_kval = 0.f;

static void jour_update(float ref, float peak)
{
    if(ref == g_jour_ref && peak == g_jour_peak) return;
    g_jour_ref = ref; g_jour_peak = peak;
    float P = peak > 1.f ? peak : 1.f;
    float k = jour_k(ref, peak);
    g_jour_kval = k;
    for(int i = 0; i < JOUR_N; i++)
    {
        float Y = expf(logf(JOUR_Y0) + (logf(JOUR_Y1) - logf(JOUR_Y0)) * float(i) / float(JOUR_N - 1));
        float u = k * Y * powf(Y / (Y + JOUR_YT), JOUR_TOE) / P;
        float T = P * u / sqrtf(1.f + u * u);
        float c = jour_c(Y, ref, peak);
        if(T < c) T = c;
        g_jour_tab[i] = logf(jour_cinv(T, ref, peak) / Y);
    }
    logoutf("hdrout day lot8 table ref=%.1f peak=%.1f k=%.2f nits/scene lnG %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f",
            ref, peak, k, g_jour_tab[0], g_jour_tab[1], g_jour_tab[2], g_jour_tab[3], g_jour_tab[4], g_jour_tab[5], g_jour_tab[6], g_jour_tab[7],
            g_jour_tab[8], g_jour_tab[9], g_jour_tab[10], g_jour_tab[11], g_jour_tab[12], g_jour_tab[13], g_jour_tab[14], g_jour_tab[15],
            g_jour_tab[16], g_jour_tab[17], g_jour_tab[18], g_jour_tab[19], g_jour_tab[20], g_jour_tab[21], g_jour_tab[22], g_jour_tab[23]);
}

// p = hdrjour uniform (on, exposure multiplier, table index per ln Y, ln of the first
// point), tab = hdrjourt. Off (p[0] = 0) outside the native classic lot 8 chain.
void hdrout_jour_params(float p[4], float tab[24])
{
    int incomplete = 0;
    float peak = used_max_nits(&incomplete);
    jour_update(float(hdroutref), peak);
    p[0] = float(hdrout_dayfix_level());
    p[1] = powf(2.f, hwrthdrexp);
    p[2] = float(JOUR_N - 1) / (logf(JOUR_Y1) - logf(JOUR_Y0));
    p[3] = logf(JOUR_Y0);
    for(int i = 0; i < JOUR_N; i++) tab[i] = g_jour_tab[i];
}

// Same arithmetic as the presentation shader. preview = per-channel Narkowicz.
// corrected 0 = linear native. 1 = lot1 luminance curve. 2 = lot3 proposal.
// Above scene 1, 1 and 2 share the same shoulder. 3 = lot 5 curve.
static void expected_sc(float r, float g, float b, float expmul, float ref, float peak, int preview, int corrected, float shoulder, float out[3])
{
    float x[3] = { r > 0.f ? r * expmul : 0.f, g > 0.f ? g * expmul : 0.f, b > 0.f ? b * expmul : 0.f };
    if(preview)
    {
        out[0] = nark_f(x[0]) * (ref / 80.f);
        out[1] = nark_f(x[1]) * (ref / 80.f);
        out[2] = nark_f(x[2]) * (ref / 80.f);
        return;
    }
    if(!corrected)
    {
        float nits[3] = { x[0] * ref, x[1] * ref, x[2] * ref };
        float Y = luma3(nits[0], nits[1], nits[2]);
        if(Y > peak && Y > 0.f)
        {
            float s = peak / Y;
            nits[0] *= s; nits[1] *= s; nits[2] *= s;
        }
        out[0] = nits[0] / 80.f; out[1] = nits[1] / 80.f; out[2] = nits[2] / 80.f;
        return;
    }
    float Y = luma3(x[0], x[1], x[2]);
    float L1 = nark_f(1.f) * ref;
    float ceiling = peak > 1.f ? peak : 1.f;
    float L;
    if(Y <= 0.f) L = 0.f;
    else if(corrected >= 4) L = lot6_f(Y, ref, peak) * ref;
    else if(corrected >= 3)
    {
        float la, sa, span;
        lot5_params(ref, peak, la, sa, span);
        L = lot5_f(Y, la, sa, span) * ref;
    }
    else if(Y <= 1.f) L = ((corrected >= 2) ? proposal_f(Y) : nark_f(Y)) * ref;
    else
    {
        float span = ceiling - L1;
        if(span < 0.f) span = 0.f;
        float kk = shoulder > 0.0001f ? shoulder : 0.0001f;
        L = L1 + span * (1.f - expf(-(Y - 1.f) / kk));
    }
    if(L > ceiling) L = ceiling;
    float nits[3] = { 0.f, 0.f, 0.f };
    if(Y > 0.000001f)
    {
        float s = L / Y;
        nits[0] = x[0] * s; nits[1] = x[1] * s; nits[2] = x[2] * s;
    }
    float m = nits[0];
    if(nits[1] > m) m = nits[1];
    if(nits[2] > m) m = nits[2];
    if(m > ceiling)
    {
        float s = ceiling / m;
        nits[0] *= s; nits[1] *= s; nits[2] *= s;
    }
    out[0] = nits[0] / 80.f; out[1] = nits[1] / 80.f; out[2] = nits[2] / 80.f;
}

static int patch_cx(int i) { return PATCH_X0 + i * PATCH_STRIDE + PATCH_W / 2; }
static int patch_cy() { return PATCH_Y0 + PATCH_H / 2; }

static bool in_mire(int x, int y)
{
    return y >= PATCH_Y0 && y < PATCH_Y0 + PATCH_H && x >= PATCH_X0 && x < PATCH_X0 + PATCH_N * PATCH_STRIDE;
}

static float hist_pct(const int *hist, int bins, int n, float cap, float q)
{
    if(n < 1) return -1.f;
    int need = int(float(n) * q + 0.5f);
    if(need < 1) need = 1;
    int acc = 0, i;
    for(i = 0; i < bins; i++)
    {
        acc += hist[i];
        if(acc >= need) return (float(i) + 0.5f) * cap / float(bins);
    }
    return cap;
}

static void read_fbo(int x, int y, float rgba[4])
{
    rgba[0] = rgba[1] = rgba[2] = rgba[3] = -1.f;
    if(x < 0 || y < 0 || x >= g_w || y >= g_h) return;
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_FLOAT, rgba);
}

static float chroma3(float r, float g, float b)
{
    float hi = r, lo = r;
    if(g > hi) hi = g;
    if(b > hi) hi = b;
    if(g < lo) lo = g;
    if(b < lo) lo = b;
    return hi > 1e-6f ? (hi - lo) / hi : 0.f;
}

static void log_signal(const char *tag, float r, float g, float b, float expmul, float ref, float peak, float shoulder)
{
    float o[3], c[3], p[3];
    float Y = luma3(r, g, b);
    expected_sc(r, g, b, expmul, ref, peak, 0, 0, shoulder, o);
    expected_sc(r, g, b, expmul, ref, peak, 0, 1, shoulder, c);
    expected_sc(r, g, b, expmul, ref, peak, 0, 2, shoulder, p);
    logoutf("hdrout check signal %s before rgb %.5f %.5f %.5f Y %.5f chroma %.3f expY %.5f gt1 %d",
            tag, r, g, b, Y, chroma3(r, g, b), Y * expmul, (Y * expmul > 1.f) ? 1 : 0);
    logoutf("hdrout check signal %s nits origin %.2f corrected %.2f proposal %.2f chroma_prop %.3f",
            tag, luma3(o[0], o[1], o[2]) * 80.f, luma3(c[0], c[1], c[2]) * 80.f, luma3(p[0], p[1], p[2]) * 80.f,
            chroma3(p[0], p[1], p[2]));
}

static void log_curve_checks(float ref, float peak, float shoulder)
{
    static const float ys[10] = { 0.f, 0.02f, 0.05f, 0.10f, 0.20f, 0.50f, 1.f, 2.f, 4.f, 8.f };
    int finite = 1, mono = 1, gray = 1, i;
    int cur = hdroutcurve >= 4 ? 4 : hdroutcurve >= 3 ? 3 : 2;
    int prevcur = cur >= 4 ? 3 : 2;
    float prev = -1.f;
    float prop[10][3], lot1[3], lotp[3];
    for(i = 0; i < 10; i++)
    {
        expected_sc(ys[i], ys[i], ys[i], 1.f, ref, peak, 0, cur, shoulder, prop[i]);
        expected_sc(ys[i], ys[i], ys[i], 1.f, ref, peak, 0, prevcur, shoulder, lotp);
        float nits = luma3(prop[i][0], prop[i][1], prop[i][2]) * 80.f;
        if(prop[i][0] != prop[i][0] || prop[i][1] != prop[i][1] || prop[i][2] != prop[i][2]) finite = 0;
        if(fabsf(prop[i][0] - prop[i][1]) > 0.0002f || fabsf(prop[i][0] - prop[i][2]) > 0.0002f) gray = 0;
        if(nits + 0.02f < prev) mono = 0;
        prev = nits;
        logoutf("hdrout check grey curve %d Y %.2f nits %.2f (curve %d %.2f)", cur, ys[i], nits, prevcur, luma3(lotp[0], lotp[1], lotp[2]) * 80.f);
    }
    // Curve 2 shares the lot 1 shoulder; curve 3 shares curve 2 up to scene 0.12;
    // curve 4 shares curve 3 up to scene LOT6_Y0.
    int shoulder_same = 1;
    int first = cur >= 3 ? 0 : 6, last = cur >= 4 ? 3 : cur >= 3 ? 4 : 10;
    for(i = first; i < last; i++)
    {
        expected_sc(ys[i], ys[i], ys[i], 1.f, ref, peak, 0, cur >= 3 ? prevcur : 1, shoulder, lot1);
        float d = fabsf(prop[i][0] - lot1[0]) + fabsf(prop[i][1] - lot1[1]) + fabsf(prop[i][2] - lot1[2]);
        if(d > 0.002f) shoulder_same = 0;
    }
    if(cur >= 4) logoutf("hdrout check curve 4: same as the lot5 curve up to scene 0.05: %d", shoulder_same);
    else if(cur >= 3) logoutf("hdrout check curve 3: same as the lot4 curve up to scene 0.10: %d", shoulder_same);
    float n1 = luma3(prop[6][0], prop[6][1], prop[6][2]) * 80.f;
    float n2 = luma3(prop[7][0], prop[7][1], prop[7][2]) * 80.f;
    float n4 = luma3(prop[8][0], prop[8][1], prop[8][2]) * 80.f;
    float n8 = luma3(prop[9][0], prop[9][1], prop[9][2]) * 80.f;
    int hdr = (n2 > n1 + 2.f && n4 > n2 + 2.f && n8 > n4 + 2.f && n8 < peak) ? 1 : 0;
    logoutf("hdrout check finite %d neutral %d monotonic %d hdr_above_white %d same_shoulder %d scene1 %.1f scene2 %.1f scene4 %.1f scene8 %.1f peak %.1f",
            finite, gray, mono, hdr, shoulder_same, n1, n2, n4, n8, peak);
}

static void analyse_scene(GLuint tex, bool yflip, float expmul, float ref, float peak, int preview, int corrected, float shoulder)
{
    GLint tw = 0, th = 0, fmt = 0;
    glActiveTexture_(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &fmt);
    const char *map = game::getclientmap();
    logoutf("hdrout check scene map=%s gl_format 0x%x size %d %d yflip %d exp %.6f ref %.1f peak %.1f preview %d corrected %d shoulder %.4f",
            map ? map : "", int(fmt), int(tw), int(th), yflip ? 1 : 0, expmul, ref, peak, preview, corrected, shoulder);
    logoutf("hdrout check exposure once: mul = 2^EV = %.6f, applied in this pass, not in the texture. Windows SDR white %.1f nits not used.",
            expmul, g_sdr_nits);
    if(tw < 8 || th < 8 || tw > 4096 || th > 4096)
    {
        logoutf("hdrout check scene read refused");
        hflush();
        return;
    }
    float *buf = new float[size_t(tw) * size_t(th) * 4];
    while(glGetError() != GL_NO_ERROR);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, buf);
    int gerr = int(glGetError());
    if(gerr)
    {
        logoutf("hdrout check scene glGetTexImage err 0x%x", gerr);
        delete[] buf;
        hflush();
        return;
    }
    const int bins = 2048;
    const float cap = 32.f;
    int hist[2048];
    memset(hist, 0, sizeof(hist));
    double sum = 0;
    int n = 0, n1 = 0, n2 = 0, n4 = 0, nbad = 0;
    float miny = 1e9f, maxy = -1.f, maxr = 0, maxg = 0, maxb = 0;
    int maxx = 0, maxyi = 0;
    float skyScore = -1.f, vegScore = -1.f, stoneDist = 1e9f;
    int skyx = -1, skyy = -1, vegx = -1, vegy = -1, stonex = -1, stoney = -1;
    float skyr = 0, skyg = 0, skyb = 0, vegr = 0, vegg = 0, vegb = 0, stoner = 0, stoneg = 0, stoneb = 0;
    double hiR = 0, hiG = 0, hiB = 0, waR = 0, waG = 0, waB = 0;
    int hiN = 0, waN = 0;
    int y, x;
    for(y = 0; y < th; y += 2)
    for(x = 0; x < tw; x += 2)
    {
        const float *p = &buf[(size_t(y) * size_t(tw) + x) * 4];
        if(p[0] != p[0] || p[1] != p[1] || p[2] != p[2]) { nbad++; continue; }
        float Y = luma3(p[0], p[1], p[2]);
        if(Y < 0.f) Y = 0.f;
        if(Y < miny) miny = Y;
        if(Y > maxy) { maxy = Y; maxx = x; maxyi = y; maxr = p[0]; maxg = p[1]; maxb = p[2]; }
        sum += Y;
        n++;
        if(Y > 1.f) n1++;
        if(Y > 2.f) n2++;
        if(Y > 4.f) n4++;
        int b = int(Y / cap * float(bins));
        if(b < 0) b = 0;
        if(b >= bins) b = bins - 1;
        hist[b]++;
        float ty = th > 1 ? float(y) / float(th - 1) : 0.f;
        float sy = yflip ? 1.f - ty : ty;
        float sx = tw > 1 ? float(x) / float(tw - 1) : 0.f;
        float mxC = p[0];
        if(p[1] > mxC) mxC = p[1];
        if(p[2] > mxC) mxC = p[2];
        float mnC = p[0];
        if(p[1] < mnC) mnC = p[1];
        if(p[2] < mnC) mnC = p[2];
        float ch = mxC > 1e-6f ? (mxC - mnC) / mxC : 0.f;
        if(sy >= 0.78f)
        {
            hiR += p[0]; hiG += p[1]; hiB += p[2]; hiN++;
        }
        if(sy >= 0.34f && sy <= 0.62f && ch < 0.18f && Y >= 0.008f && Y <= 1.2f)
        {
            waR += p[0]; waG += p[1]; waB += p[2]; waN++;
        }
        if(sy > 0.72f && p[2] > p[0] && p[2] > 0.15f && p[2] > skyScore)
        {
            skyScore = p[2]; skyx = x; skyy = y; skyr = p[0]; skyg = p[1]; skyb = p[2];
        }
        float hi = p[0] > p[2] ? p[0] : p[2];
        float veg = p[1] - hi;
        if(veg > 0.04f && veg > vegScore && Y > 0.02f && Y < 2.5f)
        {
            vegScore = veg; vegx = x; vegy = y; vegr = p[0]; vegg = p[1]; vegb = p[2];
        }
        float chroma = p[0];
        if(p[1] > chroma) chroma = p[1];
        if(p[2] > chroma) chroma = p[2];
        float lo = p[0];
        if(p[1] < lo) lo = p[1];
        if(p[2] < lo) lo = p[2];
        float cx = sx - 0.5f, cy = sy - 0.45f, dist = cx * cx + cy * cy;
        if((chroma - lo) < 0.06f && Y > 0.04f && Y < 1.2f && dist < stoneDist)
        {
            stoneDist = dist; stonex = x; stoney = y; stoner = p[0]; stoneg = p[1]; stoneb = p[2];
        }
    }
    logoutf("hdrout check histogram step=2 n=%d min %.5f p01 %.4f p10 %.4f p50 %.4f p90 %.4f p99 %.4f max %.4f mean %.4f share_gt1 %.4f share_gt2 %.4f share_gt4 %.4f nan %d",
            n, n ? miny : -1.f, hist_pct(hist, bins, n, cap, 0.01f), hist_pct(hist, bins, n, cap, 0.10f),
            hist_pct(hist, bins, n, cap, 0.50f), hist_pct(hist, bins, n, cap, 0.90f), hist_pct(hist, bins, n, cap, 0.99f),
            n ? maxy : -1.f, n ? float(sum / n) : -1.f,
            n ? float(n1) / float(n) : 0.f, n ? float(n2) / float(n) : 0.f, n ? float(n4) / float(n) : 0.f, nbad);
    logoutf("hdrout check brightest tex %d %d rgb %.4f %.4f %.4f Y %.4f (before exposure)", maxx, maxyi, maxr, maxg, maxb, maxy);
    if(skyx >= 0) logoutf("hdrout check zone sky rule=top_blue tex %d %d rgb %.4f %.4f %.4f", skyx, skyy, skyr, skyg, skyb);
    else logoutf("hdrout check zone sky not found by the rule");
    if(vegx >= 0) logoutf("hdrout check zone vegetation rule=green_excess tex %d %d rgb %.4f %.4f %.4f", vegx, vegy, vegr, vegg, vegb);
    else logoutf("hdrout check zone vegetation not found by the rule");
    if(stonex >= 0) logoutf("hdrout check zone stone rule=low_saturation_centre tex %d %d rgb %.4f %.4f %.4f", stonex, stoney, stoner, stoneg, stoneb);
    else logoutf("hdrout check zone stone not found by the rule");
    if(hiN > 0)
    {
        logoutf("hdrout check zone top_band n %d rule=sy>=0.78", hiN);
        log_signal("bande_haute", float(hiR / hiN), float(hiG / hiN), float(hiB / hiN), expmul, ref, peak, shoulder);
    }
    else logoutf("hdrout check zone top_band not found");
    if(waN > 0)
    {
        logoutf("hdrout check zone wall_band n %d rule=middle_low_saturation", waN);
        log_signal("bande_murs", float(waR / waN), float(waG / waN), float(waB / waN), expmul, ref, peak, shoulder);
    }
    else logoutf("hdrout check zone wall_band not found");

    static const char *rname[8] = {
        "haut_centre", "haut_gauche", "milieu_centre", "milieu_gauche", "milieu_droite", "bas_centre", "arme", "bas_gauche"
    };
    static const float rsx[8] = { 0.50f, 0.20f, 0.50f, 0.22f, 0.78f, 0.50f, 0.62f, 0.18f };
    static const float rsy[8] = { 0.86f, 0.80f, 0.48f, 0.40f, 0.40f, 0.18f, 0.14f, 0.20f };
    int i;
    for(i = 0; i < 8; i++)
    {
        float ty = yflip ? 1.f - rsy[i] : rsy[i];
        int tx = clamp(int(rsx[i] * float(tw - 1) + 0.5f), 0, tw - 1);
        int trow = clamp(int(ty * float(th - 1) + 0.5f), 0, th - 1);
        const float *p = &buf[(size_t(trow) * size_t(tw) + tx) * 4];
        float Y = luma3(p[0], p[1], p[2]);
        logoutf("hdrout check region %s before tex %d %d rgb %.5f %.5f %.5f Y %.5f", rname[i], tx, trow, p[0], p[1], p[2], Y);
        log_signal(rname[i], p[0], p[1], p[2], expmul, ref, peak, shoulder);
    }
    delete[] buf;
    logoutf("hdrout check zones: sky/vegetation/stone are pixel rules, not a reliable identification of the scenery.");
    hflush();
}

static void read_curve_gl(float expmul, float ref, float peak, int preview, int corrected, float shoulder)
{
    int cy = patch_cy();
    int pass = 0, i;
    g_patch_mode = preview ? 1 : corrected;
    logoutf("hdrout check GL patches after the transform. scRGB, 1 = 80 nits. Not a panel measurement. preview %d curve %d", preview, corrected);
    for(i = 0; i < PATCH_N; i++)
    {
        int cx = patch_cx(i);
        float got[4];
        read_fbo(cx, cy, got);
        expected_sc(PATCH_RGB[i][0], PATCH_RGB[i][1], PATCH_RGB[i][2], expmul, ref, peak, preview, corrected, shoulder, g_patch_expect[i]);
        g_patch_gl[i][0] = got[0];
        g_patch_gl[i][1] = got[1];
        g_patch_gl[i][2] = got[2];
        float d0 = fabsf(got[0] - g_patch_expect[i][0]);
        float d1 = fabsf(got[1] - g_patch_expect[i][1]);
        float d2 = fabsf(got[2] - g_patch_expect[i][2]);
        bool ok = d0 <= 0.02f && d1 <= 0.02f && d2 <= 0.02f;
        if(ok) pass++;
        logoutf("hdrout check patch %s x %d y %d scene %.3f %.3f %.3f expect_sc %.5f %.5f %.5f got_sc %.5f %.5f %.5f nits_got %.2f %.2f %.2f %s",
                PATCH_NAME[i], cx, cy, PATCH_RGB[i][0], PATCH_RGB[i][1], PATCH_RGB[i][2],
                g_patch_expect[i][0], g_patch_expect[i][1], g_patch_expect[i][2],
                got[0], got[1], got[2], got[0] * 80.f, got[1] * 80.f, got[2] * 80.f, ok ? "PASS" : "FAIL");
    }
    logoutf("hdrout check GL patches result %d/%d", pass, PATCH_N);
    if(PATCH_N >= 8)
    {
        float a = g_patch_gl[5][0], b = g_patch_gl[6][0], c = g_patch_gl[7][0];
        logoutf("hdrout check highlights hdr_2 %.5f hdr_4 %.5f hdr_8 %.5f distinct %d",
                a, b, c, (b > a + 0.05f && c > b + 0.05f) ? 1 : 0);
    }
    hflush();
}

static void read_regions_after()
{
    static const char *rname[8] = {
        "haut_centre", "haut_gauche", "milieu_centre", "milieu_gauche", "milieu_droite", "bas_centre", "arme", "bas_gauche"
    };
    static const float rsx[8] = { 0.50f, 0.20f, 0.50f, 0.22f, 0.78f, 0.50f, 0.62f, 0.18f };
    static const float rsy[8] = { 0.86f, 0.80f, 0.48f, 0.40f, 0.40f, 0.18f, 0.14f, 0.20f };
    int i;
    for(i = 0; i < 8; i++)
    {
        int px = clamp(int(rsx[i] * float(g_w - 1) + 0.5f), 0, g_w - 1);
        int py = clamp(int(rsy[i] * float(g_h - 1) + 0.5f), 0, g_h - 1);
        if(in_mire(px, py))
        {
            logoutf("hdrout check region %s after hidden by the patches %d %d", rname[i], px, py);
            continue;
        }
        float got[4];
        read_fbo(px, py, got);
        logoutf("hdrout check region %s after screen %d %d sc %.5f %.5f %.5f nits %.2f %.2f %.2f",
                rname[i], px, py, got[0], got[1], got[2], got[0] * 80.f, got[1] * 80.f, got[2] * 80.f);
    }
    hflush();
}

// Lab only. The next frame is written under <profile>/captures: in HDR, the linear
// scene before exposure/curve and the scRGB output before the HUD (PFM float,
// rows bottom to top); in base SDR, the 8-bit window before the swap (PPM).
// Values before the panel, not a measurement of the screen.
static int g_capture = 0;
static string g_capname;

static void capture_log(const char *kind, const char *rel, int w, int h)
{
    float cx = camera1 ? camera1->o.x : 0.f;
    float cy = camera1 ? camera1->o.y : 0.f;
    float cz = camera1 ? camera1->o.z : 0.f;
    float yaw = camera1 ? camera1->yaw : 0.f;
    float pitch = camera1 ? camera1->pitch : 0.f;
    int incomplete = 0;
    logoutf("hdrout capture written type=%s file=%s size %d %d mode=%s variant=%s curve=%d light_lot4=%d flames_lot5=%d sky_lot6_active=%d light_fix_active=%d rt_lighting=%d hwrt=%d ngx=%d exp=%.3f ref=%d max=%.1f dbg=%d cam %.2f %.2f %.2f yaw %.2f pitch %.2f",
            kind, rel, w, h, mode_name(), curve_name(), hdroutcurve, hdroutlumiere, hdroutflamme, hdrout_skyfix_level(),
            (hwrthdr && hdrout_lightfix_wanted()) ? 1 : 0, rt_lighting() ? 1 : 0, hwrt, hwrtngxmodeapplied(),
            hwrthdrexp, hdroutref, used_max_nits(&incomplete), hdrlightdbg, cx, cy, cz, yaw, pitch);
}

static bool write_pfm(const char *rel, const float *rgb, int w, int h)
{
    FILE *f = fopen(findfile(path(rel, true), "w"), "wb");
    if(!f) return false;
    fprintf(f, "PF\n%d %d\n-1.0\n", w, h);
    fwrite(rgb, sizeof(float), size_t(w) * size_t(h) * 3, f);
    fclose(f);
    return true;
}

static void capture_hdr(GLuint tex, bool yflip)
{
    g_capture = 0;
    GLint tw = 0, th = 0;
    glActiveTexture_(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    if(tw >= 8 && th >= 8 && tw <= 8192 && th <= 8192)
    {
        size_t row = size_t(tw) * 3;
        float *buf = new float[row * size_t(th)];
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_FLOAT, buf);
        if(yflip)
        {
            float *tmp = new float[row];
            for(int y = 0; y < th / 2; y++)
            {
                float *a = &buf[size_t(y) * row], *b = &buf[size_t(th - 1 - y) * row];
                memcpy(tmp, a, row * sizeof(float));
                memcpy(a, b, row * sizeof(float));
                memcpy(b, tmp, row * sizeof(float));
            }
            delete[] tmp;
        }
        defformatstring(rel, "captures/%s-scene.pfm", g_capname);
        if(write_pfm(rel, buf, tw, th)) capture_log("scene_lineaire_avant_courbe", rel, tw, th);
        delete[] buf;
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    if(g_w >= 8 && g_h >= 8)
    {
        float *buf = new float[size_t(g_w) * size_t(g_h) * 3];
        glReadPixels(0, 0, g_w, g_h, GL_RGB, GL_FLOAT, buf);
        defformatstring(rel, "captures/%s-sortie.pfm", g_capname);
        if(write_pfm(rel, buf, g_w, g_h)) capture_log("sortie_scrgb_avant_hud", rel, g_w, g_h);
        delete[] buf;
    }
    // Lot 5 flame target of this frame: the encoded sum of the flames alone.
    int fw = 0, fh = 0;
    GLuint ftex = hwrtflametex(fw, fh);
    if(ftex && fw >= 8 && fh >= 8 && fw <= 8192 && fh <= 8192)
    {
        float *buf = new float[size_t(fw) * size_t(fh) * 3];
        glBindTexture(GL_TEXTURE_2D, ftex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_FLOAT, buf);
        glBindTexture(GL_TEXTURE_2D, 0);
        defformatstring(rel, "captures/%s-flammes.pfm", g_capname);
        if(write_pfm(rel, buf, fw, fh)) capture_log("flammes_somme_encodee", rel, fw, fh);
        delete[] buf;
    }
    hflush();
}

static void capture_sdr()
{
    g_capture = 0;
    int w = screenw, h = screenh;
    if(w < 8 || h < 8) return;
    uchar *buf = new uchar[size_t(w) * size_t(h) * 3];
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, buf);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    defformatstring(rel, "captures/%s-sdr.ppm", g_capname);
    FILE *f = fopen(findfile(path(rel, true), "w"), "wb");
    if(f)
    {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for(int y = h - 1; y >= 0; y--) fwrite(&buf[size_t(y) * size_t(w) * 3], 1, size_t(w) * 3, f);
        fclose(f);
        capture_log("sdr_fenetre_8bits_avec_hud", rel, w, h);
    }
    delete[] buf;
    hflush();
}

static void hdrout_capture(const char *name)
{
    copystring(g_capname, name && name[0] ? name : "capture");
    for(char *s = g_capname; *s; s++) if(*s == '/' || *s == '\\' || *s == ':' || iscubespace(*s)) *s = '_';
    g_capture = 1;
    logoutf("hdrout capture requested %s", g_capname);
    hflush();
}
COMMANDN(hdroutcapture, hdrout_capture, "s");

bool hdrout_compose(GLuint tex, bool yflip)
{
    int want = g_mesure ? 1 : 0;
    if(want) g_mesure = 0;
    if(!tex || !ensure_size())
    {
        if(want) logoutf("hdrout check measurement dropped: no target");
        return false;
    }
    int incomplete = 0;
    float mx = used_max_nits(&incomplete);
    float expmul = powf(2.f, clamp(hwrthdrexp, -8.f, 8.f));
    float ref = float(hdroutref);
    int preview = hdrout ? 0 : 1;
    int corrected = preview ? 0 : hdroutcurve;
    float shoulder = shoulder_k(ref, mx);
    if(want)
    {
        float cx = camera1 ? camera1->o.x : 0.f;
        float cy = camera1 ? camera1->o.y : 0.f;
        float cz = camera1 ? camera1->o.z : 0.f;
        float yaw = camera1 ? camera1->yaw : 0.f;
        float pitch = camera1 ? camera1->pitch : 0.f;
        logoutf("hdrout check measurement start ngx %d rt %d nrd %d cam %.2f %.2f %.2f yaw %.2f pitch %.2f",
                hwrtngxmodeapplied(), getvar("hwrt"), getvar("hwrtnrd"), cx, cy, cz, yaw, pitch);
        analyse_scene(tex, yflip, expmul, ref, mx, preview, corrected, shoulder);
    }
    float la5, sa5, span5, b6, span6;
    lot5_params(ref, mx, la5, sa5, span5);
    lot6_params(ref, mx, b6, span6);
    perf_gl_stamp(1);
    plain_draw();
    g_map->set();
    glActiveTexture_(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    LOCALPARAMF(hdroutp, expmul, ref, mx, preview ? 1.f : 0.f);
    LOCALPARAMF(hdroutq, yflip ? 1.f : 0.f, want ? 1.f : 0.f, preview ? 0.f : float(hdroutcurve), shoulder);
    LOCALPARAMF(hdroutr, la5, sa5, span5, logf(LOT6_GAIN));
    LOCALPARAMF(hdrouts, b6, span6, LOT6_Y0, 1.f / logf(LOT6_Y1 / LOT6_Y0));
    screenquad(1, 1);
    glBindTexture(GL_TEXTURE_2D, 0);
    perf_gl_stamp(2);
    if(g_capture && !want) capture_hdr(tex, yflip);
    if(want)
    {
        read_curve_gl(expmul, ref, mx, preview, corrected, shoulder);
        read_regions_after();
        if(!preview) log_curve_checks(ref, mx, shoulder);
        g_present_patches = 1;
    }
    static int chain = 0;
    if(!chain)
    {
        chain = 1;
        logoutf("hdrout chain: linear scene, not clipped, exposure once, then the variant. Windows SDR white read, not applied.");
        logoutf("hdrout origin: scene * reference white, luminance capped at the peak, divided by 80. Nothing added to black.");
        logoutf("hdrout corrected: Narkowicz on luminance up to scene 1 (scene 1 = %.3f * white), then a shoulder to the peak. Hue kept, no per-channel clipping.", nark_f(1.f));
        logoutf("hdrout proposal: same value at scene 1 white and same shoulder. Below it, the curve input is opened (factor 1.80) then caught up between 0.40 and 0.92. No change of exposure, white, peak or HUD, and no saturation.");
        logoutf("hdrout lot4: lot3 curve 2 unchanged. With classic lighting, the light colour (lightmap, dynamic lights, models) takes the chromaticity of pow(light, 2.2) at equal luminance. Neutral light and textures unchanged. Lot3 = same chain without this fix.");
        logoutf("hdrout lot5 curve 3: curve 2 up to scene %.2f. Above, local contrast (output stops per scene stop) goes from that of curve 2 to %.2f instead of falling towards 0.2 between scene 0.5 and 1, then a soft limit at the peak. Continuous, continuous slope, monotonic.", LOT5_YA, LOT5_SM);
        logoutf("hdrout lot5 light (hdroutlumiere 2): classic lighting, lot4 colour, and luminance of overexposed light (albedo multiplier 2 x light): above 1.25, pow(L, 2.2) as on the historical screen; below 0.8 unchanged; C1 ramp in between. Not with RT.");
        logoutf("hdrout lot5 flames: additive flames summed apart, encoded as on the 8-bit screen, then composited once: hue of the clipped sum (SDR picture of the flame), luminance above white from the density. Classic and RT, internal HDR only.");
        logoutf("hdrout lot6 curve 4: curve 3 up to scene %.2f (lot5 shadows and dark tones). Above, a log-smooth brightness gain up to %.2f at scene %.2f, then a soft limit at the peak. No exposure change: blacks and shadows do not move.", LOT6_Y0, LOT6_GAIN, LOT6_Y1);
        logoutf("hdrout lot6 sky (hdroutciel): 1 = sky, cloud and fog dome hues decoded (8-bit display colours); 2 = also, the saturated blue sky (shadow luminance) takes the lot6 brightness gain; clouds and neutral tops follow the curve; dark sky pixels unchanged. Native HDR only: base SDR and preview stay as in lot5.");
        {
            static const float ys[9] = { 0.03f, 0.10f, 0.25f, 0.50f, 1.f, 2.f, 4.f, 8.f, 16.f };
            string line = "";
            for(int i = 0; i < 9; i++)
            {
                float a[3], b[3];
                expected_sc(ys[i], ys[i], ys[i], 1.f, ref, mx, 0, 3, shoulder, a);
                expected_sc(ys[i], ys[i], ys[i], 1.f, ref, mx, 0, 4, shoulder, b);
                defformatstring(part, " %.2f:%.1f/%.1f", ys[i], a[1] * 80.f, b[1] * 80.f);
                concatstring(line, part);
            }
            logoutf("hdrout nits scene:lot5/lot6 at ref %.0f peak %.0f%s", ref, mx, line);
        }
        logoutf("hdrout preview: per-channel Narkowicz, placed at reference white, no sRGB OETF. Highlights are flattened there.");
        hflush();
    }
    g_frame_kind = hdrout ? 2 : 1;
    return true;
}

void hdrout_begin_overlay()
{
    if(!g_locked) hdrout_resolve();
    bool was = g_locked;
    if(!ensure_size()) return;
    glBindFramebuffer_(GL_FRAMEBUFFER, g_fbo);
    glViewport(0, 0, g_w, g_h);
    if(!was)
    {
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glClearColor(0, 0, 0, 0);
    }
}

GLuint hdrout_drawfb()
{
    return (g_locked && g_fbo) ? g_fbo : 0;
}

bool hdrout_fp16post()
{
    return g_locked && g_ready && hwrthdr;
}

bool hdrout_present()
{
    if(hdroutread && !g_locked)
    {
        hdroutread = 0;
        logoutf("hdrout read refused: 8-bit SDL presentation, no FP16 target presented. reason=%s", g_reason);
        hflush();
    }
    if(!g_locked && (g_mesure || g_present_patches))
    {
        g_mesure = 0;
        g_present_patches = 0;
        logoutf("hdrout check measurement refused: no HDR transform on this frame");
        hflush();
    }
    if(!g_locked)
    {
        sync_drop_fences();
        hide_child();
        glBindFramebuffer_(GL_FRAMEBUFFER, 0);
        if(screenw >= 1 && screenh >= 1) glViewport(0, 0, screenw, screenh);
        if(g_capture) capture_sdr();
        take_sonde("sdl");
        note_effective("sdl");
        frameend();
        g_lastkind = 0;
        return false;
    }
    glBindFramebuffer_(GL_FRAMEBUFFER, g_fbo);
    glViewport(0, 0, g_w, g_h);
    if((hdroutmire || hdroutread) && g_mire && g_mire->program)
    {
        plain_draw();
        g_mire->set();
        screenquad(1, 1);
    }
    place_child(false);
    // A locked surface is the HDR presentation, including the loading overlay
    // which does not pass through the scene compose. It is not an error fallback.
    if(g_frame_kind < 0) g_frame_kind = hdrout ? 2 : 1;
    take_sonde("surface");
    fresh_mark();
    // This frame was drawn into the scRGB target. If unlock or Present fails,
    // do not also call SDL_GL_SwapWindow: the 8-bit window does not hold a
    // composed SDR image, and it must not receive the scRGB values. The next
    // frame composes that SDR image and presents it once.
    bool opened = unlock_share();
    bool shown = false;
    if(opened) shown = present_dx();
    if(shown)
    {
        // The child is only made visible once it holds a presented picture.
        show_child();
        sync_limit();
    }
    else sync_drop_fences();
    if(!shown)
    {
        g_frame_kind = -1;
        g_lastkind = 0;
        note_effective("fallback");
    }
    else
    {
        g_lastkind = hdrout ? 2 : 1;
        note_effective("surface");
    }
    gle::disable();
    frameend();
    return true;
}

static void hdrout_status()
{
    log_state("command");
}

static void hdrout_sonde()
{
    g_sonde = 1;
}

static void hdrout_labwin(int *mode)
{
    if(!g_parent)
    {
        logoutf("hdrout lab no window");
        return;
    }
    if(mode && *mode)
    {
        ShowWindow(g_parent, SW_MINIMIZE);
        logoutf("hdrout lab minimised");
    }
    else
    {
        ShowWindow(g_parent, SW_RESTORE);
        logoutf("hdrout lab restored");
    }
    hflush();
}

static void format_nits(char *dst, int n, float v)
{
    int r = (int)floorf(v + 0.5f);
    if(fabsf(v - float(r)) < 0.05f) nformatstring(dst, n, "%d", r);
    else nformatstring(dst, n, "%.1f", v);
}

static void hdrout_peak_label()
{
    static char buf[140];
    int incomplete = 0;
    float mx = used_max_nits(&incomplete);
    char num[32];
    format_nits(num, sizeof(num), mx);
    if(hdroutmax > 0)
        nformatstring(buf, sizeof(buf), "Peak: manual, %s nits", num);
    else if(incomplete)
        nformatstring(buf, sizeof(buf), "Peak: automatic, %s nits (display report incomplete)", num);
    else
        nformatstring(buf, sizeof(buf), "Peak: automatic, %s nits (reported by the display)", num);
    result(buf);
}

static void hdrout_hud_label()
{
    static char buf[140];
    char num[32];
    format_nits(num, sizeof(num), hud_nits());
    if(hdrouthud > 0)
        nformatstring(buf, sizeof(buf), "HUD: %s nits", num);
    else
        nformatstring(buf, sizeof(buf), "HUD: follows reference white, %s nits", num);
    result(buf);
}

static void hdrout_mesures()
{
    g_mesure = 1;
    logoutf("hdrout check measurement requested curve=%s exp=%.3f ref=%d max=%d hud=%d",
            curve_name(), hwrthdrexp, hdroutref, hdroutmax, hdrouthud);
    hflush();
}

// Whole chains: 7 = lot 7 proposal, 6 = lot 6 reference, 5 = lot 5, 61..64 = lot 6
// diagnostics on the lot 5 chain, 4 = lot 4, 51..54 = lot 5 diagnostics on the
// lot 4 chain, 3 = lot 3, 1 = restitution lot 1 correction, 0 = first native output.
static void set_chain(int n)
{
    int oldlum = hdroutlumiere, oldflame = hdroutflamme, oldsky = hdroutciel, oldcol = hdroutcouleur;
    int oldday = hdroutjour, olddose = hdroutjourcouleur;
    hdroutciel = 0;
    hdroutcouleur = 0;
    hdroutjour = 0;
    hdroutjourcouleur = HDRJOUR_COULEUR;
    switch(n)
    {
        case 8: hdroutcurve = 4; hdroutlumiere = 2; hdroutflamme = 1; hdroutciel = 2; hdroutcouleur = 1; hdroutjour = 1; break;
        case 82: hdroutcurve = 4; hdroutlumiere = 2; hdroutflamme = 1; hdroutciel = 2; hdroutjour = 1; break;
        case 7: hdroutcurve = 4; hdroutlumiere = 2; hdroutflamme = 1; hdroutciel = 2; hdroutcouleur = 1; break;
        case 6: hdroutcurve = 4; hdroutlumiere = 2; hdroutflamme = 1; hdroutciel = 2; break;
        case 61: hdroutcurve = 4; hdroutlumiere = 2; hdroutflamme = 1; break;
        case 62: hdroutcurve = 3; hdroutlumiere = 2; hdroutflamme = 1; hdroutciel = 2; break;
        case 63: hdroutcurve = 3; hdroutlumiere = 2; hdroutflamme = 1; hdroutciel = 1; break;
        case 64: hdroutcurve = 4; hdroutlumiere = 2; hdroutflamme = 1; hdroutciel = 1; break;
        case 5: hdroutcurve = 3; hdroutlumiere = 2; hdroutflamme = 1; break;
        case 51: hdroutcurve = 2; hdroutlumiere = 1; hdroutflamme = 1; break;
        case 52: hdroutcurve = 3; hdroutlumiere = 2; hdroutflamme = 0; break;
        case 53: hdroutcurve = 3; hdroutlumiere = 1; hdroutflamme = 0; break;
        case 54: hdroutcurve = 2; hdroutlumiere = 2; hdroutflamme = 0; break;
        case 4: hdroutcurve = 2; hdroutlumiere = 1; hdroutflamme = 0; break;
        case 3: hdroutcurve = 2; hdroutlumiere = 0; hdroutflamme = 0; break;
        case 1: hdroutcurve = 1; hdroutlumiere = 0; hdroutflamme = 0; break;
        default: hdroutcurve = 0; hdroutlumiere = 0; hdroutflamme = 0; break;
    }
    if(oldlum != hdroutlumiere || oldflame != hdroutflamme || oldsky != hdroutciel || oldcol != hdroutcouleur ||
       oldday != hdroutjour || (hdroutjour && olddose != hdroutjourcouleur)) hdrout_scene_changed("hdrout chaine");
}

static void hdrout_variante(int *v)
{
    int n = v ? *v : 7;
    static const int known[] = { 8, 82, 7, 6, 61, 62, 63, 64, 5, 51, 52, 53, 54, 4, 3, 1 };
    bool ok = false;
    for(int i = 0; i < int(sizeof(known) / sizeof(known[0])); i++) if(n == known[i]) ok = true;
    if(!ok) n = n > 8 ? 8 : 0;
    set_chain(n);
    logoutf("hdrout variant console %s curve %d light_lot4 %d flames_lot5 %d sky_lot6 %d colour_lot7 %d day_lot8 %d colour_dose %d", curve_name(), hdroutcurve, hdroutlumiere, hdroutflamme, hdroutciel, hdroutcouleur, hdroutjour, hdroutjourcouleur);
    log_state("variant");
}

static void hdrout_variant_cmd()
{
    result(native_label());
}

ICOMMAND(hdroutmesures, "", (), hdrout_mesures());
ICOMMAND(hdroutvariant, "", (), hdrout_variant_cmd());
ICOMMAND(hdroutvariante, "i", (int *v), hdrout_variante(v));
ICOMMAND(hdroutapercu, "", (), result(apercu_label()));
ICOMMAND(hdrouteclairage, "", (), result(lighting_label()));
ICOMMAND(hdroutlot, "", (), intret(variant_lot()));
ICOMMAND(hdroutsonde, "", (), hdrout_sonde());
ICOMMAND(hdroutstatus, "", (), hdrout_status());
ICOMMAND(hdroutmode, "", (), result(hdrout_label_text()));
ICOMMAND(hdroutpeaklabel, "", (), hdrout_peak_label());
ICOMMAND(hdrouthudlabel, "", (), hdrout_hud_label());
ICOMMAND(hdroutlabwin, "i", (int *mode), hdrout_labwin(mode));

// Model previews in the menus (player, map models). Their shaders are not the
// HUD shaders converted by hdrout_applyshader: on the scRGB target their
// display-encoded, sometimes overbright output would reach the screen as
// scene values above white. Drawn into an RGBA8 target (clipped as on an SDR
// screen), then composed like the HUD: sRGB decoded, times the HUD nits.
static GLuint g_pvfbo = 0, g_pvcolor = 0, g_pvdepth = 0;
static int g_pvw = 0, g_pvh = 0, g_pvx = 0, g_pvy = 0;
static bool g_pvon = false, g_pvbg = false;
static Shader *g_pvshader = NULL;

static void preview_release()
{
    if(g_pvfbo) { glDeleteFramebuffers_(1, &g_pvfbo); g_pvfbo = 0; }
    if(g_pvcolor) { glDeleteTextures(1, &g_pvcolor); g_pvcolor = 0; }
    if(g_pvdepth) { glDeleteTextures(1, &g_pvdepth); g_pvdepth = 0; }
    g_pvw = g_pvh = 0;
}

static bool preview_shader()
{
    static const char *vs =
        "attribute vec4 vvertex;\n"
        "uniform vec4 screentexcoord0;\n"
        "varying vec2 texcoord0;\n"
        "void main(void) {\n"
        "    gl_Position = vvertex;\n"
        "    texcoord0 = vvertex.xy * screentexcoord0.xy + screentexcoord0.zw;\n"
        "}\n";
    static const char *ps =
        "uniform sampler2D tex0;\n"
        "uniform sampler2D tex1;\n"
        "uniform vec4 hdrpv;\n"
        "varying vec2 texcoord0;\n"
        "void main(void) {\n"
        "    vec3 e = clamp(texture2D(tex0, texcoord0).rgb, 0.0, 1.0);\n"
        "    float d = texture2D(tex1, texcoord0).r;\n"
        "    if(d >= 1.0 && hdrpv.y < 0.5) discard;\n"
        "    vec3 lin = mix(e / 12.92, pow((e + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), e));\n"
        "    gl_FragColor = vec4(lin * hdrpv.x, 1.0);\n"
        "}\n";
    if(!g_pvshader || !g_pvshader->loaded() || !g_pvshader->program)
        g_pvshader = newshader(0, "hdroutpreview", vs, ps, NULL, 0);
    return g_pvshader && !g_pvshader->invalid() && g_pvshader->program;
}

// Lab comparison only: 0 = old direct drawing of the model in the scRGB target.
VAR(hdroutpreviewsdr, 0, 1, 1);
bool hdrout_preview_begin(int x, int y, int w, int h, bool background)
{
    g_pvon = false;
    if(!hdroutpreviewsdr) return false;
    if(!g_locked || !g_ui || !g_fbo || w < 1 || h < 1 || w > 8192 || h > 8192) return false;
    if(!preview_shader()) return false;
    if(!g_pvfbo || g_pvw != w || g_pvh != h)
    {
        preview_release();
        glGenTextures(1, &g_pvcolor);
        glBindTexture(GL_TEXTURE_2D, g_pvcolor);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glGenTextures(1, &g_pvdepth);
        glBindTexture(GL_TEXTURE_2D, g_pvdepth);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, w, h, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
        glBindTexture(GL_TEXTURE_2D, 0);
        glGenFramebuffers_(1, &g_pvfbo);
        glBindFramebuffer_(GL_FRAMEBUFFER, g_pvfbo);
        glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_pvcolor, 0);
        glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, g_pvdepth, 0);
        GLenum st = glCheckFramebufferStatus_(GL_FRAMEBUFFER);
        glBindFramebuffer_(GL_FRAMEBUFFER, g_fbo);
        if(st != GL_FRAMEBUFFER_COMPLETE)
        {
            logoutf("hdrout model preview: 8-bit target incomplete 0x%x, drawing directly", int(st));
            preview_release();
            return false;
        }
        g_pvw = w;
        g_pvh = h;
    }
    g_pvx = x;
    g_pvy = y;
    g_pvbg = background;
    glBindFramebuffer_(GL_FRAMEBUFFER, g_pvfbo);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    if(scissor) glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if(scissor) glEnable(GL_SCISSOR_TEST);
    g_pvon = true;
    return true;
}

void hdrout_preview_end()
{
    if(!g_pvon) return;
    g_pvon = false;
    glBindFramebuffer_(GL_FRAMEBUFFER, g_fbo);
    glViewport(g_pvx, g_pvy, g_pvw, g_pvh);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    GLboolean blend = glIsEnabled(GL_BLEND);
    if(blend) glDisable(GL_BLEND);
    Shader *prev = Shader::lastshader;
    g_pvshader->set();
    LOCALPARAMF(hdrpv, hud_nits() / 80.f, g_pvbg ? 1.f : 0.f, 0.f, 0.f);
    glActiveTexture_(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, g_pvdepth);
    glActiveTexture_(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_pvcolor);
    screenquad(1, 1);
    glActiveTexture_(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture_(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    if(blend) glEnable(GL_BLEND);
    if(prev) prev->set();
    static int logged = 0;
    if(!logged)
    {
        logged = 1;
        logoutf("hdrout model preview: 8-bit target %dx%d composited like the HUD (%.0f nits)", g_pvw, g_pvh, hud_nits());
    }
}

// Daily integration. The saved choice is hdroutpref; hwrthdr/hdrout are the
// effective state, recomputed here every frame, never saved.
// -1 = Automatic: native HDR when Windows HDR is already on for this screen
// and the NVIDIA OpenGL-D3D11 presenter started, SDR otherwise.
// 0 = SDR, kept even when Windows HDR is on. 1 = native HDR when available,
// otherwise SDR with the reason shown in Options > Graphics.
// Windows HDR, VRR, gamma and the driver are only read, never changed.
// Default SDR (was Automatic): a frozen/black screen at startup is suspected
// with Automatic, under investigation. Automatic stays in the menu; saved -1
// values are moved to 0 once by the l4zymigration step in main.cpp.
static void hdrout_pref_changed();
VARFP(hdroutpref, -1, 0, 1, hdrout_pref_changed());
// Diagnostic only, not saved: the internal HDR image tone mapped for an SDR
// look, shown in the HDR surface. Not native HDR, not in the normal menus.
VARF(hdroutapercu, 0, 0, 1, hdrout_pref_changed());

static int g_resolved_hdr = -1, g_resolved_out = -1, g_resolved_rt = -1;
// Loading screens drawn before config.cfg/autoexec.cfg stay SDR: the saved
// choice is not known yet, and an explicit SDR must never flash HDR.
static bool g_prefs_loaded = false;
void hdrout_prefsloaded()
{
    g_prefs_loaded = true;
}

static const char *reason_ui();
static bool presenter_open();
static void presenter_release(const char *why);
static void probe_output();

static bool presenter_allocated()
{
    return g_dev || g_ctx || g_dx || g_factory || g_child || g_swap;
}

// Automatic (-1) asks for native HDR when Windows HDR is on for the game's
// screen (read without any D3D device) and the presenter has not failed.
static int wanted_output()
{
    if(hdroutpref == 0) return 0;
    if(hdroutpref == 1) return 1;
    return (g_ready || (!g_disabled && g_hdr && !g_open_failed)) ? 1 : 0;
}

static void hdrout_resolve()
{
    // A minimised window keeps its state; the presenter is simply not drawn.
    if(minimized && g_resolved_hdr >= 0) return;
    int want = g_prefs_loaded ? wanted_output() : 0;
    if(g_tried && !g_locked)
    {
        // Native HDR wanted: open the presenter now (first time after the
        // config, or a switch from the menu). Not before config.cfg/autoexec.
        if(want && !g_ready && !g_open_failed && !g_disabled) presenter_open();
        // SDR (or a failed presenter): release it, once the child is hidden,
        // i.e. after an SDR frame has been presented over it.
        else if((!want || !g_ready) && !g_child_on && presenter_allocated())
            presenter_release(want ? "failed" : "sdr output");
    }
    int eff = (want && g_ready) ? 1 : 0;
    // Native HDR asked for (saved choice) but not running: say it once in the console.
    static bool warned = false;
    if(g_prefs_loaded && hdroutpref == 1 && !g_ready && !warned)
    {
        warned = true;
        conoutf(CON_WARN, "Native HDR unavailable, using SDR: %s", reason_ui());
    }
    if(g_ready) warned = false;
    int out = hdroutapercu ? 0 : 1;
    int rt = rt_lighting() ? 1 : 0;
    if(eff == g_resolved_hdr && out == g_resolved_out && rt == g_resolved_rt) return;
    bool first = g_resolved_hdr < 0;
    bool scene = !first && (eff != g_resolved_hdr || (eff && (out != g_resolved_out || rt != g_resolved_rt)));
    g_resolved_hdr = eff;
    g_resolved_out = out;
    g_resolved_rt = rt;
    hwrthdr = eff;
    hdrout = out;
    // FP16 <-> RGBA8 scene, native-only sky/colour/day, RT on/off: the
    // DLAA/DLSS history must not blend the previous picture into this one.
    if(scene) hwrttemporalreset("hdrout effective output");
    logoutf("hdrout preference=%d requested=%s effective=%s ready=%d reason=%s", hdroutpref,
            want ? "hdr_natif" : "sdr", eff ? (out ? "hdr_natif" : "apercu_sdr_surface_hdr") : "sdr",
            g_ready ? 1 : 0, g_reason[0] ? g_reason : "ok");
    hflush();
}

static void hdrout_pref_changed()
{
    if(!g_tried || !g_prefs_loaded) return;
    // Explicit HDR request while the presenter is down (Windows HDR turned on
    // after launch, or an earlier presentation error): try once more.
    if(hdroutpref == 1 && !g_ready && !g_locked && screen && !g_disabled)
    {
        presenter_release("retry");
        probe_output();
        g_open_failed = false;
    }
    hdrout_resolve();
}

// Simple reason for the menu. The detailed one stays in the log (hdroutstatus).
static const char *reason_ui()
{
    if(g_ready) return "";
    const char *r = g_reason;
    if(!r[0]) return "Native HDR output not started.";
    if(strstr(r, "Windows HDR off")) return "Windows HDR is off for this screen (Windows: Display settings > Use HDR).";
    if(strstr(r, "simulation")) return "Simulated failure (test): native HDR unavailable.";
    if(strstr(r, "WGL_NV_DX_interop") || strstr(r, "no GPU shared") || strstr(r, "interop"))
        return "Native HDR needs the NVIDIA OpenGL/Direct3D bridge, not available on this GPU/driver.";
    if(strstr(r, "Present") || strstr(r, "lock failed") || strstr(r, "unlock failed") || strstr(r, "presentation") || strstr(r, "presenter"))
        return "Native HDR stopped after a presentation error. Choose Native HDR again to retry.";
    return "Native HDR output could not start on this system.";
}

ICOMMAND(hdroutdispo, "", (), intret(g_ready ? 1 : 0));
ICOMMAND(hdrouteffectif, "", (), intret(g_lastkind < 0 ? (hwrthdr ? (hdrout ? 2 : 1) : 0) : g_lastkind));
ICOMMAND(hdroutdemande, "", (), intret(wanted_output()));
ICOMMAND(hdroutraison, "", (), result(reason_ui()));
// Peak reported by the display, 0 when it reports none.
ICOMMAND(hdroutpeakauto, "", (), intret(g_max_nits >= 80.f ? int(g_max_nits + 0.5f) : 0));
// Fine exposure: adds delta EV and keeps the value on a 0.05 EV grid.
ICOMMAND(hdroutexpstep, "f", (float *d),
{
    float v = hwrthdrexp + (d ? *d : 0.f);
    v = floorf(v * 20.f + 0.5f) / 20.f;
    if(v < -8.f) v = -8.f;
    if(v > 8.f) v = 8.f;
    if(fabsf(v) < 0.001f) v = 0.f;
    setfvar("hwrthdrexp", v);
});

// Reads, without any D3D11 device, interop, window or swap chain, whether
// Windows HDR is on for the screen of the game window, and the luminance range
// it reports. Only a DXGI factory is used, released at once.
static void probe_output()
{
    g_hdr = 0;
    g_cs = -1;
    g_min_nits = g_max_nits = g_full_nits = 0;
    g_bits = 0;
    g_output[0] = 0;
    if(!g_parent) return;
    HMONITOR mon = MonitorFromWindow(g_parent, MONITOR_DEFAULTTONEAREST);
    IDXGIFactory1 *fac1 = NULL;
    HRESULT hr = CreateDXGIFactory1(IID_IDXGIFactory1, (void **)&fac1);
    if(FAILED(hr) || !fac1)
    {
        set_reason("no DXGI factory");
        logoutf("hdrout api CreateDXGIFactory1 hr 0x%08X", (unsigned)hr);
        return;
    }
    // The adapter that drives the game's screen, else the first hardware one.
    IDXGIAdapter1 *pick = NULL, *first = NULL;
    for(UINT ai = 0; !pick; ai++)
    {
        IDXGIAdapter1 *ad = NULL;
        if(fac1->EnumAdapters1(ai, &ad) == DXGI_ERROR_NOT_FOUND || !ad) break;
        DXGI_ADAPTER_DESC1 desc;
        memset(&desc, 0, sizeof(desc));
        ad->GetDesc1(&desc);
        if(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
        {
            ad->Release();
            continue;
        }
        bool owns = false;
        for(UINT oi = 0; mon && !owns; oi++)
        {
            IDXGIOutput *out = NULL;
            if(ad->EnumOutputs(oi, &out) == DXGI_ERROR_NOT_FOUND || !out) break;
            DXGI_OUTPUT_DESC od;
            memset(&od, 0, sizeof(od));
            out->GetDesc(&od);
            out->Release();
            if(od.Monitor == mon) owns = true;
        }
        if(owns) pick = ad;
        else if(!first) first = ad;
        else ad->Release();
    }
    if(!pick) { pick = first; first = NULL; }
    if(first) first->Release();
    if(pick)
    {
        query_output(pick, mon);
        pick->Release();
    }
    else set_reason("DXGI output not found");
    fac1->Release();
    logoutf("hdrout screen probe (no D3D11 device, interop, window or swap chain) %s hdr_dxgi=%d peak=%.0f nits",
            g_output[0] ? g_output : "?", g_hdr, g_max_nits);
}

// Windows HDR on for the game's screen (read at startup). Used to leave the
// gamma table alone: on an HDR screen it can blank or flicker the display.
bool hdrout_desktophdr()
{
    if(g_hdr) return true;
    return g_disabled && g_ac_on == 1;
}
ICOMMAND(hdroutwindowshdr, "", (), intret(hdrout_desktophdr() ? 1 : 0));

// Startup, from gl_init (before config.cfg): only reads the screen state.
// No D3D11 device, GL-D3D interop, child window or swap chain is created
// here; hdrout_resolve() opens them once the saved choice asks for native HDR.
void hdrout_start()
{
    if(g_tried) return;
    g_tried = true;
    g_disabled = false;
    g_open_failed = false;
    g_hdr = 0;
    g_ac_sup = g_ac_on = -1;
    g_adapter[0] = g_output[0] = g_gl[0] = g_luid[0] = g_reason[0] = 0;
    const GLubyte *rend = glGetString(GL_RENDERER);
    copystring(g_gl, rend ? (const char *)rend : "inconnu", sizeof(g_gl));
    if(!screen)
    {
        g_disabled = true;
        set_reason("no SDL window");
        log_state("startup");
        return;
    }
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if(!SDL_GetWindowWMInfo(screen, &info) || info.subsystem != SDL_SYSWM_WINDOWS)
    {
        g_disabled = true;
        set_reason("SDL HWND unavailable");
        log_state("startup");
        return;
    }
    g_parent = info.info.win.window;
    // Diagnostic switch (black screen reports): with hdrout-off.txt in the
    // profile, no D3D device / DXGI object is created at all, only OpenGL.
    // Windows HDR is then read from the display configuration only (gamma).
    if(fileexists(findfile("hdrout-off.txt", "r"), "r"))
    {
        g_disabled = true;
        MONITORINFOEXW mi;
        memset(&mi, 0, sizeof(mi));
        mi.cbSize = sizeof(mi);
        if(GetMonitorInfoW(MonitorFromWindow(g_parent, MONITOR_DEFAULTTONEAREST), &mi))
        {
            narrow(mi.szDevice, g_output, sizeof(g_output));
            read_display_path(mi.szDevice);
        }
        set_reason("disabled by hdrout-off.txt");
        logoutf("hdrout disabled: hdrout-off.txt present, no D3D/DXGI surface created (OpenGL only)");
        log_state("startup");
        return;
    }
    probe_output();
    logoutf("hdrout presenter deferred t=%u ms: no D3D11 device, interop, window or swap chain before the config is read, then only if the output must be native HDR", SDL_GetTicks());
    log_state("startup");
}

static void perf_release();
static void perf_release_d3d();

// Destroys everything native HDR needs (swap chain, child window, interop,
// D3D11 device). The screen reading and the failure reason are kept.
static void presenter_release(const char *why)
{
    bool had = presenter_allocated();
    sync_drop_fences();
    perf_release_d3d();
    preview_release();
    hide_child();
    release_views();
    if(g_vs) { g_vs->Release(); g_vs = NULL; }
    if(g_ps) { g_ps->Release(); g_ps = NULL; }
    if(g_samp) { g_samp->Release(); g_samp = NULL; }
    if(g_rast) { g_rast->Release(); g_rast = NULL; }
    if(g_blend) { g_blend->Release(); g_blend = NULL; }
    if(g_depth) { g_depth->Release(); g_depth = NULL; }
    if(g_swap3) { g_swap3->Release(); g_swap3 = NULL; }
    if(g_swap) { g_swap->Release(); g_swap = NULL; }
    if(g_dx && pClose) pClose(g_dx);
    g_dx = NULL;
    if(g_ctx) { g_ctx->Release(); g_ctx = NULL; }
    if(g_dev) { g_dev->Release(); g_dev = NULL; }
    g_framelatency = 0;
    if(g_factory) { g_factory->Release(); g_factory = NULL; }
    if(g_child) { DestroyWindow(g_child); g_child = NULL; }
    g_child_on = false;
    if(g_bars) { DestroyWindow(g_bars); g_bars = NULL; }
    g_bars_on = g_bars_want = false;
    g_cx = g_cy = g_cw = g_ch = -1;
    g_ready = false;
    g_w = g_h = 0;
    if(had)
    {
        logoutf("hdrout presenter released (%s) t=%u ms: swap chain, child window, interop and D3D11 device destroyed", why ? why : "", SDL_GetTicks());
        hflush();
    }
}

static bool presenter_open_inner()
{
    if(!g_parent)
    {
        set_reason("SDL HWND unavailable");
        return false;
    }
    if(!g_hdr)
    {
        // Read at startup (or again when Native HDR is chosen): nothing to open.
        if(!g_reason[0] || !strcmp(g_reason, "ok")) set_reason("Windows HDR off on this screen");
        return false;
    }
    if(!load_wgl()) return false;
    IDXGIFactory1 *fac1 = NULL;
    HRESULT hr = CreateDXGIFactory1(IID_IDXGIFactory1, (void **)&fac1);
    if(FAILED(hr) || !fac1)
    {
        set_reason("no DXGI factory");
        logoutf("hdrout api CreateDXGIFactory1 hr 0x%08X", (unsigned)hr);
        return false;
    }
    hr = fac1->QueryInterface(IID_IDXGIFactory2, (void **)&g_factory);
    if(FAILED(hr) || !g_factory)
    {
        fac1->Release();
        set_reason("IDXGIFactory2 missing");
        logoutf("hdrout api Factory2 hr 0x%08X", (unsigned)hr);
        return false;
    }
    HMONITOR mon = MonitorFromWindow(g_parent, MONITOR_DEFAULTTONEAREST);
    UINT ai;
    bool opened = false;
    for(ai = 0; ; ai++)
    {
        IDXGIAdapter1 *ad = NULL;
        if(fac1->EnumAdapters1(ai, &ad) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc;
        memset(&desc, 0, sizeof(desc));
        ad->GetDesc1(&desc);
        if(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
        {
            ad->Release();
            continue;
        }
        ID3D11Device *dev = NULL;
        ID3D11DeviceContext *ctx = NULL;
        D3D_FEATURE_LEVEL fl = (D3D_FEATURE_LEVEL)0;
        hr = D3D11CreateDevice(ad, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
        if(FAILED(hr) || !dev)
        {
            logoutf("hdrout api D3D11CreateDevice hr 0x%08X", (unsigned)hr);
            ad->Release();
            continue;
        }
        HANDLE dx = pOpen(dev);
        if(!dx)
        {
            char name[160];
            narrow(desc.Description, name, sizeof(name));
            logoutf("hdrout interop refused on %s", name);
            ctx->Release();
            dev->Release();
            ad->Release();
            continue;
        }
        g_dev = dev;
        g_ctx = ctx;
        g_dx = dx;
        narrow(desc.Description, g_adapter, sizeof(g_adapter));
        formatstring(g_luid, "%08lX:%08lX", (unsigned long)desc.AdapterLuid.HighPart, (unsigned long)desc.AdapterLuid.LowPart);
        logoutf("hdrout interop open, level 0x%x", int(fl));
        query_output(ad, mon);
        ad->Release();
        opened = true;
        break;
    }
    fac1->Release();
    if(!opened)
    {
        set_reason("no GPU shared by OpenGL and D3D11");
        return false;
    }
    if(!g_hdr) return false;
    const char *sim = getenv("SAUER_HDROUT_SIM");
    if(sim && !strcmp(sim, "init"))
    {
        logoutf("hdrout SIMULATION: HDR unavailable at start-up. The chain is not created. No Windows, VRR, gamma or driver setting is changed. Not a device loss. Not proof of a hardware fault.");
        set_reason("simulation: HDR unavailable at start-up");
        return false;
    }
    if(!make_flip_shader() || !make_shaders() || !build_targets(screenw, screenh)) return false;
    g_ready = true;
    set_reason("ok");
    return true;
}

// Opens the native HDR presenter. Called by hdrout_resolve() only once the
// saved choice is known and asks for native HDR, at the start of a frame.
static bool presenter_open()
{
    if(g_ready) return true;
    if(presenter_allocated()) presenter_release("reopen");
    logoutf("hdrout opening the native HDR presenter t=%u ms (preference=%d)", SDL_GetTicks(), hdroutpref);
    bool ok = presenter_open_inner();
    if(!ok)
    {
        g_open_failed = true;
        presenter_release("open failed");
    }
    log_state(ok ? "open" : "open failed");
    return ok;
}

void hdrout_shutdown()
{
    if(!g_tried && !presenter_allocated()) return;
    presenter_release("shutdown");
    g_syncload = false;
    perf_release();
    g_pvshader = NULL;
    g_map = NULL;
    g_mire = NULL;
    g_parent = NULL;
    g_tried = false;
    g_open_failed = false;
    g_disabled = false;
}

// Performance lot 1: frame time decomposition, off unless hdroutperfwin runs.
// CPU times are QueryPerformanceCounter around the calls. GPU times are GL
// timestamp queries (and D3D11 timestamps for the presentation pass) read
// back without waiting, several frames later. Nothing is logged or written
// while a window is being timed: the summary and the raw file come after it.
enum { PERF_MAX = 30000, PERF_Q = 8, PERF_F = 16 };
static const char *const PERF_NAME[PERF_F] = {
    "periode", "cpu_hors_presentation", "bloc_presentation", "glfinish", "unlock",
    "d3d_cpu", "present_appel", "sdl_swap", "attente_barriere",
    "gpu_gl_trame", "gpu_scene", "gpu_composition", "gpu_hud", "gpu_d3d",
    "debut_cpu_a_fin_gpu", "soumission_a_fin_gpu"
};
// Times of v[14]/v[15] are seconds from the window start until the GPU
// stamps are resolved, then durations in ms.
struct perfrow { float v[PERF_F]; int kind, gpuok, gpucomp, d3dok; };
static perfrow *g_prow = NULL;
static int g_pstate = 0, g_parm = 0, g_pn = 0, g_pdrain = 0, g_plost = 0, g_pd3dlost = 0;
static unsigned g_pframe = 0, g_pbase = 0;
static double g_pend = 0, g_psec = 0, g_pA = 0, g_pB = 0, g_poff = 0, g_pt0 = 0;
static string g_pname;
static PFNGLQUERYCOUNTERPROC pQueryCounter = NULL;
static PFNGLGETQUERYOBJECTUI64VPROC pGetQueryObjectui64v = NULL;
static PFNGLGETQUERYOBJECTIVPROC pGetQueryObjectiv = NULL;
static PFNGLGENQUERIESPROC pGenQueries = NULL;
static PFNGLDELETEQUERIESPROC pDeleteQueries = NULL;
static PFNGLGETINTEGER64VPROC pGetInteger64v = NULL;
static GLuint g_pq[PERF_Q][4];
static unsigned g_pqframe[PERF_Q];
static int g_pqmask[PERF_Q];
static bool g_pqok = false, g_pqtried = false;
static ID3D11Query *g_pdq[PERF_Q][3];
static unsigned g_pdqframe[PERF_Q];
static bool g_pdqon[PERF_Q];

static double perf_now()
{
    static double inv = 0;
    LARGE_INTEGER c;
    if(inv <= 0)
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        inv = 1.0 / double(f.QuadPart);
    }
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) * inv;
}

static perfrow *perf_row()
{
    if(g_pstate != 1 || !g_prow) return NULL;
    int n = int(g_pframe - g_pbase);
    if(n < 0 || n >= PERF_MAX || n >= g_pn) return NULL;
    return &g_prow[n];
}

static void perf_add(int field, double sec)
{
    perfrow *r = perf_row();
    if(r) r->v[field] += float(sec * 1000.0);
}

static bool perf_glload()
{
    if(!g_pqtried)
    {
        g_pqtried = true;
        pQueryCounter = (PFNGLQUERYCOUNTERPROC)wglGetProcAddress("glQueryCounter");
        pGetQueryObjectui64v = (PFNGLGETQUERYOBJECTUI64VPROC)wglGetProcAddress("glGetQueryObjectui64v");
        pGetQueryObjectiv = (PFNGLGETQUERYOBJECTIVPROC)wglGetProcAddress("glGetQueryObjectiv");
        pGenQueries = (PFNGLGENQUERIESPROC)wglGetProcAddress("glGenQueries");
        pDeleteQueries = (PFNGLDELETEQUERIESPROC)wglGetProcAddress("glDeleteQueries");
        pGetInteger64v = (PFNGLGETINTEGER64VPROC)wglGetProcAddress("glGetInteger64v");
        if(pQueryCounter && pGetQueryObjectui64v && pGetQueryObjectiv && pGenQueries && pDeleteQueries && pGetInteger64v)
        {
            pGenQueries(PERF_Q * 4, &g_pq[0][0]);
            g_pqok = true;
        }
        logoutf("hdrout perf: GL queries %s", g_pqok ? "ok" : "missing (GPU times not measured)");
    }
    return g_pqok;
}

static void perf_release()
{
    if(g_pqok && pDeleteQueries) pDeleteQueries(PERF_Q * 4, &g_pq[0][0]);
    g_pqok = false;
    g_pqtried = false;
    loopi(PERF_Q) loopj(3) if(g_pdq[i][j]) { g_pdq[i][j]->Release(); g_pdq[i][j] = NULL; }
    memset(g_pdqon, 0, sizeof(g_pdqon));
    memset(g_pqmask, 0, sizeof(g_pqmask));
    g_pstate = 0;
    g_parm = 0;
}

static void perf_release_d3d()
{
    loopi(PERF_Q) loopj(3) if(g_pdq[i][j]) { g_pdq[i][j]->Release(); g_pdq[i][j] = NULL; }
    memset(g_pdqon, 0, sizeof(g_pdqon));
}

// Offset from the GL clock to the QueryPerformanceCounter clock (seconds).
static double perf_gpu_clock()
{
    GLint64 t = 0;
    pGetInteger64v(GL_TIMESTAMP, &t);
    return perf_now() - double(t) * 1e-9;
}

// Non-blocking. Frame f's stamps land in its row once all are available.
static void perf_poll(bool reuse_slot, int slot)
{
    if(!g_pqok) return;
    loopi(PERF_Q)
    {
        if(!g_pqmask[i]) continue;
        int last = 0;
        loopj(4) if(g_pqmask[i] & (1 << j)) last = j;
        GLint avail = 0;
        pGetQueryObjectiv(g_pq[i][last], GL_QUERY_RESULT_AVAILABLE, &avail);
        if(!avail)
        {
            if(reuse_slot && i == slot) { g_plost++; g_pqmask[i] = 0; }
            continue;
        }
        GLuint64 t[4] = { 0, 0, 0, 0 };
        loopj(4) if(g_pqmask[i] & (1 << j)) pGetQueryObjectui64v(g_pq[i][j], GL_QUERY_RESULT, &t[j]);
        int n = int(g_pqframe[i] - g_pbase);
        if(g_prow && n >= 0 && n < g_pn && (g_pqmask[i] & 9) == 9)
        {
            perfrow &r = g_prow[n];
            r.v[9] = float(double(t[3] - t[0]) * 1e-6);
            if((g_pqmask[i] & 6) == 6)
            {
                r.v[10] = float(double(t[1] - t[0]) * 1e-6);
                r.v[11] = float(double(t[2] - t[1]) * 1e-6);
                r.v[12] = float(double(t[3] - t[2]) * 1e-6);
                r.gpucomp = 1;
            }
            double end = double(t[3]) * 1e-9 + g_poff - g_pt0;
            r.v[14] = float((end - double(r.v[14])) * 1000.0);
            r.v[15] = float((end - double(r.v[15])) * 1000.0);
            r.gpuok = 1;
        }
        g_pqmask[i] = 0;
    }
}

static void perf_gl_stamp(int which)
{
    if(g_pstate != 1 || !g_pqok) return;
    int slot = int(g_pframe % PERF_Q);
    if(which == 0)
    {
        perf_poll(true, slot);
        g_pqmask[slot] = 0;
        g_pqframe[slot] = g_pframe;
    }
    else if(g_pqframe[slot] != g_pframe || !(g_pqmask[slot] & 1)) return;
    pQueryCounter(g_pq[slot][which], GL_TIMESTAMP);
    g_pqmask[slot] |= 1 << which;
}

static void perf_d3d_poll(bool reuse_slot, int slot)
{
    if(!g_ctx) return;
    loopi(PERF_Q)
    {
        if(!g_pdqon[i]) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        UINT64 a = 0, b = 0;
        if(g_ctx->GetData(g_pdq[i][0], &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
           g_ctx->GetData(g_pdq[i][1], &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
           g_ctx->GetData(g_pdq[i][2], &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
        {
            if(reuse_slot && i == slot) { g_pd3dlost++; g_pdqon[i] = false; }
            continue;
        }
        int n = int(g_pdqframe[i] - g_pbase);
        if(g_prow && n >= 0 && n < g_pn && !dj.Disjoint && dj.Frequency)
        {
            g_prow[n].v[13] = float(double(b - a) * 1000.0 / double(dj.Frequency));
            g_prow[n].d3dok = 1;
        }
        g_pdqon[i] = false;
    }
}

static void perf_d3d_begin()
{
    if(g_pstate != 1 || !g_dev || !g_ctx) return;
    int slot = int(g_pframe % PERF_Q);
    if(!g_pdq[slot][0] || !g_pdq[slot][1] || !g_pdq[slot][2])
    {
        D3D11_QUERY_DESC qd;
        memset(&qd, 0, sizeof(qd));
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        if(!g_pdq[slot][0] && FAILED(g_dev->CreateQuery(&qd, &g_pdq[slot][0]))) return;
        qd.Query = D3D11_QUERY_TIMESTAMP;
        if(!g_pdq[slot][1] && FAILED(g_dev->CreateQuery(&qd, &g_pdq[slot][1]))) return;
        if(!g_pdq[slot][2] && FAILED(g_dev->CreateQuery(&qd, &g_pdq[slot][2]))) return;
    }
    perf_d3d_poll(true, slot);
    g_pdqframe[slot] = g_pframe;
    g_ctx->Begin(g_pdq[slot][0]);
    g_ctx->End(g_pdq[slot][1]);
    g_pdqon[slot] = true;
}

static void perf_d3d_end()
{
    if(g_pstate != 1 || !g_ctx) return;
    int slot = int(g_pframe % PERF_Q);
    if(!g_pdqon[slot] || g_pdqframe[slot] != g_pframe) return;
    g_ctx->End(g_pdq[slot][2]);
    g_ctx->End(g_pdq[slot][0]);
}

static int perf_cmpf(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static void perf_finish()
{
    double drift = g_pqok ? (perf_gpu_clock() - g_poff) : 0;
    int n = g_pn;
    logoutf("hdrout perf window %s frames %d duration_s %.2f mode %s transport %s variant_lot %d ngx %d rt %d size %dx%d maxfps %d vsync %d gl_queries_lost %d d3d_lost %d clock_drift_ms %.4f",
            g_pname, n, g_psec, mode_name(), hdroutsync ? "optimise" : "reference_lot8", variant_lot(), hwrtngxmodeapplied(),
            rt_lighting() ? 1 : 0, screenw, screenh, getvar("maxfps"), getvar("vsync"), g_plost, g_pd3dlost, drift * 1000.0);
    float *tmp = n > 0 ? new float[n] : NULL;
    loopk(PERF_F)
    {
        int m = 0;
        double sum = 0;
        loopi(n)
        {
            const perfrow &r = g_prow[i];
            if(k >= 9 && k != 13 && !r.gpuok) continue;
            if(k >= 10 && k <= 12 && !r.gpucomp) continue;
            if(k == 13 && !r.d3dok) continue;
            if(k >= 3 && k <= 8 && k != 7 && r.kind != 1) continue;
            if(k == 7 && r.kind != 0) continue;
            tmp[m++] = r.v[k];
            sum += r.v[k];
        }
        if(!m) { logoutf("hdrout perf %s %s none", g_pname, PERF_NAME[k]); continue; }
        qsort(tmp, m, sizeof(float), perf_cmpf);
        logoutf("hdrout perf %s %s n %d median %.4f p10 %.4f p90 %.4f p99 %.4f mean %.4f ms",
                g_pname, PERF_NAME[k], m, tmp[m / 2], tmp[m / 10], tmp[(m * 9) / 10], tmp[(m * 99) / 100], sum / m);
    }
    delete[] tmp;
    createdir(findfile("perf", "w"));
    defformatstring(rel, "perf/%s.csv", g_pname);
    FILE *fp = fopen(findfile(rel, "w"), "w");
    if(fp)
    {
        fprintf(fp, "i,kind,gpuok,d3dok");
        loopk(PERF_F) fprintf(fp, ",%s", PERF_NAME[k]);
        fprintf(fp, "\n");
        loopi(n)
        {
            const perfrow &r = g_prow[i];
            fprintf(fp, "%d,%d,%d,%d", i, r.kind, r.gpuok, r.d3dok);
            loopk(PERF_F) fprintf(fp, ",%.4f", r.v[k]);
            fprintf(fp, "\n");
        }
        fclose(fp);
    }
    logoutf("hdrout perf end %s file %s", g_pname, fp ? rel : "not written");
    hflush();
    g_pstate = 0;
}

void hdrout_perf_swapstart()
{
    if(g_pstate != 1) return;
    double a = perf_now();
    int n = int(g_pframe - g_pbase);
    if(n >= 0 && n < PERF_MAX)
    {
        perfrow &r = g_prow[n];
        r.v[0] = float((a - g_pA) * 1000.0);
        r.v[1] = float((a - g_pB) * 1000.0);
        r.v[14] = float(g_pB - g_pt0);
        r.v[15] = float(a - g_pt0);
        g_pn = n + 1;
    }
    g_pA = a;
    perf_gl_stamp(3);
}

// kind: 1 = presented by the scRGB swap chain, 0 = SDL_GL_SwapWindow.
void hdrout_perf_swapend(bool surface, double sdlswap)
{
    if(g_pstate == 2)
    {
        perf_poll(false, -1);
        perf_d3d_poll(false, -1);
        if(--g_pdrain <= 0)
        {
            loopi(PERF_Q)
            {
                if(g_pqmask[i]) g_plost++;
                if(g_pdqon[i]) g_pd3dlost++;
                g_pqmask[i] = 0;
                g_pdqon[i] = false;
            }
            perf_finish();
        }
        g_pframe++;
        return;
    }
    double b = perf_now();
    if(g_pstate == 1)
    {
        perfrow *r = perf_row();
        if(r)
        {
            r->v[2] = float((b - g_pA) * 1000.0);
            r->v[7] = float(sdlswap * 1000.0);
            r->kind = surface ? 1 : 0;
        }
        if(b >= g_pend || g_pn >= PERF_MAX) { g_pstate = 2; g_pdrain = PERF_Q + 4; }
    }
    g_pB = b;
    g_pframe++;
    if(g_parm)
    {
        g_parm = 0;
        if(!g_prow) g_prow = new perfrow[PERF_MAX];
        memset(g_prow, 0, sizeof(perfrow) * PERF_MAX);
        memset(g_pqmask, 0, sizeof(g_pqmask));
        memset(g_pdqon, 0, sizeof(g_pdqon));
        g_plost = g_pd3dlost = 0;
        g_pn = 0;
        g_pbase = g_pframe;
        if(perf_glload()) g_poff = perf_gpu_clock();
        g_pA = g_pB = g_pt0 = perf_now();
        g_pend = g_pt0 + g_psec;
        g_pstate = 1;
    }
}

static void hdrout_perfwin(char *name, int *sec)
{
    if(g_pstate || g_parm) { conoutf(CON_WARN, "hdrout perf: a window is already running"); return; }
    copystring(g_pname, name && name[0] ? name : "perf");
    for(char *c = g_pname; *c; c++) if(*c == '/' || *c == '\\' || *c == ':' || iscubespace(*c)) *c = '_';
    g_psec = clamp(*sec, 2, 60);
    g_parm = 1;
}
COMMANDN(hdroutperfwin, hdrout_perfwin, "si");
ICOMMAND(hdroutperfactif, "", (), intret(g_pstate || g_parm ? 1 : 0));

static void hdrout_on_sync()
{
    sync_drop_fences();
    g_sync_timeouts = 0;
    logoutf("hdrout transport %s (hdroutsync %d). Picture, calibration and variant unchanged.",
            hdroutsync ? "optimised: no glFinish, one-frame GL fence, D3D buffer kept"
                       : "lot8 reference: glFinish before unlock, D3D buffer fetched every frame",
            hdroutsync);
    hflush();
}
ICOMMAND(hdrouttransport, "", (), result(hdroutsync ? "Version optimis" CUBE_E_AIGU "e" : "R" CUBE_E_AIGU "f" CUBE_E_AIGU "rence lot8"));

double hdrout_perf_clock() { return perf_now(); }

// Lab check, off by default: "hdroutfraicheur N" marks N frames. GL clears a
// 4x4 corner of the composed scRGB image to a value that changes every frame,
// just before the unlock. D3D reads its copy back before Present (Map waits for
// the D3D copy only). An image drawn from the previous GL frame would show the
// previous value. Not timed: the Map stalls, measurements are separate.
static int g_fresh_left = 0, g_fresh_n = 0, g_fresh_ok = 0, g_fresh_old = 0, g_fresh_bad = 0;
static float g_fresh_want = 0, g_fresh_prev = -1;

static void fresh_mark()
{
    if(g_fresh_left <= 0 || !g_locked || !g_fbo) return;
    g_fresh_prev = g_fresh_want;
    g_fresh_want = 0.5f * float(1 + (g_fresh_n % 48));
    glBindFramebuffer_(GL_FRAMEBUFFER, g_fbo);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, 4, 4);
    glClearColor(g_fresh_want, g_fresh_want, g_fresh_want, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(0, 0, 0, 0);
    glDisable(GL_SCISSOR_TEST);
}

static void fresh_check()
{
    if(g_fresh_left <= 0 || !g_ctx || !g_back || !g_staging) return;
    g_fresh_left--;
    g_fresh_n++;
    g_ctx->CopyResource(g_staging, g_back);
    D3D11_MAPPED_SUBRESOURCE map;
    memset(&map, 0, sizeof(map));
    if(FAILED(g_ctx->Map(g_staging, 0, D3D11_MAP_READ, 0, &map))) { g_fresh_bad++; return; }
    float a[4], b[4];
    sample_px(map, 1, 1, a);
    sample_px(map, 1, g_h - 2, b);
    g_ctx->Unmap(g_staging, 0);
    bool oka = fabsf(a[0] - g_fresh_want) <= 0.01f, okb = fabsf(b[0] - g_fresh_want) <= 0.01f;
    bool olda = fabsf(a[0] - g_fresh_prev) <= 0.01f, oldb = fabsf(b[0] - g_fresh_prev) <= 0.01f;
    if(oka || okb) g_fresh_ok++;
    else if(olda || oldb) g_fresh_old++;
    else g_fresh_bad++;
    if(g_fresh_left <= 0)
    {
        logoutf("hdrout transport freshness %s frames %d current_frame %d previous_frame %d other %d (GL marker before unlock, read back from the D3D buffer before Present)",
                hdroutsync ? "optimise" : "reference_lot8", g_fresh_n, g_fresh_ok, g_fresh_old, g_fresh_bad);
        hflush();
    }
}

static void hdrout_fraicheur(int *n)
{
    g_fresh_left = clamp(*n, 1, 5000);
    g_fresh_n = g_fresh_ok = g_fresh_old = g_fresh_bad = 0;
    g_fresh_want = 0;
    g_fresh_prev = -1;
}
COMMANDN(hdroutfraicheur, hdrout_fraicheur, "i");
