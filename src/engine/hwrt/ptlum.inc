// PT 3 (prototype), logical lighting: the light fittings the mapper lit with placed lamps (ptlogical.inc, lights.cpp).
// A map lamp (radius > 0) set against a light fitting the scenery shows (a lamp, lantern, chandelier, wall or ceiling
// light model, a light texture without a glow map) is that fitting's light: it is kept, as a logical source (the law
// and the size of the other sources: power x falloff / (d^2 + size^2)), the size of the fitting. A fitting no lamp is
// set against lights from the emissive list (data/ptemissive.cfg: model or texture -> colour, power, size) when the
// list gives it a power. Recognition: by name (built in, extended or refused by the list), the light textures by name.
// Mixed approach (hwrtptlogicmixed, off by default, to be decided): the lamps with no visible source at all are
// kept too, as logical sources where the mapper put them.
// Nothing of this runs unless path tracing runs with the logical lighting and hwrtptlogiclum (or hwrtptlogicmixed) is on.

static void ptlumreset() { extern void hwrtptclearcache(); hwrtptclearcache(); }
// PT 7: 1 by default (decided with PT 3).
VARF(hwrtptlogiclum, 0, 1, 1, ptlumreset());
VARF(hwrtptlogicmixed, 0, 0, 1, ptlumreset());
// A lamp is a fitting's light within this distance of the fitting (model box or light face), units.
FVARF(hwrtptlogiclumdist, 4, 32, 128, ptlumreset());
// Power of a kept lamp: the light the mapper's lamp gave at half its radius (factor of the prototype's calibration).
FVARF(hwrtptlogiclumpower, 0, 1, 16, ptlumreset());
// PT 5: a map lamp set against a window (a see-through face: stained glass, grating, glass) is the light that comes in
// through it (PT 3). 0 (default: a window lets the light through, it is not a source): such a lamp has no
// visible source; the light of the sky and the sun still comes through the window (the traced rays cross it).
VARFP(hwrtptlogicwindows, 0, 0, 1, ptlumreset());

// The emissive list: patterns of model names and texture paths (case-insensitive substrings).
struct ptlemissive { string pattern; int kind; vec color; float power, size, offz; };   // kind 0 model, 1 texture, 2 never
static vector<ptlemissive> ptlemis;
static int ptlemisversion = 0;
static bool ptlemisloaded = false;

static void ptlemisadd(int kind, const char *pat, float r, float g, float b, float power, float size, float offz)
{
    if(!pat || !pat[0]) return;
    ptlemissive &e = ptlemis.add();
    copystring(e.pattern, pat);
    e.kind = kind;
    e.color = vec(r, g, b).div(255.0f);
    e.power = power;
    e.size = size;
    e.offz = offz;
    ptlemisversion++;
}
ICOMMAND(ptemissivemodel, "sffffff", (char *p, float *r, float *g, float *b, float *pw, float *sz, float *oz), ptlemisadd(0, p, *r, *g, *b, *pw, *sz, *oz));
ICOMMAND(ptemissivetex, "sffffff", (char *p, float *r, float *g, float *b, float *pw, float *sz, float *oz), ptlemisadd(1, p, *r, *g, *b, *pw, *sz, *oz));
ICOMMAND(ptemissivenot, "s", (char *p), ptlemisadd(2, p, 0, 0, 0, 0, 0, 0));
ICOMMAND(ptemissiveclear, "", (), { ptlemis.setsize(0); ptlemisversion++; });

// Built in: what a light fitting is usually called.
static const char *const ptlmodelnames[] = { "lamp", "lantern", "light", "chandelier", "candle", "sconce", "neon", "bulb", "lustre", "luster" };
static const char *const ptlmodelnot[] = { "torch_cold", "lightning", "flashlight", "highlight" };
// (tech1soc, a stock pack: sqrlig02wb, sqrlig03bc, 064lig20bb, 064lig22bb are its square and strip lights.)
static const char *const ptltexnames[] = { "light", "lamp", "neon", "fluo", "bulb", "lantern", "lig0", "lig1", "lig2" };
static const char *const ptltexnot[] = { "lightmap", "highlight", "lightning", "skybox" };

static bool ptlhas(const char *s, const char *pat)
{
    if(!s || !pat || !pat[0]) return false;
    for(const char *p = s; *p; p++)
    {
        int i = 0;
        while(pat[i] && p[i] && tolower(uchar(p[i])) == tolower(uchar(pat[i]))) i++;
        if(!pat[i]) return true;
    }
    return false;
}

static void ptlemisload()
{
    if(ptlemisloaded) return;
    ptlemisloaded = true;
    execfile("data/ptemissive.cfg", false);
}

// Does this model name / texture path name a light fitting? The list first (refusal, then entry), then the names.
static const ptlemissive *ptlemisfind(const char *name, int kind, bool &fitting)
{
    fitting = false;
    if(!name) return NULL;
    loopv(ptlemis) if(ptlemis[i].kind == 2 && ptlhas(name, ptlemis[i].pattern)) return NULL;
    loopv(ptlemis) if(ptlemis[i].kind == kind && ptlhas(name, ptlemis[i].pattern)) { fitting = true; return &ptlemis[i]; }
    const char *const *nots = kind ? ptltexnot : ptlmodelnot;
    int nnot = kind ? int(sizeof(ptltexnot)/sizeof(ptltexnot[0])) : int(sizeof(ptlmodelnot)/sizeof(ptlmodelnot[0]));
    loopi(nnot) if(ptlhas(name, nots[i])) return NULL;
    const char *const *names = kind ? ptltexnames : ptlmodelnames;
    int nnames = kind ? int(sizeof(ptltexnames)/sizeof(ptltexnames[0])) : int(sizeof(ptlmodelnames)/sizeof(ptlmodelnames[0]));
    loopi(nnames) if(ptlhas(name, names[i])) { fitting = true; return NULL; }
    return NULL;
}

// as.cpp, for every world triangle of the world build: is its texture a light fitting's? (recorded in hwrtlumtris)
bool hwrtptlumslot(Slot *slot)
{
    if(!slot || slot->sts.empty()) return false;
    // Cached per slot index with the name's hash: another map reuses the indices for other textures (a cache keyed
    // on the index alone took turbine's and douze's walls for the light textures academy had at those indices).
    static hashtable<int, uint> cache;
    static int cacheversion = -1;
    ptlemisload();
    if(cacheversion != ptlemisversion) { cache.clear(); cacheversion = ptlemisversion; }
    const char *name = slot->sts[0].name;
    uint h = 2166136261u;
    for(const char *p = name; *p; p++) h = (h ^ uchar(*p)) * 16777619u;
    h &= ~1u;
    uint *c = cache.access(slot->index);
    if(c && (*c & ~1u) == h) return (*c & 1u) != 0;
    bool fitting = false;
    ptlemisfind(name, 1, fitting);
    cache[slot->index] = h | (fitting ? 1u : 0u);
    return fitting;
}

// One light-fitting texture face group (as the glowing panels: slot, facing, plane, 24-unit cell).
struct ptllumpanel { vec pos, n, anchor; float area; int slot; ivec cell; };
static vector<ptllumpanel> ptllumpanels;

// Windows (PT 3, with the light fittings): the see-through faces of the world, those of the cubes in the alpha
// material (stained glass, gratings: GL blends them, the traced rays go through them) and the faces of the glass
// material. A map lamp set against one is the light that comes in through it (gubo: two lamps the sky's colour, 4 units
// behind the stained glass). Built from the octree once per world build.
static vector<ptllumpanel> ptlwinpanels;
static void ptlwinadd(const vec &c, const vec &n, float area, int slot)
{
    ivec cell(int(floorf(c.x/24)), int(floorf(c.y/24)), int(floorf(c.z/24)));
    loopv(ptlwinpanels)
    {
        ptllumpanel &q = ptlwinpanels[i];
        if(q.slot != slot || q.cell != cell || q.n.dot(n) < 0.999f || fabs(vec(c).sub(q.anchor).dot(q.n)) > 0.5f) continue;
        q.pos.add(vec(c).mul(area));
        q.area += area;
        return;
    }
    ptllumpanel &p = ptlwinpanels.add();
    p.slot = slot; p.cell = cell; p.n = n; p.anchor = c; p.pos = vec(c).mul(area); p.area = area;
}
static void ptlwinwalk(cube *c, const ivec &o, int size)
{
    loopi(8)
    {
        ivec co(i, o, size);
        if(c[i].children) { ptlwinwalk(c[i].children, co, size>>1); continue; }
        int mat = c[i].material;
        bool alpha = (mat & MAT_ALPHA) != 0 && !isempty(c[i]);
        bool glass = (mat & MATF_VOLUME) == MAT_GLASS;
        if(!alpha && !glass) continue;
        loopj(6)
        {
            int dim = j>>1, side = j&1;
            vec n(0, 0, 0);
            n[dim] = side ? 1 : -1;
            vec fc = vec(co).add(float(size)/2);
            fc[dim] = side ? co[dim] + size : co[dim];
            if(alpha)
            {
                if(c[i].texture[j] == DEFAULT_SKY || !visibleface(c[i], j, co, size)) continue;
                ptlwinadd(fc, n, float(size)*float(size), c[i].texture[j]);
            }
            else
            {
                // A face of the glass volume where it meets something that is not glass and not solid.
                vec probe = vec(fc).add(vec(n).mul(0.5f));
                if(probe.x < 0 || probe.y < 0 || probe.z < 0 || probe.x >= worldsize || probe.y >= worldsize || probe.z >= worldsize) continue;
                cube &nb = lookupcube(ivec(probe));
                if((nb.material & MATF_VOLUME) == MAT_GLASS || !isempty(nb)) continue;
                ptlwinadd(fc, n, float(size)*float(size), -1);
            }
        }
    }
}

// The sources of this world build: kept lamps (merged per fitting) and fittings lit by the list.
struct ptllum { vec pos, col; float size, clear; int cls; int fitting; float rmax; };   // cls: 0 model, 1 texture, 2 list, 3 mixed, 4 window ; clear: wall clearance (-1 not yet) ; rmax (PT 4): largest radius of its lamps (0: none)
static vector<ptllum> ptllums;
static int ptllumepoch = -1, ptllumversion = -1, ptllumnents = -1, ptllumflags = -1;
// Counts of the last build (per map): the lamps and why they light (or not).
struct ptllumcount { int lamps, flame, portal, glow, lava, model, tex, window, none, sources, listsources, windows; };
static ptllumcount ptllc;
// Per entity of the last build: why that map lamp lights or not (-1 not a map lamp; 0 flame, 1 teleporter, 2 glowing
// texture, 3 light model, 4 light texture, 5 window, 6 no visible source, 7 lava).
static vector<schar> ptllampcls;

static float ptlmodelbox(const extentity &e, const vec &p, vec &center, float &rxy, float &rz)
{
    model *m = loadmapmodel(e.attr2);
    vec c(0, 0, 0), r(2, 2, 2);
    if(m) m->boundbox(c, r);
    float yaw = e.attr1*RAD, cs = cosf(yaw), sn = sinf(yaw);
    center = vec(e.o.x + c.x*cs - c.y*sn, e.o.y + c.x*sn + c.y*cs, e.o.z + c.z);
    rxy = sqrtf(r.x*r.x + r.y*r.y);
    rz = r.z;
    vec d = vec(p).sub(center);
    float dxy = max(sqrtf(d.x*d.x + d.y*d.y) - rxy, 0.0f), dz = max(fabs(d.z) - rz, 0.0f);
    return sqrtf(dxy*dxy + dz*dz);
}

static void ptlbuildlum()
{
    const vector<extentity *> &ents = entities::getents();
    extern bool hwrtpttexframe();
    int flags = hwrtptlogiclum | (hwrtptlogicmixed << 1) | (hwrtpttexframe() ? 4 : 0) | (hwrtptlogicwindows << 3);
    if(ptllumepoch == hwrtshadeepoch && ptllumversion == ptlemisversion && ptllumnents == ents.length() && ptllumflags == flags) return;
    ptllumepoch = hwrtshadeepoch;
    ptllumversion = ptlemisversion;
    ptllumnents = ents.length();
    ptllumflags = flags;
    ptlemisload();
    ptllums.setsize(0);
    memset(&ptllc, 0, sizeof(ptllc));
    ptllampcls.setsize(0);
    loopv(ents) ptllampcls.add(-1);
    // Light-fitting faces, grouped.
    ptllumpanels.setsize(0);
    loopv(hwrtlumtris)
    {
        const hwrtglowtri &t = hwrtlumtris[i];
        ivec cell(int(floorf(t.c.x/24)), int(floorf(t.c.y/24)), int(floorf(t.c.z/24)));
        int found = -1;
        loopvj(ptllumpanels)
        {
            const ptllumpanel &q = ptllumpanels[j];
            if(q.slot != t.slot || q.cell != cell || q.n.dot(t.n) < 0.999f || fabs(vec(t.c).sub(q.anchor).dot(q.n)) > 0.5f) continue;
            found = j;
            break;
        }
        if(found < 0)
        {
            ptllumpanel &p = ptllumpanels.add();
            p.slot = t.slot; p.cell = cell; p.n = t.n; p.anchor = t.c; p.pos = vec(0, 0, 0); p.area = 0;
            found = ptllumpanels.length() - 1;
        }
        ptllumpanel &p = ptllumpanels[found];
        p.pos.add(vec(t.c).mul(t.area));
        p.area += t.area;
    }
    loopv(ptllumpanels) ptllumpanels[i].pos.div(max(ptllumpanels[i].area, 1e-6f));
    // Windows (see-through faces of the world).
    ptlwinpanels.setsize(0);
    if(worldroot) ptlwinwalk(worldroot, ivec(0, 0, 0), worldsize>>1);
    loopv(ptlwinpanels) ptlwinpanels[i].pos.div(max(ptlwinpanels[i].area, 1e-6f));
    ptllc.windows = ptlwinpanels.length();
    // Light-fitting models.
    vector<int> fitents;
    vector<const ptlemissive *> fitlist;
    loopv(ents)
    {
        const extentity &e = *ents[i];
        if(e.type != ET_MAPMODEL) continue;
        bool fitting = false;
        const ptlemissive *l = ptlemisfind(mapmodelname(e.attr2), 0, fitting);
        if(!fitting) continue;
        fitents.add(i);
        fitlist.add(l);
    }
    vector<int> fitlit;   // per fitting model: index of its source, -1 none
    loopv(fitents) fitlit.add(-1);
    vector<int> panellit;
    loopv(ptllumpanels) panellit.add(-1);
    vector<int> winlit;
    loopv(ptlwinpanels) winlit.add(-1);
    float maxd = hwrtptlogiclumdist;
    loopv(ents)
    {
        const extentity &e = *ents[i];
        if(e.type != ET_LIGHT || e.attr1 <= 0) continue;
        if(e.attached && e.attached->type == ET_SPOTLIGHT) continue;
        ptllc.lamps++;
        // Already lit by another family of the logical lighting (flame, teleporter, glowing texture): left to it.
        bool flame = false, portal = false, glow = false;
        loopvj(ents)
        {
            const extentity &o = *ents[j];
            if(o.type == ET_PARTICLES && (o.attr1 == 0 || o.attr1 == 11) && o.o.dist(e.o) < 48) { flame = true; break; }
            const char *nm = entities::entname(o.type);
            if(nm && !strcmp(nm, "teleport") && o.o.dist(e.o) < 48) portal = true;
        }
        if(!flame && !portal) loopvj(hwrtglowtris) if(hwrtglowtris[j].c.dist(e.o) < 32) { glow = true; break; }
        // Lava (its clusters are logical sources, ptlpack): a lamp within 48 units of a cluster's edge is its light.
        bool lava = false;
        if(!flame && !portal && !glow) loopj(min(hwrtglowomnifinal, int(HWRT_MAX_GLOWOMNI)))
        {
            if(hwrtglowomnis[j].flags < 2.5f) continue;
            vec gp(hwrtglowomnis[j].pos[0], hwrtglowomnis[j].pos[1], hwrtglowomnis[j].pos[2]);
            if(gp.dist(e.o) - sqrtf(max(hwrtglowomniarea[j], 1.0f)/float(M_PI)) < 48) { lava = true; break; }
        }
        if(flame) { ptllc.flame++; ptllampcls[i] = 0; continue; }
        if(portal) { ptllc.portal++; ptllampcls[i] = 1; continue; }
        if(glow) { ptllc.glow++; ptllampcls[i] = 2; continue; }
        if(lava) { ptllc.lava++; ptllampcls[i] = 7; continue; }
        // A light fitting against it: model box, then light face.
        int best = -1, bestpanel = -1;
        float bestd = maxd;
        vec bc(0, 0, 0);
        float brxy = 1, brz = 1;
        loopvj(fitents)
        {
            vec c;
            float rxy, rz;
            float d = ptlmodelbox(*ents[fitents[j]], e.o, c, rxy, rz);
            if(d < bestd) { bestd = d; best = j; bc = c; brxy = rxy; brz = rz; }
        }
        loopvj(ptllumpanels)
        {
            float d = max(ptllumpanels[j].pos.dist(e.o) - sqrtf(ptllumpanels[j].area/float(M_PI)), 0.0f);
            if(d < bestd) { bestd = d; best = -1; bestpanel = j; }
        }
        // Then a window (only when no fitting is closer).
        int bestwin = -1;
        if(hwrtptlogicwindows) loopvj(ptlwinpanels)
        {
            float d = max(ptlwinpanels[j].pos.dist(e.o) - sqrtf(ptlwinpanels[j].area/float(M_PI)), 0.0f);
            if(d < bestd) { bestd = d; best = -1; bestpanel = -1; bestwin = j; }
        }
        // The lamp's light: power so its light at half its radius is what the mapper's lamp gave there.
        float R = float(e.attr1);
        vec lc = vec(e.attr2, e.attr3, e.attr4).div(255.0f);
        vec pw = vec(lc).mul(0.5f * 0.25f*R*R * max(hwrtptlogiclumpower, 0.0f));
        int cls = -1, fit = -1;
        float size = 2;
        if(best >= 0)
        {
            cls = 0; fit = best; ptllc.model++; ptllampcls[i] = 3;
            size = clamp(min(brxy, brz), 0.5f, 24.0f);
        }
        else if(bestpanel >= 0)
        {
            cls = 1; fit = bestpanel; ptllc.tex++; ptllampcls[i] = 4;
            // PT 5: the light texture lights by itself (pttex.inc): its lamp is not a source of its own.
            if(hwrtpttexframe()) continue;
            size = clamp(sqrtf(ptllumpanels[bestpanel].area/float(M_PI)), 0.5f, 24.0f);
        }
        else if(bestwin >= 0)
        {
            cls = 4; fit = bestwin; ptllc.window++; ptllampcls[i] = 5;
            size = clamp(sqrtf(ptlwinpanels[bestwin].area/float(M_PI)), 0.5f, 24.0f);
        }
        else
        {
            ptllc.none++; ptllampcls[i] = 6;
            if(!hwrtptlogicmixed) continue;
            cls = 3;
            size = max(float(hwrtlampsize), 1.0f);
        }
        if(!hwrtptlogiclum && cls != 3) continue;
        // Several lamps on one fitting (or window): one source, their powers added, where they stand on average.
        vector<int> &lit = cls == 0 ? fitlit : (cls == 4 ? winlit : panellit);
        if((cls <= 1 || cls == 4) && lit[fit] >= 0)
        {
            ptllum &s = ptllums[lit[fit]];
            int k = max(int(s.fitting), 1);
            s.pos.mul(float(k)).add(e.o).div(float(k + 1));
            s.col.add(pw);
            s.rmax = max(s.rmax, R);
            s.fitting = k + 1;
            continue;
        }
        ptllum &s = ptllums.add();
        s.pos = e.o; s.col = pw; s.size = size; s.cls = cls; s.fitting = 1; s.clear = -1; s.rmax = R;
        if(cls <= 1 || cls == 4) lit[fit] = ptllums.length() - 1;
    }
    // Fittings no lamp lights: from the list when it gives them a power.
    if(hwrtptlogiclum)
    {
        loopv(fitents) if(fitlit[i] < 0 && fitlist[i] && fitlist[i]->power > 0)
        {
            const ptlemissive &l = *fitlist[i];
            vec c;
            float rxy, rz;
            ptlmodelbox(*ents[fitents[i]], ents[fitents[i]]->o, c, rxy, rz);
            ptllum &s = ptllums.add();
            s.pos = vec(c).add(vec(0, 0, l.offz));
            s.col = vec(l.color).mul(l.power);
            s.size = l.size > 0 ? l.size : clamp(min(rxy, rz), 0.5f, 24.0f);
            s.cls = 2;
            s.fitting = 0;
            s.clear = -1;
            s.rmax = 0;
            ptllc.listsources++;
        }
        // PT 5: with the glowing textures lighting by themselves, the list's powers go to their lights (pttex.inc).
        if(!hwrtpttexframe()) loopv(ptllumpanels) if(panellit[i] < 0)
        {
            const ptllumpanel &p = ptllumpanels[i];
            bool fitting = false;
            Slot &sl = lookupslot(p.slot, false);
            const ptlemissive *l = sl.sts.empty() ? NULL : ptlemisfind(sl.sts[0].name, 1, fitting);
            if(!l || l->power <= 0) continue;
            ptllum &s = ptllums.add();
            float r = sqrtf(p.area/float(M_PI));
            s.pos = vec(p.pos).add(vec(p.n).mul(1.0f));
            s.col = vec(l->color).mul(l->power * p.area);
            s.size = l->size > 0 ? l->size : clamp(r, 0.5f, 24.0f);
            s.cls = 2;
            s.fitting = 0;
            s.clear = -1;
            s.rmax = 0;
            ptllc.listsources++;
        }
    }
    ptllc.sources = ptllums.length();
    logoutf("hwrt pt logical: %d map lamps: %d flame, %d teleporter, %d glow, %d lava, %d light model, %d light texture, %d window, %d no visible source; %d sources (%d from the list; %d fitting models, %d fitting faces, %d windows)",
            ptllc.lamps, ptllc.flame, ptllc.portal, ptllc.glow, ptllc.lava, ptllc.model, ptllc.tex, ptllc.window, ptllc.none, ptllc.sources, ptllc.listsources,
            fitents.length(), ptllumpanels.length(), ptllc.windows);
}

static void ptllumstatus()
{
    conoutf("hwrt pt logical lights: lum %d mixed %d dist %.0f power %.2f, %d lamps (flame %d, teleporter %d, glow %d, lava %d, model %d, texture %d, window %d, none %d), %d sources (%d from the list), list %d entries, %d windows",
            hwrtptlogiclum, hwrtptlogicmixed, hwrtptlogiclumdist, hwrtptlogiclumpower, ptllc.lamps, ptllc.flame, ptllc.portal, ptllc.glow, ptllc.lava, ptllc.model, ptllc.tex, ptllc.window,
            ptllc.none, ptllc.sources, ptllc.listsources, ptlemis.length(), ptllc.windows);
}
COMMANDN(hwrtptlumstatus, ptllumstatus, "");
