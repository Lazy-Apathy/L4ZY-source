// hwrt.cpp: entry points and CubeScript variables for the ray tracing layer.
//
// Everything the rest of the engine calls lives here: bring-up after gl_init,
// one hook at the end of world rendering, and teardown. The layer is a mode, not
// a replacement, so any failure just switches it off and leaves the GL client
// running exactly as before.

#include "engine.h"
#include "hwrt/hwrt.h"

// Read-only state, so config files and menus can ask before offering the option.
VAR(hwrtavailable, 1, 0, 0);
VAR(hwrtrayquery, 1, 0, 0);
VAR(hwrtngx, 1, 0, 0);
VAR(hwrtstalls, 1, 0, 0);
// 1 draws a HUD overlay of the last completed GL / Vulkan / CPU stage times.
VARP(hwrttimes, 0, 0, 1);

hwrttimings hwrttime;

void hwrttally()
{
    hwrttime.gltotal = hwrttime.glmaskworld + hwrttime.glmaskscene + hwrttime.gldepth + hwrttime.glcomposite;
    hwrttime.cputotal = hwrttime.cpuprep + hwrttime.cpusync;
    hwrttime.gptotal = hwrttime.gltotal + hwrttime.vktotal;
}

double hwrtnow()
{
    Uint64 freq = SDL_GetPerformanceFrequency();
    if(!freq) return 0;
    return double(SDL_GetPerformanceCounter()) / double(freq);
}

static bool hwrtinited = false;
static bool probequeued = false;

bool hwrtdepthlive = false;

static void disablehwrt(const char *why);

extern bool hwrtlightrecorded;
extern int hwrt;
static int hwrtlitmillis = -100000;
// 1 = the RT lighting pass was composed within the last second.
ICOMMAND(hwrtlighteffective, "", (), intret(hwrt && hwrtavailable && !hwrtfailed && totalmillis - hwrtlitmillis < 1000 ? 1 : 0));
static bool hwrtcanrun() { return hwrtavailable && hwrtrayquery && !hwrtfailed; }
ICOMMAND(hwrtisavailable, "", (), intret(hwrtcanrun() ? 1 : 0));

static int hwrtwhy = HWRT_WHY_NONE;
static string hwrtsimulate = "";

void hwrtunavailable(int why)
{
    if(hwrtwhy == HWRT_WHY_NONE) hwrtwhy = why;
}

bool hwrtsimulating(const char *mode)
{
    return hwrtsimulate[0] && !strcmp(hwrtsimulate, mode);
}

// Short English line for the menu and the assistant, "" when RT can run.
static const char *hwrtreason()
{
    if(hwrtcanrun()) return "";
    int why = hwrtwhy;
    // interop without ray query is a device that came up fine but has no RT cores
    if(why == HWRT_WHY_NONE && hwrtavailable && !hwrtrayquery && !hwrtfailed) why = HWRT_WHY_NORT;
    switch(why)
    {
        case HWRT_WHY_NOVULKAN: return "Ray tracing is not available: no Vulkan driver was found for this graphics card.";
        case HWRT_WHY_DRIVER: return "Ray tracing is not available: the graphics driver is too old for ray tracing.";
        case HWRT_WHY_NOMATCH: return "Ray tracing is not available: Vulkan does not see the graphics card the game runs on.";
        case HWRT_WHY_NORT:
        {
            static string msg;
            if(hwrtdev.name[0]) formatstring(msg, "Ray tracing is not available: this graphics card (%s) has no hardware ray tracing.", hwrtdev.name);
            else copystring(msg, "Ray tracing is not available: this graphics card has no hardware ray tracing.");
            return msg;
        }
        default: return "Ray tracing is not available: it failed to start (see the console).";
    }
}
ICOMMAND(hwrtraison, "", (), result(hwrtreason()));
// read-only: the SAUER_HWRT_SIMULATE mode in force, "" in normal use
ICOMMAND(hwrtsimulated, "", (), result(hwrtsimulate));

VARFP(hwrt, 0, 0, 1,
{
    // hwrtinit runs during gl_init, before any config is executed, so a saved
    // `hwrt 1` can be corrected here rather than left lying to menus and scripts.
    // The reason was already logged once at init, hence the silence while loading.
    // Interop alone (no ray query) is not enough: nothing would be traced.
    if(hwrt && !hwrtcanrun())
    {
        if(!initing) conoutf(CON_WARN, "hwrt: staying on OpenGL. %s", hwrtreason());
        hwrt = 0;
    }
    if(initing) return;
    if(!hwrt) hwrtdestroyshared();
    else if(hwrtavailable && !hwrthasworld()) hwrtrebuildworld();
});

// 0 = run the interop but composite nothing, 1 = overlay the phase 1 probe,
// 2 = replace the frame with it, 3 = replace with a flat Vulkan clear,
// 4 = overlay world-TLAS closest-hit barycentrics (miss is transparent),
// 5 = RTAO overlay (alias of rtaodebug 1; does not steal 1-4),
// 6 = hybrid hit shading: closest-hit samples diffuse * baked lightmap,
// 7 = point lights + one shadow ray + sun + sky (alias of hwrtlight 1).
// 2/3/4 win over 5/6/7; 7 wins over 5 and 6. Default is the lighting view
// so `hwrt 1` is RT lighting for the player; 1–6 stay available.
VARP(hwrtdebug, 0, 7, 7);
VARP(hwrtdlights, 1, 1, 4);
// Four cosine-weighted sky rays (the cap). One ray is a coin flip per
// pixel; spatial mix + TAA did not get the testers to zero dots, and they
// were already fine with four. Keep hwrtskyfilter on so neighbours still
// share the leftover. Do not drop this back to 1 for fps.
VARP(hwrtskyrays, 1, 4, 4);
// Average skyvis across the 16x16 lighting workgroup before the bake-style
// max(). One 5x5 on the same plane (four rays carry the grain; a wider
// kernel smeared mid/far lighting). 0 is the raw term. Do not drop
// hwrtskyrays back to 1.
VARP(hwrtskyfilter, 0, 1, 1);
// Frames of skyvis a converged pixel keeps. Four rays are 4 coin flips, so the
// raw fraction has an irreducible 0.25 of noise; the 5x5 brings about 12
// effective taps and lands near 0.036, which hwrtworldgain then doubles into
// roughly five 8-bit levels in shade -- and it is redrawn every frame, so it
// reads as grain crawling rather than as a fixed pattern. N frames divide that
// by sqrt(N) *and* hold it still. This is not the composited-frame TAA: skyvis
// is geometry, it needs no colour clamp, and a colour clamp is exactly what
// caps looktaa at the noise of its own neighbourhood. 0 or 1 is off.
VARP(hwrtskytemporal, 0, 32, 128);
// What the local body occludes, ordered by how badly each light type distorts
// the body's own shadow. A point lamp magnifies the silhouette by
// (lamp -> lit surface) / (lamp -> body), so a body standing next to a lamp
// projects its *volume*, not its outline: shoulders, chest and feet are at
// different distances and grow by different factors, and what lands on the
// floor is the body squashed sideways. That is the "3D model forced flat"
// a tester reported, and it is why radius 0 shows it too -- unlimited reach does
// not make a light directional, it is still a point. The sun has no position
// at all: parallel rays, magnification exactly 1, the shadow is the body's
// cross-section on any surface. It is the only source that cannot produce the
// artefact, hence the default.
// 0 = nothing, 1 = the sun alone (default), 2 = + radius-0 / glow omnis,
// 3 = + the nearest-N finite lamps (also cut where the winning lamp changes
// between two pixels), 4 = + sky (a body that close eats the hemisphere).
VARP(hwrtshadowself, 0, 1, 4);
// Projected omni only: the pad / portal still glows. Jumppads default off.
// Teleporters: every teleport ent, not only the default ring. Colour follows
// a nearby glow pad or magic-circle tint. Toggling is live.
VARP(hwrtteleportlight, 0, 1, 1);
VARP(hwrtjumppadlight, 0, 0, 1);
// Rifle / rocket smoke sprites cast partial shadows as coarse spheres. Off
// keeps the sprites and drops only their shadow. Off until the puff reads as
// smoke: an 8x6 sphere at 45% opacity draws a faceted translucent solid that
// follows the shooter, which is what a tester saw and took for a second shadow.
VARP(hwrtsmokeshadow, 0, 0, 1);
// How many shadowed glow lamps (teleports + lava) to evaluate. Nearest
// around the camera, plus those on screen. 8 is playable on triforts (54 pads).
VARP(hwrtglowdlights, 1, 2, 16);

// GL's world pass multiplies the bake by colorparams = 2 * vslot.colorscale
// (renderva.cpp): Sauerbraten stores lightmaps at half scale and the world
// shader doubles them. Traced light is in those same units, so the lighting
// view needs the same factor or every wall is half of what the rasteriser
// draws. It scales the traced light only, never the ambient constant, so
// raising it opens the gap between lit and shadowed instead of flattening it.
FVARP(hwrtworldgain, 0, 2, 8);

// Megabytes of GPU memory allowed for the model-skin array at 1024 before
// falling back to 512. This is a ceiling, not a reservation: 45 layers still
// cost ~180 MB. 192 layers at 1024 need 768 MB, so 1024 never trips the
// fallback. Changing it in-game rebuilds the array on the next skin sync.
VARFP(hwrtskinbudget, 64, 1024, 4096,
{
    if(initing) return;
    hwrtdirtyskins();
});

// How many TLAS instances the scenery may fill. Only mapmodels answer to it,
// nearest first, so lowering it never costs you a player or a corpse.
//
// A poor speed dial, and worth knowing why. Dispatch on cmvalley at 2560x1440,
// one recorded viewpoint in its forest: 5.90 ms for the nearest 256, 6.94 for
// 1024, 7.46 for all 2614. Trimming 90% of the scenery buys 1.5 ms, because
// what a ray costs is the alpha-tested canopy it enters immediately — every
// leaf it meets returns to the shader to read its transparency — and sorting
// by distance keeps exactly those. The far ones it drops are nearly free.
//
// The same viewpoint measured 1.40 ms before this, on the old hard 1024 taken
// in map order. That was not the budget being kind: the order happened to miss
// most of the near canopy, so the rays flew through the foreground into open
// air. It is the same 6 ms either way once the trees in front of you are really
// in the TLAS. Keep it at the ceiling unless a map has to be made playable.
VARP(hwrtmaxinsts, 128, 4096, HWRT_MAX_INSTANCES);

FVARP(rtaoradius, 1, 32, 1024);
FVARP(rtaoscale, 0, 0, 1);
FVARP(rtaobias, 0.01f, 0.25f, 8);

VARF(rtaodebug, 0, 0, 1,
{
    if(initing) return;
    if(rtaodebug && !hwrtrayquery)
    {
        static bool loggednorqao = false;
        if(!loggednorqao)
        {
            conoutf(CON_WARN, "rtaodebug: no ray query on this GPU, leaving phases 1-2 as they are");
            loggednorqao = true;
        }
        rtaodebug = 0;
    }
});

VARF(hwrtshade, 0, 0, 1,
{
    if(initing) return;
    if(hwrtshade && !hwrtrayquery)
    {
        static bool loggednorqshade = false;
        if(!loggednorqshade)
        {
            conoutf(CON_WARN, "hwrtshade: no ray query on this GPU, leaving phases 1-3 as they are");
            loggednorqshade = true;
        }
        hwrtshade = 0;
    }
});

VARF(hwrtlight, 0, 0, 1,
{
    if(initing) return;
    if(hwrtlight && !hwrtrayquery)
    {
        static bool loggednorqlight = false;
        if(!loggednorqlight)
        {
            conoutf(CON_WARN, "hwrtlight: no ray query on this GPU, leaving phases 1-4 as they are");
            loggednorqlight = true;
        }
        hwrtlight = 0;
    }
});

VARFP(dlaa, 0, 0, 1,
{
    if(initing || !dlaa) return;
    conoutf(CON_WARN, "dlaa: unused stub, use Anti-Aliasing in Graphics (hwrtngxmode)");
    dlaa = 0;
});

VARFP(dlss, 0, 0, 1,
{
    if(initing || !dlss) return;
    conoutf(CON_WARN, "dlss: unused stub, use Anti-Aliasing in Graphics (hwrtngxmode)");
    dlss = 0;
});

VARP(dlssquality, 0, 1, 3);

// 0 restores the pre-mask behaviour: models punch a transparent hole.
// Mode 6 still hands every model pixel back to GL. Mode 7 keeps RT-lit
// model pixels and only drops world hits that punched through an untraced
// mesh (grass, world alpha) — the same hole the hudgun used to have.
VARP(hwrtmask, 0, 1, 1);

// Mode 7 compares every world hit against the window depth GL has just
// finished rasterising. Foliage and CTF flags are in the TLAS with an
// alpha test; this still catches grass, water and world alpha cubes the
// BLAS never sees. 0 is the old see-through behaviour.
VARP(hwrtdepthmask, 0, 1, 1);

// Diagnostic views and test drives are for offline work only. Online (a
// remote server, or other players on our listen server; demo playback is
// left alone) they are put back to their play value before every frame, so
// setting one by hand, by script or from a saved config is refused, and one
// still on when we connect is switched off. `hwrtdebug 4` (the "LSD mode")
// paints every triangle, players included, in a flat barycentric colour.
// What stays allowed is the normal player choice: hwrt 0/1 with hwrtdebug
// 7 or 0 (hwrtlight is the alias of 7), AA, HDR, vanilla fullbrightmodels.
static const struct { const char *name; float play, alsook; } hwrtonlineonly[] =
{
    { "hwrtdebug", 7, 0 },          // 1-6: probe, flat clear, barycentric, RTAO, hybrid
    { "rtaodebug", 0, 0 },          // alias of hwrtdebug 5: grey AO overlay
    { "hwrtshade", 0, 0 },          // alias of hwrtdebug 6
    { "hwrtveldebug", 0, 0 },       // motion vectors light up whoever moves
    { "hwrtnrddbg", 0, 0 },         // NRD false-colour views (5 = motion vectors)
    { "hwrtdiffvis", 0, 0 },        // albedo only (no shadows) / lighting only (no textures)
    { "hwrtskyvisdbg", 0, 0 },      // sky visibility heatmaps
    { "hdrlightdbg", 0, 0 },        // classic world: light alone / texture alone
    { "hwrtdepthmask", 1, 1 },      // 0 draws traced hits over grass, water, world alpha
    // test drives: they move or pin the camera, the player, other players or a mapmodel
    { "hwrtvelwalk", 0, 0 },
    { "hwrtvelholdplayer", 0, 0 },
    { "hwrtvelholdcam", 0, 0 },
    { "hwrtvelholdothers", 0, 0 },
    { "hwrtvelfreezepose", 0, 0 },
    { "hwrtveldrive", -1, -1 },
    { "hwrtvelspin", 0, 0 },
    { "hwrtvelpitchspin", 0, 0 },
    { "hwrtvelslide", 0, 0 },
    { "hwrtvelpush", 0, 0 },
};

void hwrtonlineguard()
{
    if(!multiplayer(false) || game::isdemoplayback()) return;
    const int n = int(sizeof(hwrtonlineonly)/sizeof(hwrtonlineonly[0]));
    static ident *ids[sizeof(hwrtonlineonly)/sizeof(hwrtonlineonly[0])];
    static bool looked = false;
    if(!looked)
    {
        loopi(n) ids[i] = getident(hwrtonlineonly[i].name);
        looked = true;
    }
    // Say it for every new refusal. Only something that sets a view again
    // on every frame (a queued lab capture) is held to one line a second.
    static int lastmsg = -1000000;
    static bool lastframe = false;
    bool say = !lastframe || totalmillis - lastmsg >= 1000, reset = false;
    loopi(n)
    {
        ident *id = ids[i];
        if(!id) continue;
        float cur;
        if(id->type == ID_VAR) cur = float(*id->storage.i);
        else if(id->type == ID_FVAR) cur = *id->storage.f;
        else continue;
        if(cur == hwrtonlineonly[i].play || cur == hwrtonlineonly[i].alsook) continue;
        if(id->type == ID_VAR) setvar(id->name, int(hwrtonlineonly[i].play));
        else setfvar(id->name, hwrtonlineonly[i].play);
        if(say) logoutf("online: %s %g refused, back to %g", id->name, cur, hwrtonlineonly[i].play);
        reset = true;
    }
    if(reset && say)
    {
        conoutf(CON_WARN, "debug view disabled online");
        lastmsg = totalmillis;
    }
    lastframe = reset;
}

// 0 (default) keeps the map skybox. 1 is the old lighting-view stand-in:
// skip the cubemap and draw a disk on sunlightdir so a painted sun cannot
// disagree with the shadow. The bake skip does not depend on this.
VARP(hwrtsundisk, 0, 0, 1);

static bool hwrtlightingview()
{
    if(!hwrt || !hwrtavailable || hwrtfailed) return false;
    if(!hwrtrayquery) return false;
    // Same winner rules as hwrtrender: 2/3/4 steal the lighting view.
    if(hwrtdebug == HWRT_TRACE_FULL || hwrtdebug == HWRT_TRACE_CLEAR || hwrtdebug == HWRT_TRACE_SILHOUETTE)
        return false;
    return hwrtlight || hwrtdebug == HWRT_TRACE_LIGHT;
}

bool hwrtalignsundisk()
{
    return hwrtsundisk && hwrtlightingview();
}

// Modes 6 and 7 cover the frame with an opaque result, so they are the only
// ones that need the model mask. The diagnostics stay as they are.
bool hwrtmasksmodels()
{
    if(!hwrt || !hwrtavailable || hwrtfailed) return false;
    if(!hwrtrayquery || !hwrtmask) return false;
    if(hwrtdebug == HWRT_TRACE_FULL || hwrtdebug == HWRT_TRACE_CLEAR || hwrtdebug == HWRT_TRACE_SILHOUETTE)
        return false;
    // Mode 7 still snapshots. The mask shader keeps a shaded model hit and
    // only discards a world hit that landed behind a GL model the TLAS never
    // saw (grass, world alpha). Foliage and CTF flags are in the TLAS.
    return hwrtlight || hwrtdebug == HWRT_TRACE_LIGHT || hwrtshade || hwrtdebug == HWRT_TRACE_SHADE;
}

bool hwrtnobake()
{
    if(!hwrtlightingview()) return false;
    if(lightmaptexs.length() < LMID_RESERVED)
    {
        static bool logged = false;
        if(!logged)
        {
            conoutf(CON_WARN, "hwrt: no reserved lightmap to skip the bake, leaving vanilla lighting");
            logged = true;
        }
        return false;
    }
    return true;
}

void hwrtinit()
{
    if(hwrtinited) return;
    hwrtinited = true;
    hwrtfailed = false;

    // A stuck semaphore takes the process down without unwinding, and the engine
    // log is block buffered, so the last few lines before a hang are exactly the
    // ones that would be lost. The log is a few dozen lines a session.
    if(getlogfile()) setvbuf(getlogfile(), NULL, _IONBF, 0);

    hwrtwhy = HWRT_WHY_NONE;
    const char *sim = getenv("SAUER_HWRT_SIMULATE");
    copystring(hwrtsimulate, sim ? sim : "");
    if(hwrtsimulate[0])
    {
        static const char * const modes[] = { "novulkan", "driver", "nort", "failed" };
        bool known = false;
        loopi(int(sizeof(modes)/sizeof(modes[0]))) if(!strcmp(hwrtsimulate, modes[i])) known = true;
        if(known) conoutf(CON_WARN, "hwrt: SAUER_HWRT_SIMULATE=%s, simulating an incompatible GPU (diagnostic)", hwrtsimulate);
        else
        {
            conoutf(CON_WARN, "hwrt: SAUER_HWRT_SIMULATE=%s is not novulkan, driver, nort or failed; ignored", hwrtsimulate);
            hwrtsimulate[0] = 0;
        }
    }

    if(hwrtsimulating("driver"))
    {
        conoutf(CON_INIT, "hwrt: OpenGL driver has no GL_EXT_memory_object (simulated), staying on OpenGL");
        hwrtunavailable(HWRT_WHY_DRIVER);
        return;
    }
    if(!hwrtloadglinterop()) { hwrtunavailable(HWRT_WHY_DRIVER); return; }

    uint8_t gluuid[VK_UUID_SIZE];
    if(!hwrtglgetdeviceuuid(gluuid))
    {
        conoutf(CON_INIT, "hwrt: OpenGL will not report a device UUID, staying on OpenGL");
        hwrtunavailable(HWRT_WHY_DRIVER);
        return;
    }
    if(!hwrtinitdevice(gluuid) || !hwrtinittrace())
    {
        hwrtunavailable(HWRT_WHY_FAILED);
        hwrtdestroytrace();
        hwrtdestroydevice();
        return;
    }
    if(hwrtsimulating("failed"))
    {
        hwrtfail("simulated start-up failure (SAUER_HWRT_SIMULATE=failed)");
        hwrtunavailable(HWRT_WHY_FAILED);
        hwrtdestroytrace();
        hwrtdestroydevice();
        return;
    }

    hwrtavailable = 1;
    hwrtrayquery = hwrtdev.rayquery ? 1 : 0;
    hwrtnrdpreload();
}

void hwrtcleanup()
{
    hwrtcleanupmask();
    hwrtcleanupgltimes();
    hwrtdestroyshared();
    hwrtdestroyworld();
    hwrtdestroytrace();
    hwrtdestroylights();
    hwrtdestroydevice();
    hwrtavailable = hwrtrayquery = 0;
    hwrtinited = false;
    memset(&hwrttime, 0, sizeof(hwrttime));
}

static void disablehwrt(const char *why)
{
    conoutf(CON_WARN, "hwrt: %s, falling back to OpenGL", why);
    hwrt = 0;
    hwrtavailable = 0;
    hwrtdestroyshared();
}

void hwrtrender()
{
    if(!hwrt || !hwrtavailable || hwrtfailed) return;

    // A framebuffer with no area is a transient state, not an interop failure.
    int rw = hwrtfbw(), rh = hwrtfbh();
    if(rw <= 0 || rh <= 0) return;

    // Covers window resize, fullscreen changes, Quality internal size and resetgl.
    if(hwrtio.w != rw || hwrtio.h != rh)
    {
        if(!hwrtcreateshared(rw, rh)) { disablehwrt("could not share an image with Vulkan"); return; }
    }

    // Hand the frame over, trace, take it back. The two semaphores are the only
    // synchronisation: no glFinish, no vkQueueWaitIdle, no readback.
    int mode = max(int(hwrtdebug), int(HWRT_TRACE_OVERLAY));
    bool diagnostic = hwrtdebug == HWRT_TRACE_FULL || hwrtdebug == HWRT_TRACE_CLEAR || hwrtdebug == HWRT_TRACE_SILHOUETTE;
    bool wantao = !diagnostic && (rtaodebug || hwrtdebug == HWRT_TRACE_RTAO);
    bool wantshade = !diagnostic && (hwrtshade || hwrtdebug == HWRT_TRACE_SHADE);
    bool wantlight = !diagnostic && (hwrtlight || hwrtdebug == HWRT_TRACE_LIGHT);
    // Mode 7 wins over 5 and 6 so AO/bake cannot steal the new view. 2/3/4 still win.
    if(wantlight)
    {
        wantao = false;
        wantshade = false;
        if(!hwrtrayquery)
        {
            static bool loggednorqlight = false;
            if(!loggednorqlight)
            {
                conoutf(CON_WARN, "hwrt: hwrtdebug 7 needs ray query, leaving phases 1-2 as they are");
                loggednorqlight = true;
            }
            wantlight = false;
            if(hwrtdebug == HWRT_TRACE_LIGHT) mode = HWRT_TRACE_OVERLAY;
        }
        else mode = HWRT_TRACE_LIGHT;
    }
    // Mode 6 wins over RTAO so AO cannot steal the bake view.
    if(wantshade) wantao = false;
    if(wantshade)
    {
        if(!hwrtrayquery)
        {
            static bool loggednorqshade = false;
            if(!loggednorqshade)
            {
                conoutf(CON_WARN, "hwrt: hwrtdebug 6 needs ray query, leaving the phase 1 probe");
                loggednorqshade = true;
            }
            wantshade = false;
            if(hwrtdebug == HWRT_TRACE_SHADE) mode = HWRT_TRACE_OVERLAY;
        }
        else mode = HWRT_TRACE_SHADE;
    }
    if(wantao)
    {
        if(!hwrtrayquery)
        {
            static bool loggednorqao = false;
            if(!loggednorqao)
            {
                conoutf(CON_WARN, "hwrt: rtaodebug needs ray query, leaving phases 1-2 as they are");
                loggednorqao = true;
            }
            wantao = false;
            if(hwrtdebug == HWRT_TRACE_RTAO) mode = HWRT_TRACE_OVERLAY;
        }
        else if(!hwrtio.depthok())
        {
            static bool loggednodepth = false;
            if(!loggednodepth)
            {
                conoutf(CON_WARN, "hwrt: shared depth missing, RTAO disabled");
                loggednodepth = true;
            }
            wantao = false;
            if(hwrtdebug == HWRT_TRACE_RTAO) mode = HWRT_TRACE_OVERLAY;
        }
        else mode = HWRT_TRACE_RTAO;
    }
    if(mode == HWRT_TRACE_SILHOUETTE && !hwrtrayquery)
    {
        static bool loggednorq = false;
        if(!loggednorq)
        {
            conoutf(CON_WARN, "hwrt: hwrtdebug 4 needs ray query, leaving the phase 1 probe");
            loggednorq = true;
        }
        mode = HWRT_TRACE_OVERLAY;
    }
    // Both consumers of the shared depth want the same copy, and only one mode
    // runs per frame. RTAO reconstructs world positions from it; mode 7 uses it
    // to tell a visible hit from one the ray only reached by passing through an
    // alpha-tested mesh the TLAS has no triangles for. hwrtrender() is called
    // straight after hwrtsnapscenedepth(), so the window depth here is the
    // world plus both model passes and nothing GL draws later.
    hwrtdepthlive = false;
    if(wantao && mode == HWRT_TRACE_RTAO)
    {
        if(hwrtcopydepth()) hwrtdepthlive = true;
        else
        {
            wantao = false;
            mode = HWRT_TRACE_OVERLAY;
        }
    }
    else if(mode == HWRT_TRACE_LIGHT && hwrtdepthmask && hwrtio.depthok())
        hwrtdepthlive = hwrtcopydepth();
    {
        double t0 = hwrtnow();
        hwrtsyncdynents();
        hwrttime.cpusync = float((hwrtnow() - t0) * 1000.0);
        hwrttally();
    }
    {
        static double lastframe = 0;
        double now = hwrtnow();
        if(lastframe > 0) hwrttime.frame = float((now - lastframe) * 1000.0);
        lastframe = now;
    }

    // Nothing between the two calls issues GL work, so this GL_TIME_ELAPSED span
    // is the GL queue standing still for the whole Vulkan round trip.
    hwrtbegingltime(HWRT_GLT_HANDOFF);
    hwrtsignalgl();
    hwrtlightrecorded = false;
    double tdisp = hwrtnow();
    bool dispatched = hwrtdispatch(mode);
    hwrttime.cpudispatch = float((hwrtnow() - tdisp) * 1000.0);
    if(!dispatched)
    {
        // The Vulkan side never ran, so nothing will ever signal the semaphore GL
        // is about to wait on. Skip the wait rather than deadlock the frame.
        hwrtendgltime(HWRT_GLT_HANDOFF);
        disablehwrt("Vulkan submission failed");
        return;
    }
    hwrtwaitgl();
    hwrtendgltime(HWRT_GLT_HANDOFF);

    // Has to happen here, between the wait and the composite, or the readback
    // races the Vulkan write it is meant to inspect.
    if(probequeued) { probequeued = false; hwrtprobeshared(); }

    hwrtstalls = hwrtfencestalls;

    // The composite is a fullscreen quad with no depth of its own, and it now
    // runs inside the frame rather than after it: depth test, depth write,
    // culling and blending are all still live, and an editmode wireframe pass
    // would draw the quad as four lines. Put back exactly what was found,
    // because the passes that follow (grass, water, alpha) are vanilla's and
    // expect vanilla's depth buffer.
    GLboolean hadcull = glIsEnabled(GL_CULL_FACE),
              haddepth = glIsEnabled(GL_DEPTH_TEST),
              hadblend = glIsEnabled(GL_BLEND),
              haddepthmask = GL_TRUE;
    GLint polymode[2] = { GL_FILL, GL_FILL };
    glGetIntegerv(GL_POLYGON_MODE, polymode);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &haddepthmask);
    if(hadcull) glDisable(GL_CULL_FACE);
    if(haddepth) glDisable(GL_DEPTH_TEST);
    if(hadblend) glDisable(GL_BLEND);
    glDepthMask(GL_FALSE);
    if(polymode[0] != GL_FILL) glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    // Coverage stencil is written during GL world/model raster. The composite
    // is a fullscreen quad; leftover stencil state would rewrite it.
    hwrtvelkeep();

    if(mode == HWRT_TRACE_RTAO || mode == HWRT_TRACE_SHADE || mode == HWRT_TRACE_LIGHT || hwrtdebug)
        hwrtbegingltime(HWRT_GLT_COMPOSITE);
    if(hwrtio.skipcomposite)
    {
        // Fresh shared image after a size change (Off↔SR, Quality↔Balanced, resize).
        // Compositing an uninitialized/black target over the GL raster made the
        // first Quality colour input to NGX black. Dispatch still ran above so
        // the next frame has a filled image. This frame keeps the raster.
        hwrtio.skipcomposite = false;
    }
    else if(mode == HWRT_TRACE_RTAO) hwrtcompositeao();
    else if(mode == HWRT_TRACE_SHADE || mode == HWRT_TRACE_LIGHT)
    {
        hwrtcomposite(false, 1.0f);
        // RT lighting is effective only when its pass ran and was composed,
        // not merely because Vulkan or NRD is initialised.
        if(mode == HWRT_TRACE_LIGHT && hwrtlightrecorded) hwrtlitmillis = totalmillis;
    }
    else if(hwrtdebug) hwrtcomposite(hwrtdebug == 2 || hwrtdebug == 3, 1.0f);
    hwrtendgltime(HWRT_GLT_COMPOSITE);

    if(polymode[0] != GL_FILL) glPolygonMode(GL_FRONT_AND_BACK, GLenum(polymode[0]));
    if(haddepthmask) glDepthMask(GL_TRUE);
    if(hadblend) glEnable(GL_BLEND);
    if(haddepth) glEnable(GL_DEPTH_TEST);
    if(hadcull) glEnable(GL_CULL_FACE);
}

ICOMMAND(hwrtstats, "", (),
{
    if(!hwrtavailable) { conoutf("hwrt: unavailable"); return; }
    conoutf("hwrt: %s, Vulkan %d.%d, ray query %s",
            hwrtdev.name,
            VK_API_VERSION_MAJOR(hwrtdev.apiversion), VK_API_VERSION_MINOR(hwrtdev.apiversion),
            hwrtdev.rayquery ? "yes" : "no");
    conoutf("hwrt: shared image %dx%d, %.1f MB colour + %.1f MB depth, fence stalls %d",
            hwrtio.w, hwrtio.h, hwrtio.memorysize/(1024.0f*1024.0f),
            hwrtio.depthmemorysize/(1024.0f*1024.0f), hwrtfencestalls);
    int skyhw = 0;
    int skyhh = 0;
    hwrtskyhistsize(skyhw, skyhh);
    conoutf("hwrt: skyvis history %dx%d ready %s", skyhw, skyhh, hwrtskyhistready() ? "yes" : "no");
    if(hwrtdev.rayquery)
        conoutf("hwrt: world BLAS %d triangles, %d verts%s%s%s",
                hwrtworldtris, hwrtworldverts, hwrthasworld() ? "" : " (empty)",
                hwrthasshade() ? ", hit-shade ready" : "",
                hwrthaslights() ? ", lights ready" : "");
    if(hwrthaslights()) conoutf("hwrt: %d point lights (%d unlimited, %d glow), hwrtdlights %d, sun %s, sky %s (cvar %d rays packed %.2f, filter %s, temporal %d, hold %d, histfilter %d, age %d), world gain %.2f",
            hwrtlightcount, hwrtunlimcount, hwrtglowlightcount, int(hwrtdlights),
            hwrtsunon ? "on" : "off", hwrtskyon ? "on" : "off", int(hwrtskyrays),
            float(hwrtskyrays) + (hwrtskyfilter ? 0.25f : 0.0f),
            hwrtskyfilter ? "on" : "off", int(hwrtskytemporal),
            getvar("hwrtskyhold"), getvar("hwrtskyhistfilter"), getvar("hwrtskyage"), hwrtworldgain);
    extern int hwrtnrd;
    extern int hwrtnrdavailable;
    extern int hwrtnrdready;
    extern int hwrtnrdfailed;
    extern int hwrtnrddbg;
    conoutf("hwrt: nrd %s session %s ready %d failed %d dbg %d guides %d MB",
            hwrtnrdavailable ? (hwrtnrd ? "on" : "off") : "unavailable",
            hwrtnrdsession() ? "yes" : "no", hwrtnrdready, hwrtnrdfailed, hwrtnrddbg, hwrtnrdguidemem());
    if(hwrtdev.rayquery)
    {
        conoutf("hwrt: TLAS %d instances of %d (%d mapmodels + %d dynents + %d ragdolls)%s",
                hwrtinstancecount, int(HWRT_MAX_INSTANCES), hwrtmapmodelinsts, hwrtdynentinsts, hwrtragdollinsts,
                hwrthasdynents() ? "" : " (world only)");
        if(hwrtdroppedinsts > 0)
            conoutf(CON_WARN, "hwrt: %d mapmodels dropped, the farthest ones", hwrtdroppedinsts);
    }
    if(hwrtdev.rayquery)
        conoutf("hwrt: %d animated BLASes (%d rebuilds, %d refits)",
                hwrtanimcount, hwrtanimbuilds, hwrtanimrefits);
    conoutf("hwrt: model shading %s (%d geom, %d skin layers %dx%d, budget %d MB, %d world glow layers)",
            hwrtmodelshadeready() ? "on" : "off",
            hwrtgeomcount, hwrtskinlayers, hwrtskinw, hwrtskinh, int(hwrtskinbudget), hwrtshadeglow);
    conoutf("hwrt: model mask %s, live %s; GL depth mask %s, live %s",
            hwrtmask ? "on" : "off", hwrtmaskready() ? "yes" : "no",
            hwrtdepthmask ? "on" : "off", hwrtdepthlive ? "yes" : "no");
    extern float hwrtvelcopycost;
    extern float hwrtvelgencost;
    extern float hwrtvelobjcost;
    extern float hwrtvelskelcost;
    extern float hwrtvelskelcpu;
    extern int hwrtvelskelmem;
    extern float hwrtveldebugcost;
    extern int hwrtvelocity;
    extern int hwrtveldebug;
    conoutf("hwrt: velocity %s debug %d  copy %.2f ms  gen %.2f ms  obj %.2f ms  skel %.2f ms cpu %.2f ms  overlay %.2f ms  play %.2f ms  bones %d B",
            hwrtvelocity || hwrtveldebug ? "on" : "off", int(hwrtveldebug),
            hwrtvelcopycost, hwrtvelgencost, hwrtvelobjcost, hwrtvelskelcost, hwrtvelskelcpu, hwrtveldebugcost,
            hwrtvelcopycost + hwrtvelgencost + hwrtvelobjcost + hwrtvelskelcost, hwrtvelskelmem);
    conoutf("hwrt: gpu ms  total %.2f  (gl %.2f + vk %.2f)",
            hwrttime.gptotal, hwrttime.gltotal, hwrttime.vktotal);
    conoutf("hwrt:   mask %.2f+%.2f  depth %.2f  composite %.2f",
            hwrttime.glmaskworld, hwrttime.glmaskscene, hwrttime.gldepth, hwrttime.glcomposite);
    conoutf("hwrt:   glwait %.2f  blas %.2f  tlas %.2f  light %.2f  nrd %.2f  dispatch %.2f  vk %.2f",
            hwrttime.vkglwait, hwrttime.vkblas, hwrttime.vktlas, hwrttime.vklight, hwrttime.vknrd, hwrttime.vkdispatch, hwrttime.vktotal);
    conoutf("hwrt:   handoff %.2f  (vk %.2f inside it, %.2f lost to serialization)",
            hwrttime.glhandoff, hwrttime.vktotal, hwrttime.glhandoff - hwrttime.vktotal);
    conoutf("hwrt:   frame %.2f  = hwrt gl %.2f + handoff %.2f + vanilla gl %.2f",
            hwrttime.frame, hwrttime.gltotal, hwrttime.glhandoff,
            hwrttime.frame - hwrttime.gltotal - hwrttime.glhandoff);
    conoutf("hwrt:   cpu prep %.2f  sync %.2f  (not gpu)",
            hwrttime.cpuprep, hwrttime.cpusync);
    conoutf("hwrt:   cpu inside the handoff: dispatch %.2f  of which lights %.2f",
            hwrttime.cpudispatch, hwrttime.cpulights);
});

void hwrtdrawtimes(int conw, int conh)
{
    (void)conw; (void)conh;
    if(!hwrttimes || !hwrt || !hwrtavailable) return;
    extern float conscale;
    pushhudmatrix();
    hudmatrix.scale(conscale, conscale, 1);
    flushhudmatrix();
    int x = FONTH/2, y = FONTH/2;
    draw_textf("hwrt ms  gpu %.2f  (gl %.2f + vk %.2f)", x, y,
               hwrttime.gptotal, hwrttime.gltotal, hwrttime.vktotal);
    y += FONTH;
    draw_textf("  mask %.2f+%.2f  depth %.2f  composite %.2f", x, y,
               hwrttime.glmaskworld, hwrttime.glmaskscene, hwrttime.gldepth, hwrttime.glcomposite);
    y += FONTH;
    draw_textf("  glwait %.2f  blas %.2f  tlas %.2f  light %.2f  nrd %.2f", x, y,
               hwrttime.vkglwait, hwrttime.vkblas, hwrttime.vktlas, hwrttime.vklight, hwrttime.vknrd);
    y += FONTH;
    draw_textf("  dispatch %.2f  nrd %s", x, y,
               hwrttime.vkdispatch, hwrtnrdsession() ? "REBLUR" : "skyage");
    y += FONTH;
    draw_textf("  handoff %.2f (serial %.2f)  frame %.2f", x, y,
               hwrttime.glhandoff, hwrttime.glhandoff - hwrttime.vktotal, hwrttime.frame);
    y += FONTH;
    draw_textf("  cpu prep %.2f  sync %.2f", x, y, hwrttime.cpuprep, hwrttime.cpusync);
    pophudmatrix();
}

// Samples the shared image from the GL side on the next frame, after the wait.
ICOMMAND(hwrtprobe, "", (),
{
    if(!hwrt || !hwrtavailable) { conoutf("hwrt: turn hwrt on first"); return; }
    probequeued = true;
});

static void hwrtlook(float yaw, float pitch)
{
    if(player)
    {
        player->yaw = yaw;
        player->pitch = pitch;
        player->resetinterp();
    }
    if(camera1)
    {
        camera1->yaw = yaw;
        camera1->pitch = pitch;
    }
}

static void hwrtlooksun()
{
    extern int sunlightyaw;
    extern int sunlightpitch;
    hwrtlook(float(sunlightyaw), float(sunlightpitch));
}
COMMAND(hwrtlooksun, "");
ICOMMAND(hwrtlook, "ff", (float *yaw, float *pitch), hwrtlook(*yaw, *pitch));
