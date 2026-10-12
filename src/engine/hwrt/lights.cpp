// lights.cpp: map ET_LIGHT entities plus sun / skylight as a tiny SSBO.
//
// Updated every frame so coop-edit light moves and sun-dir tweaks are visible.
// Does not rebuild the BLAS. The game's dynamic lights (muzzle flashes,
// rockets, grenades, explosions) are packed last when hwrtdynlights is on. Must
// not take down the rest of the layer if the buffer fails to allocate.
// Sun / sky (plus the fullbrightmodels floor, the world overbright, the
// previous frame's camera for the temporal skyvis pass, and the world-diffuse
// filter/vis knobs, the extended-source settings) live in a 480-byte std430 header in front of the point
// lights so the 128-byte push constants do not grow.
// Radius-0 lights (baker unlimited omni) are packed first so the 256-cap
// cannot drop them. The lighting shader always evaluates them.

#include "engine.h"
#include "hwrt/hwrt.h"

int hwrtlightcount = 0;
int hwrtunlimcount = 0;
int hwrtglowlightcount = 0;
int hwrtwaterplanecount = 0;
int hwrtsunon = 0;
int hwrtskyon = 0;

enum { HWRT_MAX_LIGHTS = 256 };

extern int sunlight, skylight, hwrtskyrays, hwrtskyfilter, hwrtskytemporal;
extern int fullbrightmodels;
extern float hwrtworldgain;
extern int hwrtshadowself;
extern int hwrtshadowothers;
extern int hwrtshadeglow;
extern int hwrtteleportlight, hwrtjumppadlight, hwrtglowdlights;
extern int farplane;

// World.diff in hitlight.comp: 0 = lod 0 bilinear (the compute path until
// 2026-10-04: far and grazing surfaces alias and shimmer), 1 = textureGrad from
// the pixel's UV footprint on the hit triangle, isotropic, no bias (old A/B:
// stable but soft, grazing floors blur). Default 2 (aliasing-rt, 2026-10-04):
// the same footprint (neighbour pixel rays on the face plane, render
// resolution, jittered matrix), scaled by 2^hwrtdiffmipbias (+ log2(render/
// display) with hwrtdiffmipauto when DLSS/FSR upscale), filtered
// anisotropically (hwrtdiffaniso). A close (magnified) surface stays at level 0,
// exactly as 0. hwrtdifftexel (off) can floor the level at the texture's own
// texels: the array stretches every layer to the largest texture with nearest,
// so a 512 texture is 2x2 blocks at level 0; 1 gives GL's bilinear look up
// close but is much softer. Only the primary world diffuse (and its blend
// layer) changes; reflections already pick their level from a ray cone, models
// keep level 0. Not saved.
// Bias 0 by default since 2026-10-05 (was -0.5): in motion under DLAA the
// shimmer of a textured surface grows with its high frequencies, and -0.5
// kept half a level more of them than the footprint; close surfaces are
// magnified and stay at level 0 either way. The bias alone is saved and
// offered in Graphics > RT Textures (Stable 0 / Sharper -0.5).
VAR(hwrtdiffmip, 0, 2, 2);
FVARP(hwrtdiffmipbias, -3, 0, 2);
VAR(hwrtdiffmipauto, 0, 1, 1);
VAR(hwrtdiffaniso, 0, 1, 1);
// RT quality (Graphics > Ray tracing), for mid-range graphics cards. 2 High is
// the full picture and the default. 1 Medium traces the sky rays of half the
// pixels (one row pair in two, alternating every frame, filled by the sky
// filter and NRD). 0 Low also keeps a single glowing light (teleporters, lava) and drops
// the anisotropic filtering of the world textures. Applied on top of the saved
// settings, which it never changes.
VARP(hwrtquality, 0, 2, 2);
// The half-rate sky needs the sky filter to fill the pixels it skips.
bool hwrtskyhalf() { return hwrtquality < 2 && hwrtskyfilter; }
static int hwrtglowcap() { return hwrtquality < 1 ? min(int(hwrtglowdlights), 1) : int(hwrtglowdlights); }
VAR(hwrtdifftexel, 0, 0, 2);
extern bool hwrtdiffanisoready();
// What hitlight.comp receives in env.diffFilter (see sampleWorldDiff).
static float diffmipfilter()
{
    if(hwrtdiffmip < 2) return hwrtdiffmip ? 1.0f : 0.0f;
    float bias = hwrtdiffmipbias;
    if(hwrtdiffmipauto && hwrtio.w > 0 && screenw > 0 && hwrtio.w < screenw)
        bias += log2f(float(hwrtio.w) / float(screenw));
    bias = clamp(bias, -3.5f, 3.5f);
    int m = 1 + (hwrtdiffaniso && hwrtquality >= 1 && hwrtdiffanisoready() ? 1 : 0) + 2 * hwrtdifftexel;
    return 8.0f * m + bias;
}
// Console witness: the effective footprint settings.
ICOMMAND(hwrtdiffmipstatus, "", (),
{
    float f = diffmipfilter();
    conoutf("hwrtdiffmip %d bias %.2f auto %d (render %dx%d display %dx%d) aniso %d (ready %d, device max %.0f) texel %d -> env %.3f",
            hwrtdiffmip, hwrtdiffmipbias, hwrtdiffmipauto, hwrtio.w, hwrtio.h, screenw, screenh,
            hwrtdiffaniso, hwrtdiffanisoready() ? 1 : 0, hwrtdev.maxaniso, hwrtdifftexel, f);
});
// 0 play (albedo * lighting). 1 albedo only. 2 lighting only (white albedo).
// 3 lod heatmap of the world-diffuse footprint (blue 0, green 1, red 2+).
// Lab vis only: never a play look.
VAR(hwrtdiffvis, 0, 0, 3);
// Sky hemisphere seed. 0 = pixel + frame (old crawl). 1 = pixel only, so a
// still camera does not retoss 1spp skyvis every frame. A world-quantized
// seed was tried and glued a high-frequency mosaic onto close snow; do not
// revive that. Default off: still DLAA dirt/snow MAD fell, but Performance
// snow_front presented MAD rose and camera motion was not proven. Not play.
VAR(hwrtskystable, 0, 0, 1);
// Keep a converged skyvis sample instead of mixing 1/N of new 4-ray noise
// every frame. Same history images and geom/noise gates. Nearest+hold
// sparkles under DLSS jitter; bilinear lookup makes the hold valid.
VAR(hwrtskyhold, 0, 1, 1);
// Fetch the skyvis history with a 2x2 bilinear of geom-tested taps.
// Nearest floor() drops the subpixel remainder of Halton jitter.
VAR(hwrtskyhistfilter, 0, 1, 1);
// Per-pixel skyvis sample count in a matching r16f ping-pong. Mix 1/age
// until the pixel has N samples, then hold may freeze. Off restores the
// old global 1/N mix (A/B). Default on: looking back at snow was mixing
// 1/32 from the first accepted tap, which is the noisy-then-settles flash.
VAR(hwrtskyage, 0, 1, 1);
// 0 play. 1 reuse heatmap (hold green, mix dark-green, reset yellow, geom red,
// miss blue). 2 skyvis grayscale. 3 age/32 grayscale. Lab vis only.
VAR(hwrtskyvisdbg, 0, 0, 3);
// Sky rays from the blue-noise tile (trace.cpp, binding 25) instead of the pcg
// hash, for the world pixels NRD denoises. Same ray count and estimator; less
// grain into REBLUR. The skyage path (Sauer filter, models) keeps the hash.
// Default on. A change reseeds the history so neither mode inherits the
// other's running average. 0 is the old hash everywhere, kept for A/B.
VARF(hwrtskybluenoise, 0, 1, 1, hwrtinvalidateskyhistory());
// Smooth normals (Options > Graphics > RT Normals, saved). 1 (default): world
// faces are lit with the smoothed vertex normals of the classic lightmap bake
// (as.cpp, smoothworldnormals), so faceted floors, slopes and rocks lose their
// light steps. 0: the flat face normal, the RT lighting before the option.
// Diffuse light and sky rays only; textures, specular, reflections and the
// shadow ray offset keep the flat face. Sky rays follow the normal, so a
// change reseeds the sky history and the temporal filters.
VARFP(hwrtsmoothnormals, 0, 1, 1, { hwrtinvalidateskyhistory(); if(!initing) hwrttemporalreset("smooth normals"); });
// Retry four subpixel directions when the pixel-centre world hit is hiddenbygl.
// Default on. Console witness only; not persisted, no menu or key.
VAR(hwrthiddenretry, 0, 1, 1);

struct hwrtlightenv
{
    float sunDir[3];
    float sunOn;        // 1 if sunlight != 0
    float sunColor[3];  // sunlightcolor/255 * sunlightscale
    float skyRays;      // 1..4, +0.25 when hwrtskyfilter
    float skyColor[3];  // skylightcolor/255
    float skyOn;        // 1 if skylight != 0
    float modelMin;     // visibility floor for ENT_PLAYER, not GL's 1.5 plate
    float worldGain;    // GL's world overbright, colorparams (hwrtworldgain)
    float shadowSelf;   // hwrtshadowself: 0 none, 1 the sun alone, 2 + the
                        // always-evaluated omnis, 3 + nearest-N finite lamps,
                        // 4 + sky
    float glowLayers;   // world glow array layers
    // Temporal reuse of skyvis (hwrtskytemporal). prevViewProj sends a hit back
    // to the pixel that holds its own history; prevCam is where the camera
    // stood when that entry was written, so the camera-relative offset stored
    // there becomes a world position again.
    float prevViewProj[16];
    float prevCam[3];
    // 0 the pass is off, read nothing and write nothing. 1 seed: write this
    // frame's estimate but do not trust what is already there. Between the
    // two, the weight the new estimate gets in the running average.
    float skyAlpha;
    float diffFilter;   // hwrtdiffmip: 0 lod0, 1 textureGrad
    float diffVis;      // hwrtdiffvis: 0 play, 1 albedo, 2 lighting, 3 lod heat
    float skyStable;    // hwrtskystable: 0 pixel+frame RNG, 1 pixel only
    float diffPad1;     // bit0 hold, bit1 bilinear hist, bits2-3 visdbg, bit4 age, bit5 nrd, bits10-11 3 - hwrtshadowothers, bit14 smooth normals
                        // bits6-9 nrddbg, bit12 hidden-retry
    float nrdWorldToView[16];
    float nrdWorldToViewPrev[16];
    float nrdViewToClip[16];
    float nrdViewToClipPrev[16];
    float nrdHitA, nrdHitB, nrdHitC, nrdRange;
    // Extended light sources (hwrtsunsize, hwrtsoftrays, hwrtsoftnoise):
    // tan of the sun disc's angular radius (0 = point sun, one exact ray), the
    // shadow rays per extended source, the sampling pattern (bit 0 new
    // pattern every frame, bit 1 blue noise), adaptive (hwrtsoftadapt).
    float softSunTan, softRays, softNoise, softAdapt;
    // Rays by apparent size (hwrtsoftraysangle, hwrtsoftraysmin): radians of
    // source diameter per extra ray (0 = always softRays), the fewest rays.
    float softRayAngle, softRayMin, softPad0, softPad1;
    // Optimised extended sources (hwrtsoftopt): the smallest light a source must add to a pixel
    // to keep its penumbra, the tangent of the smallest apparent half-angle (hwrtsoftmin, hwrtsoftminangle),
    // the temporal weight of a new filtered fraction, the filter radius per pixel of penumbra.
    float softMinContrib, softMinTan, softTemporal, softFilterScale;
};
static_assert(sizeof(hwrtlightenv) == 480, "light env header");

struct hwrtlight
{
    float pos[3];
    float radius;   // 0 = unlimited
    float color[3]; // rgb/255
    float flags;    // 0 = shadow, 1 = EF_NOSHADOW, 2 = aura (no shadow),
                    // 3 = always-eval lamp with shadow (teleport / lava);
                    // 0 and 3 carry + source radius / 128 (extended sources)
};
static_assert(sizeof(hwrtlight) == 32, "light stride");

struct hwrtlightupload
{
    hwrtlightenv env;
    hwrtlight lights[HWRT_MAX_LIGHTS];
    // One word per lamp after the 256 slots, read by hitlight.comp. bit 0: the always-evaluated loop takes
    // it (radius 0 within the first 16, glow within the first 96, counted in
    // index order like that loop counts them). bit 1: a finite lamp the
    // nearest-N loop may elect (radius > 0, flags < 1.5).
    uint kind[HWRT_MAX_LIGHTS];
    // Traced reflections: water planes after the full light table (hitlight
    // lights.water[k]) whatever the light count. pos = min x,
    // min y, surface z; radius = max x; color = max y, group, first
    // rectangle; flags = rectangle count. The rectangles (min x, min y,
    // max x, max y) of the surfaces GL draws follow (lights.waterRect).
    hwrtlight water[HWRT_MAX_WATERPLANES];
    float waterrect[HWRT_MAX_WATERRECTS][4];
};
static_assert(sizeof(hwrtlightupload) == 480 + 256 * 32 + 256 * 4 + HWRT_MAX_WATERPLANES * 32 + HWRT_MAX_WATERRECTS * 16, "light ssbo");

// Animated water (water.cpp hwrtwaterwaves): 12 floats of parameters then up
// to HWRT_WAVE_MAXN^2 slopes (x, y) behind the table, rewritten each frame;
// with the waves off only the first float (0) matters.
enum { HWRT_WAVE_MAXN = 256, HWRT_WAVE_BYTES = 12 * 4 + HWRT_WAVE_MAXN * HWRT_WAVE_MAXN * 2 * 4 };
extern void hwrtwavewrite(void *dst, int maxn);

struct hwrtlightstate
{
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceSize size;
    bool bar;
    bool failed;
    bool logpending;
    int lastlogged;
    int lastunlim;
    int lastglow;
    int lastsun, lastsky;
};

static hwrtlightstate lights = { VK_NULL_HANDLE, VK_NULL_HANDLE, 0, false, false, true, -1, -1, -1, -1, -1 };

// Visibility floor for ENT_PLAYER, not GL's flattening fullbright.
//
// GL's setshaderparams() uses mincolor = fullbrightmodels/100. At Overbright
// 150 that is 1.5, lightreaching() already clamps there, and the wrap ramp
// is almost constant — a plastic plate. Matching that in RT is the look
// the menu does not have. The menu (DRAWTEX_MODELPREVIEW) strips
// MDL_FULLBRIGHT and lights with a studio key + spec + mdlenvmap. Mode 7
// keeps a modest unlit floor so a player is still readable in a dark hall, then
// lets Lambert, the studio fill, spec and the envmap cubemap sit above it.
// Subtle 60 → 0.32, Overbright 150 → 0.32 (the option no longer flattens;
// it only keeps the unlit side a little above MODEL_MINSHADE). Max 200 → 0.35.
static float modelfloor()
{
    if(fullbrightmodels <= 0) return 0.0f;
    return 0.32f;
}

static bool lightfail(const char *what, VkResult r = VK_SUCCESS)
{
    if(lights.failed) return false;
    lights.failed = true;
    if(r != VK_SUCCESS) conoutf(CON_WARN, "hwrt: %s failed (%s), point lights disabled", what, hwrtresultstr(r));
    else conoutf(CON_WARN, "hwrt: %s, point lights disabled", what);
    return false;
}

static void destroylightbuf()
{
    if(hwrtdev.device)
    {
        if(lights.buffer) vkDestroyBuffer(hwrtdev.device, lights.buffer, NULL);
        if(lights.memory) vkFreeMemory(hwrtdev.device, lights.memory, NULL);
    }
    if(lights.bar) hwrtbaraccount(lights.size, false);
    lights.bar = false;
    lights.buffer = VK_NULL_HANDLE;
    lights.memory = VK_NULL_HANDLE;
    lights.size = 0;
    hwrtlightcount = 0;
    hwrtunlimcount = 0;
    hwrtglowlightcount = 0;
    hwrtsunon = 0;
    hwrtskyon = 0;
}

static bool ensurelightbuf()
{
    if(lights.failed) return false;
    if(lights.buffer) return true;
    if(!hwrtdev.ok()) return lightfail("no Vulkan device for point lights");

    // Animated water (water.cpp) rides behind the table: hitlight.comp waveParams / waveSlope.
    const VkDeviceSize bytes = sizeof(hwrtlightupload) + HWRT_WAVE_BYTES;
    VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    info.size = bytes;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult r = vkCreateBuffer(hwrtdev.device, &info, NULL, &lights.buffer);
    if(r != VK_SUCCESS) return lightfail("vkCreateBuffer (lights)", r);

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(hwrtdev.device, lights.buffer, &req);
    // Every pixel of the lighting pass reads it: the BAR window when there is
    // room (video memory the CPU writes through), else system memory.
    int memtype = hwrtfindbarmemtype(req.memoryTypeBits, req.size);
    lights.bar = memtype >= 0;
    if(memtype < 0) memtype = hwrtfindmemtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if(memtype < 0) memtype = hwrtfindmemtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if(memtype < 0)
    {
        destroylightbuf();
        return lightfail("no memory type for a light buffer");
    }

    VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = uint32_t(memtype);
    r = vkAllocateMemory(hwrtdev.device, &alloc, NULL, &lights.memory);
    if(r != VK_SUCCESS && lights.bar)
    {
        lights.bar = false;
        memtype = hwrtfindmemtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if(memtype < 0) { destroylightbuf(); return lightfail("no memory type for a light buffer"); }
        alloc.memoryTypeIndex = uint32_t(memtype);
        r = vkAllocateMemory(hwrtdev.device, &alloc, NULL, &lights.memory);
    }
    if(r != VK_SUCCESS) { destroylightbuf(); return lightfail("vkAllocateMemory (lights)", r); }
    lights.size = req.size;
    if(lights.bar) hwrtbaraccount(lights.size, true);
    r = vkBindBufferMemory(hwrtdev.device, lights.buffer, lights.memory, 0);
    if(r != VK_SUCCESS) { destroylightbuf(); return lightfail("vkBindBufferMemory (lights)", r); }
    return true;
}

// Saved between frames so a hit can be sent back to the pixel that holds its
// own history. Kept here rather than beside rendergl's matrices because the
// lighting dispatch is the only consumer and it must be the matrix of the
// frame that actually ran it, not of whatever GL drew last.
static matrix4 hwrtprevcamproj;
static vec hwrtprevcam(0, 0, 0);
static bool hwrtskyhistseeded = false;
static matrix4 hwrtprevnrdw2v, hwrtprevnrdv2c;
static bool hwrtnrdmatseeded = false;

extern int hwrtnrd;
extern int hwrtnrddbg;

// A resize throws the history images away, and a world rebuild moves the
// surfaces the stored positions name. Either way the next dispatch has to
// spend one frame seeding rather than blending into a place that is gone.
void hwrtinvalidateskyhistory()
{
    hwrtskyhistseeded = false;
    hwrtnrdmatseeded = false;
    hwrtnrdinvalidate();
}

// hwrtportalcol() walks every mapmodel on the map for one teleporter, with up to
// five strstr per candidate, and the answer only changes when the map does. On
// triforts that was 54 teleporters against 1481 mapmodels every single frame:
// 18 ms of CPU stuck between the GL signal and the GL wait, which is where that
// map's 30 fps came from, and why merging the pads and capping hwrtglowdlights
// never moved it -- both run after this. Cached per entity, dropped whenever the
// world is rebuilt. Nudging a mapmodel in edit mode without a remip leaves a
// stale tint until the next allchanged(), which is a colour, not a hazard.
static int portalcolepoch = 0, portalcolbuilt = -1, portalcolnents = -1;
static vector<uchar> portalcolstate; // 0 unknown, 1 no tint, 2 tint
static vector<vec> portalcolval;

void hwrtnotelightsrebuild()
{
    lights.logpending = true;
    portalcolepoch++;
    hwrtinvalidateskyhistory();
}

// Vanilla teleporter positions, gathered once per light upload. The finite-light
// loop used to rescan every entity for each light (~8 ms a frame on triforts).
// Same entities, same order, same float distance test; nothing kept across frames.
static vector<vec> vanillateleports;

static void gathervanillateleports()
{
    vanillateleports.setsize(0);
    if(!hwrtteleportlight) return;
    const vector<extentity *> &ents = entities::getents();
    loopv(ents)
    {
        extentity &e = *ents[i];
        const char *nm = entities::entname(e.type);
        if(!nm || strcmp(nm, "teleport")) continue;
        const char *mdl = entities::entmodel(e);
        if(!mdl || strcmp(mdl, "teleporter")) continue;
        vanillateleports.add(e.o);
    }
}

static bool hwrtnearvanillateleport(const vec &o, float dist)
{
    loopv(vanillateleports) if(vanillateleports[i].dist(o) < dist) return true;
    return false;
}

bool hwrthaslights()
{
    return lights.buffer != VK_NULL_HANDLE && !lights.failed;
}

void hwrtdestroylights()
{
    if(hwrtdev.device && lights.buffer) vkDeviceWaitIdle(hwrtdev.device);
    destroylightbuf();
    lights.failed = false;
    lights.logpending = true;
    lights.lastlogged = -1;
    lights.lastunlim = -1;
    lights.lastglow = -1;
    lights.lastsun = -1;
    lights.lastsky = -1;
}

// Match a colour token as its own path bit ("blue_v", "/red/", "cyan") so
// "wired" does not count as red and a nearby torch does not paint the portal.
static bool portaltoken(const char *mdl, const char *tok)
{
    if(!mdl || !tok) return false;
    size_t n = strlen(tok);
    for(const char *p = mdl; (p = strstr(p, tok)); p++)
    {
        char prev = p == mdl ? '/' : p[-1];
        char next = p[n];
        bool head = prev=='/' || prev=='_' || prev=='-';
        bool tail = !next || next=='/' || next=='_' || next=='-' || next=='\0';
        if(head && tail) return true;
    }
    return false;
}

static bool portalcolfrommdl(const char *mdl, vec &col)
{
    if(!mdl || !mdl[0]) return false;
    if(portaltoken(mdl, "blue")) { col = vec(0.14f, 0.32f, 0.50f); return true; }
    if(portaltoken(mdl, "cyan")) { col = vec(0.16f, 0.42f, 0.52f); return true; }
    if(portaltoken(mdl, "violet") || portaltoken(mdl, "purple")) { col = vec(0.42f, 0.16f, 0.52f); return true; }
    if(portaltoken(mdl, "green")) { col = vec(0.16f, 0.50f, 0.18f); return true; }
    if(portaltoken(mdl, "yellow")) { col = vec(0.52f, 0.44f, 0.14f); return true; }
    if(portaltoken(mdl, "orange")) { col = vec(0.52f, 0.30f, 0.10f); return true; }
    if(portaltoken(mdl, "red")) { col = vec(0.52f, 0.12f, 0.08f); return true; }
    if(strstr(mdl, "pad/teleport") || strstr(mdl, "pad/teledest") || strstr(mdl, "/teleport"))
    {
        col = vec(0.22f, 0.38f, 0.50f);
        return true;
    }
    return false;
}

static int portalmdlprio(const char *mdl)
{
    if(!mdl) return 3;
    if(strstr(mdl, "effect") || strstr(mdl, "magic") || strstr(mdl, "pad/") || strstr(mdl, "teleport")) return 0;
    return 2;
}

static bool hwrtportalcol(int entidx, const extentity &tp, const char *tpmdl, vec &col)
{
    if(portalcolfrommdl(tpmdl, col)) return true;
    const vector<extentity *> &ents = entities::getents();
    if(portalcolbuilt != portalcolepoch || portalcolnents != ents.length())
    {
        portalcolbuilt = portalcolepoch;
        portalcolnents = ents.length();
        portalcolstate.setsize(0);
        portalcolval.setsize(0);
        loopi(ents.length()) { portalcolstate.add(0); portalcolval.add(vec(0, 0, 0)); }
    }
    bool cached = entidx >= 0 && entidx < portalcolstate.length();
    if(cached && portalcolstate[entidx])
    {
        if(portalcolstate[entidx] == 1) return false;
        col = portalcolval[entidx];
        return true;
    }
    float bestd = 64.0f;
    int bestprio = 99;
    bool found = false;
    vec bestcol(0.22f, 0.38f, 0.50f);
    loopv(ents)
    {
        extentity &e = *ents[i];
        if(e.type != ET_MAPMODEL) continue;
        const char *mdl = mapmodelname(e.attr2);
        vec tint;
        if(!portalcolfrommdl(mdl, tint)) continue;
        float d = e.o.dist(tp.o);
        if(d >= 64.0f) continue;
        int prio = portalmdlprio(mdl);
        if(prio > bestprio) continue;
        if(prio == bestprio && d >= bestd) continue;
        bestprio = prio;
        bestd = d;
        bestcol = tint;
        found = true;
    }
    if(cached)
    {
        portalcolstate[entidx] = found ? 2 : 1;
        if(found) portalcolval[entidx] = bestcol;
    }
    if(found) col = bestcol;
    return found;
}

// Weapon lights in RT (Options > Graphics > RT Weapon Lights, saved). 1: the
// game's dynamic lights (muzzle flashes, rockets and grenades in flight,
// explosions, flag events) light the RT scene, with the flash, flicker and
// cooling shapes of dynlight.cpp (hwrtdynshape). They are packed after every map lamp, so the 256 cap
// drops them first, and hitlight.comp evaluates them in the always-evaluated
// loop, outside the nearest-N election: a flash adds light and never takes a
// map lamp away from a pixel. 0: the RT lighting before the option (those
// lights then only reach what GL still draws, such as the HUD gun).
VARP(hwrtdynlights, 0, 1, 1);
// Lab, not saved: at most this many weapon lights, the ones closest to the
// camera; their shadow (1, walls only; 0 none). Shapes and intensities:
// dynlight.cpp.
VAR(hwrtdynlightmax, 1, 16, 32);
VAR(hwrtdynshadow, 0, 1, 1);
// Only the nearest few get a soft shadow (four rays); the others one hard ray.
// Bounds the cost of a frame full of explosions (16 soft lights: about 0.2 ms
// more at 1600x900, against 0.05 ms hard).
VAR(hwrtdynsoftmax, 0, 4, 32);
int hwrtdynlightcount = 0;
extern int numalldynlights();
extern bool getalldynlight(int n, vec &o, float &radius, vec &color, int &kind, float &src);

struct hwrtdyncand
{
    vec pos;
    vec col;
    float radius;
    float src;
    float key;
};

// A rocket explodes on the wall it hit and a muzzle can touch a wall: a lamp on
// (or a hair behind) the surface would light that surface edge-on and its
// shadow ray would start inside it. Step the light out of any geometry closer
// than `clear` along the six axes.
static vec dynclearwalls(const vec &o, float clear)
{
    static const vec axes[6] = { vec(1, 0, 0), vec(-1, 0, 0), vec(0, 1, 0), vec(0, -1, 0), vec(0, 0, 1), vec(0, 0, -1) };
    vec p = o;
    loopi(6)
    {
        float d = raycube(o, axes[i], clear, 0);
        if(d < clear) p.sub(vec(axes[i]).mul(clear - d));
    }
    return p;
}

static int packdynlights(hwrtlight *out, int room)
{
    int cap = min(int(hwrtdynlightmax), room);
    if(cap <= 0 || !camera1) return 0;
    hwrtdyncand keep[32];
    int nkeep = 0;
    int total = numalldynlights();
    loopi(total)
    {
        vec o, col;
        float radius, src;
        int kind;
        if(!getalldynlight(i, o, radius, col, kind, src)) continue;
        if(radius <= 0 || max(col.x, max(col.y, col.z)) < 0.01f) continue;
        hwrtdyncand c;
        c.pos = o;
        c.col = col;
        c.radius = radius;
        c.src = src;
        // Nearest sphere first; ties keep the game's order.
        c.key = max(camera1->o.dist(o) - radius, 0.0f);
        if(nkeep < cap) keep[nkeep++] = c;
        else if(c.key < keep[nkeep-1].key) keep[nkeep-1] = c;
        else continue;
        for(int j = nkeep - 1; j > 0 && keep[j].key < keep[j-1].key; j--) swap(keep[j], keep[j-1]);
    }
    loopi(nkeep)
    {
        hwrtlight &L = out[i];
        float clear = min(keep[i].radius * 0.25f, 6.0f);
        vec p = dynclearwalls(keep[i].pos, clear);
        L.pos[0] = p.x;
        L.pos[1] = p.y;
        L.pos[2] = p.z;
        L.radius = keep[i].radius;
        L.color[0] = keep[i].col.x;
        L.color[1] = keep[i].col.y;
        L.color[2] = keep[i].col.z;
        // 4 + source radius / 128 (soft shadow; kept inside the wall clearance so
        // no sample of the source sits in the wall): shadowed. 5: no shadow ray.
        float src = i < hwrtdynsoftmax ? clamp(keep[i].src, 0.0f, min(0.8f*clear, 60.0f)) : 0.0f;
        L.flags = hwrtdynshadow ? 4.0f + src/128.0f : 5.0f;
    }
    return nkeep;
}

struct hwrtglowcand
{
    vec pos;
    vec col;
    float radius;
    float flags;
    float dist;
    bool ring;      // in a teleporter ring's hole
    float ringang;  // world angle (radians) of the ring's horizontal normal, -1 unknown
};

// Close enough to light the room you are in (including behind the camera),
// or in front of you even if far. The shader still always-evals whatever we
// upload; this is how triforts' 54 shadowed pads drop from 30 fps to playable.
static bool hwrtglowwanted(const vec &pos, float radius)
{
    if(!camera1) return true;
    float d = camera1->o.dist(pos);
    float nearR = max(radius, 96.0f) + 32.0f;
    if(d <= nearR) return true;
    if(d > 768.0f) return false;
    vec4 clip;
    camprojmatrix.transform(pos, clip);
    if(clip.w <= 0.05f) return false;
    float x = clip.x / clip.w, y = clip.y / clip.w;
    return fabs(x) <= 1.25f && fabs(y) <= 1.25f;
}

static int hwrtpickglow(hwrtglowcand *c, int nc, int cap)
{
    if(cap < 1) cap = 1;
    if(cap > HWRT_MAX_GLOWOMNI) cap = HWRT_MAX_GLOWOMNI;
    hwrtglowcand keep[HWRT_MAX_GLOWOMNI];
    int nkeep = 0;
    loopi(nc)
    {
        if(!hwrtglowwanted(c[i].pos, c[i].radius)) continue;
        c[i].dist = camera1 ? camera1->o.dist(c[i].pos) : 0;
        if(nkeep < cap)
        {
            keep[nkeep++] = c[i];
            for(int j = nkeep - 1; j > 0 && keep[j].dist < keep[j-1].dist; j--)
                swap(keep[j], keep[j-1]);
        }
        else if(c[i].dist < keep[nkeep-1].dist)
        {
            keep[nkeep-1] = c[i];
            for(int j = nkeep - 1; j > 0 && keep[j].dist < keep[j-1].dist; j--)
                swap(keep[j], keep[j-1]);
        }
    }
    loopi(nkeep) c[i] = keep[i];
    return nkeep;
}

// One visual pad often has many teleport ents (triforts: 9 per hex so the
// whole plate triggers). Each used to be its own shadowed lamp — four
// player silhouettes and 30 fps. Merge anything this close into one omni.
static int hwrtmergeglow(hwrtglowcand *c, int nc, float mergedist)
{
    if(nc <= 1) return nc;
    uchar used[HWRT_MAX_GLOWOMNI];
    memset(used, 0, nc);
    int nout = 0;
    loopi(nc)
    {
        if(used[i]) continue;
        vec pos = c[i].pos;
        vec col = c[i].col;
        float radius = c[i].radius;
        float flags = c[i].flags;
        bool ring = c[i].ring;
        float ringang = c[i].ringang;
        int n = 1;
        for(int j = i+1; j < nc; j++)
        {
            if(used[j]) continue;
            if(fabs(c[j].flags - flags) > 0.4f) continue;
            if(c[j].pos.dist(c[i].pos) >= mergedist) continue;
            used[j] = 1;
            pos.add(c[j].pos);
            col.add(c[j].col);
            if(c[j].radius > radius) radius = c[j].radius;
            n++;
        }
        used[i] = 1;
        hwrtglowcand &o = c[nout++];
        o.pos = pos.mul(1.0f/n);
        o.col = col.mul(1.0f/n);
        o.radius = radius;
        o.flags = flags;
        o.dist = 0;
        o.ring = ring;
        o.ringang = ringang;
    }
    return nout;
}

// Extended light sources (lab, not saved, all off by default). A point source
// throws a hard shadow from a single ray; an extended one throws a penumbra
// that is sharp where the occluder touches the receiver and widens with the
// distance between them. 0 everywhere keeps the single exact ray.
// hwrtsunsize: the sun's angular diameter in degrees (the real sun is about
// 0.53); unlimited lamps (the baker's old-map sun) get at least that angular
// size too. hwrtlampsize: radius in world units of every shadowed map lamp,
// plus hwrtlampsizepct percent of the lamp's own radius (big lamps get bigger
// bulbs; that part at most hwrtlampsizemax). hwrtflamesize: the radius instead for a lamp next to a fire or
// flame particle emitter (torches, braziers); -1 takes it from the emitter. hwrtglowsize: teleporters and
// lava. Each lamp's radius is kept below 0.8 times its distance to the
// nearest wall, so no sample of the source sits inside a wall.
// hwrtsoftrays: shadow rays per extended source per pixel. hwrtsoftnoise:
// bit 0 draws a new pattern every frame (for the temporal anti-aliasing to
// average), otherwise the pattern is fixed per pixel (a still dither); bit 1
// takes the pattern from the sky's blue-noise tile instead of a hash.
// Their defaults are the values Soft shadows 2 (the default, hwrtsoftshadows below) sets.
FVAR(hwrtsunsize, 0, 1, 10);
FVAR(hwrtlampsize, 0, 4, 32);
FVAR(hwrtlampsizepct, 0, 0, 20);
FVAR(hwrtlampsizemax, 0.5f, 8, 60);
FVAR(hwrtflamesize, -1, -1, 32);
FVAR(hwrtglowsize, 0, 3, 32);
VAR(hwrtsoftrays, 1, 4, 16);
VAR(hwrtsoftnoise, 0, 1, 3);
// hwrtsoftadapt: with 4 rays or more, trace the two outermost first and stop
// there when they agree (lit or shadowed), so only the penumbra pays every ray.
VAR(hwrtsoftadapt, 0, 1, 1);
// Rays by apparent size: a source seen under an angle a (its diameter over its
// distance; the sun's own diameter) gets hwrtsoftraysmin + a / hwrtsoftraysangle
// rays, at most hwrtsoftrays. Small sources (the sun, far lamps) then take one
// ray whose pattern the anti-aliasing averages, and near big lamps, whose wide
// penumbra mottles with one ray, take more. 0 = always hwrtsoftrays.
FVAR(hwrtsoftraysangle, 0, 1.5f, 90);
VAR(hwrtsoftraysmin, 1, 1, 16);
// Optimised extended sources (sources-etendues-opti). 0 = the prototype's rays as they were (hwrtsoftrays,
// hwrtsoftraysangle, hwrtsoftadapt, hwrtsoftnoise). 1 (default) = one ray per source per pixel to a point of the
// source that changes with the pixel (blue noise) and with every frame: the same image as the prototype's "auto"
// (1 to 4 rays by apparent size) for the cost of one ray, as the code is that of a point source with a moved
// target. Optional parts on top of 1, each compiled into the lighting pass only when on (trace.cpp), measured
// and not kept by default: 2 the source's other rays (by apparent size) only where the first rays of a 16x2
// strip of pixels disagree, i.e. where a penumbra or a shadow edge crosses it; 4 the extended sources' light kept
// as a lit fraction, filtered over the 5x5 pixels of the same surface with a radius set by the estimated
// penumbra width (hwrtsoftfilter per pixel of penumbra); 8 (with 4, NRD on) that fraction also averaged over the
// last frames on the same surface (hwrtsofttemporal: weight of the new frame), held inside the range of the
// neighbouring pixels; 16 a single ray to the centre for a source that adds less than hwrtsoftmin to the pixel or
// looks smaller than hwrtsoftminangle degrees.
VAR(hwrtsoftopt, 0, 1, 31);
FVAR(hwrtsoftmin, 0, 0.03f, 1);
FVAR(hwrtsoftminangle, 0, 0.2f, 10);
FVAR(hwrtsofttemporal, 0.05f, 0.25f, 1);
FVAR(hwrtsoftfilter, 0, 0.25f, 2);
// Soft shadows, the saved setting of the menu (Graphics > Ray tracing > Shadows: Off / On). 0 off (point
// lights and hard shadows, the picture and cost of before), 1 the sun (1 degree; also the "sun" lamp of old
// maps; console only), 2 the sun and the lamps (default, the menu's On: map lamps 4 units, flames sized from
// their emitter, teleporters and lava 3 units). It sets the lab sizes above (their defaults are those of 2);
// with hwrtsoftopt 1 (default) that is one ray per source per pixel.
// The prototype's rays (hwrtsoftopt 0) get its recommended "auto" (1 to 4 rays by apparent size).
static void softshadowsapply();
VARFP(hwrtsoftshadows, 0, 2, 2, softshadowsapply());
// Soft shadow rays, the saved setting under it: rays per soft light source per pixel with the optimised rays
// (hwrtsoftopt 1), to points of the source that change with the pixel and every frame. 1 the single
// ray of before: the anti-aliasing averages it, fast but grainy; 2 (default), 4, 8 a smoother penumbra, each extra ray
// costing time (the lamps' rays most). Hard shadows (hwrtsoftshadows 0) cast none.
VARP(hwrtsoftshadowrays, 1, 2, 8);
// Soft shadow denoiser, the saved setting under them: the noisy lit fractions of the sun and the extended lamps (from
// hwrtsoftshadowrays rays) denoised by NRD SIGMA (SIGMA_SHADOW_TRANSLUCENCY) before they are added to the light, so
// the penumbra is smooth and as dark as with many rays. 0 off (the soft shadows of before), 1 on (default). Needs
// NRD (hwrtnrd, on by default) and the default optimised rays (hwrtsoftopt 1); without them, the soft shadows of
// before. hwrtsoftsigmahistory: the frames SIGMA keeps to steady it, 0 (none: follows motion at once, a little more
// grain) to 7; 5 is NVIDIA's default.
extern void hwrtsigmaretry();
VARFP(hwrtsoftsigma, 0, 1, 1, hwrtsigmaretry());
VARP(hwrtsoftsigmahistory, 0, 5, 7);
static void softshadowsapply()
{
    bool lamps = hwrtsoftshadows >= 2;
    hwrtsunsize = hwrtsoftshadows >= 1 ? 1.0f : 0.0f;
    hwrtlampsize = lamps ? 4.0f : 0.0f;
    hwrtlampsizepct = 0.0f;
    hwrtlampsizemax = 8.0f;
    hwrtflamesize = lamps ? -1.0f : 0.0f;
    hwrtglowsize = lamps ? 3.0f : 0.0f;
    if(hwrtsoftshadows >= 1)
    {
        hwrtsoftrays = 4;
        hwrtsoftnoise = 1;
        hwrtsoftraysangle = 1.5f;
        hwrtsoftraysmin = 1;
        hwrtsoftadapt = 1;
    }
    else
    {
        // Off: the pattern settings of hard shadows too (as before soft shadows were on by default).
        hwrtsoftnoise = 0;
        hwrtsoftraysangle = 0;
    }
}

// Unlimited lamps (the baker's old-map sun) the last packing saw: with the sun's size they are extended too.
static int softunlimited = 0;
// A lamp may be extended (trace.cpp gives the lamps the extra rays of hwrtsoftshadowrays only then).
bool hwrtsoftlampsextended()
{
    return hwrtlampsize > 0 || hwrtlampsizepct > 0 || hwrtflamesize != 0 || hwrtglowsize > 0 || (hwrtsunsize > 0 && softunlimited > 0);
}

// Any extended source on: trace.cpp then binds the hit-light pipeline built
// with their code (the others leave it out).
bool hwrtsoftactive()
{
    return hwrtsunsize > 0 || hwrtlampsize > 0 || hwrtlampsizepct > 0 || hwrtflamesize != 0 || hwrtglowsize > 0;
}

// NRD SIGMA asked for and possible with these rays: soft sources on, optimised rays without optional parts.
bool hwrtsoftsigmawanted()
{
    return hwrtsoftsigma && hwrtsoftactive() && (hwrtsoftopt & 1) && !(hwrtsoftopt & 6);
}

// Distance from a lamp to the nearest world geometry along the 26 axis and
// diagonal directions, up to 64. Lamps do not move outside edit mode, so it is
// cached per map and entity.
static vector<vec> softclearpos;
static vector<float> softclearval;
static string softclearmap = "";
static int softclearents = -1;
static int softcachegen = 0;

static float softclearance(const vec &o)
{
    float best = 64.0f;
    for(int x = -1; x <= 1; x++) for(int y = -1; y <= 1; y++) for(int z = -1; z <= 1; z++)
    {
        if(!x && !y && !z) continue;
        vec d = vec(x, y, z).normalize();
        best = min(best, raycube(o, d, best, 0));
    }
    return best;
}

static vector<float> softflameval;
static float softnearflame(const vector<extentity *> &ents, const vec &o);

// Clearance and nearby flame size of map lamp idx, recomputed when it moves.
static float softcachedclearance(const vector<extentity *> &ents, int idx, const vec &o, float &flame)
{
    while(softclearpos.length() <= idx) { softclearpos.add(vec(-1e9f, -1e9f, -1e9f)); softclearval.add(0.0f); softflameval.add(-1.0f); }
    if(softclearpos[idx] != o)
    {
        softclearpos[idx] = o;
        softclearval[idx] = softclearance(o);
        softflameval[idx] = softnearflame(ents, o);
    }
    flame = softflameval[idx];
    return softclearval[idx];
}

static void softcachecheck(int nents)
{
    const char *m = game::getclientmap();
    if(!m) m = "";
    if(nents == softclearents && !strcmp(m, softclearmap)) return;
    softclearpos.setsize(0);
    softclearval.setsize(0);
    softflameval.setsize(0);
    copystring(softclearmap, m);
    softclearents = nents;
    softcachegen++;
}

// The nearest fire (0), fire ball (3) or flame (11) particle emitter within 24
// units: the radius of the flame it draws (renderparticles.cpp makeparticles:
// particles spawn within +-radius and are 2 * min(radius, height) wide), or -1.
static float softnearflame(const vector<extentity *> &ents, const vec &o)
{
    float best = 24.0f, size = -1.0f;
    loopv(ents)
    {
        const extentity &e = *ents[i];
        if(e.type != ET_PARTICLES) continue;
        if(e.attr1 != 0 && e.attr1 != 3 && e.attr1 != 11) continue;
        float d = e.o.dist(o);
        if(d >= best) continue;
        best = d;
        float r, h;
        if(e.attr1 == 0) { r = e.attr2 ? e.attr2 / 100.0f : 1.5f; h = e.attr3 ? e.attr3 / 100.0f : r / 3; }
        else if(e.attr1 == 11) { r = e.attr2 / 100.0f; h = e.attr3 / 100.0f; }
        else { r = h = max(int(e.attr2), 1) / 4.0f; } // fire ball: size
        size = clamp(1.5f * (r + min(r, h)), 1.0f, 12.0f);
    }
    return size;
}

// The source radius packed into a shadowed lamp's flags (+ radius / 128): the
// wanted size, kept inside the wall clearance and below 60.
// What the last frame packed (hwrtsoftstats): shadowed map lamps given a
// source, how many the wall clearance cut down, and the radii.
struct softstats
{
    int lamps, cut, flames, glows, glowcut;
    float rsum, rmin, rmax, gmin, gmax;
    void reset() { lamps = cut = flames = glows = glowcut = 0; rsum = 0; rmin = gmin = 1e9f; rmax = gmax = 0; }
    void add(float r, bool wascut) { lamps++; if(wascut) cut++; rsum += r; rmin = min(rmin, r); rmax = max(rmax, r); }
    void addglow(float r, bool wascut) { glows++; if(wascut) glowcut++; gmin = min(gmin, r); gmax = max(gmax, r); }
};
static softstats softstat, softstatlast;

static float softlampradius(const vector<extentity *> &ents, int idx, const extentity &e)
{
    if(hwrtlampsize <= 0 && hwrtlampsizepct <= 0 && hwrtflamesize == 0) return 0.0f;
    float flame;
    float clear = softcachedclearance(ents, idx, e.o, flame);
    float want = hwrtlampsize + min(hwrtlampsizepct * 0.01f * float(max(int(e.attr1), 0)), float(hwrtlampsizemax));
    if(hwrtflamesize != 0 && flame > 0)
    {
        want = hwrtflamesize > 0 ? float(hwrtflamesize) : flame;
        softstat.flames++;
    }
    if(want <= 0) return 0.0f;
    float r = clamp(want, 0.0f, min(0.8f * clear, 60.0f));
    softstat.add(r, r < want - 0.01f);
    return r;
}

extern float hwrtteleportholeradius;
extern int hwrtteleportholeaxis;

// ring: the light sits in a teleporter ring's punched hole; its disc lies in the
// ring's plane and stays inside the hole (else samples sit behind the rim).
static float softglowradius(const vec &o, bool ring)
{
    if(hwrtglowsize <= 0) return 0.0f;
    // Few glow lamps, merged per frame: a small cache by position.
    static vec gpos[64];
    static float gval[64];
    static int gnum = 0, gents = -2;
    if(gents != softcachegen) { gnum = 0; gents = softcachegen; }
    float clear = -1.0f;
    loopi(gnum) if(gpos[i] == o) { clear = gval[i]; break; }
    if(clear < 0)
    {
        clear = softclearance(o);
        if(gnum < 64) { gpos[gnum] = o; gval[gnum] = clear; gnum++; }
    }
    float r = clamp(float(hwrtglowsize), 0.0f, min(0.8f * clear, 60.0f));
    if(ring) r = min(r, 0.95f * (hwrtteleportholeradius > 0 ? hwrtteleportholeradius : 2.0f));
    softstat.addglow(r, r < hwrtglowsize - 0.01f);
    return r;
}

ICOMMAND(hwrtsoftstats, "", (),
{
    const softstats &s = softstatlast;
    conoutf("hwrt soft: %d shadowed lamps with a source (%d cut by the wall clearance, %d flames), radius min %.2f mean %.2f max %.2f",
            s.lamps, s.cut, s.flames, s.lamps ? s.rmin : 0.0f, s.lamps ? s.rsum / s.lamps : 0.0f, s.rmax);
    conoutf("hwrt soft: %d teleporter / lava sources (%d cut), radius min %.2f max %.2f, ring hole radius %.2f axis %d",
            s.glows, s.glowcut, s.glows ? s.gmin : 0.0f, s.gmax, hwrtteleportholeradius, hwrtteleportholeaxis);
});

// Sky rays actually traced. hwrtskyrays (default 1) is tuned for NRD + blue
// noise; whenever NRD is not the effective denoiser (off, unavailable or
// failed: the Sauer filter / skyage path with the pcg hash), at least 4 rays,
// the old default. The saved preference itself is never changed here.
int hwrtskyrayseffective()
{
    int rays = clamp(int(hwrtskyrays), 1, 4);
    return hwrtnrdsession() ? rays : max(rays, 4);
}

void hwrtupdatelights()
{
    if(!hwrtdev.ok() || !hwrtdev.rayquery) return;
    if(!ensurelightbuf()) return;

    hwrtlightupload packed;
    memset(&packed, 0, sizeof(packed));

    packed.env.sunDir[0] = sunlightdir.x;
    packed.env.sunDir[1] = sunlightdir.y;
    packed.env.sunDir[2] = sunlightdir.z;
    packed.env.sunOn = sunlight ? 1.0f : 0.0f;
    packed.env.sunColor[0] = float(sunlightcolor.x) / 255.0f * sunlightscale;
    packed.env.sunColor[1] = float(sunlightcolor.y) / 255.0f * sunlightscale;
    packed.env.sunColor[2] = float(sunlightcolor.z) / 255.0f * sunlightscale;
    packed.env.skyRays = float(hwrtskyrayseffective()) + (hwrtskyfilter ? 0.25f : 0.0f);
    packed.env.skyColor[0] = float(skylightcolor.x) / 255.0f;
    packed.env.skyColor[1] = float(skylightcolor.y) / 255.0f;
    packed.env.skyColor[2] = float(skylightcolor.z) / 255.0f;
    packed.env.skyOn = skylight ? 1.0f : 0.0f;
    packed.env.modelMin = modelfloor();
    packed.env.worldGain = hwrtworldgain;
    packed.env.shadowSelf = float(hwrtshadowself);
    packed.env.glowLayers = float(max(hwrtshadeglow, 0));

    // skyvis is geometry, not shading: it does not depend on where the camera
    // is, so a hit that is recognised as the same surface point can inherit
    // the whole running average that point already carries. That is the only
    // multiplier large enough to matter here -- four rays through the 5x5 are
    // about 48 samples, and N frames of them are 48N.
    memcpy(packed.env.prevViewProj, &hwrtprevcamproj, sizeof(packed.env.prevViewProj));
    packed.env.prevCam[0] = hwrtprevcam.x;
    packed.env.prevCam[1] = hwrtprevcam.y;
    packed.env.prevCam[2] = hwrtprevcam.z;
    if(!hwrtskyhistready() || hwrtskytemporal <= 1) packed.env.skyAlpha = 0.0f;
    else packed.env.skyAlpha = hwrtskyhistseeded ? 1.0f / float(hwrtskytemporal) : 1.0f;
    packed.env.diffFilter = diffmipfilter();
    packed.env.diffVis = float(hwrtdiffvis);
    packed.env.skyStable = hwrtskystable ? 1.0f : 0.0f;
    packed.env.softSunTan = hwrtsunsize > 0 ? tanf(0.5f * hwrtsunsize * RAD) : 0.0f;
    packed.env.softRays = float(clamp(int(hwrtsoftrays), 1, 16));
    // The optimised rays always take a pattern that changes with the pixel (blue noise) and every frame.
    packed.env.softNoise = (hwrtsoftopt & 1) ? 3.0f : float(hwrtsoftnoise & 3);
    packed.env.softAdapt = hwrtsoftadapt ? 1.0f : 0.0f;
    packed.env.softRayAngle = hwrtsoftraysangle > 0 ? hwrtsoftraysangle * RAD : 0.0f;
    packed.env.softRayMin = float(clamp(int(hwrtsoftraysmin), 1, 16));
    packed.env.softMinContrib = (hwrtsoftopt & 17) == 17 ? hwrtsoftmin : 0.0f;
    packed.env.softMinTan = (hwrtsoftopt & 17) == 17 ? tanf(0.5f * hwrtsoftminangle * RAD) : 0.0f;
    packed.env.softTemporal = hwrtsofttemporal;
    packed.env.softFilterScale = hwrtsoftfilter;
        packed.env.diffPad1 = float(hwrtskyhold + (hwrtskyhistfilter ? 2 : 0) + ((hwrtskyvisdbg & 3) << 2) + (hwrtskyage ? 16 : 0) + (hwrtnrdsession() ? 32 : 0) + ((hwrtnrddbg & 15) << 6) + (hwrthiddenretry ? 4096 : 0) + (hwrtskybluenoise && hwrtskybluenoiseready() ? 8192 : 0) + (hwrtsmoothnormals ? 16384 : 0) + ((3 - clamp(hwrtshadowothers, 0, 3)) << 10));
    {
        matrix4 w2v, v2c;
        hwrtnrdcameramats(w2v, v2c);
        memcpy(packed.env.nrdWorldToView, &w2v, sizeof(packed.env.nrdWorldToView));
        memcpy(packed.env.nrdViewToClip, &v2c, sizeof(packed.env.nrdViewToClip));
        if(hwrtnrdmatseeded)
        {
            memcpy(packed.env.nrdWorldToViewPrev, &hwrtprevnrdw2v, sizeof(packed.env.nrdWorldToViewPrev));
            memcpy(packed.env.nrdViewToClipPrev, &hwrtprevnrdv2c, sizeof(packed.env.nrdViewToClipPrev));
        }
        else
        {
            memcpy(packed.env.nrdWorldToViewPrev, &w2v, sizeof(packed.env.nrdWorldToViewPrev));
            memcpy(packed.env.nrdViewToClipPrev, &v2c, sizeof(packed.env.nrdViewToClipPrev));
        }
        packed.env.nrdHitA = 3.0f;
        packed.env.nrdHitB = 0.1f;
        packed.env.nrdHitC = 20.0f;
        packed.env.nrdRange = float(max(farplane, 1024));
        hwrtprevnrdw2v = w2v;
        hwrtprevnrdv2c = v2c;
        hwrtnrdmatseeded = true;
    }
    hwrtprevcamproj = camprojmatrix;
    hwrtprevcam = camera1->o;
    hwrtskyhistseeded = true;

    int n = 0, dropped = 0, nunlim = 0;
    const vector<extentity *> &ents = entities::getents();
    softcachecheck(ents.length());
    softstat.reset();
    // Radius 0 is the baker's unlimited omni (old-map "sun"). Pack those
    // first so the 256-cap cannot drop them behind a pile of lamps.
    loopv(ents)
    {
        extentity &e = *ents[i];
        if(e.type != ET_LIGHT) continue;
        if(e.attr1 != 0) continue;
        if(e.attached && e.attached->type == ET_SPOTLIGHT) continue;
        if(n >= HWRT_MAX_LIGHTS) { dropped++; continue; }
        hwrtlight &L = packed.lights[n++];
        L.pos[0] = e.o.x;
        L.pos[1] = e.o.y;
        L.pos[2] = e.o.z;
        L.radius = 0.0f;
        L.color[0] = float(e.attr2) / 255.0f;
        L.color[1] = float(e.attr3) / 255.0f;
        L.color[2] = float(e.attr4) / 255.0f;
        L.flags = (e.flags & EF_NOSHADOW) ? 1.0f : softlampradius(ents, i, e) / 128.0f;
        nunlim++;
    }
    softunlimited = nunlim;

    // Teleporters, jumppads, lava. Finite radius but always evaluated
    // (flags >= 2), otherwise hwrtdlights nearest-N would hide them behind
    // the nearest ceiling lamp. Shadows (flags 3) so walls block. Colour is
    // kept below the world clip-to-1 so the pad is a glow, not a white hole;
    // radius is long and the shader uses 1-x^2 so the room still reads at
    // mid-range. Only the nearest few that are around the camera or on
    // screen are uploaded (hwrtglowdlights). Triforts stores 9 teleport ents
    // on each hex (54 ents, 6 pads); those are merged first.
    static const vec teleportcol(0.22f, 0.38f, 0.50f);
    static const vec jumppadcol(0.52f, 0.28f, 0.10f);
    hwrtglowcand gcands[HWRT_MAX_GLOWOMNI];
    int ngcand = 0;
    loopv(ents)
    {
        extentity &e = *ents[i];
        const char *nm = entities::entname(e.type);
        if(!nm || !nm[0]) continue;
        bool tp = !strcmp(nm, "teleport");
        bool jp = !strcmp(nm, "jumppad");
        if(!tp && !jp) continue;
        if(tp && !hwrtteleportlight) continue;
        if(jp && !hwrtjumppadlight) continue;
        if(ngcand >= HWRT_MAX_GLOWOMNI) { dropped++; continue; }
        const char *tpmdl = tp ? entities::entmodel(e) : NULL;
        bool ring = tp && tpmdl && !strcmp(tpmdl, "teleporter");
        vec col = tp ? teleportcol : jumppadcol;
        float radius = tp ? 128.0f : 96.0f;
        bool fromglow = false;
        loopj(hwrtglowomnicount)
        {
            vec gp(hwrtglowomnis[j].pos[0], hwrtglowomnis[j].pos[1], hwrtglowomnis[j].pos[2]);
            if(gp.dist(e.o) < 40.0f)
            {
                col = vec(hwrtglowomnis[j].color[0], hwrtglowomnis[j].color[1], hwrtglowomnis[j].color[2]);
                fromglow = true;
                break;
            }
        }
        if(tp && !fromglow)
        {
            vec tint;
            if(hwrtportalcol(i, e, tpmdl, tint)) col = tint;
        }
        hwrtglowcand &g = gcands[ngcand++];
        g.radius = radius;
        g.col = col;
        g.flags = 3.0f;
        g.ring = ring;
        g.ringang = -1.0f;
        if(ring)
        {
            vec o;
            float yaw = 0;
            hwrtentxform(e, o, yaw);
            vec hole = hwrthasteleporthole ? hwrtteleporthole : vec(0, 0, 7.3f);
            float rad = yaw*RAD;
            float cs = cosf(rad), sn = sinf(rad);
            g.pos = vec(o.x + hole.x*cs - hole.y*sn, o.y + hole.x*sn + hole.y*cs, o.z + hole.z);
            if(hwrtteleportholeaxis == 0 || hwrtteleportholeaxis == 1)
                g.ringang = rad + (hwrtteleportholeaxis == 1 ? float(M_PI/2) : 0.0f);
        }
        else if(tp && tpmdl)
        {
            vec o;
            float yaw = 0;
            hwrtentxform(e, o, yaw);
            g.pos = vec(o.x, o.y, o.z + 20.0f);
        }
        else g.pos = vec(e.o.x, e.o.y, e.o.z + (tp ? 20.0f : 10.0f));
    }
    loopi(hwrtglowomnicount)
    {
        if(hwrtglowomnis[i].flags < 2.5f) continue; // lava: shadowed, same budget
        if(ngcand >= HWRT_MAX_GLOWOMNI) { dropped++; continue; }
        hwrtglowcand &g = gcands[ngcand++];
        g.pos = vec(hwrtglowomnis[i].pos[0], hwrtglowomnis[i].pos[1], hwrtglowomnis[i].pos[2]);
        g.col = vec(hwrtglowomnis[i].color[0], hwrtglowomnis[i].color[1], hwrtglowomnis[i].color[2]);
        g.radius = hwrtglowomnis[i].radius;
        g.flags = hwrtglowomnis[i].flags;
        g.ring = false;
        g.ringang = -1.0f;
    }
    ngcand = hwrtmergeglow(gcands, ngcand, 40.0f);
    int nsel = hwrtpickglow(gcands, ngcand, hwrtglowcap());
    // Extended teleporter rings: the source is a disc in the ring's plane (kind bit 2,
    // bits 8-15 the normal's angle / 2pi), so no sample sits behind the rim.
    uint orient[HWRT_MAX_LIGHTS];
    memset(orient, 0, sizeof(orient));
    int nglow = 0;
    vec eglowpos[HWRT_MAX_GLOWOMNI];
    int neglow = 0;
    loopi(nsel)
    {
        if(n >= HWRT_MAX_LIGHTS) { dropped++; break; }
        hwrtlight &L = packed.lights[n++];
        L.pos[0] = gcands[i].pos.x;
        L.pos[1] = gcands[i].pos.y;
        L.pos[2] = gcands[i].pos.z;
        L.radius = gcands[i].radius;
        L.color[0] = gcands[i].col.x;
        L.color[1] = gcands[i].col.y;
        L.color[2] = gcands[i].col.z;
        L.flags = gcands[i].flags;
        // Teleporter / lava (3): + source radius / 128, as the map lamps.
        if(L.flags >= 2.5f && L.flags < 3.5f) L.flags = 3.0f + softglowradius(gcands[i].pos, gcands[i].ring) / 128.0f;
        if(L.flags > 3.0f && gcands[i].ring && gcands[i].ringang >= 0)
        {
            float a = fmodf(gcands[i].ringang, float(2*M_PI));
            if(a < 0) a += float(2*M_PI);
            orient[n-1] = 4 | (uint(a / float(2*M_PI) * 256.0f) & 255) << 8;
        }
        if(neglow < HWRT_MAX_GLOWOMNI) eglowpos[neglow++] = gcands[i].pos;
        nglow++;
    }
    loopi(hwrtglowomnicount)
    {
        if(hwrtglowomnis[i].flags >= 2.5f) continue; // lava already in the pick
        vec gp(hwrtglowomnis[i].pos[0], hwrtglowomnis[i].pos[1], hwrtglowomnis[i].pos[2]);
        bool nearent = false;
        loopj(neglow) if(gp.dist(eglowpos[j]) < 40.0f) { nearent = true; break; }
        if(nearent) continue;
        if(n >= HWRT_MAX_LIGHTS) { dropped++; continue; }
        hwrtlight &L = packed.lights[n++];
        L.pos[0] = hwrtglowomnis[i].pos[0];
        L.pos[1] = hwrtglowomnis[i].pos[1];
        L.pos[2] = hwrtglowomnis[i].pos[2];
        L.radius = hwrtglowomnis[i].radius;
        L.color[0] = hwrtglowomnis[i].color[0];
        L.color[1] = hwrtglowomnis[i].color[1];
        L.color[2] = hwrtglowomnis[i].color[2];
        L.flags = hwrtglowomnis[i].flags >= 1.5f ? hwrtglowomnis[i].flags : 2.0f;
        nglow++;
    }

    gathervanillateleports();
    loopv(ents)
    {
        extentity &e = *ents[i];
        if(e.type != ET_LIGHT) continue;
        if(e.attr1 <= 0) continue;
        // v1: skip spot cones entirely (do not evaluate, do not treat as omni).
        if(e.attached && e.attached->type == ET_SPOTLIGHT) continue;
        // Vanilla rings often have a mapper ET_LIGHT a few units in front of
        // the hole (academy: ~4 and ~8 units, hole height) plus one on the
        // trigger floor. Edit mode shows those bulbs in front of the portal.
        // Packing them would light from there; the punched-hole omni replaces them.
        if(hwrtnearvanillateleport(e.o, 16.0f)) continue;
        if(n >= HWRT_MAX_LIGHTS) { dropped++; continue; }
        hwrtlight &L = packed.lights[n++];
        L.pos[0] = e.o.x;
        L.pos[1] = e.o.y;
        L.pos[2] = e.o.z;
        L.radius = float(e.attr1);
        L.color[0] = float(e.attr2) / 255.0f;
        L.color[1] = float(e.attr3) / 255.0f;
        L.color[2] = float(e.attr4) / 255.0f;
        L.flags = (e.flags & EF_NOSHADOW) ? 1.0f : softlampradius(ents, i, e) / 128.0f;
    }

    // Weapon lights last (flags 4/5): every map lamp above keeps its slot.
    int ndyn = hwrtdynlights && n < HWRT_MAX_LIGHTS ? packdynlights(&packed.lights[n], HWRT_MAX_LIGHTS - n) : 0;
    n += ndyn;
    hwrtdynlightcount = ndyn;

    // Which loop of hitlight.comp visits each lamp, decided once here
    // with the shader's own float tests and counting order.
    {
        int nunlimk = 0, nglowk = 0;
        loopi(n)
        {
            const hwrtlight &L = packed.lights[i];
            // Weapon lights: always evaluated, outside the 16 / 96 counts and
            // never elected by nearest-N (they are last, so no count moves).
            if(L.flags >= 3.5f) { packed.kind[i] = 1; continue; }
            bool unlim = L.radius <= 0.0f;
            bool glowL = L.flags >= 1.5f && !unlim;
            uint k = 0;
            if(unlim && nunlimk < 16) { k |= 1; nunlimk++; }
            else if(glowL && nglowk < 96) { k |= 1; nglowk++; }
            else if(unlim) nunlimk++;
            else if(glowL) nglowk++;
            if(!unlim && L.flags < 1.5f) k |= 2;
            packed.kind[i] = k | orient[i];
        }
    }
    {
        float planes[HWRT_MAX_WATERPLANES][8];
        int np = hwrtreflections ? hwrtgatherwaterplanes(planes, HWRT_MAX_WATERPLANES, packed.waterrect, HWRT_MAX_WATERRECTS) : 0;
        loopi(np)
        {
            hwrtlight &W = packed.water[i];
            W.pos[0] = planes[i][0];
            W.pos[1] = planes[i][1];
            W.pos[2] = planes[i][4];
            W.radius = planes[i][2];
            W.color[0] = planes[i][3];
            W.color[1] = planes[i][5];
            W.color[2] = planes[i][6];
            W.flags = planes[i][7];
        }
        hwrtwaterplanecount = np;
    }

    void *mapped = NULL;
    VkResult r = vkMapMemory(hwrtdev.device, lights.memory, 0, sizeof(packed) + HWRT_WAVE_BYTES, 0, &mapped);
    if(r != VK_SUCCESS || !mapped)
    {
        lightfail("vkMapMemory (lights)", r);
        return;
    }
    memcpy(mapped, &packed, sizeof(packed));
    hwrtwavewrite((char *)mapped + sizeof(packed), HWRT_WAVE_MAXN);
    vkUnmapMemory(hwrtdev.device, lights.memory);

    softstatlast = softstat;
    hwrtlightcount = n;
    hwrtunlimcount = nunlim;
    hwrtglowlightcount = nglow;
    hwrtsunon = sunlight ? 1 : 0;
    hwrtskyon = skylight ? 1 : 0;
    if(dropped)
    {
        static bool loggedoverflow = false;
        if(!loggedoverflow)
        {
            conoutf(CON_WARN, "hwrt: %d lights over the %d cap, extras dropped", dropped, int(HWRT_MAX_LIGHTS));
            loggedoverflow = true;
        }
    }
    if(lights.logpending || n - ndyn != lights.lastlogged || nunlim != lights.lastunlim || nglow != lights.lastglow || hwrtsunon != lights.lastsun || hwrtskyon != lights.lastsky)
    {
        conoutf("hwrt: %d point lights (%d unlimited, %d glow), sun %s, sky %s", n - ndyn, nunlim, nglow,
                hwrtsunon ? "on" : "off", hwrtskyon ? "on" : "off");
        lights.lastlogged = n - ndyn;
        lights.lastunlim = nunlim;
        lights.lastglow = nglow;
        lights.lastsun = hwrtsunon;
        lights.lastsky = hwrtskyon;
        lights.logpending = false;
    }
}

void hwrtwritelightbuffer(VkDescriptorSet set)
{
    if(!set || !hwrthaslights()) return;
    VkDescriptorBufferInfo info = { lights.buffer, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    write.dstSet = set;
    write.dstBinding = 6;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &info;
    vkUpdateDescriptorSets(hwrtdev.device, 1, &write, 0, NULL);
}

static void hwrtskystatus_()
{
    int hw = 0;
    int hh = 0;
    hwrtskyhistsize(hw, hh);
    float packed = float(hwrtskyrayseffective()) + (hwrtskyfilter ? 0.25f : 0.0f);
    const char *alphawhy = "1/N";
    if(!hwrtskyhistready() || hwrtskytemporal <= 1) alphawhy = "0-off";
    else if(!hwrtskyhistseeded) alphawhy = "seed";
    float jx = 0;
    float jy = 0;
    float px = 0;
    float py = 0;
    hwrttemporaljitterpixels(jx, jy, px, py);
    conoutf("hwrt sky packed_rays %.2f cvar_rays %d eff_rays %d denoiser %s filter %d temporal %d alpha %s hist %dx%d io %dx%d ready %d seeded %d hold %d histfilter %d age %d visdbg %d jitter_px %.3f %.3f render %dx%d",
            packed, int(hwrtskyrays), hwrtskyrayseffective(), hwrtnrdsession() ? "nrd" : "sauer", int(hwrtskyfilter), int(hwrtskytemporal), alphawhy,
            hw, hh, hwrtio.w, hwrtio.h, hwrtskyhistready() ? 1 : 0, hwrtskyhistseeded ? 1 : 0,
            int(hwrtskyhold), int(hwrtskyhistfilter), int(hwrtskyage), int(hwrtskyvisdbg),
            jx, jy, hwrtrenderw(), hwrtrenderh());
}

ICOMMAND(hwrtskystatus, "", (), hwrtskystatus_());
