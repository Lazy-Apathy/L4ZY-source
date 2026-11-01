// Path tracing, logical lighting (hwrtpt 1 + hwrtptlogical 1, trace.cpp). The map's placed finite lamps, its ambient
// and the glow auras are left out; what lights the world is what the scenery shows as a light: the sun and the sky of
// the map, its radius-0 lamps (old-map suns, kept as they are), the flames of its fire and flame emitters, its glowing
// textures, its teleporters and its lava. Each is an emitter with a size and a power (light x units^2): its light falls
// with the square of the distance, softened by its size (no infinite peak next to it) and taken smoothly to 0 at a
// radius where it no longer shows (hitlight.comp ptLogicFalloff); the path tracing carries it further by bouncing.
// Ideas from an earlier logical relighting prototype (flame emitters keyed to their torch, teleporter visuals, amber
// flame light at equal luminance, smooth flicker), rewritten for the current light table and for the path tracing.
// Packed only when hwrtptlogicframe() is true (path tracing with the logical lighting runs this frame): ray tracing and
// path tracing with the map's lighting never run any of it.

extern bool hwrtptlogicframe();
extern float hwrtptlogicfire, hwrtptlogicglow, hwrtptlogicportal, hwrtptlogiclava, hwrtptlogicreach;
extern int hwrtptlogicflicker, hwrtptlogiclamps;
extern float hwrtglowomniarea[HWRT_MAX_GLOWOMNI];
extern vec hwrtglowomninormal[HWRT_MAX_GLOWOMNI];
// Glow clusters of the last world build (as.cpp): hwrtglowomnicount itself is set back to 0 by buildshade's
// destroyshade() right after the clusters are made, so the ray tracing never packs its lava lights.
extern int hwrtglowomnifinal;

enum { PTL_KIND = 8 };   // hitlight.comp KIND_LOGICAL; kind bits 16-23 hold the size in quarter units

// Light (luminance) at 64 units of a default fire (radius 1.5, height 0.5), in the world's light units before the world
// gain: about what triforts' stacked torch lamps give there. A teleporter at 48 units, a glowing texture: its emitted
// colour times this factor (a lamp is far brighter than what its texture shows), lava: its colour at its surface.
static const float PTL_FIRE64 = 0.20f, PTL_PORTAL48 = 0.25f, PTL_GLOWBOOST = 16.0f, PTL_LAVA = 1.0f;

static float ptlluma(const vec &c) { return c.x*0.2126f + c.y*0.7152f + c.z*0.0722f; }

// Where the brightest channel of the light is down to about 0.02 (4 levels of 255 after the world gain), within
// [48, 512]: a red or blue source keeps its reach.
static float ptlreach(const vec &power)
{
    float m = max(power.x, max(power.y, power.z));
    return clamp(sqrtf(max(m, 0.0f) / 0.02f), 48.0f, 512.0f) * clamp(float(hwrtptlogicreach), 0.25f, 4.0f);
}

// Same law as hitlight.comp ptLogicFalloff: power x falloff = light.
static float ptlfalloff(float d, float s, float r)
{
    if(d >= r) return 0;
    float x = d / r, w = 1 - x*x*x*x;
    return w*w / (d*d + s*s);
}

static uint ptlkindbits(float size) { return uint(PTL_KIND) | (uint(clamp(int(size*4 + 0.5f), 0, 255)) << 16); }

// Smooth independent flicker of a flame, +/- 12 % at most, no per-frame random number.
static float ptlflicker(int id)
{
    if(!hwrtptlogicflicker) return 1;
    float t = lastmillis * 0.001f, p = id * 2.399963f;
    return 1.0f + 0.07f*sinf(t*5.3f + p) + 0.035f*sinf(t*9.1f + p*1.7f) + 0.015f*sinf(t*17.0f + p*0.7f);
}

// PT 3: flames that follow their sprites (PT 4: drawn or simulated).
#include "ptflames.inc"

// ---------------------------------------------------------------------------------------------------------------
// Fire and flame emitters (particles 0 and 11). One source per torch: a recognised torch model whose head holds
// exactly one emitter keys it (several emitters on one torch give one light); an ambiguous one stays a source of its own.
// mapperpow, mappercol (PT 3, hwrtptflamemapper): the power of the map's lamps set against this flame (0: none) and
// their colour (power-weighted sum), see ptlgatherfires.
struct ptlfire { int emitter; vec pos, color; float size, soft, mapperpow; vec mappercol; float mapperR; };
static vector<ptlfire> ptlfires;
static string ptlfiremap = "";
static int ptlfirenents = -1, ptlfireepoch = -1, ptlfireshifted = 0, ptlfireclamped = 0, ptlfirelamps = -1, ptllampmodels = 0;

static bool ptltorch(const extentity &e)
{
    if(e.type != ET_MAPMODEL) return false;
    const char *m = mapmodelname(e.attr2);
    return m && (!strcmp(m, "objects/torch_cold") || !strcmp(m, "razgriz/objects/torch"));
}

static void ptlgatherfires()
{
    const char *map = game::getclientmap();
    if(!map) map = "";
    const vector<extentity *> &ents = entities::getents();
    if(ents.length() == ptlfirenents && hwrtshadeepoch == ptlfireepoch && !strcmp(map, ptlfiremap) && hwrtptlogiclamps == ptlfirelamps) return;
    copystring(ptlfiremap, map);
    ptlfirenents = ents.length();
    ptlfireepoch = hwrtshadeepoch;
    ptlfirelamps = hwrtptlogiclamps;
    ptlfires.setsize(0);
    ptlfireshifted = ptlfireclamped = ptllampmodels = 0;
    vector<int> owners;
    loopv(ents)
    {
        const extentity &e = *ents[i];
        if(e.type != ET_PARTICLES || (e.attr1 != 0 && e.attr1 != 11)) continue;
        // renderparticles.cpp: radius and height, 0 = the old defaults for type 0.
        float r = e.attr1 == 0 && !e.attr2 ? 1.5f : e.attr2/100.0f;
        float h = e.attr1 == 0 && !e.attr3 ? r/3 : e.attr3/100.0f;
        if(r <= 0 || h <= 0) continue;
        int owner = -1, candidates = 0;
        loopj(ents.length())
        {
            const extentity &m = *ents[j];
            if(!ptltorch(m)) continue;
            vec d = vec(e.o).sub(m.o);
            if(d.z < 2 || d.z > 9 || d.x*d.x + d.y*d.y > 64) continue;
            owner = j;
            candidates++;
        }
        if(candidates != 1) owner = -1;
        if(owner >= 0)
        {
            if(owners.find(owner) >= 0) continue;
            owners.add(owner);
        }
        // An emitter set on a wall or a cage face: an empty point of its own footprint (particles spawn within
        // +/- r), reached from the emitter without crossing a wall.
        vec anchor = e.o;
        if(shadowray(anchor, vec(0, 0, 1), 0.05f, RAY_SHADOW) < 0.05f)
        {
            static const int offs[9][2] = { {0,0},{1,0},{-1,0},{0,1},{0,-1},{1,1},{-1,1},{1,-1},{-1,-1} };
            loopk(9)
            {
                vec c = vec(e.o).add(vec(offs[k][0]*r*0.5f, offs[k][1]*r*0.5f, 0.1f));
                if(shadowray(c, vec(0, 0, 1), 0.1f, RAY_SHADOW) < 0.1f) continue;
                vec back = vec(e.o).sub(c);
                float dist = back.magnitude();
                back.div(dist);
                if(shadowray(c, back, dist, RAY_SHADOW) < dist - 0.025f) continue;
                anchor = c;
                ptlfireshifted++;
                break;
            }
        }
        // The flame rises about 30 h min(h, 1) above its emitter (speed 200 min(h, 1), life 600 h ms), under a ceiling.
        float rise = 30.0f*h*min(h, 1.0f);
        float clear = shadowray(anchor, vec(0, 0, 1), rise, RAY_SHADOW);
        if(clear < rise) { rise = max(clear - 0.2f, 0.0f); ptlfireclamped++; }
        // The flame's own colour (the default reddish 0x903020 of type 0 turned warm amber at the same luminance:
        // that red describes the flame's art, not the light on the stone), normalised to luminance 1; its power grows
        // with the flame's area (radius x height), relative to the default fire.
        vec col;
        if(e.attr1 == 0 && !e.attr4) col = vec(1.0f, 0.65f, 0.36f);
        else
        {
            int c = (((e.attr4&0xF)<<4) | ((e.attr4&0xF0)<<8) | ((e.attr4&0xF00)<<12)) + 0x0F0F0F;
            col = vec((c>>16)&255, (c>>8)&255, c&255).div(255.0f);
        }
        float l = ptlluma(col);
        if(l < 1e-3f) continue;
        col.div(l);
        float area = clamp(r*h/0.75f, 0.25f, 4.0f);
        ptlfire &f = ptlfires.add();
        f.emitter = i;
        f.pos = vec(anchor).add(vec(0, 0, rise));
        f.color = col.mul(area);
        f.size = clamp(r, 1.0f, 8.0f);
        // Soft shadows: the flame drawn as a disc of its own size (lights.cpp softnearflame), inside its clearance.
        f.soft = clamp(1.5f*(r + min(r, h)), 1.0f, 12.0f);
    }
    // Option (hwrtptlogiclamps, off by default; a proposal for the places no logical source reaches): lamp models with no
    // flame emitter (the hanging lanterns objects/lamp01 and objects/lamp02, any model named *lantern*) light like a
    // default flame from just under their model (a light inside would be hidden by the model's own mesh).
    if(hwrtptlogiclamps) loopv(ents)
    {
        const extentity &e = *ents[i];
        if(e.type != ET_MAPMODEL) continue;
        const char *mn = mapmodelname(e.attr2);
        if(!mn || (strcmp(mn, "objects/lamp01") && strcmp(mn, "objects/lamp02") && !strstr(mn, "lantern"))) continue;
        model *m = loadmapmodel(e.attr2);
        vec center(0, 0, 0), radius(1, 1, 1);
        if(m) m->boundbox(center, radius);
        float yaw = e.attr1*RAD;
        vec c(center.x*cosf(yaw) - center.y*sinf(yaw), center.x*sinf(yaw) + center.y*cosf(yaw), center.z - radius.z - 1.0f);
        ptlfire &f = ptlfires.add();
        f.emitter = i;
        f.pos = vec(e.o).add(c);
        f.color = vec(1.0f, 0.65f, 0.36f).div(ptlluma(vec(1.0f, 0.65f, 0.36f)));
        f.size = clamp(min(radius.x, radius.y), 1.0f, 4.0f);
        f.soft = f.size;
        ptllampmodels++;
    }
    // PT 3 (hwrtptflamemapper): the map's lamps set against a flame are that flame's light. Each finite lamp goes to the
    // nearest flame emitter within 48 units (the distance at which the logical lighting takes a lamp for a flame's,
    // ptlum.inc: a lamp it leaves out gives its power to its flame instead of being lost); the flame's power is then the
    // sum of its lamps' (each set so its light at half its radius is what the mapper's lamp gave there, as for the light
    // fittings).
    int nmapper = 0;
    loopv(ptlfires) { ptlfires[i].mapperpow = 0; ptlfires[i].mappercol = vec(0, 0, 0); ptlfires[i].mapperR = 0; }
    loopv(ents)
    {
        const extentity &e = *ents[i];
        if(e.type != ET_LIGHT || e.attr1 <= 0) continue;
        if(e.attached && e.attached->type == ET_SPOTLIGHT) continue;
        int best = -1;
        float bestd = 48.0f;
        loopvj(ptlfires)
        {
            const extentity &fe = *ents[ptlfires[j].emitter];
            if(fe.type != ET_PARTICLES) continue;
            float d = fe.o.dist(e.o);
            if(d < bestd) { bestd = d; best = j; }
        }
        if(best < 0) continue;
        float R = float(e.attr1);
        vec lc = vec(e.attr2, e.attr3, e.attr4).div(255.0f);
        ptlfires[best].mapperpow += ptlluma(lc) * 0.125f * R*R;
        ptlfires[best].mapperR = max(ptlfires[best].mapperR, R);
        ptlfires[best].mappercol.add(vec(lc).mul(0.125f * R*R));
        nmapper++;
    }
    int withlamps = 0;
    loopv(ptlfires) if(ptlfires[i].mapperpow > 0) withlamps++;
    logoutf("hwrt pt logical: %d fire sources (%d emitters moved off a wall, %d under a ceiling, %d lamp models; %d with %d map lamps against them)",
            ptlfires.length(), ptlfireshifted, ptlfireclamped, ptllampmodels, withlamps, nmapper);
}

// ---------------------------------------------------------------------------------------------------------------
// The light table of the logical lighting. CPU copy for what GL still draws (the first-person gun and the models GL
// lights through lightreaching, the grass): hwrtptlogicfallback.
struct ptlcpulight { vec pos, col; float s, r; };
static vector<ptlcpulight> ptlcpu;
static int ptllastfires = -1, ptllastportals = -1, ptllastlava = -1, ptllastglow = -1, ptllastdropped = -1;
static int ptlnflsprite = 0;   // PT 3: fire sources driven by their sprites this frame
static int ptllastlum = -1;   // PT 3: light-fitting lamps packed last time

static void ptladdcpu(const vec &pos, const vec &col, float s, float r)
{
    ptlcpulight &c = ptlcpu.add();
    c.pos = pos; c.col = col; c.s = s; c.r = r;
}

// Teleporters: the ray tracing's own teleport lamps (ring hole, custom model, colour from a glow pad or a magic /
// pad model) plus the visible effect models of a portal that have no trigger (arrival discs: they glow too), merged
// within 40 units as the ray tracing does. Never chosen by distance to the camera (the radiance cache holds the light
// of places out of view): all of them, at most the 96 glow slots.
static int ptlportals(hwrtglowcand *c, int maxc)
{
    static const vec teleportcol(0.22f, 0.38f, 0.50f);
    int nc = 0;
    if(!hwrtteleportlight) return 0;
    const vector<extentity *> &ents = entities::getents();
    loopv(ents)
    {
        extentity &e = *ents[i];
        const char *nm = entities::entname(e.type);
        if(!nm || strcmp(nm, "teleport")) continue;
        if(nc >= maxc) break;
        const char *tpmdl = entities::entmodel(e);
        bool ring = tpmdl && !strcmp(tpmdl, "teleporter");
        vec col = teleportcol;
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
        if(!fromglow)
        {
            vec tint;
            if(hwrtportalcol(i, e, tpmdl, tint)) col = tint;
        }
        hwrtglowcand &g = c[nc++];
        g.radius = 128.0f;
        g.col = col;
        g.flags = 3.0f;
        g.ring = ring;
        g.ringang = -1.0f;
        g.dist = 0;
        g.lava = false;
        if(ring)
        {
            vec o;
            float yaw = 0;
            hwrtentxform(e, o, yaw);
            vec hole = hwrthasteleporthole ? hwrtteleporthole : vec(0, 0, 7.3f);
            float rad = yaw*RAD, cs = cosf(rad), sn = sinf(rad);
            g.pos = vec(o.x + hole.x*cs - hole.y*sn, o.y + hole.x*sn + hole.y*cs, o.z + hole.z);
            if(hwrtteleportholeaxis == 0 || hwrtteleportholeaxis == 1)
                g.ringang = rad + (hwrtteleportholeaxis == 1 ? float(M_PI/2) : 0.0f);
        }
        else if(tpmdl)
        {
            vec o;
            float yaw = 0;
            hwrtentxform(e, o, yaw);
            g.pos = vec(o.x, o.y, o.z + 20.0f);
        }
        else g.pos = vec(e.o.x, e.o.y, e.o.z + 20.0f);
    }
    int ntrig = nc;
    loopv(ents)
    {
        const extentity &e = *ents[i];
        if(e.type != ET_MAPMODEL || nc >= maxc) continue;
        const char *asset = mapmodelname(e.attr2);
        if(!asset || (!strstr(asset, "effects/magic/") && !strstr(asset, "pad/teleport") && !strstr(asset, "pad/teledest") &&
                      !strstr(asset, "/teleport") && strcmp(asset, "teleporter"))) continue;
        vec p = e.o;
        if(strstr(asset, "pad/")) p.z += 4;
        else if(!strcmp(asset, "teleporter")) p.z += 7.3f;
        bool beside = false;
        loopj(nc) if(c[j].pos.dist(p) < 40.0f) { beside = true; break; }
        if(beside) continue;
        vec col = teleportcol;
        portalcolfrommdl(asset, col);
        hwrtglowcand &g = c[nc++];
        g.pos = p; g.col = col; g.radius = 128.0f; g.flags = 3.0f; g.ring = false; g.ringang = -1.0f; g.dist = 0; g.lava = false;
    }
    (void)ntrig;
    return hwrtmergeglow(c, nc, 40.0f);
}

// Wall clearance of glow cluster i (lava, glowing texture), once per world build (26 rays each; softglowradius keeps
// only 64 positions, and a map can have 96 clusters).
static float ptlglowclear(int i, const vec &pos)
{
    static float clearval[HWRT_MAX_GLOWOMNI];
    static int clearepoch = -1;
    if(clearepoch != hwrtshadeepoch) { loopj(HWRT_MAX_GLOWOMNI) clearval[j] = -1; clearepoch = hwrtshadeepoch; }
    if(i < 0 || i >= HWRT_MAX_GLOWOMNI) return softclearance(pos);
    if(clearval[i] < 0) clearval[i] = softclearance(pos);
    return clearval[i];
}

// Glowing surfaces: emitters built once per world build from the glowing triangles (as.cpp hwrtglowtris), grouped by
// texture slot, facing, plane and 24-unit cell, so a long strip or a big panel gives several emitters along it, each as
// wide as its part and as bright as its texture's mean glow (times the glow colour). Specks under PTL_PANELMIN units^2
// light nothing worth a lamp (they still glow, and bounce rays still see them).
static const float PTL_PANELMIN = 0.5f;
struct ptlpanel { vec pos, n, col, anchor; float area, clear; int slot; ivec cell; };
static vector<ptlpanel> ptlpanels;
static int ptlpanelepoch = -1;

static void ptlbuildpanels()
{
    if(ptlpanelepoch == hwrtshadeepoch) return;
    ptlpanelepoch = hwrtshadeepoch;
    ptlpanels.setsize(0);
    loopv(hwrtglowtris)
    {
        const hwrtglowtri &t = hwrtglowtris[i];
        if(max(t.col.x, max(t.col.y, t.col.z)) < 0.02f) continue;
        ivec cell(int(floorf(t.c.x/24)), int(floorf(t.c.y/24)), int(floorf(t.c.z/24)));
        int found = -1;
        loopvj(ptlpanels)
        {
            const ptlpanel &q = ptlpanels[j];
            if(q.slot != t.slot || q.cell != cell || q.n.dot(t.n) < 0.999f || fabs(vec(t.c).sub(q.anchor).dot(q.n)) > 0.5f) continue;
            found = j;
            break;
        }
        if(found < 0)
        {
            ptlpanel &p = ptlpanels.add();
            p.slot = t.slot; p.cell = cell; p.n = t.n; p.anchor = t.c;
            p.pos = vec(0, 0, 0); p.col = vec(0, 0, 0); p.area = 0; p.clear = -1;
            found = ptlpanels.length() - 1;
        }
        ptlpanel &p = ptlpanels[found];
        p.pos.add(vec(t.c).mul(t.area));
        p.col.add(vec(t.col).mul(t.area));
        p.area += t.area;
    }
    int kept = 0;
    float total = 0;
    loopv(ptlpanels)
    {
        ptlpanel &p = ptlpanels[i];
        p.pos.div(max(p.area, 1e-6f));
        p.col.div(max(p.area, 1e-6f));
        if(p.area >= PTL_PANELMIN) { kept++; total += p.area; }
    }
    logoutf("hwrt pt logical: %d glowing triangles -> %d glowing panels (%d of %.1f units^2 or more, %.0f units^2 in all)",
            hwrtglowtris.length(), ptlpanels.length(), kept, PTL_PANELMIN, total);
}

// PT 3: the light fittings (ptlum.inc).
#include "ptlum.inc"
// PT 5: glowing textures as lights, the map's spotlights.
#include "pttex.inc"

// Packs the logical sources after the radius-0 lamps (lights.cpp hwrtupdatelights, in place of the teleport / lava /
// aura / finite lamps of the map). kindbits: the kind word bits of each slot (KIND_LOGICAL, size, ring disc).
// Order: teleporters (always evaluated, glow slots), then the lava, the fires and the glowing textures (the nearest
// hwrtdlights of them per pixel, like the map's lamps).
static int ptlpack(hwrtlightupload &packed, int n, int &dropped, uint *kindbits, int &nglow)
{
    ptlgatherfires();
    ptlcpu.setsize(0);
    int nportal = 0, nlava = 0, nfire = 0, npanel = 0, ndrop = 0;
    // Teleporters.
    hwrtglowcand gc[HWRT_MAX_GLOWOMNI];
    int ngc = ptlportals(gc, HWRT_MAX_GLOWOMNI);
    loopi(ngc)
    {
        if(n >= hwrtlightcap) { dropped++; ndrop++; continue; }
        float l = max(ptlluma(gc[i].col), 1e-3f);
        float s = 6.0f;
        float powl = PTL_PORTAL48 * (48.0f*48.0f + s*s) * clamp(float(hwrtptlogicportal), 0.0f, 16.0f);
        vec col = vec(gc[i].col).mul(powl / l);
        float r = ptlreach(col);
        hwrtlight &L = packed.lights[n++];
        L.pos[0] = gc[i].pos.x; L.pos[1] = gc[i].pos.y; L.pos[2] = gc[i].pos.z;
        L.radius = r;
        L.color[0] = col.x; L.color[1] = col.y; L.color[2] = col.z;
        L.flags = 3.0f + softglowradius(gc[i].pos, gc[i].ring) / 128.0f;
        uint kb = ptlkindbits(s);
        if(L.flags > 3.0f && gc[i].ring && gc[i].ringang >= 0)
        {
            float a = fmodf(gc[i].ringang, float(2*M_PI));
            if(a < 0) a += float(2*M_PI);
            kb |= 4 | (uint(a / float(2*M_PI) * 256.0f) & 255) << 8;
        }
        kindbits[n-1] = kb;
        ptladdcpu(gc[i].pos, col, s, r);
        nportal++;
        nglow++;
    }
    // Lava (the ray tracing's lava clusters): its colour at its surface, an emitter as wide as the cluster. A lamp of
    // the nearest-N election (a lava map has up to 96 clusters: all of them always evaluated would cost a ray each).
    loopi(min(hwrtglowomnifinal, int(HWRT_MAX_GLOWOMNI)))
    {
        if(hwrtglowomnis[i].flags < 2.5f) continue;
        if(n >= hwrtlightcap) { dropped++; ndrop++; continue; }
        vec pos(hwrtglowomnis[i].pos[0], hwrtglowomnis[i].pos[1], hwrtglowomnis[i].pos[2]);
        vec lc(hwrtglowomnis[i].color[0], hwrtglowomnis[i].color[1], hwrtglowomnis[i].color[2]);
        float s = clamp(sqrtf(max(hwrtglowomniarea[i], 1.0f) / float(M_PI)), 4.0f, 64.0f);
        vec col = vec(lc).mul(PTL_LAVA * clamp(float(hwrtptlogiclava), 0.0f, 16.0f) * s*s);
        float r = ptlreach(col);
        hwrtlight &L = packed.lights[n++];
        L.pos[0] = pos.x; L.pos[1] = pos.y; L.pos[2] = pos.z;
        L.radius = r;
        L.color[0] = col.x; L.color[1] = col.y; L.color[2] = col.z;
        float sr = hwrtglowsize > 0 ? clamp(float(hwrtglowsize), 0.0f, min(0.8f * ptlglowclear(i, pos), 60.0f)) : 0.0f;
        L.flags = sr / 128.0f;
        kindbits[n-1] = ptlkindbits(s);
        ptladdcpu(pos, col, s, r);
        nlava++;
    }
    // Fires: shadowed lamps of the nearest-N election, soft from their flame's size when soft shadows are on.
    // PT 3 (hwrtptflames): each one moves, takes its size, colour and power from the flame's sprites of this frame
    // (PT 4: drawn by GL, or simulated when GL does not draw the flame).
    const vector<extentity *> &ents = entities::getents();
    if(hwrtptflames)
    {
        // PT 3, hwrtptflamemapper: a flame the map's lamps were set against takes their power and colour (the light the
        // mapper chose for it), its sprites still give its place, size and flicker (the sprites' light is scaled so its
        // expected value is the lamps' power).
        ptflgather();
        loopv(ptlfires)
        {
            const ptlfire &f = ptlfires[i];
            bool mp = hwrtptflamemapper && f.mapperpow > 0;
            ptflsetscale(f.emitter, mp ? f.mapperpow : 0.0f, mp ? vec(f.mappercol).div(max(ptlluma(f.mappercol), 1e-4f)) : f.color);
        }
        ptflcollect();
    }
    ptlnflsprite = 0;
    loopv(ptlfires)
    {
        if(n >= hwrtlightcap) { dropped++; ndrop++; continue; }
        const ptlfire &f = ptlfires[i];
        float fire = clamp(float(hwrtptlogicfire), 0.0f, 16.0f);
        float powl = PTL_FIRE64 * (64.0f*64.0f + f.size*f.size) * fire;
        vec fpos = f.pos, fcol = f.color;
        // PT 3: the map's lamps' power and colour (no sprite this frame: the still source keeps them).
        if(hwrtptflamemapper && f.mapperpow > 0)
        {
            fcol = vec(f.mappercol).div(max(ptlluma(f.mappercol), 1e-4f));
            powl = f.mapperpow * fire;
        }
        float fsize = f.size, fsoft = f.soft, flick = ptlflicker(f.emitter), reachpow = powl;
        vec ppos, phue;
        float ppow, psize, pexp;
        if(hwrtptflames && ptflsource(f.emitter, ppos, phue, ppow, psize, pexp))
        {
            fpos = ppos;
            fcol = phue;
            powl = ppow * fire;
            reachpow = pexp * fire;
            fsize = clamp(psize, 0.5f, 8.0f);
            fsoft = clamp(2*psize, 0.5f, 12.0f);
            flick = 1;
            ptlnflsprite++;
        }
        vec col = vec(fcol).mul(powl * flick);
        float r = ptlreach(vec(fcol).mul(reachpow));
        float sr = 0;
        if(hwrtflamesize != 0)
        {
            float want = hwrtflamesize > 0 ? float(hwrtflamesize) : fsoft;
            float flame;
            float clear = softcachedclearance(ents, f.emitter, f.pos, flame);
            sr = clamp(want, 0.0f, min(0.8f * clear, 60.0f));
        }
        hwrtlight &L = packed.lights[n++];
        L.pos[0] = fpos.x; L.pos[1] = fpos.y; L.pos[2] = fpos.z;
        L.radius = r;
        L.color[0] = col.x; L.color[1] = col.y; L.color[2] = col.z;
        L.flags = sr / 128.0f;
        kindbits[n-1] = ptlkindbits(fsize);
        ptladdcpu(fpos, col, fsize, r);
        nfire++;
    }
    // Glowing textures (the ray tracing turns only big glow clusters into an unshadowed aura): the panels built from
    // every glowing triangle, each a shadowed emitter one unit in front of its surface, as wide as it, its texture's
    // mean glow times PTL_GLOWBOOST.
    ptlbuildpanels();
    // PT 5: with the glowing textures lighting by themselves (pttex.inc), their lights replace the panels.
    if(!hwrtpttexframe()) loopv(ptlpanels)
    {
        ptlpanel &pp = ptlpanels[i];
        if(pp.area < PTL_PANELMIN) continue;
        if(n >= hwrtlightcap) { dropped++; ndrop++; continue; }
        vec nrm = pp.n;
        vec pos = vec(pp.pos).add(vec(nrm).mul(1.0f));
        float s = clamp(sqrtf(pp.area / float(M_PI)), 0.5f, 24.0f);
        vec col = vec(pp.col).mul(PTL_GLOWBOOST * clamp(float(hwrtptlogicglow), 0.0f, 16.0f) * s*s);
        float r = ptlreach(col);
        uint kb = ptlkindbits(s);
        float sr = 0;
        if(hwrtlampsize > 0)
        {
            // Soft shadows: a wall panel is a disc in the wall's plane (kind bit 2, bits 8-15 the angle of its
            // normal, as a teleporter ring); a floor or ceiling one a small disc facing the lit point.
            if(fabs(nrm.z) < 0.5f)
            {
                float a = atan2f(nrm.y, nrm.x);
                if(a < 0) a += float(2*M_PI);
                kb |= 4 | (uint(a / float(2*M_PI) * 256.0f) & 255) << 8;
                sr = min(s, 24.0f);
            }
            else
            {
                if(pp.clear < 0) pp.clear = softclearance(pos);
                sr = clamp(s, 0.0f, min(0.8f * pp.clear, 60.0f));
            }
        }
        hwrtlight &L = packed.lights[n++];
        L.pos[0] = pos.x; L.pos[1] = pos.y; L.pos[2] = pos.z;
        L.radius = r;
        L.color[0] = col.x; L.color[1] = col.y; L.color[2] = col.z;
        L.flags = sr / 128.0f;
        kindbits[n-1] = kb;
        ptladdcpu(pos, col, s, r);
        npanel++;
    }
    // PT 3: the light fittings' lamps (and with the mixed approach the lamps with no visible source), ptlum.inc.
    int nlum = 0;
    if(hwrtptlogiclum || hwrtptlogicmixed)
    {
        ptlbuildlum();
        loopv(ptllums)
        {
            if(n >= hwrtlightcap) { dropped++; ndrop++; continue; }
            ptllum &l = ptllums[i];
            float r = ptlreach(l.col);
            float sr = 0;
            if(hwrtlampsize > 0)
            {
                if(l.clear < 0) l.clear = softclearance(l.pos);
                sr = clamp(l.size, 0.0f, min(0.8f * l.clear, 60.0f));
            }
            hwrtlight &L = packed.lights[n++];
            L.pos[0] = l.pos.x; L.pos[1] = l.pos.y; L.pos[2] = l.pos.z;
            L.radius = r;
            L.color[0] = l.col.x; L.color[1] = l.col.y; L.color[2] = l.col.z;
            L.flags = sr / 128.0f;
            kindbits[n-1] = ptlkindbits(l.size);
            ptladdcpu(l.pos, l.col, l.size, r);
            nlum++;
        }
    }
    // PT 5: the glowing textures' lights (in place of the panels and of the lamps set against a light texture).
    if(hwrtpttexframe()) n = pttexpack(packed, n, dropped, kindbits);
    if(nfire != ptllastfires || nportal != ptllastportals || nlava != ptllastlava || npanel != ptllastglow || ndrop != ptllastdropped || nlum != ptllastlum)
    {
        logoutf("hwrt pt logical: %d fires, %d teleporters, %d lava, %d glowing textures, %d fitting lamps (%d over the cap)", nfire, nportal, nlava, npanel, nlum, ndrop);
        ptllastfires = nfire; ptllastportals = nportal; ptllastlava = nlava; ptllastglow = npanel; ptllastdropped = ndrop; ptllastlum = nlum;
    }
    return n;
}

// What GL still lights itself (lightmap.cpp lightreaching: the first-person gun, models GL draws, the grass): the
// logical sources' direct light at a point, with a CPU shadow ray each (no bounce: those stay out of the path tracing).
void hwrtptlogicfallback(const vec &target, vec &color, vec &dir, extentity *skip)
{
    loopv(ptlcpu)
    {
        const ptlcpulight &l = ptlcpu[i];
        vec ray = vec(target).sub(l.pos);
        float d = ray.magnitude();
        if(d >= l.r) continue;
        vec c = vec(l.col).mul(ptlfalloff(d, l.s, l.r));
        if(max(c.x, max(c.y, c.z)) < 0.004f) continue;
        if(d > 0.01f)
        {
            ray.div(d);
            if(shadowray(l.pos, ray, d, RAY_SHADOW | RAY_POLY, skip) < d - 0.01f) continue;
        }
        color.add(c);
        dir.add(vec(ray).mul(-ptlluma(c)));
    }
}
