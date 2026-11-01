// Path tracing, flames that really light (hwrtptflames, trace.cpp; prototype, with the logical lighting).
// 0: the source of PT 2 (one per torch, still, its power from the flame's size, a made-up flicker of +/- 12 %).
// 1: the source follows the flame, every frame: its place (the centre of the flame's sprites), its size (their spread),
//    its power (the light they add up to on screen: opacity x texture x area) and its colour (the colour the sprites
//    show once added up and clipped, as on screen: a red tint reads amber at the core).
//    The sprites are those GL draws (renderparticles.cpp) while the flame is drawn. PT 4 (hwrtptflamehidden 1): a
//    flame GL does not draw this frame (out of view, hidden by the potentially visible set, further than
//    maxparticledistance) takes its light from a simulation of its sprites by the very law of renderparticles.cpp
//    regularflame (3 sprites at each particle emission, lifetimes drawn the same way, same size, opacity, rise): the
//    same light and flicker whether it is seen or not, a few thousand numbers a frame on the processor.
//    hwrtptflamehidden 0 (PT 3): a flame without sprites keeps its last light half a second, then PT 2's source.
// (PT 3's method 2, the sprites as emitting discs sampled per pixel, is gone: same light and cost as method 1, no
// visible gain in play, and it lost the reflections on the walls.)

static void ptflreset() { extern void hwrtptclearcache(); hwrtptclearcache(); }
// PT 7: 1 by default (decided with PT 3 and 4), saved.
VARFP(hwrtptflames, 0, 1, 1, ptflreset());
// Smoothing of the light of method 1 over time (ms): 0 the sprites of this frame alone.
VAR(hwrtptflamesmooth, 0, 50, 1000);
// A flame the map's lamps were set against takes their power (ptlogical.inc ptlfire mapperpow): the mapper's light,
// coming from the flame. 0: every flame's power from its own size (PT 2's calibration on triforts' torches).
VARF(hwrtptflamemapper, 0, 1, 1, ptflreset());
// PT 4: 1 a flame GL does not draw this frame lights from the simulation of its sprites; 0 PT 3 (last light, then still).
VARFP(hwrtptflamehidden, 0, 1, 1, ptflreset());

extern void hwrtcollectflamesprites();
extern Texture *hwrtflametexture();

enum { PTFL_GRIDW = 8, PTFL_GRIDH = 16, PTFL_VMAX = 256 };

// One simulated sprite (PT 4): spawn point, birth and lifetime (ms of game time), quarter of the texture.
struct ptflvpart { float x, y; int born, fade, quad; };

// One fire or flame emitter (particles 0 and 11) of the map.
struct ptflemitter
{
    int ent;          // entity index
    vec o;            // emitter position
    float r, h;       // radius and height (renderparticles.cpp makeparticles)
    vec tint;         // sprite colour, 0-1
    float expect;     // expected sum of the sprites' light (opacity x texture x area x luminance of the tint)
    // This frame's sprites.
    int n;
    double e, ex, ey, ez, exx;   // light, light-weighted position and squared distance
    double cr, cg, cb;           // tint x light (for the mean tint)
    // Clipped colour on screen: a small side view of the flame.
    float grid[PTFL_GRIDW*PTFL_GRIDH][3];
    float gx0, gz0, gdx, gdz;
    // Smoothed result and when it was last seen with sprites.
    bool have;
    vec pos, hue;
    float power, size;
    int seenmillis;
    float pscale;       // hwrtptflamemapper: the map's lamps' power over the sprites' expected one (1: none)
    vec mhue;           // hwrtptflamemapper: the map's lamps' colour (luminance 1), used when usemhue
    bool usemhue;
    // PT 4: birth of the youngest sprite GL drew this frame (the flame is being emitted when it is recent), and the
    // simulated sprites (ring of PTFL_VMAX in ptflvparts from vbase), the last simulated emission.
    int newest;
    int vbase, vfirst, vcount, vlastemit;
    bool simulated;
};
static vector<ptflemitter> ptflems;
static vector<ptflvpart> ptflvparts;
static hashtable<int, int> ptflcells;   // xy cell of 16 units -> first emitter of the chain in ptflnext
static vector<int> ptflnext;
static int ptflnents = -1, ptflepoch = -1;
static string ptflmap = "";
static float ptflmask[4] = { 0.3f, 0.3f, 0.3f, 0.3f };   // mean of each quarter of the flame texture
static float ptflcore[4] = { 0.8f, 0.8f, 0.8f, 0.8f };   // mean of its bright part (texels above half): the sprite's core
static bool ptflmaskdone = false;
static int ptflframe = 0, ptflnsprites = 0, ptflnmatched = 0, ptflndrawn = 0, ptflnsim = 0, ptflnvsprites = 0;
static uint ptflseed = 0x2545F491u;   // the simulation's own random numbers (the game's rnd() is left alone)
// Calibration: the light of PT 2's torch (triforts: radius 0.75, height 0.6, default colour) is the power its sprites
// add up to on average (expected value from its parameters): same overall brightness, the flame's own variations.
static float ptflk = 0;

static int ptflcell(float x, float y) { return (int(floorf(x/16)) & 0xFFFF) | (int(floorf(y/16)) << 16); }
static uint ptflrand() { ptflseed ^= ptflseed << 13; ptflseed ^= ptflseed >> 17; ptflseed ^= ptflseed << 5; return ptflseed; }
// Same draws as the game's rndscale(x) (0 to x) and rnd(n) (0 to n-1).
static float ptflrndscale(float x) { return float(ptflrand() & 0xFFFFFF) / float(0x1000000) * x; }
static int ptflrnd(int n) { return n > 0 ? int(ptflrand() % uint(n)) : 0; }

// Expected sum of a flame's sprites' light (renderparticles.cpp regularflame: 3 sprites every emitmillis, each
// living up to fade = 600 h ms (uniform), opacity 4 x (1 - age/fade) capped at 1 (mean 0.875 over a life), size
// 2 min(r, h), quad of side 2 size), times the texture's mean and the tint's luminance.
static float ptflexpected(float r, float h, const vec &tint)
{
    extern int emitmillis;
    float rate = 3000.0f / max(emitmillis, 1);
    float life = 0.5f * max(600.0f*h, 1.0f) / 1000.0f;
    float s = 2*min(r, h), area = 4*s*s;
    float mask = 0.25f*(ptflmask[0] + ptflmask[1] + ptflmask[2] + ptflmask[3]);
    return rate * life * 0.875f * mask * area * max(ptlluma(tint), 1e-3f);
}

// The flame texture's mean per quarter (PT_RND4: a sprite shows one quarter of it), once.
static void ptflmeasuremask()
{
    if(ptflmaskdone) return;
    ptflmaskdone = true;
    Texture *t = hwrtflametexture();
    if(!t || !t->id || t->w <= 0 || t->h <= 0) return;
    GLint prev2d = 0, packalign = 4, packrow = 0, packbuf = 0, activetex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev2d);
    glGetIntegerv(GL_PACK_ALIGNMENT, &packalign);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &packrow);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packbuf);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &activetex);
    glActiveTexture_(GL_TEXTURE0);
    if(glBindBuffer_) glBindBuffer_(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glBindTexture(GL_TEXTURE_2D, t->id);
    GLint w = 0, h = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
    if(w > 0 && h > 0 && w <= 4096 && h <= 4096)
    {
        uchar *px = new uchar[size_t(w)*size_t(h)*4];
        while(glGetError() != GL_NO_ERROR);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        if(glGetError() == GL_NO_ERROR)
        {
            double q[4] = { 0, 0, 0, 0 }, qc[4] = { 0, 0, 0, 0 };
            int cnt[4] = { 0, 0, 0, 0 }, cntc[4] = { 0, 0, 0, 0 };
            loopj(h) loopi(w)
            {
                // The quarter a sprite picks (renderparticles.cpp PT_RND4: flags bit 5 the half in u, bit 6 in v).
                int k = (i >= w/2 ? 1 : 0) | (j >= h/2 ? 2 : 0);
                const uchar *p = &px[(size_t(j)*size_t(w) + size_t(i))*4];
                // A grey texture keeps its value in red (and in every channel once swizzled): the largest one.
                double v = max(p[0], max(p[1], p[2])) / 255.0;
                q[k] += v;
                cnt[k]++;
                if(v > 0.5) { qc[k] += v; cntc[k]++; }
            }
            loopk(4) if(cnt[k]) ptflmask[k] = float(q[k]/cnt[k]);
            loopk(4) if(cntc[k]) ptflcore[k] = float(qc[k]/cntc[k]);
        }
        delete[] px;
    }
    glBindTexture(GL_TEXTURE_2D, prev2d);
    glPixelStorei(GL_PACK_ALIGNMENT, packalign);
    glPixelStorei(GL_PACK_ROW_LENGTH, packrow);
    if(glBindBuffer_) glBindBuffer_(GL_PIXEL_PACK_BUFFER, packbuf);
    glActiveTexture_(uint(activetex));
    logoutf("hwrt pt flames: flame texture %dx%d, mean of its quarters %.3f %.3f %.3f %.3f, their bright parts %.3f %.3f %.3f %.3f", w, h,
            ptflmask[0], ptflmask[1], ptflmask[2], ptflmask[3], ptflcore[0], ptflcore[1], ptflcore[2], ptflcore[3]);
}

// The map's fire and flame emitters, after a map change (the simulated sprites start again with them).
static void ptflgather()
{
    const char *map = game::getclientmap();
    if(!map) map = "";
    const vector<extentity *> &ents = entities::getents();
    if(ents.length() == ptflnents && hwrtshadeepoch == ptflepoch && !strcmp(map, ptflmap)) return;
    ptflnents = ents.length();
    ptflepoch = hwrtshadeepoch;
    copystring(ptflmap, map);
    ptflems.setsize(0);
    ptflvparts.setsize(0);
    ptflcells.clear();
    ptflnext.setsize(0);
    ptflmeasuremask();
    loopv(ents)
    {
        const extentity &e = *ents[i];
        if(e.type != ET_PARTICLES || (e.attr1 != 0 && e.attr1 != 11)) continue;
        float r = e.attr1 == 0 && !e.attr2 ? 1.5f : e.attr2/100.0f;
        float h = e.attr1 == 0 && !e.attr3 ? r/3 : e.attr3/100.0f;
        if(r <= 0 || h <= 0) continue;
        int c = (((e.attr4&0xF)<<4) | ((e.attr4&0xF0)<<8) | ((e.attr4&0xF00)<<12)) + 0x0F0F0F;
        if(e.attr1 == 0 && !e.attr4) c = 0x903020;
        ptflemitter &m = ptflems.add();
        memset((void *)&m, 0, sizeof(m));
        m.ent = i;
        m.o = e.o;
        m.r = r;
        m.h = h;
        m.tint = vec((c>>16)&255, (c>>8)&255, c&255).div(255.0f);
        m.pscale = 1;
        m.mhue = vec(1, 1, 1);
        m.usemhue = false;
        m.seenmillis = -100000;
        m.newest = -100000;
        m.vbase = ptflvparts.length();
        m.vfirst = m.vcount = 0;
        m.vlastemit = -100000;
        ptflvparts.pad(PTFL_VMAX);
        int k = ptflcell(e.o.x, e.o.y);
        int *head = ptflcells.access(k);
        ptflnext.add(head ? *head : -1);
        ptflcells[k] = ptflems.length() - 1;
    }
    loopv(ptflems) ptflems[i].expect = ptflexpected(ptflems[i].r, ptflems[i].h, ptflems[i].tint);
    // Calibration on PT 2's triforts torch (radius 0.75, height 0.6, the default colour of type 0).
    float ref = ptflexpected(0.75f, 0.6f, vec(0x90/255.0f, 0x30/255.0f, 0x20/255.0f));
    float pt2 = 0.20f * (64.0f*64.0f + 1.0f) * clamp(0.75f*0.6f/0.75f, 0.25f, 4.0f);
    ptflk = ref > 0 ? pt2 / ref : 0;
}

// One sprite (drawn by GL or simulated) added to its emitter's light this frame: where it is now, its size, opacity
// 0-255, colour and quarter of the texture.
static void ptflnote(ptflemitter &m, const vec &o, float size, int alpha, const vec &tint, int quad)
{
    float s = size, area = 4*s*s;
    float a = alpha/255.0f * ptflmask[quad&3];
    float e = a * area * max(ptlluma(tint), 1e-3f);
    if(!m.n)
    {
        m.e = m.ex = m.ey = m.ez = m.exx = m.cr = m.cg = m.cb = 0;
        memset(m.grid, 0, sizeof(m.grid));
        // Side view: the flame's width either side of its axis, from just under the emitter to where sprites rise.
        float rise = 30.0f*m.h*min(m.h, 1.0f) + 4*s;
        m.gx0 = -(m.r + 2*s); m.gdx = 2*(m.r + 2*s) / PTFL_GRIDW;
        m.gz0 = -2*s; m.gdz = (rise + 2*s) / PTFL_GRIDH;
    }
    m.n++;
    m.e += e;
    m.ex += e*o.x; m.ey += e*o.y; m.ez += e*o.z;
    m.exx += e*(o.x*o.x + o.y*o.y + o.z*o.z);
    m.cr += e*tint.x/max(ptlluma(tint), 1e-3f); m.cg += e*tint.y/max(ptlluma(tint), 1e-3f); m.cb += e*tint.z/max(ptlluma(tint), 1e-3f);
    // Additive sprite in the side view, for the colour the flame shows: its bright core (the middle half of its quad, at
    // the texture's bright part, not its mean) is what piles up and clips on screen (a red tint turns amber, then
    // yellow, where the sprites crowd).
    float ac = alpha/255.0f * ptflcore[quad&3], hs = 0.5f*s;
    float hx = (o.x - m.o.x + o.y - m.o.y) * 0.70710678f, hz = o.z - m.o.z;
    int x0 = clamp(int(floorf((hx - hs - m.gx0)/m.gdx)), 0, PTFL_GRIDW-1), x1 = clamp(int(floorf((hx + hs - m.gx0)/m.gdx)), 0, PTFL_GRIDW-1);
    int z0 = clamp(int(floorf((hz - hs - m.gz0)/m.gdz)), 0, PTFL_GRIDH-1), z1 = clamp(int(floorf((hz + hs - m.gz0)/m.gdz)), 0, PTFL_GRIDH-1);
    for(int zz = z0; zz <= z1; zz++) for(int xx = x0; xx <= x1; xx++)
    {
        float *g = m.grid[zz*PTFL_GRIDW + xx];
        g[0] += tint.x*ac; g[1] += tint.y*ac; g[2] += tint.z*ac;
    }
}

// renderparticles.cpp, for each live flame sprite GL draws (spawn point, birth, where it is now, size, opacity 0-255,
// colour, quarter).
void hwrtptnoteflame(const vec &spawn, int born, const vec &o, float size, int alpha, const bvec &color, int quad)
{
    ptflnsprites++;
    // Its emitter: same height as the spawn point, within the emitter's radius around it (regularflame).
    int best = -1;
    for(int dy = -1; dy <= 1 && best < 0; dy++) for(int dx = -1; dx <= 1 && best < 0; dx++)
    {
        int *head = ptflcells.access(ptflcell(spawn.x + dx*16, spawn.y + dy*16));
        for(int i = head ? *head : -1; i >= 0; i = ptflnext[i])
        {
            const ptflemitter &m = ptflems[i];
            if(fabs(spawn.z - m.o.z) > 0.05f || fabs(spawn.x - m.o.x) > m.r + 0.05f || fabs(spawn.y - m.o.y) > m.r + 0.05f) continue;
            best = i;
            break;
        }
    }
    if(best < 0) return;
    ptflnmatched++;
    ptflemitter &m = ptflems[best];
    m.newest = max(m.newest, born);
    ptflnote(m, o, size, alpha, vec(color.x/255.0f, color.y/255.0f, color.z/255.0f), quad);
}

// PT 4: the simulated sprites of emitter m (renderparticles.cpp regularflame and the quad renderer's calc / modifyblend
// for PART_FLAME): an emission (3 sprites) whenever the game would emit (emitmillis apart, at most one per frame), each
// spawned at a random point of the emitter's square, living 1 to 600 h ms, rising at min(1, h) x 200 / 5000 units per
// ms with the flame's gravity (-15), opacity 4 x (1 - age / life) capped at 1, size 2 min(r, h).
static void ptflsimulate(ptflemitter &m, bool note)
{
    extern int emitmillis;
    int em = max(int(emitmillis), 1);
    // Sprites past their life leave the ring.
    while(m.vcount > 0)
    {
        const ptflvpart &p = ptflvparts[m.vbase + m.vfirst];
        if(lastmillis - p.born < p.fade) break;
        m.vfirst = (m.vfirst + 1) % PTFL_VMAX;
        m.vcount--;
    }
    // As the game: one emission in a frame that crosses an emitmillis boundary (updateparticles canemit). A long gap
    // (map start, a pause, the first frame) refills the flame with the emissions of a whole lifetime, each with its own
    // birth, as the game's replay does for a flame that comes back into view.
    int life = max(int(600.0f*m.h), 1) + 1;
    int from = lastmillis, to = lastmillis;
    if(lastmillis - m.vlastemit > life + em) from = lastmillis - life;
    else if(lastmillis - m.vlastemit < em) from = to + 1;
    for(int when = from; when <= to; when += em)
    {
        loopi(3)
        {
            if(m.vcount >= PTFL_VMAX) { m.vfirst = (m.vfirst + 1) % PTFL_VMAX; m.vcount--; }
            ptflvpart &p = ptflvparts[m.vbase + (m.vfirst + m.vcount) % PTFL_VMAX];
            p.x = m.o.x + ptflrndscale(m.r*2.0f) - m.r;
            p.y = m.o.y + ptflrndscale(m.r*2.0f) - m.r;
            p.born = when;
            p.fade = ptflrnd(max(int(600.0f*m.h), 1)) + 1;
            p.quad = ptflrnd(4);
            m.vcount++;
        }
    }
    if(from <= to) m.vlastemit = lastmillis - lastmillis%em;
    if(!note) return;
    float size = 2.0f*min(m.r, m.h), vz = min(1.0f, m.h)*200.0f;
    loopi(m.vcount)
    {
        const ptflvpart &p = ptflvparts[m.vbase + (m.vfirst + i) % PTFL_VMAX];
        int ts = lastmillis - p.born;
        if(ts < 0) continue;
        int blend = p.fade <= 5 ? 255 : max(255 - (ts<<8)/p.fade, 0);
        if(blend <= 1) continue;
        blend = min(blend<<2, 255);
        float t = float(min(ts, p.fade));
        vec o(p.x, p.y, m.o.z + vz*t/5000.0f + t*t/(2.0f*5000.0f*15.0f));
        ptflnote(m, o, size, blend, m.tint, p.quad);
        ptflnvsprites++;
    }
}

// Before the lights are packed (ptlpack, every path traced frame with the logical lighting and hwrtptflames 1): this
// frame's sprites, then each emitter's light.
static void ptflcollect()
{
    ptflgather();
    ptflframe++;
    ptflnsprites = ptflnmatched = ptflndrawn = ptflnsim = ptflnvsprites = 0;
    loopv(ptflems) ptflems[i].n = 0;
    hwrtcollectflamesprites();
    extern int emitmillis;
    float dt = curtime > 0 ? float(curtime) : 16.0f;
    float keep = hwrtptflamesmooth > 0 ? expf(-dt / float(hwrtptflamesmooth)) : 0.0f;
    // A flame is being emitted when GL drew a sprite born within the last few emissions (a flame that left the view
    // still shows its last sprites while they die out: those are not its light any more).
    int recent = max(3*max(int(emitmillis), 1), 2*int(dt) + int(emitmillis));
    loopv(ptflems)
    {
        ptflemitter &m = ptflems[i];
        bool drawn = m.n > 0 && lastmillis - m.newest <= recent;
        m.simulated = false;
        if(hwrtptflamehidden)
        {
            if(drawn) { ptflsimulate(m, false); ptflndrawn++; }
            else
            {
                m.n = 0;
                ptflsimulate(m, true);
                m.simulated = true;
                ptflnsim++;
            }
        }
        if(m.n <= 0 || m.e <= 1e-6)
        {
            if(totalmillis - m.seenmillis > 500) m.have = false;
            continue;
        }
        vec c(float(m.ex/m.e), float(m.ey/m.e), float(m.ez/m.e));
        float var = float(m.exx/m.e) - c.squaredlen();
        float spread = sqrtf(max(var, 0.0f));
        // Colour on screen: each cell of the side view clipped at 1 per channel, as the additive sprites are.
        double vr = 0, vg = 0, vb = 0;
        loopj(PTFL_GRIDW*PTFL_GRIDH)
        {
            const float *g = m.grid[j];
            vr += min(g[0], 1.0f); vg += min(g[1], 1.0f); vb += min(g[2], 1.0f);
        }
        vec hue = vec(float(vr), float(vg), float(vb));
        float hl = ptlluma(hue);
        hue = hl > 1e-6f ? hue.div(hl) : vec(1, 0.65f, 0.36f).div(ptlluma(vec(1, 0.65f, 0.36f)));
        float power = float(m.e) * ptflk * m.pscale;
        if(!m.have || keep <= 0)
        {
            m.pos = c; m.hue = hue; m.power = power; m.size = spread;
        }
        else
        {
            m.pos.lerp(c, m.pos, keep);
            m.hue.lerp(hue, m.hue, keep);
            m.power = power + (m.power - power)*keep;
            m.size = spread + (m.size - spread)*keep;
        }
        m.have = true;
        m.seenmillis = totalmillis;
    }
}

// ptlogical.inc, before ptflcollect: the power and colour (luminance 1) of the map's lamps set against emitter ent
// (power 0: none, the flame's own power and colour).
static void ptflsetscale(int ent, float mapperpow, const vec &mapperhue)
{
    loopv(ptflems) if(ptflems[i].ent == ent)
    {
        ptflemitter &m = ptflems[i];
        m.pscale = mapperpow > 0 ? mapperpow / max(m.expect * ptflk, 1e-3f) : 1.0f;
        m.usemhue = mapperpow > 0;
        m.mhue = mapperhue;
        return;
    }
}

// The source of fire emitter `ent` this frame: place, colour (luminance 1), power, size. False: no sprite lately and
// no simulation (PT 2's source then).
static bool ptflsource(int ent, vec &pos, vec &hue, float &power, float &size, float &expect)
{
    loopv(ptflems)
    {
        const ptflemitter &m = ptflems[i];
        if(m.ent != ent) continue;
        if(!m.have) return false;
        pos = m.pos; hue = m.usemhue ? m.mhue : m.hue; power = m.power; size = m.size; expect = m.expect * ptflk * m.pscale;
        return true;
    }
    return false;
}

static void ptflstatus()
{
    int seen = 0;
    float pmin = 1e9f, pmax = 0, ptot = 0;
    loopv(ptflems) if(ptflems[i].have) { seen++; pmin = min(pmin, ptflems[i].power); pmax = max(pmax, ptflems[i].power); ptot += ptflems[i].power; }
    conoutf("hwrt pt flames: method %d, hidden %d, %d emitters, %d lit (%d drawn by GL, %d simulated), %d sprites this frame (%d matched, %d simulated), power %.0f-%.0f (mean %.0f), calibration %.1f, smooth %d ms",
            hwrtptflames, hwrtptflamehidden, ptflems.length(), seen, ptflndrawn, ptflnsim, ptflnsprites, ptflnmatched, ptflnvsprites,
            seen ? pmin : 0.0f, pmax, seen ? ptot/seen : 0.0f, ptflk, hwrtptflamesmooth);
}
COMMANDN(hwrtptflamestatus, ptflstatus, "");
