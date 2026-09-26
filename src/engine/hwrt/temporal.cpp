// temporal.cpp: lot 6 HUD-less native-res capture + trial jitter for a
// future DLAA evaluate. Does not load Streamline, NGX, or apply DLSS.
//
// Capture point (hwrttemporalcapture): after renderavatar(), before the
// velocity overlay, glare, fog overlay, postfx, and gl_drawhud. The FPS
// gun is in the colour and depth images. The reticle, text and 2D UI are
// not. Available whenever hwrtvelocity, hwrtveldebug, hwrtdlaajitter or
// hwrtdlaa is on. DLAA drives capture and jitter without extra diagnostic cvars.
//
// Colour domain: GL_RGB8 blit of the window. LDR UNORM after the current
// 3D lighting path (world, RT composite, models, particles, optional
// lookao / motionblur / looktaa, then the gun). Not linear HDR. A float
// velocity texture does not make this an HDR chain. hdrtonemap, if
// installed, runs later in renderpostfx and is outside this image.
//
// Depth: GL_DEPTH_COMPONENT24 copy of the window after the gun. World
// pixels use the main camproj (trial-jittered when the trial is on).
// Gun pixels use avatarfov + projmatrix.scalez(avatardepth), with the
// same pixel jitter. Mixed clip space. Do not reconstruct gun pixels
// with the world inverse.
//
// Trial jitter (hwrtdlaajitter, not VARP, default 0): Halton(2,3)
// 16-sample, centered, applied once per main gl_drawframe to projmatrix.c
// as a depth-independent NDC shift. The same NDC offset is replayed on
// the avatar projection so the gun moves by the same number of pixels.
// Secondary views (cubemap, preview, reflections) do not advance the
// sequence. looktaa jitter and looktaa accumulation are skipped while
// the trial is on so the offset is not applied twice. Turning the trial
// off restores the usual looktaa path.

#include "engine.h"

static const char *TEMPORAL_COLOR_DOMAIN =
    "LDR UNORM RGB8 window blit after 3D (world, RT composite, models, particles, optional lookao/motionblur/looktaa, FPS gun). Not linear HDR. Before glare, fog overlay, postfx, HUD, reticle, text.";

static const char *TEMPORAL_DEPTH_CONV =
    "GL_DEPTH_COMPONENT24 window depth AFTER the FPS gun. World fragments: main camproj (trial-jittered when hwrtdlaajitter is on). Gun fragments: avatarfov + avatardepth scalez, same pixel jitter. Mixed clip-space. Do not unproject gun pixels with the world inverse. Velocity was generated from pre-gun world depth plus an avatar-space HUD raster; B/A coverage marks which path wrote the pixel.";

static const char *TEMPORAL_MOTION_CONV =
    "RGBA16F RG = previous_unjittered_pixel - current_unjittered_pixel, engine pixels, +X right, +Y up (GL origin bottom-left). history_uv = uv + motion / resolution + (prev_jitter_px - curr_jitter_px) / resolution when comparing two jittered colour images. Geometry is rasterised with the jittered matrices. motionVectorsJittered is false. Packed MV.y is negated for D3D. NGX InMVScale is {1,1} for these pixel MVs.";

static const char *TEMPORAL_COVERAGE_NOTE =
    "Velocity B=1 is an internal covered mark (opaque world / rigid mapmodels / colour-visible mrfixit / attachments / FPS gun). A=1 is character/attachment/gun. These are NOT NGX transparency / bias / reactivity masks. Sky is B=0; pack reconstructs camera-only MVs for uncovered pixels. Water, particles, grass, world-alpha, ragdolls, monsters and unvalidated playermodels stay uncovered.";

static const char *TEMPORAL_VALID_UNTIL =
    "Valid after hwrttemporalcapture() of this main frame until the next main gl_drawframe overwrites the textures.";

extern int hwrtvelocity, hwrtveldebug, hwrtvelforce;

static hwrttemporalinput temporal{};
static hwrttemporalinput temporal_done{};
static bool tdoneok = false;
static bool tdoneapplied = false;
static bool tdonecaptured = false;
static GLuint tcolor = 0, tdepth = 0;
static int tw = 0, th = 0;
static GLenum tcolorfmt = 0;
static bool tfailed = false;
static uint tframeid = 0;
static bool treset = true;
static bool tcaptured = false;
static bool tapplied = false;
static int tlooktaawarn = 0;

static matrix4 tproj_unjit, tproj_used;
static matrix4 tcam_unjit, tcam_used, tinv_used;
static matrix4 tavatar_proj_unjit, tavatar_proj_used;
static matrix4 tavatar_cam_unjit, tavatar_cam_used;
static bool tavatarok = false;

static int tseq = 0;
static int tseqw = 0, tseqh = 0;
static float tjx_px = 0, tjy_px = 0, tprevjx_px = 0, tprevjy_px = 0;
static float tjx_ndc = 0, tjy_ndc = 0;

static void hwrtdlaajitterchanged();

VARF(hwrtdlaajitter, 0, 0, 1, hwrtdlaajitterchanged());
// Diagnostic only (default 1 = play). 0 reconstructs RT primary rays from the
// unjittered camproj so raster/NGX keep Halton while lighting does not.
// Not a play fix: NGX would still be told the colour is jittered.
VARF(hwrtdlaajitterrt, 0, 1, 1, { if(!initing) hwrttemporalreset("jitterrt"); });

static bool temporalwanted()
{
    return (hwrtvelocity || hwrtveldebug || hwrtdlaajitter || hwrtdlaaneedsdata()) && screenw > 0 && screenh > 0 && !tfailed
        && (!minimized || hwrtvelneeddraw());
}

static int temporalrw()
{
    int w = hwrtrenderw();
    return w > 0 ? w : screenw;
}

static int temporalrh()
{
    int h = hwrtrenderh();
    return h > 0 ? h : screenh;
}

static inline float temporalhalton(int i, int base)
{
    float f = 1, r = 0;
    while(i > 0)
    {
        f /= base;
        r += f * (i % base);
        i /= base;
    }
    return r;
}

static void temporalclearsamples()
{
    tseq = 0;
    tseqw = tseqh = 0;
    tjx_px = tjy_px = tprevjx_px = tprevjy_px = 0;
    tjx_ndc = tjy_ndc = 0;
}

void hwrttemporalbeginframe()
{
    tapplied = false;
    tcaptured = false;
    temporal.valid = false;
}

void hwrttemporalreset(const char *why)
{
    treset = true;
    tcaptured = false;
    temporal.valid = false;
    tavatarok = false;
    // If the current Halton sample was already written into this frame's
    // projection, keep it so the gun / metadata cannot disagree with the
    // world. Only drop previous-frame jitter. A reset before applymain
    // (allocate / resolution) still starts a fresh sample.
    bool keepcurrent = tapplied && why && (!strcmp(why, "resolution") || !strcmp(why, "allocate"));
    if(keepcurrent)
    {
        tprevjx_px = tprevjy_px = 0;
    }
    else temporalclearsamples();
    // Drop the previous-frame bias persist so a map change, Off→Quality, resize
    // or fallback cannot replay an old mask on a different picture. Does not
    // disable NGX and does not reset history by itself; tin->reset still does.
    if(!initing) hwrtdlaabiasreset();
    if(why && why[0] && !initing)
        conoutf("hwrt temporal: reset (%s)%s", why, keepcurrent ? " [kept applied sample]" : "");
}

static void hwrtdlaajitterchanged()
{
    if(initing) return;
    tlooktaawarn = 0;
    hwrttemporalreset(hwrtdlaajitter ? "jitter on" : "jitter off");
    hwrtresetvelocity();
    if(hwrtdlaajitter)
        conoutf("hwrt temporal: trial jitter on (Halton 2,3 x16, not saved). looktaa jitter/accumulation skipped while this is on. Vectors omit jitter. The picture can shimmer without a resolve — that is not DLAA quality.");
    else
        conoutf("hwrt temporal: trial jitter off, usual looktaa path restored");
}

bool hwrtdlaajitteractive()
{
    return (hwrtdlaajitter || hwrtdlaaneedsdata()) && screenw > 0 && screenh > 0;
}

static void temporalapplyoffset(matrix4 &proj)
{
    proj.c.x -= tjx_ndc;
    proj.c.y -= tjy_ndc;
}

void hwrttemporalapplymain()
{
    tcaptured = false;
    temporal.valid = false;
    tframeid++;
    tproj_unjit = projmatrix;
    tavatarok = false;

    if(!hwrtdlaajitteractive())
    {
        temporalclearsamples();
        tapplied = true;
        return;
    }
    if(screenw < 1 || screenh < 1)
    {
        tapplied = true;
        return;
    }
    int rw = temporalrw(), rh = temporalrh();
    if(rw < 1 || rh < 1)
    {
        tapplied = true;
        return;
    }
    int seqlen = hwrtdlaajitterseqlen();
    if(seqlen < 8) seqlen = 8;
    if(tseqw != rw || tseqh != rh)
    {
        tseq = 0;
        tseqw = rw;
        tseqh = rh;
        tprevjx_px = tprevjy_px = 0;
        treset = true;
    }
    tprevjx_px = tjx_px;
    tprevjy_px = tjy_px;
    tseq++;
    int i = int(tseq % seqlen) + 1;
    // Pixel units of the *render* target, engine +X right +Y up. Range (-0.5, 0.5)
    // after centering. NDC = px * 2 / render_size so a still world point moves +px
    // pixels of the internal colour buffer that NGX sees.
    tjx_px = temporalhalton(i, 2) - 0.5f;
    tjy_px = temporalhalton(i, 3) - 0.5f;
    tjx_ndc = tjx_px * 2.0f / rw;
    tjy_ndc = tjy_px * 2.0f / rh;
    temporalapplyoffset(projmatrix);
    tapplied = true;
    if(looktaa && !tlooktaawarn)
    {
        tlooktaawarn = 1;
        conoutf(CON_WARN, "hwrtdlaajitter: looktaa jitter and accumulation skipped this session so the Halton offset is applied once. Set looktaa 0 for the trial (the play default).");
    }
}

void hwrttemporalapplystored(matrix4 &proj)
{
    if(!hwrtdlaajitteractive() || screenw < 1 || screenh < 1) return;
    temporalapplyoffset(proj);
}

void hwrttemporalaftercam()
{
    tproj_used = projmatrix;
    tcam_used = camprojmatrix;
    tinv_used = invcamprojmatrix;
    if(hwrtdlaajitteractive())
        tcam_unjit.muld(tproj_unjit, cammatrix);
    else
        tcam_unjit = camprojmatrix;
}

void hwrttemporalnoteavatar()
{
    tavatar_proj_used = projmatrix;
    tavatar_cam_used = camprojmatrix;
    if(hwrtdlaajitteractive())
        tavatar_cam_unjit.muld(tavatar_proj_unjit, cammatrix);
    else
        tavatar_cam_unjit = camprojmatrix;
    tavatarok = true;
}

const matrix4 &hwrttemporalworldunjit() { return tcam_unjit; }
const matrix4 &hwrttemporalworldused() { return tcam_used; }

void hwrttemporalinvcamprojforrt(matrix4 &out)
{
    if(hwrtdlaajitterrt || !hwrtdlaajitteractive())
    {
        out = invcamprojmatrix;
        return;
    }
    if(!out.invert(tcam_unjit))
        out = invcamprojmatrix;
}
const matrix4 &hwrttemporalavatarunjit() { return tavatarok ? tavatar_cam_unjit : tcam_unjit; }
const matrix4 &hwrttemporalavatarused() { return tavatarok ? tavatar_cam_used : tcam_used; }

void hwrttemporaljitterpixels(float &currx, float &curry, float &prevx, float &prevy)
{
    currx = tjx_px;
    curry = tjy_px;
    prevx = tprevjx_px;
    prevy = tprevjy_px;
}

uint hwrttemporalframeid() { return tframeid; }
bool hwrttemporalresetflag() { return treset; }
void hwrttemporalsetreset(bool reset) { if(reset) treset = true; }

static void temporalfreetex()
{
    if(tcolor) { glDeleteTextures(1, &tcolor); tcolor = 0; }
    if(tdepth) { glDeleteTextures(1, &tdepth); tdepth = 0; }
    tw = th = 0;
    tcaptured = false;
    temporal.valid = false;
}

void cleanuptemporal()
{
    temporalfreetex();
    tfailed = false;
    tavatarok = false;
    temporal = hwrttemporalinput();
}

static bool temporalensure()
{
    if(tfailed) return false;
    if(!hasFBO)
    {
        conoutf(CON_WARN, "hwrt temporal: needs framebuffer objects, capture disabled");
        tfailed = true;
        return false;
    }
    bool wantdrhdr = hwrthdractive();
    GLenum wantfmt = wantdrhdr ? GL_RGBA16F : GL_RGB8;
    if(tcolor && tw == temporalrw() && th == temporalrh() && tcolorfmt == wantfmt) return true;
    int w = temporalrw(), h = temporalrh();
    if(w < 8 || h < 8) return false;
    if(tcolor && (tw != w || th != h || tcolorfmt != wantfmt))
    {
        glDeleteTextures(1, &tcolor);
        tcolor = 0;
        treset = true;
        conoutf("hwrt temporal: historique couleur invalide (%s)", tcolorfmt && tcolorfmt != wantfmt ? "format" : "taille");
    }
    if(!tcolor) glGenTextures(1, &tcolor);
    if(wantdrhdr)
    {
        glBindTexture(GL_TEXTURE_2D, tcolor);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, NULL);
    }
    else createtexture(tcolor, w, h, NULL, 3, 0, GL_RGB);
    tcolorfmt = wantfmt;
    if(!tdepth) glGenTextures(1, &tdepth);
    glBindTexture(GL_TEXTURE_2D, tdepth);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
    tw = w;
    th = h;
    treset = true;
    conoutf("hwrt temporal: %dx%d %s + D24 HUD-less capture (gun in, UI out)", w, h, wantdrhdr ? "RGBA16F" : "RGB8");
    return true;
}

void hwrttemporalcapture()
{
    temporal.valid = false;
    tcaptured = false;
    if(!temporalwanted()) return;
    if(!temporalensure()) return;

    GLint prevfb = 0, prevactive = 0, prevtex = 0;
    GLboolean hadscissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevfb);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevactive);
    glActiveTexture_(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevtex);
    if(hadscissor) glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer_(GL_FRAMEBUFFER, hwrtmainfbo());
    int cw = min(tw, temporalrw()), ch = min(th, temporalrh());
    glBindTexture(GL_TEXTURE_2D, tcolor);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, cw, ch);
    glBindTexture(GL_TEXTURE_2D, tdepth);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, cw, ch);
    if(hadscissor) glEnable(GL_SCISSOR_TEST);
    glBindTexture(GL_TEXTURE_2D, prevtex);
    glActiveTexture_(GLenum(prevactive));
    glBindFramebuffer_(GL_FRAMEBUFFER, prevfb);

    tcaptured = true;
    temporal.frameid = tframeid;
    temporal.width = tw;
    temporal.height = th;
    temporal.colortex = tcolor;
    temporal.depthtex = tdepth;
    temporal.velocitytex = hwrtvelocitytexid();
    temporal.jitter_px = tjx_px;
    temporal.jitter_py = tjy_px;
    temporal.prev_jitter_px = tprevjx_px;
    temporal.prev_jitter_py = tprevjy_px;
    temporal.jitter_ndc_x = tjx_ndc;
    temporal.jitter_ndc_y = tjy_ndc;
    temporal.view = cammatrix;
    temporal.proj_unjit = tproj_unjit;
    temporal.proj_used = tproj_used;
    temporal.camproj_unjit = tcam_unjit;
    temporal.camproj_used = tcam_used;
    temporal.invcamproj_used = tinv_used;
    temporal.avatar_proj_unjit = tavatarok ? tavatar_proj_unjit : tproj_unjit;
    temporal.avatar_proj_used = tavatarok ? tavatar_proj_used : tproj_used;
    temporal.avatar_camproj_unjit = hwrttemporalavatarunjit();
    temporal.avatar_camproj_used = hwrttemporalavatarused();
    temporal.world_ndc_dx = tproj_used.c.x - tproj_unjit.c.x;
    temporal.world_ndc_dy = tproj_used.c.y - tproj_unjit.c.y;
    if(tavatarok)
    {
        temporal.avatar_ndc_dx = tavatar_proj_used.c.x - tavatar_proj_unjit.c.x;
        temporal.avatar_ndc_dy = tavatar_proj_used.c.y - tavatar_proj_unjit.c.y;
        temporal.same_ndc_on_avatar =
            fabs(temporal.world_ndc_dx - temporal.avatar_ndc_dx) < 1e-5f &&
            fabs(temporal.world_ndc_dy - temporal.avatar_ndc_dy) < 1e-5f;
    }
    else
    {
        temporal.avatar_ndc_dx = temporal.avatar_ndc_dy = 0;
        temporal.same_ndc_on_avatar = false;
    }
    temporal.avatar_noted = tavatarok;
    temporal.nearz = nearplane;
    temporal.farz = float(farplane);
    temporal.world_fovy = fovy;
    temporal.avatar_fov = curavatarfov;
    temporal.avatar_depth = avatardepth;
    temporal.color_internalformat = int(tcolorfmt);
    temporal.color_domain = hwrthdractive()
        ? "HDR lot2: RGBA16F copy of the linear scene FBO (gun in, HUD out), before exposure and tone mapping. Not an LDR history."
        : TEMPORAL_COLOR_DOMAIN;
    temporal.depth_convention = TEMPORAL_DEPTH_CONV;
    temporal.motion_convention = TEMPORAL_MOTION_CONV;
    temporal.coverage_note = TEMPORAL_COVERAGE_NOTE;
    temporal.valid_until = TEMPORAL_VALID_UNTIL;
    bool sizedok = tcolor && tdepth && tw == temporalrw() && th == temporalrh() && cw == tw && ch == th;
    int vw = 0, vh = 0;
    hwrtvelocitysize(vw, vh);
    bool velok = true;
    if(hwrtvelocity || hwrtdlaaneedsdata())
        velok = hwrtvelocityready() && hwrtvelocitytexid() != 0 && vw == tw && vh == th;
    temporal.valid = sizedok && velok;
    temporal.reset = treset || !temporal.valid || hwrtvelocitycutframe();
    if(temporal.valid) treset = false;
    temporal_done = temporal;
    tdoneok = true;
    tdoneapplied = tapplied;
    tdonecaptured = tcaptured;
}

const hwrttemporalinput *hwrttemporalcurrent()
{
    return &temporal;
}

void hwrttemporalanalytic(float &dx_px, float &dy_px)
{
    dx_px = dy_px = 0;
    if(screenw < 1 || screenh < 1 || !camera1) return;
    vec world = vec(camdir).mul(64).add(camera1->o);
    vec4 usedh, unjith;
    tcam_used.transform(world, usedh);
    tcam_unjit.transform(world, unjith);
    if(usedh.w == 0 || unjith.w == 0) return;
    float ux = usedh.x / usedh.w, uy = usedh.y / usedh.w;
    float nx = unjith.x / unjith.w, ny = unjith.y / unjith.w;
    dx_px = (ux - nx) * 0.5f * float(temporal.width > 0 ? temporal.width : temporalrw());
    dy_px = (uy - ny) * 0.5f * float(temporal.height > 0 ? temporal.height : temporalrh());
}

void hwrttemporalfprint(FILE *f)
{
    if(!f) return;
    // Print the last completed capture. applymain() clears the in-progress
    // tavatarok flag at the start of the next frame; a dump must not read that
    // half-built state, and must not reset history in order to observe it.
    const hwrttemporalinput *t = tdoneok ? &temporal_done : &temporal;
    fprintf(f, "temporal_source %s  dump_does_not_modify_temporal_state yes\n",
            tdoneok ? "last_completed_capture" : "no_completed_capture");
    fprintf(f, "temporal_valid %s frameid %u reset %s captured %s jitter_mode %d seq %d size %dx%d\n",
            t->valid ? "yes" : "no", t->frameid, t->reset ? "yes" : "no",
            (tdoneok ? tdonecaptured : tcaptured) ? "yes" : "no",
            int(hwrtdlaajitter), tseq, t->width, t->height);
    fprintf(f, "temporal_color fmt 0x%x tex %u  %s\n", int(t->color_internalformat), (unsigned)t->colortex, t->color_domain ? t->color_domain : TEMPORAL_COLOR_DOMAIN);
    fprintf(f, "temporal_depth GL_DEPTH_COMPONENT24 tex %u  mixed world/avatar clip after the FPS gun\n", (unsigned)t->depthtex);
    fprintf(f, "temporal_velocity RGBA16F tex %u  (internal B/A are not DLSS masks)\n", (unsigned)t->velocitytex);
    fprintf(f, "jitter_px_curr %.6f %.6f  prev %.6f %.6f  units=render_pixels  +X right +Y up (GL)  seqlen %d  render %dx%d output %dx%d\n",
            t->jitter_px, t->jitter_py, t->prev_jitter_px, t->prev_jitter_py, hwrtdlaajitterseqlen(), t->width, t->height, screenw, screenh);
    fprintf(f, "jitter_ndc_curr %.8f %.8f  applied as projmatrix.c.xy -= ndc (NDC x increases by ndc.x)\n",
            t->jitter_ndc_x, t->jitter_ndc_y);
    fprintf(f, "looktaa %d  (skipped while hwrtdlaajitter is on so the offset is not applied twice)\n", int(looktaa));
    fprintf(f, "avatar_fov %.4f avatardepth %.4f world_fovy %.4f near %.4f far %d  avatar_noted %s\n",
            t->avatar_fov, t->avatar_depth, t->world_fovy, t->nearz, int(t->farz + 0.5f),
            t->avatar_noted ? "yes" : "no");
    float ax = 0, ay = 0;
    int aw = t->width > 0 ? t->width : temporalrw();
    int ah = t->height > 0 ? t->height : temporalrh();
    if(aw > 0 && ah > 0)
    {
        vec viewpt(0, 0, -64);
        vec4 usedh, unjith;
        t->proj_used.transform(viewpt, usedh);
        t->proj_unjit.transform(viewpt, unjith);
        if(usedh.w != 0 && unjith.w != 0)
        {
            float ux = usedh.x / usedh.w, uy = usedh.y / usedh.w;
            float nx = unjith.x / unjith.w, ny = unjith.y / unjith.w;
            ax = (ux - nx) * 0.5f * float(aw);
            ay = (uy - ny) * 0.5f * float(ah);
        }
    }
    fprintf(f, "analytical_subpixel_px %.6f %.6f  programmed %.6f %.6f  err %.6f %.6f  (from captured proj, view-space point; dump does not call apply/reset)\n",
            ax, ay, t->jitter_px, t->jitter_py, ax - t->jitter_px, ay - t->jitter_py);
    if(!t->avatar_noted)
        fprintf(f, "jitter_applied_this_frame %s  same_ndc_on_avatar n/a  (no FPS gun projection on this captured frame: dead/thirdperson/hudgun off, or renderavatar skipped)\n",
                (tdoneok ? tdoneapplied : tapplied) ? "yes" : "no");
    else
        fprintf(f, "jitter_applied_this_frame %s  same_ndc_on_avatar %s  world_ndc_delta %.8f %.8f  avatar_ndc_delta %.8f %.8f  (compared captured proj.c, not the in-progress applymain flag)\n",
                (tdoneok ? tdoneapplied : tapplied) ? "yes" : "no",
                t->same_ndc_on_avatar ? "yes" : "no",
                t->world_ndc_dx, t->world_ndc_dy, t->avatar_ndc_dx, t->avatar_ndc_dy);
    fprintf(f, "temporal_valid_rule color+depth present at render size; velocity present at that size when hwrtvelocity or NGX needs data. reset means previous history is unusable. reset=true does not make a missing or stale resource valid.\n");
    fprintf(f, "ngx_notes matrices UNJITTERED; jitterOffset is pixel space with Y flipped for D3D; NGX MV_Scale {1,1} for these pixel MVs; motionVectorsJittered false; LDR RGB8; DepthInverted false\n");
    fprintf(f, "valid_until %s\n", TEMPORAL_VALID_UNTIL);
}

static void hwrttemporalstats()
{
    const hwrttemporalinput *t = hwrttemporalcurrent();
    float ax, ay;
    hwrttemporalanalytic(ax, ay);
    conoutf("hwrt temporal: valid %s frame %u reset %s %dx%d jitter %d seq %d px %.4f %.4f prev %.4f %.4f analytical %.4f %.4f color %u depth %u vel %u looktaa %d (skipped if jitter on)",
            t->valid ? "yes" : "no", t->frameid, t->reset ? "yes" : "no",
            t->width, t->height, int(hwrtdlaajitter), tseq,
            tjx_px, tjy_px, tprevjx_px, tprevjy_px, ax, ay,
            (unsigned)t->colortex, (unsigned)t->depthtex, (unsigned)t->velocitytex,
            int(looktaa));
}
COMMAND(hwrttemporalstats, "");

void hwrttemporalbeginavatarproj()
{
    tavatar_proj_unjit = projmatrix;
}
