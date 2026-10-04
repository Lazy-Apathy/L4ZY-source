#include "engine.h"

VARFP(waterreflect, 0, 1, 1, { cleanreflections(); preloadwatershaders(); });
VARFP(waterrefract, 0, 1, 1, { cleanreflections(); preloadwatershaders(); });
VARFP(waterenvmap, 0, 1, 1, { cleanreflections(); preloadwatershaders(); });
VARFP(waterfallrefract, 0, 0, 1, { cleanreflections(); preloadwatershaders(); });

/* vertex water */
VARP(watersubdiv, 0, 2, 3);
VARP(waterlod, 0, 1, 3);

static int wx1, wy1, wx2, wy2, wz, wsize, wsubdiv;
static float whoffset, whphase;

static inline float vertwangle(int v1, int v2)
{
    static const float whscale = 59.0f/23.0f/(2*M_PI);
    v1 &= wsize-1;
    v2 &= wsize-1;
    return v1*v2*whscale+whoffset;
}

static inline float vertwphase(float angle)
{
    float s = angle - int(angle) - 0.5f;
    s *= 8 - fabs(s)*16;
    return WATER_AMPLITUDE*s-WATER_OFFSET;
}

static inline void vertw(int v1, int v2, int v3)
{
    float h = vertwphase(vertwangle(v1, v2));
    gle::attribf(v1, v2, v3+h);
}

static inline void vertwq(float v1, float v2, float v3)
{
    gle::attribf(v1, v2, v3+whphase);
}

static inline void vertwn(float v1, float v2, float v3)
{
    float h = -WATER_OFFSET;
    gle::attribf(v1, v2, v3+h);
}

struct waterstrip
{
    int x1, y1, x2, y2, z;
    ushort size, subdiv;

    int numverts() const { return 2*((y2-y1)/subdiv + 1)*((x2-x1)/subdiv); }

    void save()
    {
        x1 = wx1;
        y1 = wy1;
        x2 = wx2;
        y2 = wy2;
        z = wz;
        size = wsize;
        subdiv = wsubdiv;
    }

    void restore()
    {
        wx1 = x1;
        wy1 = y1;
        wx2 = x2;
        wy2 = y2;
        wz = z;
        wsize = size;
        wsubdiv = subdiv;
    }
};
vector<waterstrip> waterstrips;

void flushwaterstrips()
{
    if(gle::attribbuf.length()) xtraverts += gle::end();
    gle::defvertex();
    int numverts = 0;
    loopv(waterstrips) numverts += waterstrips[i].numverts();
    gle::begin(GL_TRIANGLE_STRIP, numverts);
    loopv(waterstrips)
    {
        waterstrips[i].restore();
        for(int x = wx1; x < wx2; x += wsubdiv)
        {
            for(int y = wy1; y <= wy2; y += wsubdiv)
            {
                vertw(x,         y, wz);
                vertw(x+wsubdiv, y, wz);
            }
            x += wsubdiv;
            if(x >= wx2) break;
            for(int y = wy2; y >= wy1; y -= wsubdiv)
            {
                vertw(x,         y, wz);
                vertw(x+wsubdiv, y, wz);
            }
        }
        gle::multidraw();
    }
    waterstrips.setsize(0);
    wsize = 0;
    xtraverts += gle::end();
}

void flushwater(int mat = MAT_WATER, bool force = true)
{
    if(wsize)
    {
        if(wsubdiv >= wsize)
        {
            if(gle::attribbuf.empty()) { gle::defvertex(); gle::begin(GL_QUADS); }
            vertwq(wx1, wy1, wz);
            vertwq(wx2, wy1, wz);
            vertwq(wx2, wy2, wz);
            vertwq(wx1, wy2, wz);
        }
        else waterstrips.add().save();
        wsize = 0;
    }

    if(force)
    {
        if(gle::attribbuf.length()) xtraverts += gle::end();
        if(waterstrips.length()) flushwaterstrips();
    }
}

void rendervertwater(int subdiv, int xo, int yo, int z, int size, int mat)
{
    if(wsize == size && wsubdiv == subdiv && wz == z)
    {
        if(wx2 == xo)
        {
            if(wy1 == yo && wy2 == yo + size) { wx2 += size; return; }
        }
        else if(wy2 == yo && wx1 == xo && wx2 == xo + size) { wy2 += size; return; }
    }

    flushwater(mat, false);

    wx1 = xo;
    wy1 = yo;
    wx2 = xo + size,
    wy2 = yo + size;
    wz = z;
    wsize = size;
    wsubdiv = subdiv;

    ASSERT((wx1 & (subdiv - 1)) == 0);
    ASSERT((wy1 & (subdiv - 1)) == 0);
}

int calcwatersubdiv(int x, int y, int z, int size)
{
    float dist;
    if(camera1->o.x >= x && camera1->o.x < x + size &&
       camera1->o.y >= y && camera1->o.y < y + size)
        dist = fabs(camera1->o.z - float(z));
    else
        dist = vec(x + size/2, y + size/2, z + size/2).dist(camera1->o) - size*1.42f/2;
    int subdiv = watersubdiv + int(dist) / (32 << waterlod);
    return subdiv >= 31 ? INT_MAX : 1<<subdiv;
}

int renderwaterlod(int x, int y, int z, int size, int mat)
{
    if(size <= (32 << waterlod))
    {
        int subdiv = calcwatersubdiv(x, y, z, size);
        if(subdiv < size * 2) rendervertwater(min(subdiv, size), x, y, z, size, mat);
        return subdiv;
    }
    else
    {
        int subdiv = calcwatersubdiv(x, y, z, size);
        if(subdiv >= size)
        {
            if(subdiv < size * 2) rendervertwater(size, x, y, z, size, mat);
            return subdiv;
        }
        int childsize = size / 2,
            subdiv1 = renderwaterlod(x, y, z, childsize, mat),
            subdiv2 = renderwaterlod(x + childsize, y, z, childsize, mat),
            subdiv3 = renderwaterlod(x + childsize, y + childsize, z, childsize, mat),
            subdiv4 = renderwaterlod(x, y + childsize, z, childsize, mat),
            minsubdiv = subdiv1;
        minsubdiv = min(minsubdiv, subdiv2);
        minsubdiv = min(minsubdiv, subdiv3);
        minsubdiv = min(minsubdiv, subdiv4);
        if(minsubdiv < size * 2)
        {
            if(minsubdiv >= size) rendervertwater(size, x, y, z, size, mat);
            else
            {
                if(subdiv1 >= size) rendervertwater(childsize, x, y, z, childsize, mat);
                if(subdiv2 >= size) rendervertwater(childsize, x + childsize, y, z, childsize, mat);
                if(subdiv3 >= size) rendervertwater(childsize, x + childsize, y + childsize, z, childsize, mat);
                if(subdiv4 >= size) rendervertwater(childsize, x, y + childsize, z, childsize, mat);
            } 
        }
        return minsubdiv;
    }
}

void renderflatwater(int x, int y, int z, int rsize, int csize, int mat)
{
    if(gle::attribbuf.empty()) { gle::defvertex(); gle::begin(GL_QUADS); }
    vertwn(x,       y,       z);
    vertwn(x+rsize, y,       z);
    vertwn(x+rsize, y+csize, z);
    vertwn(x,       y+csize, z);
}

VARFP(vertwater, 0, 1, 1, allchanged());

static inline void renderwater(const materialsurface &m, int mat = MAT_WATER)
{
    if(!vertwater || drawtex == DRAWTEX_MINIMAP) renderflatwater(m.o.x, m.o.y, m.o.z, m.rsize, m.csize, mat);
    else if(renderwaterlod(m.o.x, m.o.y, m.o.z, m.csize, mat) >= int(m.csize) * 2)
        rendervertwater(m.csize, m.o.x, m.o.y, m.o.z, m.csize, mat);
}

void setuplava(Texture *tex, float scale)
{
    float xk = TEX_SCALE/(tex->xs*scale);
    float yk = TEX_SCALE/(tex->ys*scale);
    float scroll = lastmillis/1000.0f;
    LOCALPARAMF(lavatexgen, xk, yk, scroll, scroll);
    gle::normal(vec(0, 0, 1));
    whoffset = fmod(float(lastmillis/2000.0f/(2*M_PI)), 1.0f);
    whphase = vertwphase(whoffset);
}

void renderlava(const materialsurface &m)
{
    renderwater(m, MAT_LAVA);
}

void flushlava()
{
    flushwater(MAT_LAVA);
}

/* reflective/refractive water */

#define MAXREFLECTIONS 16

struct Reflection
{
    GLuint tex, refracttex;
    int material, height, depth, age;
    bool init;
    matrix4 projmat;
    occludequery *query, *prevquery;
    vector<materialsurface *> matsurfs;

    Reflection() : tex(0), refracttex(0), material(-1), height(-1), depth(0), age(0), init(false), query(NULL), prevquery(NULL)
    {}
};

VARP(reflectdist, 0, 2000, 10000);

#define WATERVARS(name) \
    bvec name##color(0x14, 0x46, 0x50), name##fallcolor(0, 0, 0); \
    HVARFR(name##colour, 0, 0x144650, 0xFFFFFF, \
    { \
        if(!name##colour) name##colour = 0x144650; \
        name##color = bvec((name##colour>>16)&0xFF, (name##colour>>8)&0xFF, name##colour&0xFF); \
    }); \
    VARR(name##fog, 0, 150, 10000); \
    VARR(name##spec, 0, 150, 1000); \
    HVARFR(name##fallcolour, 0, 0, 0xFFFFFF, \
    { \
        name##fallcolor = bvec((name##fallcolour>>16)&0xFF, (name##fallcolour>>8)&0xFF, name##fallcolour&0xFF); \
    });

WATERVARS(water)
WATERVARS(water2)
WATERVARS(water3)
WATERVARS(water4)

GETMATIDXVAR(water, colour, int)
GETMATIDXVAR(water, color, const bvec &)
GETMATIDXVAR(water, fallcolour, int)
GETMATIDXVAR(water, fallcolor, const bvec &)
GETMATIDXVAR(water, fog, int)
GETMATIDXVAR(water, spec, int)

#define LAVAVARS(name) \
    bvec name##color(0xFF, 0x40, 0x00); \
    HVARFR(name##colour, 0, 0xFF4000, 0xFFFFFF, \
    { \
        if(!name##colour) name##colour = 0xFF4000; \
        name##color = bvec((name##colour>>16)&0xFF, (name##colour>>8)&0xFF, name##colour&0xFF); \
    }); \
    VARR(name##fog, 0, 50, 10000);

LAVAVARS(lava)
LAVAVARS(lava2)
LAVAVARS(lava3)
LAVAVARS(lava4)

GETMATIDXVAR(lava, colour, int)
GETMATIDXVAR(lava, color, const bvec &)
GETMATIDXVAR(lava, fog, int)

void setprojtexmatrix(Reflection &ref)
{
    if(ref.init)
    {
        ref.init = false;
        (ref.projmat = camprojmatrix).projective();
    }
    
    LOCALPARAM(watermatrix, ref.projmat);
}

Reflection reflections[MAXREFLECTIONS];
Reflection waterfallrefraction;
GLuint reflectionfb = 0, reflectiondb = 0;

GLuint getwaterfalltex() { return waterfallrefraction.refracttex ? waterfallrefraction.refracttex : notexture->id; }

// Traced water reflections (hwrtreflections, RT only). The lighting pass asks
// for the planes GL will reflect this frame and traces them itself; a traced
// plane then skips its 2^reflectsize planar render (drawreflections) and the
// water shader reads the traced image (renderwater). Each plane goes with the
// rectangles of its surfaces GL draws this frame (merged where they line up),
// so the trace finds exactly the water GL draws. Only planes whose surfaces
// really overlap one at another height stay on GL's pass: the trace keeps
// the nearest surface along the ray, and that may not be the one GL shows.
// (Testing the planes' whole bounding boxes made far apart pools of one
// height cover a puddle below them, and the puddle fell back to the planar
// render each time one of their surfaces came into view: haste.)
// Planes of one height share a group, which is what the image records, so a
// face where two of them meet matches either.
static int rtreflframe = 0, rtreflgathered = -1;
static int rtreflgroup[MAXREFLECTIONS];
static float rtreflz[MAXREFLECTIONS];
// Water vertices wave in z, so a pixel GL draws at the very edge of a plane
// can meet the flat plane just outside a rectangle: the trace looks this far
// beyond them when no rectangle holds the point (hitlight.comp REFL_MARGIN).
static const float RTREFLMARGIN = 2.0f;
static void cleanuptracedreflectionparticles();
static GLuint tracedreflectiontex();

struct rtreflrect { float x0, y0, x1, y1; };

// Joins rectangles that share a whole edge: rows of equal y span that touch
// in x, then columns of equal x span that touch in y, twice.
static bool rtreflrowless(const rtreflrect &a, const rtreflrect &b)
{
    if(a.y0 != b.y0) return a.y0 < b.y0;
    if(a.y1 != b.y1) return a.y1 < b.y1;
    return a.x0 < b.x0;
}
static bool rtreflcolless(const rtreflrect &a, const rtreflrect &b)
{
    if(a.x0 != b.x0) return a.x0 < b.x0;
    if(a.x1 != b.x1) return a.x1 < b.x1;
    return a.y0 < b.y0;
}
static int mergertreflrects(rtreflrect *r, int n)
{
    loopk(4)
    {
        if(n < 2) break;
        bool rows = !(k&1);
        if(rows) quicksort(r, n, rtreflrowless);
        else quicksort(r, n, rtreflcolless);
        int m = 0;
        loopi(n)
        {
            if(m)
            {
                rtreflrect &p = r[m-1];
                if(rows ? p.y0 == r[i].y0 && p.y1 == r[i].y1 && r[i].x0 <= p.x1
                        : p.x0 == r[i].x0 && p.x1 == r[i].x1 && r[i].y0 <= p.y1)
                {
                    if(rows) p.x1 = max(p.x1, r[i].x1);
                    else p.y1 = max(p.y1, r[i].y1);
                    continue;
                }
            }
            r[m++] = r[i];
        }
        n = m;
    }
    return n;
}

// Largest first: most water pixels then stop at the first rectangle (hitlight.comp).
static bool rtreflbigger(const rtreflrect &a, const rtreflrect &b)
{
    return (a.x1 - a.x0)*(a.y1 - a.y0) > (b.x1 - b.x0)*(b.y1 - b.y0);
}

static inline bool rtreflrectsoverlap(const rtreflrect &a, const rtreflrect &b)
{
    return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}

int hwrtgatherwaterplanes(float (*out)[8], int maxplanes, float (*rects)[4], int maxrects)
{
    rtreflgathered = rtreflframe;
    memset(rtreflgroup, 0, sizeof(rtreflgroup));
    if(!waterreflect || drawtex || (editmode && showmat)) return 0;
    float offset = -WATER_OFFSET;
    static vector<rtreflrect> pool;
    pool.setsize(0);
    int first[MAXREFLECTIONS], count[MAXREFLECTIONS];
    rtreflrect box[MAXREFLECTIONS];
    bool cand[MAXREFLECTIONS];
    loopi(MAXREFLECTIONS)
    {
        Reflection &ref = reflections[i];
        first[i] = pool.length();
        count[i] = 0;
        cand[i] = ref.height>=0 && !ref.age && !ref.matsurfs.empty() && camera1->o.z >= ref.height+offset;
        if(!cand[i]) continue;
        loopvj(ref.matsurfs)
        {
            const materialsurface &m = *ref.matsurfs[j];
            rtreflrect &r = pool.add();
            r.x0 = m.o.x; r.y0 = m.o.y;
            r.x1 = m.o.x + m.rsize; r.y1 = m.o.y + m.csize;
        }
        count[i] = mergertreflrects(&pool[first[i]], ref.matsurfs.length());
        pool.setsize(first[i] + count[i]);
        quicksort(&pool[first[i]], count[i], rtreflbigger);
        rtreflrect &b = box[i];
        b = pool[first[i]];
        for(int k = first[i]+1; k < first[i]+count[i]; k++)
        {
            const rtreflrect &r = pool[k];
            b.x0 = min(b.x0, r.x0); b.y0 = min(b.y0, r.y0);
            b.x1 = max(b.x1, r.x1); b.y1 = max(b.y1, r.y1);
        }
    }
    bool clash[MAXREFLECTIONS];
    loopi(MAXREFLECTIONS)
    {
        clash[i] = false;
        if(!cand[i]) continue;
        loopj(MAXREFLECTIONS)
        {
            if(j == i || !cand[j] || reflections[j].height == reflections[i].height || !rtreflrectsoverlap(box[i], box[j])) continue;
            for(int a = first[i]; a < first[i]+count[i] && !clash[i]; a++)
            {
                if(!rtreflrectsoverlap(pool[a], box[j])) continue;
                for(int b = first[j]; b < first[j]+count[j]; b++) if(rtreflrectsoverlap(pool[a], pool[b])) { clash[i] = true; break; }
            }
            if(clash[i]) break;
        }
    }
    int n = 0, nr = 0;
    loopi(MAXREFLECTIONS)
    {
        if(!cand[i] || clash[i] || n >= maxplanes || nr + count[i] > maxrects) continue;
        int group = i + 1;
        loopj(i) if(rtreflgroup[j] && reflections[j].height == reflections[i].height) { group = rtreflgroup[j]; break; }
        rtreflgroup[i] = group;
        rtreflz[i] = reflections[i].height + offset;
        float *p = out[n++];
        p[0] = box[i].x0 - RTREFLMARGIN; p[1] = box[i].y0 - RTREFLMARGIN;
        p[2] = box[i].x1 + RTREFLMARGIN; p[3] = box[i].y1 + RTREFLMARGIN;
        p[4] = rtreflz[i];
        p[5] = float(group);
        p[6] = float(nr);
        p[7] = float(count[i]);
        loopk(count[i])
        {
            const rtreflrect &r = pool[first[i]+k];
            float *q = rects[nr++];
            q[0] = r.x0; q[1] = r.y0; q[2] = r.x1; q[3] = r.y1;
        }
    }
    return n;
}

// Group of plane i if the lighting pass traced it for this very frame, else 0.
static int rtreflectedgroup(int i)
{
    if(rtreflgathered != rtreflframe || drawtex || !hwrtrefllive()) return 0;
    return rtreflgroup[i];
}

VAR(oqwater, 0, 2, 2);
VARFP(waterfade, 0, 1, 1, { cleanreflections(); preloadwatershaders(); });

void preloadwatershaders(bool force)
{
    static bool needwater = false;
    if(force) needwater = true;
    if(!needwater) return;

    useshaderbyname("waterglare");

    if(waterenvmap && !waterreflect)
        useshaderbyname(waterrefract ? (waterfade ? "waterenvfade" : "waterenvrefract") : "waterenv");
    else useshaderbyname(waterrefract ? (waterfade ? "waterfade" : "waterrefract") : (waterreflect ? "waterreflect" : "water"));

    useshaderbyname(waterrefract ? (waterfade ? "underwaterfade" : "underwaterrefract") : "underwater");
    // The traced-reflection variants are only ever used with ray tracing on.
    if(hwrt && hwrtreflections && waterreflect)
        useshaderbyname(waterrefract ? (waterfade ? "waterfadert" : "waterrefractrt") : "waterreflectrt");

    extern int waterfallenv;
    useshaderbyname(waterfallenv ? "waterfallenv" : "waterfall");
    if(waterfallrefract) useshaderbyname(waterfallenv ? "waterfallenvrefract" : "waterfallrefract");
}

void renderwater()
{
    if(editmode && showmat && !drawtex) return;
    if(!rplanes) return;

    glDisable(GL_CULL_FACE);

    if(!glaring && drawtex != DRAWTEX_MINIMAP)
    {
        if(waterrefract)
        {
            if(waterfade)
            {
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            }
        }
        else
        {
            glDepthMask(GL_FALSE);
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_SRC_ALPHA);
        }
    }

    GLOBALPARAM(camera, camera1->o);
    GLOBALPARAMF(millis, lastmillis/1000.0f);

    #define SETWATERSHADER(which, name) \
    do { \
        static Shader *name##shader = NULL; \
        if(!name##shader) name##shader = lookupshaderbyname(#name); \
        which##shader = name##shader; \
    } while(0)

    Shader *aboveshader = NULL;
    if(glaring) SETWATERSHADER(above, waterglare);
    else if(drawtex == DRAWTEX_MINIMAP) aboveshader = notextureshader;
    else if(waterenvmap && !waterreflect)
    {
        if(waterrefract)
        {
            if(waterfade) SETWATERSHADER(above, waterenvfade);
            else SETWATERSHADER(above, waterenvrefract);
        }
        else SETWATERSHADER(above, waterenv);
    }
    else if(waterrefract) 
    {
        if(waterfade) SETWATERSHADER(above, waterfade);
        else SETWATERSHADER(above, waterrefract);
    }
    else if(waterreflect) SETWATERSHADER(above, waterreflect);
    else SETWATERSHADER(above, water);

    Shader *belowshader = NULL;
    if(!glaring && drawtex != DRAWTEX_MINIMAP)
    {
        if(waterrefract)
        {
            if(waterfade) SETWATERSHADER(below, underwaterfade);
            else SETWATERSHADER(below, underwaterrefract);
        }
        else SETWATERSHADER(below, underwater);
    }

    // Traced reflections: the traced image replaces the planar texture on the
    // planes the lighting pass traced this frame.
    bool rtrefl = !glaring && !drawtex && waterreflect && hwrtrefllive();
    float rtinvw = 0, rtinvh = 0;
    if(rtrefl)
    {
        int rw = 0, rh = 0;
        hwrtresultsize(&rw, &rh);
        if(rw > 0 && rh > 0) { rtinvw = 1.0f/rw; rtinvh = 1.0f/rh; }
        else rtrefl = false;
    }
    // Unit 4 is left alone unless a plane reads it (classic lighting never does).
    if(rtrefl)
    {
        glActiveTexture_(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, tracedreflectiontex());
        glActiveTexture_(GL_TEXTURE0);
    }
    // Planes the RT traced this frame use the same shader reading the traced
    // image instead of the planar texture (glsl.cfg, suffix "rt"); the
    // shaders above are never touched, so classic lighting stays as it was.
    // They are lazy shaders, compiled the first time a traced plane needs one.
    Shader *rtshader = NULL;
    if(rtrefl)
    {
        static Shader *rtshaders[3] = { NULL, NULL, NULL };
        static const char * const rtnames[3] = { "waterfadert", "waterrefractrt", "waterreflectrt" };
        int k = waterrefract ? (waterfade ? 0 : 1) : 2;
        if(!rtshaders[k] || !rtshaders[k]->loaded()) rtshaders[k] = useshaderbyname(rtnames[k]);
        if(rtshaders[k] && rtshaders[k]->loaded()) rtshader = rtshaders[k];
    }

    vec ambient(max(skylightcolor[0], ambientcolor[0]), max(skylightcolor[1], ambientcolor[1]), max(skylightcolor[2], ambientcolor[2]));
    float offset = -WATER_OFFSET;
    loopi(MAXREFLECTIONS)
    {
        Reflection &ref = reflections[i];
        if(ref.height<0 || ref.age || ref.matsurfs.empty()) continue;
        if(!glaring && oqfrags && oqwater && ref.query && ref.query->owner==&ref)
        {
            if(!ref.prevquery || ref.prevquery->owner!=&ref || checkquery(ref.prevquery))
            {
                if(checkquery(ref.query)) continue;
            }
        }

        bool below = camera1->o.z < ref.height+offset;
        if(below) 
        {
            if(!belowshader) continue;
            belowshader->set();
        }
        else
        {
            int rtgroup = rtrefl && rtshader ? rtreflectedgroup(i) : 0;
            if(rtgroup)
            {
                rtshader->set();
                LOCALPARAMF(rtrefl, 1.0f, float(rtgroup), rtinvw, rtinvh);
            }
            else aboveshader->set();
        }

        if(!glaring && drawtex != DRAWTEX_MINIMAP)
        {
            if(waterreflect || waterrefract)
            {
                if(waterreflect || !waterenvmap) glBindTexture(GL_TEXTURE_2D, waterreflect ? ref.tex : ref.refracttex);
                setprojtexmatrix(ref);
            }

            if(waterrefract)
            {
                glActiveTexture_(GL_TEXTURE3);
                glBindTexture(GL_TEXTURE_2D, ref.refracttex);
                if(waterfade) 
                {
                    float fadeheight = ref.height+offset+(below ? -2 : 2);
                    LOCALPARAMF(waterheight, fadeheight);
                }
            }
        }

        MSlot &mslot = lookupmaterialslot(ref.material);
        glActiveTexture_(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, mslot.sts.inrange(2) ? mslot.sts[2].t->id : notexture->id);
        glActiveTexture_(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, mslot.sts.inrange(3) ? mslot.sts[3].t->id : notexture->id);
        glActiveTexture_(GL_TEXTURE0);
        if(!glaring && waterenvmap && !waterreflect && drawtex != DRAWTEX_MINIMAP)
        {
            glBindTexture(GL_TEXTURE_CUBE_MAP, lookupenvmap(mslot));
        }

        whoffset = fmod(float(lastmillis/600.0f/(2*M_PI)), 1.0f);
        whphase = vertwphase(whoffset);

        gle::color(getwatercolor(ref.material));
        int wfog = getwaterfog(ref.material), wspec = getwaterspec(ref.material);

        const entity *lastlight = (const entity *)-1;
        int lastdepth = -1;
        loopvj(ref.matsurfs)
        {
            materialsurface &m = *ref.matsurfs[j];

            entity *light = (m.light && m.light->type==ET_LIGHT ? m.light : NULL);
            if(light!=lastlight)
            {
                flushwater();
                vec lightpos = light ? light->o : vec(worldsize/2, worldsize/2, worldsize);
                float lightrad = light && light->attr1 ? light->attr1 : worldsize*8.0f;
                vec lightcol = (light ? vec(light->attr2, light->attr3, light->attr4) : vec(ambient)).div(255.0f).mul(wspec/100.0f);
                LOCALPARAM(lightpos, lightpos);
                LOCALPARAM(lightcolor, lightcol);
                LOCALPARAMF(lightradius, lightrad);
                lastlight = light;
            }

            if(!glaring && !waterrefract && m.depth!=lastdepth)
            {
                flushwater();
                float depth = !wfog ? 1.0f : min(0.75f*m.depth/wfog, 0.95f);
                depth = max(depth, !below && (waterreflect || waterenvmap) ? 0.3f : 0.6f);
                LOCALPARAMF(depth, depth, 1.0f-depth);
                lastdepth = m.depth;
            }

            renderwater(m);
        }
        flushwater();
    }

    if(!glaring && drawtex != DRAWTEX_MINIMAP)
    {
        if(waterrefract)
        {
            if(waterfade) glDisable(GL_BLEND);
        }
        else
        {
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
        }
    }

    glEnable(GL_CULL_FACE);
}

void setupwaterfallrefract()
{
    glBindTexture(GL_TEXTURE_2D, waterfallrefraction.refracttex ? waterfallrefraction.refracttex : notexture->id);
    setprojtexmatrix(waterfallrefraction);
}

void cleanreflection(Reflection &ref)
{
    ref.material = -1;
    ref.height = -1;
    ref.init = false;
    ref.query = ref.prevquery = NULL;
    ref.matsurfs.setsize(0);
    if(ref.tex)
    {
        glDeleteTextures(1, &ref.tex);
        ref.tex = 0;
    }
    if(ref.refracttex)
    {
        glDeleteTextures(1, &ref.refracttex);
        ref.refracttex = 0;
    }
}

void cleanreflections()
{
    cleanuptracedreflectionparticles();
    loopi(MAXREFLECTIONS) cleanreflection(reflections[i]);
    cleanreflection(waterfallrefraction);
    if(reflectionfb)
    {
        glDeleteFramebuffers_(1, &reflectionfb);
        reflectionfb = 0;
    }
    if(reflectiondb)
    {
        glDeleteRenderbuffers_(1, &reflectiondb);
        reflectiondb = 0;
    }
}

VARFP(reflectsize, 6, 8, 11, cleanreflections());

void genwatertex(GLuint &tex, GLuint &fb, GLuint &db, bool refract = false)
{
    static const GLenum colorfmts[] = { GL_RGBA, GL_RGBA8, GL_RGB, GL_RGB8, GL_FALSE },
                        depthfmts[] = { GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT32, GL_FALSE };
    static GLenum reflectfmt = GL_FALSE, refractfmt = GL_FALSE, depthfmt = GL_FALSE;
    static bool usingalpha = false;
    bool needsalpha = refract && waterrefract && waterfade;
    if(refract && usingalpha!=needsalpha)
    {
        usingalpha = needsalpha;
        refractfmt = GL_FALSE;
    }
    int size = 1<<reflectsize;
    while(size>hwtexsize) size /= 2;

    glGenTextures(1, &tex);
    char *buf = new char[size*size*4];
    memset(buf, 0, size*size*4);

    GLenum &colorfmt = refract ? refractfmt : reflectfmt;
    if(colorfmt && fb && db)
    {
        createtexture(tex, size, size, buf, 3, 1, colorfmt);
        delete[] buf;
        return;
    }

    if(!fb) glGenFramebuffers_(1, &fb);
    int find = needsalpha ? 0 : 2;
    do
    {
        createtexture(tex, size, size, buf, 3, 1, colorfmt ? colorfmt : colorfmts[find]);
        glBindFramebuffer_(GL_FRAMEBUFFER, fb);
        glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        if(glCheckFramebufferStatus_(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE) break;
    }
    while(!colorfmt && colorfmts[++find]);
    if(!colorfmt) colorfmt = colorfmts[find];

    delete[] buf;

    if(!db) { glGenRenderbuffers_(1, &db); depthfmt = GL_FALSE; }
    if(!depthfmt) glBindRenderbuffer_(GL_RENDERBUFFER, db);
    find = 0;
    do
    {
        if(!depthfmt) glRenderbufferStorage_(GL_RENDERBUFFER, depthfmts[find], size, size);
        glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, db);
        if(glCheckFramebufferStatus_(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE) break;
    }
    while(!depthfmt && depthfmts[++find]);
    if(!depthfmt)
    {
        glBindRenderbuffer_(GL_RENDERBUFFER, 0);
        depthfmt = depthfmts[find];
    }

    hwrtbindscenefb();
}

void addwaterfallrefraction(materialsurface &m)
{
    Reflection &ref = waterfallrefraction;
    if(ref.age>=0)
    {
        ref.age = -1;
        ref.init = false;
        ref.matsurfs.setsize(0);
        ref.material = MAT_WATER;
        ref.height = INT_MAX;
    }
    ref.matsurfs.add(&m);

    if(!ref.refracttex) genwatertex(ref.refracttex, reflectionfb, reflectiondb);
}

void addreflection(materialsurface &m)
{
    int mat = m.material, height = m.o.z;
    Reflection *ref = NULL, *oldest = NULL;
    loopi(MAXREFLECTIONS)
    {
        Reflection &r = reflections[i];
        if(r.height<0)
        {
            if(!ref) ref = &r;
        }
        else if(r.height==height && r.material==mat) 
        {
            r.matsurfs.add(&m);
            r.depth = max(r.depth, int(m.depth));
            if(r.age<0) return;
            ref = &r;
            break;
        }
        else if(!oldest || r.age>oldest->age) oldest = &r;
    }
    if(!ref)
    {
        if(!oldest || oldest->age<0) return;
        ref = oldest;
    }
    if(ref->height!=height || ref->material!=mat) 
    {
        ref->material = mat;
        ref->height = height;
        ref->prevquery = NULL;
    }
    rplanes++;
    ref->age = -1;
    ref->init = false;
    ref->matsurfs.setsize(0);
    ref->matsurfs.add(&m);
    ref->depth = m.depth;
    if(drawtex == DRAWTEX_MINIMAP) return;

    if(waterreflect && !ref->tex) genwatertex(ref->tex, reflectionfb, reflectiondb);
    if(waterrefract && !ref->refracttex) genwatertex(ref->refracttex, reflectionfb, reflectiondb, true);
}

static void drawmaterialquery(const materialsurface &m, float offset, float border = 0, float reflect = -1)
{
    if(gle::attribbuf.empty())
    {
        gle::defvertex();
        gle::begin(GL_QUADS);
    }
    float x = m.o.x, y = m.o.y, z = m.o.z, csize = m.csize + border, rsize = m.rsize + border;
    if(reflect >= 0) z = 2*reflect - z;
    switch(m.orient)
    {
#define GENFACEORIENT(orient, v0, v1, v2, v3) \
        case orient: v0 v1 v2 v3 break;
#define GENFACEVERT(orient, vert, mx,my,mz, sx,sy,sz) \
            gle::attribf(mx sx, my sy, mz sz); 
        GENFACEVERTS(x, x, y, y, z, z, - border, + csize, - border, + rsize, + offset, - offset)
#undef GENFACEORIENT
#undef GENFACEVERT
    }
}

extern void drawreflection(float z, bool refract, int fogdepth = -1, const bvec &col = bvec(0, 0, 0));

int rplanes = 0;

void queryreflection(Reflection &ref, bool init)
{
    if(init)
    {
        nocolorshader->set();
        glDepthMask(GL_FALSE);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glDisable(GL_CULL_FACE);
    }
    startquery(ref.query);
    loopvj(ref.matsurfs)
    {
        materialsurface &m = *ref.matsurfs[j];
        float offset = 0.1f;
        if(m.orient==O_TOP)
        {
            offset = WATER_OFFSET +
                (vertwater ? WATER_AMPLITUDE*(camera1->pitch > 0 || m.depth < WATER_AMPLITUDE+0.5f ? -1 : 1) : 0);
            if(fabs(m.o.z-offset - camera1->o.z) < 0.5f && m.depth > WATER_AMPLITUDE+1.5f)
                offset += camera1->pitch > 0 ? -1 : 1;
        }
        drawmaterialquery(m, offset);
    }
    xtraverts += gle::end();
    endquery(ref.query);
}

void queryreflections()
{
    rplanes = 0;
    if(!drawtex) rtreflframe++;

    static int lastsize = 0;
    int size = 1<<reflectsize;
    while(size>hwtexsize) size /= 2;
    if(size!=lastsize) { if(lastsize) cleanreflections(); lastsize = size; }

    for(vtxarray *va = visibleva; va; va = va->next)
    {
        if(!va->matsurfs || va->occluded >= OCCLUDE_BB || va->curvfc >= VFC_FOGGED) continue;
        int lastmat = -1;
        loopi(va->matsurfs)
        {
            materialsurface &m = va->matbuf[i];
            if(m.material != lastmat)
            {
                if((m.material&MATF_VOLUME) != MAT_WATER || m.orient == O_BOTTOM) { i += m.skip; continue; }
                if(m.orient != O_TOP)
                {
                    if(!waterfallrefract || !getwaterfog(m.material)) { i += m.skip; continue; }
                }
                lastmat = m.material;
            }
            if(m.orient==O_TOP) addreflection(m);
            else addwaterfallrefraction(m);
        }
    }
  
    loopi(MAXREFLECTIONS)
    {
        Reflection &ref = reflections[i];
        ++ref.age;
        if(ref.height>=0 && !ref.age && ref.matsurfs.length())
        {
            if(waterpvsoccluded(ref.height)) ref.matsurfs.setsize(0);
        }
    }
    if(waterfallrefract)
    {
        Reflection &ref = waterfallrefraction;
        ++ref.age;
        if(ref.height>=0 && !ref.age && ref.matsurfs.length())
        {
            if(waterpvsoccluded(-1)) ref.matsurfs.setsize(0);
        }
    }

    if((editmode && showmat && !drawtex) || !oqfrags || !oqwater || drawtex == DRAWTEX_MINIMAP) return;

    int refs = 0;
    if(waterreflect || waterrefract) loopi(MAXREFLECTIONS)
    {
        Reflection &ref = reflections[i];
        ref.prevquery = oqwater > 1 ? ref.query : NULL;
        ref.query = ref.height>=0 && !ref.age && ref.matsurfs.length() ? newquery(&ref) : NULL;
        if(ref.query) queryreflection(ref, !refs++);
    }
    if(waterfallrefract)
    {
        Reflection &ref = waterfallrefraction;
        ref.prevquery = oqwater > 1 ? ref.query : NULL;
        ref.query = ref.height>=0 && !ref.age && ref.matsurfs.length() ? newquery(&ref) : NULL;
        if(ref.query) queryreflection(ref, !refs++);
    }

    if(refs)
    {
        glDepthMask(GL_TRUE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glEnable(GL_CULL_FACE);
    }

	glFlush();
}

VARP(maxreflect, 1, 2, 8);

int refracting = 0, refractfog = 0;
bvec refractcolor(0, 0, 0);
bool reflecting = false, fading = false, fogging = false;
float reflectz = 1e16f;

VAR(maskreflect, 0, 2, 16);

void maskreflection(Reflection &ref, float offset, bool reflect, bool clear = false)
{
    const bvec &wcol = getwatercolor(ref.material);
    vec color = wcol.tocolor();
    if(!maskreflect)
    {
        if(clear) glClearColor(color.r, color.g, color.b, 1);
        glClear(GL_DEPTH_BUFFER_BIT | (clear ? GL_COLOR_BUFFER_BIT : 0));
        return;
    }
    glClearDepth(0);
    glClear(GL_DEPTH_BUFFER_BIT);
    glClearDepth(1);
    glDepthRange(1, 1);
    glDepthFunc(GL_ALWAYS);
    glDisable(GL_CULL_FACE);
    if(clear)
    {
        notextureshader->set();
        gle::color(color);
    }
    else
    {
        nocolorshader->set();
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    }
    float reflectheight = reflect ? ref.height + offset : -1;
    loopv(ref.matsurfs)
    {
        materialsurface &m = *ref.matsurfs[i];
        drawmaterialquery(m, -offset, maskreflect, reflectheight);
    }
    xtraverts += gle::end();
    if(!clear) glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_CULL_FACE);
    glDepthFunc(GL_LESS);
    glDepthRange(0, 1);
}

VAR(reflectscissor, 0, 1, 1);
VAR(reflectvfc, 0, 1, 1);

static bool calcscissorbox(Reflection &ref, int size, vec &clipmin, vec &clipmax, int &sx, int &sy, int &sw, int &sh)
{
    materialsurface &m0 = *ref.matsurfs[0];
    int dim = dimension(m0.orient), r = R[dim], c = C[dim];
    ivec bbmin = m0.o, bbmax = bbmin;
    bbmax[r] += m0.rsize;
    bbmax[c] += m0.csize;
    loopvj(ref.matsurfs)
    {
        materialsurface &m = *ref.matsurfs[j];
        bbmin[r] = min(bbmin[r], m.o[r]);
        bbmin[c] = min(bbmin[c], m.o[c]);
        bbmax[r] = max(bbmax[r], m.o[r] + m.rsize);
        bbmax[c] = max(bbmax[c], m.o[c] + m.csize);
        bbmin[dim] = min(bbmin[dim], m.o[dim]);
        bbmax[dim] = max(bbmax[dim], m.o[dim]);
    }

    vec4 v[8];
    float sx1 = 1, sy1 = 1, sx2 = -1, sy2 = -1;
    loopi(8)
    {
        vec4 &p = v[i];
        camprojmatrix.transform(vec(i&1 ? bbmax.x : bbmin.x, i&2 ? bbmax.y : bbmin.y, (i&4 ? bbmax.z + WATER_AMPLITUDE : bbmin.z - WATER_AMPLITUDE) - WATER_OFFSET), p);
        if(p.z >= -p.w)
        {
            float x = p.x / p.w, y = p.y / p.w;
            sx1 = min(sx1, x);
            sy1 = min(sy1, y);
            sx2 = max(sx2, x);
            sy2 = max(sy2, y);
        }
    }
    if(sx1 >= sx2 || sy1 >= sy2) return false;
    loopi(8)
    {
        const vec4 &p = v[i];
        if(p.z >= -p.w) continue;    
        loopj(3)
        { 
            const vec4 &o = v[i^(1<<j)];
            if(o.z <= -o.w) continue;
            float t = (p.z + p.w)/(p.z + p.w - o.z - o.w),
                  w = p.w + t*(o.w - p.w),
                  x = (p.x + t*(o.x - p.x))/w,
                  y = (p.y + t*(o.y - p.y))/w;
            sx1 = min(sx1, x);
            sy1 = min(sy1, y);
            sx2 = max(sx2, x);
            sy2 = max(sy2, y);
        }
    }
    if(sx1 <= -1 && sy1 <= -1 && sx2 >= 1 && sy2 >= 1) return false;
    sx1 = max(sx1, -1.0f);
    sy1 = max(sy1, -1.0f);
    sx2 = min(sx2, 1.0f);
    sy2 = min(sy2, 1.0f);
    if(reflectvfc)
    {
        clipmin.x = clamp(clipmin.x, sx1, sx2);
        clipmin.y = clamp(clipmin.y, sy1, sy2);
        clipmax.x = clamp(clipmax.x, sx1, sx2);
        clipmax.y = clamp(clipmax.y, sy1, sy2);
    }
    sx = int(floor((sx1+1)*0.5f*size));
    sy = int(floor((sy1+1)*0.5f*size));
    sw = max(int(ceil((sx2+1)*0.5f*size)) - sx, 0);
    sh = max(int(ceil((sy2+1)*0.5f*size)) - sy, 0);
    return true;
}

VARR(refractclear, 0, 0, 1);

void drawreflections()
{
    if((editmode && showmat && !drawtex) || drawtex == DRAWTEX_MINIMAP) return;

    static int lastdrawn = 0;
    int refs = 0, n = lastdrawn;
    float offset = -WATER_OFFSET;
    int size = 1<<reflectsize;
    while(size>hwtexsize) size /= 2;

    if(waterreflect || waterrefract) loopi(MAXREFLECTIONS)
    {
        int idx = ++n%MAXREFLECTIONS;
        Reflection &ref = reflections[idx];
        if(ref.height<0 || ref.age || ref.matsurfs.empty()) continue;
        if(oqfrags && oqwater && ref.query && ref.query->owner==&ref)
        { 
            if(!ref.prevquery || ref.prevquery->owner!=&ref || checkquery(ref.prevquery))
            {
                if(checkquery(ref.query)) continue;
            }
        }
        // The RT traced this plane's reflection: no planar render for it, and
        // nothing at all when there is no refraction to draw either.
        bool traced = rtreflectedgroup(idx) != 0;
        if(traced && !(waterrefract && ref.refracttex)) continue;

        if(!refs) 
        {
            glViewport(0, 0, size, size);
            glBindFramebuffer_(GL_FRAMEBUFFER, reflectionfb);
            hwrthdrsetlin(false);
        }
        refs++;
        ref.init = true;
        lastdrawn = n;

        vec clipmin(-1, -1, -1), clipmax(1, 1, 1);
        int sx, sy, sw, sh;
        bool scissor = reflectscissor && calcscissorbox(ref, size, clipmin, clipmax, sx, sy, sw, sh);
        if(scissor) glScissor(sx, sy, sw, sh);
        else
        {
            sx = sy = 0;
            sw = sh = size;
        }

        const bvec &wcol = getwatercolor(ref.material);
        int wfog = getwaterfog(ref.material);

        if(waterreflect && ref.tex && camera1->o.z >= ref.height+offset && !traced)
        {
            glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ref.tex, 0);
            if(scissor) glEnable(GL_SCISSOR_TEST);
            maskreflection(ref, offset, true);
            savevfcP();
            setvfcP(ref.height+offset, clipmin, clipmax); 
            drawreflection(ref.height+offset, false);
            restorevfcP();
            if(scissor) glDisable(GL_SCISSOR_TEST);
        }

        if(waterrefract && ref.refracttex)
        {
            glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ref.refracttex, 0);
            if(scissor) glEnable(GL_SCISSOR_TEST);
            maskreflection(ref, offset, false, refractclear || !wfog || (ref.depth>=10000 && camera1->o.z >= ref.height + offset));
            if(wfog || waterfade)
            {
                savevfcP();
                setvfcP(-1, clipmin, clipmax);
                drawreflection(ref.height+offset, true, wfog, wcol);
                restorevfcP();
            }
            if(scissor) glDisable(GL_SCISSOR_TEST);
        }    

        if(refs>=maxreflect) break;
    }

    if(waterfallrefract && waterfallrefraction.refracttex)
    {
        Reflection &ref = waterfallrefraction;

        if(ref.height<0 || ref.age || ref.matsurfs.empty()) goto nowaterfall;
        if(oqfrags && oqwater && ref.query && ref.query->owner==&ref)
        {
            if(!ref.prevquery || ref.prevquery->owner!=&ref || checkquery(ref.prevquery))
            {
                if(checkquery(ref.query)) goto nowaterfall;
            }
        }

        if(!refs)
        {
            glViewport(0, 0, size, size);
            glBindFramebuffer_(GL_FRAMEBUFFER, reflectionfb);
            hwrthdrsetlin(false);
        }
        refs++;
        ref.init = true;

        vec clipmin(-1, -1, -1), clipmax(1, 1, 1);
        int sx, sy, sw, sh;
        bool scissor = reflectscissor && calcscissorbox(ref, size, clipmin, clipmax, sx, sy, sw, sh);
        if(scissor) glScissor(sx, sy, sw, sh);
        else
        {
            sx = sy = 0;
            sw = sh = size;
        }

        glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ref.refracttex, 0);
        if(scissor) glEnable(GL_SCISSOR_TEST);
        maskreflection(ref, -0.1f, false);
        savevfcP();
        setvfcP(-1, clipmin, clipmax);
        drawreflection(-1, true); 
        restorevfcP();
        if(scissor) glDisable(GL_SCISSOR_TEST);
    }
nowaterfall:

    if(!refs) return;
    hwrtbindscenefb();
}

// Particles in the traced reflections. The traced image holds what each pixel
// of a traced plane reflects, and how far along the reflected path that is
// (hitlight.comp). GL never draws into that shared image: a copy of it goes to
// a texture of our own, then for each group a full-screen pass turns the path
// length into the depth the GL mirror camera would give the same point, and the
// particles are drawn with that camera into the copy (alpha left alone). The
// water shader then reads the copy. A particle behind what the reflection hits
// fails the depth test; pixels of other groups keep depth 0.
static GLuint rtpartfb = 0, rtpartdepthrb = 0, rtpartcolor = 0;
static int rtpartw = 0, rtparth = 0, rtpartframe = -1;

static void cleanuptracedreflectionparticles()
{
    if(rtpartfb) { glDeleteFramebuffers_(1, &rtpartfb); rtpartfb = 0; }
    if(rtpartdepthrb) { glDeleteRenderbuffers_(1, &rtpartdepthrb); rtpartdepthrb = 0; }
    if(rtpartcolor) { glDeleteTextures(1, &rtpartcolor); rtpartcolor = 0; }
    rtpartw = rtparth = 0;
    rtpartframe = -1;
}

static bool setuptracedreflectionparticles(int w, int h)
{
    if(rtpartfb && rtpartw == w && rtparth == h) return true;
    cleanuptracedreflectionparticles();
    glGenTextures(1, &rtpartcolor);
    glBindTexture(GL_TEXTURE_2D, rtpartcolor);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, w, h, 0, GL_RGBA, GL_FLOAT, NULL);
    // Nearest, like the traced image: the alpha is an id and a length.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenRenderbuffers_(1, &rtpartdepthrb);
    glBindRenderbuffer_(GL_RENDERBUFFER, rtpartdepthrb);
    glRenderbufferStorage_(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    glBindRenderbuffer_(GL_RENDERBUFFER, 0);

    glGenFramebuffers_(1, &rtpartfb);
    glBindFramebuffer_(GL_FRAMEBUFFER, rtpartfb);
    glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rtpartcolor, 0);
    glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rtpartdepthrb);
    if(glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        cleanuptracedreflectionparticles();
        conoutf(CON_WARN, "hwrt: no framebuffer for the particles of the traced reflections");
        return false;
    }
    rtpartw = w;
    rtparth = h;
    return true;
}

// What the water shader samples for a traced plane this frame: the copy with
// the particles when that pass ran, else the traced image itself.
static GLuint tracedreflectiontex()
{
    if(rtpartcolor && rtpartframe == rtreflframe) return rtpartcolor;
    return hwrtreflgltex();
}

void drawtracedreflectionparticles()
{
    if(glaring || drawtex || !hwrtrefllive() || rtreflgathered != rtreflframe) return;
    if(!hasparticlework()) return;
    int groups[MAXREFLECTIONS], ngroups = 0;
    float groupz[MAXREFLECTIONS];
    loopi(MAXREFLECTIONS)
    {
        int g = rtreflectedgroup(i);
        if(!g) continue;
        bool seen = false;
        loopj(ngroups) if(groups[j] == g) { seen = true; break; }
        if(seen) continue;
        groups[ngroups] = g;
        groupz[ngroups] = rtreflz[i];
        ngroups++;
    }
    if(!ngroups) return;
    int w = 0, h = 0;
    hwrtresultsize(&w, &h);
    GLuint tex = hwrtreflgltex();
    if(w <= 0 || h <= 0 || !tex) return;

    // Lazy shaders: useshaderbyname compiles them the first time.
    static Shader *depthshader = NULL, *copyshader = NULL;
    if(!depthshader || !depthshader->loaded()) depthshader = useshaderbyname("hwrtreflpartdepth");
    if(!copyshader || !copyshader->loaded()) copyshader = useshaderbyname("hwrtreflpartcopy");
    if(!depthshader || !copyshader || !depthshader->loaded() || !copyshader->loaded()) return;

    // Everything this pass changes goes back exactly as it was: the frame
    // keeps drawing water, grass and its own particles right after.
    GLint prevdraw = 0, prevread = 0, prevviewport[4], prevdepthfunc = GL_LESS, prevactive = GL_TEXTURE0, prevtex = 0;
    GLfloat prevclear = 1;
    GLboolean prevdepthmask = GL_TRUE, prevcolormask[4];
    GLboolean prevblend = glIsEnabled(GL_BLEND), prevcull = glIsEnabled(GL_CULL_FACE), prevdepthtest = glIsEnabled(GL_DEPTH_TEST);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevdraw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevread);
    glGetIntegerv(GL_VIEWPORT, prevviewport);
    glGetIntegerv(GL_DEPTH_FUNC, &prevdepthfunc);
    glGetFloatv(GL_DEPTH_CLEAR_VALUE, &prevclear);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &prevdepthmask);
    glGetBooleanv(GL_COLOR_WRITEMASK, prevcolormask);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevactive);
    glActiveTexture_(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevtex);

    if(setuptracedreflectionparticles(w, h))
    {
        matrix4 maininvcamproj = invcamprojmatrix;
        glBindFramebuffer_(GL_FRAMEBUFFER, rtpartfb);
        glViewport(0, 0, w, h);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glEnable(GL_DEPTH_TEST);
        glActiveTexture_(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);

        // The copy: colour and alpha, every pixel.
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_FALSE);
        glDepthFunc(GL_ALWAYS);
        copyshader->set();
        LOCALPARAMF(reflpart, 0, 0, 1.0f/w, 1.0f/h);
        screenquad();

        loopk(ngroups)
        {
            hwrtreflectcamera(groupz[k], true);

            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            glDepthMask(GL_TRUE);
            glClearDepth(0.0);
            glClear(GL_DEPTH_BUFFER_BIT);
            glDepthFunc(GL_ALWAYS);
            glDisable(GL_BLEND);
            glDisable(GL_CULL_FACE);
            depthshader->set();
            LOCALPARAM(reflinvcamproj, maininvcamproj);
            LOCALPARAM(reflcamproj, camprojmatrix);
            LOCALPARAMF(reflpart, float(groups[k]), groupz[k], 1.0f/w, 1.0f/h);
            LOCALPARAM(reflcam, camera1->o);
            glActiveTexture_(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, tex);
            screenquad();

            glDepthFunc(GL_LESS);
            glEnable(GL_CULL_FACE);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
            hwrthdrsetlin(true);
            renderparticles();

            hwrtreflectcamera(groupz[k], false);
        }
        rtpartframe = rtreflframe;
    }

    glBindFramebuffer_(GL_DRAW_FRAMEBUFFER, prevdraw);
    glBindFramebuffer_(GL_READ_FRAMEBUFFER, prevread);
    glViewport(prevviewport[0], prevviewport[1], prevviewport[2], prevviewport[3]);
    glDepthFunc(prevdepthfunc);
    glClearDepth(prevclear);
    glDepthMask(prevdepthmask);
    glColorMask(prevcolormask[0], prevcolormask[1], prevcolormask[2], prevcolormask[3]);
    if(prevblend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if(prevcull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if(prevdepthtest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glActiveTexture_(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, prevtex);
    glActiveTexture_(prevactive);
    // Same value the scene pass runs with (linear only in an HDR frame).
    hwrthdrsetlin(true);
}
