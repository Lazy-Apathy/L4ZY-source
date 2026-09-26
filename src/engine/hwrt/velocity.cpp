// velocity.cpp: motion vectors for static scenery, rigid mapmodels,
// skinned mrfixit (lot 3), mrfixit attached armour / vwep (lot 4),
// the first-person hudgun (lot 5), and lot 6 unjittered motion + HUD-less
// capture for a future DLAA evaluate.
//
// This lot does not turn on DLAA / DLSS / FSR / Streamline / HDR.
//
// Image (hwrtvelocitytex, RGBA16F):
//   RG  motion in pixels, previous_unjittered - current_unjittered
//       history_uv = uv + motion / vec2(w, h)
//                 + (prev_jitter_px - curr_jitter_px) / vec2(w, h)
//         when comparing two colour frames that were actually jittered
//       +X = previous location is to the right
//       +Y = previous location is up (OpenGL, origin bottom-left)
//   B   1 = covered (static world, rigid mapmodel, colour-visible
//       pickup / teleport / carrot (hwrtentxform spin+bob), colour-visible
//       skinned player/bot including supported attachments, or the
//       first-person hudgun), 0 = unsupported
//   A   1 = skinned character / attachment / hudgun pixel, 0 = scenery
//
// B/A are internal engine marks. They are not Streamline transparency,
// bias, or reactivity masks.
//
// Matrices: main-camera camproj captured immediately after
// setcamprojmatrix() in gl_drawframe. When hwrtdlaajitter is on, the
// used matrices include the trial Halton offset and a parallel unjittered
// pair is stored for motion. looktaa jitter is not stripped (the play
// path has looktaa 0; the trial skips looktaa so the offset is not
// applied twice). Geometry is rasterised with the used (jittered) matrices.
//
// Covered pixels:
//   1  opaque world cubes, mapmodels whose geometry does not deform
//      (a global translation or mdlspin is allowed; EF_ANIM, skeletal
//      mapmodels, and vertex-frame interpolation stay unsupported),
//      colour-visible pickups / teleports / carrots via hwrtentxform
//      (same lastmillis yaw and z-bob as fpsgame renderentities),
//      colour-visible ENT_PLAYER skeletal meshes (lot 3: mrfixit body),
//      mrfixit attachments that share that colour path (lot 4:
//      LINK_REUSE armour/horns skinned with the live pose; LINK_TAG vwep
//      with the full world matrix including the tag bone), and the
//      first-person hudgun (lot 5: mrfixit/hudguns/*, own avatar FOV
//      and avatardepth, own history slots)
//   0  ragdolls, monsters, flags, deforming
//      mapmodels, attachments on other playermodels, water, grass,
//      materials, world-alpha, particles, the first-person body when it
//      is only a shadow occluder. Sky never writes 1.
// The hudgun is rasterised into this image after the world/character
// passes, with the same avatar projection the colour gun uses. Coverage
// follows its visible silhouette (alphatest + depth against the scene
// the colour gun tests). It does not share slots with the third-person
// body or vwep.
//
// Character motion is rasterised into this FBO against a blit of the scene
// depth, with the skin alphatest, using the same GL render path that produced
// the colour image. Previous positions come from the dualquats and world
// matrix stored from that instance's last colour-visible draw, not from
// rewinding lastmillis. A hidden character behind a wall fails the depth
// test and cannot punch the wall. The first-person body is not noted during
// the colour pass (it is only drawn into the shadow map), so it cannot write
// vectors onto the floor. Body, each attachment, and the hudgun keep distinct
// history slots (dynent + lifesequence + tag + model); they never share one
// slot. Hudgun previous clip uses the previous avatar camproj, not the
// world camproj.
//
// Rigid object motion is rasterised into this FBO against a blit of the
// scene depth, with the model's alphatest. A hidden object behind a wall
// fails that depth test and cannot punch the wall. A vis-culled model that
// the RT composite still painted in front of stored depth can write, because
// the test uses the visible surface's depth rather than GL's octree vis.
// Pixels unmarked as excluded (particles, water, unsupported models) keep
// that stencil value: the object / character pass must not turn them into
// covered motion, even when the effect wrote no depth. Sky / empty pixels
// stay 0 so a vis-culled model in front of the sky can still write. The
// visible RT instance uses the same lastmillis mdlspin as this pass, so
// colour and vectors describe the same rigid pose. No lighting or shadow
// path is changed to hide errors. Without a matching previous object or
// character pose, coverage stays and motion is written as zero.
//
// Matrices: the main-camera camproj captured immediately after
// setcamprojmatrix() in gl_drawframe. Trial jitter (hwrtdlaajitter) is
// applied there once; looktaa is skipped while the trial is on. Jitter
// is stripped from the motion uniforms, not from the rasterised geometry.
//
// History is discarded on map load, resize, a camera cut, turning the
// feature off, and entering reprojection from a mode that was not copying
// colour. A cut zeroes motion for that frame but does not clear coverage:
// a wall stays marked as static scenery even when there is no usable
// previous colour. The pre-cut colour is unusable from that frame; the
// next frames reseed history from the new view and matching matrices.
// Generating the image does not change the presented frame; only
// hwrtveldebug > 0 replaces the scene with the overlay.
//
// Colour-HUD coverage diagnostic (lot 5 close-out): while a proof or
// A/B store is pending, the real colour hudgun draw writes stencil 4
// with the same depth test and alphatest as the presented gun. That
// mask is compared to velocity A=1. It is not reconstructed from the
// velocity image or the ID buffer. Off when no diagnostic is queued.
// hwrtvelzoom / hwrtvelqvelocity / hwrtvelqcamcut / hwrtvelqjitter /
// hwrtvelqscreenres / hwrtvelqfire / hwrtvelqfreezepose are consumed on
// the frame they reach the head of that queue so they cannot race waitshots.

#include "engine.h"

#ifndef GL_TIME_ELAPSED
#define GL_TIME_ELAPSED 0x88BF
#endif
#ifndef GL_DRAW_FRAMEBUFFER_BINDING
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_READ_FRAMEBUFFER_BINDING
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#endif
#ifndef GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE 0x8CD0
#endif
#ifndef GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE
#define GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE 0x8217
#endif

enum { VEL_QUERY_RING = 3, VEL_STAT_N = 32 };
enum { VELST_OFF = 0, VELST_MARK, VELST_UNMARK, VELST_KEEP, VELST_SKEL };
enum { VEL_ST_EMPTY = 0, VEL_ST_COVER = 1, VEL_ST_EXCLUDE = 2, VEL_ST_SKEL = 3, VEL_ST_HUDCOL = 4 };
enum { VELSHOT_SHOT = 0, VELSHOT_WAIT, VELSHOT_PROOF, VELSHOT_ABSTORE, VELSHOT_ABDIFF, VELSHOT_SETVEL, VELSHOT_ZOOM, VELSHOT_CAMCUT, VELSHOT_PROOFALIVE, VELSHOT_SETJITTER, VELSHOT_SCREENRES, VELSHOT_FIRE, VELSHOT_FREEZEPOSE };

static void velmaybeoff();
void cleanupvelocity();

VARF(hwrtvelocity, 0, 0, 1, velmaybeoff());
VARF(hwrtveldebug, 0, 0, 3, velmaybeoff());
FVAR(hwrtvelscale, 4, 32, 256);
// Test-only camera drive so a screenshot can land on a frame that actually
// contains motion. Degrees / world-units per frame. Play binds leave these at 0.
FVAR(hwrtvelspin, -30, 0, 30);
FVAR(hwrtvelpitchspin, -30, 0, 30);
FVAR(hwrtvelslide, -8, 0, 8);
FVAR(hwrtvelpush, -8, 0, 8);
// Test-only: keep the local player in the run cycle, and emit a grey smoke
// puff in front of the camera. Both are off in play. They do not write
// scenery vectors; they exist so the exclusion mask can be photographed
// on a walking character and on a low-contrast transparent effect.
VARF(hwrtvelwalk, 0, 0, 1, { if(!hwrtvelwalk && player) player->move = 0; });
VAR(hwrtvelsmoke, 0, 0, 1);
FVAR(hwrtvelsmokedist, 1, 16, 64);
VAR(hwrtveldbgmm, 0, 0, 1);
VAR(hwrtveldrive, -1, -1, 10000);
FVAR(hwrtveldriveslide, -40, 0, 40);
FVAR(hwrtveldrivepush, -40, 0, 40);
VAR(hwrtvelholdcam, 0, 0, 1);
VAR(hwrtvelholdplayer, 0, 0, 1);
VAR(hwrtvelholdothers, 0, 0, 1);
VAR(hwrtvelfreezepose, 0, 0, 1);
// Proof / isolated-profile only: keep drawing while the window is minimized
// so a validation run does not steal focus from a live client.
VAR(hwrtvelforce, 0, 0, 1);
// Lot 4: write vectors for mrfixit attached armour / vwep / horns. Off
// leaves those pixels excluded (lot 3 behaviour) for an A/B cost check.
VAR(hwrtvelequip, 0, 1, 1);
// Lot 5: write vectors for the first-person hudgun. Off leaves those
// pixels as scenery (lot 4 behaviour) for an A/B cost check.
VAR(hwrtvelhud, 0, 1, 1);
// 0 = diagnostic only: skip pickup / teleport object MVs (colour still
// spins). Play and the ammo lab leave this at 1.
VAR(hwrtvelents, 0, 1, 1);
// Diagnostic only: mark the colour-pass hudgun silhouette (mains + weapon)
// into the window stencil while the real colour draw runs. Proof shots
// turn this on by themselves. Play leaves it at 0.
VAR(hwrtvelhudcolour, 0, 0, 1);
// Test-only: 1 restores the lot-4 bug (recycle any not-yet-noted slot before
// growing). Default 0 is the corrected policy. Used for before/after traces.
VAR(hwrtvelslotsteal, 0, 0, 1);
// Test-only: draw body slots high-to-low so attachment allocation order flips.
VAR(hwrtveldrawrev, 0, 0, 1);
VAR(hwrtvelbenchdone, 1, 1, 1);

float hwrtvelcopycost = 0, hwrtvelgencost = 0, hwrtvelobjcost = 0, hwrtvelskelcost = 0, hwrtveldebugcost = 0;
float hwrtvelskelcpu = 0;
int hwrtvelskelmem = 0;
bool hwrtvelobjpass = false;
bool hwrtvelskelpass = false;
bool hwrtvelhudpass = false;
int hwrtvelunlitmode = 0;
int hwrtvelidmode = 0;
enum { VEL_RIGID_SLOT = 200, VEL_RIGID_KEEP = 24, VEL_RIGID_HUB = 4 };
static bool velidobj = false;
static bool velidclipover = false;
static matrix4 velidclipmat;

static GLuint veltex = 0, velfb = 0, velstencil = 0;
static GLuint veldepth = 0;
static GLuint velhist[2] = { 0, 0 };
static int velw = 0, velh = 0;
static int velhistslot = 0;
static bool velready = false, velhisthas = false, velhistfresh = false;
static bool velcutframe = false;
static int velpostcut = -1;
static int veldbgmode = 0;
static bool velfailed = false;
static bool velstactive = false;
static int velstmode = VELST_OFF;
static int velcoverpending = 0;
static int velallocmillis = 0, velresizemillis = 0;
enum { VELSTSNAP_MAX = 12 };
struct velstsnap
{
    char tag[40];
    int n0, n1, n2, n3, n4, nother, nread;
    int fb, sten_on, func, ref, mask, wmask, fail, zfail, zpass;
    int velmode, velactive, coverpending, shadowmapping, glaring, drawtex;
    int gl_matches_engine;
};
static velstsnap velstsnaps[VELSTSNAP_MAX];
static int velstsnapn = 0;
static int velwasdebug = 0;
static matrix4 velcurr, velprev;
static matrix4 velcurrunjit, velprevunjit;
static vec velcam(0, 0, 0);
static float velyaw = 0, velpitch = 0;
static bool velcamset = false;
static vec velwalkfrom(0, 0, 0);
static bool velwalkset = false;
static vec velprevcam(0, 0, 0);
static float velprevyaw = 0, velprevpitch = 0;
static bool velprevcamok = false;
static int velprevmillis = 0;
static int velnotemillis = 0;
static int velplacetwo = 0;
static int veloverlapmode = 0;
static vec velholdplayerpos(0, 0, 0);
static vec velholdcampos(0, 0, 0);
static vec velholdpos(0, 0, 0);
static float velholdyaw = 0, velholdpitch = 0;
static float velholdplayeryaw = 0, velholdplayerpitch = 0;

enum { VEL_SHOTQ = 128 };
struct velshotreq { string name; int debug; int kind; float arg; };
static velshotreq velshotq[VEL_SHOTQ];
static int velshothead = 0, velshotcount = 0;
static float velzoomhold = -1;
static int velalivewait = 0;
static matrix4 vellogworld, vellogprevworld;
static bool velloghas = false;
static int vellogidx = -1;

static GLuint velqcopy[VEL_QUERY_RING], velqgen[VEL_QUERY_RING], velqobj[VEL_QUERY_RING], velqskel[VEL_QUERY_RING], velqdbg[VEL_QUERY_RING];
static bool velqok = false;
static bool velqcopypend[VEL_QUERY_RING], velqgenpend[VEL_QUERY_RING], velqobjpend[VEL_QUERY_RING], velqskelpend[VEL_QUERY_RING], velqdbgpend[VEL_QUERY_RING];
static int velqcopyslot = 0, velqgenslot = 0, velqobjslot = 0, velqskelslot = 0, velqdbgslot = 0;
static int velqcopyact = -1, velqgenact = -1, velqobjact = -1, velqskelact = -1, velqdbgact = -1;
static int velqepoch = 1;
static int velqwatchequip = -1;
static int velqcopyep[VEL_QUERY_RING], velqgenep[VEL_QUERY_RING], velqobjep[VEL_QUERY_RING], velqskelep[VEL_QUERY_RING], velqdbgep[VEL_QUERY_RING];
static int velqcopyeq[VEL_QUERY_RING], velqgeneq[VEL_QUERY_RING], velqobjeq[VEL_QUERY_RING], velqskeleq[VEL_QUERY_RING], velqdbgeq[VEL_QUERY_RING];
static int velhqcopy = 0, velhqgen = 0, velhqobj = 0, velhqskel = 0;
static int velhqcopyep = -1, velhqgenep = -1, velhqobjep = -1, velhqskelep = -1;
static int velhqcopyeq = -1, velhqgeneq = -1, velhqobjeq = -1, velhqskeleq = -1;

static float velstat_copy[VEL_STAT_N], velstat_gen[VEL_STAT_N], velstat_obj[VEL_STAT_N], velstat_skel[VEL_STAT_N], velstat_dbg[VEL_STAT_N], velstat_skelcpu[VEL_STAT_N];
static int velstatn = 0, velstati = 0;

struct velobjhist
{
    matrix4 world;
    matrix4 curused, prevused, curunjit, prevunjit;
    int attr2;
    uchar type;
    bool valid, seen, haveclip, haveprevclip;
    velobjhist() : attr2(-1), type(ET_EMPTY), valid(false), seen(false), haveclip(false), haveprevclip(false)
    {
        world.identity();
        curused.identity();
        prevused.identity();
        curunjit.identity();
        prevunjit.identity();
    }
};
static vector<velobjhist> velobjs;
static vector<int> velplaced;
static int velobjidx = -1;
static bool velobjhasprev = false, velobjgotroot = false;
static matrix4 velobjprevworld, velobjcurrroot;
static dynent *velhudowner = NULL;

enum { VEL_SKEL_MAX = 64, VEL_ATT_MAX = 4 };
enum { VELATT_BODY = 0, VELATT_ARMOUR = 1, VELATT_WEAPON = 2, VELATT_HORNS = 3, VELATT_HUDGUN = 4 };
enum { VELTR_N = 10, VELTR_CAND_MAX = 16384, VELTR_KEEP_MAX = 240, VELTR_PER_REG = 12 };
struct velattrec
{
    char tag[32];
    char name[80];
    int anim, basetime;
    velattrec() : anim(-1), basetime(0) { tag[0] = 0; name[0] = 0; }
};
struct velskelhist
{
    dynent *d;
    model *m;
    int anim, basetime, basetime2;
    vec pos, o, prevpos;
    float yaw, pitch;
    int nverts, nbones, seq, uid, vweights, vblends;
    int attachkey, attachkind, natt;
    dualquat *bones, *prevdraw;
    matrix4 world, curmm, prevmm, curmmunjit, prevmmunjit;
    int notedframe, committedframe, nprevdraw;
    bool validprev, usedprev, noted, gpuskin;
    velattrec atts[VEL_ATT_MAX];
    velskelhist() : d(NULL), m(NULL), anim(0), basetime(0), basetime2(0), pos(0, 0, 0), o(0, 0, 0), prevpos(0, 0, 0),
        yaw(0), pitch(0), nverts(0), nbones(0), seq(0), uid(0), vweights(0), vblends(0), attachkey(0), attachkind(0), natt(0),
        bones(NULL), prevdraw(NULL), notedframe(-1), committedframe(-1), nprevdraw(0),
        validprev(false), usedprev(false), noted(false), gpuskin(false)
    {
        world.identity();
        curmm.identity();
        prevmm.identity();
        curmmunjit.identity();
        prevmmunjit.identity();
    }
};
static velskelhist velskels[VEL_SKEL_MAX];
static int velskeln = 0, velskelframe = 0, velskelactive = -1, velskeldrawn = 0, velequipdrawn = 0, velhuddrawn = 0;
static matrix4 velavatarcurr, velprevavatar;
static matrix4 velavatarcurrunjit, velprevavatarunjit;
static bool velprevavatarok = false;
static float velprevavatarfov = 0;
static bool velprevavatarfovok = false;
static uchar *velhudcolst = NULL;
static int velhudcolw = 0, velhudcolh = 0;
static bool velhudcolok = false, velhudcolmarking = false;
static GLboolean velhudcol_hadst = GL_FALSE;
static GLint velhudcol_wmask = 0xFF, velhudcol_func = GL_ALWAYS, velhudcol_ref = 0, velhudcol_vmask = 0xFF;
static GLint velhudcol_fail = GL_KEEP, velhudcol_zfail = GL_KEEP, velhudcol_zpass = GL_KEEP;
static vec velcolourmuzzle(-1, -1, -1);
static bool velcolourmuzzleok = false;
static uchar *velabref = NULL, *velabmask = NULL;
static int velabw = 0, velabh = 0;
static vec velabmuzzle(-1, -1, -1);
static bool velabok = false;
static const dualquat *velskelprevb = NULL;
static int velskelprevn = 0;
static dualquat *velskelprevstore = NULL;
static int velskelprevstoremax = 0;

enum { VELFIND_MATCH = 0, VELFIND_EMPTY, VELFIND_GROW, VELFIND_RECYCLE, VELFIND_STEAL, VELFIND_FAIL };
static FILE *velslotlogf = NULL;
static string velslotlogpath;
static int velslotlogn = 0;
struct velslotwatch
{
    int uid, kind, key, slot, committed, notedframe;
    bool usedprev, validprev;
};
static velslotwatch velwatches[VEL_SKEL_MAX];
static int velwatchn = 0;

enum { VELBENCH_IDLE = 0, VELBENCH_WAITQ, VELBENCH_WARM, VELBENCH_DROP, VELBENCH_RUN };
static int velbenchphase = VELBENCH_IDLE;
static int velbenchleft = 0, velbenchwarm = 24, velbenchnwant = 32, velbenchequip = 0, velbenchn = 0, velbenchdiscard = 0, velbenchwait = 0, velbenchrunframes = 0;
static float velbench_copy[VEL_STAT_N], velbench_gen[VEL_STAT_N], velbench_obj[VEL_STAT_N], velbench_skel[VEL_STAT_N], velbench_cpu[VEL_STAT_N], velbench_play[VEL_STAT_N];
static int velbench_chars[VEL_STAT_N], velbench_equipn[VEL_STAT_N];
static string velbenchname;
static char *velbenchthen = NULL;
static char *velbenchpending = NULL;
static int velspinfleft = 0, velslidefleft = 0, velpushfleft = 0;
static int velframewaitleft = 0;
static char *velframewaitcmd = NULL;
static int veldpleft = 0, veldsleft = 0;
static float veldpstep = 0, veldsstep = 0;

static void veltickframewait()
{
    if(velframewaitleft <= 0) return;
    velframewaitleft--;
    if(velframewaitleft > 0 || !velframewaitcmd) return;
    char *cmd = velframewaitcmd;
    velframewaitcmd = NULL;
    execute(cmd);
    delete[] cmd;
}

static const char *veltrname[VELTR_N] = { "torso", "arm_l", "arm_r", "leg_l", "leg_r", "armour", "weapon", "horns", "hud_hands", "hud_gun" };
enum
{
    VELTR_VALID = 0,
    VELTR_OFFSCREEN_CUR,
    VELTR_OFFSCREEN_PREV,
    VELTR_OCCLUDED_WORLD,
    VELTR_OCCLUDED_OTHER_TRI,
    VELTR_OCCLUDED_OTHER_CHAR,
    VELTR_NEWLY_VISIBLE,
    VELTR_DISAPPEARED,
    VELTR_SAMPLE_MISS,
    VELTR_NO_HISTORY,
    VELTR_CAUSE_N
};
static const char *veltrcause[VELTR_CAUSE_N] =
{
    "valid", "offscreen_cur", "offscreen_prev", "occluded_world", "occluded_other_tri",
    "occluded_other_char", "newly_visible", "disappeared", "sample_miss", "no_history"
};
struct veltrack
{
    int slot, region, mesh, tri;
    vec cur, prev;
    float score, area;
    veltrack() : slot(-1), region(-1), mesh(-1), tri(-1), cur(0, 0, 0), prev(0, 0, 0), score(-1), area(0) {}
};
static vector<veltrack> veltrackcands;
static veltrack veltracks[VELTR_KEEP_MAX];
static int veltrackn = 0;
static bool veltrackon = false;

struct velrtri
{
    int mesh, tri;
    vec a, b, c, pos;
    float axisdist, area, scx, scy;
};
static vector<velrtri> velrigidtris;

bool hwrtvelidobj() { return velidobj; }
int hwrtvelidslot() { return velidobj ? VEL_RIGID_SLOT : -1; }

void hwrtvelidsetclip(const matrix4 *m)
{
    if(m)
    {
        velidclipover = true;
        velidclipmat = *m;
    }
    else velidclipover = false;
}

void hwrtvelidapplyclip()
{
    if(velidclipover) GLOBALPARAM(modelmatrix, velidclipmat);
}

void hwrtveladdrigidtri(int mesh, int tri, const vec &a, const vec &b, const vec &c, float axisdist, float area)
{
    velrtri &t = velrigidtris.add();
    t.mesh = mesh;
    t.tri = tri;
    t.a = a;
    t.b = b;
    t.c = c;
    t.pos = vec(a).add(b).add(c).mul(1.0f/3.0f);
    t.axisdist = axisdist;
    t.area = area;
    t.scx = t.scy = 0;
}

static GLuint velunlittex[2] = { 0, 0 };
static GLuint velidtex[2] = { 0, 0 };
static GLuint veldiagfb = 0, veldiagds = 0;
static GLuint velscenefb[2] = { 0, 0 };
static GLuint velsceneds[2] = { 0, 0 };
static GLuint velscenecol[2] = { 0, 0 };
static int velsceneslot = 0;
static bool velscenehasprev = false;
static bool velunlitok = false;
static bool velidok = false;
static int veldiagw = 0, veldiagh = 0;

static void velskelfree(velskelhist &h)
{
    DELETEA(h.bones);
    DELETEA(h.prevdraw);
    h.nbones = 0;
    h.nprevdraw = 0;
    h.nverts = 0;
    h.validprev = false;
    h.usedprev = false;
    h.d = NULL;
    h.m = NULL;
    h.noted = false;
    h.attachkey = 0;
    h.attachkind = 0;
    h.uid = 0;
    h.seq = 0;
    h.natt = 0;
    h.committedframe = -1;
}

static void velcopyskelprevdraw(velskelhist &h);
static void velbindskelprevdraw(velskelhist &h);

static int velskelmembytes()
{
    int n = 0;
    loopi(velskeln)
    {
        if(velskels[i].bones) n += velskels[i].nbones * int(sizeof(dualquat));
        if(velskels[i].prevdraw) n += velskels[i].nprevdraw * int(sizeof(dualquat));
    }
    if(velskelprevstore) n += velskelprevstoremax * int(sizeof(dualquat));
    return n;
}

static bool velwanted()
{
    return (hwrtvelocity || hwrtveldebug || hwrtdlaajitter || hwrtdlaaneedsdata()) && screenw > 0 && screenh > 0 && !velfailed
        && (!minimized || velshotcount > 0 || hwrtvelforce);
}

bool hwrtvelneeddraw()
{
    return velshotcount > 0 || hwrtvelforce;
}

static bool velhistusable()
{
    return velhisthas && velhistfresh && velready && !velcutframe;
}

static void velprobelog(const char *tag);
static void velwriteproof(const char *name, GLuint prevhist);
static bool velwantproof();
static void hwrtvelclearplaced();
static int velensuremm(const char *name);
static extentity *velplacemm(const char *name, const vec &o, int yaw, int *outidx);
static void velbenchstep();

static void velstatreset()
{
    velstatn = velstati = 0;
    hwrtvelcopycost = hwrtvelgencost = hwrtvelobjcost = hwrtvelskelcost = hwrtveldebugcost = 0;
    hwrtvelskelcpu = 0;
}

static void velstatpush()
{
    velstat_copy[velstati] = hwrtvelcopycost;
    velstat_gen[velstati] = hwrtvelgencost;
    velstat_obj[velstati] = hwrtvelobjcost;
    velstat_skel[velstati] = hwrtvelskelcost;
    velstat_dbg[velstati] = hwrtveldebugcost;
    velstat_skelcpu[velstati] = hwrtvelskelcpu;
    velstati = (velstati + 1) % VEL_STAT_N;
    if(velstatn < VEL_STAT_N) velstatn++;
}

static void velreset(const char *why)
{
    bool waslive = velready || velhisthas;
    velready = false;
    velhisthas = false;
    velhistfresh = false;
    velcamset = false;
    velcutframe = false;
    velpostcut = -1;
    veldbgmode = 0;
    velobjs.shrink(0);
    loopi(velskeln) velskelfree(velskels[i]);
    velskeln = 0;
    velskelactive = -1;
    velskelprevb = NULL;
    velskelprevn = 0;
    DELETEA(velskelprevstore);
    velskelprevstoremax = 0;
    hwrtvelskelmem = 0;
    velequipdrawn = 0;
    velhuddrawn = 0;
    velprevavatarok = false;
    velprevavatarfovok = false;
    velhudcolok = false;
    velhudcolmarking = false;
    velcolourmuzzleok = false;
    hwrtvelobjpass = false;
    hwrtvelskelpass = false;
    hwrtvelhudpass = false;
    hwrtvelunlitmode = 0;
    hwrtvelidmode = 0;
    velidobj = false;
    velidclipover = false;
    veltrackon = false;
    veltrackn = 0;
    veltrackcands.setsize(0);
    velrigidtris.setsize(0);
    velunlitok = false;
    velidok = false;
    velscenehasprev = false;
}

static void velmaybeoff()
{
    if(!hwrtvelocity && !hwrtveldebug && !hwrtdlaajitter && !hwrtdlaaneedsdata())
    {
        cleanupvelocity();
        cleanuptemporal();
        velwasdebug = 0;
        return;
    }
    if(hwrtveldebug < 2 || velwasdebug < 2)
    {
        if(velhisthas || velhistfresh)
            conoutf("hwrt velocity: colour history dropped (debug %d -> %d, not a matching pair)", velwasdebug, int(hwrtveldebug));
        velhisthas = false;
        velhistfresh = false;
    }
    velwasdebug = hwrtveldebug;
}

void hwrtresetvelocity()
{
    velreset("map");
    hwrttemporalreset("map");
}

GLuint hwrtvelocitytexid() { return veltex; }
bool hwrtvelocitycutframe() { return velcutframe; }
bool hwrtvelocityready() { return velready; }

void cleanupvelocity()
{
    if(velslotlogf) { fclose(velslotlogf); velslotlogf = NULL; }
    if(velfb) { glDeleteFramebuffers_(1, &velfb); velfb = 0; }
    if(veltex) { glDeleteTextures(1, &veltex); veltex = 0; }
    if(veldepth) { glDeleteTextures(1, &veldepth); veldepth = 0; }
    if(velstencil) { glDeleteRenderbuffers_(1, &velstencil); velstencil = 0; }
    loopi(2) if(velhist[i]) { glDeleteTextures(1, &velhist[i]); velhist[i] = 0; }
    if(velunlittex[0] || velidtex[0] || veldiagfb || velscenefb[0])
    {
        if(veldiagfb) { glDeleteFramebuffers_(1, &veldiagfb); veldiagfb = 0; }
        if(veldiagds) { glDeleteRenderbuffers_(1, &veldiagds); veldiagds = 0; }
        loopi(2)
        {
            if(velscenefb[i]) { glDeleteFramebuffers_(1, &velscenefb[i]); velscenefb[i] = 0; }
            if(velsceneds[i]) { glDeleteRenderbuffers_(1, &velsceneds[i]); velsceneds[i] = 0; }
            if(velscenecol[i]) { glDeleteTextures(1, &velscenecol[i]); velscenecol[i] = 0; }
            if(velunlittex[i]) { glDeleteTextures(1, &velunlittex[i]); velunlittex[i] = 0; }
            if(velidtex[i]) { glDeleteTextures(1, &velidtex[i]); velidtex[i] = 0; }
        }
        velunlitok = false;
        velidok = false;
        velscenehasprev = false;
        veldiagw = veldiagh = 0;
    }
    if(velqok)
    {
        glDeleteQueries_(VEL_QUERY_RING, velqcopy);
        glDeleteQueries_(VEL_QUERY_RING, velqgen);
        glDeleteQueries_(VEL_QUERY_RING, velqobj);
        glDeleteQueries_(VEL_QUERY_RING, velqskel);
        glDeleteQueries_(VEL_QUERY_RING, velqdbg);
        velqok = false;
    }
    memset(velqcopypend, 0, sizeof(velqcopypend));
    memset(velqgenpend, 0, sizeof(velqgenpend));
    memset(velqobjpend, 0, sizeof(velqobjpend));
    memset(velqskelpend, 0, sizeof(velqskelpend));
    memset(velqdbgpend, 0, sizeof(velqdbgpend));
    velqcopyslot = velqgenslot = velqobjslot = velqskelslot = velqdbgslot = 0;
    velqcopyact = velqgenact = velqobjact = velqskelact = velqdbgact = -1;
    velqepoch++;
    velw = velh = 0;
    velstactive = false;
    velstmode = VELST_OFF;
    velcoverpending = 0;
    DELETEA(velhudcolst);
    velhudcolw = velhudcolh = 0;
    velhudcolok = false;
    velhudcolmarking = false;
    // Keep the CPU A/B colour snapshot. Turning generation off is the
    // comparison, so cleanup must not erase the store.
    velfailed = false;
    velwasdebug = 0;
    velstatreset();
    velreset("off");
}

static void velinittimers()
{
    if(velqok) return;
    if(!glGenQueries_ || !glBeginQuery_ || !glEndQuery_ || !glGetQueryObjectiv_ || !glGetQueryObjectuiv_) return;
    glGenQueries_(VEL_QUERY_RING, velqcopy);
    glGenQueries_(VEL_QUERY_RING, velqgen);
    glGenQueries_(VEL_QUERY_RING, velqobj);
    glGenQueries_(VEL_QUERY_RING, velqskel);
    glGenQueries_(VEL_QUERY_RING, velqdbg);
    memset(velqcopypend, 0, sizeof(velqcopypend));
    memset(velqgenpend, 0, sizeof(velqgenpend));
    memset(velqobjpend, 0, sizeof(velqobjpend));
    memset(velqskelpend, 0, sizeof(velqskelpend));
    memset(velqdbgpend, 0, sizeof(velqdbgpend));
    velqok = true;
}

static bool velharvest(GLuint *ids, bool *pend, int slot, float *dst, int *epochs, int *equips, int *okst, int *outep, int *outeq)
{
    if(okst) *okst = 0;
    if(outep) *outep = -1;
    if(outeq) *outeq = -1;
    if(!velqok || !pend[slot]) return false;
    GLint avail = 0;
    glGetQueryObjectiv_(ids[slot], GL_QUERY_RESULT_AVAILABLE, &avail);
    if(!avail) return false;
    GLuint ns = 0;
    glGetQueryObjectuiv_(ids[slot], GL_QUERY_RESULT, &ns);
    pend[slot] = false;
    if(outep) *outep = epochs[slot];
    if(outeq) *outeq = equips[slot];
    if(epochs[slot] != velqepoch)
    {
        if(okst) *okst = -1;
        return false;
    }
    *dst = ns / 1.0e6f;
    if(okst) *okst = 1;
    return true;
}

static void velbeginq(GLuint *ids, bool *pend, int *slot, int *act, int *epochs, int *equips)
{
    if(!velqok || *act >= 0 || pend[*slot]) return;
    glBeginQuery_(GL_TIME_ELAPSED, ids[*slot]);
    epochs[*slot] = velqepoch;
    equips[*slot] = int(hwrtvelequip);
    *act = *slot;
}

static void velendq(bool *pend, int *slot, int *act)
{
    if(*act < 0) return;
    glEndQuery_(GL_TIME_ELAPSED);
    pend[*slot] = true;
    *slot = (*slot + 1) % VEL_QUERY_RING;
    *act = -1;
}

static void makecolourtex(GLuint &id, int w, int h, GLenum fmt, int filter)
{
    if(!id) glGenTextures(1, &id);
    createtexture(id, w, h, NULL, 3, filter, fmt);
}

static void makedepthtex(GLuint &id, int w, int h)
{
    if(!id) glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
}

static int velstencilbits()
{
    // GL_STENCIL_BITS was removed from the 3.3 core profile (INVALID_ENUM).
    // Ask the default framebuffer's stencil attachment instead.
    typedef void (APIENTRYP PFNGETFBPARAM)(GLenum, GLenum, GLenum, GLint *);
    static PFNGETFBPARAM getfbparam = NULL;
    static bool looked = false;
    if(!looked)
    {
        looked = true;
        getfbparam = (PFNGETFBPARAM)getprocaddress("glGetFramebufferAttachmentParameteriv");
        if(!getfbparam) getfbparam = (PFNGETFBPARAM)getprocaddress("glGetFramebufferAttachmentParameterivEXT");
    }
    GLint prevfb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevfb);
    glBindFramebuffer_(GL_FRAMEBUFFER, 0);
    GLint bits = 0;
    if(getfbparam)
    {
        GLint type = 0;
        getfbparam(GL_FRAMEBUFFER, GL_STENCIL, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
        if(type != GL_NONE)
            getfbparam(GL_FRAMEBUFFER, GL_STENCIL, GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE, &bits);
    }
    else
    {
        int sdlbits = 0;
        SDL_GL_GetAttribute(SDL_GL_STENCIL_SIZE, &sdlbits);
        bits = sdlbits;
    }
    glBindFramebuffer_(GL_FRAMEBUFFER, prevfb);
    return bits;
}

static bool velensure()
{
    if(velfailed) return false;
    if(!hasFBO || !hasTF || !hasFBB)
    {
        conoutf(CON_WARN, "hwrt velocity: needs framebuffer blit and float textures");
        velfailed = true;
        return false;
    }
    int bits = velstencilbits();
    if(bits < 1)
    {
        conoutf(CON_WARN, "hwrt velocity: no stencil buffer, cannot mark covered surfaces");
        velfailed = true;
        return false;
    }
    Shader *gen = useshaderbyname("hwrtvelocity");
    Shader *dbg = useshaderbyname("hwrtveldebug");
    Shader *obj = useshaderbyname("hwrtvelobject");
    Shader *skel = useshaderbyname("hwrtvelskel");
    if(!gen || gen->invalid() || !dbg || dbg->invalid() || !obj || obj->invalid() || !skel || skel->invalid())
    {
        conoutf(CON_WARN, "hwrt velocity: shaders missing, diagnostic disabled");
        velfailed = true;
        return false;
    }
    int w = hwrtfbw(), h = hwrtfbh();
    if(w < 8 || h < 8) { w = screenw; h = screenh; }
    if(veltex && velw == w && velh == h) return true;

    bool firstalloc = !veltex;
    makecolourtex(veltex, w, h, GL_RGBA16F, 0);
    loopi(2) makecolourtex(velhist[i], w, h, GL_RGB, 1);
    makedepthtex(veldepth, w, h);
    if(!velstencil) glGenRenderbuffers_(1, &velstencil);
    glBindRenderbuffer_(GL_RENDERBUFFER, velstencil);
    glRenderbufferStorage_(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glBindRenderbuffer_(GL_RENDERBUFFER, 0);
    if(!velfb) glGenFramebuffers_(1, &velfb);
    glBindFramebuffer_(GL_FRAMEBUFFER, velfb);
    glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, veltex, 0);
    glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, velstencil);
    GLenum status = glCheckFramebufferStatus_(GL_FRAMEBUFFER);
    if(status != GL_FRAMEBUFFER_COMPLETE)
    {
        glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
        glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, velstencil);
        status = glCheckFramebufferStatus_(GL_FRAMEBUFFER);
    }
    glBindFramebuffer_(GL_FRAMEBUFFER, 0);
    if(status != GL_FRAMEBUFFER_COMPLETE)
    {
        conoutf(CON_WARN, "hwrt velocity: FBO incomplete (0x%x)", int(status));
        cleanupvelocity();
        velfailed = true;
        return false;
    }
    velw = w;
    velh = h;
    if(firstalloc)
    {
        velallocmillis = lastmillis;
        velreset("allocate");
        hwrttemporalreset("allocate");
    }
    else
    {
        velresizemillis = lastmillis;
        velreset("resolution");
        hwrttemporalreset("resolution");
    }
    conoutf("hwrt velocity: %dx%d RGBA16F, pixels, +Y up, stencil %d, static world + rigid mapmodels + skinned players (%s)",
            w, h, velstencilbits(), firstalloc ? "allocate" : "resize");
    return true;
}

void hwrtvelocitysize(int &w, int &h)
{
    w = velw;
    h = velh;
}

bool hwrtvelensureresources()
{
    if(!velwanted()) return false;
    return velensure();
}

static void velcopytex(GLuint tex)
{
    GLint prevfb = 0, prevactive = 0, prevtex = 0;
    GLboolean hadscissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevfb);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevactive);
    glActiveTexture_(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevtex);
    if(hadscissor) glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer_(GL_FRAMEBUFFER, hwrtmainfbo());
    glBindTexture(GL_TEXTURE_2D, tex);
    int cw = min(velw, hwrtfbw()), ch = min(velh, hwrtfbh());
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, cw, ch);
    if(hadscissor) glEnable(GL_SCISSOR_TEST);
    glBindTexture(GL_TEXTURE_2D, prevtex);
    glActiveTexture_(GLenum(prevactive));
    glBindFramebuffer_(GL_FRAMEBUFFER, prevfb);
}

static void velsetstencil(int mode)
{
    if(!velstactive) return;
    velstmode = mode;
    switch(mode)
    {
        case VELST_MARK:
            glEnable(GL_STENCIL_TEST);
            glStencilMask(0xFF);
            glStencilFunc(GL_ALWAYS, VEL_ST_COVER, 0xFF);
            glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
            break;
        case VELST_UNMARK:
            glEnable(GL_STENCIL_TEST);
            glStencilMask(0xFF);
            glStencilFunc(GL_ALWAYS, VEL_ST_EXCLUDE, 0xFF);
            glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
            break;
        case VELST_SKEL:
            glEnable(GL_STENCIL_TEST);
            glStencilMask(0xFF);
            glStencilFunc(GL_ALWAYS, VEL_ST_SKEL, 0xFF);
            glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
            break;
        case VELST_KEEP:
        default:
            // Offscreen glare / reflections / shadow maps keep the window
            // stencil as-is. A leftover ALWAYS+REPLACE would paint COVER or
            // SKEL over the décor. Fullscreen quads must hit this path.
            glDisable(GL_STENCIL_TEST);
            glStencilMask(0);
            glStencilFunc(GL_ALWAYS, 0, 0xFF);
            glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
            break;
    }
}

bool hwrtvelmarking()
{
    return velstactive && !drawtex && !reflecting && !refracting && !shadowmapping && !glaring;
}

int hwrtvelcoverpending()
{
    return velcoverpending;
}

bool hwrtveloverlay()
{
    return hwrtveldebug > 0;
}

bool hwrtvelrigidmapmodel(const extentity &e)
{
    if(e.flags&EF_ANIM) return false;
    model *m = loadmapmodel(e.attr2);
    if(!m) return true;
    if(m->deforms()) return false;
    return true;
}

bool hwrtvelstaticmapmodel(const extentity &e)
{
    if(!hwrtvelrigidmapmodel(e)) return false;
    model *m = loadmapmodel(e.attr2);
    if(!m) return true;
    if(fabs(m->spinyaw) > 1e-3f || fabs(m->spinpitch) > 1e-3f) return false;
    return true;
}

void hwrtvelbeginframe()
{
    velstactive = false;
    velstmode = VELST_OFF;
    velstsnapn = 0;
    velhudcolok = false;
    velhudcolmarking = false;
    velcolourmuzzleok = false;
    if(!velwanted() || !velensure()) return;
    velstactive = true;
    velcoverpending = 0;
    velskelframe++;
    loopi(velskeln) velskels[i].noted = false;
    glClearStencil(0);
    glStencilMask(0xFF);
    glClear(GL_STENCIL_BUFFER_BIT);
    velstmode = VELST_OFF;
    velsetstencil(VELST_KEEP);
}

void hwrtvelmark()
{
    velcoverpending = 1;
    if(hwrtvelmarking()) velsetstencil(VELST_MARK);
}

void hwrtvelunmark()
{
    velcoverpending = -1;
    if(hwrtvelmarking()) velsetstencil(VELST_UNMARK);
}

void hwrtvelmarkskel()
{
    velcoverpending = 2;
    if(hwrtvelmarking()) velsetstencil(VELST_SKEL);
}

void hwrtvelkeep()
{
    velcoverpending = 0;
    if(velstactive) velsetstencil(VELST_KEEP);
}

void hwrtvelendmark()
{
    if(!velstactive) return;
    glDisable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
    velstactive = false;
    velstmode = VELST_OFF;
    velcoverpending = 0;
}

static bool velwantcolourhud()
{
    if(drawtex || reflecting || refracting || glaring) return false;
    if(isthirdperson()) return false;
    if(hwrtvelhudcolour) return true;
    if(velshotcount <= 0) return false;
    int k = velshotq[velshothead].kind;
    return k == VELSHOT_PROOF || k == VELSHOT_PROOFALIVE || k == VELSHOT_ABSTORE;
}

void hwrtvelbegincolourhud()
{
    velhudcolmarking = false;
    if(!velwantcolourhud()) return;
    GLint fb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
    if(fb != 0) return;
    velhudcol_hadst = glIsEnabled(GL_STENCIL_TEST);
    glGetIntegerv(GL_STENCIL_WRITEMASK, &velhudcol_wmask);
    glGetIntegerv(GL_STENCIL_FUNC, &velhudcol_func);
    glGetIntegerv(GL_STENCIL_REF, &velhudcol_ref);
    glGetIntegerv(GL_STENCIL_VALUE_MASK, &velhudcol_vmask);
    glGetIntegerv(GL_STENCIL_FAIL, &velhudcol_fail);
    glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL, &velhudcol_zfail);
    glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &velhudcol_zpass);
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
    glStencilFunc(GL_ALWAYS, VEL_ST_HUDCOL, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    velhudcolmarking = true;
}

void hwrtvelendcolourhud()
{
    if(!velhudcolmarking) return;
    velhudcolmarking = false;
    GLint fb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
    (void)fb;
    int w = hwrtfbw(), h = hwrtfbh();
    if(w >= 8 && h >= 8)
    {
        if(!velhudcolst || velhudcolw != w || velhudcolh != h)
        {
            DELETEA(velhudcolst);
            velhudcolst = new uchar[w * h];
            velhudcolw = w;
            velhudcolh = h;
        }
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, velhudcolst);
        velhudcolok = true;
    }
    if(velhudcol_hadst) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    glStencilMask(velhudcol_wmask);
    glStencilFunc(velhudcol_func, velhudcol_ref, velhudcol_vmask);
    glStencilOp(velhudcol_fail, velhudcol_zfail, velhudcol_zpass);
}

void hwrtvelnotecolourmuzzle(const vec &m)
{
    velcolourmuzzle = m;
    velcolourmuzzleok = true;
}

void hwrtvelpunchmovingmm()
{
}

void hwrtvelsetobjparams(float alphatest)
{
    LOCALPARAMF(velparams, velw > 0 ? 1.0f/velw : 1.0f, velh > 0 ? 1.0f/velh : 1.0f,
                velobjhasprev && !velcutframe ? 1.0f : 0.0f, alphatest);
    float jcx = 0, jcy = 0, jpx = 0, jpy = 0;
    hwrttemporaljitterpixels(jcx, jcy, jpx, jpy);
    LOCALPARAMF(veljitter, jcx, jcy, jpx, jpy);
}

void hwrtvelonobjmatrix(const matrix4 &modelmatrix, const matrix4 &stackm, float scale, const vec &translate)
{
    matrix4 world = stackm;
    if(scale != 1) world.scale(scale);
    if(!translate.iszero()) world.translate(translate);
    if(!velobjgotroot)
    {
        velobjcurrroot = world;
        velobjgotroot = true;
    }
    matrix4 prevmm;
    matrix4 currunjit;
    const matrix4 &ccamunjit = hwrtvelhudpass ? velavatarcurrunjit : velcurrunjit;
    currunjit.mul(ccamunjit, world);
    GLOBALPARAM(currunjitmatrix, currunjit);
    matrix4 prevused = modelmatrix;
    if(velobjhasprev && !velcutframe)
    {
        matrix4 invroot;
        if(invroot.invert(velobjcurrroot))
        {
            matrix4 rel;
            rel.mul(invroot, world);
            matrix4 prevworld;
            prevworld.mul(velobjprevworld, rel);
            const matrix4 &pcamunjit = (hwrtvelhudpass && velprevavatarok) ? velprevavatarunjit : velprevunjit;
            prevmm.mul(pcamunjit, prevworld);
            const matrix4 &pcamused = (hwrtvelhudpass && velprevavatarok) ? velprevavatar : velprev;
            prevused.mul(pcamused, prevworld);
        }
        else prevmm = currunjit;
    }
    else prevmm = currunjit;
    GLOBALPARAM(prevmodelmatrix, prevmm);
    if(velobjidx >= 0 && velobjs.inrange(velobjidx) && !hwrtvelunlitmode && !hwrtvelidmode)
    {
        velobjhist &oh = velobjs[velobjidx];
        if(oh.haveclip)
        {
            oh.prevused = oh.curused;
            oh.prevunjit = oh.curunjit;
            oh.haveprevclip = true;
        }
        else
        {
            oh.prevused = prevused;
            oh.prevunjit = prevmm;
            oh.haveprevclip = velobjhasprev && !velcutframe;
        }
        oh.curused = modelmatrix;
        oh.curunjit = currunjit;
        oh.haveclip = true;
    }
    if(hwrtvelskelpass && velskelactive >= 0 && velskelactive < VEL_SKEL_MAX)
    {
        velskelhist &h = velskels[velskelactive];
            if(!hwrtvelunlitmode && !hwrtvelidmode)
            {
                h.curmm = modelmatrix;
                h.prevmm = prevused;
                h.curmmunjit = currunjit;
                h.prevmmunjit = prevmm;
            }
            else if(hwrtvelunlitmode == 2 || hwrtvelidmode == 2) GLOBALPARAM(modelmatrix, h.prevmm);
            else GLOBALPARAM(modelmatrix, h.curmm);
    }
    if(velobjidx == hwrtveldrive)
    {
        vellogworld = world;
        vellogprevworld = velobjhasprev ? velobjprevworld : world;
        velloghas = true;
        vellogidx = velobjidx;
    }
}

static void velobjbegin(int idx, int attr2, uchar type)
{
    velobjidx = idx;
    velobjgotroot = false;
    velobjhasprev = false;
    while(velobjs.length() <= idx) velobjs.add();
    velobjhist &h = velobjs[idx];
    h.seen = true;
    if(h.valid && h.type == type && (type != ET_MAPMODEL || h.attr2 == attr2))
    {
        velobjhasprev = true;
        velobjprevworld = h.world;
    }
}

static void velobjend()
{
    if(velobjidx < 0 || !velobjs.inrange(velobjidx) || !velobjgotroot)
    {
        velobjidx = -1;
        velobjgotroot = false;
        velobjhasprev = false;
        return;
    }
    velobjhist &h = velobjs[velobjidx];
    h.world = velobjcurrroot;
    const vector<extentity *> &ents = entities::getents();
    if(ents.inrange(velobjidx) && ents[velobjidx])
    {
        h.attr2 = ents[velobjidx]->attr2;
        h.type = ents[velobjidx]->type;
        h.valid = true;
    }
    else h.valid = false;
    velobjidx = -1;
    velobjgotroot = false;
    velobjhasprev = false;
}

static bool velobjkeep(const extentity &e)
{
    if(e.type == ET_MAPMODEL) return true;
    const char *name = entities::entmodel(e);
    return name && name[0];
}

static bool velentwant(const extentity &e)
{
    if(e.type == ET_MAPMODEL) return false;
    const char *name = entities::entmodel(e);
    if(!name || !name[0]) return false;
    const char *tn = entities::entname(e.type);
    if(tn && !strcmp(tn, "teleport")) return true;
    if(tn && (!strcmp(tn, "carrot") || !strcmp(tn, "respawnpoint"))) return true;
    return e.spawned() != 0;
}

static void velobjage()
{
    const vector<extentity *> &ents = entities::getents();
    while(velobjs.length() < ents.length()) velobjs.add();
    loopv(velobjs)
    {
        if(!ents.inrange(i) || !velobjkeep(*ents[i]) || !velobjs[i].seen)
        {
            velobjs[i].valid = false;
            velobjs[i].haveclip = false;
            velobjs[i].haveprevclip = false;
        }
        velobjs[i].seen = false;
    }
}

static bool velskelfpbody(dynent *d)
{
    extern int thirdperson;
    if(!d) return false;
    if(d->state == CS_DEAD || d->state == CS_SPECTATOR || d->state == CS_SPAWNING) return false;
    if(thirdperson) return false;
    if(d == player || (physent *)d == camera1) return true;
    dynent *hide = hwrtcameradynent();
    return hide && d == hide;
}

static bool velismrfixit(model *m)
{
    if(!m || !m->name) return false;
    return !strcmp(m->name, "mrfixit") || !strncmp(m->name, "mrfixit/", 8);
}

static int velattachkey(const char *tag)
{
    if(!tag || !tag[0]) return 0;
    uint h = 2166136261u;
    for(const char *s = tag; *s; s++) { h ^= (uchar)*s; h *= 16777619u; }
    int k = int(h & 0x7fffffff);
    return k ? k : 1;
}

static int velattachkind(const char *tag)
{
    if(!tag || !tag[0]) return VELATT_BODY;
    if(!strcmp(tag, "tag_shield")) return VELATT_ARMOUR;
    if(!strcmp(tag, "tag_weapon")) return VELATT_WEAPON;
    if(!strcmp(tag, "tag_powerup")) return VELATT_HORNS;
    return VELATT_ARMOUR;
}

static const char *velkindname(int k)
{
    switch(k)
    {
        case VELATT_ARMOUR: return "armour";
        case VELATT_WEAPON: return "weapon";
        case VELATT_HORNS: return "horns";
        case VELATT_HUDGUN: return "hudgun";
        default: return "body";
    }
}

static int velhudkey(const char *tag)
{
    uint h = 2166136261u ^ 0x48554447u;
    if(tag && tag[0])
        for(const char *s = tag; *s; s++) { h ^= (uchar)*s; h *= 16777619u; }
    int k = int(h & 0x7fffffff);
    return k ? k : 1;
}

static void velslotlog(const char *fmt, ...)
{
    if(!velslotlogf) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(velslotlogf, fmt, args);
    va_end(args);
    fflush(velslotlogf);
}

static const char *velfindname(int d)
{
    switch(d)
    {
        case VELFIND_MATCH: return "match";
        case VELFIND_EMPTY: return "empty";
        case VELFIND_GROW: return "grow";
        case VELFIND_RECYCLE: return "recycle_stale";
        case VELFIND_STEAL: return "steal_unnoted";
        default: return "fail";
    }
}

static int velslotlastframe(const velskelhist &h)
{
    int a = h.notedframe, b = h.committedframe;
    return a > b ? a : b;
}

// A slot whose instance has not been visited yet this frame is still live:
// bodies are noted during the colour pass, attachments during the velocity
// pass. "not noted yet" is not "gone". Recycle only after a full missed
// frame, and only after empty slots and unused capacity are exhausted.
static int velfindslot(dynent *d, int attachkey, bool create, int *decision = NULL)
{
    int slot = -1, empty = -1, aged = -1, stale = -1, staleage = -1;
    loopi(velskeln)
    {
        if(velskels[i].d == d && velskels[i].attachkey == attachkey) { slot = i; break; }
        if(empty < 0 && !velskels[i].d) empty = i;
        if(aged < 0 && velskels[i].d && !velskels[i].noted) aged = i;
        if(velskels[i].d)
        {
            int last = velslotlastframe(velskels[i]);
            if(last < velskelframe - 1)
            {
                int age = velskelframe - last;
                if(stale < 0 || age > staleage) { stale = i; staleage = age; }
            }
        }
    }
    auto decide = [&](int why, int s) -> int
    {
        if(decision) *decision = why;
        return s;
    };
    if(slot >= 0) return decide(VELFIND_MATCH, slot);
    if(!create) return decide(VELFIND_FAIL, -1);
    if(empty >= 0)
    {
        velslotlog("ALLOC empty slot %d uid %d key %d n %d/%d\n",
                   empty, d ? hwrtdynentuid(d) : 0, attachkey, velskeln, VEL_SKEL_MAX);
        return decide(VELFIND_EMPTY, empty);
    }
    if(hwrtvelslotsteal)
    {
        // Original lot-4 policy: treat any not-yet-noted occupant as aged and
        // evict it before growing. Kept only to photograph the bug.
        if(aged >= 0)
        {
            velskelhist &v = velskels[aged];
            velslotlog("ALLOC steal_unnoted slot %d victim uid %d kind %s key %d noted %s last %d usedprev %s validprev %s model %s (need uid %d key %d, n %d/%d)\n",
                       aged, v.uid, velkindname(v.attachkind), v.attachkey,
                       v.noted ? "yes" : "no", velslotlastframe(v),
                       v.usedprev ? "yes" : "no", v.validprev ? "yes" : "no",
                       v.m && v.m->name ? v.m->name : "?",
                       d ? hwrtdynentuid(d) : 0, attachkey, velskeln, VEL_SKEL_MAX);
            velskelfree(v);
            return decide(VELFIND_STEAL, aged);
        }
        if(velskeln >= VEL_SKEL_MAX) return decide(VELFIND_FAIL, -1);
        int s = velskeln++;
        velslotlog("ALLOC grow slot %d uid %d key %d n %d/%d (steal-policy, no unnoted victim)\n",
                   s, d ? hwrtdynentuid(d) : 0, attachkey, velskeln, VEL_SKEL_MAX);
        return decide(VELFIND_GROW, s);
    }
    if(velskeln < VEL_SKEL_MAX)
    {
        int s = velskeln++;
        velslotlog("ALLOC grow slot %d uid %d key %d n %d/%d\n",
                   s, d ? hwrtdynentuid(d) : 0, attachkey, velskeln, VEL_SKEL_MAX);
        return decide(VELFIND_GROW, s);
    }
    if(stale >= 0)
    {
        velskelhist &v = velskels[stale];
        velslotlog("ALLOC recycle_stale slot %d victim uid %d kind %s key %d last %d (need uid %d key %d, n %d/%d)\n",
                   stale, v.uid, velkindname(v.attachkind), v.attachkey, velslotlastframe(v),
                   d ? hwrtdynentuid(d) : 0, attachkey, velskeln, VEL_SKEL_MAX);
        velskelfree(v);
        return decide(VELFIND_RECYCLE, stale);
    }
    velslotlog("ALLOC fail uid %d key %d n %d/%d (no empty, at cap, no stale)\n",
               d ? hwrtdynentuid(d) : 0, attachkey, velskeln, VEL_SKEL_MAX);
    return decide(VELFIND_FAIL, -1);
}

bool hwrtvelequipok(dynent *d, model *parent, model *equip)
{
    if(!hwrtvelequip) return false;
    if(!d || !equip) return false;
    if(d->type != ENT_PLAYER || d->ragdoll) return false;
    if(velskelfpbody(d)) return false;
    if(!velismrfixit(parent)) return false;
    return true;
}

int hwrtvelskelattachregion()
{
    if(velskelactive < 0 || velskelactive >= VEL_SKEL_MAX) return -1;
    switch(velskels[velskelactive].attachkind)
    {
        case VELATT_ARMOUR: return 5;
        case VELATT_WEAPON: return 6;
        case VELATT_HORNS: return 7;
        case VELATT_HUDGUN:
            return velskels[velskelactive].attachkey == velhudkey(NULL) ? 8 : 9;
        default: return -1;
    }
}

struct velequipsnap
{
    int slot;
    bool gotroot, hasprev;
    matrix4 prevworld, currroot;
    const dualquat *prevb;
    int prevn;
};
static velequipsnap velequipsaved;

int hwrtvelskelequip(dynent *d, model *parent, model *m, const char *tag, int anim, int basetime)
{
    if(!d && hwrtvelhudpass) d = velhudowner;
    if(!hwrtvelskelpass || !d || !m) return -1;
    int key = 0, kind = 0;
    if(hwrtvelhudpass)
    {
        if(!hwrtvelhud) return -1;
        key = velhudkey(tag);
        kind = VELATT_HUDGUN;
    }
    else
    {
        if(!hwrtvelequipok(d, parent, m)) return -1;
        key = velattachkey(tag);
        if(!key) return -1;
        kind = velattachkind(tag);
    }
    int parentslot = velskelactive;
    int seq = hwrtdynentseq(d);
    int uid = hwrtdynentuid(d);
    int why = VELFIND_FAIL;
    int slot = velfindslot(d, key, true, &why);
    if(slot < 0) return -1;
    velskelhist &h = velskels[slot];
    if(h.d && (h.d != d || h.attachkey != key)) velskelfree(h);
    if(h.m && h.m != m) h.validprev = false;
    if(h.seq != seq || h.uid != uid) h.validprev = false;
    h.d = d;
    h.m = m;
    h.anim = anim;
    h.basetime = basetime;
    h.basetime2 = 0;
    h.seq = seq;
    h.uid = uid;
    h.attachkey = key;
    h.attachkind = kind;
    h.natt = 0;
    if(parentslot >= 0 && parentslot < velskeln)
    {
        velskelhist &p = velskels[parentslot];
        h.pos = p.pos;
        h.yaw = p.yaw;
        h.pitch = p.pitch;
        if(h.validprev && h.o.dist(p.pos) > 24) h.validprev = false;
    }
    h.notedframe = velskelframe;
    h.noted = true;
    bool haveprev = h.validprev && !velcutframe && (!hwrtvelhudpass || velprevavatarok);
    if((hwrtvelunlitmode == 2 || hwrtvelidmode == 2) && !haveprev) return -1;
    velequipsaved.slot = parentslot;
    velequipsaved.gotroot = velobjgotroot;
    velequipsaved.hasprev = velobjhasprev;
    velequipsaved.prevworld = velobjprevworld;
    velequipsaved.currroot = velobjcurrroot;
    velequipsaved.prevb = velskelprevb;
    velequipsaved.prevn = velskelprevn;
    velskelactive = slot;
    velobjgotroot = false;
    velobjhasprev = haveprev;
    if(!hwrtvelunlitmode && !hwrtvelidmode) h.usedprev = velobjhasprev;
    if(!hwrtvelunlitmode && !hwrtvelidmode)
        velslotlog("EQUIP frame %d slot %d uid %d kind %s tag %s model %s alloc %s usedprev %s last %d parent %d drawrev %d stealpolicy %d\n",
                   velskelframe, slot, uid, velkindname(kind), tag ? tag : "", m->name ? m->name : "?",
                   velfindname(why), h.usedprev ? "yes" : "no", velslotlastframe(h),
                   parentslot, int(hwrtveldrawrev), int(hwrtvelslotsteal));
    velobjprevworld = h.world;
    if(!hwrtvelunlitmode && !hwrtvelidmode) velcopyskelprevdraw(h);
    velbindskelprevdraw(h);
    return slot;
}

void hwrtvelskelrestoreslot(int slot)
{
    if(slot < 0 || slot >= velskeln)
    {
        velskelactive = -1;
        return;
    }
    velskelactive = slot;
    velobjgotroot = velequipsaved.gotroot;
    velobjhasprev = velequipsaved.hasprev;
    velobjprevworld = velequipsaved.prevworld;
    velobjcurrroot = velequipsaved.currroot;
    velskelprevb = velequipsaved.prevb;
    velskelprevn = velequipsaved.prevn;
}

void hwrtvelnotechar(dynent *d, model *m, int anim, int basetime, int basetime2, const vec &pos, float yaw, float pitch, modelattach *a)
{
    if(!velwanted() || !d || !m) return;
    if(!hwrtvelmarking() || shadowmapping) return;
    if(d->type != ENT_PLAYER || d->ragdoll) return;
    if(!m->skeletal()) return;
    if(velskelfpbody(d)) return;
    if(anim & ANIM_NORENDER) return;

    int seq = hwrtdynentseq(d);
    int uid = hwrtdynentuid(d);
    int slot = velfindslot(d, 0, true);
    if(slot < 0) return;
    velskelhist &h = velskels[slot];
    if(h.d && (h.d != d || h.attachkey != 0)) velskelfree(h);
    if(h.m && h.m != m) h.validprev = false;
    if(h.seq != seq || h.uid != uid) h.validprev = false;
    if(h.validprev && h.o.dist(pos) > 24) h.validprev = false;
    h.d = d;
    h.m = m;
    h.anim = anim;
    h.basetime = basetime;
    h.basetime2 = basetime2;
    h.pos = pos;
    h.yaw = yaw;
    h.pitch = pitch;
    h.seq = seq;
    h.uid = uid;
    h.attachkey = 0;
    h.attachkind = VELATT_BODY;
    h.notedframe = velskelframe;
    h.noted = true;
    h.natt = 0;
    if(a)
    {
        for(int i = 0; a[i].tag && h.natt < VEL_ATT_MAX; i++)
        {
            if(!a[i].name || !a[i].name[0]) continue;
            copystring(h.atts[h.natt].tag, a[i].tag);
            copystring(h.atts[h.natt].name, a[i].name);
            h.atts[h.natt].anim = a[i].anim;
            h.atts[h.natt].basetime = a[i].basetime;
            h.natt++;
        }
    }
}

void hwrtvelstarthud(dynent *d, model *m, int anim, int basetime, int basetime2, const vec &pos, float yaw, float pitch)
{
    if(!hwrtvelskelpass || !hwrtvelhudpass || !hwrtvelhud) return;
    if(!d || !m || !m->skeletal()) return;
    if(isthirdperson()) return;
    if(anim & ANIM_NORENDER) return;

    int seq = hwrtdynentseq(d);
    int uid = hwrtdynentuid(d);
    int key = velhudkey(NULL);
    int why = VELFIND_FAIL;
    int slot = velfindslot(d, key, true, &why);
    if(slot < 0) return;
    velskelhist &h = velskels[slot];
    if(h.d && (h.d != d || h.attachkey != key)) velskelfree(h);
    if(h.m && h.m != m) h.validprev = false;
    if(h.seq != seq || h.uid != uid) h.validprev = false;
    if(h.validprev && h.o.dist(pos) > 24) h.validprev = false;
    velhudowner = d;
    h.d = d;
    h.m = m;
    h.anim = anim;
    h.basetime = basetime;
    h.basetime2 = basetime2;
    h.pos = pos;
    h.yaw = yaw;
    h.pitch = pitch;
    h.seq = seq;
    h.uid = uid;
    h.attachkey = key;
    h.attachkind = VELATT_HUDGUN;
    h.notedframe = velskelframe;
    h.noted = true;
    h.natt = 0;
    velobjidx = -1;
    velobjgotroot = false;
    velobjhasprev = h.validprev && !velcutframe && velprevavatarok;
    if(!hwrtvelunlitmode && !hwrtvelidmode) h.usedprev = velobjhasprev;
    velobjprevworld = h.world;
    velskelactive = slot;
    if(!hwrtvelunlitmode && !hwrtvelidmode)
    {
        velcopyskelprevdraw(h);
        velslotlog("HUD frame %d slot %d uid %d model %s alloc %s usedprev %s last %d fov %.1f\n",
                   velskelframe, slot, uid, m->name ? m->name : "?",
                   velfindname(why), h.usedprev ? "yes" : "no", velslotlastframe(h),
                   curavatarfov);
    }
    velbindskelprevdraw(h);
}

bool hwrtvelskelprevbones(const dualquat *&bones, int &nbones)
{
    bones = velskelprevb;
    nbones = velskelprevn;
    return velskelprevb && velskelprevn > 0 && velskelactive >= 0 && velskels[velskelactive].validprev && !velcutframe && (!hwrtvelhudpass || velprevavatarok);
}

void hwrtvelskelkeep(dynent *d, model *m, const dualquat *bones, int nbones, int nverts, int vweights, int vblends, bool gpuskin)
{
    if(!d) d = velhudowner;
    if(!d || velskelactive < 0) return;
    if(nbones > 0 && !bones) return;
    velskelhist &h = velskels[velskelactive];
    if(h.committedframe == velskelframe) return;
    if(h.d != d) return;
    if(h.m && h.m != m) h.validprev = false;
    h.m = m;
    if(h.nverts && h.nverts != nverts) h.validprev = false;
    if(nbones < 1)
    {
        if(h.nbones)
        {
            DELETEA(h.bones);
            h.nbones = 0;
            h.validprev = false;
        }
        h.nverts = nverts;
        h.vweights = 0;
        h.vblends = 0;
        h.gpuskin = false;
        if(velobjgotroot) h.world = velobjcurrroot;
        h.prevpos = h.o;
        h.o = h.pos;
        h.committedframe = velskelframe;
        if(h.attachkind == VELATT_HUDGUN) velhuddrawn++;
        else if(h.attachkind) velequipdrawn++;
        hwrtvelskelmem = velskelmembytes();
        return;
    }
    if(h.nbones && h.nbones != nbones)
    {
        DELETEA(h.bones);
        h.validprev = false;
    }
    if(!h.bones || h.nbones != nbones)
    {
        DELETEA(h.bones);
        h.bones = new dualquat[nbones];
        h.nbones = nbones;
    }
    memcpy(h.bones, bones, nbones * sizeof(dualquat));
    h.nverts = nverts;
    h.vweights = vweights;
    h.vblends = vblends;
    h.gpuskin = gpuskin;
    if(velobjgotroot) h.world = velobjcurrroot;
    h.prevpos = h.o;
    h.o = h.pos;
    h.committedframe = velskelframe;
    if(h.attachkind == VELATT_HUDGUN) velhuddrawn++;
    else if(h.attachkind) velequipdrawn++;
    hwrtvelskelmem = velskelmembytes();
}

static bool velshotkindproof(int k)
{
    return k == VELSHOT_PROOF || k == VELSHOT_PROOFALIVE;
}

static bool velwantproof()
{
    return velshotcount > 0 && velshotkindproof(velshotq[velshothead].kind);
}

bool hwrtvelskelwanttrack()
{
    return veltrackon && hwrtvelskelpass && !hwrtvelunlitmode && !hwrtvelidmode;
}

int hwrtvelskelactiveslot()
{
    return velskelactive;
}

int hwrtvelskelregion(const char *n)
{
    if(!n || !n[0]) return -1;
    if(!strcmp(n, "Torso") || !strcmp(n, "Hips") || !strcmp(n, "Spine1") || !strcmp(n, "Spine2") || !strcmp(n, "SpineTwist")) return 0;
    if(!strcmp(n, "UpperArm.L") || !strcmp(n, "LowerArm.L")) return 1;
    if(!strcmp(n, "UpperArm.R") || !strcmp(n, "LowerArm.R")) return 2;
    if(!strcmp(n, "Thigh.L") || !strcmp(n, "Shin.L")) return 3;
    if(!strcmp(n, "Thigh.R") || !strcmp(n, "Shin.R")) return 4;
    return -1;
}

float hwrtvelskelscreenscore(const vec &a, const vec &b, const vec &c)
{
    if(velskelactive < 0 || velskelactive >= VEL_SKEL_MAX) return 0;
    const matrix4 &mm = velskels[velskelactive].curmm;
    vec4 pa, pb, pc;
    mm.transform(vec4(a.x, a.y, a.z, 1), pa);
    mm.transform(vec4(b.x, b.y, b.z, 1), pb);
    mm.transform(vec4(c.x, c.y, c.z, 1), pc);
    if(pa.w <= 1e-4f || pb.w <= 1e-4f || pc.w <= 1e-4f) return 0;
    float ax = pa.x / pa.w, ay = pa.y / pa.w;
    float bx = pb.x / pb.w, by = pb.y / pb.w;
    float cx = pc.x / pc.w, cy = pc.y / pc.w;
    float cross = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    float ndc = 0.5f * fabs(cross);
    if(ndc < 1e-10f) return 0;
    return ndc * (velw * 0.5f) * (velh * 0.5f);
}

void hwrtvelskeltrack(int region, int mesh, int tri, const vec &cur, const vec &prev, float score, float area)
{
    if(!veltrackon || region < 0 || region >= VELTR_N) return;
    if(velskelactive < 0 || velskelactive >= VEL_SKEL_MAX) return;
    if(veltrackcands.length() >= VELTR_CAND_MAX) return;
    veltrack &t = veltrackcands.add();
    t.slot = velskelactive;
    t.region = region;
    t.mesh = mesh;
    t.tri = tri;
    t.cur = cur;
    t.prev = prev;
    t.score = score;
    t.area = area;
}

static void veltrackreset()
{
    veltrackcands.setsize(0);
    veltrackn = 0;
}

static bool velclippixel(const matrix4 &mm, const vec &pos, int w, int h, float &px, float &py, float &pw, float *pz = NULL)
{
    vec4 clip;
    mm.transform(vec4(pos.x, pos.y, pos.z, 1), clip);
    pw = clip.w;
    px = 0;
    py = 0;
    if(pz) *pz = 1;
    if(clip.w <= 1e-5f) return false;
    px = (clip.x / clip.w * 0.5f + 0.5f) * w;
    py = (clip.y / clip.w * 0.5f + 0.5f) * h;
    if(pz) *pz = clip.z / clip.w * 0.5f + 0.5f;
    return px >= 0 && py >= 0 && px < w && py < h;
}

static void velidpixel(const float *idpix, int w, int h, int x, int y, int &slot, int &mesh, int &tri, float &z);
static bool velidfind(const float *idpix, int w, int h, float px, float py, int slot, int mesh, int tri, int radius, int &ox, int &oy, float &z, float &off);

static void veltrackfinalize(const float *idpix, int idw, int idh)
{
    veltrackn = 0;
    if(veltrackcands.empty()) return;
    hashtable<int, int> visn;
    if(idpix && idw > 0 && idh > 0)
    {
        for(int y = 0; y < idh; y += 2) for(int x = 0; x < idw; x += 2)
        {
            int s, m, t;
            float z;
            velidpixel(idpix, idw, idh, x, y, s, m, t, z);
            if(s < 0 || s >= velskeln || m < 0 || t < 0) continue;
            if(velskels[s].attachkind == VELATT_BODY) continue;
            int key = ((s & 255) << 24) | ((m & 255) << 16) | (t & 65535);
            visn.access(key, 0)++;
        }
    }
    loopi(velskeln) loopj(VELTR_N)
    {
        float keptx[VELTR_PER_REG], kepty[VELTR_PER_REG];
        int nk = 0;
        for(;;)
        {
            int best = -1;
            float bests = -1;
            for(int ci = 0; ci < veltrackcands.length(); ci++)
            {
                veltrack &c = veltrackcands[ci];
                if(c.slot != i || c.region != j) continue;
                float s = c.score * (c.area + 1e-4f);
                if(idpix && idw > 0 && idh > 0 && velskels[i].noted)
                {
                    int key = ((c.slot & 255) << 24) | ((c.mesh & 255) << 16) | (c.tri & 65535);
                    int *pv = visn.access(key);
                    if(pv && *pv > 0) s += 1e6f + float(*pv);
                    else
                    {
                        float px = 0, py = 0, pw = 0;
                        if(velclippixel(velskels[i].curmm, c.cur, idw, idh, px, py, pw))
                        {
                            int ox, oy;
                            float z, off;
                            if(velidfind(idpix, idw, idh, px, py, c.slot, c.mesh, c.tri, 2, ox, oy, z, off))
                                s += 1e6f;
                        }
                    }
                }
                if(s > bests) { bests = s; best = ci; }
            }
            if(best < 0) break;
            veltrack chosen = veltrackcands[best];
            veltrackcands[best].slot = -1;
            float cx = 0, cy = 0, cw = 0;
            bool onscreen = velskels[i].noted && velclippixel(velskels[i].curmm, chosen.cur, velw, velh, cx, cy, cw);
            if(!onscreen && nk >= 2) continue;
            bool farenough = true;
            float mind2 = j >= 5 ? 9.0f : 36.0f;
            if(nk >= 2 && onscreen)
            {
                loopk(nk)
                {
                    float dx = cx - keptx[k], dy = cy - kepty[k];
                    if(dx*dx + dy*dy < mind2) { farenough = false; break; }
                }
            }
            if(!farenough) continue;
            if(veltrackn >= VELTR_KEEP_MAX) return;
            veltracks[veltrackn++] = chosen;
            if(nk < VELTR_PER_REG)
            {
                keptx[nk] = cx;
                kepty[nk] = cy;
                nk++;
            }
            if(nk >= VELTR_PER_REG) break;
        }
    }
}

static void velskelage()
{
    loopi(velskeln)
    {
        velskelhist &h = velskels[i];
        if(!h.noted || h.notedframe != velskelframe)
        {
            if(h.d && velslotlogf)
                velslotlog("FREE frame %d slot %d uid %d kind %s last %d (not visited this image)\n",
                           velskelframe, i, h.uid, velkindname(h.attachkind), velslotlastframe(h));
            // Real disappearance: drop the occupant so the slot is empty next
            // image. A later reintroduction must not resume this pose.
            velskelfree(h);
        }
    }
}

static void velslotdumpframe()
{
    if(!velslotlogf) return;
    velslotlog("FRAME %d millis %d drawrev %d stealpolicy %d bodies %d equip %d n %d watch %d\n",
               velskelframe, lastmillis, int(hwrtveldrawrev), int(hwrtvelslotsteal),
               velskeldrawn, velequipdrawn, velskeln, velwatchn);
    loopi(velskeln)
    {
        velskelhist &h = velskels[i];
        if(!h.d) continue;
        velslotlog("  slot %d uid %d kind %s key %d model %s noted %s notedframe %d committed %d usedprev %s validprev %s\n",
                   i, h.uid, velkindname(h.attachkind), h.attachkey,
                   h.m && h.m->name ? h.m->name : "?",
                   h.noted && h.notedframe==velskelframe ? "yes" : "no",
                   h.notedframe, h.committedframe,
                   h.usedprev ? "yes" : "no", h.validprev ? "yes" : "no");
    }
    loopj(velwatchn)
    {
        velslotwatch &w = velwatches[j];
        int found = -1;
        loopi(velskeln)
        {
            velskelhist &h = velskels[i];
            if(h.d && h.uid == w.uid && h.attachkind == w.kind && h.attachkey == w.key) { found = i; break; }
        }
        if(found < 0)
        {
            velslotlog("  WATCH_MISS uid %d kind %s key %d (was slot %d)\n",
                       w.uid, velkindname(w.kind), w.key, w.slot);
            continue;
        }
        velskelhist &h = velskels[found];
        const char *verdict = "keep";
        if(found != w.slot) verdict = "moved";
        if(!h.usedprev && w.usedprev) verdict = "LOST_PREV";
        if(!h.noted || h.notedframe != velskelframe) verdict = "UNNOTED";
        velslotlog("  WATCH uid %d kind %s slot %d->%d usedprev %s->%s committed %d->%d %s\n",
                   w.uid, velkindname(w.kind), w.slot, found,
                   w.usedprev ? "yes" : "no", h.usedprev ? "yes" : "no",
                   w.committed, h.committedframe, verdict);
        w.slot = found;
        w.usedprev = h.usedprev;
        w.validprev = h.validprev;
        w.committed = h.committedframe;
        w.notedframe = h.notedframe;
    }
    velslotlogn++;
}

static bool velneeddiaghist()
{
    loopi(velshotcount)
        if(velshotkindproof(velshotq[(velshothead + i) % VEL_SHOTQ].kind))
            return true;
    return false;
}

void hwrtvelstencilsnap(const char *tag)
{
    if(!tag || !tag[0]) return;
    if(velshotcount <= 0 || !velshotkindproof(velshotq[velshothead].kind)) return;
    if(velstsnapn >= VELSTSNAP_MAX) return;
    int w = screenw, h = screenh;
    if(w < 8 || h < 8) return;

    velstsnap &s = velstsnaps[velstsnapn++];
    memset(&s, 0, sizeof(s));
    strncpy(s.tag, tag, sizeof(s.tag) - 1);
    s.tag[sizeof(s.tag) - 1] = 0;

    GLint fb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
    s.fb = int(fb);
    s.sten_on = glIsEnabled(GL_STENCIL_TEST) ? 1 : 0;
    glGetIntegerv(GL_STENCIL_FUNC, &s.func);
    glGetIntegerv(GL_STENCIL_REF, &s.ref);
    glGetIntegerv(GL_STENCIL_VALUE_MASK, &s.mask);
    glGetIntegerv(GL_STENCIL_WRITEMASK, &s.wmask);
    glGetIntegerv(GL_STENCIL_FAIL, &s.fail);
    glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL, &s.zfail);
    glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &s.zpass);
    s.velmode = velstmode;
    s.velactive = velstactive ? 1 : 0;
    s.coverpending = velcoverpending;
    s.shadowmapping = shadowmapping ? 1 : 0;
    s.glaring = glaring ? 1 : 0;
    s.drawtex = drawtex;

    int expect_on = (velstmode == VELST_KEEP || velstmode == VELST_OFF || !velstactive) ? 0 : 1;
    int expect_ref = 0;
    if(velstmode == VELST_MARK) expect_ref = VEL_ST_COVER;
    else if(velstmode == VELST_UNMARK) expect_ref = VEL_ST_EXCLUDE;
    else if(velstmode == VELST_SKEL) expect_ref = VEL_ST_SKEL;
    s.gl_matches_engine = (s.sten_on == expect_on) ? 1 : 0;
    if(expect_on)
    {
        if(s.func != int(GL_ALWAYS) || s.ref != expect_ref || s.zpass != int(GL_REPLACE))
            s.gl_matches_engine = 0;
    }

    uchar *pix = new uchar[w * h];
    GLint oldalign = 1;
    glGetIntegerv(GL_PACK_ALIGNMENT, &oldalign);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, pix);
    glPixelStorei(GL_PACK_ALIGNMENT, oldalign);
    const int step = 2;
    for(int y = 0; y < h; y += step) for(int x = 0; x < w; x += step)
    {
        int v = pix[y * w + x];
        s.nread++;
        if(v == VEL_ST_EMPTY) s.n0++;
        else if(v == VEL_ST_COVER) s.n1++;
        else if(v == VEL_ST_EXCLUDE) s.n2++;
        else if(v == VEL_ST_SKEL) s.n3++;
        else if(v == VEL_ST_HUDCOL) s.n4++;
        else s.nother++;
    }
    delete[] pix;
}

static void velblitds(GLuint srcfb, GLuint dstfb)
{
    if(!srcfb || !dstfb || !glBlitFramebuffer_) return;
    GLint prevdraw = 0, prevread = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevdraw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevread);
    glBindFramebuffer_(GL_READ_FRAMEBUFFER, srcfb);
    glBindFramebuffer_(GL_DRAW_FRAMEBUFFER, dstfb);
    glBlitFramebuffer_(0, 0, velw, velh, 0, 0, velw, velh, GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer_(GL_READ_FRAMEBUFFER, prevread);
    glBindFramebuffer_(GL_DRAW_FRAMEBUFFER, prevdraw);
}

static bool velmakedsfb(GLuint &fb, GLuint &ds, GLuint &col)
{
    makecolourtex(col, velw, velh, GL_RGB, 0);
    if(!ds) glGenRenderbuffers_(1, &ds);
    glBindRenderbuffer_(GL_RENDERBUFFER, ds);
    glRenderbufferStorage_(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, velw, velh);
    glBindRenderbuffer_(GL_RENDERBUFFER, 0);
    if(!fb) glGenFramebuffers_(1, &fb);
    glBindFramebuffer_(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, col, 0);
    glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, ds);
    GLenum st = glCheckFramebufferStatus_(GL_FRAMEBUFFER);
    if(st != GL_FRAMEBUFFER_COMPLETE)
    {
        glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
        glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, ds);
        st = glCheckFramebufferStatus_(GL_FRAMEBUFFER);
    }
    glBindFramebuffer_(GL_FRAMEBUFFER, 0);
    return st == GL_FRAMEBUFFER_COMPLETE;
}

static bool velensurediag()
{
    Shader *u = useshaderbyname("hwrtvelunlit");
    Shader *ids = useshaderbyname("hwrtvelid");
    if(!u || u->invalid() || velw < 8 || velh < 8) return false;
    bool idshader = ids && !ids->invalid();
    if(velunlittex[0] && veldiagfb && velunlitok && veldiagw == velw && veldiagh == velh && (!idshader || velidok))
        return velunlitok;
    loopi(2) makecolourtex(velunlittex[i], velw, velh, GL_RGB, 1);
    loopi(2) makecolourtex(velidtex[i], velw, velh, GL_RGBA32F, 0);
    if(!veldiagds) glGenRenderbuffers_(1, &veldiagds);
    glBindRenderbuffer_(GL_RENDERBUFFER, veldiagds);
    glRenderbufferStorage_(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, velw, velh);
    glBindRenderbuffer_(GL_RENDERBUFFER, 0);
    if(!veldiagfb) glGenFramebuffers_(1, &veldiagfb);
    glBindFramebuffer_(GL_FRAMEBUFFER, veldiagfb);
    glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, velunlittex[0], 0);
    glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, veldiagds);
    GLenum st = glCheckFramebufferStatus_(GL_FRAMEBUFFER);
    if(st != GL_FRAMEBUFFER_COMPLETE)
    {
        glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
        glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, veldiagds);
        st = glCheckFramebufferStatus_(GL_FRAMEBUFFER);
    }
    velunlitok = st == GL_FRAMEBUFFER_COMPLETE;
    velidok = velunlitok && idshader;
    loopi(2) velmakedsfb(velscenefb[i], velsceneds[i], velscenecol[i]);
    veldiagw = velw;
    veldiagh = velh;
    glBindFramebuffer_(GL_FRAMEBUFFER, 0);
    if(!velunlitok) conoutf(CON_WARN, "hwrt velocity: diagnostic FBO incomplete, unlit/id skipped");
    else if(!idshader) conoutf(CON_WARN, "hwrt velocity: hwrtvelid shader missing, identity tracks skipped");
    return velunlitok;
}

static void velbinddiag(GLuint colortex, GLuint scenefb)
{
    glBindFramebuffer_(GL_FRAMEBUFFER, veldiagfb);
    glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colortex, 0);
    if(scenefb) velblitds(scenefb, veldiagfb);
    glViewport(0, 0, velw, velh);
    GLfloat oldclear[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, oldclear);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(oldclear[0], oldclear[1], oldclear[2], oldclear[3]);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.0f, -1.0f);
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0);
    glStencilFunc(GL_NOTEQUAL, VEL_ST_EXCLUDE, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

static void velcopyskelprevdraw(velskelhist &h)
{
    if(!(h.validprev && h.bones && h.nbones > 0 && !velcutframe))
    {
        DELETEA(h.prevdraw);
        h.nprevdraw = 0;
        return;
    }
    if(!h.prevdraw || h.nprevdraw != h.nbones)
    {
        DELETEA(h.prevdraw);
        h.prevdraw = new dualquat[h.nbones];
        h.nprevdraw = h.nbones;
    }
    memcpy(h.prevdraw, h.bones, h.nbones * sizeof(dualquat));
}

static void velbindskelprevdraw(velskelhist &h)
{
    velskelprevb = NULL;
    velskelprevn = 0;
    if(h.prevdraw && h.nprevdraw > 0 && h.validprev && !velcutframe)
    {
        velskelprevb = h.prevdraw;
        velskelprevn = h.nprevdraw;
    }
}

static modelattach velattstack[VEL_ATT_MAX+1];

static modelattach *velattsfor(velskelhist &h)
{
    memset(velattstack, 0, sizeof(velattstack));
    int n = 0;
    if(h.natt > 0)
    {
        loopi(h.natt)
        {
            if(!h.atts[i].tag[0] || !h.atts[i].name[0]) continue;
            velattstack[n] = modelattach(h.atts[i].tag, h.atts[i].name, h.atts[i].anim, h.atts[i].basetime);
            velattstack[n].m = loadmodel(h.atts[i].name);
            n++;
            if(n >= VEL_ATT_MAX) break;
        }
        return n > 0 ? velattstack : NULL;
    }
    if(hwrtdynentattach(h.d, velattstack, VEL_ATT_MAX+1) > 0) return velattstack;
    return NULL;
}

static void velrenderskel(velskelhist &h, int which)
{
    velobjidx = -1;
    velobjgotroot = false;
    velobjhasprev = h.validprev && !velcutframe && (!hwrtvelhudpass || velprevavatarok);
    velobjprevworld = h.world;
    vec pos = (which == 1 && h.validprev) ? h.prevpos : h.pos;
    dynent *rd = h.attachkind == VELATT_HUDGUN ? NULL : h.d;
    h.m->startrender();
    h.m->render(h.anim, h.basetime, h.basetime2, pos, h.yaw, h.pitch, rd, velattsfor(h));
    h.m->endrender();
}

static void velsetskelgl()
{
    glBindFramebuffer_(GL_FRAMEBUFFER, velfb);
    glViewport(0, 0, velw, velh);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.0f, -1.0f);
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0);
    glStencilFunc(GL_NOTEQUAL, VEL_ST_EXCLUDE, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_CULL_FACE);
}

static void veldrawdiagpass(int which, bool idpass)
{
    GLuint color = idpass ? velidtex[which] : velunlittex[which];
    GLuint scene = 0;
    if(which == 0) scene = velscenefb[velsceneslot];
    else if(velscenehasprev) scene = velscenefb[velsceneslot ^ 1];
    else return;
    velbinddiag(color, scene);
    if(idpass) hwrtvelidmode = which == 0 ? 1 : 2;
    else hwrtvelunlitmode = which == 0 ? 1 : 2;
    loopi(velskeln)
    {
        velskelhist &h = velskels[i];
        if(!h.noted || h.notedframe != velskelframe || !h.d || !h.m) continue;
        if(h.attachkey) continue;
        if(h.d->ragdoll || velskelfpbody(h.d)) continue;
        if(which == 1 && !h.validprev) continue;
        velskelactive = i;
        velbindskelprevdraw(h);
        velrenderskel(h, which);
    }
    hwrtvelunlitmode = 0;
    hwrtvelidmode = 0;
    velskelactive = -1;
    velskelprevb = NULL;
    velskelprevn = 0;
}

static void veldrawskel()
{
    if(!camera1 || !veltex) return;
    velskeldrawn = 0;
    velequipdrawn = 0;
    hwrtvelskelcpu = 0;
    veltrackreset();
    veltrackon = velwantproof();
    bool wantdiag = veltrackon && velensurediag();

    velsetskelgl();
    hwrtvelskelpass = true;
    hwrtvelunlitmode = 0;
    hwrtvelidmode = 0;
    Uint64 t0 = SDL_GetPerformanceCounter();
    int nbody = velskeln;
    loopj(nbody)
    {
        int i = hwrtveldrawrev ? nbody - 1 - j : j;
        velskelhist &h = velskels[i];
        if(!h.noted || h.notedframe != velskelframe || !h.d || !h.m) continue;
        if(h.attachkey) continue;
        if(h.d->ragdoll || velskelfpbody(h.d)) continue;
        velskelactive = i;
        velskelprevb = NULL;
        velskelprevn = 0;
        h.usedprev = h.validprev && !velcutframe;
        velcopyskelprevdraw(h);
        if(h.validprev && h.bones && h.nbones > 0 && !velcutframe)
        {
            if(velskelprevstoremax < h.nbones)
            {
                DELETEA(velskelprevstore);
                velskelprevstore = new dualquat[h.nbones];
                velskelprevstoremax = h.nbones;
            }
            memcpy(velskelprevstore, h.bones, h.nbones * sizeof(dualquat));
            velskelprevb = velskelprevstore;
            velskelprevn = h.nbones;
        }
        velsetskelgl();
        velrenderskel(h, 0);
        loopk(velskeln)
            if(velskels[k].d == h.d && velskels[k].committedframe == velskelframe)
                velskels[k].validprev = true;
        velskeldrawn++;
    }
    if(wantdiag)
    {
        veldrawdiagpass(0, false);
        veldrawdiagpass(1, false);
        if(velidok)
        {
            veldrawdiagpass(0, true);
            veldrawdiagpass(1, true);
        }
        velsetskelgl();
    }
    Uint64 freq = SDL_GetPerformanceFrequency();
    if(freq) hwrtvelskelcpu = float((SDL_GetPerformanceCounter() - t0) * 1000.0 / double(freq));
    velskelactive = -1;
    velskelprevb = NULL;
    velskelprevn = 0;
    hwrtvelskelpass = false;
    hwrtvelunlitmode = 0;
    hwrtvelidmode = 0;
}

static void velsethudgl()
{
    glBindFramebuffer_(GL_FRAMEBUFFER, velfb);
    glViewport(0, 0, velw, velh);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(0, 0);
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0);
    glStencilFunc(GL_ALWAYS, VEL_ST_SKEL, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_CULL_FACE);
}

static void veldrawhuddiag(int which, bool idpass)
{
    int rootkey = velhudkey(NULL);
    GLuint color = idpass ? velidtex[which] : velunlittex[which];
    GLuint scene = 0;
    if(which == 0) scene = velscenefb[velsceneslot];
    else if(velscenehasprev) scene = velscenefb[velsceneslot ^ 1];
    else return;
    velbinddiag(color, scene);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(0, 0);
    glStencilFunc(GL_ALWAYS, 0, 0xFF);
    if(idpass) hwrtvelidmode = which == 0 ? 1 : 2;
    else hwrtvelunlitmode = which == 0 ? 1 : 2;
    hwrtvelhudpass = true;
    loopi(velskeln)
    {
        velskelhist &h = velskels[i];
        if(!h.noted || h.notedframe != velskelframe || !h.d || !h.m) continue;
        if(h.attachkind != VELATT_HUDGUN || h.attachkey != rootkey) continue;
        if(which == 1 && (!h.validprev || !velprevavatarok)) continue;
        velskelactive = i;
        velbindskelprevdraw(h);
        velrenderskel(h, which);
    }
    hwrtvelhudpass = false;
    hwrtvelunlitmode = 0;
    hwrtvelidmode = 0;
    velskelactive = -1;
    velskelprevb = NULL;
    velskelprevn = 0;
}

static void veldrawhud()
{
    velhuddrawn = 0;
    if(!hwrtvelhud || !camera1 || !veltex || isthirdperson())
    {
        velprevavatarok = false;
        velhudowner = NULL;
        return;
    }

    velhudowner = player;
    hwrtvelbeginavatar();
    velavatarcurr = camprojmatrix;
    velavatarcurrunjit = hwrttemporalavatarunjit();
    velsethudgl();
    hwrtvelskelpass = true;
    hwrtvelhudpass = true;
    hwrtvelunlitmode = 0;
    hwrtvelidmode = 0;
    velskelactive = -1;
    velobjhasprev = false;
    velobjgotroot = false;
    Uint64 t0 = SDL_GetPerformanceCounter();
    hwrtveldrawavatar();
    loopk(velskeln)
        if(velskels[k].attachkind == VELATT_HUDGUN && velskels[k].committedframe == velskelframe)
            velskels[k].validprev = true;
    if(veltrackon && velensurediag())
    {
        veldrawhuddiag(0, false);
        veldrawhuddiag(1, false);
        if(velidok)
        {
            veldrawhuddiag(0, true);
            veldrawhuddiag(1, true);
        }
        velsethudgl();
    }
    Uint64 freq = SDL_GetPerformanceFrequency();
    if(freq) hwrtvelskelcpu += float((SDL_GetPerformanceCounter() - t0) * 1000.0 / double(freq));
    hwrtvelhudpass = false;
    hwrtvelskelpass = false;
    velskelactive = -1;
    velskelprevb = NULL;
    velskelprevn = 0;
    hwrtvelendavatar();
    velprevavatar = velavatarcurr;
    velprevavatarunjit = velavatarcurrunjit;
    velprevavatarok = velhuddrawn > 0;
    velprevavatarfov = curavatarfov;
    velprevavatarfovok = true;
    velhudowner = NULL;
}

static void veldrawrigidid(int which)
{
    int idx = hwrtveldrive;
    if(idx < 0 || !velobjs.inrange(idx) || !velobjs[idx].valid || !velobjs[idx].haveclip) return;
    if(which == 1 && !velobjs[idx].haveprevclip) return;
    if(!velidok || !velidtex[which] || !veldiagfb) return;
    const vector<extentity *> &ents = entities::getents();
    if(!ents.inrange(idx) || !ents[idx] || ents[idx]->type != ET_MAPMODEL) return;
    model *m = loadmapmodel(ents[idx]->attr2);
    if(!m) return;
    velobjhist &oh = velobjs[idx];
    glBindFramebuffer_(GL_FRAMEBUFFER, veldiagfb);
    glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, velidtex[which], 0);
    glViewport(0, 0, velw, velh);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.0f, -1.0f);
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0);
    glStencilFunc(GL_NOTEQUAL, VEL_ST_EXCLUDE, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    hwrtvelidmode = which == 0 ? 1 : 2;
    velidobj = true;
    hwrtvelidsetclip(which == 0 ? &oh.curused : &oh.prevused);
    m->startrender();
    extentity &e = *ents[idx];
    m->render(ANIM_MAPMODEL|ANIM_LOOP, 0, 0, e.o, e.attr1, 0, NULL, NULL);
    m->endrender();
    hwrtvelidsetclip(NULL);
    velidobj = false;
    hwrtvelidmode = 0;
}

static void velendskelgl()
{
    velslotdumpframe();
    velskelage();
    veltrackon = false;
    glBindFramebuffer_(GL_FRAMEBUFFER, velfb);
    glViewport(0, 0, velw, velh);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(0, 0);
    glDepthFunc(GL_LESS);
    glDisable(GL_STENCIL_TEST);
    glStencilMask(0);
    glStencilFunc(GL_ALWAYS, 0, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
}

static void veldrawrigid()
{
    if(!camera1 || !veltex) return;
    const vector<extentity *> &ents = entities::getents();
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.0f, -1.0f);
    // Keep excluded pixels (particles / water / unsupported / attached guns).
    // Cover (1), empty (0) and skinned players (3) may be replaced when this
    // geometry is in front of the blitted depth. ZERO the stencil on a depth
    // pass so a hidden body is not still tagged SKEL (that would look like a
    // colour-visible character behind the wall). Ref is EXCLUDE so REPLACE
    // cannot write COVER; ZERO is the op that does not use the ref.
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
    glStencilFunc(GL_NOTEQUAL, VEL_ST_EXCLUDE, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_ZERO);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_CULL_FACE);
    hwrtvelobjpass = true;
    velloghas = false;
    vellogidx = -1;

    model *lastm = NULL;
    int n = 0;
    loopv(ents)
    {
        extentity &e = *ents[i];
        if(e.type != ET_MAPMODEL || (e.flags&EF_NOVIS)) continue;
        if(!hwrtvelrigidmapmodel(e)) continue;
        model *m = loadmapmodel(e.attr2);
        if(!m) continue;
        vec center, radius;
        m->boundbox(center, radius);
        float rad = max(radius.x, max(radius.y, radius.z));
        if(m->scale > 0) rad *= m->scale;
        if(rad < 16) rad = 16;
        if(isvisiblesphere(rad, e.o) == VFC_NOT_VISIBLE) continue;
        if(m != lastm)
        {
            if(lastm) lastm->endrender();
            m->startrender();
            lastm = m;
        }
        velobjbegin(i, e.attr2, e.type);
        m->render(ANIM_MAPMODEL|ANIM_LOOP, 0, 0, e.o, e.attr1, 0, NULL, NULL);
        velobjend();
        n++;
    }
    int nent = 0, nentskip = 0;
    if(!hwrtvelents)
    {
        static int entwarn;
        if(entwarn != lastmillis/2000)
        {
            entwarn = lastmillis/2000;
            conoutf("hwrt velocity: diagnostic hwrtvelents 0, pickup object MVs skipped");
        }
    }
    else loopv(ents)
    {
        extentity &e = *ents[i];
        if(!velentwant(e)) continue;
        const char *mdlname = entities::entmodel(e);
        model *m = loadmodel(mdlname);
        if(!m || m->skeletal()) { nentskip++; continue; }
        vec o;
        float yaw;
        hwrtentxform(e, o, yaw);
        vec center, radius;
        m->boundbox(center, radius);
        float rad = max(radius.x, max(radius.y, radius.z));
        if(m->scale > 0) rad *= m->scale;
        if(rad < 16) rad = 16;
        if(isvisiblesphere(rad, o) == VFC_NOT_VISIBLE) { nentskip++; continue; }
        if(m != lastm)
        {
            if(lastm) lastm->endrender();
            m->startrender();
            lastm = m;
        }
        velobjbegin(i, e.attr2, e.type);
        m->render(ANIM_MAPMODEL|ANIM_LOOP, 0, 0, o, yaw, 0, NULL, NULL);
        velobjend();
        nent++;
        n++;
    }
    if(lastm) lastm->endrender();
    hwrtvelobjpass = false;
    velobjage();
    glDisable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(0, 0);
    glDepthFunc(GL_LESS);
    glDisable(GL_STENCIL_TEST);
    glStencilMask(0);
    glStencilFunc(GL_ALWAYS, 0, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    {
        static int objlogs = 0;
        if(objlogs < 6)
        {
            conoutf("hwrt velocity: rigid object pass %d mapmodels + %d pickups (%d pickup skip, hwrtvelents %d)", n - nent, nent, nentskip, int(hwrtvelents));
            objlogs++;
        }
    }
}

static float wrap180(float a)
{
    while(a > 180) a -= 360;
    while(a < -180) a += 360;
    return a;
}

static bool velposejump()
{
    if(!camera1 || !velcamset) return false;
    if(camera1->o.dist(velcam) > 24) return true;
    if(fabs(wrap180(camera1->yaw - velyaw)) > 50) return true;
    if(fabs(wrap180(camera1->pitch - velpitch)) > 50) return true;
    return false;
}

static void velshotpush(const char *name, int debug, int kind = VELSHOT_SHOT, float arg = 0)
{
    if(velshotcount >= VEL_SHOTQ)
    {
        conoutf(CON_WARN, "hwrt velocity: screenshot queue full");
        return;
    }
    int slot = (velshothead + velshotcount) % VEL_SHOTQ;
    copystring(velshotq[slot].name, name && name[0] ? name : "vel");
    velshotq[slot].debug = debug;
    velshotq[slot].kind = kind;
    velshotq[slot].arg = arg;
    velshotcount++;
}

static void velshotpop()
{
    if(velshotcount <= 0) return;
    velshothead = (velshothead + 1) % VEL_SHOTQ;
    velshotcount--;
}

static void veldocamcut();

static void velapplyqueuedshots()
{
    while(velshotcount > 0)
    {
        velshotreq &q = velshotq[velshothead];
        if(q.kind == VELSHOT_SETVEL)
        {
            int on = q.arg > 0.5f ? 1 : 0;
            setvar("hwrtvelocity", on);
            conoutf("hwrt velocity: queued hwrtvelocity %d (this frame)", on);
            velshotpop();
            continue;
        }
        if(q.kind == VELSHOT_ZOOM)
        {
            velzoomhold = q.arg;
            if(velzoomhold < 0)
            {
                disablezoom();
                velzoomhold = -1;
                conoutf("hwrt velocity: queued zoom released (computezoom runs normally)");
            }
            else
            {
                velzoomhold = clamp(velzoomhold, 0.0f, 1.0f);
                forcezoom(velzoomhold);
                conoutf("hwrt velocity: queued zoom hold %.3f curfov %.3f avatarfov %.3f (same FOV this colour frame and the velocity HUD pass)",
                        velzoomhold, curfov, curavatarfov);
            }
            velshotpop();
            continue;
        }
        if(q.kind == VELSHOT_CAMCUT)
        {
            veldocamcut();
            velshotpop();
            continue;
        }
        if(q.kind == VELSHOT_SETJITTER)
        {
            int on = q.arg > 0.5f ? 1 : 0;
            setvar("hwrtdlaajitter", on);
            conoutf("hwrt velocity: queued hwrtdlaajitter %d (this frame)", on);
            velshotpop();
            continue;
        }
        if(q.kind == VELSHOT_SCREENRES)
        {
            int w = 0, h = 0;
            if(sscanf(q.name, "%d %d", &w, &h) == 2 && w >= 8 && h >= 8)
            {
                defformatstring(cmd, "screenres %d %d", w, h);
                execute(cmd);
                if(screen)
                {
                    SDL_GetWindowSize(screen, &screenw, &screenh);
                    if(screenw > 0 && screenh > 0) gl_resize();
                }
                conoutf("hwrt velocity: queued screenres %d %d applied this frame (drawable %dx%d)", w, h, screenw, screenh);
            }
            velshotpop();
            continue;
        }
        if(q.kind == VELSHOT_FIRE)
        {
            execute("hwrtvelfire");
            velshotpop();
            continue;
        }
        if(q.kind == VELSHOT_FREEZEPOSE)
        {
            int on = q.arg > 0.5f ? 1 : 0;
            setvar("hwrtvelfreezepose", on);
            conoutf("hwrt velocity: queued hwrtvelfreezepose %d (this frame)", on);
            velshotpop();
            continue;
        }
        break;
    }
    if(velzoomhold >= 0) forcezoom(velzoomhold);
}

static bool velaliveproofready()
{
    return player && player->state == CS_ALIVE && !isthirdperson();
}

void hwrtapplyvelspin()
{
    if(velbenchpending)
    {
        char *cmd = velbenchpending;
        velbenchpending = NULL;
        execute(cmd);
        delete[] cmd;
    }
    velapplyqueuedshots();
    if(velshotcount > 0 && !velshotkindproof(velshotq[velshothead].kind))
        hwrtveldebug = velshotq[velshothead].debug;
    if(hwrtvelwalk && player)
    {
        if(player->state == CS_DEAD) player->state = CS_ALIVE;
        if(player->state == CS_ALIVE) player->move = 1;
    }
    if(hwrtvelsmoke && camera1 && screenw > 0)
    {
        vec fwd, right;
        vecfromyawpitch(camera1->yaw, camera1->pitch, 1, 0, fwd);
        vecfromyawpitch(camera1->yaw, 0, 0, 1, right);
        vec p = vec(camera1->o).add(vec(fwd).mul(hwrtvelsmokedist));
        // Real smoke sprites (may be skipped if the particle system refuses
        // the add). The visible exclusion proof is hwrtveldraweffects().
        particle_splash(PART_SMOKE, 10, 1100, p, 0x555555, 3.4f, 3, -8);
        particle_splash(PART_SMOKE, 6, 1100, vec(p).add(vec(right).mul(2.2f)), 0x555555, 3.0f, 2, -8);
        particle_splash(PART_SMOKE, 6, 1100, vec(p).add(vec(0, 0, 1.4f)), 0x555555, 3.0f, 2, -8);
    }
    if((hwrtvelocity || hwrtveldebug || hwrtdlaaneedsdata()) && hwrtveldrive >= 0 && camera1)
    {
        const vector<extentity *> &ents = entities::getents();
        if(ents.inrange(hwrtveldrive) && ents[hwrtveldrive] && ents[hwrtveldrive]->type == ET_MAPMODEL)
        {
            extentity &e = *ents[hwrtveldrive];
            if(hwrtveldriveslide != 0)
            {
                vec right;
                vecfromyawpitch(camera1->yaw, 0, 0, 1, right);
                float step = hwrtveldriveslide * curtime / 1000.0f;
                if(step > 2) step = 2;
                else if(step < -2) step = -2;
                e.o.add(right.mul(step));
            }
            if(hwrtveldrivepush != 0)
            {
                vec fwd;
                vecfromyawpitch(camera1->yaw, 0, 1, 0, fwd);
                float step = hwrtveldrivepush * curtime / 1000.0f;
                if(step > 2) step = 2;
                else if(step < -2) step = -2;
                e.o.add(fwd.mul(step));
            }
            if(veldsleft > 0)
            {
                vec right;
                vecfromyawpitch(camera1->yaw, 0, 0, 1, right);
                e.o.add(right.mul(veldsstep));
                veldsleft--;
                if(veldsleft <= 0) veldsstep = 0;
            }
            if(veldpleft > 0)
            {
                vec fwd;
                vecfromyawpitch(camera1->yaw, 0, 1, 0, fwd);
                e.o.add(fwd.mul(veldpstep));
                veldpleft--;
                if(veldpleft <= 0) veldpstep = 0;
            }
        }
    }
    if(hwrtvelholdplayer && player)
    {
        player->o = velholdplayerpos;
        player->vel = vec(0, 0, 0);
        player->falling = vec(0, 0, 0);
        if(!hwrtvelwalk)
        {
            player->move = 0;
            player->strafe = 0;
        }
        player->yaw = velholdplayeryaw;
        player->pitch = velholdplayerpitch;
        player->resetinterp();
    }
    if(hwrtvelholdcam && camera1)
    {
        camera1->o = velholdcampos;
        camera1->yaw = velholdyaw;
        camera1->pitch = velholdpitch;
        if(player && camera1 == player && hwrtvelholdplayer)
        {
            player->o = velholdplayerpos;
            player->resetinterp();
        }
    }
    if(hwrtvelholdothers)
    {
        int n = game::numdynents();
        loopi(n)
        {
            dynent *d = game::iterdynents(i);
            if(!d || d == player || d->type != ENT_PLAYER) continue;
            d->vel = vec(0, 0, 0);
            d->falling = vec(0, 0, 0);
            d->move = 0;
            d->strafe = 0;
            d->resetinterp();
        }
    }
    if((velplacetwo > 0 || veloverlapmode) && player && camera1)
    {
        if(velplacetwo > 0) velplacetwo--;
        vec stand(0, 0, 0);
        if(veloverlapmode)
        {
            // Hold-camera first so the bot sits on the proof ray, closer to
            // the camera than the local player (real front/back masking).
            vec cam = hwrtvelholdcam ? velholdcampos : camera1->o;
            vec view = vec(player->o).sub(cam);
            view.z = 0;
            float mlen = view.magnitude();
            if(mlen > 1e-3f)
            {
                view.mul(1.0f / mlen);
                stand = vec(player->o).sub(vec(view).mul(6.0f));
                vec up(0, 0, 1), right;
                right.cross(view, up);
                if(right.squaredlen() > 1e-6f) right.normalize();
                else right = vec(1, 0, 0);
                // Slight lateral offset: both bodies stay readable, but the
                // nearer one still covers a real patch of the farther one.
                stand.add(right.mul(2.2f));
                stand.z = player->o.z;
            }
            else stand = player->o;
        }
        else
        {
            vec right, fwd;
            vecfromyawpitch(camera1->yaw, 0, 0, 1, right);
            vecfromyawpitch(camera1->yaw, 0, 1, 0, fwd);
            stand = vec(player->o).sub(fwd.mul(7)).add(right.mul(5));
        }
        int n = game::numdynents();
        loopi(n)
        {
            dynent *d = game::iterdynents(i);
            if(!d || d == player || d->type != ENT_PLAYER) continue;
            d->o = stand;
            d->yaw = player->yaw;
            d->pitch = 0;
            d->vel = vec(0, 0, 0);
            d->falling = vec(0, 0, 0);
            d->move = 0;
            d->strafe = 0;
            d->resetinterp();
        }
    }
    if(!camera1) { veltickframewait(); return; }
    if(hwrtvelspin == 0 && hwrtvelpitchspin == 0 && hwrtvelslide == 0 && hwrtvelpush == 0 &&
       velspinfleft <= 0 && velslidefleft <= 0 && velpushfleft <= 0)
    {
        veltickframewait();
        return;
    }
    // DLAA turns velocity on through hwrtdlaaneedsdata() while leaving the
    // hwrtvelocity cvar at 0. The test drives must still move that camera.
    if(!hwrtvelocity && !hwrtveldebug && !hwrtdlaaneedsdata()) { veltickframewait(); return; }
    if(hwrtvelspin != 0)
    {
        camera1->yaw += hwrtvelspin;
        while(camera1->yaw >= 360) camera1->yaw -= 360;
        while(camera1->yaw < 0) camera1->yaw += 360;
    }
    if(hwrtvelpitchspin != 0)
    {
        camera1->pitch = clamp(camera1->pitch + hwrtvelpitchspin, -89.0f, 89.0f);
    }
    if(hwrtvelslide != 0)
    {
        vec right;
        vecfromyawpitch(camera1->yaw, 0, 0, 1, right);
        camera1->o.add(right.mul(hwrtvelslide));
        if(player == camera1) player->resetinterp();
    }
    if(hwrtvelpush != 0)
    {
        vec fwd;
        vecfromyawpitch(camera1->yaw, 0, 1, 0, fwd);
        camera1->o.add(fwd.mul(hwrtvelpush));
        if(player == camera1) player->resetinterp();
    }
    if(hwrtvelholdcam)
    {
        velholdcampos = camera1->o;
        velholdyaw = camera1->yaw;
        velholdpitch = camera1->pitch;
        velholdpos = camera1->o;
        // holdplayer used to restore the setpos origin after this push/spin,
        // so translation tests stayed pinned. Play leaves both holds at 0.
        if(hwrtvelholdplayer && player && camera1 == player)
        {
            velholdplayerpos = camera1->o;
            velholdplayeryaw = camera1->yaw;
            velholdplayerpitch = camera1->pitch;
        }
    }
    if(velspinfleft > 0)
    {
        velspinfleft--;
        if(velspinfleft <= 0) hwrtvelspin = 0;
    }
    if(velslidefleft > 0)
    {
        velslidefleft--;
        if(velslidefleft <= 0) hwrtvelslide = 0;
    }
    if(velpushfleft > 0)
    {
        velpushfleft--;
        if(velpushfleft <= 0) hwrtvelpush = 0;
    }
    veltickframewait();
}

void hwrtveldraweffects()
{
    if(!hwrtvelsmoke || !camera1) return;
    if(!hwrtvelmarking()) return;

    // Test-only low-contrast haze (no depth write), drawn while stencil is
    // UNMARK. Same class as particles: the object pass must not revive these
    // pixels. Play leaves hwrtvelsmoke at 0, so this is not in the cost.

    vec p = vec(camera1->o).add(vec(camdir).mul(hwrtvelsmokedist));
    vec puffs[3] = {
        p,
        vec(p).sub(vec(camright).mul(3.4f)),
        vec(p).add(vec(camup).mul(1.2f))
    };
    const float sz = 2.1f;
    bvec4 col(0x6C, 0x6C, 0x6C, 0xB8);

    GLboolean hadblend = glIsEnabled(GL_BLEND);
    GLboolean hadcull = glIsEnabled(GL_CULL_FACE);
    GLboolean haddepth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean dmask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &dmask);

    notextureshader->set();
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);

    gle::defvertex();
    gle::defcolor(4, GL_UNSIGNED_BYTE);
    gle::begin(GL_QUADS);
    loopi(3)
    {
        const vec &o = puffs[i];
        gle::attribf(o.x+(-camright.x+camup.x)*sz, o.y+(-camright.y+camup.y)*sz, o.z+(-camright.z+camup.z)*sz); gle::attrib(col);
        gle::attribf(o.x+( camright.x+camup.x)*sz, o.y+( camright.y+camup.y)*sz, o.z+( camright.z+camup.z)*sz); gle::attrib(col);
        gle::attribf(o.x+( camright.x-camup.x)*sz, o.y+( camright.y-camup.y)*sz, o.z+( camright.z-camup.z)*sz); gle::attrib(col);
        gle::attribf(o.x+(-camright.x-camup.x)*sz, o.y+(-camright.y-camup.y)*sz, o.z+(-camright.z-camup.z)*sz); gle::attrib(col);
    }
    gle::end();

    if(hadblend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if(hadcull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if(haddepth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glDepthMask(dmask);
}

void hwrtnotecamproj()
{
    if(!velwanted()) return;
    velcurr = camprojmatrix;
    velcurrunjit = hwrttemporalworldunjit();
}

static void velbindgen()
{
    Shader *s = useshaderbyname("hwrtvelocity");
    if(!s || s->invalid()) return;
    s->set();
    glActiveTexture_(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, veldepth);
    glActiveTexture_(GL_TEXTURE0);
    matrix4 invcur;
    invcur.invert(velcurr);
    LOCALPARAM(invcamproj, invcur);
    LOCALPARAM(currunjitproj, velcurrunjit);
    LOCALPARAM(prevunjitproj, velprevunjit);
    float jcx = 0, jcy = 0, jpx = 0, jpy = 0;
    hwrttemporaljitterpixels(jcx, jcy, jpx, jpy);
    LOCALPARAMF(veljitter, jcx, jcy, jpx, jpy);
    // z = previous matrix is usable (motion). Coverage is the stencil, not this.
    // A camera cut keeps coverage and turns motion off for this frame only.
    LOCALPARAMF(velparams, 1.0f/velw, 1.0f/velh, velready && !velcutframe ? 1.0f : 0.0f, 0);
}

void hwrtgenvelocity()
{
    hwrtvelstencilsnap("before_endmark");
    hwrtvelendmark();
    if(!hwrtveldebug) hwrtveldebugcost = 0;
    if(!velwanted()) return;
    if(!velensure())
    {
        velready = false;
        return;
    }
    velinittimers();
    if(velbenchphase == VELBENCH_IDLE || velbenchphase == VELBENCH_WAITQ || velbenchphase == VELBENCH_WARM)
    {
        if(velqwatchequip != int(hwrtvelequip))
        {
            velqepoch++;
            velqwatchequip = int(hwrtvelequip);
        }
    }
    else velqwatchequip = int(hwrtvelequip);
    velhqcopy = velhqgen = velhqobj = velhqskel = 0;
    velhqcopyep = velhqgenep = velhqobjep = velhqskelep = -1;
    velhqcopyeq = velhqgeneq = velhqobjeq = velhqskeleq = -1;
    velharvest(velqcopy, velqcopypend, velqcopyslot, &hwrtvelcopycost, velqcopyep, velqcopyeq, &velhqcopy, &velhqcopyep, &velhqcopyeq);
    velharvest(velqgen, velqgenpend, velqgenslot, &hwrtvelgencost, velqgenep, velqgeneq, &velhqgen, &velhqgenep, &velhqgeneq);
    velharvest(velqobj, velqobjpend, velqobjslot, &hwrtvelobjcost, velqobjep, velqobjeq, &velhqobj, &velhqobjep, &velhqobjeq);
    velharvest(velqskel, velqskelpend, velqskelslot, &hwrtvelskelcost, velqskelep, velqskeleq, &velhqskel, &velhqskelep, &velhqskeleq);

    velcutframe = velposejump();
    if(velcutframe) hwrttemporalsetreset(true);
    if(velcamset)
    {
        velprevcam = velcam;
        velprevyaw = velyaw;
        velprevpitch = velpitch;
        velprevcamok = true;
        velprevmillis = velnotemillis;
    }
    if(velcutframe)
    {
        // Covered pixels stay covered. The previous colour belongs to a
        // different view, so reprojection must not use it from this frame.
        velhisthas = false;
        velpostcut = 0;
        conoutf("hwrt velocity: camera cut this frame - colour history unusable, coverage kept, motion off  cam %.1f %.1f %.1f yaw %.1f pitch %.1f",
                camera1 ? camera1->o.x : 0, camera1 ? camera1->o.y : 0, camera1 ? camera1->o.z : 0,
                camera1 ? camera1->yaw : 0, camera1 ? camera1->pitch : 0);
    }
    else if(velpostcut >= 0)
    {
        velpostcut++;
        if(velpostcut > 4) velpostcut = -1;
    }

    velbeginq(velqcopy, velqcopypend, &velqcopyslot, &velqcopyact, velqcopyep, velqcopyeq);
    GLboolean hadscissor = glIsEnabled(GL_SCISSOR_TEST);
    if(hadscissor) glDisable(GL_SCISSOR_TEST);
    velcopytex(veldepth);
    if(hasFBB && glBlitFramebuffer_)
    {
        GLint prevdraw = 0, prevread = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevdraw);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevread);
        glBindFramebuffer_(GL_READ_FRAMEBUFFER, hwrtmainfbo());
        glBindFramebuffer_(GL_DRAW_FRAMEBUFFER, velfb);
        int bw = min(velw, hwrtfbw()), bh = min(velh, hwrtfbh());
        glBlitFramebuffer_(0, 0, bw, bh, 0, 0, bw, bh, GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer_(GL_READ_FRAMEBUFFER, prevread);
        glBindFramebuffer_(GL_DRAW_FRAMEBUFFER, prevdraw);
    }
    if(hadscissor) glEnable(GL_SCISSOR_TEST);
    {
        GLint prevsnap = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevsnap);
        glBindFramebuffer_(GL_FRAMEBUFFER, velfb);
        hwrtvelstencilsnap("velfb_after_blit");
        glBindFramebuffer_(GL_FRAMEBUFFER, prevsnap);
    }
    velendq(velqcopypend, &velqcopyslot, &velqcopyact);

    if(velneeddiaghist()) velensurediag();

    GLint prevfb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevfb);
    GLboolean hadcull = glIsEnabled(GL_CULL_FACE),
              haddepth = glIsEnabled(GL_DEPTH_TEST),
              hadblend = glIsEnabled(GL_BLEND),
              hadstencil = glIsEnabled(GL_STENCIL_TEST);
    GLboolean haddepthmask = GL_TRUE;
    GLint oldsmask = 0xFF;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &haddepthmask);
    glGetIntegerv(GL_STENCIL_WRITEMASK, &oldsmask);

    velbeginq(velqgen, velqgenpend, &velqgenslot, &velqgenact, velqgenep, velqgeneq);

    glBindFramebuffer_(GL_FRAMEBUFFER, velfb);
    glViewport(0, 0, velw, velh);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDepthMask(GL_FALSE);
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0);
    glStencilFunc(GL_EQUAL, VEL_ST_COVER, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    GLfloat oldclear[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, oldclear);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(oldclear[0], oldclear[1], oldclear[2], oldclear[3]);
    velbindgen();
    screenquad(1, 1);

    velendq(velqgenpend, &velqgenslot, &velqgenact);

    velbeginq(velqobj, velqobjpend, &velqobjslot, &velqobjact, velqobjep, velqobjeq);
    veldrawrigid();
    velendq(velqobjpend, &velqobjslot, &velqobjact);

    // ID/unlit must see rigid occluders. The colour-pass DS blit (default FB)
    // can omit mapmodel depth when the RT composite restored world-only depth,
    // while the colour RGB still shows the wall. Snapshot after the object
    // pass so a character behind a mapmodel fails depth instead of staying
    // "valid" / punching the ID buffer.
    if(velneeddiaghist() && velscenefb[velsceneslot]) velblitds(velfb, velscenefb[velsceneslot]);

    velbeginq(velqskel, velqskelpend, &velqskelslot, &velqskelact, velqskelep, velqskeleq);
    veldrawskel();
    veldrawhud();
    if(veltrackon && velidok)
    {
        veldrawrigidid(0);
        veldrawrigidid(1);
    }
    velendskelgl();
    hwrtvelstencilsnap("velfb_after_hud");
    velendq(velqskelpend, &velqskelslot, &velqskelact);

    glBindFramebuffer_(GL_FRAMEBUFFER, prevfb);
    hwrtbindscenefb();
    if(hadblend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if(haddepth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if(hadcull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if(hadstencil) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    glStencilMask(oldsmask);
    if(haddepthmask) glDepthMask(GL_TRUE); else glDepthMask(GL_FALSE);

    if(camera1)
    {
        velcam = camera1->o;
        velyaw = camera1->yaw;
        velpitch = camera1->pitch;
        velcamset = true;
    }
    velprev = velcurr;
    velprevunjit = velcurrunjit;
    velready = true;
    velnotemillis = lastmillis;
    velstatpush();
    velbenchstep();
    if(velneeddiaghist() && velscenefb[0])
    {
        velsceneslot ^= 1;
        velscenehasprev = true;
    }
    if(velcutframe || velpostcut == 1 || velpostcut == 2)
        velprobelog(velcutframe ? "CUT" : "POSTCUT");
}

static void velshotdir()
{
    string d1, d2;
    copystring(d1, "shots");
    path(d1);
    createdir(d1);
    copystring(d2, "shots/velocity-lot6");
    path(d2);
    createdir(d2);
}

static void velsavepng(const char *fn, ImageData &image)
{
    extern void saveimage(const char *filename, int format, ImageData &image, bool flip);
    saveimage(fn, 2, image, true);
}

static void velputrgb(ImageData &im, int x, int y, int r, int g, int b)
{
    if(x < 0 || y < 0 || x >= im.w || y >= im.h) return;
    uchar *p = im.data + (y * im.w + x) * 3;
    p[0] = uchar(r);
    p[1] = uchar(g);
    p[2] = uchar(b);
}

static void velmark(ImageData &im, int x, int y, int r, int g, int b)
{
    for(int dy = -4; dy <= 4; dy++)
        for(int dx = -4; dx <= 4; dx++)
        {
            int px = x + dx, py = y + dy;
            if(dx*dx + dy*dy > 16) continue;
            if(dx*dx + dy*dy >= 9) velputrgb(im, px, py, 0, 0, 0);
            else velputrgb(im, px, py, r, g, b);
        }
}

static void velmarkcross(ImageData &im, int x, int y, int r, int g, int b)
{
    for(int k = -6; k <= 6; k++)
    {
        velputrgb(im, x + k, y, r, g, b);
        velputrgb(im, x, y + k, r, g, b);
    }
}

static void velcopyrect(const ImageData &src, ImageData &dst, int x0, int y0)
{
    loopi(dst.h) loopj(dst.w)
    {
        int sx = x0 + j, sy = y0 + i;
        if(sx < 0 || sy < 0 || sx >= src.w || sy >= src.h) { velputrgb(dst, j, i, 0, 0, 0); continue; }
        uchar *p = src.data + (sy * src.w + sx) * 3;
        velputrgb(dst, j, i, p[0], p[1], p[2]);
    }
}

static uint velhashbones(const dualquat *b, int n)
{
    uint h = 2166136261u;
    if(!b || n < 1) return 0;
    loopi(n)
    {
        union { float f; uint u; } u;
        u.f = b[i].real.x; h = (h ^ u.u) * 16777619u;
        u.f = b[i].real.w; h = (h ^ u.u) * 16777619u;
        u.f = b[i].dual.x; h = (h ^ u.u) * 16777619u;
    }
    return h;
}

struct velpt { int x, y, px, py; float vx, vy; };

static bool velinteriorpt(const float *velpix, int w, int h, int x, int y, float vx, float vy, bool skel)
{
    if(x < 2 || y < 2 || x >= w - 2 || y >= h - 2) return false;
    int px = int((x + 0.5f) + vx), py = int((y + 0.5f) + vy);
    if(px < 2 || py < 2 || px >= w - 2 || py >= h - 2) return false;
    for(int dy = -2; dy <= 2; dy++)
        for(int dx = -2; dx <= 2; dx++)
        {
            int idx = ((y + dy) * w + (x + dx)) * 4;
            if(velpix[idx + 2] < 0.5f) return false;
            if(skel && velpix[idx + 3] < 0.5f) return false;
            if(fabs(velpix[idx] - vx) > 2.5f || fabs(velpix[idx + 1] - vy) > 2.5f) return false;
        }
    return true;
}

static int velpicksamples(const float *velpix, int w, int h, velpt *pts, int maxn, bool skel)
{
    struct cand { int x, y; float mag, vx, vy; };
    cand best[64];
    int nb = 0;
    for(int y = 4; y < h - 4; y += 3)
        for(int x = 4; x < w - 4; x += 3)
        {
            int idx = (y * w + x) * 4;
            float vx = velpix[idx], vy = velpix[idx + 1];
            if(velpix[idx + 2] < 0.5f) continue;
            if(skel && velpix[idx + 3] < 0.5f) continue;
            if(!velinteriorpt(velpix, w, h, x, y, vx, vy, skel)) continue;
            float mag = sqrtf(vx * vx + vy * vy);
            if(nb < 64)
            {
                best[nb].x = x; best[nb].y = y; best[nb].mag = mag; best[nb].vx = vx; best[nb].vy = vy;
                nb++;
            }
            else
            {
                int widx = 0;
                loopi(64) if(best[i].mag < best[widx].mag) widx = i;
                if(mag > best[widx].mag)
                {
                    best[widx].x = x; best[widx].y = y; best[widx].mag = mag;
                    best[widx].vx = vx; best[widx].vy = vy;
                }
            }
        }
    loopi(nb)
        for(int j = i; j > 0 && best[j].mag > best[j - 1].mag; j--)
        {
            cand t = best[j];
            best[j] = best[j - 1];
            best[j - 1] = t;
        }
    int npt = 0;
    const float mind2 = 24.f * 24.f;
    loopk(2)
    {
        loopi(nb)
        {
            if(npt >= maxn) break;
            if(k == 0 && best[i].mag < 0.8f) continue;
            bool farok = true;
            loopj(npt)
            {
                float dx = float(best[i].x - pts[j].x), dy = float(best[i].y - pts[j].y);
                if(dx * dx + dy * dy < mind2) { farok = false; break; }
            }
            if(!farok) continue;
            pts[npt].x = best[i].x;
            pts[npt].y = best[i].y;
            pts[npt].vx = best[i].vx;
            pts[npt].vy = best[i].vy;
            pts[npt].px = int((best[i].x + 0.5f) + best[i].vx);
            pts[npt].py = int((best[i].y + 0.5f) + best[i].vy);
            npt++;
        }
        if(npt >= 3) break;
    }
    return npt;
}

static void velsampleraw(const ImageData &im, float u, float v, int out[3])
{
    float x = clamp(u, 0.0f, 1.0f) * (im.w - 1);
    float y = clamp(v, 0.0f, 1.0f) * (im.h - 1);
    int x0 = clamp(int(floor(x)), 0, im.w - 1);
    int y0 = clamp(int(floor(y)), 0, im.h - 1);
    int x1 = min(x0 + 1, im.w - 1);
    int y1 = min(y0 + 1, im.h - 1);
    float fx = x - x0, fy = y - y0;
    loopk(3)
    {
        float c00 = im.data[(y0 * im.w + x0) * 3 + k];
        float c10 = im.data[(y0 * im.w + x1) * 3 + k];
        float c01 = im.data[(y1 * im.w + x0) * 3 + k];
        float c11 = im.data[(y1 * im.w + x1) * 3 + k];
        out[k] = int(c00 * (1 - fx) * (1 - fy) + c10 * fx * (1 - fy) + c01 * (1 - fx) * fy + c11 * fx * fy + 0.5f);
    }
}

static int velfcmp(const void *a, const void *b)
{
    float fa = *(const float *)a, fb = *(const float *)b;
    return fa < fb ? -1 : (fa > fb ? 1 : 0);
}

static void velidpixel(const float *idpix, int w, int h, int x, int y, int &slot, int &mesh, int &tri, float &z)
{
    slot = mesh = tri = -1;
    z = 1;
    if(!idpix || x < 0 || y < 0 || x >= w || y >= h) return;
    int idx = (y * w + x) * 4;
    slot = int(idpix[idx] + 0.5f) - 1;
    mesh = int(idpix[idx + 1] + 0.5f) - 1;
    tri = int(idpix[idx + 2] + 0.5f) - 1;
    z = idpix[idx + 3];
}

static bool velidfind(const float *idpix, int w, int h, float px, float py, int slot, int mesh, int tri, int radius, int &ox, int &oy, float &z, float &off)
{
    ox = int(floor(px));
    oy = int(floor(py));
    z = 1;
    off = 1e9f;
    if(!idpix) return false;
    int x0 = max(int(floor(px)) - radius, 0);
    int y0 = max(int(floor(py)) - radius, 0);
    int x1 = min(int(ceil(px)) + radius, w - 1);
    int y1 = min(int(ceil(py)) + radius, h - 1);
    bool found = false;
    for(int y = y0; y <= y1; y++)
        for(int x = x0; x <= x1; x++)
        {
            int s, m, t;
            float iz;
            velidpixel(idpix, w, h, x, y, s, m, t, iz);
            if(s != slot || m != mesh || t != tri) continue;
            float dx = (x + 0.5f) - px, dy = (y + 0.5f) - py;
            float d = sqrtf(dx * dx + dy * dy);
            if(d < off)
            {
                off = d;
                ox = x;
                oy = y;
                z = iz;
                found = true;
            }
        }
    return found;
}

static int velidcause(const float *idpix, int w, int h, float px, float py, int slot, int mesh, int tri, const uchar *stpix, bool onscreen, bool hashist, bool cur)
{
    if(!onscreen) return cur ? VELTR_OFFSCREEN_CUR : VELTR_OFFSCREEN_PREV;
    if(!hashist && !cur) return VELTR_NO_HISTORY;
    if(!idpix) return VELTR_SAMPLE_MISS;
    int ox, oy;
    float z, off;
    // Raster margin only (pixel-center vs centroid). Do not search far
    // enough to pick a triangle that is actually behind another surface.
    if(velidfind(idpix, w, h, px, py, slot, mesh, tri, 1, ox, oy, z, off)) return VELTR_VALID;
    if(velidfind(idpix, w, h, px, py, slot, mesh, tri, 2, ox, oy, z, off)) return VELTR_VALID;
    int s, m, t;
    float iz;
    velidpixel(idpix, w, h, int(floor(px)), int(floor(py)), s, m, t, iz);
    if(s >= 0 && s != slot)
    {
        if(s < velskeln && slot < velskeln && velskels[s].d && velskels[s].d == velskels[slot].d)
            return VELTR_OCCLUDED_OTHER_TRI;
        return VELTR_OCCLUDED_OTHER_CHAR;
    }
    if(s == slot && (m != mesh || t != tri)) return VELTR_OCCLUDED_OTHER_TRI;
    int ix = int(floor(px)), iy = int(floor(py));
    if(stpix && ix >= 0 && iy >= 0 && ix < w && iy < h && stpix[iy * w + ix] == VEL_ST_SKEL)
        return VELTR_SAMPLE_MISS;
    return VELTR_OCCLUDED_WORLD;
}

enum {
    VELR_VALID = 0,
    VELR_OFFSCREEN_CUR,
    VELR_OFFSCREEN_PREV,
    VELR_OCCLUDED_WORLD,
    VELR_OCCLUDED_OTHER_TRI,
    VELR_OCCLUDED_OTHER_CHAR,
    VELR_NEWLY_VISIBLE,
    VELR_DISAPPEARED,
    VELR_SAMPLE_MISS,
    VELR_NO_HISTORY,
    VELR_SCENERY,
    VELR_N
};
static const char *velrcause[VELR_N] =
{
    "valid", "offscreen_cur", "offscreen_prev", "occluded_world", "occluded_other_tri",
    "occluded_other_char", "newly_visible", "disappeared", "sample_miss", "no_history", "scenery"
};

struct velrstat
{
    int mesh, tri, kind, cause, sx, sy, idslot, idmesh, idtri;
    float rcx, rcy, rpx, rpy, ucx, ucy, upx, upy;
    float bufx, bufy, expx, expy, mag, err, sampleoff, axisdist, area;
    bool worldpix, a1;
};

static velrstat velrstored[VEL_RIGID_KEEP + VEL_RIGID_HUB];
static int velrnstored = 0, velridpixels = 0, velridprev = 0;
static int velr_nvalid = 0, velr_nblade = 0, velr_nbladevalid = 0, velr_nhubvalid = 0;
static int velr_ncause[VELR_N];
static float velr_med = -1, velr_p95 = -1, velr_max = -1, velr_magmed = -1, velr_magp95 = -1, velr_magmax = -1;
static float velr_axismin = -1, velr_axismax = -1;

static bool velidfindslot(const float *idpix, int w, int h, float px, float py, int slot, int radius, int &ox, int &oy, int &mesh, int &tri, float &z, float &off)
{
    ox = int(floor(px));
    oy = int(floor(py));
    mesh = tri = -1;
    z = 1;
    off = 1e9f;
    if(!idpix) return false;
    int x0 = max(int(floor(px)) - radius, 0);
    int y0 = max(int(floor(py)) - radius, 0);
    int x1 = min(int(ceil(px)) + radius, w - 1);
    int y1 = min(int(ceil(py)) + radius, h - 1);
    bool found = false;
    for(int y = y0; y <= y1; y++)
        for(int x = x0; x <= x1; x++)
        {
            int s, m, t;
            float iz;
            velidpixel(idpix, w, h, x, y, s, m, t, iz);
            if(s != slot) continue;
            float dx = (x + 0.5f) - px, dy = (y + 0.5f) - py;
            float d = sqrtf(dx * dx + dy * dy);
            if(d < off)
            {
                off = d;
                ox = x;
                oy = y;
                mesh = m;
                tri = t;
                z = iz;
                found = true;
            }
        }
    return found;
}

static int velrigidclassify(const float *idpix, int w, int h, float px, float py, int mesh, int tri, bool onscreen, bool hashist, bool cur, int &sx, int &sy, float &sampleoff, int &idslot, int &idmesh, int &idtri)
{
    sx = int(floor(px));
    sy = int(floor(py));
    sampleoff = sqrtf(((sx + 0.5f) - px)*((sx + 0.5f) - px) + ((sy + 0.5f) - py)*((sy + 0.5f) - py));
    idslot = idmesh = idtri = -1;
    if(!onscreen) return cur ? VELR_OFFSCREEN_CUR : VELR_OFFSCREEN_PREV;
    if(!hashist && !cur) return VELR_NO_HISTORY;
    if(!idpix) return VELR_SAMPLE_MISS;
    int ox, oy;
    float z, off;
    if(velidfind(idpix, w, h, px, py, VEL_RIGID_SLOT, mesh, tri, 1, ox, oy, z, off) ||
       velidfind(idpix, w, h, px, py, VEL_RIGID_SLOT, mesh, tri, 2, ox, oy, z, off))
    {
        sx = ox;
        sy = oy;
        sampleoff = off;
        idslot = VEL_RIGID_SLOT;
        idmesh = mesh;
        idtri = tri;
        return VELR_VALID;
    }
    int sm = -1, st = -1;
    if(velidfindslot(idpix, w, h, px, py, VEL_RIGID_SLOT, 1, ox, oy, sm, st, z, off))
    {
        sx = ox;
        sy = oy;
        sampleoff = off;
        idslot = VEL_RIGID_SLOT;
        idmesh = sm;
        idtri = st;
        return VELR_VALID;
    }
    int s, m, t;
    float iz;
    velidpixel(idpix, w, h, int(floor(px)), int(floor(py)), s, m, t, iz);
    idslot = s;
    idmesh = m;
    idtri = t;
    if(s == VEL_RIGID_SLOT) return VELR_OCCLUDED_OTHER_TRI;
    if(s >= 0) return VELR_OCCLUDED_OTHER_CHAR;
    return VELR_SCENERY;
}

static void velrigidresetproof()
{
    velrnstored = 0;
    velridpixels = velridprev = 0;
    velr_nvalid = velr_nblade = velr_nbladevalid = velr_nhubvalid = 0;
    memset(velr_ncause, 0, sizeof(velr_ncause));
    velr_med = velr_p95 = velr_max = -1;
    velr_magmed = velr_magp95 = velr_magmax = -1;
    velr_axismin = velr_axismax = -1;
}

static void velrigidfill(velrstat &s, velrtri &t, int kind, velobjhist &oh, int w, int h, const float *velpix, const float *idcur, const float *idprev)
{
    memset(&s, 0, sizeof(s));
    s.mesh = t.mesh;
    s.tri = t.tri;
    s.kind = kind;
    s.axisdist = t.axisdist;
    s.area = t.area;
    s.err = -1;
    float rcw = 0, rpw = 0, ucw = 0, upw = 0;
    bool curok = velclippixel(oh.curused, t.pos, w, h, s.rcx, s.rcy, rcw);
    bool prevok = oh.haveprevclip && velclippixel(oh.prevused, t.pos, w, h, s.rpx, s.rpy, rpw);
    velclippixel(oh.curunjit, t.pos, w, h, s.ucx, s.ucy, ucw);
    bool upok = oh.haveprevclip && velclippixel(oh.prevunjit, t.pos, w, h, s.upx, s.upy, upw);
    bool hashist = oh.haveprevclip && !velcutframe;
    int curcause = velrigidclassify(idcur, w, h, s.rcx, s.rcy, t.mesh, t.tri, curok, true, true, s.sx, s.sy, s.sampleoff, s.idslot, s.idmesh, s.idtri);
    int psx, psy, pidslot, pidmesh, pidtri;
    float poff;
    int prevcause = velrigidclassify(idprev, w, h, s.rpx, s.rpy, t.mesh, t.tri, prevok, hashist, false, psx, psy, poff, pidslot, pidmesh, pidtri);
    int cause = curcause;
    if(curcause == VELR_VALID && prevcause == VELR_VALID) cause = VELR_VALID;
    else if(curcause == VELR_VALID && prevcause == VELR_OFFSCREEN_PREV) cause = VELR_OFFSCREEN_PREV;
    else if(curcause == VELR_VALID && prevcause == VELR_NO_HISTORY) cause = VELR_NO_HISTORY;
    else if(curcause == VELR_VALID && prevcause != VELR_VALID) cause = VELR_NEWLY_VISIBLE;
    else if(curcause != VELR_VALID && prevcause == VELR_VALID) cause = VELR_DISAPPEARED;
    else cause = curcause;
    if(!upok && cause == VELR_VALID) cause = VELR_NO_HISTORY;
    s.cause = cause;
    int sx = clamp(s.sx, 0, w - 1), sy = clamp(s.sy, 0, h - 1);
    int vidx = (sy * w + sx) * 4;
    s.bufx = velpix[vidx];
    s.bufy = velpix[vidx + 1];
    s.worldpix = velpix[vidx + 2] >= 0.5f && velpix[vidx + 3] < 0.5f;
    s.a1 = velpix[vidx + 3] >= 0.5f;
    s.mag = sqrtf(s.bufx * s.bufx + s.bufy * s.bufy);
    s.expx = s.upx - s.ucx;
    s.expy = s.upy - s.ucy;
    if(cause == VELR_VALID)
    {
        float dx = s.bufx - s.expx, dy = s.bufy - s.expy;
        s.err = sqrtf(dx * dx + dy * dy);
    }
    // B=1/A=0 without this instance's ID is scenery, never valid.
    if(cause == VELR_VALID && s.idslot != VEL_RIGID_SLOT) s.cause = VELR_SCENERY;
}

static velrtri *velrigidfindtri(int mesh, int tri)
{
    loopi(velrigidtris.length())
        if(velrigidtris[i].mesh == mesh && velrigidtris[i].tri == tri)
            return &velrigidtris[i];
    return NULL;
}

static float velrigidaxis(model *rm, const vec &modelpos)
{
    vec p = modelpos;
    if(rm)
    {
        p.add(rm->translate);
        p.mul(rm->scale);
    }
    return sqrtf(p.x*p.x + p.z*p.z);
}

static bool velbary2d(float px, float py, float ax, float ay, float bx, float by, float cx, float cy, float &u, float &v, float &w)
{
    float den = (by - cy)*(ax - cx) + (cx - bx)*(ay - cy);
    if(fabs(den) < 1e-12f) return false;
    u = ((by - cy)*(px - cx) + (cx - bx)*(py - cy)) / den;
    v = ((cy - ay)*(px - cx) + (ax - cx)*(py - cy)) / den;
    w = 1.0f - u - v;
    return true;
}

static void velclampbary(float &u, float &v, float &w)
{
    if(u < 0) u = 0;
    if(v < 0) v = 0;
    if(w < 0) w = 0;
    float s = u + v + w;
    if(s > 1e-8f) { u /= s; v /= s; w /= s; }
    else { u = v = w = 1.0f/3.0f; }
}

static int velrigidpickvis(vector<velrtri> &vis, int wantkind, float axmin, float axmax, int *out, int maxn)
{
    int n = 0;
    float keptx[VEL_RIGID_KEEP], kepty[VEL_RIGID_KEEP];
    int nk = 0;
    for(;;)
    {
        int best = -1;
        float bests = -1;
        loopi(vis.length())
        {
            velrtri &t = vis[i];
            bool blade = t.axisdist >= axmin;
            bool hub = t.axisdist <= axmax;
            if(wantkind == 0 && !blade) continue;
            if(wantkind == 1 && !hub) continue;
            bool farenough = true;
            loopk(nk)
            {
                float dx = t.scx - keptx[k], dy = t.scy - kepty[k];
                if(dx*dx + dy*dy < (wantkind == 0 ? 22.0f*22.0f : 10.0f*10.0f)) { farenough = false; break; }
            }
            if(!farenough) continue;
            bool used = false;
            loopj(n) if(out[j] == i) { used = true; break; }
            if(used) continue;
            float scr = wantkind == 1 ? (1.0f / (t.axisdist + 1e-3f)) : t.axisdist;
            if(scr > bests) { bests = scr; best = i; }
        }
        if(best < 0) break;
        out[n++] = best;
        if(nk < VEL_RIGID_KEEP) { keptx[nk] = vis[best].scx; kepty[nk] = vis[best].scy; nk++; }
        if(n >= maxn) break;
    }
    return n;
}

static void velrigidrun(ImageData &cur, ImageData &prev, ImageData &follow, int w, int h, const float *velpix, const float *idcur, const float *idprev, float djx, float djy, const char *clean)
{
    velrigidresetproof();
    int idx = hwrtveldrive;
    if(idx < 0 || !velobjs.inrange(idx) || !velobjs[idx].valid || !velobjs[idx].haveclip) return;
    const vector<extentity *> &ents = entities::getents();
    if(!ents.inrange(idx) || !ents[idx] || ents[idx]->type != ET_MAPMODEL) return;
    model *rm = loadmapmodel(ents[idx]->attr2);
    if(!rm) return;
    velobjhist &oh = velobjs[idx];
    velrigidtris.setsize(0);
    rm->hwrtvelenumrigidtris();
    int fanx0 = w, fany0 = h, fanx1 = 0, fany1 = 0;
    if(idcur)
    {
        for(int y = 0; y < h; y++) for(int x = 0; x < w; x++)
        {
            int s, m, t; float z;
            velidpixel(idcur, w, h, x, y, s, m, t, z);
            if(s != VEL_RIGID_SLOT) continue;
            velridpixels++;
            fanx0 = min(fanx0, x); fany0 = min(fany0, y);
            fanx1 = max(fanx1, x); fany1 = max(fany1, y);
        }
    }
    if(idprev)
    {
        for(int y = 0; y < h; y += 2) for(int x = 0; x < w; x += 2)
        {
            int s, m, t; float z;
            velidpixel(idprev, w, h, x, y, s, m, t, z);
            if(s == VEL_RIGID_SLOT) velridprev++;
        }
    }
    if(velrigidtris.empty() || !idcur) return;
    vector<velrtri> vis;
    const int step = 2;
    for(int y = 0; y < h; y += step) for(int x = 0; x < w; x += step)
    {
        int s, mesh, tri; float z;
        velidpixel(idcur, w, h, x, y, s, mesh, tri, z);
        if(s != VEL_RIGID_SLOT) continue;
        velrtri *src = velrigidfindtri(mesh, tri);
        if(!src) continue;
        float ax = 0, ay = 0, aw = 0, bx = 0, by = 0, bw = 0, cx = 0, cy = 0, cw = 0;
        velclippixel(oh.curused, src->a, w, h, ax, ay, aw);
        velclippixel(oh.curused, src->b, w, h, bx, by, bw);
        velclippixel(oh.curused, src->c, w, h, cx, cy, cw);
        float u = 1.0f/3.0f, v = 1.0f/3.0f, ww = 1.0f/3.0f;
        velrtri samp = *src;
        if(velbary2d(x + 0.5f, y + 0.5f, ax, ay, bx, by, cx, cy, u, v, ww))
        {
            velclampbary(u, v, ww);
            samp.pos = vec(src->a).mul(u).add(vec(src->b).mul(v)).add(vec(src->c).mul(ww));
        }
        else samp.pos = src->pos;
        samp.axisdist = velrigidaxis(rm, samp.pos);
        samp.scx = x + 0.5f;
        samp.scy = y + 0.5f;
        vis.add(samp);
    }
    if(vis.empty()) return;
    float maxax = 0;
    loopi(vis.length()) if(vis[i].axisdist > maxax) maxax = vis[i].axisdist;
    velr_axismax = maxax;
    velr_axismin = maxax;
    loopi(vis.length()) if(vis[i].axisdist < velr_axismin) velr_axismin = vis[i].axisdist;
    float bladeth = 0.45f * maxax;
    float hubth = 0.22f * maxax;
    int bladeidx[VEL_RIGID_KEEP], hubidx[VEL_RIGID_HUB];
    int nblade = velrigidpickvis(vis, 0, bladeth, hubth, bladeidx, VEL_RIGID_KEEP);
    int nhub = velrigidpickvis(vis, 1, bladeth, hubth, hubidx, VEL_RIGID_HUB);
    velr_nblade = nblade;
    float bladeerr[VEL_RIGID_KEEP], blademag[VEL_RIGID_KEEP];
    int nberr = 0, nbmag = 0;
    loopi(nblade)
    {
        if(velrnstored >= VEL_RIGID_KEEP + VEL_RIGID_HUB) break;
        velrstat &s = velrstored[velrnstored++];
        velrigidfill(s, vis[bladeidx[i]], 0, oh, w, h, velpix, idcur, idprev);
        velr_ncause[clamp(s.cause, 0, VELR_N - 1)]++;
        if(s.cause == VELR_VALID)
        {
            velr_nvalid++;
            velr_nbladevalid++;
            if(nberr < VEL_RIGID_KEEP) bladeerr[nberr++] = s.err;
            if(nbmag < VEL_RIGID_KEEP)
            {
                float em = sqrtf(s.expx * s.expx + s.expy * s.expy);
                blademag[nbmag++] = em;
            }
        }
        int r = 255, g = 220, b = 40;
        if(s.cause == VELR_SCENERY) { r = 255; g = 40; b = 40; }
        else if(s.cause == VELR_OCCLUDED_OTHER_CHAR) { r = 255; g = 40; b = 220; }
        else if(s.cause == VELR_OCCLUDED_OTHER_TRI || s.cause == VELR_OCCLUDED_WORLD) { r = 40; g = 220; b = 255; }
        else if(s.cause != VELR_VALID) { r = 160; g = 160; b = 160; }
        velmark(cur, s.sx, s.sy, r, g, b);
        velmark(prev, int(s.rpx), int(s.rpy), r, g, b);
        velmark(follow, s.sx, s.sy, r, g, b);
        if(s.cause == VELR_VALID)
        {
            int vx = int((s.sx + 0.5f) + s.bufx + djx + (s.bufx + djx >= 0 ? 0.5f : -0.5f));
            int vy = int((s.sy + 0.5f) + s.bufy + djy + (s.bufy + djy >= 0 ? 0.5f : -0.5f));
            velmarkcross(follow, vx + w, vy, r, g, b);
            velmark(follow, int(s.rpx) + w, int(s.rpy), 40, 255, 80);
        }
    }
    loopi(nhub)
    {
        if(velrnstored >= VEL_RIGID_KEEP + VEL_RIGID_HUB) break;
        velrstat &s = velrstored[velrnstored++];
        velrigidfill(s, vis[hubidx[i]], 1, oh, w, h, velpix, idcur, idprev);
        velr_ncause[clamp(s.cause, 0, VELR_N - 1)]++;
        if(s.cause == VELR_VALID)
        {
            velr_nvalid++;
            velr_nhubvalid++;
        }
        int r = 80, g = 200, b = 255;
        if(s.cause != VELR_VALID) { r = 100; g = 100; b = 140; }
        velmark(cur, s.sx, s.sy, r, g, b);
        velmark(prev, int(s.rpx), int(s.rpy), r, g, b);
        velmark(follow, s.sx, s.sy, r, g, b);
    }
    if(nberr > 0)
    {
        qsort(bladeerr, nberr, sizeof(float), velfcmp);
        velr_med = bladeerr[nberr / 2];
        velr_p95 = bladeerr[int(0.95f * (nberr - 1))];
        velr_max = bladeerr[nberr - 1];
    }
    if(nbmag > 0)
    {
        qsort(blademag, nbmag, sizeof(float), velfcmp);
        velr_magmed = blademag[nbmag / 2];
        velr_magp95 = blademag[int(0.95f * (nbmag - 1))];
        velr_magmax = blademag[nbmag - 1];
    }
    int x0 = fanx0, y0 = fany0, x1 = fanx1, y1 = fany1;
    if(x1 > x0 && y1 > y0 && clean && clean[0])
    {
        int pad = 48;
        x0 = max(x0 - pad, 0); y0 = max(y0 - pad, 0);
        x1 = min(x1 + pad, w - 1); y1 = min(y1 + pad, h - 1);
        int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
        if(cw >= 8 && ch >= 8)
        {
            ImageData crop(cw, ch, 3);
            velcopyrect(cur, crop, x0, y0);
            defformatstring(fnblades, "shots/velocity-lot6/%s_blades.png", clean);
            path(fnblades);
            velsavepng(fnblades, crop);
        }
    }
}

static void velzoomblit(const ImageData &src, ImageData &dst, int dx, int dy, int cx, int cy, int zw)
{
    int x0 = cx - zw / 2, y0 = cy - zw / 2;
    loopi(zw) loopj(zw)
    {
        int sx = x0 + j, sy = y0 + i;
        if(sx < 0 || sy < 0 || sx >= src.w || sy >= src.h) velputrgb(dst, dx + j, dy + i, 12, 12, 12);
        else
        {
            uchar *p = src.data + (sy * src.w + sx) * 3;
            velputrgb(dst, dx + j, dy + i, p[0], p[1], p[2]);
        }
    }
}

static void velwriteproof(const char *name, GLuint prevhist)
{
    if(!name || !name[0] || !veltex || velw < 8 || velh < 8) return;
    string clean;
    copystring(clean, name);
    for(char *s = clean; *s; s++) if(*s == '/' || *s == '\\' || *s == ':' || iscubespace(*s)) *s = '_';

    string savedhome;
    copystring(savedhome, homedir);
    homedir[0] = '\0';
    velshotdir();

    int w = velw, h = velh;
    if(w > screenw) w = screenw;
    if(h > screenh) h = screenh;
    ImageData cur(w, h, 3), prev(velw, velh, 3), vecim(w, h, 3), warp(w, h, 3), follow(w * 2, h, 3);
    ImageData sil(w, h, 3), hudsil(w, h, 3), unlitcur(w, h, 3), unlitprev(w, h, 3), unlitwarp(w, h, 3);
    GLint prevfb = 0, prevtex = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevfb);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevtex);

    glBindFramebuffer_(GL_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, cur.data);

    if(prevhist && velw == w && velh == h)
    {
        glBindTexture(GL_TEXTURE_2D, prevhist);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, prev.data);
    }
    else
    {
        memset(prev.data, 0, velw * velh * 3);
        if(prevhist && (velw != w || velh != h))
            conoutf(CON_WARN, "hwrt velocity: proof prev skipped (size mismatch %dx%d vs %dx%d)", velw, velh, w, h);
    }

    float *velpix = new float[w * h * 4];
    uchar *stpix = new uchar[w * h];
    glBindFramebuffer_(GL_FRAMEBUFFER, velfb);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_FLOAT, velpix);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, stpix);
    bool haveunlit = velunlitok && velunlittex[0] && velunlittex[1];
    if(haveunlit)
    {
        glBindTexture(GL_TEXTURE_2D, velunlittex[0]);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, unlitcur.data);
        glBindTexture(GL_TEXTURE_2D, velunlittex[1]);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, unlitprev.data);
    }
    else
    {
        memset(unlitcur.data, 0, w * h * 3);
        memset(unlitprev.data, 0, w * h * 3);
    }
    float *idcur = NULL, *idprev = NULL;
    bool haveid = velidok && velidtex[0] && velidtex[1];
    if(haveid)
    {
        idcur = new float[w * h * 4];
        idprev = new float[w * h * 4];
        glBindTexture(GL_TEXTURE_2D, velidtex[0]);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, idcur);
        glBindTexture(GL_TEXTURE_2D, velidtex[1]);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, idprev);
    }
    int ncands = veltrackcands.length();
    veltrackfinalize(idcur, w, h);
    glBindFramebuffer_(GL_FRAMEBUFFER, prevfb);
    glBindTexture(GL_TEXTURE_2D, prevtex);

    defformatstring(fncur, "shots/velocity-lot6/%s_cur.png", clean);
    defformatstring(fnprev, "shots/velocity-lot6/%s_prev.png", clean);
    defformatstring(fnvec, "shots/velocity-lot6/%s_vec.png", clean);
    defformatstring(fnwarp, "shots/velocity-lot6/%s_warp.png", clean);
    defformatstring(fnfollow, "shots/velocity-lot6/%s_follow.png", clean);
    defformatstring(fnsil, "shots/velocity-lot6/%s_sil.png", clean);
    defformatstring(fnhudsil, "shots/velocity-lot6/%s_hudsil.png", clean);
    defformatstring(fnunlit, "shots/velocity-lot6/%s_unlit.png", clean);
    defformatstring(fnunlitw, "shots/velocity-lot6/%s_unlit_warp.png", clean);
    defformatstring(fnunlitc, "shots/velocity-lot6/%s_unlit_vs_colour.png", clean);
    defformatstring(fnid, "shots/velocity-lot6/%s_id.png", clean);
    defformatstring(fnzoom, "shots/velocity-lot6/%s_zoom.png", clean);
    defformatstring(fninfo, "shots/velocity-lot6/%s_info.txt", clean);
    path(fncur); path(fnprev); path(fnvec); path(fnwarp); path(fnfollow); path(fnsil); path(fnhudsil); path(fnunlit); path(fnunlitw); path(fninfo);

    float scale = 1.0f / hwrtvelscale;
    int ncover = 0, ncover_world = 0, ncover_char = 0, nmove = 0, nskel = 0;
    int nstskel = 0, nboth = 0, nvelonly = 0, nskelonly = 0, nsilbbox = 0;
    int nsthist[5] = { 0, 0, 0, 0, 0 }, nstother = 0;
    int silx0 = w, sily0 = h, silx1 = 0, sily1 = 0;
    uchar *chud = new uchar[w * h];
    uchar *vhud = new uchar[w * h];
    memset(chud, 0, w * h);
    memset(vhud, 0, w * h);
    bool havehudcol = velhudcolok && velhudcolst && velhudcolw == w && velhudcolh == h;
    double eunlit = 0;
    int nunlit = 0, nunlitpix = 0, nunlitst = 0;
    float jcx = 0, jcy = 0, jpx = 0, jpy = 0;
    hwrttemporaljitterpixels(jcx, jcy, jpx, jpy);
    double sumabsv = 0, maxabsv = 0, ejitonly = 0;
    int nbelow005 = 0, nbelow025 = 0, njitonly = 0;
    loopi(h) loopj(w)
    {
        int idx = (i * w + j) * 4;
        float vx = velpix[idx], vy = velpix[idx + 1], cov = velpix[idx + 2];
        int r = 184, g = 0, b = 173;
        bool a1 = velpix[idx + 3] >= 0.5f;
        int stv = stpix[i * w + j];
        if(stv >= 0 && stv <= 4) nsthist[stv]++;
        else nstother++;
        bool st = stv == VEL_ST_SKEL;
        if(cov >= 0.5f)
        {
            r = clamp(int(vx * scale * 255.0f + 127.5f), 0, 255);
            g = clamp(int(vy * scale * 255.0f + 127.5f), 0, 255);
            b = 31;
            ncover++;
            if(a1) { nskel++; ncover_char++; }
            else ncover_world++;
            float mag = sqrtf(vx * vx + vy * vy);
            if(mag > 0.8f) nmove++;
            sumabsv += mag;
            if(mag > maxabsv) maxabsv = mag;
            if(mag < 0.05f) nbelow005++;
            if(mag < 0.25f) nbelow025++;
        }
        if(st) nstskel++;
        if(havehudcol && velhudcolst[i * w + j] == VEL_ST_HUDCOL) chud[i * w + j] = 1;
        if(a1) vhud[i * w + j] = 1;
        if(st && a1) nboth++;
        else if(a1 && !st) nvelonly++;
        else if(st && !a1) nskelonly++;
        if(st || a1)
        {
            silx0 = min(silx0, j); sily0 = min(sily0, i);
            silx1 = max(silx1, j); sily1 = max(sily1, i);
            nsilbbox++;
        }
        // Independent colour-pass silhouette (stencil 3) vs velocity A=1.
        // Green = both, red = velocity geometry only, blue = colour character only.
        int sr = 20, sg = 20, sb = 20;
        int cr0 = cur.data[(i * w + j) * 3], cg0 = cur.data[(i * w + j) * 3 + 1], cb0 = cur.data[(i * w + j) * 3 + 2];
        sr = cr0 / 3; sg = cg0 / 3; sb = cb0 / 3;
        if(st && a1) { sr = 40; sg = 200; sb = 70; }
        else if(a1) { sr = 220; sg = 40; sb = 40; }
        else if(st) { sr = 50; sg = 90; sb = 230; }
        velputrgb(sil, j, i, sr, sg, sb);
        velputrgb(vecim, j, i, r, g, b);
        velputrgb(follow, j, i, cr0, cg0, cb0);
        int pr = prev.data[(i * w + j) * 3], pg = prev.data[(i * w + j) * 3 + 1], pb = prev.data[(i * w + j) * 3 + 2];
        velputrgb(follow, j + w, i, pr, pg, pb);
        if(cov >= 0.5f)
        {
            float pu = (j + 0.5f) / w + vx / w + (jpx - jcx) / w;
            float pv = (i + 0.5f) / h + vy / h + (jpy - jcy) / h;
            int samp[3];
            if(pu < 0 || pu > 1 || pv < 0 || pv > 1) { samp[0] = 12; samp[1] = 46; samp[2] = 71; }
            else velsampleraw(prev, pu, pv, samp);
            velputrgb(warp, j, i, samp[0], samp[1], samp[2]);
            float ju = (j + 0.5f) / w + (jpx - jcx) / w;
            float jv = (i + 0.5f) / h + (jpy - jcy) / h;
            int jsamp[3];
            if(ju < 0 || ju > 1 || jv < 0 || jv > 1) { jsamp[0] = 12; jsamp[1] = 46; jsamp[2] = 71; }
            else velsampleraw(prev, ju, jv, jsamp);
            ejitonly += abs(cr0 - jsamp[0]) + abs(cg0 - jsamp[1]) + abs(cb0 - jsamp[2]);
            njitonly++;
        }
        else velputrgb(warp, j, i, 184, 0, 173);
        if(haveunlit)
        {
            int ur = unlitcur.data[(i * w + j) * 3], ug = unlitcur.data[(i * w + j) * 3 + 1], ub = unlitcur.data[(i * w + j) * 3 + 2];
            if(ur + ug + ub > 8)
            {
                nunlitpix++;
                if(st) nunlitst++;
            }
            if(st && a1)
            {
                float pu = (j + 0.5f) / w + vx / w + (jpx - jcx) / w;
                float pv = (i + 0.5f) / h + vy / h + (jpy - jcy) / h;
                int samp[3];
                if(pu < 0 || pu > 1 || pv < 0 || pv > 1) { samp[0] = 0; samp[1] = 0; samp[2] = 0; }
                else velsampleraw(unlitprev, pu, pv, samp);
                velputrgb(unlitwarp, j, i, samp[0], samp[1], samp[2]);
                eunlit += abs(ur - samp[0]) + abs(ug - samp[1]) + abs(ub - samp[2]);
                nunlit++;
            }
            else velputrgb(unlitwarp, j, i, ur, ug, ub);
        }
        else velputrgb(unlitwarp, j, i, 0, 0, 0);
    }

    int hud_both = 0, hud_velonly = 0, hud_colonly = 0;
    int hud_vel_edge = 0, hud_vel_int = 0, hud_col_edge = 0, hud_col_int = 0;
    loopi(h) loopj(w)
    {
        int cr0 = cur.data[(i * w + j) * 3], cg0 = cur.data[(i * w + j) * 3 + 1], cb0 = cur.data[(i * w + j) * 3 + 2];
        int sr = cr0 / 3, sg = cg0 / 3, sb = cb0 / 3;
        bool c = chud[i * w + j] != 0;
        bool v = vhud[i * w + j] != 0;
        bool edge = false;
        if(c != v)
        {
            for(int dy = -1; dy <= 1 && !edge; dy++)
                for(int dx = -1; dx <= 1; dx++)
                {
                    if(!dx && !dy) continue;
                    int nx = j + dx, ny = i + dy;
                    if(nx < 0 || ny < 0 || nx >= w || ny >= h) { edge = true; break; }
                    if(!chud[ny * w + nx] && !vhud[ny * w + nx]) { edge = true; break; }
                }
        }
        if(c && v)
        {
            hud_both++;
            sr = 40; sg = 200; sb = 70;
        }
        else if(v)
        {
            hud_velonly++;
            if(edge) { hud_vel_edge++; sr = 210; sg = 50; sb = 40; }
            else { hud_vel_int++; sr = 255; sg = 0; sb = 220; }
        }
        else if(c)
        {
            hud_colonly++;
            if(edge) { hud_col_edge++; sr = 50; sg = 90; sb = 230; }
            else { hud_col_int++; sr = 255; sg = 220; sb = 40; }
        }
        velputrgb(hudsil, j, i, sr, sg, sb);
    }

    struct velpt pts[8];
    int npt = velpicksamples(velpix, w, h, pts, 8, true);
    if(npt < 3) npt = velpicksamples(velpix, w, h, pts, 8, false);
    static const int cols[8][3] = {
        {255, 255, 0}, {0, 255, 255}, {255, 80, 80}, {255, 160, 0},
        {80, 255, 80}, {255, 0, 255}, {160, 255, 80}, {80, 160, 255}
    };
    double ewarp = 0, eident = 0;
    int nerr = 0;
    loopi(npt)
    {
        int x = pts[i].x, y = pts[i].y;
        int samp[3];
        velsampleraw(prev, (pts[i].px + 0.5f) / w, (pts[i].py + 0.5f) / h, samp);
        int cr = cur.data[(y * w + x) * 3], cg = cur.data[(y * w + x) * 3 + 1], cb = cur.data[(y * w + x) * 3 + 2];
        int pr = prev.data[(y * w + x) * 3], pg = prev.data[(y * w + x) * 3 + 1], pb = prev.data[(y * w + x) * 3 + 2];
        ewarp += abs(cr - samp[0]) + abs(cg - samp[1]) + abs(cb - samp[2]);
        eident += abs(cr - pr) + abs(cg - pg) + abs(cb - pb);
        nerr++;
    }
    loopi(npt)
    {
        int r = cols[i][0], g = cols[i][1], b = cols[i][2];
        velmark(cur, pts[i].x, pts[i].y, r, g, b);
        velmark(prev, pts[i].px, pts[i].py, r, g, b);
        velmark(warp, pts[i].x, pts[i].y, r, g, b);
        velmark(follow, pts[i].x, pts[i].y, r, g, b);
        velmark(follow, pts[i].px + w, pts[i].py, r, g, b);
    }

    struct veltrstat
    {
        int slot, region, mesh, tri, cause, cx, cy, px, py, vxpx, vypx, sx, sy, idslot;
        float err, mag, score, sampleoff, expx, expy, cxf, cyf, pxf, pyf, area;
        float rastererr, rexpx, rexpy, ucx, ucy, upx, upy, bufx, bufy;
    };
    veltrstat trs[VELTR_KEEP_MAX];
    int ntr = 0;
    int ncause[VELTR_CAUSE_N];
    memset(ncause, 0, sizeof(ncause));
    float validerr[VELTR_KEEP_MAX];
    float validoff[VELTR_KEEP_MAX];
    float validraster[VELTR_KEEP_MAX];
    int nvalid = 0;
    float djx = jpx - jcx, djy = jpy - jcy;
    loopi(veltrackn)
    {
        veltrack &t = veltracks[i];
        if(t.slot < 0 || t.slot >= velskeln) continue;
        velskelhist &hs = velskels[t.slot];
        float rcx, rcy, rcw, rcz, rpx, rpy, rpw, rpz;
        float ucx, ucy, ucw, ucz, upx, upy, upw, upz;
        bool curok = velclippixel(hs.curmm, t.cur, w, h, rcx, rcy, rcw, &rcz);
        bool prevok = velclippixel(hs.prevmm, t.prev, w, h, rpx, rpy, rpw, &rpz);
        velclippixel(hs.curmmunjit, t.cur, w, h, ucx, ucy, ucw, &ucz);
        velclippixel(hs.prevmmunjit, t.prev, w, h, upx, upy, upw, &upz);
        bool hashist = hs.usedprev;
        int curcause = velidcause(idcur, w, h, rcx, rcy, t.slot, t.mesh, t.tri, stpix, curok, true, true);
        int prevcause = velidcause(idprev, w, h, rpx, rpy, t.slot, t.mesh, t.tri, NULL, prevok, hashist, false);
        int cause = curcause;
        if(curcause == VELTR_VALID && prevcause == VELTR_VALID) cause = VELTR_VALID;
        else if(curcause == VELTR_VALID && prevcause == VELTR_OFFSCREEN_PREV) cause = VELTR_OFFSCREEN_PREV;
        else if(curcause == VELTR_VALID && prevcause != VELTR_VALID) cause = VELTR_NEWLY_VISIBLE;
        else if(curcause != VELTR_VALID && prevcause == VELTR_VALID) cause = VELTR_DISAPPEARED;
        else cause = curcause;
        int sx = int(floor(rcx)), sy = int(floor(rcy));
        float sampleoff = 0, zid = 1;
        if(cause == VELTR_VALID || curcause == VELTR_VALID)
        {
            int ox = sx, oy = sy;
            if(!velidfind(idcur, w, h, rcx, rcy, t.slot, t.mesh, t.tri, 2, ox, oy, zid, sampleoff))
            {
                ox = sx;
                oy = sy;
                sampleoff = sqrtf(((sx + 0.5f) - rcx)*((sx + 0.5f) - rcx) + ((sy + 0.5f) - rcy)*((sy + 0.5f) - rcy));
            }
            sx = ox;
            sy = oy;
        }
        else sampleoff = sqrtf(((sx + 0.5f) - rcx)*((sx + 0.5f) - rcx) + ((sy + 0.5f) - rcy)*((sy + 0.5f) - rcy));
        int vidx = (clamp(sy, 0, h - 1) * w + clamp(sx, 0, w - 1)) * 4;
        float vx = velpix[vidx], vy = velpix[vidx + 1];
        float vxpx = (sx + 0.5f) + vx + djx, vypx = (sy + 0.5f) + vy + djy;
        float expx = upx - ucx, expy = upy - ucy;
        float rexpx = rpx - rcx, rexpy = rpy - rcy;
        int idslot = -1, idmesh = -1, idtri = -1;
        float idz = 1;
        velidpixel(idcur, w, h, int(floor(rcx)), int(floor(rcy)), idslot, idmesh, idtri, idz);
        float err = -1, rastererr = -1;
        if(cause == VELTR_VALID)
        {
            float dx = vx - expx, dy = vy - expy;
            err = sqrtf(dx * dx + dy * dy);
            float rdx = vx - rexpx, rdy = vy - rexpy;
            rastererr = sqrtf(rdx * rdx + rdy * rdy);
            if(nvalid < VELTR_KEEP_MAX)
            {
                validerr[nvalid] = err;
                validoff[nvalid] = sampleoff;
                validraster[nvalid] = rastererr;
                nvalid++;
            }
        }
        if(ntr >= VELTR_KEEP_MAX) continue;
        veltrstat &s = trs[ntr++];
        s.slot = t.slot; s.region = t.region; s.mesh = t.mesh; s.tri = t.tri;
        s.cause = cause;
        s.cx = int(rcx); s.cy = int(rcy); s.px = int(rpx); s.py = int(rpy);
        s.sx = sx; s.sy = sy;
        s.vxpx = int(vxpx + (vxpx >= 0 ? 0.5f : -0.5f));
        s.vypx = int(vypx + (vypx >= 0 ? 0.5f : -0.5f));
        s.err = err; s.score = t.score; s.area = t.area;
        s.mag = sqrtf(vx * vx + vy * vy);
        s.sampleoff = sampleoff;
        s.expx = expx; s.expy = expy;
        s.cxf = rcx; s.cyf = rcy; s.pxf = rpx; s.pyf = rpy;
        s.idslot = idslot;
        s.rastererr = rastererr; s.rexpx = rexpx; s.rexpy = rexpy;
        s.ucx = ucx; s.ucy = ucy; s.upx = upx; s.upy = upy;
        s.bufx = vx; s.bufy = vy;
        ncause[clamp(cause, 0, VELTR_CAUSE_N - 1)]++;
        int r = cols[min(t.region, 7)][0], g = cols[min(t.region, 7)][1], b = cols[min(t.region, 7)][2];
        if(cause == VELTR_OCCLUDED_OTHER_CHAR)
        {
            velmark(sil, int(rcx), int(rcy), 255, 40, 220);
            velmark(follow, int(rcx), int(rcy), 255, 40, 220);
            velmark(cur, int(rcx), int(rcy), 255, 40, 220);
        }
        else if(cause == VELTR_OCCLUDED_WORLD)
        {
            velmark(sil, int(rcx), int(rcy), 40, 220, 255);
            velmark(follow, int(rcx), int(rcy), 40, 220, 255);
            velmark(cur, int(rcx), int(rcy), 40, 220, 255);
        }
        else if(cause == VELTR_VALID)
        {
            velmark(sil, sx, sy, r, g, b);
            velmark(hudsil, sx, sy, r, g, b);
            velmark(follow, sx, sy, r, g, b);
            velmark(follow, s.px + w, s.py, r, g, b);
            velmarkcross(follow, s.vxpx + w, s.vypx, r, g, b);
        }
    }

    float med = -1, p95 = -1, mx = -1, medoff = -1, medraster = -1;
    if(nvalid > 0)
    {
        qsort(validerr, nvalid, sizeof(float), velfcmp);
        qsort(validoff, nvalid, sizeof(float), velfcmp);
        qsort(validraster, nvalid, sizeof(float), velfcmp);
        med = validerr[nvalid / 2];
        p95 = validerr[int(0.95f * (nvalid - 1))];
        mx = validerr[nvalid - 1];
        medoff = validoff[nvalid / 2];
        medraster = validraster[nvalid / 2];
    }

    velrigidrun(cur, prev, follow, w, h, velpix, idcur, idprev, djx, djy, clean);

    velsavepng(fncur, cur);
    velsavepng(fnprev, prev);
    velsavepng(fnvec, vecim);
    velsavepng(fnwarp, warp);
    velsavepng(fnfollow, follow);
    velsavepng(fnsil, sil);
    if(havehudcol) velsavepng(fnhudsil, hudsil);
    if(haveunlit)
    {
        ImageData unlitpair(w * 2, h, 3);
        ImageData unlitcmp(w * 2, h, 3);
        loopi(h) loopj(w)
        {
            uchar *c = unlitcur.data + (i * w + j) * 3;
            uchar *p = unlitprev.data + (i * w + j) * 3;
            uchar *col = cur.data + (i * w + j) * 3;
            velputrgb(unlitpair, j, i, c[0], c[1], c[2]);
            velputrgb(unlitpair, j + w, i, p[0], p[1], p[2]);
            velputrgb(unlitcmp, j, i, col[0], col[1], col[2]);
            velputrgb(unlitcmp, j + w, i, c[0], c[1], c[2]);
        }
        velsavepng(fnunlit, unlitpair);
        velsavepng(fnunlitw, unlitwarp);
        path(fnunlitc);
        velsavepng(fnunlitc, unlitcmp);
    }
    if(haveid && idcur && idprev)
    {
        ImageData idim(w * 2, h, 3);
        loopi(h) loopj(w)
        {
            int s, m, t;
            float z;
            velidpixel(idcur, w, h, j, i, s, m, t, z);
            int r = s < 0 ? 20 : 40 + (s * 90) % 200;
            int g = s < 0 ? 20 : 40 + (t * 13) % 200;
            int b = s < 0 ? 20 : 40 + (m * 70) % 200;
            velputrgb(idim, j, i, r, g, b);
            velidpixel(idprev, w, h, j, i, s, m, t, z);
            r = s < 0 ? 20 : 40 + (s * 90) % 200;
            g = s < 0 ? 20 : 40 + (t * 13) % 200;
            b = s < 0 ? 20 : 40 + (m * 70) % 200;
            velputrgb(idim, j + w, i, r, g, b);
        }
        path(fnid);
        velsavepng(fnid, idim);
    }
    {
        const int zw = 96, ncol = 6, nrow = 2;
        ImageData zoom(zw * ncol, zw * nrow, 3);
        memset(zoom.data, 20, zoom.w * zoom.h * 3);
        int pick[6], npick = 0;
        auto tryadd = [&](int idx)
        {
            if(npick >= 6 || idx < 0) return;
            loopk(npick) if(pick[k] == idx) return;
            pick[npick++] = idx;
        };
        loopi(ntr) if(trs[i].cause == VELTR_OCCLUDED_OTHER_CHAR) tryadd(i);
        loopi(ntr) if(trs[i].cause == VELTR_OCCLUDED_WORLD) tryadd(i);
        loopi(ntr) if(trs[i].cause == VELTR_VALID && trs[i].region >= 5) tryadd(i);
        loopi(ntr) if(npick < 5 && trs[i].cause == VELTR_VALID)
        {
            bool have = false;
            loopk(npick) if(trs[pick[k]].cause == VELTR_VALID && trs[pick[k]].region == trs[i].region && trs[pick[k]].slot == trs[i].slot) have = true;
            if(!have) tryadd(i);
        }
        int worst = -1;
        loopi(ntr) if(trs[i].cause == VELTR_VALID && (worst < 0 || trs[i].err > trs[worst].err)) worst = i;
        tryadd(worst);
        loopi(npick)
        {
            veltrstat &s = trs[pick[i]];
            int cx = s.cause == VELTR_VALID ? s.sx : s.cx;
            int cy = s.cause == VELTR_VALID ? s.sy : s.cy;
            velzoomblit(follow, zoom, i * zw, 0, cx, cy, zw);
            velzoomblit(follow, zoom, i * zw, zw, s.px + w, s.py, zw);
            if(s.cause == VELTR_OCCLUDED_OTHER_CHAR)
            {
                velmark(zoom, i * zw + zw / 2, zw / 2, 255, 40, 220);
                velmark(zoom, i * zw + zw / 2, zw + zw / 2, 255, 40, 220);
            }
            else if(s.cause == VELTR_OCCLUDED_WORLD)
            {
                velmark(zoom, i * zw + zw / 2, zw / 2, 40, 220, 255);
                velmark(zoom, i * zw + zw / 2, zw + zw / 2, 40, 220, 255);
            }
            else
            {
                velmark(zoom, i * zw + zw / 2, zw / 2, 255, 255, 0);
                int lx = i * zw + zw / 2 + (s.vxpx - s.px), ly = zw + zw / 2 + (s.vypx - s.py);
                velmarkcross(zoom, lx, ly, 255, 40, 40);
                velmark(zoom, i * zw + zw / 2, zw + zw / 2, 40, 255, 80);
            }
        }
        path(fnzoom);
        velsavepng(fnzoom, zoom);
    }
    if(nsilbbox && silx1 > silx0 && sily1 > sily0)
    {
        int pad = 48;
        int x0 = max(silx0 - pad, 0), y0 = max(sily0 - pad, 0);
        int x1 = min(silx1 + pad, w - 1), y1 = min(sily1 + pad, h - 1);
        int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
        if(cw >= 8 && ch >= 8)
        {
            ImageData crop(cw, ch, 3), cropf(cw * 2, ch, 3), crops(cw, ch, 3);
            velcopyrect(cur, crop, x0, y0);
            velcopyrect(sil, crops, x0, y0);
            loopi(ch) loopj(cw)
            {
                uchar *l = follow.data + ((y0 + i) * follow.w + (x0 + j)) * 3;
                uchar *r = follow.data + ((y0 + i) * follow.w + (x0 + j + w)) * 3;
                velputrgb(cropf, j, i, l[0], l[1], l[2]);
                velputrgb(cropf, j + cw, i, r[0], r[1], r[2]);
            }
            defformatstring(fncrop, "shots/velocity-lot6/%s_crop.png", clean);
            defformatstring(fnsilc, "shots/velocity-lot6/%s_sil_crop.png", clean);
            defformatstring(fnfc, "shots/velocity-lot6/%s_follow_crop.png", clean);
            path(fncrop); path(fnsilc); path(fnfc);
            velsavepng(fncrop, crop);
            velsavepng(fnsilc, crops);
            velsavepng(fnfc, cropf);
        }
    }

    int frontslot = -1;
    float frontd = 1e9f;
    if(camera1) loopi(velskeln)
    {
        velskelhist &h = velskels[i];
        if(!h.noted || h.notedframe != velskelframe || !h.d) continue;
        if(h.attachkey) continue;
        float d = camera1->o.dist(h.pos);
        if(d < frontd) { frontd = d; frontslot = i; }
    }
    int n_on = 0, n_off = 0, n_world = 0, n_otherchar = 0, n_valid_front = 0, n_valid_back = 0;
    int bx0 = w, by0 = h, bx1 = 0, by1 = 0;
    loopi(ntr)
    {
        bool on = trs[i].cx >= 0 && trs[i].cy >= 0 && trs[i].cx < w && trs[i].cy < h;
        if(on)
        {
            n_on++;
            bx0 = min(bx0, trs[i].cx); by0 = min(by0, trs[i].cy);
            bx1 = max(bx1, trs[i].cx); by1 = max(by1, trs[i].cy);
        }
        else n_off++;
        if(trs[i].cause == VELTR_OCCLUDED_WORLD) n_world++;
        if(trs[i].cause == VELTR_OCCLUDED_OTHER_CHAR) n_otherchar++;
        if(trs[i].cause == VELTR_VALID)
        {
            if(frontslot >= 0 && trs[i].slot < velskeln && velskels[trs[i].slot].d == velskels[frontslot].d) n_valid_front++;
            else n_valid_back++;
        }
    }
    int padb = 28;
    if(n_on) { bx0 = max(bx0 - padb, 0); by0 = max(by0 - padb, 0); bx1 = min(bx1 + padb, w - 1); by1 = min(by1 + padb, h - 1); }
    int boxpix = 0, punch_a1 = 0, box_cover = 0;
    if(n_on && bx1 > bx0 && by1 > by0)
    {
        for(int y = by0; y <= by1; y++) for(int x = bx0; x <= bx1; x++)
        {
            int idx = (y * w + x) * 4;
            boxpix++;
            if(velpix[idx + 2] >= 0.5f) box_cover++;
            if(velpix[idx + 3] >= 0.5f) punch_a1++;
        }
        int cw = bx1 - bx0 + 1, ch = by1 - by0 + 1;
        ImageData occbox(cw * 3, ch, 3);
        loopi(ch) loopj(cw)
        {
            int x = bx0 + j, y = by0 + i;
            uchar *c = cur.data + (y * w + x) * 3;
            velputrgb(occbox, j, i, c[0], c[1], c[2]);
            int idx = (y * w + x) * 4;
            int r = 184, g = 0, b = 173;
            if(velpix[idx + 2] >= 0.5f)
            {
                r = clamp(int(velpix[idx] / hwrtvelscale * 255.0f + 127.5f), 0, 255);
                g = clamp(int(velpix[idx + 1] / hwrtvelscale * 255.0f + 127.5f), 0, 255);
                b = 31;
                if(velpix[idx + 3] >= 0.5f) { r = 255; g = 40; b = 40; }
            }
            velputrgb(occbox, j + cw, i, r, g, b);
            int sr = 20, sg = 20, sb = 20;
            if(idcur)
            {
                int s, m, t; float z;
                velidpixel(idcur, w, h, x, y, s, m, t, z);
                if(s >= 0) { sr = 40 + (s * 90) % 200; sg = 40 + (t * 13) % 200; sb = 40 + (m * 70) % 200; }
            }
            velputrgb(occbox, j + 2 * cw, i, sr, sg, sb);
        }
        defformatstring(fnocc, "shots/velocity-lot6/%s_occbox.png", clean);
        path(fnocc);
        velsavepng(fnocc, occbox);
    }
    int ov_id_front = 0, ov_id_back = 0, ov_a1_front = 0, ov_a1_back = 0, ov_n = 0;
    loopi(ntr) if(trs[i].cause == VELTR_OCCLUDED_OTHER_CHAR)
    {
        int x0 = max(trs[i].cx - 8, 0), y0 = max(trs[i].cy - 8, 0);
        int x1 = min(trs[i].cx + 8, w - 1), y1 = min(trs[i].cy + 8, h - 1);
        for(int y = y0; y <= y1; y++) for(int x = x0; x <= x1; x++)
        {
            ov_n++;
            int s, m, t; float z;
            velidpixel(idcur, w, h, x, y, s, m, t, z);
            int a1 = velpix[(y * w + x) * 4 + 3] >= 0.5f;
            if(s >= 0 && s < velskeln && frontslot >= 0 && velskels[s].d && velskels[s].d == velskels[frontslot].d) { ov_id_front++; if(a1) ov_a1_front++; }
            if(s >= 0 && s < velskeln && trs[i].slot >= 0 && trs[i].slot < velskeln && velskels[s].d && velskels[s].d == velskels[trs[i].slot].d) { ov_id_back++; if(a1) ov_a1_back++; }
        }
    }

    FILE *tf = fopen(fninfo, "w");
    if(tf)
    {
        fprintf(tf, "name %s\n", clean);
        fprintf(tf, "lastmillis %d prev_frame_millis %d totalmillis %d curtime %d\n", lastmillis, velprevmillis, totalmillis, curtime);
        fprintf(tf, "frame_pair current_colour=%s previous_colour=%s vectors=%s warp=%s sil=%s blades=shots/velocity-lot6/%s_blades.png\n", fncur, fnprev, fnvec, fnwarp, fnsil, clean);
        fprintf(tf, "debug %d overlaymode %d histusable %s histhas %s histfresh %s cut %s ready %s\n",
                int(hwrtveldebug), veldbgmode ? veldbgmode : int(hwrtveldebug),
                velhistusable() ? "yes" : "no", velhisthas ? "yes" : "no", velhistfresh ? "yes" : "no",
                velcutframe ? "yes" : "no", velready ? "yes" : "no");
        fprintf(tf, "camera %.4f %.4f %.4f yaw %.4f pitch %.4f hold %d\n",
                camera1 ? camera1->o.x : 0, camera1 ? camera1->o.y : 0, camera1 ? camera1->o.z : 0,
                camera1 ? camera1->yaw : 0, camera1 ? camera1->pitch : 0, int(hwrtvelholdcam));
        if(velprevcamok)
            fprintf(tf, "prev_cam %.4f %.4f %.4f yaw %.4f pitch %.4f dist %.4f (stored at end of previous velocity frame)\n",
                    velprevcam.x, velprevcam.y, velprevcam.z, velprevyaw, velprevpitch,
                    camera1 ? camera1->o.dist(velprevcam) : 0);
        extern int gpuskel;
        fprintf(tf, "gpuskel_cvar %d  colour_and_vectors_same_two_frames lastmillis %d <- %d\n", int(gpuskel), lastmillis, velprevmillis);
        fprintf(tf, "camproj_current captured after setcamprojmatrix; character pass replays the colour GL render with stored previous bones (not lastmillis rewind)\n");
        hwrttemporalfprint(tf);
        {
            const hwrttemporalinput *tin = hwrttemporalcurrent();
            fprintf(tf, "resources screen %dx%d velocity %dx%d color_capture %dx%d veltex %u colortex %u depthtex %u overlay %d first_alloc_millis %d last_resize_millis %d now %d\n",
                    screenw, screenh, velw, velh,
                    tin ? tin->width : 0, tin ? tin->height : 0,
                    (unsigned)hwrtvelocitytexid(),
                    tin ? (unsigned)tin->colortex : 0, tin ? (unsigned)tin->depthtex : 0,
                    int(hwrtveldebug), velallocmillis, velresizemillis, lastmillis);
        }
        fprintf(tf, "stencil_velfb empty %d cover %d exclude %d skel %d hudcol %d other %d  (histogram of the velocity FBO after HUD; world generate writes only EQUAL COVER)\n",
                nsthist[0], nsthist[1], nsthist[2], nsthist[3], nsthist[4], nstother);
        loopi(velstsnapn)
        {
            velstsnap &s = velstsnaps[i];
            fprintf(tf, "stencil_snap %s fb %d nread %d empty %d cover %d exclude %d skel %d hudcol %d other %d sten_on %d func 0x%x ref %d wmask %d zpass 0x%x velmode %d active %d pending %d shadow %d glare %d drawtex %d gl_matches_engine %s\n",
                    s.tag, s.fb, s.nread, s.n0, s.n1, s.n2, s.n3, s.n4, s.nother,
                    s.sten_on, s.func, s.ref, s.wmask, s.zpass, s.velmode, s.velactive, s.coverpending,
                    s.shadowmapping, s.glaring, s.drawtex, s.gl_matches_engine ? "yes" : "no");
        }
        fprintf(tf, "mv_covered %d  mean_abs %.4f px  max %.4f  below_0.05 %d  below_0.25 %d  (unjittered; still+jitter should sit near 0 without forcing zeros)\n",
                ncover, ncover ? sumabsv / ncover : 0, maxabsv, nbelow005, nbelow025);
        fprintf(tf, "jitter_only_warp_mean_abs_rgb %.4f  samples %d  (prev colour shifted by programmed jitter only — independent of the velocity shader)\n",
                njitonly ? ejitonly / njitonly : 0, njitonly);
        fprintf(tf, "supported_model mrfixit (playermodel 0) third-person / bot ENT_PLAYER plus attached armour (LINK_REUSE) and vwep (LINK_TAG). Horns share the armour path. First-person hudgun (lot 5) uses distinct slots and the avatar FOV / avatardepth projection.\n");
        fprintf(tf, "excluded ragdolls, first-person body-as-occluder, other playermodel attachments, monsters, deforming mapmodels, water, particles\n");
        fprintf(tf, "camproj_hud current avatar FOV %.3f prev_avatar_FOV %s%.3f zoomprogress %.3f avatardepth (colour gun uses the same projection; previous clip uses stored avatar camproj, not world camproj)\n",
                curavatarfov, velprevavatarfovok ? "" : "none ", velprevavatarfovok ? velprevavatarfov : 0, getzoomprogress());
        fprintf(tf, "skel_pixels %d drawn_bodies %d drawn_equip %d drawn_hud %d pose_slots %d bone_bytes %d cpu_skin %.3f ms gpu_skel %.3f ms hwrtvelequip %d hwrtvelhud %d\n",
                nskel, velskeldrawn, velequipdrawn, velhuddrawn, velskeln, hwrtvelskelmem, hwrtvelskelcpu, hwrtvelskelcost, int(hwrtvelequip), int(hwrtvelhud));
        loopi(velskeln)
        {
            velskelhist &h = velskels[i];
            if(!h.d || !h.m) continue;
            fprintf(tf, "skel_slot %d kind %s key %d model %s noted %s usedprev %s hist %s bones %d verts %d vweights %d vblends %d gpuskin %s bonehash %08x uid %d seq %d pos %.3f %.3f %.3f prevpos %.3f %.3f %.3f yaw %.2f pitch %.2f anim %d natt %d\n",
                    i, velkindname(h.attachkind), h.attachkey, h.m->name ? h.m->name : "?",
                    h.noted && h.notedframe==velskelframe ? "yes" : "no",
                    h.usedprev ? "yes" : "no", h.validprev ? "yes" : "no", h.nbones, h.nverts, h.vweights, h.vblends, h.gpuskin ? "yes" : "no",
                    velhashbones(h.bones, h.nbones), h.uid, h.seq,
                    h.pos.x, h.pos.y, h.pos.z, h.prevpos.x, h.prevpos.y, h.prevpos.z, h.yaw, h.pitch, h.anim, h.natt);
        }
        fprintf(tf, "covered %d world_B1_A0 %d char_B1_A1 %d skel_a1 %d colour_stencil_skel %d moving %d a1_samples %d (world generate writes EQUAL COVER only; gun is A=1. A gun-only covered count with opaque stairs in colour is a coverage bug.)\n",
                ncover, ncover_world, ncover_char, nskel, nstskel, nmove, npt);
        fprintf(tf, "silhouette_vs_colour  both %d  velocity_only %d  colour_only %d  iou %.4f  recall %.4f  precision %.4f\n",
                nboth, nvelonly, nskelonly,
                (nboth + nvelonly + nskelonly) > 0 ? nboth / float(nboth + nvelonly + nskelonly) : 0,
                nstskel ? nboth / float(nstskel) : 0,
                nskel ? nboth / float(nskel) : 0);
        fprintf(tf, "sil_legend green=body_or_equip_match red=velocity_geometry_only blue=colour_only (unsupported / alpha holes)\n");
        fprintf(tf, "colour_hud_stencil %s  (window stencil 4 written during the real colour hudgun draw: same pose, avatar projection, depth test, alphatest; not velocity A, not the ID pass)\n",
                havehudcol ? "yes" : "no");
        fprintf(tf, "hud_vs_colour  both %d  velocity_only %d  colour_only %d  iou %.4f  recall %.4f  precision %.4f\n",
                hud_both, hud_velonly, hud_colonly,
                (hud_both + hud_velonly + hud_colonly) > 0 ? hud_both / float(hud_both + hud_velonly + hud_colonly) : 0,
                (hud_both + hud_colonly) > 0 ? hud_both / float(hud_both + hud_colonly) : 0,
                (hud_both + hud_velonly) > 0 ? hud_both / float(hud_both + hud_velonly) : 0);
        fprintf(tf, "hud_mismatch_edge  velocity_only %d  colour_only %d  (adjacent to empty / image border)\n", hud_vel_edge, hud_col_edge);
        fprintf(tf, "hud_mismatch_interior  velocity_only %d  colour_only %d  (inside the union silhouette; should be ~0)\n", hud_vel_int, hud_col_int);
        fprintf(tf, "hudsil_legend dim colour + green=both red=vel_only_edge magenta=vel_only_interior blue=colour_only_edge yellow=colour_only_interior\n");
        if(velcolourmuzzleok)
            fprintf(tf, "colour_muzzle %.4f %.4f %.4f  (tag_muzzle after the colour hudgun draw, not the velocity replay)\n",
                    velcolourmuzzle.x, velcolourmuzzle.y, velcolourmuzzle.z);
        else
            fprintf(tf, "colour_muzzle none\n");
        if(haveunlit)
        {
            fprintf(tf, "unlit_mean_abs_rgb warp_via_vectors %.2f  samples %d  (stable albedo, no lighting)\n", nunlit ? eunlit / nunlit : 0, nunlit);
            fprintf(tf, "unlit_vs_colour  unlit_pixels %d  overlap_colour_stencil_skel %d  unlit_only %d\n", nunlitpix, nunlitst, nunlitpix - nunlitst);
            fprintf(tf, "unlit_depth colour-pass depth+stencil of EACH image (current vs stored previous), then characters with that image's pose/matrix. Walls occlude. Self-occlusion and overlap use depth. Not lighting.\n");
        }
        else
            fprintf(tf, "unlit_view not_captured\n");
        fprintf(tf, "id_buffer %s  (RGBA32F slot/mesh/tri/depth, raster of the velocity geometry against that image's colour depth; not velocity A)\n", haveid ? "yes" : "no");
        if(haveid && idcur)
        {
            int idkind[5] = { 0, 0, 0, 0, 0 }, idn = 0;
            for(int y = 0; y < h; y++) for(int x = 0; x < w; x++)
            {
                int s, m, t; float z;
                velidpixel(idcur, w, h, x, y, s, m, t, z);
                if(s < 0 || s >= velskeln || !velskels[s].d) continue;
                int k = clamp(velskels[s].attachkind, 0, 4);
                idkind[k]++;
                idn++;
            }
            fprintf(tf, "id_pixels total %d body %d armour %d weapon %d horns %d hudgun %d rigid_fan %d  (visible velocity geometry vs colour depth; rigid_fan is ID slot %d of the driven mapmodel)\n",
                    idn, idkind[0], idkind[1], idkind[2], idkind[3], idkind[4], velridpixels, VEL_RIGID_SLOT);
        }
        fprintf(tf, "track_cands %d cap %d kept %d (equipment prefers ID-visible centroids; body uses bone regions)\n",
                ncands, VELTR_CAND_MAX, veltrackn);
        const vector<extentity *> &ents = entities::getents();
        int idx = hwrtveldrive;
        if(ents.inrange(idx) && ents[idx] && ents[idx]->type == ET_MAPMODEL)
        {
            extentity &e = *ents[idx];
            model *m = loadmapmodel(e.attr2);
            float gyaw = float(e.attr1);
            float gpitch = 0;
            if(m)
            {
                gyaw += m->spinyaw * lastmillis / 1000.0f + m->offsetyaw;
                gpitch += m->offsetpitch + m->spinpitch * lastmillis / 1000.0f;
            }
            fprintf(tf, "driven_ent %d origin %.4f %.4f %.4f attr1 %d attr2 %d\n", idx, e.o.x, e.o.y, e.o.z, e.attr1, e.attr2);
            fprintf(tf, "mdlspin_per_sec %.4f %.4f lastmillis %d -> pose_yaw %.4f pose_pitch %.4f (same lastmillis for GL draw, RT instance, velocity pass)\n",
                    m ? m->spinyaw : 0, m ? m->spinpitch : 0, lastmillis, gyaw, gpitch);
        }
        if(velloghas)
        {
            fprintf(tf, "vel_world_current %.5f %.5f %.5f %.5f  %.5f %.5f %.5f %.5f  %.5f %.5f %.5f %.5f  %.5f %.5f %.5f %.5f\n",
                    vellogworld.a.x, vellogworld.a.y, vellogworld.a.z, vellogworld.a.w,
                    vellogworld.b.x, vellogworld.b.y, vellogworld.b.z, vellogworld.b.w,
                    vellogworld.c.x, vellogworld.c.y, vellogworld.c.z, vellogworld.c.w,
                    vellogworld.d.x, vellogworld.d.y, vellogworld.d.z, vellogworld.d.w);
            fprintf(tf, "vel_world_previous %.5f %.5f %.5f %.5f  %.5f %.5f %.5f %.5f  %.5f %.5f %.5f %.5f  %.5f %.5f %.5f %.5f\n",
                    vellogprevworld.a.x, vellogprevworld.a.y, vellogprevworld.a.z, vellogprevworld.a.w,
                    vellogprevworld.b.x, vellogprevworld.b.y, vellogprevworld.b.z, vellogprevworld.b.w,
                    vellogprevworld.c.x, vellogprevworld.c.y, vellogprevworld.c.z, vellogprevworld.c.w,
                    vellogprevworld.d.x, vellogprevworld.d.y, vellogprevworld.d.z, vellogprevworld.d.w);
        }
        extern int thirdperson, animoverride;
        fprintf(tf, "player %.4f %.4f %.4f yaw %.4f pitch %.4f holdplayer %d freezepose %d walk %d thirdperson %d animoverride %d drawn %d\n",
                player ? player->o.x : 0, player ? player->o.y : 0, player ? player->o.z : 0,
                player ? player->yaw : 0, player ? player->pitch : 0,
                int(hwrtvelholdplayer), int(hwrtvelfreezepose), int(hwrtvelwalk),
                thirdperson, animoverride, velskeldrawn);
        fprintf(tf, "sample_rule 5x5 covered, smooth flow, previous on-screen; A=1 samples are NOT independent of the velocity pass\n");
        fprintf(tf, "interior_mean_abs_rgb  warp_via_vectors %.2f  identity_same_pixel %.2f  (colour; lighting can dominate — use unlit and barycentric)\n",
                nerr ? ewarp / nerr : 0, nerr ? eident / nerr : 0);
        loopi(npt)
            fprintf(tf, "sample %d cur %d %d -> prev %d %d  vel %.2f %.2f px  |v| %.2f  onscreen %s\n",
                    i, pts[i].x, pts[i].y, pts[i].px, pts[i].py, pts[i].vx, pts[i].vy,
                    sqrtf(pts[i].vx * pts[i].vx + pts[i].vy * pts[i].vy),
                    (pts[i].px >= 0 && pts[i].py >= 0 && pts[i].px < w && pts[i].py < h) ? "yes" : "no");
        int npairh = 0, npairg = 0;
        loopi(ntr)
        {
            if(trs[i].cause != VELTR_VALID) continue;
            if(trs[i].region != 8 && trs[i].region != 9) continue;
            int *cnt = trs[i].region == 8 ? &npairh : &npairg;
            if(*cnt >= 4) continue;
            int cx = clamp(trs[i].cx, 0, w - 1), cy = clamp(trs[i].cy, 0, h - 1);
            int px = trs[i].px, py = trs[i].py;
            int cr = cur.data[(cy * w + cx) * 3], cg = cur.data[(cy * w + cx) * 3 + 1], cb = cur.data[(cy * w + cx) * 3 + 2];
            int pr = 0, pg = 0, pb = 0;
            bool prevon = px >= 0 && py >= 0 && px < w && py < h;
            if(prevon)
            {
                pr = prev.data[(py * w + px) * 3];
                pg = prev.data[(py * w + px) * 3 + 1];
                pb = prev.data[(py * w + px) * 3 + 2];
            }
            fprintf(tf, "colour_pair %s cur %d %d rgb %d %d %d  indep_prev %d %d rgb %d %d %d  on_colour_hud %s on_vel_a1 %s\n",
                    veltrname[trs[i].region], cx, cy, cr, cg, cb, px, py, pr, pg, pb,
                    chud[cy * w + cx] ? "yes" : "no", vhud[cy * w + cx] ? "yes" : "no");
            (*cnt)++;
        }
        fprintf(tf, "colour_pair_n hud_hands %d hud_gun %d  (true colour images, independent previous of the same triangle)\n", npairh, npairg);
        fprintf(tf, "barycentric_tracks triangle centroids. Raster (jittered) clip is used to find IDs and decide visibility. Expected motion compared to the RG buffer is unjittered_prev - unjittered_curr of the same 3D point (independent of the velocity shader). Raster displacement is kept for overlays. sample_off is |pixel_center - raster_centroid|.\n");
        fprintf(tf, "sample_compare velocity is read at the matching-ID pixel center. unjit_exp = prev_unjit_centroid - curr_unjit_centroid. raster_disp = prev_raster - curr_raster. err_px is |buffer - unjit_exp|, not |buffer - raster_disp|. Do not subtract a jitter length from an aggregated error.\n");
        fprintf(tf, "track_threshold pass if valid unjittered median<=1.00 px and p95<=2.50 px. Buffer is RGBA16F per pixel; pixel-center vs centroid is <=sqrt(0.5)~0.71 px; raster margin 2px on small/edge triangles. Still+jitter should sit near 0 on the unjittered error, not follow |delta_jitter|.\n");
        fprintf(tf, "unlit_scope characters drawn with each image's pose/matrix against THAT image's colour-pass depth+stencil (world occlusion included: walls hide the body). Not characters-only. Point visibility uses the same real-scene depth via the ID pass.\n");
        fprintf(tf, "track_total %d  valid %d  median %.3f  p95 %.3f  max %.3f  sample_off_median %.3f  raster_err_median %.3f  delta_jitter %.4f %.4f\n",
                ntr, nvalid, med, p95, mx, medoff, medraster, djx, djy);
        fprintf(tf, "track_by_region");
        loopj(VELTR_N)
        {
            int nv = 0, nt = 0;
            float rmax = -1;
            loopi(ntr) if(trs[i].region == j)
            {
                nt++;
                if(trs[i].cause == VELTR_VALID)
                {
                    nv++;
                    if(trs[i].err > rmax) rmax = trs[i].err;
                }
            }
            fprintf(tf, " %s n=%d valid=%d max=%.3f", veltrname[j], nt, nv, rmax);
        }
        fprintf(tf, "\n");
        fprintf(tf, "track_reject");
        loopi(VELTR_CAUSE_N) if(i != VELTR_VALID) fprintf(tf, " %s %d", veltrcause[i], ncause[i]);
        fprintf(tf, "\n");
        fprintf(tf, "occ_wall tracks_onscreen %d offscreen %d occluded_world %d punch_skel_a1_in_projbox %d box_pixels %d box_covered %d skel_a1_total %d (punch must be 0 when the body is behind a wall still in view)\n",
                n_on, n_off, n_world, punch_a1, boxpix, box_cover, nskel);
        fprintf(tf, "occ_overlap front_slot %d other_char %d valid_front %d valid_back %d zone_px %d id_front %d id_back %d vel_a1_front %d vel_a1_back %d (hidden tracks must be other_char; visible A=1 in the zone must be the front slot)\n",
                frontslot, n_otherchar, n_valid_front, n_valid_back, ov_n, ov_id_front, ov_id_back, ov_a1_front, ov_a1_back);
        int nshown = 0;
        loopj(4)
        {
            int want = j==0 ? VELTR_OCCLUDED_OTHER_CHAR : (j==1 ? VELTR_OCCLUDED_WORLD : VELTR_VALID);
            loopi(ntr)
            {
                if(trs[i].cause != want) continue;
                if(want == VELTR_VALID && j==2 && trs[i].region < 5) continue;
                if(want == VELTR_VALID && j==3 && trs[i].region >= 5) continue;
                if(nshown >= 40) break;
                fprintf(tf, "track %s slot %d kind %s mesh %d tri %d cause %s id_slot %d score %.3f area %.4f raster_cur %.1f %.1f sample_px %d %d raster_prev %.1f %.1f unjit_cur %.1f %.1f unjit_prev %.1f %.1f buf %.2f %.2f unjit_exp %.2f %.2f raster_disp %.2f %.2f |v| %.2f err_unjit %.3f err_raster %.3f sample_off %.3f warp_prev %d %d\n",
                        trs[i].region >= 0 && trs[i].region < VELTR_N ? veltrname[trs[i].region] : "?",
                        trs[i].slot,
                        trs[i].slot >= 0 && trs[i].slot < velskeln ? velkindname(velskels[trs[i].slot].attachkind) : "?", trs[i].mesh, trs[i].tri,
                        trs[i].cause >= 0 && trs[i].cause < VELTR_CAUSE_N ? veltrcause[trs[i].cause] : "?",
                        trs[i].idslot,
                        trs[i].score, trs[i].area,
                        trs[i].cxf, trs[i].cyf, trs[i].sx, trs[i].sy, trs[i].pxf, trs[i].pyf,
                        trs[i].ucx, trs[i].ucy, trs[i].upx, trs[i].upy,
                        trs[i].bufx, trs[i].bufy, trs[i].expx, trs[i].expy, trs[i].rexpx, trs[i].rexpy,
                        trs[i].mag, trs[i].err, trs[i].rastererr, trs[i].sampleoff, trs[i].vxpx, trs[i].vypx);
                nshown++;
            }
        }
        fprintf(tf, "markers: filled=raster previous of the centroid on the jittered colour image; cross=current sample + unjittered buffer + (prev_jitter-curr_jitter), which must land on that previous raster pixel. magenta=occluded_other_char, cyan=occluded_world. zoom.png: top=current crop, bottom=previous (green=raster prev, red cross=compensated vector).\n");
        fprintf(tf, "hud_skel_tracks do not validate a rigid mapmodel. The old AABB centre/face test is not used: those points are not mesh vertices and B=1/A=0 also matches scenery.\n");
        fprintf(tf, "rigid_mesh_tracks sample ID pixels of this instance (slot %d) on real mesh triangles, barycentric 3D on that triangle, then keep a spread of blade vs hub samples. Raster (jittered) clip is used for IDs, depth membership and visibility. Expected motion is unjittered_prev - unjittered_curr of the same material point. Valid only when slot %d is visible in both images. B=1/A=0 without that ID is cause scenery, never valid. High error does not reject. Hub and HUD shotgun do not validate blades.\n", VEL_RIGID_SLOT, VEL_RIGID_SLOT);
        {
            int idxd = hwrtveldrive;
            if(idxd >= 0 && velobjs.inrange(idxd) && velobjs[idxd].valid && velobjs[idxd].haveclip)
            {
                velobjhist &oh = velobjs[idxd];
                const vector<extentity *> &entsr = entities::getents();
                model *rm = NULL;
                if(entsr.inrange(idxd) && entsr[idxd] && entsr[idxd]->type == ET_MAPMODEL)
                    rm = loadmapmodel(entsr[idxd]->attr2);
                fprintf(tf, "rigid_object idx %d haveprev %s model %s tris %d id_pixels_cur %d id_pixels_prev_stride2 %d axisdist_min %.3f axisdist_max %.3f blade_thresh %.3f\n",
                        idxd, oh.haveprevclip ? "yes" : "no", rm && rm->name ? rm->name : "?",
                        velrigidtris.length(), velridpixels, velridprev, velr_axismin, velr_axismax,
                        velr_axismax > 0 ? 0.40f * velr_axismax : 0);
                loopi(velrnstored)
                {
                    velrstat &s = velrstored[i];
                    float emag = sqrtf(s.expx * s.expx + s.expy * s.expy);
                    fprintf(tf, "rigid_track %d kind %s mesh %d tri %d raster_cur %.1f %.1f sample_px %d %d raster_prev %.1f %.1f unjit_cur %.1f %.1f unjit_prev %.1f %.1f unjit_exp %.2f %.2f buf %.2f %.2f |exp| %.2f |v| %.2f err_unjit %.3f sample_off %.3f axisdist %.3f id_slot %d id_mesh %d id_tri %d cause %s covered_world %s a1 %s\n",
                            i, s.kind == 0 ? "blade" : "hub",
                            s.mesh, s.tri, s.rcx, s.rcy, s.sx, s.sy, s.rpx, s.rpy,
                            s.ucx, s.ucy, s.upx, s.upy, s.expx, s.expy, s.bufx, s.bufy,
                            emag, s.mag, s.err, s.sampleoff, s.axisdist,
                            s.idslot, s.idmesh, s.idtri,
                            s.cause >= 0 && s.cause < VELR_N ? velrcause[s.cause] : "?",
                            s.worldpix ? "yes" : "no", s.a1 ? "yes" : "no");
                }
                fprintf(tf, "rigid_reject");
                loopi(VELR_N) if(i != VELR_VALID) fprintf(tf, " %s %d", velrcause[i], velr_ncause[i]);
                fprintf(tf, "\n");
                fprintf(tf, "rigid_blade_total %d valid %d median %.3f p95 %.3f max %.3f |exp|_median %.3f |exp|_p95 %.3f |exp|_max %.3f (mesh triangles far from spin axis, not AABB, not hub, not HUD shotgun)\n",
                        velr_nblade, velr_nbladevalid, velr_med, velr_p95, velr_max, velr_magmed, velr_magp95, velr_magmax);
                fprintf(tf, "rigid_hub_valid %d (reported, does not validate blades)\n", velr_nhubvalid);
                fprintf(tf, "rigid_track_total %d valid %d median %.3f p95 %.3f max %.3f (blade stats; hub excluded from median/p95/max)\n",
                        velr_nblade, velr_nbladevalid, velr_med, velr_p95, velr_max);
            }
            else fprintf(tf, "rigid_object none (hwrtveldrive %d)\n", int(hwrtveldrive));
        }
        fclose(tf);
    }
    delete[] velpix;
    delete[] stpix;
    delete[] idcur;
    delete[] idprev;
    delete[] chud;
    delete[] vhud;
    copystring(homedir, savedhome);
    conoutf("hwrt velocity proof %s  lastmillis %d  histusable %s  hud iou %.3f edge %d/%d interior %d/%d  track_med %.2f px nvalid %d",
            clean, lastmillis, velhistusable() ? "yes" : "no",
            (hud_both + hud_velonly + hud_colonly) > 0 ? hud_both / float(hud_both + hud_velonly + hud_colonly) : 0,
            hud_vel_edge, hud_col_edge, hud_vel_int, hud_col_int,
            med, nvalid);
}

void hwrtdrawveldebug()
{
    bool proofqueued = velshotcount > 0 && velshotkindproof(velshotq[velshothead].kind);
    if((!hwrtveldebug && !proofqueued) || !velwanted() || !veltex) return;
    if(!velensure()) return;

    velinittimers();
    velharvest(velqdbg, velqdbgpend, velqdbgslot, &hwrtveldebugcost, velqdbgep, velqdbgeq, NULL, NULL, NULL);

    bool needhist = hwrtveldebug >= 2;
    int mode = hwrtveldebug;
    if(needhist && !velhistusable()) mode = 1;
    veldbgmode = mode;
    if(velcutframe || (velpostcut >= 0 && velpostcut <= 2))
        conoutf("hwrt velocity: overlay frame cut=%s post=%d requested %d used %d histusable %s histhas %s histfresh %s",
                velcutframe ? "yes" : "no", velpostcut, int(hwrtveldebug), mode,
                velhistusable() ? "yes" : "no", velhisthas ? "yes" : "no", velhistfresh ? "yes" : "no");

    velbeginq(velqdbg, velqdbgpend, &velqdbgslot, &velqdbgact, velqdbgep, velqdbgeq);

    if(needhist) velcopytex(velhist[velhistslot]);
    GLuint hist = velhist[velhistslot ^ 1];
    if(proofqueued)
    {
        bool aliveproof = velshotq[velshothead].kind == VELSHOT_PROOFALIVE;
        if(aliveproof && !velaliveproofready() && velalivewait < 90)
            ;
        else
        {
            if(!velhistusable() && hwrtveldebug >= 2)
                conoutf(CON_WARN, "hwrt velocity: proof %s histusable=no (stay on debug 2 for several frames first)", velshotq[velshothead].name);
            velwriteproof(velshotq[velshothead].name, needhist ? hist : 0);
        }
    }

    if(!hwrtveldebug)
    {
        velendq(velqdbgpend, &velqdbgslot, &velqdbgact);
        return;
    }

    Shader *s = useshaderbyname("hwrtveldebug");
    if(!s || s->invalid())
    {
        velendq(velqdbgpend, &velqdbgslot, &velqdbgact);
        return;
    }
    s->set();
    glActiveTexture_(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, veltex);
    glActiveTexture_(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, needhist ? velhist[velhistslot] : veltex);
    glActiveTexture_(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, hist);
    glActiveTexture_(GL_TEXTURE0);
    LOCALPARAMF(velparams, 1.0f/velw, 1.0f/velh, float(mode), 1.0f/hwrtvelscale);
    float jcx = 0, jcy = 0, jpx = 0, jpy = 0;
    hwrttemporaljitterpixels(jcx, jcy, jpx, jpy);
    LOCALPARAMF(veljitter, jcx, jcy, jpx, jpy);
    screenquad(1, 1);

    velendq(velqdbgpend, &velqdbgslot, &velqdbgact);

    if(needhist)
    {
        velhistslot ^= 1;
        velhisthas = velhistfresh;
        velhistfresh = true;
    }
    else
    {
        velhisthas = false;
        velhistfresh = false;
    }
}

static const char *veldebugname(int m)
{
    switch(m)
    {
        case 1: return "vecteurs (gris = immobile, couleur = mouvement, magenta = non pris en charge)";
        case 2: return "reprojection de l'image precedente (doit coller au decor; magenta = ignore)";
        case 3: return "erreur de reprojection (vert = aligne, rouge = faux, magenta = ignore)";
        default: return "off";
    }
}

static void hwrtvelcycle()
{
    int prev = hwrtveldebug;
    int next = (hwrtveldebug + 1) % 4;
    if(!next)
    {
        setvar("hwrtveldebug", 0);
        setvar("hwrtvelocity", 0);
    }
    else
    {
        setvar("hwrtvelocity", 1);
        setvar("hwrtveldebug", next);
    }
    conoutf("hwrt velocity: hwrtvelcycle %d -> %d  %s  histusable %s",
            prev, int(hwrtveldebug), veldebugname(hwrtveldebug),
            velhistusable() ? "yes" : "no");
}
COMMAND(hwrtvelcycle, "");

static void hwrtvelstats()
{
    conoutf("hwrt velocity: %s debug %d  %dx%d  colourhist %s usable %s cut %s  copy %.2f ms  gen %.2f ms  obj %.2f ms  skel %.2f ms cpu %.2f ms  overlay %.2f ms  play %.2f ms  bones %d B  chars %d equip %d hud %d",
            veltex ? "on" : "off", int(hwrtveldebug), velw, velh,
            velhisthas ? "yes" : "no", velhistusable() ? "yes" : "no", velcutframe ? "yes" : "no",
            hwrtvelcopycost, hwrtvelgencost, hwrtvelobjcost, hwrtvelskelcost, hwrtvelskelcpu, hwrtveldebugcost,
            hwrtvelcopycost + hwrtvelgencost + hwrtvelobjcost + hwrtvelskelcost,
            hwrtvelskelmem, velskeldrawn, velequipdrawn, velhuddrawn);
}
COMMAND(hwrtvelstats, "");

static void velstatminmax(const float *v, int n, float &mn, float &avg, float &mx)
{
    mn = avg = mx = 0;
    if(n <= 0) return;
    mn = mx = v[0];
    double s = 0;
    loopi(n)
    {
        if(v[i] < mn) mn = v[i];
        if(v[i] > mx) mx = v[i];
        s += v[i];
    }
    avg = float(s / n);
}

static void velstatmoments(const float *v, int n, float &mn, float &avg, float &mx, float &sd)
{
    velstatminmax(v, n, mn, avg, mx);
    sd = 0;
    if(n < 2) return;
    double s = 0;
    loopi(n) { double d = v[i] - avg; s += d*d; }
    sd = float(sqrt(s / (n - 1)));
}

static void velintminmax(const int *v, int n, int &mn, float &avg, int &mx)
{
    mn = mx = 0;
    avg = 0;
    if(n <= 0) return;
    mn = mx = v[0];
    double s = 0;
    loopi(n)
    {
        if(v[i] < mn) mn = v[i];
        if(v[i] > mx) mx = v[i];
        s += v[i];
    }
    avg = float(s / n);
}

static void velbenchwrite();

static void velbenchstep()
{
    if(velbenchphase == VELBENCH_IDLE) return;
    if(velbenchphase == VELBENCH_WAITQ)
    {
        if(velshotcount > 0) { velbenchwait++; return; }
        if(hwrtveldebug) hwrtveldebug = 0;
        velbenchphase = VELBENCH_WARM;
        velbenchleft = velbenchwarm;
        velbenchequip = int(hwrtvelequip);
        conoutf("hwrt velocity: bench warmup %d frames (equip %d debug 0, no capture queue)", velbenchwarm, velbenchequip);
        return;
    }
    if(hwrtveldebug) hwrtveldebug = 0;
    if(velshotcount > 0) { velbenchwait++; return; }
    if(int(hwrtvelequip) != velbenchequip)
    {
        conoutf(CON_WARN, "hwrt velocity: bench aborted, hwrtvelequip changed %d -> %d", velbenchequip, int(hwrtvelequip));
        velbenchphase = VELBENCH_IDLE;
        hwrtvelbenchdone = 1;
        return;
    }
    if(velbenchphase == VELBENCH_WARM)
    {
        velbenchleft--;
        if(velbenchleft <= 0)
        {
            velqepoch++;
            velbenchn = 0;
            velbenchdiscard = 0;
            velbenchphase = VELBENCH_DROP;
            velbenchleft = VEL_QUERY_RING + 2;
            conoutf("hwrt velocity: bench drop %d GPU queries from warmup (epoch %d)", velbenchleft, velqepoch);
        }
        return;
    }
    if(velbenchphase == VELBENCH_DROP)
    {
        velbenchleft--;
        if(velbenchleft <= 0)
        {
            velbenchphase = VELBENCH_RUN;
            velbenchn = 0;
            velbenchrunframes = 0;
            conoutf("hwrt velocity: bench measure %d frames", velbenchnwant);
        }
        return;
    }
    if(velbenchphase != VELBENCH_RUN) return;
    velbenchrunframes++;
    if(!velqok)
    {
        if(velbenchn < VEL_STAT_N)
        {
            int i = velbenchn;
            velbench_copy[i] = velbench_gen[i] = velbench_obj[i] = velbench_skel[i] = 0;
            velbench_cpu[i] = hwrtvelskelcpu;
            velbench_play[i] = 0;
            velbench_chars[i] = velskeldrawn;
            velbench_equipn[i] = velequipdrawn;
            velbenchn++;
        }
        if(velbenchn >= velbenchnwant) velbenchwrite();
        return;
    }
    bool ready = velhqcopy > 0 && velhqgen > 0 && velhqobj > 0 && velhqskel > 0;
    bool mismatch = velhqcopy < 0 || velhqgen < 0 || velhqobj < 0 || velhqskel < 0
        || velhqcopyeq != velbenchequip || velhqskeleq != velbenchequip
        || velhqcopyep != velqepoch || velhqskelep != velqepoch;
    if(!ready)
    {
        if(mismatch) velbenchdiscard++;
        return;
    }
    if(mismatch)
    {
        velbenchdiscard++;
        return;
    }
    if(velbenchn < VEL_STAT_N)
    {
        int i = velbenchn;
        velbench_copy[i] = hwrtvelcopycost;
        velbench_gen[i] = hwrtvelgencost;
        velbench_obj[i] = hwrtvelobjcost;
        velbench_skel[i] = hwrtvelskelcost;
        velbench_cpu[i] = hwrtvelskelcpu;
        velbench_play[i] = hwrtvelcopycost + hwrtvelgencost + hwrtvelobjcost + hwrtvelskelcost;
        velbench_chars[i] = velskeldrawn;
        velbench_equipn[i] = velequipdrawn;
        velbenchn++;
    }
    if(velbenchn >= velbenchnwant || velbenchrunframes > velbenchnwant + 60) velbenchwrite();
}

static void velbenchwrite()
{
    int n = velbenchn;
    float cmn, cavg, cmx, csd, gmn, gavg, gmx, gsd, omn, oavg, omx, osd, smn, savg, smx, ssd, kmn, kavg, kmx, ksd, pmn, pavg, pmx, psd;
    velstatmoments(velbench_copy, n, cmn, cavg, cmx, csd);
    velstatmoments(velbench_gen, n, gmn, gavg, gmx, gsd);
    velstatmoments(velbench_obj, n, omn, oavg, omx, osd);
    velstatmoments(velbench_skel, n, smn, savg, smx, ssd);
    velstatmoments(velbench_cpu, n, kmn, kavg, kmx, ksd);
    velstatmoments(velbench_play, n, pmn, pavg, pmx, psd);
    int cmin, cmax, emin, emax;
    float cmean, emean;
    velintminmax(velbench_chars, n, cmin, cmean, cmax);
    velintminmax(velbench_equipn, n, emin, emean, emax);

    conoutf("hwrt velocity bench %d tagged frames  hwrtvelequip %d  debug 0  %dx%d", n, velbenchequip, velw, velh);
    conoutf("  GPU queries belong to this condition (epoch %d, discarded %d)", velqepoch, velbenchdiscard);
    conoutf("  drawn bodies min %d avg %.2f max %d  equip min %d avg %.2f max %d", cmin, cmean, cmax, emin, emean, emax);
    conoutf("  copy GPU  min %.3f avg %.3f max %.3f sd %.3f ms", cmn, cavg, cmx, csd);
    conoutf("  gen  GPU  min %.3f avg %.3f max %.3f sd %.3f ms", gmn, gavg, gmx, gsd);
    conoutf("  obj  GPU  min %.3f avg %.3f max %.3f sd %.3f ms", omn, oavg, omx, osd);
    conoutf("  skel GPU  min %.3f avg %.3f max %.3f sd %.3f ms", smn, savg, smx, ssd);
    conoutf("  skel CPU  min %.3f avg %.3f max %.3f sd %.3f ms  mem %d B", kmn, kavg, kmx, ksd, hwrtvelskelmem);
    conoutf("  play copy+gen+obj+skel GPU  min %.3f avg %.3f max %.3f sd %.3f ms", pmn, pavg, pmx, psd);

    string savedhome;
    copystring(savedhome, homedir);
    homedir[0] = '\0';
    string d1, d2;
    copystring(d1, "shots"); path(d1); createdir(d1);
    copystring(d2, "shots/velocity-lot6"); path(d2); createdir(d2);
    string fnnamed;
    if(velbenchname[0]) formatstring(fnnamed, "shots/velocity-lot6/%s.txt", velbenchname);
    else formatstring(fnnamed, "shots/velocity-lot6/bench-equip%d.txt", velbenchequip);
    path(fnnamed);
    FILE *bf = fopen("shots/velocity-lot6/bench-last.txt", "w");
    FILE *bf2 = fopen(fnnamed, "w");
    FILE *outs[2] = { bf, bf2 };
    loopk(2)
    {
        FILE *out = outs[k];
        if(!out) continue;
        fprintf(out, "hwrt velocity bench %d tagged frames (GPU TIME_ELAPSED, no CPU readback)\n", n);
        fprintf(out, "protocol apply-setting, warmup %d real frames, drop %d in-flight GPU queries, measure %d, no capture queue, no ID/unlit, no extra depth copies\n",
                velbenchwarm, VEL_QUERY_RING + 2, velbenchnwant);
        fprintf(out, "hwrtvelequip %d debug %d %dx%d epoch %d discarded_mismatch %d\n",
                velbenchequip, int(hwrtveldebug), velw, velh, velqepoch, velbenchdiscard);
        fprintf(out, "drawn_bodies min %d avg %.2f max %d\n", cmin, cmean, cmax);
        fprintf(out, "drawn_equip min %d avg %.2f max %d\n", emin, emean, emax);
        if(velbenchequip == 0 && emax > 0)
            fprintf(out, "WARNING expected drawn_equip 0 with hwrtvelequip 0, got max %d\n", emax);
        if(velbenchequip == 1 && emax < 1)
            fprintf(out, "WARNING expected drawn_equip > 0 with hwrtvelequip 1, got max %d\n", emax);
        fprintf(out, "copy GPU min %.3f avg %.3f max %.3f sd %.3f ms\n", cmn, cavg, cmx, csd);
        fprintf(out, "gen  GPU min %.3f avg %.3f max %.3f sd %.3f ms\n", gmn, gavg, gmx, gsd);
        fprintf(out, "obj  GPU min %.3f avg %.3f max %.3f sd %.3f ms\n", omn, oavg, omx, osd);
        fprintf(out, "skel GPU min %.3f avg %.3f max %.3f sd %.3f ms\n", smn, savg, smx, ssd);
        fprintf(out, "skel CPU min %.3f avg %.3f max %.3f sd %.3f ms mem %d B\n", kmn, kavg, kmx, ksd, hwrtvelskelmem);
        fprintf(out, "play copy+gen+obj+skel GPU min %.3f avg %.3f max %.3f sd %.3f ms\n", pmn, pavg, pmx, psd);
        fprintf(out, "overlay not measured (debug 0, diagnostic path off)\n");
        fclose(out);
    }
    conoutf("hwrt velocity: wrote shots/velocity-lot6/bench-last.txt and %s", fnnamed);
    copystring(homedir, savedhome);

    velbenchphase = VELBENCH_IDLE;
    hwrtvelbenchdone = 1;
    velbenchpending = velbenchthen;
    velbenchthen = NULL;
}

static void hwrtvelbench()
{
    if(velbenchn > 0)
        conoutf("hwrt velocity: last tagged bench has %d samples (hwrtvelbenchrun). rolling window below is NOT condition-safe.", velbenchn);
    else
        conoutf(CON_WARN, "hwrt velocity: hwrtvelbench dumps the rolling window; it does not wait. Use hwrtvelbenchrun.");
    float cmn, cavg, cmx, gmn, gavg, gmx, omn, oavg, omx, smn, savg, smx, dmn, davg, dmx, kcpu, kavg, kmax;
    velstatminmax(velstat_copy, velstatn, cmn, cavg, cmx);
    velstatminmax(velstat_gen, velstatn, gmn, gavg, gmx);
    velstatminmax(velstat_obj, velstatn, omn, oavg, omx);
    velstatminmax(velstat_skel, velstatn, smn, savg, smx);
    velstatminmax(velstat_dbg, velstatn, dmn, davg, dmx);
    velstatminmax(velstat_skelcpu, velstatn, kcpu, kavg, kmax);
    conoutf("hwrt velocity rolling %d frames (may mix conditions):", velstatn);
    conoutf("  copy min %.3f avg %.3f max %.3f ms", cmn, cavg, cmx);
    conoutf("  gen  min %.3f avg %.3f max %.3f ms", gmn, gavg, gmx);
    conoutf("  obj  min %.3f avg %.3f max %.3f ms", omn, oavg, omx);
    conoutf("  skel GPU min %.3f avg %.3f max %.3f ms", smn, savg, smx);
    conoutf("  skel CPU min %.3f avg %.3f max %.3f ms mem %d B chars %d equip %d hud %d", kcpu, kavg, kmax, hwrtvelskelmem, velskeldrawn, velequipdrawn, velhuddrawn);
}
COMMAND(hwrtvelbench, "");

static void hwrtvelbenchrun(int *warm, int *n, char *name, char *then)
{
    if(velbenchphase != VELBENCH_IDLE)
    {
        conoutf(CON_WARN, "hwrt velocity: bench already running");
        return;
    }
    velbenchwarm = clamp(*warm, 8, 120);
    velbenchnwant = clamp(*n, 8, int(VEL_STAT_N));
    copystring(velbenchname, name && name[0] ? name : "");
    DELETEA(velbenchthen);
    if(then && then[0]) velbenchthen = newstring(then);
    hwrtvelocity = 1;
    hwrtveldebug = 0;
    hwrtvelbenchdone = 0;
    velbenchn = 0;
    velbenchdiscard = 0;
    velbenchwait = 0;
    velbenchequip = int(hwrtvelequip);
    velbenchphase = VELBENCH_WAITQ;
    conoutf("hwrt velocity: benchrun warmup %d measure %d file %s equip %d (waits for capture queue to drain, does not enqueue captures)",
            velbenchwarm, velbenchnwant, velbenchname[0] ? velbenchname : "(auto)", velbenchequip);
}
COMMAND(hwrtvelbenchrun, "iiss");

static void velholdset(const vec &o, float yaw, float pitch)
{
    velholdpos = o;
    velholdcampos = o;
    velholdplayerpos = o;
    velholdyaw = yaw;
    velholdpitch = pitch;
    velholdplayeryaw = yaw;
    velholdplayerpitch = pitch;
    hwrtvelholdcam = 1;
    hwrtvelholdplayer = 1;
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

static void hwrtvelnudge(float *dx, float *dy, float *dz)
{
    vec d(*dx, *dy, *dz);
    if(player)
    {
        player->o.add(d);
        player->resetinterp();
        velholdplayerpos = player->o;
    }
    if(camera1)
    {
        camera1->o.add(d);
        velholdcampos = camera1->o;
        velholdpos = camera1->o;
    }
    hwrtvelholdcam = 1;
}
COMMAND(hwrtvelnudge, "fff");

static void hwrtvelsetpos(float *x, float *y, float *z, float *yaw, float *pitch)
{
    velholdset(vec(*x, *y, *z), *yaw, *pitch);
    velspinfleft = velslidefleft = velpushfleft = 0;
    hwrtvelspin = hwrtvelpitchspin = hwrtvelslide = hwrtvelpush = 0;
    conoutf("hwrt velocity: setpos %.1f %.1f %.1f yaw %.1f pitch %.1f", *x, *y, *z, *yaw, *pitch);
}
COMMAND(hwrtvelsetpos, "fffff");

static void hwrtvelspinn(float *dps, int *n)
{
    hwrtvelspin = *dps;
    velspinfleft = max(*n, 0);
    if(velspinfleft <= 0) hwrtvelspin = 0;
    conoutf("hwrt velocity: spin %.4f for %d frames", hwrtvelspin, velspinfleft);
}
COMMAND(hwrtvelspinn, "fi");

static void hwrtvelsliden(float *step, int *n)
{
    hwrtvelslide = *step;
    velslidefleft = max(*n, 0);
    if(velslidefleft <= 0) hwrtvelslide = 0;
    conoutf("hwrt velocity: slide %.4f for %d frames", hwrtvelslide, velslidefleft);
}
COMMAND(hwrtvelsliden, "fi");

static void hwrtvelpushn(float *step, int *n)
{
    hwrtvelpush = *step;
    velpushfleft = max(*n, 0);
    if(velpushfleft <= 0) hwrtvelpush = 0;
    conoutf("hwrt velocity: push %.4f for %d frames", hwrtvelpush, velpushfleft);
}
COMMAND(hwrtvelpushn, "fi");

static void hwrtveldrivepushn(float *step, int *n)
{
    veldpstep = *step;
    veldpleft = max(*n, 0);
    if(veldpleft <= 0) veldpstep = 0;
    conoutf("hwrt velocity: drivepush %.4f for %d frames (idx %d)", veldpstep, veldpleft, int(hwrtveldrive));
}
COMMAND(hwrtveldrivepushn, "fi");

static void hwrtveldrivesliden(float *step, int *n)
{
    veldsstep = *step;
    veldsleft = max(*n, 0);
    if(veldsleft <= 0) veldsstep = 0;
    conoutf("hwrt velocity: driveslide %.4f for %d frames (idx %d)", veldsstep, veldsleft, int(hwrtveldrive));
}
COMMAND(hwrtveldrivesliden, "fi");

static void hwrtframewait(int *n, char *cmd)
{
    DELETEA(velframewaitcmd);
    velframewaitleft = max(*n, 0);
    if(velframewaitleft > 0 && cmd && cmd[0]) velframewaitcmd = newstring(cmd);
    conoutf("hwrt velocity: wait %d frames", velframewaitleft);
}
COMMAND(hwrtframewait, "is");

static void hwrtvellistitems()
{
    const vector<extentity *> &ents = entities::getents();
    int shown = 0, spawned = 0;
    loopv(ents)
    {
        extentity &e = *ents[i];
        const char *name = entities::entmodel(e);
        if(!name || !name[0]) continue;
        if(e.spawned()) spawned++;
        if(shown >= 24) continue;
        shown++;
        conoutf("item %d type %s mdl %s spawned %d at %.1f %.1f %.1f",
            i, entities::entname(e.type), name, e.spawned() ? 1 : 0, e.o.x, e.o.y, e.o.z);
    }
    conoutf("hwrt velocity: %d spawned entmodel ents (listed %d)", spawned, shown);
}
COMMAND(hwrtvellistitems, "");

static void hwrtvelaimitem()
{
    if(!camera1)
    {
        conoutf("hwrt velocity: aimitem skipped (no camera)");
        return;
    }
    const vector<extentity *> &ents = entities::getents();
    int best = -1, ammo = -1;
    float bestd = 1e9f, ammod = 1e9f;
    loopv(ents)
    {
        extentity &e = *ents[i];
        const char *name = entities::entmodel(e);
        if(!name || !name[0] || !e.spawned()) continue;
        const char *tn = entities::entname(e.type);
        if(tn && !strcmp(tn, "teleport")) continue;
        float d = camera1->o.squaredist(e.o);
        if(d < bestd) { bestd = d; best = i; }
        if(!strncmp(name, "ammo/", 5) && d < ammod) { ammod = d; ammo = i; }
    }
    int idx = ammo >= 0 ? ammo : best;
    if(idx < 0)
    {
        conoutf("hwrt velocity: aimitem found no spawned pickup");
        return;
    }
    extentity &e = *ents[idx];
    vec target = e.o;
    target.z += 1.2f;
    vec cam = vec(target).add(vec(22, -16, 8));
    vec dir = vec(target).sub(cam);
    float yaw, pitch;
    vectoyawpitch(dir, yaw, pitch);
    velholdset(cam, yaw, pitch);
    conoutf("hwrt velocity: aiming %s idx %d at %.1f %.1f %.1f from %.1f %.1f %.1f yaw %.1f pitch %.1f",
        entities::entmodel(e), idx, e.o.x, e.o.y, e.o.z, cam.x, cam.y, cam.z, yaw, pitch);
}
COMMAND(hwrtvelaimitem, "");

static void hwrtvelaimfloor()
{
    if(!camera1)
    {
        conoutf("hwrt velocity: aimfloor skipped (no camera)");
        return;
    }
    vec cam = camera1->o;
    velholdset(cam, camera1->yaw, -58.0f);
    conoutf("hwrt velocity: aimfloor at %.1f %.1f %.1f yaw %.1f pitch -58", cam.x, cam.y, cam.z, camera1->yaw);
}
COMMAND(hwrtvelaimfloor, "");

static bool velsnowtex(int tex, const char *&name)
{
    VSlot &vs = lookupvslot(tex, false);
    if(!vs.slot) return false;
    loopv(vs.slot->sts)
    {
        const char *n = vs.slot->sts[i].name;
        if(n && n[0] && strstr(n, "snow"))
        {
            name = n;
            return true;
        }
    }
    return false;
}

static void hwrtvelaimsnow()
{
    if(worldsize < 8)
    {
        conoutf("hwrt velocity: aimsnow skipped (no world)");
        return;
    }
    vec best(0, 0, 0);
    const char *bestname = NULL;
    float bestscore = 1e9f;
    int nfound = 0;
    const int step = 16;
    const int z0 = 256, z1 = min(worldsize, 900);
    const int m = 96;
    for(int z = z0; z < z1; z += step)
    for(int y = m; y < worldsize - m; y += step)
    for(int x = m; x < worldsize - m; x += step)
    {
        ivec ro;
        int rsize = 0;
        cube &c = lookupcube(ivec(x, y, z), 0, ro, rsize);
        if(isempty(c)) continue;
        const char *name = NULL;
        if(!velsnowtex(c.texture[O_TOP], name)) continue;
        int upz = min(worldsize - 1, ro.z + max(rsize, step) + 1);
        if(!isempty(lookupcube(ivec(ro.x + rsize/2, ro.y + rsize/2, upz)))) continue;
        if(rsize > 64) continue;
        nfound++;
        vec p(ro.x + rsize*0.5f, ro.y + rsize*0.5f, float(ro.z + rsize));
        bool dirty = name && strstr(name, "dirty_snow");
        float score = (dirty ? 0.0f : 8000.0f) + fabsf(p.z - 540.0f);
        if(nfound <= 12) conoutf("snow floor %s size %d at %.0f %.0f %.0f", name ? name : "?", rsize, p.x, p.y, p.z);
        if(!bestname || score < bestscore)
        {
            best = p;
            bestname = name;
            bestscore = score;
        }
    }
    if(!bestname)
    {
        conoutf("hwrt velocity: aimsnow found no snow floor");
        return;
    }
    vec cam = best;
    cam.z += 18;
    velholdset(cam, 210.0f, -72.0f);
    conoutf("hwrt velocity: aimsnow floor %s at %.1f %.1f %.1f from %.1f %.1f %.1f yaw 210 pitch -72 (%d floors)",
        bestname, best.x, best.y, best.z, cam.x, cam.y, cam.z, nfound);
}
COMMAND(hwrtvelaimsnow, "");

static void velprobelog(const char *tag)
{
    if(!veltex || !velfb || velw < 8 || velh < 8)
    {
        conoutf("hwrt velocity: %s probe skipped (no image)", tag ? tag : "probe");
        return;
    }
    const int tw = min(96, velw);
    const int th = min(96, velh);
    const int x0 = (velw - tw) / 2;
    const int y0 = (velh - th) / 2;
    static float pix[96*96*4];
    GLint prevfb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevfb);
    glBindFramebuffer_(GL_FRAMEBUFFER, velfb);
    glReadPixels(x0, y0, tw, th, GL_RGBA, GL_FLOAT, pix);
    glBindFramebuffer_(GL_FRAMEBUFFER, prevfb);
    int nvalid = 0;
    int ninv = 0;
    double sum = 0, sumx = 0, sumy = 0;
    double mx = 0;
    loopi(tw * th)
    {
        float vx = pix[i*4];
        float vy = pix[i*4+1];
        float v = pix[i*4+2];
        if(v < 0.5f) { ninv++; continue; }
        float m = sqrtf(vx*vx + vy*vy);
        nvalid++;
        sum += m;
        sumx += vx;
        sumy += vy;
        if(m > mx) mx = m;
    }
    conoutf("hwrt velocity %s centre %dx%d: covered %d  unsupported %d  mean vx %.2f vy %.2f  |v| %.3f px  max %.3f px  ready %s  histusable %s  cut %s  overlaymode %d",
            tag ? tag : "probe", tw, th, nvalid, ninv,
            nvalid ? sumx/nvalid : 0.0, nvalid ? sumy/nvalid : 0.0,
            nvalid ? sum/nvalid : 0.0, mx,
            velready ? "yes" : "no",
            velhistusable() ? "yes" : "no",
            velcutframe ? "yes" : "no",
            veldbgmode ? veldbgmode : int(hwrtveldebug));
}

static void hwrtvelprobe()
{
    velprobelog("probe");
}
COMMAND(hwrtvelprobe, "");

static void hwrtvelcam()
{
    if(!camera1)
    {
        conoutf("hwrt velocity: no camera");
        return;
    }
    conoutf("hwrt velocity: cam %.3f %.3f %.3f yaw %.3f pitch %.3f  player %.1f %.1f %.1f  skel %d slots %d mem %d  ready %s histhas %s histfresh %s histusable %s cut %s debug %d",
            camera1->o.x, camera1->o.y, camera1->o.z, camera1->yaw, camera1->pitch,
            player ? player->o.x : 0, player ? player->o.y : 0, player ? player->o.z : 0,
            velskeldrawn, velskeln, hwrtvelskelmem,
            velready ? "yes" : "no", velhisthas ? "yes" : "no", velhistfresh ? "yes" : "no",
            velhistusable() ? "yes" : "no", velcutframe ? "yes" : "no", int(hwrtveldebug));
}
COMMAND(hwrtvelcam, "");

static void hwrtvelstartwalk()
{
    extern int thirdperson;
    if(editmode) toggleedit(false);
    if(player && player->state == CS_DEAD) execute("respawn");
    if(editmode) toggleedit(false);
    thirdperson = 1;
    hwrtvelwalk = 1;
    hwrtvelholdplayer = 0;
    hwrtvelfreezepose = 0;
    setvar("animoverride", 0);
    if(player)
    {
        if(player->state != CS_ALIVE) player->state = CS_ALIVE;
        player->move = 1;
        velwalkfrom = player->o;
        velwalkset = true;
        conoutf("hwrt velocity: walk start state=%d (0=alive,1=dead,4=edit) move=%d pos %.1f %.1f %.1f",
                int(player->state), int(player->move), player->o.x, player->o.y, player->o.z);
        if(player->state == CS_ALIVE && player->move > 0)
            conoutf("hwrt velocity: expecting ANIM_FORWARD (alive, move>0)");
        else
            conoutf("hwrt velocity: walk animation may not play");
    }
}
COMMAND(hwrtvelstartwalk, "");

static void hwrtvelwalkcheck()
{
    if(!player)
    {
        conoutf("hwrt velocity: no player");
        return;
    }
    float dist = velwalkset ? player->o.dist(velwalkfrom) : 0;
    conoutf("hwrt velocity: walk check state=%d move=%d travelled %.2f  pos %.1f %.1f %.1f",
            int(player->state), int(player->move), dist, player->o.x, player->o.y, player->o.z);
}
COMMAND(hwrtvelwalkcheck, "");

static void hwrtveldelbots()
{
    velplacetwo = 0;
    veloverlapmode = 0;
    loopi(8) execute("delbot");
    conoutf("hwrt velocity: requested delbot x8");
}
COMMAND(hwrtveldelbots, "");

static void hwrtvelplacechar()
{
    extern int thirdperson;
    execute("paused 0");
    execute("spectator 0");
    veloverlapmode = 0;
    velplacetwo = 0;
    if(editmode) toggleedit(false);
    if(player && player->state == CS_DEAD) execute("respawn");
    execute("delbot");
    setvar("playermodel", 0);
    setvar("forceplayermodels", 1);
    setvar("hudgun", 0);
    thirdperson = 1;
    hwrtvelwalk = 0;
    hwrtvelholdplayer = 1;
    hwrtvelholdcam = 1;
    setvar("animoverride", 0);
    hwrtvelfreezepose = 1;
    vec po(668, 591, 566);
    vec cam(626, 551, 578);
    if(player)
    {
        player->o = po;
        player->yaw = 180;
        player->pitch = 0;
        player->vel = vec(0, 0, 0);
        player->falling = vec(0, 0, 0);
        player->move = 0;
        player->strafe = 0;
        player->resetinterp();
        velholdplayerpos = po;
        velholdplayeryaw = 180;
        velholdplayerpitch = 0;
    }
    if(camera1 && camera1 != (physent *)player)
    {
        camera1->o = cam;
        vectoyawpitch(vec(po).sub(cam), camera1->yaw, camera1->pitch);
        velholdcampos = cam;
        velholdyaw = camera1->yaw;
        velholdpitch = camera1->pitch;
        velholdpos = cam;
    }
    else if(camera1)
    {
        vectoyawpitch(vec(po).sub(cam), camera1->yaw, camera1->pitch);
        velholdcampos = cam;
        velholdyaw = camera1->yaw;
        velholdpitch = camera1->pitch;
        velholdpos = cam;
    }
    const char *mdl = player ? hwrtdynentmdlname(player) : "mrfixit";
    execute("hwrtvelgiveequip 0 1 0");
    conoutf("hwrt velocity: character %s at %.1f %.1f %.1f  cam %.1f %.1f %.1f yaw %.1f (thirdperson, pose frozen until hwrtvelfreezepose 0, blue armour + shotgun)",
            mdl ? mdl : "mrfixit", po.x, po.y, po.z, cam.x, cam.y, cam.z, camera1 ? camera1->yaw : 0);
}
COMMAND(hwrtvelplacechar, "");

static void hwrtvelanim(int *n)
{
    int a = *n;
    if(a) hwrtvelfreezepose = 0;
    setvar("animoverride", a);
    conoutf("hwrt velocity: animoverride %d (0=off, 2=idle, 3=forward, 4=backward)", a);
}
COMMAND(hwrtvelanim, "i");

static void hwrtvelfirstperson()
{
    extern int thirdperson;
    thirdperson = 0;
    hwrtvelholdcam = 0;
    hwrtvelholdplayer = 0;
    conoutf("hwrt velocity: first person - local body is shadow occluder only, must not write vectors on the floor");
}
COMMAND(hwrtvelfirstperson, "");

static void hwrtvelplacehud()
{
    extern int thirdperson;
    execute("paused 0");
    execute("spectator 0");
    veloverlapmode = 0;
    velplacetwo = 0;
    if(editmode) toggleedit(false);
    if(player && player->state == CS_DEAD) execute("respawn");
    execute("delbot");
    setvar("playermodel", 0);
    setvar("forceplayermodels", 1);
    setvar("hudgun", 1);
    setvar("hudgunsway", 0);
    thirdperson = 0;
    hwrtvelwalk = 0;
    hwrtvelholdplayer = 1;
    hwrtvelholdcam = 1;
    setvar("animoverride", 0);
    hwrtvelfreezepose = 1;
    hwrtvelspin = 0;
    hwrtvelpitchspin = 0;
    vec po(668, 591, 566);
    if(player)
    {
        player->o = po;
        player->yaw = 180;
        player->pitch = -8;
        player->vel = vec(0, 0, 0);
        player->falling = vec(0, 0, 0);
        player->move = 0;
        player->strafe = 0;
        player->resetinterp();
        velholdplayerpos = po;
        velholdplayeryaw = 180;
        velholdplayerpitch = -8;
    }
    if(camera1)
    {
        camera1->o = po;
        camera1->yaw = 180;
        camera1->pitch = -8;
        velholdcampos = po;
        velholdyaw = 180;
        velholdpitch = -8;
        velholdpos = po;
    }
    execute("hwrtvelgiveequip 0 1 0");
    execute("hwrtvelkeepgun 1");
    conoutf("hwrt velocity: first-person shotgun at %.1f %.1f %.1f yaw 180 pitch -8 (hudgun on, sway off, pose frozen)",
            po.x, po.y, po.z);
}
COMMAND(hwrtvelplacehud, "");

static void hwrtvelhudsway()
{
    extern int thirdperson;
    thirdperson = 0;
    setvar("hudgun", 1);
    setvar("hudgunsway", 1);
    hwrtvelfreezepose = 0;
    hwrtvelholdplayer = 0;
    hwrtvelholdcam = 0;
    hwrtvelwalk = 1;
    setvar("animoverride", 0);
    if(player)
    {
        if(player->state != CS_ALIVE) player->state = CS_ALIVE;
        player->move = 1;
    }
    conoutf("hwrt velocity: first-person walk with hudgun sway");
}
COMMAND(hwrtvelhudsway, "");

static void hwrtvelhudspinview()
{
    extern int thirdperson;
    thirdperson = 0;
    setvar("hudgun", 1);
    setvar("hudgunsway", 0);
    hwrtvelfreezepose = 1;
    hwrtvelholdplayer = 1;
    hwrtvelholdcam = 1;
    hwrtvelwalk = 0;
    hwrtvelspin = 0.45f;
    if(player)
    {
        velholdplayerpos = player->o;
        velholdplayeryaw = player->yaw;
        velholdplayerpitch = player->pitch;
        player->move = 0;
        player->vel = vec(0, 0, 0);
    }
    if(camera1)
    {
        velholdcampos = camera1->o;
        velholdyaw = camera1->yaw;
        velholdpitch = camera1->pitch;
    }
    conoutf("hwrt velocity: first-person camera yaw spin, player held, gun follows view");
}
COMMAND(hwrtvelhudspinview, "");

static void hwrtvelthirdperson()
{
    extern int thirdperson;
    thirdperson = 1;
    conoutf("hwrt velocity: third person - local mrfixit is in the colour image");
}
COMMAND(hwrtvelthirdperson, "");

static void hwrtveladdbots(int *n)
{
    int want = clamp(*n, 1, 8);
    loopi(want) execute("addbot");
    conoutf("hwrt velocity: requested %d bots (addbot). forceplayermodels 1 keeps mrfixit.", want);
}
COMMAND(hwrtveladdbots, "i");

static void hwrtveltwochars()
{
    extern int thirdperson;
    thirdperson = 1;
    hwrtvelfreezepose = 0;
    hwrtvelholdplayer = 1;
    hwrtvelholdcam = 1;
    hwrtvelholdothers = 1;
    hwrtvelwalk = 1;
    setvar("animoverride", 0);
    if(player)
    {
        velholdplayerpos = player->o;
        velholdplayeryaw = player->yaw;
        velholdplayerpitch = player->pitch;
        player->move = 1;
    }
    execute("addbot");
    execute("hwrtvelgiveequip 0 1 0");
    velplacetwo = 45;
    conoutf("hwrt velocity: local mrfixit walks in place; extra bot overlaps it on screen and stays idle");
}
COMMAND(hwrtveltwochars, "");

static void hwrtvelslottrace(char *name)
{
    if(velslotlogf)
    {
        fclose(velslotlogf);
        velslotlogf = NULL;
    }
    if(!name || !name[0] || !strcmp(name, "-") || !strcmp(name, "0"))
    {
        conoutf("hwrt velocity: slot trace off");
        return;
    }
    string savedhome;
    copystring(savedhome, homedir);
    homedir[0] = '\0';
    velshotdir();
    formatstring(velslotlogpath, "shots/velocity-lot6/%s.txt", name);
    path(velslotlogpath);
    velslotlogf = fopen(velslotlogpath, "w");
    copystring(homedir, savedhome);
    velslotlogn = 0;
    if(!velslotlogf)
    {
        conoutf(CON_WARN, "hwrt velocity: cannot write %s", velslotlogpath);
        return;
    }
    velslotlog("slot trace %s stealpolicy %d drawrev %d\n", name, int(hwrtvelslotsteal), int(hwrtveldrawrev));
    conoutf("hwrt velocity: slot trace %s", velslotlogpath);
}
COMMAND(hwrtvelslottrace, "s");

static void hwrtvelslotwatch()
{
    velwatchn = 0;
    loopi(velskeln)
    {
        velskelhist &h = velskels[i];
        if(!h.d) continue;
        velslotwatch &w = velwatches[velwatchn++];
        w.uid = h.uid;
        w.kind = h.attachkind;
        w.key = h.attachkey;
        w.slot = i;
        w.committed = h.committedframe;
        w.notedframe = h.notedframe;
        w.usedprev = h.usedprev;
        w.validprev = h.validprev;
    }
    velslotlog("WATCH_ARM n=%d frame %d\n", velwatchn, velskelframe);
    conoutf("hwrt velocity: watching %d live histories", velwatchn);
}
COMMAND(hwrtvelslotwatch, "");

static void hwrtvelslotchars()
{
    extern int thirdperson;
    execute("hwrtvelplacechar");
    thirdperson = 1;
    hwrtvelfreezepose = 0;
    hwrtvelholdplayer = 1;
    hwrtvelholdcam = 1;
    hwrtvelholdothers = 1;
    hwrtvelwalk = 1;
    setvar("animoverride", 0);
    setvar("hwrtvelkeepgun", 1);
    setvar("hwrtvelquadwho", 0);
    if(player)
    {
        player->move = 1;
        velholdplayerpos = player->o;
        velholdplayeryaw = player->yaw;
        velholdplayerpitch = player->pitch;
    }
    execute("addbot");
    execute("hwrtvelgiveequip 0 1 0");
    velplacetwo = 100000;
    conoutf("hwrt velocity: slot-test two mrfixit, same shotgun, local walks, bot idle");
}
COMMAND(hwrtvelslotchars, "");

static void hwrtvelgameparticles()
{
    if(!camera1)
    {
        conoutf("hwrt velocity: no camera");
        return;
    }
    // Real game sprites only. The grey test veil (hwrtvelsmoke) is a lot-2
    // stand-in and must not be mistaken for this trial.
    hwrtvelsmoke = 0;
    vec p = camera1->o;
    if(player) p = vec(player->o).add(vec(camera1->o).sub(player->o).mul(0.35f));
    else p.add(vec(camdir).mul(hwrtvelsmokedist));
    particle_splash(PART_SMOKE, 40, 2000, p, 0x888888, 5.0f, 8, -20);
    particle_fireball(p, 12, PART_EXPLOSION, 800, 0xFF8020, 6.0f);
    conoutf("hwrt velocity: requested game particles at %.1f %.1f %.1f (no artificial veil). If none appear, this test is not done.",
            p.x, p.y, p.z);
}
COMMAND(hwrtvelgameparticles, "");

static void hwrtvelwalkinplace()
{
    extern int thirdperson;
    thirdperson = 1;
    hwrtvelfreezepose = 0;
    hwrtvelholdplayer = 1;
    hwrtvelholdcam = 1;
    hwrtvelwalk = 1;
    setvar("animoverride", 3);
    if(player)
    {
        velholdplayerpos = player->o;
        velholdplayeryaw = player->yaw;
        velholdplayerpitch = player->pitch;
        player->move = 1;
    }
    conoutf("hwrt velocity: in-place forward (camera fixed, global pos held, limbs should move)");
}
COMMAND(hwrtvelwalkinplace, "");

static void hwrtvelwalkcam()
{
    extern int thirdperson;
    thirdperson = 1;
    hwrtvelfreezepose = 0;
    hwrtvelholdplayer = 0;
    hwrtvelholdcam = 1;
    hwrtvelwalk = 1;
    setvar("animoverride", 0);
    if(player)
    {
        if(player->state != CS_ALIVE) player->state = CS_ALIVE;
        if(camera1)
        {
            player->yaw = camera1->yaw;
            while(player->yaw >= 360) player->yaw -= 360;
            while(player->yaw < 0) player->yaw += 360;
        }
        player->move = 1;
        velwalkfrom = player->o;
        velwalkset = true;
    }
    conoutf("hwrt velocity: walk with camera held - global translation + limb deformation");
}
COMMAND(hwrtvelwalkcam, "");

static void hwrtvelbothmove()
{
    extern int thirdperson;
    thirdperson = 1;
    hwrtvelfreezepose = 0;
    hwrtvelholdplayer = 1;
    hwrtvelholdcam = 1;
    hwrtvelwalk = 1;
    hwrtvelspin = 0.45f;
    setvar("animoverride", 3);
    if(player)
    {
        velholdplayerpos = player->o;
        velholdplayeryaw = player->yaw;
        velholdplayerpitch = player->pitch;
        player->move = 1;
    }
    conoutf("hwrt velocity: camera spin + in-place limb animation (character stays in frame)");
}
COMMAND(hwrtvelbothmove, "");

static void hwrtvelhidechar()
{
    if(!camera1 || !player)
    {
        conoutf("hwrt velocity: no camera/player");
        return;
    }
    vec fwd, hit;
    vec besthit(0, 0, 0), bestfwd(0, 0, 0);
    float bestd = 1e9f;
    bool found = false;
    for(int dyaw = -70; dyaw <= 70; dyaw += 10)
    {
        vecfromyawpitch(camera1->yaw + dyaw, clamp(camera1->pitch, -40.0f, 10.0f), 1, 0, fwd);
        float dist = raycubepos(camera1->o, fwd, hit, 0, RAY_CLIPMAT|RAY_SKIPFIRST);
        if(dist > 10 && dist < 180 && dist < bestd)
        {
            bestd = dist;
            besthit = hit;
            bestfwd = fwd;
            found = true;
        }
    }
    if(!found)
    {
        conoutf("hwrt velocity: no wall ahead to hide behind");
        return;
    }
    vec po = vec(besthit).add(vec(bestfwd).mul(18));
    po.z = player->o.z;
    player->o = po;
    player->vel = vec(0, 0, 0);
    player->falling = vec(0, 0, 0);
    player->move = 0;
    player->resetinterp();
    velholdplayerpos = po;
    velholdplayeryaw = player->yaw;
    velholdplayerpitch = player->pitch;
    hwrtvelholdplayer = 1;
    hwrtvelholdcam = 1;
    hwrtvelwalk = 0;
    hwrtvelfreezepose = 1;
    conoutf("hwrt velocity: character behind wall at %.1f %.1f %.1f (wall dist %.1f). Wall vectors must remain.",
            po.x, po.y, po.z, bestd);
}
COMMAND(hwrtvelhidechar, "");

static void hwrtvelwallocclude()
{
    if(!camera1 || !player)
    {
        conoutf("hwrt velocity: no camera/player");
        return;
    }
    execute("paused 0");
    execute("spectator 0");
    veloverlapmode = 0;
    hwrtvelclearplaced();
    vec cam = camera1->o;
    vec pl = player->o;
    float holdyaw = camera1->yaw, holdpitch = camera1->pitch;
    vec dir = vec(pl).sub(cam);
    float dist = dir.magnitude();
    if(dist < 12)
    {
        conoutf("hwrt velocity: camera too close to place an occluder");
        return;
    }
    dir.mul(1.0f / dist);
    vec up(0, 0, 1);
    vec right;
    right.cross(dir, up);
    if(right.squaredlen() < 1e-6f) right = vec(1, 0, 0);
    else right.normalize();
    vec mid = vec(cam).add(vec(dir).mul(dist * 0.42f));
    float feetz = pl.z - player->eyeheight;
    mid.z = feetz + 10;
    const char *name = "xeno/box1";
    bool wasedit = editmode;
    if(!editmode) toggleedit(true);
    int midx = velensuremm(name);
    model *m = loadmapmodel(midx);
    vec bc(0, 0, 0), br(4, 4, 4);
    float scale = 1;
    if(m)
    {
        m->boundbox(bc, br);
        if(m->scale > 0) scale = m->scale;
    }
    float stepx = clamp(max(br.x, br.y) * 2 * scale * 0.85f, 3.0f, 10.0f);
    float stepz = clamp(br.z * 2 * scale * 0.85f, 3.0f, 10.0f);
    int nx = 5, nz = 5;
    int yaw = int(camera1->yaw) % 360;
    int nplaced = 0;
    for(int iz = 0; iz < nz; iz++)
        for(int ix = -nx/2; ix <= nx/2; ix++)
        {
            vec o = mid;
            o.add(vec(right).mul(ix * stepx));
            o.z = feetz + 1.5f + iz * stepz;
            int pidx = -1;
            if(velplacemm(name, o, yaw, &pidx)) nplaced++;
        }
    if(!wasedit && editmode) toggleedit(false);
    if(player)
    {
        player->o = pl;
        player->vel = vec(0, 0, 0);
        player->falling = vec(0, 0, 0);
        player->move = 0;
        player->resetinterp();
        velholdplayerpos = pl;
        velholdplayeryaw = player->yaw;
        velholdplayerpitch = player->pitch;
    }
    camera1->o = cam;
    camera1->yaw = holdyaw;
    camera1->pitch = holdpitch;
    velholdcampos = cam;
    velholdyaw = holdyaw;
    velholdpitch = holdpitch;
    hwrtvelholdplayer = 1;
    hwrtvelholdcam = 1;
    hwrtvelwalk = 0;
    hwrtvelfreezepose = 1;
    conoutf("hwrt velocity: occluder wall %d boxes on view ray at %.1f %.1f %.1f (player unchanged %.1f %.1f %.1f, cam %.1f %.1f %.1f). Body must stay in frustum, vectors must not punch the wall.",
            nplaced, mid.x, mid.y, mid.z, pl.x, pl.y, pl.z, cam.x, cam.y, cam.z);
}
COMMAND(hwrtvelwallocclude, "");

static void hwrtveloverlapchars()
{
    extern int thirdperson;
    thirdperson = 1;
    velplacetwo = 0;
    veloverlapmode = 1;
    hwrtvelfreezepose = 0;
    hwrtvelholdplayer = 1;
    hwrtvelholdcam = 1;
    hwrtvelholdothers = 1;
    hwrtvelwalk = 1;
    // Global animoverride would force the same clip on the bot. Local walks
    // in place (move 1); the bot stays idle (move 0 via holdothers).
    setvar("animoverride", 0);
    if(player)
    {
        velholdplayerpos = player->o;
        velholdplayeryaw = player->yaw;
        velholdplayerpitch = player->pitch;
        player->move = 1;
    }
    execute("addbot");
    execute("hwrtvelgiveequip 0 1 0");
    conoutf("hwrt velocity: bot idle in front of local mrfixit (same camera ray, 6u closer). Local runs in place. Overlap must produce occluded_other_char.");
}
COMMAND(hwrtveloverlapchars, "");

static void hwrtvelswitchmodel()
{
    int next = getvar("playermodel") ? 0 : 1;
    setvar("playermodel", next);
    conoutf("hwrt velocity: playermodel %d (0=mrfixit supported this lot, 1=snoutx10k shares the path but is not claimed)", next);
}
COMMAND(hwrtvelswitchmodel, "");

static void velaimat(const vec &target);

static int velensuremm(const char *name)
{
    extern vector<mapmodelinfo> mapmodels;
    loopv(mapmodels) if(!strcmp(mapmodels[i].name, name)) return i;
    defformatstring(cmd, "mapmodel 0 0 0 \"%s\"", name);
    execute(cmd);
    return mapmodels.length() - 1;
}

static extentity *velplacemm(const char *name, const vec &o, int yaw, int *outidx)
{
    extern vector<mapmodelinfo> mapmodels;
    bool wasedit = editmode;
    if(!editmode) toggleedit(true);
    int midx = velensuremm(name);
    extern int entcamdir;
    int oldcd = entcamdir;
    entcamdir = 0;
    int n0 = entities::getents().length();
    defformatstring(cmd, "newent mapmodel %d %d", yaw, midx);
    execute(cmd);
    entcamdir = oldcd;
    const vector<extentity *> &ents = entities::getents();
    extentity *placed = NULL;
    int pidx = -1;
    if(ents.length() > n0)
    {
        placed = ents.last();
        pidx = ents.length() - 1;
    }
    else
        loopvrev(ents)
            if(ents[i]->type == ET_MAPMODEL)
            {
                placed = ents[i];
                pidx = i;
                break;
            }
    if(placed)
    {
        placed->type = ET_MAPMODEL;
        placed->attr1 = yaw;
        placed->attr2 = midx;
        placed->attr3 = 0;
        placed->o = o;
        if(velobjs.inrange(pidx)) velobjs[pidx].valid = false;
        bool already = false;
        loopv(velplaced) if(velplaced[i] == pidx) { already = true; break; }
        if(!already) velplaced.add(pidx);
    }
    if(!wasedit && editmode) toggleedit(false);
    if(outidx) *outidx = pidx;
    return placed;
}

static void hwrtvelplacefan()
{
    if(!camera1) { conoutf("hwrt velocity: no camera"); return; }
    vec fwd;
    vecfromyawpitch(camera1->yaw, 0, 1, 0, fwd);
    vec o = vec(camera1->o).add(fwd.mul(22));
    o.z = camera1->o.z;
    int pidx = -1;
    extentity *placed = velplacemm("mapmodels/yves_allaire/e6/e6fanblade/vertical", o, 0, &pidx);
    model *m = placed ? loadmapmodel(placed->attr2) : NULL;
    if(m && fabs(m->spinyaw) < 1e-3f && fabs(m->spinpitch) < 1e-3f)
    {
        m->spinpitch = 500;
        conoutf("hwrt velocity: mdlspin was 0, forcing spinpitch 500 on this model");
    }
    hwrtveldrive = pidx;
    conoutf("hwrt velocity: fan idx %d at %.1f %.1f %.1f spin=%.1f/%.1f rigid=%d static=%d deform=%d (hwrtveldrive %d)",
            pidx, o.x, o.y, o.z,
            m ? m->spinyaw : 0, m ? m->spinpitch : 0,
            placed ? (hwrtvelrigidmapmodel(*placed) ? 1 : 0) : -1,
            placed ? (hwrtvelstaticmapmodel(*placed) ? 1 : 0) : -1,
            m && m->deforms() ? 1 : 0, int(hwrtveldrive));
    hwrtveldbgmm = 1;
}
COMMAND(hwrtvelplacefan, "");

static void hwrtvelmodelspin(float *yaw, float *pitch)
{
    const vector<extentity *> &ents = entities::getents();
    int idx = hwrtveldrive;
    if(!ents.inrange(idx) || !ents[idx] || ents[idx]->type != ET_MAPMODEL)
    {
        conoutf("hwrt velocity: no driven model to set spin");
        return;
    }
    model *m = loadmapmodel(ents[idx]->attr2);
    if(!m)
    {
        conoutf("hwrt velocity: no model for spin");
        return;
    }
    m->spinyaw = *yaw;
    m->spinpitch = *pitch;
    if(velobjs.inrange(idx)) velobjs[idx].valid = false;
    conoutf("hwrt velocity: model spin %.1f %.1f on idx %d", *yaw, *pitch, idx);
}
COMMAND(hwrtvelmodelspin, "ff");

static void hwrtvelplaceprop()
{
    if(!camera1) { conoutf("hwrt velocity: no camera"); return; }
    vec fwd;
    vecfromyawpitch(camera1->yaw, 0, 1, 0, fwd);
    vec o = vec(camera1->o).add(fwd.mul(22));
    o.z = camera1->o.z - 1;
    int pidx = -1;
    extentity *placed = velplacemm("mapmodels/justice/vending", o, 30, &pidx);
    hwrtveldrive = pidx;
    conoutf("hwrt velocity: vending idx %d at %.1f %.1f %.1f rigid=%d deform=%d (hwrtveldrive %d)",
            pidx, o.x, o.y, o.z,
            placed ? (hwrtvelrigidmapmodel(*placed) ? 1 : 0) : -1,
            (placed && loadmapmodel(placed->attr2) && loadmapmodel(placed->attr2)->deforms()) ? 1 : 0,
            int(hwrtveldrive));
}
COMMAND(hwrtvelplaceprop, "");

static void hwrtvelplaceat(float *x, float *y, float *z, float *yaw)
{
    vec o(*x, *y, *z);
    int pidx = -1;
    extentity *placed = velplacemm("mapmodels/justice/vending", o, int(*yaw), &pidx);
    hwrtveldrive = pidx;
    conoutf("hwrt velocity: vending idx %d at %.1f %.1f %.1f yaw %d (hwrtveldrive %d)",
            pidx, o.x, o.y, o.z, int(*yaw), int(hwrtveldrive));
    (void)placed;
}
COMMAND(hwrtvelplaceat, "ffff");

static void hwrtvelplaceoccluded()
{
    if(!camera1) { conoutf("hwrt velocity: no camera"); return; }
    vec left, hit;
    vecfromyawpitch(camera1->yaw, 0, 0, -1, left);
    float dist = raycubepos(camera1->o, left, hit, 0, RAY_CLIPMAT|RAY_SKIPFIRST);
    if(dist < 3)
    {
        vecfromyawpitch(camera1->yaw, 0, 0, 1, left);
        dist = raycubepos(camera1->o, left, hit, 0, RAY_CLIPMAT|RAY_SKIPFIRST);
    }
    if(dist < 3)
    {
        conoutf("hwrt velocity: no side wall to hide behind");
        return;
    }
    vec o = vec(hit).add(vec(left).mul(10));
    int pidx = -1;
    extentity *placed = velplacemm("mapmodels/yves_allaire/e6/e6fanblade/vertical", o, 0, &pidx);
    conoutf("hwrt velocity: occluded fan idx %d behind side wall at %.1f %.1f %.1f (hit dist %.1f) rigid=%d",
            pidx, o.x, o.y, o.z, dist, placed ? (hwrtvelrigidmapmodel(*placed) ? 1 : 0) : -1);
}
COMMAND(hwrtvelplaceoccluded, "");

static void hwrtvelplacealphafront()
{
    if(!camera1) { conoutf("hwrt velocity: no camera"); return; }
    vec fwd;
    vecfromyawpitch(camera1->yaw, 0, 1, 0, fwd);
    vec o = vec(camera1->o).add(fwd.mul(18));
    o.z = camera1->o.z;
    int pidx = -1;
    extentity *placed = velplacemm("mapmodels/yves_allaire/e6/e6fanblade/vertical", o, 0, &pidx);
    conoutf("hwrt velocity: alpha fan idx %d in front of camera at %.1f %.1f %.1f rigid=%d",
            pidx, o.x, o.y, o.z, placed ? (hwrtvelrigidmapmodel(*placed) ? 1 : 0) : -1);
}
COMMAND(hwrtvelplacealphafront, "");

static void hwrtveldeldriven()
{
    const vector<extentity *> &ents = entities::getents();
    if(!ents.inrange(hwrtveldrive) || !ents[hwrtveldrive])
    {
        conoutf("hwrt velocity: no driven entity to delete");
        return;
    }
    ents[hwrtveldrive]->type = ET_EMPTY;
    if(velobjs.inrange(hwrtveldrive)) velobjs[hwrtveldrive].valid = false;
    conoutf("hwrt velocity: cleared entity %d", int(hwrtveldrive));
    hwrtveldrive = -1;
}
COMMAND(hwrtveldeldriven, "");

static void hwrtvelclearplaced()
{
    const vector<extentity *> &ents = entities::getents();
    int n = 0;
    loopv(velplaced)
    {
        int idx = velplaced[i];
        if(!ents.inrange(idx) || !ents[idx]) continue;
        if(ents[idx]->type == ET_MAPMODEL)
        {
            ents[idx]->type = ET_EMPTY;
            n++;
        }
        if(velobjs.inrange(idx)) velobjs[idx].valid = false;
    }
    velplaced.setsize(0);
    hwrtveldrive = -1;
    conoutf("hwrt velocity: cleared %d placed test mapmodels", n);
}
COMMAND(hwrtvelclearplaced, "");

static void velabstore(const char *name)
{
    string savedhome;
    copystring(savedhome, homedir);
    homedir[0] = '\0';
    velshotdir();
    int w = screenw, h = screenh;
    DELETEA(velabref);
    DELETEA(velabmask);
    velabref = new uchar[w * h * 3];
    velabmask = new uchar[w * h];
    memset(velabmask, 0, w * h);
    velabw = w;
    velabh = h;
    GLint prevfb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevfb);
    glBindFramebuffer_(GL_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, velabref);
    glBindFramebuffer_(GL_FRAMEBUFFER, prevfb);
    if(velhudcolok && velhudcolst && velhudcolw == w && velhudcolh == h)
        memcpy(velabmask, velhudcolst, w * h);
    velabmuzzle = velcolourmuzzle;
    velabok = true;
    ImageData image(w, h, 3);
    memcpy(image.data, velabref, w * h * 3);
    defformatstring(fn, "shots/velocity-lot6/%s.png", name);
    path(fn);
    velsavepng(fn, image);
    copystring(homedir, savedhome);
    int nhud = 0;
    loopi(w * h) if(velabmask[i] == VEL_ST_HUDCOL) nhud++;
    conoutf("hwrt velocity: colour A/B store %s  %dx%d  hud_stencil %d  muzzle %.3f %.3f %.4f  velocity %d",
            fn, w, h, nhud, velabmuzzle.x, velabmuzzle.y, velabmuzzle.z, int(hwrtvelocity));
}

static void velabdiff(const char *name)
{
    string savedhome;
    copystring(savedhome, homedir);
    homedir[0] = '\0';
    velshotdir();
    int w = screenw, h = screenh;
    ImageData cur(w, h, 3), diffim(w, h, 3);
    GLint prevfb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevfb);
    glBindFramebuffer_(GL_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, cur.data);
    glBindFramebuffer_(GL_FRAMEBUFFER, prevfb);
    defformatstring(fncur, "shots/velocity-lot6/%s.png", name);
    defformatstring(fndiff, "shots/velocity-lot6/%s_diff.png", name);
    defformatstring(fninfo, "shots/velocity-lot6/%s_info.txt", name);
    path(fncur); path(fndiff); path(fninfo);
    velsavepng(fncur, cur);
    int nany = 0, nstrong = 0, nhud = 0, nhudany = 0, nhudstrong = 0;
    double sum = 0, hudsum = 0;
    int maxd = 0, hudmax = 0;
    bool have = velabok && velabref && velabw == w && velabh == h;
    if(have)
    {
        loopi(h) loopj(w)
        {
            int idx = (i * w + j) * 3;
            int dr = abs(int(cur.data[idx]) - int(velabref[idx]));
            int dg = abs(int(cur.data[idx + 1]) - int(velabref[idx + 1]));
            int db = abs(int(cur.data[idx + 2]) - int(velabref[idx + 2]));
            int d = dr + dg + db;
            bool hud = velabmask && velabmask[i * w + j] == VEL_ST_HUDCOL;
            if(d > 0)
            {
                nany++;
                if(hud) nhudany++;
            }
            if(d > 12)
            {
                nstrong++;
                if(hud) nhudstrong++;
            }
            sum += d;
            if(d > maxd) maxd = d;
            if(hud)
            {
                nhud++;
                hudsum += d;
                if(d > hudmax) hudmax = d;
            }
            int sr = cur.data[idx] / 3, sg = cur.data[idx + 1] / 3, sb = cur.data[idx + 2] / 3;
            if(d > 12) { sr = hud ? 255 : 40; sg = hud ? 40 : 200; sb = hud ? 40 : 40; }
            velputrgb(diffim, j, i, sr, sg, sb);
        }
        velsavepng(fndiff, diffim);
    }
    FILE *tf = fopen(fninfo, "w");
    if(tf)
    {
        fprintf(tf, "name %s\n", name);
        fprintf(tf, "compare colour with velocity generation ON (store) vs OFF (this frame)\n");
        fprintf(tf, "size %dx%d  store_ok %s  lastmillis %d  freezepose %d  holdcam %d  holdplayer %d\n",
                w, h, have ? "yes" : "no", lastmillis, int(hwrtvelfreezepose), int(hwrtvelholdcam), int(hwrtvelholdplayer));
        fprintf(tf, "muzzle_store %.4f %.4f %.4f  muzzle_now %.4f %.4f %.4f\n",
                velabmuzzle.x, velabmuzzle.y, velabmuzzle.z,
                velcolourmuzzle.x, velcolourmuzzle.y, velcolourmuzzle.z);
        fprintf(tf, "full_frame  changed_any %d  changed_gt12 %d  mean_abs_rgb %.4f  max %d  pixels %d\n",
                nany, nstrong, (w > 0 && h > 0) ? sum / (w * h) : 0, maxd, w * h);
        fprintf(tf, "hud_colour_mask  pixels %d  changed_any %d  changed_gt12 %d  mean_abs_rgb %.4f  max %d\n",
                nhud, nhudany, nhudstrong, nhud ? hudsum / nhud : 0, hudmax);
        fprintf(tf, "diff_legend dim colour, red=hud pixel changed>12, green=other pixel changed>12\n");
        fclose(tf);
    }
    copystring(homedir, savedhome);
    conoutf("hwrt velocity: colour A/B diff %s  hud changed>12 %d/%d  full %d  muzzle now %.3f %.3f %.3f",
            name, nhudstrong, nhud, nstrong, velcolourmuzzle.x, velcolourmuzzle.y, velcolourmuzzle.z);
}

void hwrtflushvelshot()
{
    if(velshotcount <= 0 || screenw < 8 || screenh < 8) return;
    int kind = velshotq[velshothead].kind;
    if(kind == VELSHOT_SETVEL || kind == VELSHOT_ZOOM || kind == VELSHOT_CAMCUT ||
       kind == VELSHOT_SETJITTER || kind == VELSHOT_SCREENRES || kind == VELSHOT_FIRE ||
       kind == VELSHOT_FREEZEPOSE) return;
    if(kind == VELSHOT_PROOFALIVE)
    {
        if(!velaliveproofready() && velalivewait < 90)
        {
            velalivewait++;
            return;
        }
        if(!velaliveproofready())
            conoutf(CON_WARN, "hwrt velocity: proofalive %s timed out still not alive", velshotq[velshothead].name);
        velalivewait = 0;
        kind = VELSHOT_PROOF;
    }
    string name;
    copystring(name, velshotq[velshothead].name);
    velshotpop();
    if(!name[0] || name[0] == '-') return;
    for(char *s = name; *s; s++) if(*s == '/' || *s == '\\' || *s == ':' || iscubespace(*s)) *s = '_';

    if(kind == VELSHOT_ABSTORE)
    {
        velabstore(name);
        return;
    }
    if(kind == VELSHOT_ABDIFF)
    {
        velabdiff(name);
        return;
    }

    string savedhome;
    copystring(savedhome, homedir);
    homedir[0] = '\0';
    velshotdir();

    defformatstring(fn, velshotkindproof(kind) ? "shots/velocity-lot6/%s_overlay.png" : "shots/velocity-lot6/%s.png", name);
    path(fn);

    ImageData image(screenw, screenh, 3);
    GLint prevfb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevfb);
    glBindFramebuffer_(GL_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, texalign(image.data, screenw, 3));
    glReadPixels(0, 0, screenw, screenh, GL_RGB, GL_UNSIGNED_BYTE, image.data);
    glBindFramebuffer_(GL_FRAMEBUFFER, prevfb);

    velsavepng(fn, image);

    copystring(homedir, savedhome);
    conoutf("hwrt velocity shot %s  lastmillis %d debug %d", fn, lastmillis, int(hwrtveldebug));
}

static void hwrtvelshot(char *name)
{
    velshotpush(name && name[0] ? name : "vel", hwrtveldebug);
}
COMMAND(hwrtvelshot, "s");

static void hwrtvelwaitshot(int *n)
{
    int frames = clamp(*n, 1, 90);
    loopi(frames) velshotpush("-", hwrtveldebug, VELSHOT_WAIT);
}
COMMAND(hwrtvelwaitshot, "i");

static void hwrtvelproofshot(char *name)
{
    velshotpush(name && name[0] ? name : "proof", hwrtveldebug, VELSHOT_PROOF);
}
COMMAND(hwrtvelproofshot, "s");

static void hwrtvelabstore(char *name)
{
    velshotpush(name && name[0] ? name : "ab_on", 0, VELSHOT_ABSTORE);
}
COMMAND(hwrtvelabstore, "s");

static void hwrtvelabdiff(char *name)
{
    velshotpush(name && name[0] ? name : "ab_off", 0, VELSHOT_ABDIFF);
}
COMMAND(hwrtvelabdiff, "s");

static void hwrtvelqvelocity(int *on)
{
    velshotpush("-", hwrtveldebug, VELSHOT_SETVEL, *on ? 1.0f : 0.0f);
}
COMMAND(hwrtvelqvelocity, "i");

static void hwrtvelproofalive(char *name)
{
    velalivewait = 0;
    velshotpush(name && name[0] ? name : "spawn_t", hwrtveldebug, VELSHOT_PROOFALIVE);
}
COMMAND(hwrtvelproofalive, "s");

static void hwrtvelqcamcut()
{
    velshotpush("-", hwrtveldebug, VELSHOT_CAMCUT);
}
COMMAND(hwrtvelqcamcut, "");

static void hwrtvelfacewall()
{
    if(!camera1)
    {
        conoutf("hwrt velocity: no camera");
        return;
    }
    vec besthit(0, 0, 0), bestfwd(0, 0, 0);
    float bestd = 1e9f;
    bool found = false;
    for(int dyaw = -90; dyaw <= 90; dyaw += 15)
    {
        for(int dp = -25; dp <= 5; dp += 10)
        {
            vec fwd, hit;
            vecfromyawpitch(camera1->yaw + dyaw, clamp(camera1->pitch + dp, -89.0f, 89.0f), 1, 0, fwd);
            float dist = raycubepos(camera1->o, fwd, hit, 0, RAY_CLIPMAT|RAY_SKIPFIRST);
            if(dist > 7 && dist < 40 && dist < bestd)
            {
                bestd = dist;
                besthit = hit;
                bestfwd = fwd;
                found = true;
            }
        }
    }
    if(!found)
    {
        conoutf("hwrt velocity: no nearby wall to face");
        return;
    }
    vec o = vec(besthit).sub(vec(bestfwd).mul(12));
    float yaw, pitch;
    vectoyawpitch(vec(besthit).sub(o), yaw, pitch);
    velholdset(o, yaw, pitch);
    conoutf("hwrt velocity: facing wall dist %.1f at %.1f %.1f %.1f yaw %.1f pitch %.1f",
            bestd, o.x, o.y, o.z, yaw, pitch);
}
COMMAND(hwrtvelfacewall, "");

static void hwrtvelhudwall()
{
    extern int thirdperson;
    if(!camera1)
    {
        conoutf("hwrt velocity: no camera");
        return;
    }
    thirdperson = 0;
    setvar("hudgun", 1);
    vec besthit(0, 0, 0), bestfwd(0, 0, 0);
    float bestd = 1e9f;
    bool found = false;
    for(int dyaw = -90; dyaw <= 90; dyaw += 15)
    {
        for(int dp = -25; dp <= 5; dp += 10)
        {
            vec fwd, hit;
            vecfromyawpitch(camera1->yaw + dyaw, clamp(camera1->pitch + dp, -89.0f, 89.0f), 1, 0, fwd);
            float dist = raycubepos(camera1->o, fwd, hit, 0, RAY_CLIPMAT|RAY_SKIPFIRST);
            if(dist > 3 && dist < 50 && dist < bestd)
            {
                bestd = dist;
                besthit = hit;
                bestfwd = fwd;
                found = true;
            }
        }
    }
    if(!found)
    {
        conoutf("hwrt velocity: no nearby wall for hud clip");
        return;
    }
    vec o = vec(besthit).sub(vec(bestfwd).mul(2.6f));
    float yaw, pitch;
    vectoyawpitch(vec(besthit).sub(o), yaw, pitch);
    velholdset(o, yaw, pitch);
    hwrtvelfreezepose = 1;
    conoutf("hwrt velocity: hud against wall dist %.2f at %.1f %.1f %.1f yaw %.1f pitch %.1f",
            2.6f, o.x, o.y, o.z, yaw, pitch);
}
COMMAND(hwrtvelhudwall, "");

static void hwrtvelhudoccluder()
{
    extern int thirdperson;
    if(!camera1)
    {
        conoutf("hwrt velocity: no camera");
        return;
    }
    thirdperson = 0;
    setvar("hudgun", 1);
    vec fwd;
    vecfromyawpitch(camera1->yaw, camera1->pitch, 1, 0, fwd);
    vec o = vec(camera1->o).add(fwd.mul(3.4f));
    o.z -= 0.35f;
    int pidx = -1;
    extentity *placed = velplacemm("mapmodels/justice/vending", o, int(camera1->yaw) + 90, &pidx);
    hwrtveldrive = pidx;
    hwrtvelfreezepose = 1;
    conoutf("hwrt velocity: opaque vending idx %d at %.1f %.1f %.1f (%.1f in front of hud) rigid=%d",
            pidx, o.x, o.y, o.z, 3.4f,
            placed ? (hwrtvelrigidmapmodel(*placed) ? 1 : 0) : -1);
}
COMMAND(hwrtvelhudoccluder, "");

static void veldocamcut()
{
    hwrtvelholdcam = 0;
    hwrtvelholdplayer = 0;
    hwrtvelspin = 0;
    hwrtvelpitchspin = 0;
    if(!camera1)
    {
        conoutf("hwrt velocity: no camera");
        return;
    }
    vec o = camera1->o;
    o.add(vec(80, 45, 8));
    camera1->o = o;
    camera1->yaw = wrap180(camera1->yaw + 95);
    if(player)
    {
        player->o = o;
        player->yaw = camera1->yaw;
        player->pitch = camera1->pitch;
        player->resetinterp();
    }
    conoutf("hwrt velocity: camera cut to %.1f %.1f %.1f yaw %.1f (hold off, >24 units + >50 yaw)",
            o.x, o.y, o.z, camera1->yaw);
}

static void hwrtvelcamcut()
{
    veldocamcut();
}
COMMAND(hwrtvelcamcut, "");

static void hwrtvelzoom(float *p)
{
    velshotpush("-", hwrtveldebug, VELSHOT_ZOOM, *p);
}
COMMAND(hwrtvelzoom, "f");

static void hwrtvelqjitter(int *on)
{
    velshotpush("-", hwrtveldebug, VELSHOT_SETJITTER, *on ? 1.0f : 0.0f);
}
COMMAND(hwrtvelqjitter, "i");

static void hwrtvelqscreenres(int *w, int *h)
{
    defformatstring(nm, "%d %d", max(*w, 8), max(*h, 8));
    velshotpush(nm, hwrtveldebug, VELSHOT_SCREENRES);
}
COMMAND(hwrtvelqscreenres, "ii");

static void hwrtvelqfire()
{
    velshotpush("-", hwrtveldebug, VELSHOT_FIRE);
}
COMMAND(hwrtvelqfire, "");

static void hwrtvelqfreezepose(int *on)
{
    velshotpush("-", hwrtveldebug, VELSHOT_FREEZEPOSE, *on ? 1.0f : 0.0f);
}
COMMAND(hwrtvelqfreezepose, "i");

static void velaimat(const vec &target)
{
    vec from = target;
    from.add(vec(32, -12, 18));
    if(player)
    {
        player->o = from;
        player->resetinterp();
    }
    if(camera1)
    {
        camera1->o = from;
        vectoyawpitch(vec(target).sub(from), camera1->yaw, camera1->pitch);
        if(player)
        {
            player->yaw = camera1->yaw;
            player->pitch = camera1->pitch;
        }
    }
}

static void hwrtvelaimwater()
{
    vec best(0, 0, 0);
    bool found = false;
    float bestd = 1e9f;
    int step = max(32, worldsize / 32);
    int zstep = max(16, worldsize / 16);
    for(int z = 0; z < worldsize; z += zstep)
    for(int y = 0; y < worldsize; y += step)
    for(int x = 0; x < worldsize; x += step)
    {
        vec p(x, y, z);
        ushort vol = lookupmaterial(p)&MATF_VOLUME;
        if(vol != MAT_WATER && vol != MAT_GLASS) continue;
        float d = camera1 ? p.dist(camera1->o) : 0;
        if(!found || d < bestd) { bestd = d; best = p; found = true; }
    }
    if(!found)
    {
        conoutf("hwrt velocity: no water or glass on this map");
        return;
    }
    velaimat(best);
    conoutf("hwrt velocity: aimed at liquid/glass (%.0f %.0f %.0f)", best.x, best.y, best.z);
}
COMMAND(hwrtvelaimwater, "");

static void hwrtvelaimanim()
{
    const vector<extentity *> &ents = entities::getents();
    vec origin = camera1 ? camera1->o : vec(0, 0, 0);
    const extentity *best = NULL;
    float bestd = 1e9f;
    loopv(ents)
    {
        extentity &e = *ents[i];
        if(e.type != ET_MAPMODEL) continue;
        if(e.flags&EF_NOVIS) continue;
        if(hwrtvelrigidmapmodel(e)) continue;
        float d = e.o.dist(origin);
        if(d < bestd) { bestd = d; best = &e; }
    }
    if(!best)
    {
        conoutf("hwrt velocity: no animated/spinning/skeletal mapmodel on this map");
        return;
    }
    model *m = loadmapmodel(best->attr2);
    vec from = best->o;
    from.add(vec(22, -8, 6));
    if(player)
    {
        player->o = from;
        player->resetinterp();
    }
    if(camera1)
    {
        camera1->o = from;
        vectoyawpitch(vec(best->o).sub(from), camera1->yaw, camera1->pitch);
        if(player)
        {
            player->yaw = camera1->yaw;
            player->pitch = camera1->pitch;
        }
    }
    hwrtveldbgmm = 1;
    conoutf("hwrt velocity: aimed at non-static mapmodel (%.0f %.0f %.0f) attr2=%d spin=%.1f/%.1f dist=%.1f",
            best->o.x, best->o.y, best->o.z, best->attr2,
            m ? m->spinyaw : 0, m ? m->spinpitch : 0,
            camera1 ? camera1->o.dist(best->o) : 0);
}
COMMAND(hwrtvelaimanim, "");
