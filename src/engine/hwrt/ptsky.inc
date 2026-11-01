// Path tracing, the sky as a light (hwrtptsky, trace.cpp; prototype). The sky GL draws (texture.cpp hwrtskyenvtex: the
// cubemap rendered once per map with the skybox, its colour, yaw, clouds and atmosphere), read back when the path
// tracing first needs it after a map or sky change, becomes a map of radiance in the world's light units (display
// values: a white wall under a uniform sky shows that sky's brightness), 128 x 64 directions (latitude-longitude, z up).
// The sun the map already lights with (sunlight, sunlightdir) is masked out of it (no double count of a sun painted in
// the skybox), and the tables to draw a direction in proportion to its light (importance sampling) go with it, plus
// the mean colour of the sky above the horizon seen from each normal direction (the light a surface gets from the part
// of the sky it sees, times the share of its bounce rays that escaped). hitlight.comp reads it (set 1 binding 4).
// Nothing of it runs or exists unless path tracing runs with hwrtptsky 1.

extern int sunlight, skylight;
// PT 7: 1 by default (decided with PT 3); the sky's light is switched off with hwrtptskyscale 0.
VARF(hwrtptsky, 0, 1, 1, hwrtptreset());
// 0: the sky's radiance as drawn (display values); 1: scaled so an open floor gets the light the map's sky gave
// (skylight x world gain), where the map has a sky light; times hwrtptskyscale in both cases.
static void ptskydirty();
// PT 7: 1 by default (decided with PT 3); the scale is saved (0: no light from the sky).
VARF(hwrtptskynorm, 0, 1, 1, ptskydirty());
FVARFP(hwrtptskyscale, 0, 1, 8, ptskydirty());
// PT 4, the sky's strength from the sun (hwrtptskyforce 1, before hwrtptskynorm): on a map with a sun (sunlight, or an
// old-map radius-0 lamp), an open floor under the whole sky gets hwrtptskyratio of the light a surface facing the sun
// gets, in physical light (a clear day, ASHRAE's clear-sky model: the sky's light on the ground is 10 to 14 % of the
// sun's beam on a surface facing it, more under haze or thin cloud). The skybox still gives the colour and where the
// light comes from. A map without a sun keeps hwrtptskynorm's rule (the map's sky light). Proposed: 0.3, a hazy sky
// (an open floor in the shade shows about half the sun's light; the canon's mappers chose 0.27 to 0.48); 0.13 a clear sky.
// PT 7: 1 by default (decided with PT 4, 0.3), both saved.
VARFP(hwrtptskyforce, 0, 1, 1, ptskydirty());
FVARFP(hwrtptskyratio, 0, 0.3f, 1, ptskydirty());
// PT 5: the sky's strength for one map. hwrtptskyratio is the player's own (every map); hwrtptskymap is this map's
// (-1: none, hwrtptskyratio applies), kept in ptskymaps.cfg in the player's folder and set again when that map loads:
// a player who sets it in a game finds it again on that map.
static void ptskymapchanged();
FVARF(hwrtptskymap, -1, -1, 1, ptskymapchanged());
struct ptskymapent { string map; float ratio; };
static vector<ptskymapent> ptskymaps;
static bool ptskymapsloaded = false;
static string ptskymapcur = "";
static void ptskymapput(const char *map, float r)
{
    loopv(ptskymaps) if(!strcmp(ptskymaps[i].map, map))
    {
        if(r < 0) ptskymaps.remove(i); else ptskymaps[i].ratio = r;
        return;
    }
    if(r < 0) return;
    ptskymapent &e = ptskymaps.add();
    copystring(e.map, map);
    e.ratio = r;
}
ICOMMAND(ptskymapentry, "sf", (char *m, float *r), { if(m && m[0]) ptskymapput(m, clamp(*r, 0.0f, 1.0f)); });
static void ptskymapsload()
{
    if(ptskymapsloaded) return;
    ptskymapsloaded = true;
    execfile("ptskymaps.cfg", false);
}
static void ptskymapswrite()
{
    stream *f = openutf8file(path("ptskymaps.cfg", true), "w");
    if(!f) return;
    f->printf("// sky strength kept per map (path tracing), written by the game\n");
    loopv(ptskymaps) f->printf("ptskymapentry %s %.3f\n", escapestring(ptskymaps[i].map), ptskymaps[i].ratio);
    delete f;
}
// The player set this map's value (from the menu): kept, or forgotten (-1).
static void ptskymapchanged()
{
    const char *map = game::getclientmap();
    if(map && map[0])
    {
        ptskymapsload();
        ptskymapput(map, hwrtptskymap);
        ptskymapswrite();
        copystring(ptskymapcur, map);
    }
    ptskydirty();
}
// When a map loads: hwrtptskymap takes its kept value (or -1), without writing anything.
static void ptskymapapply()
{
    const char *map = game::getclientmap();
    if(!map) map = "";
    if(!strcmp(map, ptskymapcur)) return;
    copystring(ptskymapcur, map);
    ptskymapsload();
    float r = -1;
    loopv(ptskymaps) if(!strcmp(ptskymaps[i].map, map)) r = ptskymaps[i].ratio;
    if(r != hwrtptskymap) { hwrtptskymap = r; ptskydirty(); }
}
// How the sky adds to the direct light (sun, lamps): hwrtptcombine (0 the larger of the two, as the maps were baked;
// 1 their sum), as for the map's sky.
// Importance sampling: one more ray per cache update (and per pixel with hwrtptskypixel 1) drawn in proportion to the
// sky's light, weighed against the cosine ray (multiple importance sampling, balance heuristic).
// PT 7: saved (menu: "Extra ray toward the bright sky"; its default waits for a try with and without).
VARFP(hwrtptskyis, 0, 1, 1, hwrtptreset());
// The sun's disc and halo masked out of the sky around sunlightdir (degrees of radius; 0 keeps the painted sun).
FVARF(hwrtptskysunmask, 0, 10, 45, ptskydirty());
// The skybox's own sun is often painted elsewhere than the map's sun (autumn: same side, 19 degrees lower), or the map's
// sun is an old-map radius-0 lamp (academy, gubo): when the map has a sun of either kind, the brightest spot of the sky
// (a texel far above the sky's median) is masked out the same way.
VARF(hwrtptskypeak, 0, 1, 1, ptskydirty());
// What a pixel takes from the sky it sees: 0 the share of its bounce rays that escaped times the mean colour of the
// sky above the horizon on its side (steady, no extra ray); 1 the radiance of the very directions that escaped, with
// the importance-sampled ray, through the bounced light's denoiser (exact, one more ray, grainier; always added).
// PT 6: 1 is the default: with 0 the escaped share is kept over frames by the ray tracing's own sky filter, not by the
// denoiser, and starts again from one ray on every pixel that comes into view (bright specks while turning).
VARFP(hwrtptskypixel, 0, 1, 1, hwrtptreset());
// PT 5: 1 the sky's light keeps the skybox's brightness, direction by direction, but takes the colour of the map's sky
// light (skylight, as the lightmaps show it): a warm shade where the mapper chose a warm sky light under a blue skybox.
// 0: the skybox's own colours (PT 3). A map without a sky light keeps the skybox's. PT 6: 1 is the default (decision of
// 26/10).
VARFP(hwrtptskyhue, 0, 1, 1, ptskydirty());

#define loopc(m) loop(c, m)
enum { PTSKY_W = 128, PTSKY_H = 64, PTSKY_NW = 32, PTSKY_NH = 16 };
// Layout of the buffer, in vec4: 4 header, PTSKY_W*PTSKY_H radiance (rgb, a = probability of the texel), the
// conditional CDF of each row (PTSKY_W*PTSKY_H floats, 4 per vec4), the marginal CDF of the rows (PTSKY_H floats), the
// mean sky colour by normal (PTSKY_NW*PTSKY_NH), then (PT 5) the same mean in physical light (hwrtptcombine 3).
enum
{
    PTSKY_RAD = 4,
    PTSKY_COND = PTSKY_RAD + PTSKY_W*PTSKY_H,
    PTSKY_MARG = PTSKY_COND + PTSKY_W*PTSKY_H/4,
    PTSKY_NTAB = PTSKY_MARG + PTSKY_H/4,
    PTSKY_NTAB2 = PTSKY_NTAB + PTSKY_NW*PTSKY_NH,
    PTSKY_VEC4 = PTSKY_NTAB2 + PTSKY_NW*PTSKY_NH
};
static const VkDeviceSize PTSKY_BYTES = VkDeviceSize(PTSKY_VEC4) * 16;

struct ptskyinfo
{
    GLuint gltex;
    int epoch, skylightv, sunlightv, built, ok;
    float scale, sunmask, sunx, suny, sunz;
    int norm;
    // Diagnostics (hwrtptskystatus): mean radiance above the horizon, the light of an open floor before and after
    // scaling, the share of the sky's light the sun mask removed, the brightest direction left.
    float meanup, floorphys, floorfinal, masked, peak, peakyaw, peakpitch, k;
    int face;
    float peakmasked;
    int dark;   // 1: the picture is far darker than the map's sky (hwrtptskynorm 1): the map's sky colour is used
    // PT 4: 1 the strength comes from the sun (hwrtptskyforce), and that sun as a surface facing it shows it.
    int forced;
    float sunref;
};
static ptskyinfo ptskyi = { 0, -1, -1, -1, 0, 0, -1, -1, 0, 0, 0, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static float *ptskydata = NULL;   // CPU copy of the buffer, PTSKY_VEC4 * 4 floats
static void ptskydirty() { ptskyi.built = 0; hwrtptreset(); }
static void ptskyforget() { ptskyi.built = 0; ptskyi.ok = 0; ptskyi.gltex = 0; }

static float ptskylum(float r, float g, float b) { return 0.2126f*r + 0.7152f*g + 0.0722f*b; }

// The GL cubemap face and texel of a world direction (OpenGL cube map selection; the ray tracing samples the same
// cubemap with world directions, binding 27).
static void ptskycubetexel(const vec &d, int &face, float &s, float &t)
{
    float ax = fabs(d.x), ay = fabs(d.y), az = fabs(d.z), sc, tc, ma;
    if(ax >= ay && ax >= az) { ma = ax; if(d.x > 0) { face = 0; sc = -d.z; tc = -d.y; } else { face = 1; sc = d.z; tc = -d.y; } }
    else if(ay >= az) { ma = ay; if(d.y > 0) { face = 2; sc = d.x; tc = d.z; } else { face = 3; sc = d.x; tc = -d.z; } }
    else { ma = az; if(d.z > 0) { face = 4; sc = d.x; tc = -d.y; } else { face = 5; sc = -d.x; tc = -d.y; } }
    s = 0.5f*(sc/ma + 1.0f);
    t = 0.5f*(tc/ma + 1.0f);
}

static vec ptskydir(float u, float v)
{
    float phi = u * 2.0f*float(M_PI), th = v * float(M_PI);
    return vec(sinf(th)*cosf(phi), sinf(th)*sinf(phi), cosf(th));
}

// Reads the sky cubemap back (GL thread, the frame's own GL context) and builds the radiance map. False: no sky.
static bool ptskybuild(GLuint gltex)
{
    if(!ptskydata) ptskydata = new float[PTSKY_VEC4*4];
    memset(ptskydata, 0, sizeof(float)*PTSKY_VEC4*4);
    GLint prevcube = 0, packalign = 4, packrow = 0, packbuf = 0, activetex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP, &prevcube);
    glGetIntegerv(GL_PACK_ALIGNMENT, &packalign);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &packrow);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packbuf);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &activetex);
    glActiveTexture_(GL_TEXTURE0);
    if(glBindBuffer_) glBindBuffer_(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, gltex);
    GLint tw = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_CUBE_MAP_POSITIVE_X, 0, GL_TEXTURE_WIDTH, &tw);
    uchar *faces = NULL;
    bool ok = tw > 0 && tw <= 4096;
    if(ok)
    {
        faces = new uchar[size_t(tw)*size_t(tw)*4*6];
        loopi(6)
        {
            while(glGetError() != GL_NO_ERROR);
            glGetTexImage(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, GL_RGBA, GL_UNSIGNED_BYTE, faces + size_t(i)*size_t(tw)*size_t(tw)*4);
            if(glGetError() != GL_NO_ERROR) { ok = false; break; }
        }
    }
    glBindTexture(GL_TEXTURE_CUBE_MAP, prevcube);
    glPixelStorei(GL_PACK_ALIGNMENT, packalign);
    glPixelStorei(GL_PACK_ROW_LENGTH, packrow);
    if(glBindBuffer_) glBindBuffer_(GL_PIXEL_PACK_BUFFER, packbuf);
    glActiveTexture_(uint(activetex));
    if(!ok) { delete[] faces; return false; }

    // Radiance: 4x4 cube samples per latitude-longitude texel (display values, 0-1).
    static float rad[PTSKY_W*PTSKY_H][3];
    loopj(PTSKY_H) loopi(PTSKY_W)
    {
        float acc[3] = { 0, 0, 0 };
        loopk(4) loopl(4)
        {
            vec d = ptskydir((i + (l + 0.5f)/4.0f)/PTSKY_W, (j + (k + 0.5f)/4.0f)/PTSKY_H);
            int f;
            float s, t;
            ptskycubetexel(d, f, s, t);
            int x = clamp(int(s*tw), 0, tw-1), y = clamp(int(t*tw), 0, tw-1);
            const uchar *px = faces + (size_t(f)*size_t(tw)*size_t(tw) + size_t(y)*size_t(tw) + size_t(x))*4;
            loopc(3) acc[c] += px[c];
        }
        loopc(3) rad[j*PTSKY_W + i][c] = acc[c] / (16.0f*255.0f);
    }
    delete[] faces;

    // The painted sun: the texels within hwrtptskysunmask degrees of the map's sun take the mean of the ring around it
    // (as wide again), so the sun lights once, through sunlight.
    float total0 = 0, removed = 0;
    loopj(PTSKY_H) loopi(PTSKY_W)
    {
        float th = (j + 0.5f)/PTSKY_H*float(M_PI);
        total0 += ptskylum(rad[j*PTSKY_W+i][0], rad[j*PTSKY_W+i][1], rad[j*PTSKY_W+i][2]) * sinf(th);
    }
    ptskyi.masked = 0;
    if(sunlight && hwrtptskysunmask > 0)
    {
        vec sd = sunlightdir;
        if(sd.magnitude() > 1e-4f)
        {
            sd.normalize();
            float c0 = cosf(hwrtptskysunmask*RAD), c1 = cosf(min(2*hwrtptskysunmask, 89.0f)*RAD);
            float ring[3] = { 0, 0, 0 }, rw = 0;
            loopj(PTSKY_H) loopi(PTSKY_W)
            {
                vec d = ptskydir((i + 0.5f)/PTSKY_W, (j + 0.5f)/PTSKY_H);
                float cs = d.dot(sd);
                if(cs <= c0 && cs > c1)
                {
                    float w = sinf((j + 0.5f)/PTSKY_H*float(M_PI));
                    ring[0] += rad[j*PTSKY_W+i][0]*w; ring[1] += rad[j*PTSKY_W+i][1]*w; ring[2] += rad[j*PTSKY_W+i][2]*w;
                    rw += w;
                }
            }
            if(rw > 0) loopc(3) ring[c] /= rw;
            loopj(PTSKY_H) loopi(PTSKY_W)
            {
                vec d = ptskydir((i + 0.5f)/PTSKY_W, (j + 0.5f)/PTSKY_H);
                if(d.dot(sd) <= c0) continue;
                float *r = rad[j*PTSKY_W+i];
                float th = (j + 0.5f)/PTSKY_H*float(M_PI);
                float l0 = ptskylum(r[0], r[1], r[2]);
                // Only what is brighter than the ring goes (a dark sky around a dark sun disc is left as it is).
                float lr = ptskylum(ring[0], ring[1], ring[2]);
                if(l0 > lr)
                {
                    removed += (l0 - lr) * sinf(th);
                    loopc(3) r[c] = ring[c];
                }
            }
        }
    }
    // The painted sun: with a sun of the map (sunlight or a radius-0 lamp), the brightest texel left, when it is far above
    // the sky's median (4 times, and at least 0.8), gets the same treatment around it.
    ptskyi.peakmasked = 0;
    bool mapsun = sunlight != 0;
    if(!mapsun)
    {
        const vector<extentity *> &ents = entities::getents();
        loopv(ents) if(ents[i]->type == ET_LIGHT && ents[i]->attr1 == 0) { mapsun = true; break; }
    }
    if(hwrtptskypeak && mapsun && hwrtptskysunmask > 0)
    {
        static float lums[PTSKY_W*PTSKY_H];
        int bi = 0;
        loopi(PTSKY_W*PTSKY_H) { lums[i] = ptskylum(rad[i][0], rad[i][1], rad[i][2]); if(lums[i] > lums[bi]) bi = i; }
        float peak = lums[bi];
        vector<float> sorted;
        loopi(PTSKY_W*PTSKY_H) sorted.add(lums[i]);
        sorted.sort();
        float median = sorted[sorted.length()/2];
        if(peak >= 0.8f && peak > 4*median)
        {
            vec pd = ptskydir((bi%PTSKY_W + 0.5f)/PTSKY_W, (bi/PTSKY_W + 0.5f)/PTSKY_H);
            float c0 = cosf(hwrtptskysunmask*RAD), c1 = cosf(min(2*hwrtptskysunmask, 89.0f)*RAD);
            float ring[3] = { 0, 0, 0 }, rw = 0;
            loopj(PTSKY_H) loopi(PTSKY_W)
            {
                vec d = ptskydir((i + 0.5f)/PTSKY_W, (j + 0.5f)/PTSKY_H);
                float cs = d.dot(pd);
                if(cs <= c0 && cs > c1)
                {
                    float w = sinf((j + 0.5f)/PTSKY_H*float(M_PI));
                    ring[0] += rad[j*PTSKY_W+i][0]*w; ring[1] += rad[j*PTSKY_W+i][1]*w; ring[2] += rad[j*PTSKY_W+i][2]*w;
                    rw += w;
                }
            }
            if(rw > 0) loopc(3) ring[c] /= rw;
            float lr = ptskylum(ring[0], ring[1], ring[2]);
            loopj(PTSKY_H) loopi(PTSKY_W)
            {
                vec d = ptskydir((i + 0.5f)/PTSKY_W, (j + 0.5f)/PTSKY_H);
                if(d.dot(pd) <= c0) continue;
                float *r = rad[j*PTSKY_W+i];
                float l0 = ptskylum(r[0], r[1], r[2]);
                if(l0 > lr)
                {
                    float sr = (l0 - lr) * sinf((j + 0.5f)/PTSKY_H*float(M_PI));
                    removed += sr;
                    ptskyi.peakmasked += sr;
                    loopc(3) r[c] = ring[c];
                }
            }
            if(total0 > 0) ptskyi.peakmasked /= total0;
        }
    }
    ptskyi.masked = total0 > 0 ? removed / total0 : 0;

    // PT 5 (hwrtptskyhue): the map's sky colour at the skybox's brightness.
    if(hwrtptskyhue && skylight)
    {
        float sc[3] = { skylightcolor.x/255.0f, skylightcolor.y/255.0f, skylightcolor.z/255.0f };
        float sl = ptskylum(sc[0], sc[1], sc[2]);
        if(sl > 1e-4f) loopi(PTSKY_W*PTSKY_H)
        {
            float l = ptskylum(rad[i][0], rad[i][1], rad[i][2]);
            loopc(3) rad[i][c] = l * sc[c] / sl;
        }
    }
    // Mean radiance above the horizon and the light of an open floor (cosine-weighted), before scaling.
    float up[3] = { 0, 0, 0 }, upw = 0, fl[3] = { 0, 0, 0 }, flw = 0, peak = -1;
    int peaki = 0;
    loopj(PTSKY_H) loopi(PTSKY_W)
    {
        float th = (j + 0.5f)/PTSKY_H*float(M_PI), st = sinf(th), ct = cosf(th);
        const float *r = rad[j*PTSKY_W+i];
        float l = ptskylum(r[0], r[1], r[2]);
        if(l > peak) { peak = l; peaki = j*PTSKY_W+i; }
        if(ct <= 0) continue;
        loopc(3) { up[c] += r[c]*st; fl[c] += r[c]*st*ct; }
        upw += st;
        flw += st*ct;
    }
    if(upw > 0) loopc(3) up[c] /= upw;
    if(flw > 0) loopc(3) fl[c] /= flw;
    ptskyi.meanup = ptskylum(up[0], up[1], up[2]);
    ptskyi.floorphys = ptskylum(fl[0], fl[1], fl[2]);
    ptskyi.peak = peak;
    {
        vec d = ptskydir((peaki%PTSKY_W + 0.5f)/PTSKY_W, (peaki/PTSKY_W + 0.5f)/PTSKY_H);
        ptskyi.peakyaw = atan2f(d.y, d.x)/RAD;
        ptskyi.peakpitch = asinf(clamp(d.z, -1.0f, 1.0f))/RAD;
    }
    // Scale: hwrtptskyscale, and with hwrtptskynorm 1 an open floor gets the map's sky light (skylight x world gain).
    extern float hwrtworldgain;
    float gain = max(hwrtworldgain, 0.0f);
    float k = 1;
    ptskyi.forced = 0;
    ptskyi.sunref = 0;
    // PT 4, hwrtptskyforce 1: the map's sun, as a surface facing it shows it (display values after the world gain, the
    // 8-bit clip of the direct light kept): sunlight's colour and scale, else the brightest radius-0 lamp.
    if(hwrtptskyforce)
    {
        float sun = 0;
        if(sunlight) sun = ptskylum(sunlightcolor.x/255.0f, sunlightcolor.y/255.0f, sunlightcolor.z/255.0f) * sunlightscale;
        else
        {
            const vector<extentity *> &ents = entities::getents();
            loopv(ents) if(ents[i]->type == ET_LIGHT && ents[i]->attr1 == 0)
                sun = max(sun, ptskylum(ents[i]->attr2/255.0f, ents[i]->attr3/255.0f, ents[i]->attr4/255.0f));
        }
        ptskyi.sunref = min(sun, 1.0f) * gain;
    }
    if(hwrtptskyforce && ptskyi.sunref > 0 && ptskyi.floorphys > 1e-4f)
    {
        // The open floor's light, in display values: (ratio x sun^2.2)^(1/2.2).
        // PT 5: this map's own strength when it has one.
        float ratio = hwrtptskymap >= 0 ? hwrtptskymap : hwrtptskyratio;
        float target = powf(clamp(ratio, 0.0f, 1.0f) * powf(ptskyi.sunref, 2.2f), 1.0f/2.2f);
        k = target / ptskyi.floorphys;
        ptskyi.forced = 1;
    }
    else if(hwrtptskynorm && skylight && ptskyi.floorphys > 1e-4f)
        k = ptskylum(skylightcolor.x/255.0f, skylightcolor.y/255.0f, skylightcolor.z/255.0f) * gain / ptskyi.floorphys;
    // A picture far darker than the map's sky (an almost black sky box over a map with a lit sky: face-capture, an
    // open floor of 0.001 as drawn, 400 times too dark) has no colour or direction worth scaling: the map's own sky
    // colour stays, as in the PT 2. The canon's other maps need 1.6 to 6 times.
    if(ptskyi.forced) ptskyi.dark = ptskyi.floorphys < 0.01f ? 1 : 0;
    else ptskyi.dark = hwrtptskynorm && skylight && (ptskyi.floorphys < 0.01f || k > 16.0f) ? 1 : 0;
    k *= hwrtptskyscale;
    ptskyi.k = k;
    ptskyi.floorfinal = ptskyi.floorphys * k;
    // hwrtptskyscale 0: no sky light at all (the essai's "sky off"), whatever the picture.
    if(ptskyi.dark && hwrtptskyscale > 0) return false;

    // Radiance with the scale, and the probability of each texel: its light (the sky below the horizon at a quarter of
    // its weight, a floor of 2 % of the mean so no direction is impossible) times its solid angle.
    float *v4 = ptskydata;
    double S = 0;
    static float wt[PTSKY_W*PTSKY_H];
    float meanl = max(ptskyi.meanup, 1e-4f);
    loopj(PTSKY_H) loopi(PTSKY_W)
    {
        float th = (j + 0.5f)/PTSKY_H*float(M_PI), st = sinf(th);
        const float *r = rad[j*PTSKY_W+i];
        float l = ptskylum(r[0], r[1], r[2]) + 0.02f*meanl;
        if(cosf(th) < 0) l *= 0.25f;
        wt[j*PTSKY_W+i] = l * st;
        S += wt[j*PTSKY_W+i];
    }
    if(S <= 0) S = 1;
    loopj(PTSKY_H) loopi(PTSKY_W)
    {
        int t = j*PTSKY_W + i;
        float *o = &v4[(PTSKY_RAD + t)*4];
        loopc(3) o[c] = rad[t][c]*k;
        o[3] = float(wt[t]/S);
    }
    // Conditional CDF of each row, marginal CDF of the rows.
    float *cond = &v4[PTSKY_COND*4], *marg = &v4[PTSKY_MARG*4];
    double rowsum[PTSKY_H];
    double acc = 0;
    loopj(PTSKY_H)
    {
        double rs = 0;
        loopi(PTSKY_W) rs += wt[j*PTSKY_W+i];
        rowsum[j] = rs;
        double a = 0;
        loopi(PTSKY_W)
        {
            a += wt[j*PTSKY_W+i];
            cond[j*PTSKY_W+i] = rs > 0 ? float(a/rs) : float(i+1)/PTSKY_W;
        }
        cond[j*PTSKY_W + PTSKY_W-1] = 1.0f;
        acc += rs;
        marg[j] = float(acc/S);
    }
    marg[PTSKY_H-1] = 1.0f;
    (void)rowsum;
    // Mean colour of the sky above the horizon seen from each normal direction (cosine-weighted); a normal that sees
    // none of it (a ceiling) takes its whole hemisphere.
    float *ntab = &v4[PTSKY_NTAB*4];
    loopj(PTSKY_NH) loopi(PTSKY_NW)
    {
        vec n = ptskydir((i + 0.5f)/PTSKY_NW, (j + 0.5f)/PTSKY_NH);
        double num[3] = { 0, 0, 0 }, den = 0, num2[3] = { 0, 0, 0 }, den2 = 0, lin[3] = { 0, 0, 0 }, lin2[3] = { 0, 0, 0 };
        for(int jj = 0; jj < PTSKY_H; jj += 2) for(int ii = 0; ii < PTSKY_W; ii += 2)
        {
            vec d = ptskydir((ii + 1.0f)/PTSKY_W, (jj + 1.0f)/PTSKY_H);
            float cs = d.dot(n);
            if(cs <= 0) continue;
            float st = sinf((jj + 1.0f)/PTSKY_H*float(M_PI));
            const float *r = rad[jj*PTSKY_W+ii];
            double w = cs*st;
            if(d.z > 0) { num[0] += r[0]*w; num[1] += r[1]*w; num[2] += r[2]*w; den += w; loopc(3) lin[c] += pow(double(r[c]), 2.2)*w; }
            num2[0] += r[0]*w; num2[1] += r[1]*w; num2[2] += r[2]*w; den2 += w;
            loopc(3) lin2[c] += pow(double(r[c]), 2.2)*w;
        }
        float *o = &ntab[(j*PTSKY_NW + i)*4];
        if(den > 1e-6) loopc(3) o[c] = float(num[c]/den)*k;
        else if(den2 > 1e-6) loopc(3) o[c] = float(num2[c]/den2)*k;
        o[3] = 0;
        // PT 5: the physical light of that sky ((radiance x k)^2.2, averaged): a surface seeing a share f of it gets f times
        // this (hwrtptcombine 3).
        float *o2 = &v4[(PTSKY_NTAB2 + j*PTSKY_NW + i)*4];
        float k22 = powf(max(k, 0.0f), 2.2f);
        if(den > 1e-6) loopc(3) o2[c] = float(lin[c]/den)*k22;
        else if(den2 > 1e-6) loopc(3) o2[c] = float(lin2[c]/den2)*k22;
        o2[3] = 0;
    }
    // Header.
    v4[0] = PTSKY_W; v4[1] = PTSKY_H; v4[2] = PTSKY_NW; v4[3] = PTSKY_NH;
    v4[4] = 1.0f; v4[5] = k; v4[6] = ptskyi.floorfinal; v4[7] = ptskyi.masked;
    return true;
}

// Before the lighting pass of a path traced frame (ptprepare): (re)build the sky after a map or sky change, then copy
// it to the buffer. false: no sky picture (hitlight keeps the map's sky colour).
static bool ptskyupdate(void *mapped)
{
    if(!hwrtptsky || !mapped) return false;
    // PT 5: a map just loaded takes its own sky strength, if the player kept one.
    ptskymapapply();
    extern GLuint hwrtskyenvtex();
    GLuint tex = hwrtskyenvtex();
    if(!tex) return false;
    vec sd = sunlightdir;
    if(ptskyi.built && ptskyi.gltex == tex && ptskyi.epoch == hwrtshadeepoch && ptskyi.skylightv == skylight &&
       ptskyi.sunlightv == sunlight && ptskyi.sunx == sd.x && ptskyi.suny == sd.y && ptskyi.sunz == sd.z)
        return ptskyi.ok != 0;
    ptskyi.gltex = tex;
    ptskyi.epoch = hwrtshadeepoch;
    ptskyi.skylightv = skylight;
    ptskyi.sunlightv = sunlight;
    ptskyi.sunx = sd.x; ptskyi.suny = sd.y; ptskyi.sunz = sd.z;
    ptskyi.built = 1;
    ptskyi.dark = 0;
    ptskyi.ok = ptskybuild(tex) ? 1 : 0;
    if(!ptskyi.ok)
    {
        if(ptskyi.dark)
            logoutf("hwrt pt sky: the sky picture is too dark for the map's sky light (open floor as drawn %.3f, x %.0f needed), the map's sky colour is used",
                    ptskyi.floorphys, ptskyi.k);
        else conoutf(CON_WARN, "hwrt pt: sky cubemap could not be read, the map's sky colour is used");
        return false;
    }
    memcpy(mapped, ptskydata, size_t(PTSKY_BYTES));
    logoutf("hwrt pt sky: open floor %.3f (as drawn %.3f, x %.2f), mean above the horizon %.3f, sun mask %.1f %% of the sky's light (painted sun %.1f %%), brightest %.2f at yaw %.0f pitch %.0f",
            ptskyi.floorfinal, ptskyi.floorphys, ptskyi.k, ptskyi.meanup, ptskyi.masked*100, ptskyi.peakmasked*100, ptskyi.peak, ptskyi.peakyaw, ptskyi.peakpitch);
    return true;
}

ICOMMAND(hwrtptskystatus, "", (),
{
    conoutf("hwrt pt sky: on %d, built %d ok %d (too dark %d), norm %d scale %.2f combine %d is %d pixel %d sunmask %.1f, open floor %.3f (as drawn %.3f, x %.3f), mean above the horizon %.3f, masked %.1f %%, brightest %.2f yaw %.0f pitch %.0f, map skylight %d %d %d sun %d",
            hwrtptsky, ptskyi.built, ptskyi.ok, ptskyi.dark, hwrtptskynorm, hwrtptskyscale, hwrtptcombine, hwrtptskyis, hwrtptskypixel, hwrtptskysunmask,
            ptskyi.floorfinal, ptskyi.floorphys, ptskyi.k, ptskyi.meanup, ptskyi.masked*100, ptskyi.peak, ptskyi.peakyaw, ptskyi.peakpitch,
            int(skylightcolor.x), int(skylightcolor.y), int(skylightcolor.z), sunlight ? 1 : 0);
    extern float hwrtworldgain;
    conoutf("hwrt pt sky: force %d ratio %.3f (this map %.3f) (from the sun %d, sun as shown %.3f), map sky as shown %.3f, world gain %.2f, sun colour %d %d %d x %.2f",
            hwrtptskyforce, hwrtptskyratio, hwrtptskymap, ptskyi.forced, ptskyi.sunref,
            ptskylum(skylightcolor.x/255.0f, skylightcolor.y/255.0f, skylightcolor.z/255.0f) * hwrtworldgain * (skylight ? 1 : 0), hwrtworldgain,
            int(sunlightcolor.x), int(sunlightcolor.y), int(sunlightcolor.z), sunlightscale);
});
#undef loopc
