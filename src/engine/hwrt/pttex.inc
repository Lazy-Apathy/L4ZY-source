// PT 5 (prototype): glowing textures as lights (hwrtpttexlights, trace.cpp), and the map's spotlights (hwrtptspot).
// The light textures the scenery shows (the light fittings the logical lighting recognises by name or through
// data/ptemissive.cfg, ptlum.inc, and every texture with a glow map) light from their own surface: each group of their
// faces (same texture, facing, plane and 24-unit cell, as the logical lighting's panels) is a surface emitter one unit in
// front of it, as wide as the group, that lights only what is in front of it (cosine of the angle to its normal). Its
// light is physical light (the light it emits times its area, falling with the square of the distance, softened by its
// size, faded to 0 at its reach) shown again before it joins the lamps (hitlight.comp ptSurfFalloff): the light of a
// panel spreads as real light does, without a blinding spot under it.
// Strength: hwrtpttexpower (1: the surface emits what a white wall shows in the lumel's full light), times the glow's own
// brightness for a glow map, times the list's power when data/ptemissive.cfg gives one. Colour: the glow's colour; for a
// light fitting, the colour of the map's lamps set against it (the light the mapper chose for it), else the list's
// colour, else a warm white.
// With the map's lighting the map's lamps all stay (the texture's light adds to theirs); with the logical lighting the
// texture's light replaces the fitting lamps set against it and the glowing panels (ptlogical.inc, ptlum.inc).
// Nothing of this runs unless path tracing runs with hwrtpttexlights 1 (hwrtpttexframe).

extern bool hwrtpttexframe(), hwrtptspotframe(), hwrtptlampsortframe();
extern float hwrtpttexpower, hwrtpttexreach, hwrtpttexgroup;
extern int hwrtpttexlights;

enum { PTT_KIND_SURFACE = 16, PTT_KIND_SPOT = 32 };   // hitlight.comp KIND_SURFACE, KIND_SPOT

// A direction in the kind word: bits 8-15 its azimuth / 2pi, bits 24-31 its elevation from -90 to 90 degrees.
static uint ptkinddir(const vec &d)
{
    float az = atan2f(d.y, d.x);
    if(az < 0) az += float(2*M_PI);
    float el = asinf(clamp(d.z, -1.0f, 1.0f));
    uint a = uint(az / float(2*M_PI) * 256.0f + 0.5f) & 255;
    uint e = uint(clamp(int((el / float(M_PI) + 0.5f) * 255.0f + 0.5f), 0, 255));
    return (a << 8) | (e << 24);
}

struct pttexgroup { vec pos, n, col, anchor; float area, clear, bright; int slot; ivec cell; bool glow; };
static vector<pttexgroup> pttexgroups;
static int pttexepoch = -1, pttexversion = -1, pttexnents = -1;
// PT 7: the grouping distance the groups were made with, and how many 24-unit groups they came from.
static float pttexgroupdist = -1;
static int pttexrawcount = 0;
static int pttexlast = -1, pttexlastdrop = -1;

static void pttexadd(const hwrtglowtri &t, bool glow)
{
    ivec cell(int(floorf(t.c.x/24)), int(floorf(t.c.y/24)), int(floorf(t.c.z/24)));
    loopv(pttexgroups)
    {
        pttexgroup &q = pttexgroups[i];
        if(q.glow != glow || q.slot != t.slot || q.cell != cell || q.n.dot(t.n) < 0.999f || fabs(vec(t.c).sub(q.anchor).dot(q.n)) > 0.5f) continue;
        q.pos.add(vec(t.c).mul(t.area));
        q.col.add(vec(t.col).mul(t.area));
        q.area += t.area;
        return;
    }
    pttexgroup &g = pttexgroups.add();
    g.pos = vec(t.c).mul(t.area); g.n = t.n; g.col = vec(t.col).mul(t.area); g.anchor = t.c;
    g.area = t.area; g.clear = -1; g.bright = 1; g.slot = t.slot; g.cell = cell; g.glow = glow;
}

// PT 7: the light a group sends (its colour times its brightness times its area: what the packing multiplies by the
// strength), and its luminance.
static vec pttexflux(const pttexgroup &g) { return vec(g.col).mul(g.bright * g.area); }
struct pttexorder { float key; int idx; };
static bool pttexorderless(const pttexorder &a, const pttexorder &b) { return a.key > b.key || (a.key == b.key && a.idx < b.idx); }

// PT 7 (hwrtpttexgroup): neighbouring groups become one light. The strongest group not yet taken starts a light; a
// group not yet taken joins it when it faces the same way (within 2.6 degrees), lies in its plane (within 2 units),
// touches a group already in it (within PTT_LINK units: the 24-unit groups next to it, diagonals included: the light
// never jumps over a gap) and is within `dist` units of the starting group. The light sends the sum of their light
// (law 0), from the point their light is centred on (in the starting group's plane), as wide as their area together:
// seen from farther than the group is wide, it lights as they did. Law 1 (comparison only): the sum of the light
// each one shows from afar (each surface emitter's light is shown on its own, hitlight.comp ptSurfFalloff),
// (sum of light^(1/2.2))^2.2. Same groups, same order, same result every time: nothing depends on the camera. Specks
// under 0.5 units^2 stay as they are (never packed).
static const float PTT_LINK = 36.0f;
static void pttexmerge(float dist, int law)
{
    pttexrawcount = pttexgroups.length();
    int ng = pttexgroups.length();
    if(dist <= 0 || ng < 2) return;
    vector<pttexorder> order;
    loopi(ng)
    {
        const pttexgroup &g = pttexgroups[i];
        pttexorder &o = order.add();
        o.key = g.area >= 0.5f ? ptlluma(pttexflux(g)) : -1.0f;
        o.idx = i;
    }
    order.sort(pttexorderless);
    // Cells of PTT_LINK units: the groups that touch a group are in its cell or the 26 around it.
    hashtable<ivec, int> head(1024);
    vector<int> next;
    loopi(ng) next.add(-1);
    loopi(ng)
    {
        const vec &p = pttexgroups[i].pos;
        int &h = head.access(ivec(int(floorf(p.x/PTT_LINK)), int(floorf(p.y/PTT_LINK)), int(floorf(p.z/PTT_LINK))), -1);
        next[i] = h;
        h = i;
    }
    vector<int> taken;
    loopi(ng) taken.add(-1);
    vector<pttexgroup> merged;
    vector<int> members;
    loopv(order)
    {
        int s = order[i].idx;
        if(taken[s] >= 0) continue;
        const pttexgroup &seed = pttexgroups[s];
        taken[s] = merged.length();
        members.setsize(0);
        members.add(s);
        // Grown from the starting group, one touching group after another, in the map's order within each cell.
        for(int k = 0; order[i].key >= 0 && k < members.length(); k++)
        {
            const vec &mp = pttexgroups[members[k]].pos;
            ivec c(int(floorf(mp.x/PTT_LINK)), int(floorf(mp.y/PTT_LINK)), int(floorf(mp.z/PTT_LINK)));
            for(int dz = -1; dz <= 1; dz++) for(int dy = -1; dy <= 1; dy++) for(int dx = -1; dx <= 1; dx++)
            {
                const int *h = head.access(ivec(c.x + dx, c.y + dy, c.z + dz));
                for(int j = h ? *h : -1; j >= 0; j = next[j])
                {
                    if(taken[j] >= 0) continue;
                    const pttexgroup &g = pttexgroups[j];
                    if(g.area < 0.5f || seed.n.dot(g.n) < 0.999f || g.pos.dist(mp) > PTT_LINK) continue;
                    vec d = vec(g.pos).sub(seed.pos);
                    if(fabs(d.dot(seed.n)) > 2.0f || d.magnitude() > dist) continue;
                    taken[j] = merged.length();
                    members.add(j);
                }
            }
        }
        pttexgroup m = seed;
        if(members.length() > 1)
        {
            // Members in the map's order (the sums below do not depend on the cells' order).
            members.sort();
            vec flux(0, 0, 0), shown(0, 0, 0), pos(0, 0, 0);
            float area = 0, w = 0;
            loopvj(members)
            {
                const pttexgroup &g = pttexgroups[members[j]];
                vec f = pttexflux(g);
                flux.add(f);
                shown.add(vec(powf(max(f.x, 0.0f), 1/2.2f), powf(max(f.y, 0.0f), 1/2.2f), powf(max(f.z, 0.0f), 1/2.2f)));
                float l = ptlluma(f);
                pos.add(vec(g.pos).mul(l));
                w += l;
                area += g.area;
            }
            if(w > 1e-9f) pos.div(w); else pos = seed.pos;
            // In the starting group's plane.
            pos.sub(vec(seed.n).mul(vec(pos).sub(seed.pos).dot(seed.n)));
            if(law == 1) flux = vec(powf(shown.x, 2.2f), powf(shown.y, 2.2f), powf(shown.z, 2.2f));
            m.pos = pos;
            m.area = area;
            m.bright = 1;
            m.col = vec(flux).div(max(area, 1e-6f));
            m.clear = -1;
        }
        merged.add(m);
    }
    pttexgroups.setsize(0);
    loopv(merged) pttexgroups.add(merged[i]);
}

// The groups of this world build, their colour and brightness (cached per world build, list and entities).
static void pttexbuild()
{
    const vector<extentity *> &ents = entities::getents();
    // PT 7: and per grouping distance.
    float gdist = clamp(float(hwrtpttexgroup), 0.0f, 512.0f);
    if(pttexepoch == hwrtshadeepoch && pttexversion == ptlemisversion && pttexnents == ents.length() && pttexgroupdist == gdist) return;
    pttexgroupdist = gdist;
    pttexepoch = hwrtshadeepoch;
    pttexversion = ptlemisversion;
    pttexnents = ents.length();
    ptlemisload();
    pttexgroups.setsize(0);
    loopv(hwrtlumtris) pttexadd(hwrtlumtris[i], false);
    loopv(hwrtglowtris)
    {
        const hwrtglowtri &t = hwrtglowtris[i];
        if(max(t.col.x, max(t.col.y, t.col.z)) < 0.02f) continue;
        pttexadd(t, true);
    }
    int withlamps = 0, fromlist = 0;
    loopv(pttexgroups)
    {
        pttexgroup &g = pttexgroups[i];
        g.pos.div(max(g.area, 1e-6f));
        g.col.div(max(g.area, 1e-6f));
        Slot &sl = lookupslot(g.slot, false);
        bool fitting = false;
        const ptlemissive *l = sl.sts.empty() ? NULL : ptlemisfind(sl.sts[0].name, 1, fitting);
        if(g.glow)
        {
            // The glow's colour; its brightness (luminance, at most 1) scales the strength.
            g.bright = clamp(ptlluma(g.col), 0.0f, 1.0f);
            if(g.bright > 1e-4f) g.col.div(ptlluma(g.col));
        }
        else
        {
            // A light fitting: the colour of the map's lamps set against it (within hwrtptlogiclumdist of its faces, in
            // front of them), weighted by their power; else the list's colour; else a warm white.
            float r = sqrtf(g.area / float(M_PI));
            vec lc(0, 0, 0);
            loopvj(ents)
            {
                const extentity &e = *ents[j];
                if(e.type != ET_LIGHT || e.attr1 <= 0) continue;
                vec d = vec(e.o).sub(g.pos);
                if(d.dot(g.n) < -1.0f) continue;
                if(max(d.magnitude() - r, 0.0f) > hwrtptlogiclumdist) continue;
                float R = float(e.attr1);
                lc.add(vec(e.attr2, e.attr3, e.attr4).div(255.0f).mul(R*R));
            }
            if(ptlluma(lc) > 1e-4f) { g.col = lc.div(ptlluma(lc)); withlamps++; }
            else if(l && (l->color.x > 0 || l->color.y > 0 || l->color.z > 0) && ptlluma(l->color) > 1e-4f) g.col = vec(l->color).div(ptlluma(l->color));
            else g.col = vec(1.0f, 0.9f, 0.8f).div(ptlluma(vec(1.0f, 0.9f, 0.8f)));
            g.bright = 1;
        }
        // The list's power, when it gives one, multiplies the strength (1 = as the others).
        if(l && l->power > 0) { g.bright *= l->power; fromlist++; }
    }
    // PT 7: neighbouring groups as one light.
    pttexmerge(gdist, 0);
    logoutf("hwrt pt textures: %d light texture groups (%d light fittings with map lamps against them, %d powers from the list), %d lights (grouped within %.0f units)",
            pttexrawcount, withlamps, fromlist, pttexgroups.length(), gdist);
}

// PT 5: how far a group's light still shows (its shown light down to 0.05, before the world gain), within [16, reach]:
// what sorts it against the map's lamps (their radius) when the table is full.
static float pttexuseful(const pttexgroup &g, float power, float reach)
{
    vec col = vec(g.col).mul(power * g.bright * g.area);
    float m = max(col.x, max(col.y, col.z));
    return clamp(sqrtf(max(m, 0.0f) / 0.00137f), 16.0f, reach);
}

// Packs the texture emitters after the slots already used (map lighting: after the map's lamps; logical lighting: in
// ptlpack). kindbits: the kind word bits of each slot.
// keep (PT 5, ptpackmaplamps): only the groups it marks (NULL: all).
static int pttexpack(hwrtlightupload &packed, int n, int &dropped, uint *kindbits, const vector<bool> *keep = NULL)
{
    pttexbuild();
    int ntex = 0, ndrop = 0;
    float reach = clamp(float(hwrtpttexreach), 64.0f, 2048.0f);
    float power = clamp(float(hwrtpttexpower), 0.0f, 16.0f);
    // PT 5: more groups than the slots left (16 kept for the weapon lights): the ones whose light comes nearest the camera.
    vector<bool> nearkeep;
    if(!keep)
    {
        int room = max(hwrtlightcap - n - 16, 0), count = 0;
        loopv(pttexgroups) if(pttexgroups[i].area >= 0.5f) count++;
        if(count > room)
        {
            vec cam = camera1 ? camera1->o : vec(0, 0, 0);
            vector<float> keys, sorted;
            loopv(pttexgroups)
            {
                const pttexgroup &g = pttexgroups[i];
                float k = g.area >= 0.5f ? max(cam.dist(g.pos) - pttexuseful(g, power, reach), 0.0f) : 1e30f;
                keys.add(k);
                if(g.area >= 0.5f) sorted.add(k);
            }
            sorted.sort();
            float limit = room > 0 ? sorted[room - 1] : -1.0f;
            int kept = 0;
            loopv(pttexgroups) { bool k = keys[i] <= limit && kept < room; if(k) kept++; nearkeep.add(k); }
            keep = &nearkeep;
            dropped += count - kept;
            ndrop += count - kept;
        }
    }
    loopv(pttexgroups)
    {
        pttexgroup &g = pttexgroups[i];
        if(g.area < 0.5f) continue;
        if(keep && (!keep->inrange(i) || !(*keep)[i])) continue;
        vec col = vec(g.col).mul(power * g.bright * g.area);
        float m = max(col.x, max(col.y, col.z));
        if(m <= 1e-6f) continue;
        if(n >= hwrtlightcap) { dropped++; ndrop++; continue; }
        float s = clamp(sqrtf(g.area / float(M_PI)), 0.5f, 24.0f);
        // Reach: where its shown light (before the world gain) falls to about 0.01 (physical 0.01^2.2), within [48, reach].
        float r = clamp(sqrtf(m / 4.0e-5f), 48.0f, reach);
        vec pos = vec(g.pos).add(vec(g.n).mul(1.0f));
        uint kb = uint(PTT_KIND_SURFACE) | (uint(clamp(int(s*4 + 0.5f), 0, 255)) << 16) | ptkinddir(g.n);
        float sr = 0;
        if(hwrtlampsize > 0)
        {
            // Soft shadows as the glowing panels: a wall emitter is a disc in the wall's plane (kind bit 2, the azimuth
            // already in bits 8-15), a floor or ceiling one a disc facing the lit point.
            if(fabs(g.n.z) < 0.5f) { kb |= 4; sr = min(s, 24.0f); }
            else
            {
                if(g.clear < 0) g.clear = softclearance(pos);
                sr = clamp(s, 0.0f, min(0.8f * g.clear, 60.0f));
            }
        }
        hwrtlight &L = packed.lights[n++];
        L.pos[0] = pos.x; L.pos[1] = pos.y; L.pos[2] = pos.z;
        L.radius = r;
        L.color[0] = col.x; L.color[1] = col.y; L.color[2] = col.z;
        L.flags = sr / 128.0f;
        kindbits[n-1] = kb;
        ntex++;
    }
    if(ntex != pttexlast || ndrop != pttexlastdrop)
    {
        logoutf("hwrt pt textures: %d glowing texture lights (%d over the cap)", ntex, ndrop);
        pttexlast = ntex;
        pttexlastdrop = ndrop;
    }
    return n;
}

// The kind word of a map lamp attached to a spotlight (hwrtptspot): its axis (from the lamp to the spotlight entity)
// and its half-angle in degrees (bits 16-23), for hitlight.comp ptSpotAtten (the baker's cone, lightmap.cpp).
static uint ptspotkind(const extentity &e)
{
    vec axis = vec(e.attached->o).sub(e.o);
    if(axis.magnitude() < 1e-4f) axis = vec(0, 0, -1);
    axis.normalize();
    return uint(PTT_KIND_SPOT) | (uint(clamp(int(e.attached->attr1), 1, 89)) << 16) | ptkinddir(axis);
}

// PT 5 (hwrtptlampsort): the lamp table holds 256 lights. A map with more (Alloy: 538 lamps, triforts: 650 and more)
// loses the ones after the first 256 of the map, wherever the player is: rooms whose lamps come late in the map stay
// dark. With the path tracing and the map's lighting, when the map's lamps (and the glowing textures' lights) do not all
// fit, the ones whose light comes nearest the camera (distance to the sphere they light) are packed, 16 slots kept for
// the weapon lights. The ray tracing keeps its own order (it does not change).
struct ptlampcand { float key; int idx; bool tex; };
static int ptlampsortlast = -1, ptlampsortdrop = -1;

static bool ptlampcandok(const extentity &e, bool spot)
{
    if(e.type != ET_LIGHT || e.attr1 <= 0) return false;
    if(e.attached && e.attached->type == ET_SPOTLIGHT && !spot) return false;
    if(hwrtnearvanillateleport(e.o, 16.0f)) return false;
    return true;
}

static void ptpackonelamp(hwrtlightupload &packed, int &n, uint *kindbits, const vector<extentity *> &ents, int i, bool spot)
{
    const extentity &e = *ents[i];
    hwrtlight &L = packed.lights[n++];
    L.pos[0] = e.o.x;
    L.pos[1] = e.o.y;
    L.pos[2] = e.o.z;
    L.radius = float(e.attr1);
    L.color[0] = float(e.attr2) / 255.0f;
    L.color[1] = float(e.attr3) / 255.0f;
    L.color[2] = float(e.attr4) / 255.0f;
    L.flags = (e.flags & EF_NOSHADOW) ? 1.0f : softlampradius(ents, i, e) / 128.0f;
    if(spot && e.attached && e.attached->type == ET_SPOTLIGHT) kindbits[n-1] = ptspotkind(e);
}

// The map's lamps and (texl) the glowing textures' lights, after the slots already used. Packs all of them in the
// map's order when they fit; else the nearest ones (see above).
static int ptpackmaplamps(hwrtlightupload &packed, int n, int &dropped, uint *kindbits, const vector<extentity *> &ents, bool spot, bool texl)
{
    vector<ptlampcand> cand;
    vec cam = camera1 ? camera1->o : vec(0, 0, 0);
    loopv(ents)
    {
        const extentity &e = *ents[i];
        if(!ptlampcandok(e, spot)) continue;
        ptlampcand &c = cand.add();
        c.key = max(cam.dist(e.o) - float(e.attr1), 0.0f);
        c.idx = i;
        c.tex = false;
    }
    int nlamps = cand.length();
    if(texl)
    {
        pttexbuild();
        float reach = clamp(float(hwrtpttexreach), 64.0f, 2048.0f);
        loopv(pttexgroups)
        {
            const pttexgroup &g = pttexgroups[i];
            if(g.area < 0.5f) continue;
            ptlampcand &c = cand.add();
            c.key = max(cam.dist(g.pos) - pttexuseful(g, clamp(float(hwrtpttexpower), 0.0f, 16.0f), reach), 0.0f);
            c.idx = i;
            c.tex = true;
        }
    }
    int room = max(hwrtlightcap - n - 16, 0);
    if(cand.length() <= room)
    {
        loopv(cand) if(!cand[i].tex && n < hwrtlightcap) ptpackonelamp(packed, n, kindbits, ents, cand[i].idx, spot);
        if(texl) n = pttexpack(packed, n, dropped, kindbits);
        if(ptlampsortlast != 0) { logoutf("hwrt pt lamps: all %d map lamps fit the table", nlamps); ptlampsortlast = 0; }
        return n;
    }
    // Keep the room nearest: a partial selection by key (the lamps of the map, then the textures' lights, in their order).
    vector<float> keys;
    loopv(cand) keys.add(cand[i].key);
    keys.sort();
    float limit = keys[room - 1];
    int kept = 0, keptlimit = 0;
    loopv(cand) if(cand[i].key < limit) kept++;
    vector<bool> keep;
    loopv(cand)
    {
        bool k = cand[i].key < limit;
        if(!k && cand[i].key == limit && kept + keptlimit < room) { k = true; keptlimit++; }
        keep.add(k);
    }
    int nkept = 0, ntexkept = 0;
    loopv(cand) if(keep[i] && !cand[i].tex && n < hwrtlightcap) { ptpackonelamp(packed, n, kindbits, ents, cand[i].idx, spot); nkept++; }
    if(texl)
    {
        // The kept glowing textures' lights only.
        vector<bool> texkeep;
        loopv(pttexgroups) texkeep.add(false);
        loopv(cand) if(keep[i] && cand[i].tex) { texkeep[cand[i].idx] = true; ntexkept++; }
        n = pttexpack(packed, n, dropped, kindbits, &texkeep);
    }
    int ndrop = cand.length() - nkept - ntexkept;
    dropped += ndrop;
    if(ptlampsortlast != nkept || ptlampsortdrop != ndrop)
    {
        logoutf("hwrt pt lamps: %d map lamps and %d glowing texture lights for %d slots: the %d nearest the camera are kept (%d lamps, %d textures)",
                nlamps, cand.length() - nlamps, room, nkept + ntexkept, nkept, ntexkept);
        ptlampsortlast = nkept;
        ptlampsortdrop = ndrop;
    }
    return n;
}
