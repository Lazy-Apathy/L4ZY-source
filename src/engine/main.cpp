// main.cpp: initialisation & main loop

#include "engine.h"

#ifdef SDL_VIDEO_DRIVER_X11
#include "SDL_syswm.h"
#endif

SVAR(tradversion, SAUER_TRAD_VERSION);

extern void cleargamma();
extern void restoredesktopwindow();
extern void logwindowstate(const char *when);

static bool shuttingdown = false;

void cleanup()
{
    shuttingdown = true;
    hdrout_shutdown();
    matchrec::stop();
    recorder::stop();
    hwrtcleanup();
    cleanupserver();
    SDL_ShowCursor(SDL_TRUE);
    SDL_SetRelativeMouseMode(SDL_FALSE);
    if(screen) SDL_SetWindowGrab(screen, SDL_FALSE);
    cleargamma();
    // Leave covering/exclusive before destroying the window so NVIDIA OpenGL
    // present on a native-size surface does not keep HDR/SDR brightness stuck.
    restoredesktopwindow();
    freeocta(worldroot);
    extern void clear_command(); clear_command();
    extern void clear_console(); clear_console();
    extern void clear_mdls();    clear_mdls();
    extern void clear_sound();   clear_sound();
    closelogfile();
    SDL_Quit();
}

extern void writeinitcfg();

void quit()                     // normal exit
{
    hwrtflushvelshot();
    writeinitcfg();
    writeservercfg();
    abortconnect();
    disconnect();
    localdisconnect();
    writecfg();
    cleanup();
    exit(EXIT_SUCCESS);
}

void fatal(const char *s, ...)    // failure exit
{
    static int errors = 0;
    errors++;

    if(errors <= 2) // print up to one extra recursive error
    {
        defvformatstring(msg,s,s);
        logoutf("%s", msg);

        if(errors <= 1) // avoid recursion
        {
            if(SDL_WasInit(SDL_INIT_VIDEO))
            {
                SDL_ShowCursor(SDL_TRUE);
                SDL_SetRelativeMouseMode(SDL_FALSE);
                if(screen) SDL_SetWindowGrab(screen, SDL_FALSE);
                cleargamma();
                restoredesktopwindow();
            }
            SDL_Quit();
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Cube 2: Sauerbraten fatal error", msg, NULL);
        }
    }

    exit(EXIT_FAILURE);
}

int curtime = 0, lastmillis = 1, elapsedtime = 0, totalmillis = 1;
// 0 = wall-clock dt (play). Lab particle comparisons set this in once.cfg only.
VAR(hwrtdlaafixedcurtime, 0, 0, 200);

dynent *player = NULL;

int initing = NOT_INITING;

bool initwarning(const char *desc, int level, int type)
{
    if(initing < level) 
    {
        addchange(desc, type);
        return true;
    }
    return false;
}

VAR(desktopw, 1, 0, 0);
VAR(desktoph, 1, 0, 0);
int screenw = 0, screenh = 0;
SDL_Window *screen = NULL;
SDL_GLContext glcontext = NULL;

#define SCR_MINW 320
#define SCR_MINH 200
#define SCR_MAXW 10000
#define SCR_MAXH 10000
#define SCR_DEFAULTW 1024
#define SCR_DEFAULTH 768
VARF(scr_w, SCR_MINW, -1, SCR_MAXW, initwarning("screen resolution"));
VARF(scr_h, SCR_MINH, -1, SCR_MAXH, initwarning("screen resolution"));
VARF(depthbits, 0, 0, 32, initwarning("depth-buffer precision"));
VARF(fsaa, -1, -1, 16, initwarning("anti-aliasing"));

void writeinitcfg()
{
    stream *f = openutf8file("init.cfg", "w");
    if(!f) return;
    f->printf("// automatically written on exit, DO NOT MODIFY\n// modify settings in game\n");
    extern int fullscreen, fullscreendesktop, windowmode;
    f->printf("windowmode %d\n", windowmode);
    f->printf("fullscreen %d\n", fullscreen);
    f->printf("fullscreendesktop %d\n", fullscreendesktop);
    f->printf("scr_w %d\n", scr_w);
    f->printf("scr_h %d\n", scr_h);
    f->printf("depthbits %d\n", depthbits);
    f->printf("fsaa %d\n", fsaa);
    extern int usesound, soundchans, soundfreq, soundbufferlen;
    extern char *audiodriver;
    f->printf("usesound %d\n", usesound);
    f->printf("soundchans %d\n", soundchans);
    f->printf("soundfreq %d\n", soundfreq);
    f->printf("soundbufferlen %d\n", soundbufferlen);
    if(audiodriver[0]) f->printf("audiodriver %s\n", escapestring(audiodriver));
    delete f;
}

COMMAND(quit, "");

static void getbackgroundres(int &w, int &h)
{
    float wk = 1, hk = 1;
    if(w < 1024) wk = 1024.0f/w;
    if(h < 768) hk = 768.0f/h;
    wk = hk = max(wk, hk);
    w = int(ceil(w*wk));
    h = int(ceil(h*hk));
}

string backgroundcaption = "";
Texture *backgroundmapshot = NULL;
string backgroundmapname = "";
char *backgroundmapinfo = NULL;

void setbackgroundinfo(const char *caption = NULL, Texture *mapshot = NULL, const char *mapname = NULL, const char *mapinfo = NULL)
{
    renderedframe = false;
    copystring(backgroundcaption, caption ? caption : "");
    backgroundmapshot = mapshot;
    copystring(backgroundmapname, mapname ? mapname : "");
    if(mapinfo != backgroundmapinfo)
    {
        DELETEA(backgroundmapinfo);
        if(mapinfo) backgroundmapinfo = newstring(mapinfo);
    }
}

void restorebackground(bool force = false)
{
    if(renderedframe)
    {
        if(!force) return;
        setbackgroundinfo();
    }
    renderbackground(backgroundcaption[0] ? backgroundcaption : NULL, backgroundmapshot, backgroundmapname[0] ? backgroundmapname : NULL, backgroundmapinfo, true);
}

void bgquad(float x, float y, float w, float h, float tx = 0, float ty = 0, float tw = 1, float th = 1)
{
    gle::begin(GL_TRIANGLE_STRIP);
    gle::attribf(x,   y);   gle::attribf(tx,      ty);
    gle::attribf(x+w, y);   gle::attribf(tx + tw, ty);
    gle::attribf(x,   y+h); gle::attribf(tx,      ty + th);
    gle::attribf(x+w, y+h); gle::attribf(tx + tw, ty + th);
    gle::end();
}

static void bgrect(float x, float y, float w, float h)
{
    gle::defvertex(2);
    gle::begin(GL_TRIANGLE_STRIP);
    gle::attribf(x, y);
    gle::attribf(x+w, y);
    gle::attribf(x, y+h);
    gle::attribf(x+w, y+h);
    gle::end();
}

static void bgtextscaled(const char *s, float x, float y, float scale, int r, int g, int b)
{
    pushhudmatrix();
    hudmatrix.translate(x, y, 0);
    hudmatrix.scale(scale, scale, 1);
    flushhudmatrix();
    hudshader->set();
    draw_text(s, 0, 0, r, g, b);
    pophudmatrix();
}

SVAR(mainmenubg, "packages/base/gothic-df.jpg");

static void rendermenuscene(int w, int h)
{
    Texture *t = textureload(mainmenubg, 3, true, false);
    if(t == notexture) t = textureload("packages/base/industry.jpg", 3, true, false);
    if(t == notexture) t = textureload("data/background.png", 0, true, false);
    float ia = t->ys > 0 ? float(t->xs)/float(t->ys) : 1.0f;
    float sa = h > 0 ? float(w)/float(h) : 1.0f;
    float tw = 1, th = 1, tx = 0, ty = 0;
    if(ia > sa) { tw = sa/ia; tx = 0.5f*(1 - tw); }
    else { th = ia/sa; ty = 0.5f*(1 - th); }
    hudshader->set();
    gle::colorf(0.42f, 0.44f, 0.48f);
    gle::defvertex(2);
    gle::deftexcoord0();
    glBindTexture(GL_TEXTURE_2D, t->id);
    bgquad(0, 0, w, h, tx, ty, tw, th);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    hudnotextureshader->set();
    gle::colorub(0, 0, 0, 110);
    gle::defvertex(2);
    gle::begin(GL_TRIANGLE_STRIP);
    gle::attribf(0, 0);
    gle::attribf(float(w), 0);
    gle::attribf(0, float(h));
    gle::attribf(float(w), float(h));
    gle::end();
    hudshader->set();
    glDisable(GL_BLEND);
}

static void drawloadchrome(int w, int h, const char *caption, Texture *mapshot, const char *mapname, const char *mapinfo)
{
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    float stripe = max(3.0f, w * 0.0035f);
    hudnotextureshader->set();
    gle::colorub(255, 122, 24, 50);
    bgrect(0, 0, stripe, float(h));

    const char *gametitle = "L4ZY";
    float titlesz = 0.042f * min(w, h) / FONTH;
    float tx = w * 0.055f;
    float ty = h * 0.07f;
    bgtextscaled(gametitle, tx, ty, titlesz, 255, 122, 24);
    int tw = text_width(gametitle);
    hudnotextureshader->set();
    gle::colorub(255, 122, 24, 200);
    bgrect(tx, ty + FONTH * titlesz - max(2.0f, titlesz * 6), tw * titlesz, max(2.0f, titlesz * 3));
    defformatstring(sub, "Sauerbraten RT  %s", SAUER_TRAD_VERSION);
    bgtextscaled(sub, tx, ty + FONTH * titlesz * 1.15f, titlesz * 0.42f, 138, 145, 153);

    if(mapshot || mapname)
    {
        int infowidth = 12 * FONTH;
        float sz = 0.32f * min(w, h), msz = (0.70f * min(w, h) - sz) / (infowidth + FONTH);
        float x = 0.5f * (w - sz), y = 0.28f * h;
        if(mapinfo)
        {
            int mw, mh;
            text_bounds(mapinfo, mw, mh, infowidth);
            x -= 0.5f * (mw * msz + FONTH * msz);
        }
        hudshader->set();
        gle::colorf(1, 1, 1);
        gle::defvertex(2);
        gle::deftexcoord0();
        if(mapshot && mapshot != notexture)
        {
            glBindTexture(GL_TEXTURE_2D, mapshot->id);
            bgquad(x, y, sz, sz);
        }
        else
        {
            hudnotextureshader->set();
            gle::colorub(10, 12, 16, 200);
            bgrect(x, y, sz, sz);
            int qw, qh;
            text_bounds("?", qw, qh);
            float qsz = sz * 0.5f / max(qw, qh);
            bgtextscaled("?", x + 0.5f * (sz - qw * qsz), y + 0.5f * (sz - qh * qsz), qsz, 255, 255, 255);
        }
        float b = max(2.0f, sz * 0.012f);
        hudnotextureshader->set();
        gle::colorub(255, 122, 24, 210);
        bgrect(x, y, sz, b);
        bgrect(x, y + sz - b, sz, b);
        bgrect(x, y, b, sz);
        bgrect(x + sz - b, y, b, sz);
        if(mapname)
        {
            int ntw = text_width(mapname);
            float nsz = sz / (10 * FONTH);
            if(ntw * nsz > sz * 0.9f) nsz = sz * 0.9f / max(ntw, 1);
            bgtextscaled(mapname, x + 0.5f * (sz - ntw * nsz), y + sz + FONTH * nsz * 0.25f, nsz, 255, 255, 255);
        }
        if(mapinfo)
        {
            pushhudmatrix();
            hudmatrix.translate(x + sz + FONTH * msz, y, 0);
            hudmatrix.scale(msz, msz, 1);
            flushhudmatrix();
            hudshader->set();
            draw_text(mapinfo, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, -1, infowidth);
            pophudmatrix();
        }
    }

    hudshader->set();
    glDisable(GL_BLEND);
}

void renderbackground(const char *caption, Texture *mapshot, const char *mapname, const char *mapinfo, bool restore, bool force)
{
    if(!inbetweenframes && !force) return;

    if(!restore || force) stopsounds(); // stop sounds while loading
 
    int w = screenw, h = screenh;
    if(forceaspect) w = int(ceil(h*forceaspect));
    getbackgroundres(w, h);
    gettextres(w, h);

    loopi(restore ? 1 : 3)
    {
        hdrout_begin_overlay();
        hudmatrix.ortho(0, w, h, 0, -1, 1);
        resethudmatrix();
        rendermenuscene(w, h);
        if(caption || mapshot || mapname) drawloadchrome(w, h, caption, mapshot, mapname, mapinfo);
        if(!restore) swapbuffers(false);
    }

    if(!restore) setbackgroundinfo(caption, mapshot, mapname, mapinfo);
}

VAR(progressbackground, 0, 0, 1);

float loadprogress = 0;

void renderprogress(float bar, const char *text, GLuint tex, bool background)   // also used during loading
{
    if(!inbetweenframes || drawtex) return;

    extern int menufps, maxfps;
    int fps = menufps ? (maxfps ? min(maxfps, menufps) : menufps) : maxfps;
    if(fps)
    {
        static int lastprogress = 0;
        int ticks = SDL_GetTicks(), diff = ticks - lastprogress;
        if(bar > 0 && diff >= 0 && diff < (1000 + fps-1)/fps) return;
        lastprogress = ticks;
    }

    clientkeepalive();      // make sure our connection doesn't time out while loading maps etc.
    
    SDL_PumpEvents(); // keep the event queue awake to avoid 'beachball' cursor

    extern int mesa_swap_bug, curvsync;
    bool forcebackground = progressbackground || (mesa_swap_bug && (curvsync || totalmillis==1));
    hdrout_begin_overlay();
    if(background || forcebackground) restorebackground(forcebackground);

    int w = screenw, h = screenh;
    if(forceaspect) w = int(ceil(h*forceaspect));
    getbackgroundres(w, h);
    gettextres(w, h);

    hudmatrix.ortho(0, w, h, 0, -1, 1);
    resethudmatrix();

    float fw, fh, fx, fy, pad;
    hudnotextureshader->set();
    glDisable(GL_BLEND);
    if(!renderedframe)
    {
        float plateh = 0.12f * min(w, h);
        gle::colorub(8, 10, 14, 255);
        bgrect(0, float(h) - plateh, float(w), plateh);
        fw = 0.46f * w;
        fh = max(12.0f, 0.018f * h);
        fx = 0.5f * (w - fw);
        fy = h - 0.035f * min(w, h) - fh;
        if(backgroundcaption[0])
        {
            int ctw = text_width(backgroundcaption);
            float csz = 0.028f * min(w, h) / FONTH;
            float cx = 0.5f * (w - ctw * csz);
            float cy = fy - FONTH * csz - 0.012f * h;
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            bgtextscaled(backgroundcaption, cx, cy, csz, 255, 255, 255);
            glDisable(GL_BLEND);
            hudnotextureshader->set();
        }
    }
    else
    {
        fw = 0.28f * min(w, h);
        fh = max(10.0f, 0.016f * h);
        fx = w - fw - 0.03f * min(w, h);
        fy = 0.03f * min(w, h);
        gle::colorub(8, 10, 14, 255);
        bgrect(fx - 8, fy - 8, fw + 16, fh + 16);
    }
    pad = max(2.0f, fh * 0.16f);

    gle::colorub(18, 20, 24, 255);
    bgrect(fx, fy, fw, fh);
    gle::colorub(255, 122, 24, 255);
    bgrect(fx, fy, fw, pad);
    bgrect(fx, fy + fh - pad, fw, pad);
    bgrect(fx, fy, pad, fh);
    bgrect(fx + fw - pad, fy, pad, fh);
    if(bar > 0)
    {
        float inner = max(0.0f, fw - pad * 2);
        gle::colorub(255, 122, 24, 255);
        bgrect(fx + pad, fy + pad, inner * clamp(bar, 0.0f, 1.0f), fh - pad * 2);
    }

    if(text)
    {
        int tw = text_width(text);
        float tsz = (fh * 0.62f) / FONTH;
        if(tw * tsz > fw - pad * 4) tsz = (fw - pad * 4) / max(tw, 1);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        bgtextscaled(text, fx + 0.5f * (fw - tw * tsz), fy + 0.5f * (fh - FONTH * tsz), tsz, 255, 255, 255);
        glDisable(GL_BLEND);
    }

    if(tex)
    {
        float sz = 0.28f * min(w, h), x = 0.5f * (w - sz), y = 0.32f * h;
        hudshader->set();
        gle::colorf(1, 1, 1);
        gle::defvertex(2);
        gle::deftexcoord0();
        glBindTexture(GL_TEXTURE_2D, tex);
        bgquad(x, y, sz, sz);
        float b = max(2.0f, sz * 0.012f);
        hudnotextureshader->set();
        gle::colorub(255, 122, 24, 255);
        bgrect(x, y, sz, b);
        bgrect(x, y + sz - b, sz, b);
        bgrect(x, y, b, sz);
        bgrect(x + sz - b, y, b, sz);
    }

    hudshader->set();
    swapbuffers(false);
}

int keyrepeatmask = 0, textinputmask = 0;
Uint32 textinputtime = 0;
VAR(textinputfilter, 0, 5, 1000);

void keyrepeat(bool on, int mask)
{
    if(on) keyrepeatmask |= mask;
    else keyrepeatmask &= ~mask;
}

void textinput(bool on, int mask)
{
    if(on)
    {
        if(!textinputmask)
        {
            SDL_StartTextInput();
            textinputtime = SDL_GetTicks();
        }
        textinputmask |= mask;
    }
    else if(textinputmask)
    {
        textinputmask &= ~mask;
        if(!textinputmask) SDL_StopTextInput();
    }
}

#ifdef WIN32
// SDL_WarpMouseInWindow behaves erratically on Windows, so force relative mouse instead.
VARN(relativemouse, userelativemouse, 1, 1, 0);
#else
VARNP(relativemouse, userelativemouse, 0, 1, 1);
#endif

bool shouldgrab = false, grabinput = false, minimized = false, canrelativemouse = true, relativemouse = false;

#ifdef SDL_VIDEO_DRIVER_X11
VAR(sdl_xgrab_bug, 0, 0, 1);
#endif

void inputgrab(bool on, bool delay = false)
{
#ifdef SDL_VIDEO_DRIVER_X11
    bool wasrelativemouse = relativemouse;
#endif
    if(on)
    {
        SDL_ShowCursor(SDL_FALSE);
        if(canrelativemouse && userelativemouse)
        {
            if(SDL_SetRelativeMouseMode(SDL_TRUE) >= 0)
            {
                SDL_SetWindowGrab(screen, SDL_TRUE);
                relativemouse = true;
            }
            else
            {
                SDL_SetWindowGrab(screen, SDL_FALSE);
                canrelativemouse = false;
                relativemouse = false;
            }
        }
    }
    else
    {
        SDL_SetWindowGrab(screen, SDL_FALSE);
        SDL_SetRelativeMouseMode(SDL_FALSE);
        relativemouse = false;
        // Keep the OS cursor hidden over the game: Sauer draws its own. Windows
        // still shows a cursor on the other screen once the pointer leaves.
        SDL_ShowCursor(SDL_FALSE);
    }
    shouldgrab = delay;

#ifdef SDL_VIDEO_DRIVER_X11
    if((relativemouse || wasrelativemouse) && sdl_xgrab_bug)
    {
        // Workaround for buggy SDL X11 pointer grabbing
        union { SDL_SysWMinfo info; uchar buf[sizeof(SDL_SysWMinfo) + 128]; };
        SDL_GetVersion(&info.version);
        if(SDL_GetWindowWMInfo(screen, &info) && info.subsystem == SDL_SYSWM_X11)
        {
            if(relativemouse)
            {
                uint mask = ButtonPressMask | ButtonReleaseMask | PointerMotionMask | FocusChangeMask;
                XGrabPointer(info.info.x11.display, info.info.x11.window, True, mask, GrabModeAsync, GrabModeAsync, info.info.x11.window, None, CurrentTime);
            }
            else XUngrabPointer(info.info.x11.display, CurrentTime);
        }
    }
#endif
}

static void ignoremousemotion();

static bool shouldgrabmouse()
{
    if(!screen) return false;
    if(!(SDL_GetWindowFlags(screen) & SDL_WINDOW_INPUT_FOCUS)) return false;
    if(gui2dvisible() || overlaycursor()) return false;
    return true;
}

static void syncinputgrab()
{
    bool want = shouldgrabmouse();
    if(want != grabinput)
    {
        inputgrab(grabinput = want);
        ignoremousemotion();
    }
    if(!grabinput && (gui2dvisible() || overlaycursor()) && screenw > 0 && screenh > 0)
    {
        int mx, my;
        SDL_GetMouseState(&mx, &my);
        g3d_setcursor(mx / float(screenw), my / float(screenh));
    }
}

bool initwindowpos = false;

enum { WM_WINDOWED = 0, WM_EXCLUSIVE = 1, WM_BORDERLESS = 2 };

static int lastuseddisplay = 0;
static bool applyingwindowmode = false;

static int currentdisplayindex()
{
    if(screen)
    {
        int d = SDL_GetWindowDisplayIndex(screen);
        if(d >= 0) { lastuseddisplay = d; return d; }
    }
    return lastuseddisplay;
}

static void getdisplaybounds(SDL_Rect &bounds)
{
    int display = currentdisplayindex();
    if(SDL_GetDisplayBounds(display, &bounds) < 0 && SDL_GetDisplayBounds(0, &bounds) < 0)
    {
        bounds.x = bounds.y = 0;
        bounds.w = desktopw > 0 ? desktopw : SCR_DEFAULTW;
        bounds.h = desktoph > 0 ? desktoph : SCR_DEFAULTH;
    }
    desktopw = bounds.w;
    desktoph = bounds.h;
}

void applywindowmode();
void logwindowstate(const char *when);
void restoredesktopwindow();

VARF(windowmode, 0, WM_BORDERLESS, 2, applywindowmode());

extern int fullscreen, fullscreendesktop;

static bool windowflagsexclusive()
{
    return screen && (SDL_GetWindowFlags(screen) & SDL_WINDOW_FULLSCREEN) != 0;
}

static bool windowcoversdesktop()
{
    if(!screen) return false;
    SDL_Rect bounds;
    getdisplaybounds(bounds);
    int w = 0, h = 0, x = 0, y = 0;
    SDL_GetWindowSize(screen, &w, &h);
    SDL_GetWindowPosition(screen, &x, &y);
    return w >= bounds.w && h >= bounds.h && x <= bounds.x + 8 && y <= bounds.y + 8;
}

void applywindowmode()
{
    if(!screen || applyingwindowmode || shuttingdown) return;

    Uint32 flags = SDL_GetWindowFlags(screen);
    bool exclusive = (flags & SDL_WINDOW_FULLSCREEN) != 0;
    int w = 0, h = 0;
    SDL_GetWindowSize(screen, &w, &h);
    SDL_Rect bounds;
    getdisplaybounds(bounds);

    bool already = false;
    switch(windowmode)
    {
        case WM_EXCLUSIVE:
            already = exclusive;
            break;
        case WM_BORDERLESS:
            already = !exclusive && (flags & SDL_WINDOW_BORDERLESS) && w == bounds.w && h == bounds.h;
            break;
        default:
            already = !exclusive && !(flags & SDL_WINDOW_BORDERLESS) && (flags & SDL_WINDOW_RESIZABLE);
            break;
    }
    if(already)
    {
        fullscreen = windowmode == WM_EXCLUSIVE ? 1 : 0;
        fullscreendesktop = 0;
        SDL_GetWindowSize(screen, &screenw, &screenh);
        logoutf("video: windowmode %d already applied, skip", windowmode);
        return;
    }

    applyingwindowmode = true;
    logoutf("video: apply windowmode %d (flags=0x%x %dx%d exclusive=%d covering=%d)",
            windowmode, (unsigned)flags, w, h, exclusive ? 1 : 0, windowcoversdesktop() ? 1 : 0);

    // Only leave exclusive fullscreen when we are actually leaving it.
    // Calling SDL_SetWindowFullscreen(0) on every apply caused extra
    // desktop transitions, including when autoexec restated the same mode.
    if(exclusive && windowmode != WM_EXCLUSIVE)
        SDL_SetWindowFullscreen(screen, 0);

    switch(windowmode)
    {
        case WM_EXCLUSIVE:
            SDL_SetWindowResizable(screen, SDL_FALSE);
            SDL_SetWindowBordered(screen, SDL_TRUE);
            SDL_SetWindowSize(screen, scr_w, scr_h);
            SDL_SetWindowFullscreen(screen, SDL_WINDOW_FULLSCREEN);
            initwindowpos = true;
            break;
        case WM_BORDERLESS:
            SDL_SetWindowResizable(screen, SDL_FALSE);
            SDL_SetWindowBordered(screen, SDL_FALSE);
            SDL_SetWindowPosition(screen, bounds.x, bounds.y);
            SDL_SetWindowSize(screen, bounds.w, bounds.h);
            SDL_SetWindowPosition(screen, bounds.x, bounds.y);
            initwindowpos = true;
            break;
        default:
            SDL_SetWindowBordered(screen, SDL_TRUE);
            SDL_SetWindowResizable(screen, SDL_TRUE);
            SDL_SetWindowSize(screen, scr_w, scr_h);
            SDL_SetWindowPosition(screen, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
            initwindowpos = false;
            break;
    }

    fullscreen = windowmode == WM_EXCLUSIVE ? 1 : 0;
    fullscreendesktop = 0;

    SDL_GetWindowSize(screen, &screenw, &screenh);
    if(glcontext) gl_resize();
    applyingwindowmode = false;
    logwindowstate("video: after applywindowmode");
}

// Kept for init.cfg / console. init.cfg `fullscreen` is ignored at boot so this
// copy can default to borderless; `fullscreen 1` later still means exclusive.
VARF(fullscreen, 0, 0, 1,{
    if(applyingwindowmode || initing == INIT_RESET) return;
    int want = fullscreen ? WM_EXCLUSIVE : WM_WINDOWED;
    if(windowmode != want)
    {
        windowmode = want;
        applywindowmode();
    }
});

VARF(fullscreendesktop, 0, 0, 1,
{
    if(applyingwindowmode || initing == INIT_RESET || !fullscreendesktop) return;
    // Old hidden cvar: redirect to real borderless, never SDL_WINDOW_FULLSCREEN_DESKTOP.
    if(windowmode != WM_BORDERLESS)
    {
        windowmode = WM_BORDERLESS;
        applywindowmode();
    }
});

void screenres(int w, int h)
{
    scr_w = clamp(w, SCR_MINW, SCR_MAXW);
    scr_h = clamp(h, SCR_MINH, SCR_MAXH);
    if(screen)
    {
        if(windowmode == WM_EXCLUSIVE) applywindowmode();
        else if(windowmode == WM_WINDOWED)
        {
            SDL_SetWindowSize(screen, scr_w, scr_h);
            SDL_SetWindowPosition(screen, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
            initwindowpos = false;
        }
        // borderless keeps native monitor size; scr_w/scr_h remembered for windowed/exclusive
    }
    else
    {
        initwarning("screen resolution");
    }
}

ICOMMAND(screenres, "ii", (int *w, int *h), screenres(*w, *h));

static void setgamma(int val)
{   
    if(screen && SDL_SetWindowBrightness(screen, val/100.0f) < 0) conoutf(CON_ERROR, "Could not set gamma: %s", SDL_GetError());
}   

static int curgamma = 100;
VARFNP(gamma, reqgamma, 30, 100, 300,
{
    if(initing || reqgamma == curgamma) return;
    curgamma = reqgamma;
    setgamma(curgamma);
});

void restoregamma()
{       
    if(initing || reqgamma == 100) return;
    curgamma = reqgamma;
    setgamma(curgamma);
}

void cleargamma()
{
    if(curgamma != 100 && screen) SDL_SetWindowBrightness(screen, 1.0f);
}

void logwindowstate(const char *when)
{
    if(!screen)
    {
        logoutf("%s: no window", when);
        return;
    }
    int w = 0, h = 0, x = 0, y = 0;
    SDL_GetWindowSize(screen, &w, &h);
    SDL_GetWindowPosition(screen, &x, &y);
    int display = SDL_GetWindowDisplayIndex(screen);
    Uint32 flags = SDL_GetWindowFlags(screen);
    SDL_Rect bounds;
    getdisplaybounds(bounds);
    logoutf("%s: windowmode %d flags=0x%x client %dx%d pos %d,%d display %d desktop %dx%d exclusive=%d covering=%d gamma_req %d gamma_cur %d",
            when, windowmode, (unsigned)flags, w, h, x, y, display, bounds.w, bounds.h,
            windowflagsexclusive() ? 1 : 0, windowcoversdesktop() ? 1 : 0, reqgamma, curgamma);
}

void restoredesktopwindow()
{
    if(!screen) return;
    bool exclusive = windowflagsexclusive();
    bool covering = windowcoversdesktop() || windowmode == WM_BORDERLESS || windowmode == WM_EXCLUSIVE;
    if(!exclusive && !covering)
    {
        logwindowstate("video: quit, window already not covering");
        return;
    }
    logwindowstate("video: leaving covering/exclusive before destroy");
    applyingwindowmode = true;
    if(exclusive) SDL_SetWindowFullscreen(screen, 0);
    SDL_SetWindowBordered(screen, SDL_TRUE);
    SDL_SetWindowResizable(screen, SDL_TRUE);
    int ww = clamp(scr_w, SCR_MINW, SCR_MAXW);
    int hh = clamp(scr_h, SCR_MINH, SCR_MAXH);
    SDL_Rect bounds;
    getdisplaybounds(bounds);
    // A native-size surface is what NVIDIA OpenGL promotes to exclusive on HDR.
    if(ww >= bounds.w) ww = max(SCR_MINW, bounds.w - 32);
    if(hh >= bounds.h) hh = max(SCR_MINH, bounds.h - 32);
    SDL_SetWindowSize(screen, ww, hh);
    SDL_SetWindowPosition(screen, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    applyingwindowmode = false;
    SDL_PumpEvents();
    SDL_Delay(100);
    SDL_PumpEvents();
    logwindowstate("video: restored windowed for quit");
}

int curvsync = -1;
void restorevsync()
{
    if(initing || !glcontext) return;
    extern int vsync, vsynctear;
    if(!SDL_GL_SetSwapInterval(vsync ? (vsynctear ? -1 : 1) : 0))
        curvsync = vsync;
}

VARFP(vsync, 0, 0, 1, restorevsync());
VARFP(vsynctear, 0, 0, 1, { if(vsync) restorevsync(); });

void setupscreen()
{
    int keepdisplay = currentdisplayindex();

    if(glcontext)
    {
        SDL_GL_DeleteContext(glcontext);
        glcontext = NULL;
    }
    if(screen)
    {
        SDL_DestroyWindow(screen);
        screen = NULL;
    }
    curvsync = -1;
    lastuseddisplay = keepdisplay;

    SDL_Rect bounds;
    if(SDL_GetDisplayBounds(keepdisplay, &bounds) < 0 && SDL_GetDisplayBounds(0, &bounds) < 0)
        fatal("failed querying desktop bounds: %s", SDL_GetError());
    desktopw = bounds.w;
    desktoph = bounds.h;

    if(scr_h < 0) scr_h = windowmode != WM_WINDOWED ? desktoph : SCR_DEFAULTH;
    if(scr_w < 0) scr_w = (scr_h*desktopw)/desktoph;
    scr_w = clamp(scr_w, SCR_MINW, SCR_MAXW);
    scr_h = clamp(scr_h, SCR_MINH, SCR_MAXH);

    int winx = SDL_WINDOWPOS_UNDEFINED, winy = SDL_WINDOWPOS_UNDEFINED, winw = scr_w, winh = scr_h, flags = 0;
    if(windowmode == WM_WINDOWED) flags |= SDL_WINDOW_RESIZABLE;
    else if(windowmode == WM_BORDERLESS)
    {
        winx = bounds.x;
        winy = bounds.y;
        winw = bounds.w;
        winh = bounds.h;
        flags |= SDL_WINDOW_BORDERLESS;
        initwindowpos = true;
    }
    else
    {
        flags |= SDL_WINDOW_FULLSCREEN;
        initwindowpos = true;
    }

    SDL_GL_ResetAttributes();
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, depthbits ? depthbits : 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    static const int configs[] =
    {
        0x3, /* try everything */
        0x2, 0x1, /* try disabling one at a time */
        0 /* try disabling everything */
    };
    int config = 0;
    if(!fsaa)
    {
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
    }
    loopi(sizeof(configs)/sizeof(configs[0]))
    {
        config = configs[i];
        if(!depthbits && config&1) continue;
        if(fsaa<=0 && config&2) continue;
        if(depthbits) SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, config&1 ? depthbits : 24);
        if(fsaa>0)
        {
            SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, config&2 ? 1 : 0);
            SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, config&2 ? fsaa : 0);
        }
        screen = SDL_CreateWindow("L4ZY", winx, winy, winw, winh, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS | flags);
        if(!screen) continue;

    #ifdef __APPLE__
        static const int glversions[] = { 32, 20 };
    #else
        static const int glversions[] = { 33, 32, 31, 30, 20 };
    #endif
        loopj(sizeof(glversions)/sizeof(glversions[0]))
        {
            glcompat = glversions[j] <= 30 ? 1 : 0;
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, glversions[j] / 10);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, glversions[j] % 10);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, glversions[j] >= 32 ? SDL_GL_CONTEXT_PROFILE_CORE : 0);
            glcontext = SDL_GL_CreateContext(screen);
            if(glcontext) break;
        }
        if(glcontext) break;
    }
    if(!screen) fatal("failed to create OpenGL window: %s", SDL_GetError());
    else if(!glcontext) fatal("failed to create OpenGL context: %s", SDL_GetError());
    else
    {
        if(depthbits && (config&1)==0) conoutf(CON_WARN, "%d bit z-buffer not supported - disabling", depthbits);
        if(fsaa>0 && (config&2)==0) conoutf(CON_WARN, "%dx anti-aliasing not supported - disabling", fsaa);
    }

    SDL_SetWindowMinimumSize(screen, SCR_MINW, SCR_MINH);
    SDL_SetWindowMaximumSize(screen, SCR_MAXW, SCR_MAXH);

    SDL_GetWindowSize(screen, &screenw, &screenh);
    logwindowstate("video: setupscreen");
}

void resetgl()
{
    clearchanges(CHANGE_GFX);

    renderbackground("Resetting OpenGL");

    extern void cleanupva();
    extern void cleanupparticles();
    extern void cleanupdecals();
    extern void cleanupblobs();
    extern void cleanupsky();
    extern void cleanupmodels();
    extern void cleanupprefabs();
    extern void cleanuplightmaps();
    extern void cleanupblendmap();
    extern void cleanshadowmap();
    extern void cleanreflections();
    extern void cleanupglare();
    extern void cleanupdepthfx();
    extern void cleanuplookao();
    extern void cleanuplooktaa();
    extern void cleanupvelocity();
    extern void cleanuptemporal();
    matchrec::cleanup();
    recorder::cleanup();
    cleanupva();
    cleanupparticles();
    cleanupdecals();
    cleanupblobs();
    cleanupsky();
    cleanupmodels();
    cleanupprefabs();
    cleanuptextures();
    cleanuplightmaps();
    cleanupblendmap();
    cleanshadowmap();
    cleanreflections();
    cleanupglare();
    cleanupdepthfx();
    cleanuplookao();
    cleanuplooktaa();
    cleanupvelocity();
    cleanuptemporal();
    cleanupshaders();
    cleanupgl();
    
    setupscreen();
    syncinputgrab();
    gl_init();

    inbetweenframes = false;
    if(!reloadtexture(*notexture) ||
       !reloadtexture("data/background.png"))
        fatal("failed to reload core texture");
    reloadfonts();
    inbetweenframes = true;
    renderbackground("Initializing...");
    restoregamma();
    restorevsync();
    reloadshaders();
    reloadtextures();
    initlights();
    allchanged(true);
}

COMMAND(resetgl, "");

static queue<SDL_Event, 32> events;

static inline bool filterevent(const SDL_Event &event)
{
    switch(event.type)
    {
        case SDL_MOUSEMOTION:
            if(grabinput && !relativemouse && !(SDL_GetWindowFlags(screen) & SDL_WINDOW_FULLSCREEN))
            {
                if(event.motion.x == screenw / 2 && event.motion.y == screenh / 2)
                    return false;  // ignore any motion events generated by SDL_WarpMouse
                #ifdef __APPLE__
                if(event.motion.y == 0)
                    return false;  // let mac users drag windows via the title bar
                #endif
            }
            break;
    }
    return true;
}

template <int SIZE> static inline bool pumpevents(queue<SDL_Event, SIZE> &events)
{
    while(events.empty())
    {
        SDL_PumpEvents();
        databuf<SDL_Event> buf = events.reserve(events.capacity());
        int n = SDL_PeepEvents(buf.getbuf(), buf.remaining(), SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT);
        if(n <= 0) return false;
        loopi(n) if(filterevent(buf.buf[i])) buf.put(buf.buf[i]);
        events.addbuf(buf);
    }
    return true;
}

static int interceptkeysym = 0;

static int interceptevents(void *data, SDL_Event *event)
{
    switch(event->type)
    {
        case SDL_MOUSEMOTION: return 0;
        case SDL_KEYDOWN:
            if(event->key.keysym.sym == interceptkeysym)
            {
                interceptkeysym = -interceptkeysym;
                return 0;
            }
            break;
    }
    return 1;
}

static void clearinterceptkey()
{
    SDL_DelEventWatch(interceptevents, NULL);
    interceptkeysym = 0;
}

bool interceptkey(int sym)
{
    if(!interceptkeysym)
    {
        interceptkeysym = sym;
        SDL_FilterEvents(interceptevents, NULL);
        if(interceptkeysym < 0)
        {
            interceptkeysym = 0;
            return true;
        }
        SDL_AddEventWatch(interceptevents, NULL);
    }
    else if(abs(interceptkeysym) != sym) interceptkeysym = sym;
    SDL_PumpEvents();
    if(interceptkeysym < 0)
    {
        clearinterceptkey();
        interceptkeysym = sym;
        SDL_FilterEvents(interceptevents, NULL);
        interceptkeysym = 0;
        return true;
    }
    return false;
}

static void ignoremousemotion()
{
    SDL_PumpEvents();
    SDL_FlushEvent(SDL_MOUSEMOTION);
}

static void resetmousemotion()
{
    if(grabinput && !relativemouse && !(SDL_GetWindowFlags(screen) & SDL_WINDOW_FULLSCREEN))
    {
        SDL_WarpMouseInWindow(screen, screenw / 2, screenh / 2);
    }
}

static void checkmousemotion(int &dx, int &dy)
{
    while(pumpevents(events))
    {
        SDL_Event &event = events.removing();
        if(event.type != SDL_MOUSEMOTION) return;
        dx += event.motion.xrel;
        dy += event.motion.yrel;
        events.remove();
    }
}

void checkinput()
{
    if(interceptkeysym) clearinterceptkey();
    //int lasttype = 0, lastbut = 0;
    bool mousemoved = false;
    int focused = 0;
    while(pumpevents(events))
    {
        SDL_Event &event = events.remove();

        if(focused && event.type!=SDL_WINDOWEVENT)
        {
            if(!gui2dvisible() && !overlaycursor() && grabinput != (focused>0)) inputgrab(grabinput = focused>0, shouldgrab);
            focused = 0;
        }

        switch(event.type)
        {
            case SDL_QUIT:
                quit();
                return;

            case SDL_TEXTINPUT:
                if(textinputmask && int(event.text.timestamp-textinputtime) >= textinputfilter)
                {
                    uchar buf[SDL_TEXTINPUTEVENT_TEXT_SIZE+1];
                    size_t len = decodeutf8(buf, sizeof(buf)-1, (const uchar *)event.text.text, strlen(event.text.text));
                    if(len > 0) { buf[len] = '\0'; processtextinput((const char *)buf, len); }
                }
                break;

            case SDL_KEYDOWN:
            case SDL_KEYUP:
                if(keyrepeatmask || !event.key.repeat)
                    processkey(event.key.keysym.sym, event.key.state==SDL_PRESSED, event.key.keysym.mod | SDL_GetModState());
                break;

            case SDL_WINDOWEVENT:
                switch(event.window.event)
                {
                    case SDL_WINDOWEVENT_CLOSE:
                        quit();
                        break;

                    case SDL_WINDOWEVENT_SHOWN:
                    case SDL_WINDOWEVENT_EXPOSED:
                        minimized = false;
                        break;

                    case SDL_WINDOWEVENT_FOCUS_GAINED:
                        shouldgrab = true;
                        minimized = false;
                        if(screen && glcontext) SDL_GL_MakeCurrent(screen, glcontext);
                        break;
                    case SDL_WINDOWEVENT_ENTER:
                        shouldgrab = false;
                        focused = 1;
                        break;

                    case SDL_WINDOWEVENT_LEAVE:
                    case SDL_WINDOWEVENT_FOCUS_LOST:
                        shouldgrab = false;
                        focused = -1;
                        break;

                    case SDL_WINDOWEVENT_MINIMIZED:
                        minimized = true;
                        break;

                    case SDL_WINDOWEVENT_MAXIMIZED:
                    case SDL_WINDOWEVENT_RESTORED:
                        minimized = false;
                        break;

                    case SDL_WINDOWEVENT_MOVED:
                        if(windowmode == WM_BORDERLESS && screen && !applyingwindowmode)
                        {
                            // Alt+Tab often fires MOVED with a 1–8px position jitter.
                            // Snapping on that rebuilt the GL window and the Vulkan
                            // shared image mid-frame, which left a black screen.
                            // Only snap when the window actually changed monitor or size.
                            int prevdisplay = lastuseddisplay;
                            SDL_Rect bounds;
                            getdisplaybounds(bounds);
                            int w, h;
                            SDL_GetWindowSize(screen, &w, &h);
                            if(lastuseddisplay != prevdisplay || w != bounds.w || h != bounds.h)
                                applywindowmode();
                        }
                        break;

                    case SDL_WINDOWEVENT_RESIZED:
                        break;

                    case SDL_WINDOWEVENT_SIZE_CHANGED:
                    {
                        if(shuttingdown) break;
                        SDL_GetWindowSize(screen, &screenw, &screenh);
                        if(screenw <= 0 || screenh <= 0) break;
                        if(windowmode == WM_WINDOWED)
                        {
                            scr_w = clamp(screenw, SCR_MINW, SCR_MAXW);
                            scr_h = clamp(screenh, SCR_MINH, SCR_MAXH);
                        }
                        gl_resize();
                        break;
                    }
                }
                break;

            case SDL_MOUSEMOTION:
                if(grabinput)
                {
                    int dx = event.motion.xrel, dy = event.motion.yrel;
                    checkmousemotion(dx, dy);
                    if(!g3d_movecursor(dx, dy)) mousemove(dx, dy);
                    mousemoved = true;
                }
                else if((gui2dvisible() || overlaycursor()) && screenw > 0 && screenh > 0)
                    g3d_setcursor(event.motion.x / float(screenw), event.motion.y / float(screenh));
                else if(shouldgrab) inputgrab(grabinput = true);
                break;

            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
                if(!grabinput && (gui2dvisible() || overlaycursor()) && screenw > 0 && screenh > 0)
                    g3d_setcursor(event.button.x / float(screenw), event.button.y / float(screenh));
                switch(event.button.button)
                {
                    case SDL_BUTTON_LEFT: processkey(-1, event.button.state==SDL_PRESSED); break;
                    case SDL_BUTTON_MIDDLE: processkey(-2, event.button.state==SDL_PRESSED); break;
                    case SDL_BUTTON_RIGHT: processkey(-3, event.button.state==SDL_PRESSED); break;
                    case SDL_BUTTON_X1: processkey(-6, event.button.state==SDL_PRESSED); break;
                    case SDL_BUTTON_X2: processkey(-7, event.button.state==SDL_PRESSED); break;
                }
                break;

            case SDL_MOUSEWHEEL:
                if(event.wheel.y > 0) { processkey(-4, true); processkey(-4, false); }
                else if(event.wheel.y < 0) { processkey(-5, true); processkey(-5, false); }
                break;
        }
    }
    if(focused) { if(grabinput != (focused>0)) inputgrab(grabinput = focused>0, shouldgrab); focused = 0; }
    syncinputgrab();
    if(mousemoved) resetmousemotion();
}

void swapbuffers(bool overlay)
{
    recorder::capture(overlay);
    extern void hdrout_perf_swapstart();
    extern void hdrout_perf_swapend(bool surface, double sdlswap);
    extern double hdrout_perf_clock();
    hdrout_perf_swapstart();
    if(hdrout_present()) { hdrout_perf_swapend(true, 0); return; }
    gle::disable();
    double t0 = hdrout_perf_clock();
    SDL_GL_SwapWindow(screen);
    hdrout_perf_swapend(false, hdrout_perf_clock() - t0);
}
 
VAR(menufps, 0, 60, 1000);
VARP(maxfps, 0, 200, 1000);

void limitfps(int &millis, int curmillis)
{
    int limit = (mainmenu || minimized) && menufps ? (maxfps ? min(maxfps, menufps) : menufps) : maxfps;
    if(!limit) return;
    static int fpserror = 0;
    int delay = 1000/limit - (millis-curmillis);
    if(delay < 0) fpserror = 0;
    else
    {
        fpserror += 1000%limit;
        if(fpserror >= limit)
        {
            ++delay;
            fpserror -= limit;
        }
        if(delay > 0)
        {
            SDL_Delay(delay);
            millis += delay;
        }
    }
}

#if defined(WIN32) && !defined(_DEBUG) && !defined(__GNUC__)
void stackdumper(unsigned int type, EXCEPTION_POINTERS *ep)
{
    if(!ep) fatal("unknown type");
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT *context = ep->ContextRecord;
    char out[512];
    formatstring(out, "Cube 2: Sauerbraten Win32 Exception: 0x%x [0x%x]\n\n", er->ExceptionCode, er->ExceptionCode==EXCEPTION_ACCESS_VIOLATION ? er->ExceptionInformation[1] : -1);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
#ifdef _AMD64_
	STACKFRAME64 sf = {{context->Rip, 0, AddrModeFlat}, {}, {context->Rbp, 0, AddrModeFlat}, {context->Rsp, 0, AddrModeFlat}, 0};
    while(::StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), GetCurrentThread(), &sf, context, NULL, ::SymFunctionTableAccess, ::SymGetModuleBase, NULL))
	{
		union { IMAGEHLP_SYMBOL64 sym; char symext[sizeof(IMAGEHLP_SYMBOL64) + sizeof(string)]; };
		sym.SizeOfStruct = sizeof(sym);
		sym.MaxNameLength = sizeof(symext) - sizeof(sym);
		IMAGEHLP_LINE64 line;
		line.SizeOfStruct = sizeof(line);
        DWORD64 symoff;
		DWORD lineoff;
        if(SymGetSymFromAddr64(GetCurrentProcess(), sf.AddrPC.Offset, &symoff, &sym) && SymGetLineFromAddr64(GetCurrentProcess(), sf.AddrPC.Offset, &lineoff, &line))
#else
    STACKFRAME sf = {{context->Eip, 0, AddrModeFlat}, {}, {context->Ebp, 0, AddrModeFlat}, {context->Esp, 0, AddrModeFlat}, 0};
    while(::StackWalk(IMAGE_FILE_MACHINE_I386, GetCurrentProcess(), GetCurrentThread(), &sf, context, NULL, ::SymFunctionTableAccess, ::SymGetModuleBase, NULL))
	{
		union { IMAGEHLP_SYMBOL sym; char symext[sizeof(IMAGEHLP_SYMBOL) + sizeof(string)]; };
		sym.SizeOfStruct = sizeof(sym);
		sym.MaxNameLength = sizeof(symext) - sizeof(sym);
		IMAGEHLP_LINE line;
		line.SizeOfStruct = sizeof(line);
        DWORD symoff, lineoff;
        if(SymGetSymFromAddr(GetCurrentProcess(), sf.AddrPC.Offset, &symoff, &sym) && SymGetLineFromAddr(GetCurrentProcess(), sf.AddrPC.Offset, &lineoff, &line))
#endif
        {
            char *del = strrchr(line.FileName, '\\');
            concformatstring(out, "%s - %s [%d]\n", sym.Name, del ? del + 1 : line.FileName, line.LineNumber);
        }
    }
    fatal(out);
}
#endif

#define MAXFPSHISTORY 60

int fpspos = 0, fpshistory[MAXFPSHISTORY];

void resetfpshistory()
{
    loopi(MAXFPSHISTORY) fpshistory[i] = 1;
    fpspos = 0;
}

void updatefpshistory(int millis)
{
    fpshistory[fpspos++] = max(1, min(1000, millis));
    if(fpspos>=MAXFPSHISTORY) fpspos = 0;
}

void getfps(int &fps, int &bestdiff, int &worstdiff)
{
    int total = fpshistory[MAXFPSHISTORY-1], best = total, worst = total;
    loopi(MAXFPSHISTORY-1)
    {
        int millis = fpshistory[i];
        total += millis;
        if(millis < best) best = millis;
        if(millis > worst) worst = millis;
    }

    fps = (1000*MAXFPSHISTORY)/total;
    bestdiff = 1000/best-fps;
    worstdiff = fps-1000/worst;
}

void getfps_(int *raw)
{
    int fps, bestdiff, worstdiff;
    if(*raw) fps = 1000/fpshistory[(fpspos+MAXFPSHISTORY-1)%MAXFPSHISTORY];
    else getfps(fps, bestdiff, worstdiff);
    intret(fps);
}

COMMANDN(getfps, getfps_, "i");

bool inbetweenframes = false, renderedframe = true;

static bool findarg(int argc, char **argv, const char *str)
{
    for(int i = 1; i<argc; i++) if(strstr(argv[i], str)==argv[i]) return true;
    return false;
}

static int clockrealbase = 0, clockvirtbase = 0;
static void clockreset() { clockrealbase = SDL_GetTicks(); clockvirtbase = totalmillis; }
VARFP(clockerror, 990000, 1000000, 1010000, clockreset());
VARFP(clockfix, 0, 0, 1, clockreset());

int getclockmillis()
{
    int millis = SDL_GetTicks() - clockrealbase;
    if(clockfix) millis = int(millis*(double(clockerror)/1000000));
    millis += clockvirtbase;
    return max(millis, totalmillis);
}

VAR(numcpus, 1, 1, 16);

int main(int argc, char **argv)
{
    #ifdef WIN32
    //atexit((void (__cdecl *)(void))_CrtDumpMemoryLeaks);
    #ifndef _DEBUG
    #ifndef __GNUC__
    __try {
    #endif
    #endif
    #endif

    setlogfile(NULL);

    int dedicated = 0;
    char *load = NULL, *initscript = NULL;

    initing = INIT_RESET;
    // set home dir first
    for(int i = 1; i<argc; i++) if(argv[i][0]=='-' && argv[i][1] == 'q') { sethomedir(&argv[i][2]); break; }
    // set log after home dir, but before anything else
    for(int i = 1; i<argc; i++) if(argv[i][0]=='-' && argv[i][1] == 'g')
    {
        const char *file = argv[i][2] ? &argv[i][2] : "log.txt";
        setlogfile(file);
        logoutf("Setting log file: %s", file);
        break;
    }
    execfile("init.cfg", false);
    for(int i = 1; i<argc; i++)
    {
        if(argv[i][0]=='-') switch(argv[i][1])
        {
            case 'q': if(homedir[0]) logoutf("Using home directory: %s", homedir); break;
            case 'r': /* compat, ignore */ break;
            case 'k':
            {
                const char *dir = addpackagedir(&argv[i][2]);
                if(dir) logoutf("Adding package directory: %s", dir);
                break;
            }
            case 'g': break;
            case 'd': dedicated = atoi(&argv[i][2]); if(dedicated<=0) dedicated = 2; break;
            case 'w': scr_w = clamp(atoi(&argv[i][2]), SCR_MINW, SCR_MAXW); if(!findarg(argc, argv, "-h")) scr_h = -1; break;
            case 'h': scr_h = clamp(atoi(&argv[i][2]), SCR_MINH, SCR_MAXH); if(!findarg(argc, argv, "-w")) scr_w = -1; break;
            case 'z': depthbits = atoi(&argv[i][2]); break;
            case 'b': /* compat, ignore */ break;
            case 'a': fsaa = atoi(&argv[i][2]); break;
            case 'v': /* compat, ignore */ break;
            case 't':
            {
                int t = atoi(&argv[i][2]);
                if(t <= 0) windowmode = WM_WINDOWED;
                else if(t == 2) windowmode = WM_BORDERLESS;
                else windowmode = WM_EXCLUSIVE;
                fullscreen = windowmode == WM_EXCLUSIVE ? 1 : 0;
                break;
            }
            case 's': /* compat, ignore */ break;
            case 'f': /* compat, ignore */ break; 
            case 'l': 
            {
                char pkgdir[] = "packages/"; 
                load = strstr(path(&argv[i][2]), path(pkgdir)); 
                if(load) load += sizeof(pkgdir)-1; 
                else load = &argv[i][2]; 
                break;
            }
            case 'x': initscript = &argv[i][2]; break;
            default: if(!serveroption(argv[i])) gameargs.add(argv[i]); break;
        }
        else gameargs.add(argv[i]);
    }
    initing = NOT_INITING;

    numcpus = clamp(SDL_GetCPUCount(), 1, 16);

    if(dedicated <= 1)
    {
        logoutf("init: sdl");

        if(SDL_Init(SDL_INIT_TIMER|SDL_INIT_VIDEO|SDL_INIT_AUDIO)<0) fatal("Unable to initialize SDL: %s", SDL_GetError());
        {
            SDL_version v;
            SDL_GetVersion(&v);
            logoutf("init: SDL %u.%u.%u", v.major, v.minor, v.patch);
        }

#ifdef SDL_VIDEO_DRIVER_X11
        SDL_version version;
        SDL_GetVersion(&version);
        if (SDL_VERSIONNUM(version.major, version.minor, version.patch) <= SDL_VERSIONNUM(2, 0, 12))
            sdl_xgrab_bug = 1;
#endif
    }
    
    logoutf("init: net");
    if(enet_initialize()<0) fatal("Unable to initialise network module");
    atexit(enet_deinitialize);
    enet_time_set(0);

    logoutf("init: game");
    game::parseoptions(gameargs);
    initserver(dedicated>0, dedicated>1);  // never returns if dedicated
    ASSERT(dedicated <= 1);
    game::initclient();

    logoutf("init: video");
    SDL_SetHint(SDL_HINT_GRAB_KEYBOARD, "0");
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
    setupscreen();
    SDL_ShowCursor(SDL_FALSE);
    SDL_StopTextInput(); // workaround for spurious text-input events getting sent on first text input toggle?

    logoutf("init: L4ZY (Sauerbraten 2020 + RT/HDR/translation, client %s)", SAUER_TRAD_VERSION);
    logoutf("init: gl");
    gl_checkextensions();
    gl_init();
    notexture = textureload("packages/textures/notexture.png");
    if(!notexture) fatal("could not find core textures");

    logoutf("init: console");
    if(!execfile("data/stdlib.cfg", false)) fatal("cannot find data files (you are running from the wrong folder, try .bat file in the main folder)");   // this is the first file we load.
    if(!execfile("data/font.cfg", false)) fatal("cannot find font definitions");
    if(!setfont("default")) fatal("no default font specified");

    inbetweenframes = true;
    renderbackground("Initializing...");

    logoutf("init: world");
    camera1 = player = game::iterdynents(0);
    emptymap(0, true, NULL, false);

    logoutf("init: sound");
    initsound();

    logoutf("init: cfg");
    initing = INIT_LOAD;
    execfile("data/keymap.cfg");
    execfile("data/stdedit.cfg");
    execfile("data/sounds.cfg");
    execfile("data/menus.cfg");
    execfile("data/heightmap.cfg");
    execfile("data/blendbrush.cfg");
    execfile("data/hwrt.cfg");
    defformatstring(gamecfgname, "data/game_%s.cfg", game::gameident());
    execfile(gamecfgname);
    if(game::savedservers()) execfile(game::savedservers(), false);
    
    identflags |= IDF_PERSIST;
    
    if(!execfile(game::savedconfig(), false)) 
    {
        execfile(game::defaultconfig());
        writecfg(game::restoreconfig());
    }
    execfile(game::autoexec(), false);

    identflags &= ~IDF_PERSIST;
    extern void hdrout_prefsloaded();
    hdrout_prefsloaded();

    initing = INIT_GAME;
    game::loadconfigs();

    initing = NOT_INITING;

    logoutf("init: render");
    restoregamma();
    restorevsync();
    loadshaders();
    initparticles();
    initdecals();

    identflags |= IDF_PERSIST;

    logoutf("init: mainloop");

    if(execfile("once.cfg", false)) remove(findfile("once.cfg", "rb"));

    if(load)
    {
        logoutf("init: localconnect");
        //localconnect();
        game::changemap(load);
    }

    if(initscript) execute(initscript);

    initmumble();
    resetfpshistory();

    menuprocess();
    grabinput = shouldgrabmouse();
    inputgrab(grabinput);
    ignoremousemotion();

    for(;;)
    {
        static int frames = 0;
        int millis = getclockmillis();
        limitfps(millis, totalmillis);
        elapsedtime = millis - totalmillis;
        static int timeerr = 0;
        int scaledtime = game::scaletime(elapsedtime) + timeerr;
        curtime = scaledtime/100;
        timeerr = scaledtime%100;
        if(!multiplayer(false) && curtime>200) curtime = 200;
        if(game::ispaused()) curtime = 0;
        // Lab-only: force a constant simulated millisecond step so particle
        // age is comparable across Off/DLAA/Quality even when capture stalls.
        // Not VARP; never seed this from a play profile.
        else if(hwrtdlaafixedcurtime > 0 && !multiplayer(false)) curtime = hwrtdlaafixedcurtime;
		lastmillis += curtime;
        totalmillis = millis;
        updatetime();
 
        checkinput();
        menuprocess();
        syncinputgrab();
        tryedit();

        game::polltranslation();
        game::pollfriends();
        if(lastmillis) game::updateworld();

        checksleep(lastmillis);

        serverslice(false, 0);

        if(frames) updatefpshistory(elapsedtime);
        frames++;

        // miscellaneous general game effects
        recomputecamera();
        updateparticles();
        updatesounds();

        if(minimized && !hwrtvelneeddraw()) continue;

        inbetweenframes = false;
        if(mainmenu) gl_drawmainmenu();
        else gl_drawframe();
        swapbuffers();
        renderedframe = inbetweenframes = true;
    }
    
    ASSERT(0);   
    return EXIT_FAILURE;

    #if defined(WIN32) && !defined(_DEBUG) && !defined(__GNUC__)
    } __except(stackdumper(0, GetExceptionInformation()), EXCEPTION_CONTINUE_SEARCH) { return 0; }
    #endif
}
