// lights.cpp: map ET_LIGHT entities plus sun / skylight as a tiny SSBO.
//
// Updated every frame so coop-edit light moves and sun-dir tweaks are visible.
// Does not rebuild the BLAS, does not copy dynlights / muzzle flashes, and
// must not take down the rest of the layer if the buffer fails to allocate.
// Sun / sky (plus the fullbrightmodels floor, the world overbright, the
// previous frame's camera for the temporal skyvis pass, and the world-diffuse
// filter/vis knobs) live in a 432-byte std430 header in front of the point
// lights so the 128-byte push constants do not grow.
// Radius-0 lights (baker unlimited omni) are packed first so the 256-cap
// cannot drop them. The lighting shader always evaluates them.

#include "engine.h"
#include "hwrt/hwrt.h"

int hwrtlightcount = 0;
int hwrtunlimcount = 0;
int hwrtglowlightcount = 0;
int hwrtsunon = 0;
int hwrtskyon = 0;

enum { HWRT_MAX_LIGHTS = 256 };

extern int sunlight, skylight, hwrtskyrays, hwrtskyfilter, hwrtskytemporal;
extern int fullbrightmodels;
extern float hwrtworldgain;
extern int hwrtshadowself;
extern int hwrtshadeglow;
extern int hwrtteleportlight, hwrtjumppadlight, hwrtglowdlights;
extern int farplane;

// World.diff in hitlight.comp: 0 = lod 0 bilinear (old compute path), 1 =
// textureGrad from the pixel's UV footprint on the hit triangle. Default off:
// close surfaces (the user grain case) correctly stay at lod 0, so enabling
// this does not remove the presented crawl and it softens minifying regions.
// Diagnostic A/B keeps 1 available. Not a jitter kill and not a play look.
VAR(hwrtdiffmip, 0, 0, 1);
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
    float diffPad1;     // bit0 hold, bit1 bilinear hist, bits2-3 visdbg, bit4 age, bit5 nrd
                        // bits6-9 nrddbg, bit12 hidden-retry
    float nrdWorldToView[16];
    float nrdWorldToViewPrev[16];
    float nrdViewToClip[16];
    float nrdViewToClipPrev[16];
    float nrdHitA, nrdHitB, nrdHitC, nrdRange;
};
static_assert(sizeof(hwrtlightenv) == 432, "light env header");

struct hwrtlight
{
    float pos[3];
    float radius;   // 0 = unlimited
    float color[3]; // rgb/255
    float flags;    // 0 = shadow, 1 = EF_NOSHADOW, 2 = aura (no shadow),
                    // 3 = always-eval lamp with shadow (teleport / lava)
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
};
static_assert(sizeof(hwrtlightupload) == 432 + 256 * 32 + 256 * 4, "light ssbo");

struct hwrtlightstate
{
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceSize size;
    bool failed;
    bool logpending;
    int lastlogged;
    int lastunlim;
    int lastglow;
    int lastsun, lastsky;
};

static hwrtlightstate lights = { VK_NULL_HANDLE, VK_NULL_HANDLE, 0, false, true, -1, -1, -1, -1, -1 };

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

    const VkDeviceSize bytes = sizeof(hwrtlightupload);
    VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    info.size = bytes;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult r = vkCreateBuffer(hwrtdev.device, &info, NULL, &lights.buffer);
    if(r != VK_SUCCESS) return lightfail("vkCreateBuffer (lights)", r);

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(hwrtdev.device, lights.buffer, &req);
    int memtype = hwrtfindmemtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
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
    if(r != VK_SUCCESS) { destroylightbuf(); return lightfail("vkAllocateMemory (lights)", r); }
    r = vkBindBufferMemory(hwrtdev.device, lights.buffer, lights.memory, 0);
    if(r != VK_SUCCESS) { destroylightbuf(); return lightfail("vkBindBufferMemory (lights)", r); }
    lights.size = req.size;
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

struct hwrtglowcand
{
    vec pos;
    vec col;
    float radius;
    float flags;
    float dist;
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
    }
    return nout;
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
    packed.env.skyRays = float(hwrtskyrays) + (hwrtskyfilter ? 0.25f : 0.0f);
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
    packed.env.diffFilter = hwrtdiffmip ? 1.0f : 0.0f;
    packed.env.diffVis = float(hwrtdiffvis);
    packed.env.skyStable = hwrtskystable ? 1.0f : 0.0f;
        packed.env.diffPad1 = float(hwrtskyhold + (hwrtskyhistfilter ? 2 : 0) + ((hwrtskyvisdbg & 3) << 2) + (hwrtskyage ? 16 : 0) + (hwrtnrdsession() ? 32 : 0) + ((hwrtnrddbg & 15) << 6) + (hwrthiddenretry ? 4096 : 0) + (hwrtskybluenoise && hwrtskybluenoiseready() ? 8192 : 0));
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
        L.flags = (e.flags & EF_NOSHADOW) ? 1.0f : 0.0f;
        nunlim++;
    }

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
        if(ring)
        {
            vec o;
            float yaw = 0;
            hwrtentxform(e, o, yaw);
            vec hole = hwrthasteleporthole ? hwrtteleporthole : vec(0, 0, 7.3f);
            float rad = yaw*RAD;
            float cs = cosf(rad), sn = sinf(rad);
            g.pos = vec(o.x + hole.x*cs - hole.y*sn, o.y + hole.x*sn + hole.y*cs, o.z + hole.z);
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
    }
    ngcand = hwrtmergeglow(gcands, ngcand, 40.0f);
    int nsel = hwrtpickglow(gcands, ngcand, hwrtglowdlights);
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
        L.flags = (e.flags & EF_NOSHADOW) ? 1.0f : 0.0f;
    }

    // Which loop of hitlight.comp visits each lamp, decided once here
    // with the shader's own float tests and counting order.
    {
        int nunlimk = 0, nglowk = 0;
        loopi(n)
        {
            const hwrtlight &L = packed.lights[i];
            bool unlim = L.radius <= 0.0f;
            bool glowL = L.flags >= 1.5f && !unlim;
            uint k = 0;
            if(unlim && nunlimk < 16) { k |= 1; nunlimk++; }
            else if(glowL && nglowk < 96) { k |= 1; nglowk++; }
            else if(unlim) nunlimk++;
            else if(glowL) nglowk++;
            if(!unlim && L.flags < 1.5f) k |= 2;
            packed.kind[i] = k;
        }
    }
    void *mapped = NULL;
    VkResult r = vkMapMemory(hwrtdev.device, lights.memory, 0, sizeof(packed), 0, &mapped);
    if(r != VK_SUCCESS || !mapped)
    {
        lightfail("vkMapMemory (lights)", r);
        return;
    }
    memcpy(mapped, &packed, sizeof(packed));
    vkUnmapMemory(hwrtdev.device, lights.memory);

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
    if(lights.logpending || n != lights.lastlogged || nunlim != lights.lastunlim || nglow != lights.lastglow || hwrtsunon != lights.lastsun || hwrtskyon != lights.lastsky)
    {
        conoutf("hwrt: %d point lights (%d unlimited, %d glow), sun %s, sky %s", n, nunlim, nglow,
                hwrtsunon ? "on" : "off", hwrtskyon ? "on" : "off");
        lights.lastlogged = n;
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
    float packed = float(hwrtskyrays) + (hwrtskyfilter ? 0.25f : 0.0f);
    const char *alphawhy = "1/N";
    if(!hwrtskyhistready() || hwrtskytemporal <= 1) alphawhy = "0-off";
    else if(!hwrtskyhistseeded) alphawhy = "seed";
    float jx = 0;
    float jy = 0;
    float px = 0;
    float py = 0;
    hwrttemporaljitterpixels(jx, jy, px, py);
    conoutf("hwrt sky packed_rays %.2f cvar_rays %d filter %d temporal %d alpha %s hist %dx%d io %dx%d ready %d seeded %d hold %d histfilter %d age %d visdbg %d jitter_px %.3f %.3f render %dx%d",
            packed, int(hwrtskyrays), int(hwrtskyfilter), int(hwrtskytemporal), alphawhy,
            hw, hh, hwrtio.w, hwrtio.h, hwrtskyhistready() ? 1 : 0, hwrtskyhistseeded ? 1 : 0,
            int(hwrtskyhold), int(hwrtskyhistfilter), int(hwrtskyage), int(hwrtskyvisdbg),
            jx, jy, hwrtrenderw(), hwrtrenderh());
}

ICOMMAND(hwrtskystatus, "", (), hwrtskystatus_());
