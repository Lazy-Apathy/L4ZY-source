#ifndef __ENGINE_H__
#define __ENGINE_H__

#include "cube.h"
#include "world.h"

#ifndef STANDALONE

#include "octa.h"
#include "lightmap.h"
#include "bih.h"
#include "texture.h"
#include "model.h"

extern dynent *player;
extern physent *camera1;                // special ent that acts as camera, same object as player1 in FPS mode

extern int worldscale, worldsize;
extern int mapversion;
extern char *maptitle;
extern vector<ushort> texmru;
extern int xtraverts, xtravertsva;
extern const ivec cubecoords[8];
extern const ivec facecoords[6][4];
extern const uchar fv[6][4];
extern const uchar fvmasks[64];
extern const uchar faceedgesidx[6][4];
extern bool inbetweenframes, renderedframe;

extern SDL_Window *screen;
extern int screenw, screenh;
extern int zpass;

extern vector<int> entgroup;

// rendertext
struct font
{
    struct charinfo
    {
        short x, y, w, h, offsetx, offsety, advance, tex;
    };

    char *name;
    vector<Texture *> texs;
    vector<charinfo> chars;
    int charoffset, defaultw, defaulth, scale;

    font() : name(NULL) {}
    ~font() { DELETEA(name); }
};

#define FONTH (curfont->scale)
#define FONTW (FONTH/2)
#define MINRESW 640
#define MINRESH 480

extern font *curfont;
extern const matrix4x3 *textmatrix;

extern void reloadfonts();

// texture
extern int hwtexsize, hwcubetexsize, hwmaxaniso, maxtexsize;

extern Texture *textureload(const char *name, int clamp = 0, bool mipit = true, bool msg = true);
extern int texalign(const void *data, int w, int bpp);
extern void cleanuptexture(Texture *t);
extern uchar *loadalphamask(Texture *t);
extern void loadlayermasks();
extern Texture *cubemapload(const char *name, bool mipit = true, bool msg = true, bool transient = false);
extern void drawcubemap(int size, const vec &o, float yaw, float pitch, const cubemapside &side, bool onlysky = false);
extern void loadshaders();
extern void setuptexparameters(int tnum, void *pixels, int clamp, int filter, GLenum format = GL_RGB, GLenum target = GL_TEXTURE_2D, bool swizzle = false);
extern void createtexture(int tnum, int w, int h, void *pixels, int clamp, int filter, GLenum component = GL_RGB, GLenum target = GL_TEXTURE_2D, int pw = 0, int ph = 0, int pitch = 0, bool resize = true, GLenum format = GL_FALSE, bool swizzle = false);
extern void blurtexture(int n, int bpp, int w, int h, uchar *dst, const uchar *src, int margin = 0);
extern void blurnormals(int n, int w, int h, bvec *dst, const bvec *src, int margin = 0);
extern void renderpostfx();
extern void initenvmaps();
extern void genenvmaps();
extern ushort closestenvmap(const vec &o);
extern ushort closestenvmap(int orient, const ivec &co, int size);
extern GLuint lookupenvmap(ushort emid);
extern GLuint lookupenvmap(Slot &slot);
extern bool reloadtexture(Texture &tex);
extern bool reloadtexture(const char *name);
extern void setuptexcompress();
extern void clearslots();
extern void compacteditvslots();
extern void compactmruvslots();
extern void compactvslots(cube *c, int n = 8);
extern void compactvslot(int &index);
extern void compactvslot(VSlot &vs);
extern int compactvslots();
extern void reloadtextures();
extern void cleanuptextures();

// shadowmap

extern int shadowmap, shadowmapcasters;
extern bool shadowmapping;
extern matrix4 shadowmatrix;

extern bool isshadowmapcaster(const vec &o, float rad);
extern bool addshadowmapcaster(const vec &o, float xyrad, float zrad);
extern bool isshadowmapreceiver(vtxarray *va);
extern void rendershadowmap();
extern void pushshadowmap();
extern void popshadowmap();
extern void rendershadowmapreceivers();
extern void guessshadowdir();

// pvs
extern void clearpvs();
extern bool pvsoccluded(const ivec &bbmin, const ivec &bbmax);
extern bool pvsoccludedsphere(const vec &center, float radius);
extern bool waterpvsoccluded(int height);
extern void setviewcell(const vec &p);
extern void savepvs(stream *f);
extern void loadpvs(stream *f, int numpvs);
extern int getnumviewcells();

static inline bool pvsoccluded(const ivec &bborigin, int size)
{
    return pvsoccluded(bborigin, ivec(bborigin).add(size));
}

// rendergl
extern bool hasVAO, hasFBO, hasAFBO, hasDS, hasTF, hasTRG, hasTSW, hasS3TC, hasFXT1, hasLATC, hasRGTC, hasAF, hasFBB, hasUBO, hasMBR;
extern int glversion, glslversion, glcompat;

enum { DRAWTEX_NONE = 0, DRAWTEX_ENVMAP, DRAWTEX_MINIMAP, DRAWTEX_MODELPREVIEW };

extern float curfov, curavatarfov, fovy, aspect, forceaspect, avatardepth, nearplane;
extern int farplane;
extern int drawtex;
extern bool renderedgame;
extern const matrix4 viewmatrix;
extern matrix4 cammatrix, projmatrix, camprojmatrix, invcammatrix, invcamprojmatrix, lastcamprojmatrix;
extern int looktaa;
extern int hwrtdlaajitter;
extern int hwrtdlaajitterrt;
extern int hwrtdlaa;
extern bvec fogcolor;
extern vec curfogcolor;
extern int fog;
extern float curfogstart, curfogend;

extern void gl_checkextensions();
extern bool hasext(const char *ext);
extern void *getprocaddress(const char *name);
extern void gl_init();
extern void gl_resize();
extern void cleanupgl();
extern void rendergame(bool mainpass = false);
extern void invalidatepostfx();
extern void gl_drawhud();
extern void gl_drawframe();
extern void gl_drawmainmenu();
extern void drawminimap();
extern void drawtextures();
extern void enablepolygonoffset(GLenum type);
extern void disablepolygonoffset(GLenum type);
extern void calcspherescissor(const vec &center, float size, float &sx1, float &sy1, float &sx2, float &sy2);
extern int pushscissor(float sx1, float sy1, float sx2, float sy2);
extern void popscissor();
extern void recomputecamera();
extern void screenquad();
extern void screenquad(float sw, float sh);
extern void screenquadflipped(float sw, float sh);
extern void screenquad(float sw, float sh, float sw2, float sh2);
extern void screenquadoffset(float x, float y, float w, float h);
extern void screenquadoffset(float x, float y, float w, float h, float x2, float y2, float w2, float h2);
extern void addlookao();
extern void cleanuplookao();
extern void addlooktaa();
extern void cleanuplooktaa();
extern void hwrtapplyvelspin();
extern void hwrtnotecamproj();
extern void hwrttemporalbeginframe();
extern void hwrttemporalapplymain();
extern void hwrttemporalaftercam();
extern void hwrttemporalapplystored(matrix4 &proj);
extern void hwrttemporalbeginavatarproj();
extern void hwrttemporalnoteavatar();
extern void hwrttemporalcapture();
extern void hwrttemporalreset(const char *why);
extern void cleanuptemporal();
extern GLuint hwrtvelocitytexid();
extern bool hwrtvelocitycutframe();
extern bool hwrtvelocityready();
extern void hwrtvelocitysize(int &w, int &h);
extern bool hwrtvelensureresources();
extern const matrix4 &hwrttemporalworldunjit();
extern void hwrttemporalinvcamprojforrt(matrix4 &out);
extern const matrix4 &hwrttemporalworldused();
extern const matrix4 &hwrttemporalavatarunjit();
extern const matrix4 &hwrttemporalavatarused();
extern void hwrttemporaljitterpixels(float &currx, float &curry, float &prevx, float &prevy);
extern bool hwrtdlaajitteractive();
extern bool hwrtdlaaneedsdata();
extern bool hwrtdlaaactive();
extern void hwrtdlaaframe();
extern void hwrtdlaaflushshot();
extern void hwrtdlaaafterhud();
extern void hwrtdlaacleanup();
extern int hwrtfbw();
extern int hwrtfbh();
extern int hwrtrenderw();
extern int hwrtrenderh();
extern GLuint hwrtmainfbo();
extern bool hwrtsceneinternal();
extern void hwrtbindscenefb();
extern bool hwrthdractive();
extern bool hwrthdrframe();
extern bool hwrthdrpresented();
extern void hwrthdrsetlin(bool targetlinear);
extern bool hwrtflamebegin();
extern void hwrtflameend();
extern GLuint hwrtflametex(int &w, int &h);
extern void hdrout_start();
extern void hdrout_shutdown();
extern bool hdrout_compose(GLuint tex, bool yflip);
extern void hdrout_begin_overlay();
extern bool hdrout_present();
extern GLuint hdrout_drawfb();
extern bool hdrout_fp16post();
extern void hdrout_drawlabel();
extern bool hdrout_preview_begin(int x, int y, int w, int h, bool background);
extern void hdrout_preview_end();
extern void hwrthdrsuspend();
extern void hwrthdrstampifprobe();
extern GLuint hwrthdrscenecolor();
extern void hwrtstriphdrlook();
extern void hwrtscenebegin();
extern void hwrtsceneend();
extern bool hwrtsceneblitwindow();
extern int hwrtdlaajitterseqlen();
extern int hwrtngxmoderequested();
extern int hwrtngxmodeapplied();
extern uint hwrttemporalframeid();
extern bool hwrttemporalresetflag();
extern void hwrttemporalsetreset(bool reset);
extern void hwrttemporalfprint(FILE *f);
extern void hwrttemporalanalytic(float &dx_px, float &dy_px);

// Lot 6 HUD-less 3D image for a future native-res DLAA evaluate.
// GPU textures; .valid is true only after hwrttemporalcapture() of a coherent
// same-frame resource set (colour, depth, and velocity when hwrtvelocity is
// on), until the next main gl_drawframe overwrites them (.valid_until).
// .reset means previous history is unusable; it does not make a missing
// resource valid.
struct hwrttemporalinput
{
    bool valid;
    uint frameid;
    bool reset;
    int width, height;
    GLuint colortex;
    GLuint depthtex;
    GLuint velocitytex;
    float jitter_px, jitter_py;
    float prev_jitter_px, prev_jitter_py;
    float jitter_ndc_x, jitter_ndc_y;
    matrix4 view;
    matrix4 proj_unjit, proj_used;
    matrix4 camproj_unjit, camproj_used, invcamproj_used;
    matrix4 avatar_proj_unjit, avatar_proj_used;
    matrix4 avatar_camproj_unjit, avatar_camproj_used;
    bool avatar_noted;
    bool same_ndc_on_avatar;
    float world_ndc_dx, world_ndc_dy, avatar_ndc_dx, avatar_ndc_dy;
    float nearz, farz, world_fovy, avatar_fov, avatar_depth;
    int color_internalformat;
    const char *color_domain;
    const char *depth_convention;
    const char *motion_convention;
    const char *coverage_note;
    const char *valid_until;
};
extern const hwrttemporalinput *hwrttemporalcurrent();
extern void hwrtvelbeginframe();
extern void hwrtvelmark();
extern void hwrtvelunmark();
extern void hwrtvelkeep();
extern void hwrtvelendmark();
extern void hwrtvelstencilsnap(const char *tag);
extern void hwrtvelpunchmovingmm();
extern bool hwrtvelmarking();
extern int hwrtvelcoverpending();
extern bool hwrtveloverlay();
extern bool hwrtvelstaticmapmodel(const extentity &e);
extern bool hwrtvelrigidmapmodel(const extentity &e);
extern bool hwrtvelobjpass;
extern bool hwrtvelskelpass;
extern bool hwrtvelhudpass;
extern int hwrtvelunlitmode;
extern int hwrtvelidmode;
extern int hwrtvelfreezepose;
extern void hwrtvelonobjmatrix(const matrix4 &modelmatrix, const matrix4 &stackm, float scale, const vec &translate);
extern void hwrtvelsetobjparams(float alphatest);
extern bool hwrtvelidobj();
extern int hwrtvelidslot();
extern void hwrtvelidsetclip(const matrix4 *m);
extern void hwrtvelidapplyclip();
extern void hwrtveladdrigidtri(int mesh, int tri, const vec &a, const vec &b, const vec &c, float axisdist, float area);
extern void hwrtvelmarkskel();
extern int hwrtvelequip;
extern int hwrtvelhud;
extern void hwrtvelnotechar(dynent *d, model *m, int anim, int basetime, int basetime2, const vec &pos, float yaw, float pitch, modelattach *a = NULL);
extern void hwrtvelstarthud(dynent *d, model *m, int anim, int basetime, int basetime2, const vec &pos, float yaw, float pitch);
extern bool hwrtvelskelprevbones(const dualquat *&bones, int &nbones);
extern void hwrtvelskelkeep(dynent *d, model *m, const dualquat *bones, int nbones, int nverts, int vweights = 0, int vblends = 0, bool gpuskin = false);
extern bool hwrtvelskelwanttrack();
extern int hwrtvelskelactiveslot();
extern int hwrtvelskelregion(const char *bonename);
extern void hwrtvelskeltrack(int region, int mesh, int tri, const vec &cur, const vec &prev, float score, float area);
extern float hwrtvelskelscreenscore(const vec &a, const vec &b, const vec &c);
extern bool hwrtvelequipok(dynent *d, model *parent, model *equip);
extern int hwrtvelskelequip(dynent *d, model *parent, model *m, const char *tag, int anim, int basetime);
extern void hwrtvelskelrestoreslot(int slot);
extern int hwrtvelskelattachregion();
extern void hwrtvelbeginavatar();
extern void hwrtvelendavatar();
extern void hwrtveldrawavatar();
extern void hwrtvelbegincolourhud();
extern void hwrtvelendcolourhud();
extern void hwrtvelnotecolourmuzzle(const vec &m);
extern int hwrtveldbgmm;
extern void hwrtgenvelocity();
extern void hwrtresetvelocity();
extern void cleanupvelocity();
extern void hwrtdrawveldebug();
extern void hwrtflushvelshot();
extern bool hwrtvelneeddraw();
extern void hwrtveldraweffects();
extern const char *hwrtdynentmdlname(dynent *d);
extern void hwrtentxform(const extentity &e, vec &o, float &yaw);
extern dynent *hwrtcameradynent();
extern bool hwrtdynentvisible(dynent *d);
extern void hudquad(float x, float y, float w, float h, float tx = 0, float ty = 0, float tw = 1, float th = 1);

// hwrt: Vulkan hardware ray tracing layer running beside the GL renderer
extern void hwrtinit();
extern void hwrtcleanup();
extern void hwrtrender();
extern void hwrtdrawtimes(int conw, int conh);
extern void hwrtrebuildworld();
extern void hwrtdropworld();
extern bool hwrtalignsundisk();
extern bool hwrtnobake();
extern void hwrtsnapworlddepth();
extern void hwrtsnapscenedepth();
extern void hwrtdrawalignsun(int farplane, float skyclip, float topclip, int faces);

// CPU skin of a dynent's current pose for the HWRT animated BLAS.
// geom may be NULL to fill only the cache key. Returns false when the model is
// static or the pose is not known yet (use the rest-pose BLAS).
struct hwrtposekey
{
    model *m;
    void *ragdoll;
    uint hash;
    bool worldspace; // ragdoll verts are already in world space; instance is identity
};
// positions/indices feed the BLAS; tcs, norms and skins feed the lighting
// shader. tcs and norms are per vertex, skins is one GL texture id per triangle.
// masks / glows are the matching mdlglow channel (0 / 3 if the mesh has none).
// specs is mdlspec (default 1); the packer stores maskscale.x = 0.5*spec.
// envscales is per vertex (envmapmin, envmapmax) for the menu cubemap mix.
struct hwrtskingeom
{
    vector<vec> positions, norms;
    vector<vec2> tcs, envscales;
    vector<uint> indices, skins, masks;
    vector<float> glows, specs;

    void reset()
    {
        positions.setsize(0);
        norms.setsize(0);
        tcs.setsize(0);
        envscales.setsize(0);
        indices.setsize(0);
        skins.setsize(0);
        masks.setsize(0);
        glows.setsize(0);
        specs.setsize(0);
    }
};
extern bool hwrtskinmodel(dynent *d, model *m, float pitch, hwrtskingeom *geom, hwrtposekey &key);
extern void hwrtlookupskin(model *m, Texture *tex, Texture *&masks, float &glow, float &spec, float &envmin, float &envmax, GLuint &envid, float &alphatest);
extern void hwrtnoteenvmap(GLuint gltex);
// Game fills the same attachments renderplayer() uses (gun, armour, quad).
// Weak fallback returns 0 so an unpatched fpsgame still links.
extern int hwrtdynentattach(dynent *d, modelattach *dst, int maxa);
extern int hwrtdynentseq(dynent *d);
extern int hwrtdynentuid(dynent *d);
extern void setfogcolor(const vec &v);
extern void zerofogcolor();
extern void resetfogcolor();
extern void setfogdist(float start, float end);
extern void clearfogdist();
extern void resetfogdist();
extern void writecrosshairs(stream *f);

namespace modelpreview
{
    extern void start(int x, int y, int w, int h, bool background = true);
    extern void end();
}

// renderextras
extern void render3dbox(vec &o, float tofloor, float toceil, float xradius, float yradius = 0);

// octa
extern cube *newcubes(uint face = F_EMPTY, int mat = MAT_AIR);
extern cubeext *growcubeext(cubeext *ext, int maxverts);
extern void setcubeext(cube &c, cubeext *ext);
extern cubeext *newcubeext(cube &c, int maxverts = 0, bool init = true);
extern void getcubevector(cube &c, int d, int x, int y, int z, ivec &p);
extern void setcubevector(cube &c, int d, int x, int y, int z, const ivec &p);
extern int familysize(const cube &c);
extern void freeocta(cube *c);
extern void discardchildren(cube &c, bool fixtex = false, int depth = 0);
extern void optiface(uchar *p, cube &c);
extern void validatec(cube *c, int size = 0);
extern bool isvalidcube(const cube &c);
extern ivec lu;
extern int lusize;
extern cube &lookupcube(const ivec &to, int tsize = 0, ivec &ro = lu, int &rsize = lusize);
extern const cube *neighbourstack[32];
extern int neighbourdepth;
extern const cube &neighbourcube(const cube &c, int orient, const ivec &co, int size, ivec &ro = lu, int &rsize = lusize);
extern void resetclipplanes();
extern int getmippedtexture(const cube &p, int orient);
extern void forcemip(cube &c, bool fixtex = true);
extern bool subdividecube(cube &c, bool fullcheck=true, bool brighten=true);
extern void edgespan2vectorcube(cube &c);
extern int faceconvexity(const ivec v[4]);
extern int faceconvexity(const ivec v[4], int &vis);
extern int faceconvexity(const vertinfo *verts, int numverts, int size);
extern int faceconvexity(const cube &c, int orient);
extern void calcvert(const cube &c, const ivec &co, int size, ivec &vert, int i, bool solid = false);
extern void calcvert(const cube &c, const ivec &co, int size, vec &vert, int i, bool solid = false);
extern uint faceedges(const cube &c, int orient);
extern bool collapsedface(const cube &c, int orient);
extern bool touchingface(const cube &c, int orient);
extern bool flataxisface(const cube &c, int orient);
extern bool collideface(const cube &c, int orient);
extern int genclipplane(const cube &c, int i, vec *v, plane *clip);
extern void genclipplanes(const cube &c, const ivec &co, int size, clipplanes &p, bool collide = true);
extern bool visibleface(const cube &c, int orient, const ivec &co, int size, ushort mat = MAT_AIR, ushort nmat = MAT_AIR, ushort matmask = MATF_VOLUME);
extern int classifyface(const cube &c, int orient, const ivec &co, int size);
extern int visibletris(const cube &c, int orient, const ivec &co, int size, ushort nmat = MAT_ALPHA, ushort matmask = MAT_ALPHA);
extern int visibleorient(const cube &c, int orient);
extern void genfaceverts(const cube &c, int orient, ivec v[4]);
extern int calcmergedsize(int orient, const ivec &co, int size, const vertinfo *verts, int numverts);
extern void invalidatemerges(cube &c, const ivec &co, int size, bool msg);
extern void calcmerges();

extern int mergefaces(int orient, facebounds *m, int sz);
extern void mincubeface(const cube &cu, int orient, const ivec &o, int size, const facebounds &orig, facebounds &cf, ushort nmat = MAT_AIR, ushort matmask = MATF_VOLUME);

static inline cubeext &ext(cube &c)
{
    return *(c.ext ? c.ext : newcubeext(c));
}

// ents
extern char *entname(entity &e);
extern bool haveselent();
extern undoblock *copyundoents(undoblock *u);
extern void pasteundoent(int idx, const entity &ue);
extern void pasteundoents(undoblock *u);

// octaedit
extern void cancelsel();
extern void rendertexturepanel(int w, int h);
extern void addundo(undoblock *u);
extern void commitchanges(bool force = false);
extern void rendereditcursor();
extern void tryedit();

extern bool prefabloaded(const char *name);
extern void renderprefab(const char *name, const vec &o, float yaw, float pitch, float roll, float size = 1, const vec &color = vec(1, 1, 1));
extern void previewprefab(const char *name, const vec &color);

// octarender
extern vector<tjoint> tjoints;
extern vector<vtxarray *> varoot, valist;

extern ushort encodenormal(const vec &n);
extern vec decodenormal(ushort norm);
extern void guessnormals(const vec *pos, int numverts, vec *normals);
extern void reduceslope(ivec &n);
extern void findtjoints();
extern void octarender();
extern void allchanged(bool load = false);
extern void clearvas(cube *c);
extern void destroyva(vtxarray *va, bool reparent = true);
extern bool readva(vtxarray *va, ushort *&edata, vertex *&vdata);
extern void updatevabb(vtxarray *va, bool force = false);
extern void updatevabbs(bool force = false);

// renderva
extern vtxarray *visibleva, *reflectedva;

extern void visiblecubes(bool cull = true);
extern void setvfcP(float z = -1, const vec &bbmin = vec(-1, -1, -1), const vec &bbmax = vec(1, 1, 1));
extern void savevfcP();
extern void restorevfcP();
extern void rendergeom(float causticspass = 0, bool fogpass = false);
extern void renderalphageom(bool fogpass = false);
extern void rendermapmodels();
extern void renderreflectedgeom(bool causticspass = false, bool fogpass = false);
extern void renderreflectedmapmodels();
extern void renderoutline();
extern bool rendersky(bool explicitonly = false);

extern bool isfoggedsphere(float rad, const vec &cv);
extern int isvisiblesphere(float rad, const vec &cv);
extern bool bboccluded(const ivec &bo, const ivec &br);
extern occludequery *newquery(void *owner);
extern void startquery(occludequery *query);
extern void endquery(occludequery *query);
extern bool checkquery(occludequery *query, bool nowait = false);
extern void resetqueries();
extern int getnumqueries();
extern void startbb(bool mask = true);
extern void endbb(bool mask = true);
extern void drawbb(const ivec &bo, const ivec &br);

extern int oqfrags;

// dynlight

extern void updatedynlights();
extern int finddynlights();
extern void calcdynlightmask(vtxarray *va);
extern int setdynlights(vtxarray *va);
extern bool getdynlight(int n, vec &o, float &radius, vec &color);

// material

extern int showmat;

extern int findmaterial(const char *name);
extern const char *findmaterialname(int mat);
extern const char *getmaterialdesc(int mat, const char *prefix = "");
extern void genmatsurfs(const cube &c, const ivec &co, int size, vector<materialsurface> &matsurfs);
extern void rendermatsurfs(materialsurface *matbuf, int matsurfs);
extern void rendermatgrid(materialsurface *matbuf, int matsurfs);
extern int optimizematsurfs(materialsurface *matbuf, int matsurfs);
extern void setupmaterials(int start = 0, int len = 0);
extern void rendermaterials();
extern int visiblematerial(const cube &c, int orient, const ivec &co, int size, ushort matmask = MATF_VOLUME);

// water
extern int refracting, refractfog;
extern bool reflecting, fading, fogging;
extern float reflectz;
extern int reflectdist, vertwater, waterrefract, waterreflect, waterfade, caustics, waterfallrefract;

#define GETMATIDXVAR(name, var, type) \
    type get##name##var(int mat) \
    { \
        switch(mat&MATF_INDEX) \
        { \
            default: case 0: return name##var; \
            case 1: return name##2##var; \
            case 2: return name##3##var; \
            case 3: return name##4##var; \
        } \
    }

extern const bvec &getwatercolor(int mat);
extern const bvec &getwaterfallcolor(int mat);
extern int getwaterfog(int mat);
extern const bvec &getlavacolor(int mat);
extern int getlavafog(int mat);
extern const bvec &getglasscolor(int mat);

extern void cleanreflections();
extern void queryreflections();
extern void drawreflections();
extern void renderwater();
extern void setuplava(Texture *tex, float scale);
extern void renderlava(const materialsurface &m);
extern void flushlava();
extern void loadcaustics(bool force = false);
extern void preloadwatershaders(bool force = false);

// glare
extern bool glaring;

extern void drawglaretex();
extern void addglare();

// depthfx
extern bool depthfxing;

extern void drawdepthfxtex();

// server
extern vector<const char *> gameargs;

extern void initserver(bool listen, bool dedicated);
extern void cleanupserver();
extern void serverslice(bool dedicated, uint timeout);
extern void updatetime();

extern ENetSocket connectmaster(bool wait);
extern void localclienttoserver(int chan, ENetPacket *);
extern void localconnect();
extern bool serveroption(char *opt);

// serverbrowser
extern bool resolverwait(const char *name, ENetAddress *address);
extern int connectwithtimeout(ENetSocket sock, const char *hostname, const ENetAddress &address);
extern void addserver(const char *name, int port = 0, const char *password = NULL, bool keep = false);
extern void writeservercfg();

// client
extern void localdisconnect(bool cleanup = true);
extern void localservertoclient(int chan, ENetPacket *packet);
extern void connectserv(const char *servername, int port, const char *serverpassword);
extern void abortconnect();
extern void clientkeepalive();

// command
extern hashnameset<ident> idents;
extern int identflags;

extern void clearoverrides();
extern void writecfg(const char *name = NULL);

extern void checksleep(int millis);
extern void clearsleep(bool clearoverrides = true);

// console
extern void processtextinput(const char *str, int len);
extern void processkey(int code, bool isdown, int modstate = 0);
extern int rendercommand(int x, int y, int w);
extern int renderconsole(int w, int h, int abovehud);
extern void conoutf(const char *s, ...) PRINTFARGS(1, 2);
extern void conoutf(int type, const char *s, ...) PRINTFARGS(2, 3);
extern void resetcomplete();
extern void complete(char *s, int maxlen, const char *cmdprefix);
const char *getkeyname(int code);
extern const char *addreleaseaction(char *s);
extern void writebinds(stream *f);
extern void writecompletions(stream *f);

// main
enum
{
    NOT_INITING = 0,
    INIT_GAME,
    INIT_LOAD,
    INIT_RESET
};
extern int initing, numcpus;

enum
{
    CHANGE_GFX   = 1<<0,
    CHANGE_SOUND = 1<<1
};
extern bool initwarning(const char *desc, int level = INIT_RESET, int type = CHANGE_GFX);

extern bool grabinput, minimized;

extern bool interceptkey(int sym);

extern float loadprogress;
extern void renderbackground(const char *caption = NULL, Texture *mapshot = NULL, const char *mapname = NULL, const char *mapinfo = NULL, bool restore = false, bool force = false);
extern void renderprogress(float bar, const char *text, GLuint tex = 0, bool background = false);

extern void getfps(int &fps, int &bestdiff, int &worstdiff);
extern void swapbuffers(bool overlay = true);
extern int getclockmillis();

enum { KR_CONSOLE = 1<<0, KR_GUI = 1<<1, KR_EDITMODE = 1<<2 };

extern void keyrepeat(bool on, int mask = ~0);

enum { TI_CONSOLE = 1<<0, TI_GUI = 1<<1 };

extern void textinput(bool on, int mask = ~0);

// menu
extern void menuprocess();
extern void addchange(const char *desc, int type);
extern void clearchanges(int type);

// physics
extern void mousemove(int dx, int dy);
extern bool overlapsdynent(const vec &o, float radius);
extern void rotatebb(vec &center, vec &radius, int yaw);
extern float shadowray(const vec &o, const vec &ray, float radius, int mode, extentity *t = NULL);
struct ShadowRayCache;
extern ShadowRayCache *newshadowraycache();
extern void freeshadowraycache(ShadowRayCache *&cache);
extern void resetshadowraycache(ShadowRayCache *cache);
extern float shadowray(ShadowRayCache *cache, const vec &o, const vec &ray, float radius, int mode, extentity *t = NULL);

// world

extern vector<int> outsideents;

extern void entcancel();
extern void entitiesinoctanodes();
extern void attachentities();
extern void freeoctaentities(cube &c);
extern bool pointinsel(const selinfo &sel, const vec &o);

extern void resetmap();
extern void startmap(const char *name);

// rendermodel
struct mapmodelinfo { string name; model *m; };

extern bool modelloaded(const char *name);
extern void findanims(const char *pattern, vector<int> &anims);
extern void loadskin(const char *dir, const char *altdir, Texture *&skin, Texture *&masks);
extern mapmodelinfo *getmminfo(int i);
extern void startmodelquery(occludequery *query);
extern void endmodelquery();
extern void preloadmodelshaders(bool force = false);
extern void preloadusedmapmodels(bool msg = false, bool bih = false);

static inline model *loadmapmodel(int n)
{
    extern vector<mapmodelinfo> mapmodels;
    if(mapmodels.inrange(n))
    {
        model *m = mapmodels[n].m;
        return m ? m : loadmodel(NULL, n);
    }
    return NULL;
}

// renderparticles
extern void initparticles();
extern void clearparticles();
extern void clearparticleemitters();
extern void seedparticles();
extern void updateparticles();
extern void renderparticles(bool mainpass = false);
extern bool printparticles(extentity &e, char *buf, int len);
struct hwrtpartlabframe
{
    int nexp, nsmoke;
    int ageexp_min, ageexp_max;
    int agesmoke_min, agesmoke_max;
    float growth;
    int inside;
    float ox, oy, oz;
    int kind, dist;
};
extern void hwrtpartlabstats(hwrtpartlabframe &st);
extern void hwrtpartbiasdraw();
extern void hwrtdlaabiascapture();
extern void hwrtdlaabiasreset();

// decal
extern void initdecals();
extern void cleardecals();
extern void renderdecals(bool mainpass = false);

// blob

enum
{
    BLOB_STATIC = 0,
    BLOB_DYNAMIC
};

extern int showblobs;

extern void initblobs(int type = -1);
extern void resetblobs();
extern void renderblob(int type, const vec &o, float radius, float fade = 1);
extern void flushblobs();

// rendersky
extern int explicitsky;
extern double skyarea;
extern char *skybox;

extern void setupsky();
extern void drawskybox(int farplane, bool limited, bool force = false);
extern bool limitsky();
extern bool shouldrenderskyenvmap();
extern bool shouldclearskyboxglare();

// 3dgui
extern void g3d_render();
extern void g3d_render2d();
extern bool g3d_windowhit(bool on, bool act);
extern bool g3d_key(int code, bool isdown);
extern bool g3d_input(const char *str, int len);
// menus
extern int mainmenu;
extern bool gui2dvisible();
extern bool overlaycursor();

extern void clearmainmenu();
extern void g3d_mainmenu();

// sound
extern void clearmapsounds();
extern void checkmapsounds();
extern void updatesounds();
extern void preloadmapsounds();

extern void initmumble();
extern void closemumble();
extern void updatemumble();

// grass
extern void generategrass();
extern void rendergrass();
extern void cleanupgrass();

// blendmap
extern int blendpaintmode;

struct BlendMapCache;
extern BlendMapCache *newblendmapcache();
extern void freeblendmapcache(BlendMapCache *&cache);
extern bool setblendmaporigin(BlendMapCache *cache, const ivec &o, int size);
extern bool hasblendmap(BlendMapCache *cache);
extern uchar lookupblendmap(BlendMapCache *cache, const vec &pos);
extern void resetblendmap();
extern void enlargeblendmap();
extern void shrinkblendmap(int octant);
extern void optimizeblendmap();
extern void stoppaintblendmap();
extern void trypaintblendmap();
extern void renderblendbrush(GLuint tex, float x, float y, float w, float h);
extern void renderblendbrush();
extern bool loadblendmap(stream *f, int info);
extern void saveblendmap(stream *f);
extern uchar shouldsaveblendmap();

// recorder

namespace recorder
{
    extern void stop();
    extern void capture(bool overlay = true);
    extern void cleanup();
}

#endif

#endif

