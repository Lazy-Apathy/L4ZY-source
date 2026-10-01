// creates multiple gui windows that float inside the 3d world

// special feature is that its mostly *modeless*: you can use this menu while playing, without turning menus on or off
// implementationwise, it is *stateless*: it keeps no internal gui structure, hit tests are instant, usage & implementation is greatly simplified

#include "engine.h"

#include "textedit.h"

static bool layoutpass, actionon = false;
static int mousebuttons = 0;
static struct gui *windowhit = NULL;

static float firstx, firsty;

enum {FIELDCOMMIT, FIELDABORT, FIELDEDIT, FIELDSHOW, FIELDKEY};

static int fieldmode = FIELDSHOW; 
static bool fieldsactive = false;
bool guifieldboxed = false; // set by guiinputfield around its field
int guipanelspare = 0; // free height left in the last laid out menu panel (negative = it scrolls)

static bool hascursor;
static float cursorx = 0.5f, cursory = 0.5f;

#define SHADOW 4
#define ICON_SIZE (FONTH-SHADOW)
#define SKIN_W 256
#define SKIN_H 128
#define SKIN_SCALE 4
#define INSERT (3*SKIN_SCALE)
#define TAB2D_H (FONTH + INSERT)
#define MAXCOLUMNS 16
#define GUI_PAD 18
#define SCROLL_GAP 18
#define SCROLL_THICK 4
#define SCROLL_OUT 10
#define GUI_PANEL_W 0.56f
#define GUI_PANEL_H 0.80f
#define GUI_PANEL_TOP 0.09f

static Texture *guibackdroptex = NULL, *guibackdropnext = NULL;

VARP(guiautotab, 6, 16, 40);
VARP(guiclicktab, 0, 0, 1);
VARP(guifadein, 0, 1, 1);
VARP(guipreviewtime, 0, 15, 1000);
VARP(scoreboardalpha, 0, 40, 100);

static int lastpreview = 0;

static inline bool throttlepreview(bool loaded)
{
    if(loaded) return true;
    if(totalmillis - lastpreview < guipreviewtime) return false;
    lastpreview = totalmillis;
    return true;
}

struct gui : g3d_gui
{
    struct list
    {
        int parent, w, h, springs, curspring, column;
    };

    int firstlist, nextlist;
    int columns[MAXCOLUMNS];
    int visih, scrolloff, bodyh, bodyw;
    bool interactive, fixedpanel;

    bool hasscrollbar() const { return gui2d && visih > 0 && visih < bodyh; }
    int bodyrpad() const { return (fixedpanel || hasscrollbar()) ? SCROLL_GAP + SCROLL_THICK + SCROLL_OUT : GUI_PAD; }
    int bodylpad() const { return bodyrpad(); }
    float scrollx() const { return float((bodyw > 0 ? bodyw : xsize)/2 + SCROLL_GAP); }
    int framew() const { return bodyw > 0 ? bodyw : xsize; }

    static vector<list> lists;
    static float hitx, hity;
    static int curdepth, curlist, xsize, ysize, curx, cury;
    static bool shouldmergehits, shouldautotab;
    static bool bannerlayout, lastbanner, bannercenter;
    static int bannerw;
    static float floatu, floatv, floatscale;
    static int overlayw, overlayh, floatsavedx, floatsavedy, floatsavedxs, floatsavedys, floatox, floatoy;
    static bool infloating, floatsavedcenter;
    static int *bindscroll, *bindscrollmax;
    static bool pvorbit, pvdrag;
    static float pvlastx;

    int orbitplayer_(int x, int y, int size, int model, int team, int weap)
    {
        bool hit = windowhit==this && hitx>=x && hity>=y && hitx<x+size && hity<y+size;
        if(hit && (mousebuttons&G3D_DOWN))
        {
            pvorbit = true;
            pvdrag = false;
            pvlastx = hitx;
            game::holdplayerpreview(true);
        }
        if(pvorbit && (mousebuttons&G3D_PRESSED))
        {
            float dx = hitx - pvlastx;
            if(fabs(dx) > 2) pvdrag = true;
            if(pvdrag) game::rotateplayerpreview(-dx * 360.0f / max(float(size), 1.0f));
            pvlastx = hitx;
        }
        if(!(mousebuttons&(G3D_PRESSED|G3D_DOWN)))
        {
            if(pvorbit) game::holdplayerpreview(false);
            pvorbit = false;
        }
        int x1 = int(floor(screenw*(x*scale.x+origin.x))), y1 = int(floor(screenh*(1 - ((y+size)*scale.y+origin.y)))),
            x2 = int(ceil(screenw*((x+size)*scale.x+origin.x))), y2 = int(ceil(screenh*(1 - (y*scale.y+origin.y))));
        if(x2 > x1 && y2 > y1)
        {
            glDisable(GL_BLEND);
            modelpreview::start(x1, y1, x2-x1, y2-y1, false);
            game::renderplayerpreview(model, team, weap);
            modelpreview::end();
            restoreafterpreview();
        }
        int ret = hit ? mousebuttons|G3D_ROLLOVER : 0;
        if(pvdrag && (ret&G3D_UP)) ret &= ~G3D_UP;
        if(!pvorbit) pvdrag = false;
        return ret;
    }

    static void reset()
    {
        lists.setsize(0);
    }

    static int ty, tx, tpos, *tcurrent, tcolor; //tracking tab size and position since uses different layout method...

    void hoverfill_(int x, int y, int w, bool pressed)
    {
        hudnotextureshader->set();
        gle::colorub(255, 122, 24, pressed ? 96 : 50);
        rect_(x, y, w, FONTH);
        hudshader->set();
    }

    void hoverfilltext_(int textx, int textw, bool pressed)
    {
        hoverfill_(textx - INSERT, cury, max(textw, 8) + INSERT*2, pressed);
    }

    // disabled: takes a clickable button's room (rows do not jump when an option
    // becomes available) but draws grey, no hover, and never reports a click
    int button_(const char *text, int color, const char *icon, bool clickable, bool center, bool underline = false, bool disabled = false)
    {
        if(disabled) color = 0x707070;
        const int padding = 18;
        int w = 0;
        if(icon) w += ICON_SIZE;
        if(icon && text) w += padding;
        if(text) w += text_width(text);

        if(visible())
        {
            int hitw = (clickable && isvertical()) ? max(w, xsize) : w;
            bool hit = !disabled && ishit(hitw, FONTH);
            if(hit && clickable) color = 0xFF7A18;
            int x = curx;
            bool centertext = center || (bannercenter && isvertical() && !icon);
            if(isvertical() && centertext) x += (xsize-w)/2;

            if(hit && clickable)
            {
                if(bannerlayout && isvertical())
                    hoverfilltext_(x, w, actionon);
                else
                    hoverfill_(curx, cury, isvertical() ? max(xsize, w) : w, actionon);
            }
            if(underline && isvertical() && text)
            {
                hudnotextureshader->set();
                gle::colorub(255, 122, 24, 200);
                rect_(x, cury + FONTH - 6, max(w, 8), 2);
                hudshader->set();
            }

            if(icon)
            {
                if(icon[0] != ' ')
                {
                    const char *ext = strrchr(icon, '.');
                    defformatstring(tname, "packages/icons/%s%s", icon, ext ? "" : ".jpg");
                    icon_(textureload(tname, 3), false, x, cury, ICON_SIZE, clickable && hit, NULL, disabled);
                }
                x += ICON_SIZE;
            }
            if(icon && text) x += padding;
            if(text) text_(text, x, cury, color, center || (hit && clickable && actionon), hit && clickable);
        }
        int extra = 0;
        if(isvertical())
        {
            if(clickable) extra = (bannercenter || bannerlayout) ? FONTH/4 : FONTH/6;
            else if(underline) extra = FONTH/8;
        }
        int flags = layout(w, FONTH + extra);
        return disabled ? flags&G3D_ROLLOVER : flags;
    }

    bool allowautotab(bool on)
    {
        bool oldval = shouldautotab;
        shouldautotab = on;
        return oldval;
    }

    void autotab() 
    { 
        if(tcurrent)
        {
            if(layoutpass && !tpos) tcurrent = NULL; //disable tabs because you didn't start with one
            if(shouldautotab && !curdepth && (layoutpass ? 0 : cury) + ysize > guiautotab*FONTH) tab(NULL, tcolor); 
        }
    }

    bool shouldtab()
    {
        if(tcurrent && shouldautotab)
        {
            if(layoutpass)
            {
                int space = guiautotab*FONTH - ysize;
                if(space < 0) return true;
                int l = lists[curlist].parent;
                while(l >= 0)
                {
                    space -= lists[l].h;
                    if(space < 0) return true;
                    l = lists[l].parent;
                }
            }
            else
            {
                int space = guiautotab*FONTH - cury;
                if(ysize > space) return true;
                int l = lists[curlist].parent;
                while(l >= 0)
                {
                    if(lists[l].h > space) return true;
                    l = lists[l].parent;
                }
            }
        }
        return false;
    }

    bool visible()
    {
        if(layoutpass) return false;
        if(tcurrent && tpos!=*tcurrent) return false;
        // 3D previews disable GL scissor; never draw rows that start under the frame.
        if(gui2d && visih > 0 && visih < bodyh && cury >= -bodyh + scrolloff + visih) return false;
        return true;
    }

    //tab is always at top of page
    void tab(const char *name, int color) 
    {
        if(curdepth != 0) return;
        if(color) tcolor = color;
        tpos++; 
        if(!name) name = intstr(tpos); 
        int w = max(text_width(name) - 2*INSERT, 0);
        if(layoutpass) 
        {  
            ty = max(ty, ysize); 
            ysize = 0;
        }
        else 
        {	
            cury = -ysize;
            if(gui2d)
            {
                int pad = INSERT;
                int tw = text_width(name);
                int x1 = curx + tx;
                int x2 = x1 + tw + pad*2;
                int y1 = cury - TAB2D_H, y2 = cury;
                bool hit = tcurrent && windowhit==this && hitx>=x1 && hity>=y1 && hitx<x2 && hity<y2;
                if(hit && (!guiclicktab || mousebuttons&G3D_DOWN))
                    *tcurrent = tpos;
                hudnotextureshader->set();
                if(visible()) gle::colorub(255, 122, 24, 48);
                else gle::colorub(28, 32, 38, 200);
                rect_(x1, y1, x2-x1, y2-y1);
                if(visible())
                {
                    gle::colorub(255, 122, 24, 220);
                    rect_(x1, y2-3, x2-x1, 3);
                }
                hudshader->set();
                text_(name, x1 + pad, y1 + (TAB2D_H - FONTH)/2, visible() || hit ? 0xFF7A18 : 0x8A9199, visible());
            }
            else
            {
                int h = FONTH-2*INSERT,
                    x1 = curx + tx,
                    x2 = x1 + w + ((skinx[3]-skinx[2]) + (skinx[5]-skinx[4]))*SKIN_SCALE,
                    y1 = cury - ((skiny[6]-skiny[1])-(skiny[3]-skiny[2]))*SKIN_SCALE-h,
                    y2 = cury;
                bool hit = tcurrent && windowhit==this && hitx>=x1 && hity>=y1 && hitx<x2 && hity<y2;
                if(hit && (!guiclicktab || mousebuttons&G3D_DOWN))
                    *tcurrent = tpos;
                int txoff = (skinx[3]-skinx[2])*SKIN_SCALE - (w ? INSERT : INSERT/2);
                int tyoff = (skiny[2]-skiny[1])*SKIN_SCALE - INSERT;
                drawskin(x1-skinx[visible()?2:6]*SKIN_SCALE, y1-skiny[1]*SKIN_SCALE, w, h, visible()?10:19, 9, 2, light, alpha);
                text_(name, x1 + txoff, y1 + tyoff, tcolor, visible());
            }
        }
        if(gui2d) tx += text_width(name) + INSERT*3;
        else tx += w + ((skinx[5]-skinx[4]) + (skinx[3]-skinx[2]))*SKIN_SCALE; 
    }

    bool ishorizontal() const { return curdepth&1; }
    bool isvertical() const { return !ishorizontal(); }

    void pushlist()
    {	
        if(layoutpass)
        {
            if(curlist>=0)
            {
                lists[curlist].w = xsize;
                lists[curlist].h = ysize;
            }
            list &l = lists.add();
            l.parent = curlist;
            l.springs = 0;
            l.column = -1;
            curlist = lists.length()-1;
            xsize = ysize = 0;
        }
        else
        {
            curlist = nextlist++;
            if(curlist >= lists.length()) // should never get here unless script code doesn't use same amount of lists in layout and render passes
            {
                list &l = lists.add();
                l.parent = curlist;
                l.springs = 0;
                l.column = -1;
                l.w = l.h = 0;
            }
            list &l = lists[curlist];
            l.curspring = 0;
            if(l.springs > 0)
            {
                if(ishorizontal()) xsize = l.w; else ysize = l.h;
            }
            else
            {
                xsize = l.w;
                ysize = l.h;
            }
        }
        curdepth++;	
    }

    void poplist()
    {
        if(!lists.inrange(curlist)) return;
        list &l = lists[curlist];
        if(layoutpass)
        {
            l.w = xsize;
            l.h = ysize;
            if(l.column >= 0) columns[l.column] = max(columns[l.column], ishorizontal() ? ysize : xsize);
        }
        curlist = l.parent;
        curdepth--;
        if(lists.inrange(curlist))
        {   
            int w = xsize, h = ysize;
            if(ishorizontal()) cury -= h; else curx -= w;
            list &p = lists[curlist];
            xsize = p.w;
            ysize = p.h;
            if(!layoutpass && p.springs > 0)
            {
                list &s = lists[p.parent];
                if(ishorizontal()) xsize = s.w; else ysize = s.h;
            } 
            layout(w, h);
        }
    }

    int text  (const char *text, int color, const char *icon) { autotab(); return button_(text, color, icon, false, false); }
    int button(const char *text, int color, const char *icon) { autotab(); return button_(text, color, icon, true, false); }
    int title (const char *text, int color, const char *icon) { autotab(); return button_(text, color, icon, false, false, true); }
    int disabledbutton(const char *text, const char *icon) { autotab(); return button_(text, 0x707070, icon, true, false, false, true); }

    void separator() { autotab(); line_(FONTH/3); }
    void progress(float percent) { autotab(); line_((FONTH*4)/5, percent); }

    //use to set min size (useful when you have progress bars)
    void strut(float size) { layout(isvertical() ? int(size*FONTW) : 0, isvertical() ? 0 : int(size*FONTH)); }
    //add space between list items
    void space(float size) { layout(isvertical() ? 0 : int(size*FONTW), isvertical() ? int(size*FONTH) : 0); }

    void spring(int weight) 
    { 
        if(curlist < 0) return;
        list &l = lists[curlist];
        if(layoutpass) { if(l.parent >= 0) l.springs += weight; return; }
        int nextspring = min(l.curspring + weight, l.springs);
        if(nextspring <= l.curspring) return;
        if(ishorizontal())
        {
            int w = xsize - l.w;
            layout((w*nextspring)/l.springs - (w*l.curspring)/l.springs, 0);
        }
        else
        {
            int h = ysize - l.h;
            layout(0, (h*nextspring)/l.springs - (h*l.curspring)/l.springs);
        }
        l.curspring = nextspring;
    }

    void column(int col)
    {
        if(curlist < 0 || !layoutpass || col < 0 || col >= MAXCOLUMNS) return;
        list &l = lists[curlist];
        l.column = col;
    }

    void setbanner(bool on) { bannerlayout = bannercenter = on; }
    void setbannercenter(bool on) { bannercenter = on; }
    void bannersplit() { if(layoutpass) bannerw = max(bannerw, isvertical() ? xsize : ysize); }

    void startfloat(float u, float v)
    {
        floatu = u;
        floatv = v;
        floatscale = 0.62f;
        infloating = true;
        floatsavedcenter = bannercenter;
        bannercenter = false;
        floatsavedx = curx;
        floatsavedy = cury;
        floatsavedxs = xsize;
        floatsavedys = ysize;
        pushlist();
        pushlist();
        if(!layoutpass)
        {
            floatox = int((u - origin.x) / scale.x);
            floatoy = int((v - origin.y) / scale.y);
            curx = 0;
            cury = 0;
            pushhudtranslate(float(floatox), float(floatoy), floatscale, floatscale);
        }
    }

    void endfloat()
    {
        if(!lists.inrange(curlist)) return;
        poplist();
        if(!lists.inrange(curlist)) return;
        list &l = lists[curlist];
        if(layoutpass)
        {
            l.w = xsize;
            l.h = ysize;
            overlayw = xsize;
            overlayh = ysize;
        }
        else
        {
            overlayw = l.w;
            overlayh = l.h;
            pophudmatrix();
            hudshader->set();
        }
        curlist = l.parent;
        curdepth--;
        curx = floatsavedx;
        cury = floatsavedy;
        xsize = floatsavedxs;
        ysize = floatsavedys;
        bannercenter = floatsavedcenter;
        infloating = false;
    }

    int colorswatch(int coloridx, bool selected)
    {
        autotab();
        static const uchar cols[10][3] =
        {
            {  64, 255, 128 },
            {  96, 160, 255 },
            { 255, 192,  64 },
            { 255,  64,  64 },
            { 128, 128, 128 },
            { 192,  64, 192 },
            { 255, 128,   0 },
            { 255, 255, 255 },
            {  96, 240, 255 },
            { 255, 128, 200 }
        };
        int size = FONTH;
        if(visible())
        {
            float hx, hy;
            maphit(hx, hy);
            bool hit = windowhit==this && hx>=curx && hy>=cury && hx<curx+size && hy<cury+size;
            hudnotextureshader->set();
            if(coloridx < 0)
            {
                if(selected)
                {
                    gle::colorub(255, 122, 24, 255);
                    rect_(curx, cury, size, size);
                    gle::colorub(0, 0, 0, 255);
                    rect_(curx + 3, cury + 3, size - 6, size - 6);
                    gle::colorub(40, 40, 40, 255);
                    rect_(curx + 6, cury + 6, size - 12, size - 12);
                }
                else
                {
                    gle::colorub(0, 0, 0, 255);
                    rect_(curx, cury, size, size);
                    int inset = hit ? 4 : 3;
                    gle::colorub(40, 40, 40, 255);
                    rect_(curx + inset, cury + inset, size - inset*2, size - inset*2);
                }
                float x0 = curx + size*0.28f, y0 = cury + size*0.28f;
                float x1 = curx + size*0.72f, y1 = cury + size*0.72f;
                gle::colorub(200, 200, 200, 255);
                gle::defvertex(2);
                gle::begin(GL_LINES);
                gle::attribf(x0, y0); gle::attribf(x1, y1);
                gle::attribf(x1, y0); gle::attribf(x0, y1);
                xtraverts += gle::end();
            }
            else
            {
                coloridx = clamp(coloridx, 0, 9);
                if(selected)
                {
                    gle::colorub(255, 122, 24, 255);
                    rect_(curx, cury, size, size);
                    gle::colorub(0, 0, 0, 255);
                    rect_(curx + 3, cury + 3, size - 6, size - 6);
                    gle::colorub(cols[coloridx][0], cols[coloridx][1], cols[coloridx][2], 255);
                    rect_(curx + 6, cury + 6, size - 12, size - 12);
                }
                else
                {
                    gle::colorub(0, 0, 0, 255);
                    rect_(curx, cury, size, size);
                    int inset = hit ? 4 : 3;
                    gle::colorub(cols[coloridx][0], cols[coloridx][1], cols[coloridx][2], 255);
                    rect_(curx + inset, cury + inset, size - inset*2, size - inset*2);
                }
            }
            hudshader->set();
        }
        return layout(size + FONTH/8, size);
    }

    void maphit(float &hx, float &hy)
    {
        hx = hitx;
        hy = hity;
        if(infloating && floatscale > 0)
        {
            hx = (hitx - floatox) / floatscale;
            hy = (hity - floatoy) / floatscale;
        }
    }

    int layout(int w, int h)
    {
        if(layoutpass)
        {
            if(ishorizontal())
            {
                xsize += w;
                ysize = max(ysize, h);
            }
            else
            {
                xsize = max(xsize, w);
                ysize += h;
            }
            return 0;
        }
        else
        {
            bool hit = ishit(w, h);
            bool vis = visible();
            if(ishorizontal()) curx += w;
            else cury += h;
            return (hit && vis) ? mousebuttons|G3D_ROLLOVER : 0;
        }
    }

    bool mergehits(bool on) 
    { 
        bool oldval = shouldmergehits;
        shouldmergehits = on; 
        return oldval;
    }

    bool ishit(int w, int h, int x = curx, int y = cury)
    {
        float hx, hy;
        maphit(hx, hy);
        if(shouldmergehits) return windowhit==this && (ishorizontal() ? hx>=x && hx<x+w : hy>=y && hy<y+h);
        if(ishorizontal()) h = ysize;
        else w = xsize;
        return windowhit==this && hx>=x && hy>=y && hx<x+w && hy<y+h;
    }

    int image(Texture *t, float scale, const char *overlaid)
    {
        autotab();
        if(scale==0) scale = 1;
        int size = (int)(scale*2*FONTH)-SHADOW;
        if(visible()) icon_(t, overlaid!=NULL, curx, cury, size, ishit(size+SHADOW, size+SHADOW), overlaid);
        return layout(size+SHADOW, size+SHADOW);
    }
    
    int texture(VSlot &vslot, float scale, bool overlaid)
    {
        autotab();
        if(scale==0) scale = 1;
        int size = (int)(scale*2*FONTH)-SHADOW;
        if(visible()) previewslot(vslot, overlaid, curx, cury, size, ishit(size+SHADOW, size+SHADOW));
        return layout(size+SHADOW, size+SHADOW);
    }

    int playerpreview(int model, int team, int weap, float sizescale, const char *overlaid)
    {
        autotab();
        if(sizescale==0) sizescale = 1;
        int size = (int)(sizescale*2*FONTH)-SHADOW;
        int ret = 0;
        if(model>=0 && visible())
        {
            float xs = size, ys = size, xi = curx, yi = cury;
            if(overlaid && actionon && windowhit==this)
            {
                hudnotextureshader->set();
                gle::colorf(0, 0, 0, 0.75f);
                rect_(xi+SHADOW, yi+SHADOW, xs, ys);
                hudshader->set();
            }
            ret = orbitplayer_(curx, cury, size, model, team, weap);
            if(overlaid)
            {
                bool hit = ret&G3D_ROLLOVER;
                if(hit)
                {
                    hudnotextureshader->set();
                    glBlendFunc(GL_ZERO, GL_SRC_COLOR);
                    gle::colorf(1, 0.55f, 0.18f);
                    rect_(xi, yi, xs, ys);
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                    hudshader->set();
                }
                if(overlaid[0]) text_(overlaid, xi + xs/12, yi + ys - ys/12 - FONTH, hit ? 0xFF7A18 : 0xFFFFFF, hit, hit);
                if(!overlaytex) overlaytex = textureload("data/guioverlay.png", 3);
                gle::color(light);
                glBindTexture(GL_TEXTURE_2D, overlaytex->id);
                rect_(xi, yi, xs, ys, 0);
            }
        }
        int laid = layout(size+SHADOW, size+SHADOW);
        if(ret) return ret;
        if(pvdrag && (laid&G3D_UP)) laid &= ~G3D_UP;
        return laid;
    }

    int playeroverlay(int model, int team, int weap, float sizescale)
    {
        if(layoutpass || model<0 || !visible()) return 0;
        (void)sizescale;
        (void)sizescale;
        const float u0 = 0.50f, u1 = 0.97f, v0 = 0.04f, v1 = 0.98f;
        bool hit = cursorx >= u0 && cursorx < u1 && cursory >= v0 && cursory < v1;
        if(hit && (mousebuttons&G3D_DOWN))
        {
            pvorbit = true;
            pvdrag = false;
            pvlastx = cursorx;
            game::holdplayerpreview(true);
        }
        if(pvorbit && (mousebuttons&G3D_PRESSED))
        {
            float dx = cursorx - pvlastx;
            if(fabs(dx) > 0.004f) pvdrag = true;
            if(pvdrag) game::rotateplayerpreview(-dx * 360.0f / max(u1 - u0, 0.1f));
            pvlastx = cursorx;
        }
        if(!(mousebuttons&(G3D_PRESSED|G3D_DOWN)))
        {
            if(pvorbit) game::holdplayerpreview(false);
            pvorbit = false;
        }
        int vx = int(floor(screenw * u0));
        int vw = int(ceil(screenw * u1)) - vx;
        int vy = int(floor(screenh * (1 - v1)));
        int vh = int(ceil(screenh * (1 - v0))) - vy;
        if(vw > 1 && vh > 1)
        {
            glDisable(GL_BLEND);
            modelpreview::start(vx, vy, vw, vh, false);
            game::renderplayerpreview(model, team, weap);
            modelpreview::end();
            restoreafterpreview();
        }
        int ret = hit ? mousebuttons|G3D_ROLLOVER : 0;
        if(pvdrag && (ret&G3D_UP)) ret &= ~G3D_UP;
        if(!pvorbit) pvdrag = false;
        return ret;
    }

    int modelpreview(const char *name, int anim, float sizescale, const char *overlaid, bool throttle)
    {
        autotab();
        if(sizescale==0) sizescale = 1;
        int size = (int)(sizescale*2*FONTH)-SHADOW;
        if(name[0] && visible() && (!throttle || throttlepreview(modelloaded(name))))
        {
            bool hit = ishit(size+SHADOW, size+SHADOW);
            float xs = size, ys = size, xi = curx, yi = cury;
            if(overlaid && hit && actionon)
            {
                hudnotextureshader->set();
                gle::colorf(0, 0, 0, 0.75f);
                rect_(xi+SHADOW, yi+SHADOW, xs, ys);
                hudshader->set();
            }
            int x1 = int(floor(screenw*(xi*scale.x+origin.x))), y1 = int(floor(screenh*(1 - ((yi+ys)*scale.y+origin.y)))),
                x2 = int(ceil(screenw*((xi+xs)*scale.x+origin.x))), y2 = int(ceil(screenh*(1 - (yi*scale.y+origin.y))));
            glDisable(GL_BLEND);
            modelpreview::start(x1, y1, x2-x1, y2-y1, overlaid!=NULL);
            model *m = loadmodel(name);
            if(m)
            {
                entitylight light;
                light.color = vec(1, 1, 1);
                light.dir = vec(0, -1, 2).normalize();
                vec center, radius;
                m->boundbox(center, radius);
                float yaw;
                vec o = calcmodelpreviewpos(radius, yaw).sub(center);
                rendermodel(&light, name, anim, o, yaw, 0, 0, NULL, NULL, 0);
            }
            modelpreview::end();
            restoreafterpreview();
            if(overlaid)
            {
                if(hit)
                {
                    hudnotextureshader->set();
                    glBlendFunc(GL_ZERO, GL_SRC_COLOR);
                    gle::colorf(1, 0.55f, 0.18f);
                    rect_(xi, yi, xs, ys);
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                    hudshader->set();
                }
                if(overlaid[0]) text_(overlaid, xi + xs/12, yi + ys - ys/12 - FONTH, hit ? 0xFF7A18 : 0xFFFFFF, hit, hit);
                if(!overlaytex) overlaytex = textureload("data/guioverlay.png", 3);
                gle::color(light);
                glBindTexture(GL_TEXTURE_2D, overlaytex->id);
                rect_(xi, yi, xs, ys, 0);
            }
        }
        return layout(size+SHADOW, size+SHADOW);
    }

    int prefabpreview(const char *prefab, const vec &color, float sizescale, const char *overlaid, bool throttle)
    {
        autotab();
        if(sizescale==0) sizescale = 1;
        int size = (int)(sizescale*2*FONTH)-SHADOW;
        if(prefab[0] && visible() && (!throttle || throttlepreview(prefabloaded(prefab))))
        {
            bool hit = ishit(size+SHADOW, size+SHADOW);
            float xs = size, ys = size, xi = curx, yi = cury;
            if(overlaid && hit && actionon)
            {
                hudnotextureshader->set();
                gle::colorf(0, 0, 0, 0.75f);
                rect_(xi+SHADOW, yi+SHADOW, xs, ys);
                hudshader->set();
            }
            int x1 = int(floor(screenw*(xi*scale.x+origin.x))), y1 = int(floor(screenh*(1 - ((yi+ys)*scale.y+origin.y)))),
                x2 = int(ceil(screenw*((xi+xs)*scale.x+origin.x))), y2 = int(ceil(screenh*(1 - (yi*scale.y+origin.y))));
            glDisable(GL_BLEND);
            modelpreview::start(x1, y1, x2-x1, y2-y1, overlaid!=NULL);
            previewprefab(prefab, color);
            modelpreview::end();
            restoreafterpreview();
            if(overlaid)
            {
                if(hit)
                {
                    hudnotextureshader->set();
                    glBlendFunc(GL_ZERO, GL_SRC_COLOR);
                    gle::colorf(1, 0.55f, 0.18f);
                    rect_(xi, yi, xs, ys);
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                    hudshader->set();
                }
                if(overlaid[0]) text_(overlaid, xi + FONTH/2, yi + FONTH/2, hit ? 0xFF7A18 : 0xFFFFFF, hit, hit);
                if(!overlaytex) overlaytex = textureload("data/guioverlay.png", 3);
                gle::color(light);
                glBindTexture(GL_TEXTURE_2D, overlaytex->id);
                rect_(xi, yi, xs, ys, 0);
            }
        }
        return layout(size+SHADOW, size+SHADOW);
    }
 
    void slider(int &val, int vmin, int vmax, int color, const char *label)
    {
        autotab();
        int x = curx;
        int y = cury;
        int offset = vmin < vmax ? clamp(val, vmin, vmax) : clamp(val, vmax, vmin);
        int span = vmax==vmin ? 1 : (vmax-vmin);
        if(gui2d && isvertical())
        {
            int barh = max(FONTH/7, 6);
            if(visible())
            {
                if(!label) label = intstr(val);
                int lw = text_width(label);
                int bw = max(xsize, lw + FONTH);
                bool hit = ishit(bw, FONTH, x, y);
                float pct = float(offset-vmin)/float(span);
                if(pct < 0) pct = 0;
                if(pct > 1) pct = 1;
                hudnotextureshader->set();
                gle::colorub(40, 44, 52, 230);
                rect_(x, y + (FONTH-barh)/2, bw, barh);
                int fillw = max(int(bw * pct), barh);
                gle::colorub(255, 122, 24, hit ? 255 : 200);
                rect_(x, y + (FONTH-barh)/2, fillw, barh);
                hudshader->set();
                if(hit) color = 0xFF7A18;
                int px = x + int((bw-lw)*pct);
                text_(label, px, y, color, hit && actionon, hit);
                if(hit && actionon)
                {
                    int vnew = int((float(span+((vmin<vmax)?1:-1))*(hitx-x-FONTH/2))/max(bw-lw, 1));
                    vnew += vmin;
                    vnew = vmin < vmax ? clamp(vnew, vmin, vmax) : clamp(vnew, vmax, vmin);
                    if(vnew != val) val = vnew;
                }
            }
            layout(0, FONTH + FONTH/8);
            return;
        }
        line_((FONTH*2)/3);
        if(visible())
        {
            if(!label) label = intstr(val);
            int w = text_width(label);

            bool hit;
            int px, py;
            if(ishorizontal())
            {
                hit = ishit(FONTH, ysize, x, y);
                px = x + (FONTH-w)/2;
                py = y + (ysize-FONTH) - ((ysize-FONTH)*(offset-vmin))/span;
            }
            else
            {
                hit = ishit(xsize, FONTH, x, y);
                px = x + FONTH/2 - w/2 + ((xsize-w)*(offset-vmin))/span;
                py = y;
            }

            if(hit) color = 0xFF7A18;
            text_(label, px, py, color, hit && actionon, hit);
            if(hit && actionon)
            {
                int vnew = (vmin < vmax ? 1 : -1)+vmax-vmin;
                if(ishorizontal()) vnew = int((vnew*(y+ysize-FONTH/2-hity))/(ysize-FONTH));
                else vnew = int((vnew*(hitx-x-FONTH/2))/(xsize-w));
                vnew += vmin;
                vnew = vmin < vmax ? clamp(vnew, vmin, vmax) : clamp(vnew, vmax, vmin);
                if(vnew != val) val = vnew;
            }
        }
    }

    char *field(const char *name, int color, int length, int height, const char *initval, int initmode)
    {
        return field_(name, color, length, height, initval, initmode, FIELDEDIT);
    }

    char *keyfield(const char *name, int color, int length, int height, const char *initval, int initmode)
    {
        return field_(name, color, length, height, initval, initmode, FIELDKEY);
    }

    char *field_(const char *name, int color, int length, int height, const char *initval, int initmode, int fieldtype = FIELDEDIT)
    {	
        editor *e = useeditor(name, initmode, false, initval); // generate a new editor if necessary
        if(layoutpass)
        {
            if(initval && e->mode==EDITORFOCUSED && (e!=currentfocus() || fieldmode == FIELDSHOW))
            {
                if(strcmp(e->lines[0].text, initval)) e->clear(initval);
            }
            e->linewrap = (length<0);
            e->maxx = (e->linewrap) ? -1 : length;
            e->maxy = (height<=0)?1:-1;
            e->pixelwidth = abs(length)*FONTW;
            if(e->linewrap && e->maxy==1) 
            {
                int temp;
                text_bounds(e->lines[0].text, temp, e->pixelheight, e->pixelwidth); //only single line editors can have variable height
                if(guifieldboxed) e->pixelheight = max(e->pixelheight, FONTH*2);
            }
            else 
                e->pixelheight = FONTH*max(height, 1); 
        }
        int h = e->pixelheight;
        int w = e->pixelwidth + FONTW;
        
        bool wasvertical = isvertical();
        if(wasvertical && e->maxy != 1) pushlist();
        
        // 2D menus underline a one-line field; a boxed field (guiinputfield) keeps its frame
        bool underline = gui2d && e->maxy==1 && !guifieldboxed;
        char *result = NULL;
        if(visible() && !layoutpass)
        {
            e->rendered = true;

            bool hit = ishit(w, h);
            float hx, hy;
            maphit(hx, hy);
            if(hit) 
            {
                if(mousebuttons&G3D_DOWN) //mouse request focus
                {   
                    if(fieldtype==FIELDKEY) e->clear();
                    useeditor(name, initmode, true); 
                    e->mark(false);
                    fieldmode = fieldtype;
                } 
            }
            bool editing = (fieldmode != FIELDSHOW) && (e==currentfocus());
            if(hit && editing && (mousebuttons&G3D_PRESSED)!=0 && fieldtype==FIELDEDIT) e->hit(int(floor(hx-((underline) ? curx : curx+FONTW/2))), int(floor(hy-cury)), (mousebuttons&G3D_DRAGGED)!=0); //mouse request position
            if(editing && ((fieldmode==FIELDCOMMIT) || (fieldmode==FIELDABORT) || !hit)) // commit field if user pressed enter or wandered out of focus 
            {
                if(fieldmode==FIELDCOMMIT || (fieldmode!=FIELDABORT && !hit)) result = e->currentline().text;
                e->active = (e->mode!=EDITORFOCUSED);
                fieldmode = FIELDSHOW;
            } 
            else fieldsactive = true;
            
            int drawx = (underline) ? curx : curx+FONTW/2;
            int drawcolor = color;
            if(underline && hit)
            {
                int tw = text_width(e->currentline().text);
                hoverfilltext_(drawx, tw, editing);
                drawcolor = 0xFF7A18;
            }
            e->draw(drawx, cury, drawcolor, hit && editing);
            
            if(!underline || editing)
            {
                hudnotextureshader->set();
                glDisable(GL_BLEND);
                if(editing) gle::colorub(255, 122, 24, 220);
                else gle::colorub(color>>16, (color>>8)&0xFF, color&0xFF);
                if(underline)
                {
                    int tw = text_width(e->currentline().text);
                    rect_(drawx - INSERT, cury+h-2, max(tw, 8) + INSERT*2, 2);
                }
                else rect_(curx, cury, w, h, true);
                glEnable(GL_BLEND);
                hudshader->set();
            }
        }
        layout(w, h);
        
        if(e->maxy != 1)
        {
            int slines = e->limitscrolly();
            if(slines > 0) 
            {
                int pos = e->scrolly;
                slider(e->scrolly, slines, 0, color, NULL);
                if(pos != e->scrolly) e->cy = e->scrolly; 
            }
            if(wasvertical) poplist();
        }
        
        return result;
    }

    void rect_(float x, float y, float w, float h, bool lines = false)
    {
        gle::defvertex(2);
        gle::begin(lines ? GL_LINE_LOOP : GL_TRIANGLE_STRIP);
        gle::attribf(x, y);
        gle::attribf(x + w, y);
        if(lines) gle::attribf(x + w, y + h);
        gle::attribf(x, y + h);
        if(!lines) gle::attribf(x + w, y + h);
        xtraverts += gle::end();
    }

    void rect_(float x, float y, float w, float h, int usetc)
    {
        gle::defvertex(2);
        gle::deftexcoord0();
        gle::begin(GL_TRIANGLE_STRIP);
        static const vec2 tc[5] = { vec2(0, 0), vec2(1, 0), vec2(1, 1), vec2(0, 1), vec2(0, 0) };
        gle::attribf(x, y); gle::attrib(tc[usetc]);
        gle::attribf(x + w, y); gle::attrib(tc[usetc+1]);
        gle::attribf(x, y + h); gle::attrib(tc[usetc+3]);
        gle::attribf(x + w, y + h); gle::attrib(tc[usetc+2]);
        xtraverts += gle::end();
    }

    void text_(const char *text, int x, int y, int color, bool shadow, bool force = false) 
    {
        if(shadow) draw_text(text, x+SHADOW, y+SHADOW, 0x00, 0x00, 0x00, -0xC0);
        draw_text(text, x, y, color>>16, (color>>8)&0xFF, color&0xFF, force ? -0xFF : 0xFF);
    }

    void background(int color, int inheritw, int inherith)
    {
        if(layoutpass) return;
        hudnotextureshader->set();
        gle::colorub(color>>16, (color>>8)&0xFF, color&0xFF, 0x80);
        int w = xsize, h = ysize;
        if(inheritw>0) 
        {
            int parentw = curlist, parentdepth = 0;
            for(;parentdepth < inheritw && lists[parentw].parent>=0; parentdepth++)
                parentw = lists[parentw].parent;
            list &p = lists[parentw];
            w = p.springs > 0 && (curdepth-parentdepth)&1 ? lists[p.parent].w : p.w;
        }
        if(inherith>0)
        {
            int parenth = curlist, parentdepth = 0;
            for(;parentdepth < inherith && lists[parenth].parent>=0; parentdepth++)
                parenth = lists[parenth].parent;
            list &p = lists[parenth];
            h = p.springs > 0 && !((curdepth-parentdepth)&1) ? lists[p.parent].h : p.h;
        }
        rect_(curx, cury, w, h);
        hudshader->set();
    }

    void icon_(Texture *t, bool overlaid, int x, int y, int size, bool hit, const char *title = NULL, bool dim = false)
    {
        float scale = float(size)/max(t->xs, t->ys); //scale and preserve aspect ratio
        float xs = t->xs*scale, ys = t->ys*scale;
        x += int((size-xs)/2);
        y += int((size-ys)/2);
        vec color = hit ? vec(1, 0.55f, 0.18f) : (overlaid ? vec(1, 1, 1) : light);
        if(dim) color.mul(0.4f);
        glBindTexture(GL_TEXTURE_2D, t->id);
        if(hit && actionon)
        {
            gle::colorf(0, 0, 0, 0.75f);
            rect_(x+SHADOW, y+SHADOW, xs, ys, 0);
        }
        gle::color(color);
        rect_(x, y, xs, ys, 0);

        if(overlaid)
        {
            if(!overlaytex) overlaytex = textureload("data/guioverlay.png", 3);
            glBindTexture(GL_TEXTURE_2D, overlaytex->id);
            gle::color(light);
            rect_(x, y, xs, ys, 0);
            if(title) text_(title, x + xs/12, y + ys - ys/12 - FONTH, hit ? 0xFF7A18 : 0xFFFFFF, hit && actionon, hit);
        }
    }        

    void previewslot(VSlot &vslot, bool overlaid, int x, int y, int size, bool hit)
    {
        Slot &slot = *vslot.slot;
        if(slot.sts.empty()) return;
        VSlot *layer = NULL;
        Texture *t = NULL, *glowtex = NULL, *layertex = NULL;
        if(slot.loaded)
        {
            t = slot.sts[0].t;
            if(t == notexture) return;
            Slot &slot = *vslot.slot;
            if(slot.texmask&(1<<TEX_GLOW)) { loopvj(slot.sts) if(slot.sts[j].type==TEX_GLOW) { glowtex = slot.sts[j].t; break; } }
            if(vslot.layer)
            {
                layer = &lookupvslot(vslot.layer);
                if(!layer->slot->sts.empty()) layertex = layer->slot->sts[0].t;
            }
        }
        else if(slot.thumbnail && slot.thumbnail != notexture) t = slot.thumbnail;
        else return;
        float xt = min(1.0f, t->xs/(float)t->ys), yt = min(1.0f, t->ys/(float)t->xs), xs = size, ys = size;
        if(hit && actionon) 
        {
            hudnotextureshader->set();
            gle::colorf(0, 0, 0, 0.75f);
            rect_(x+SHADOW, y+SHADOW, xs, ys);
            hudshader->set();	
        }
        SETSHADER(hudrgb);
        gle::defvertex(2);
        gle::deftexcoord0();
        const vec &color = hit ? vec(1, 0.55f, 0.18f) : (overlaid ? vec(1, 1, 1) : light);
        vec2 tc[4] = { vec2(0, 0), vec2(1, 0), vec2(1, 1), vec2(0, 1) };
        float xoff = vslot.offset.x, yoff = vslot.offset.y;
        if(vslot.rotation)
        {
            const texrotation &r = texrotations[vslot.rotation];
            if(r.swapxy) { swap(xoff, yoff); loopk(4) swap(tc[k].x, tc[k].y); }
            if(r.flipx) { xoff *= -1; loopk(4) tc[k].x *= -1; }
            if(r.flipy) { yoff *= -1; loopk(4) tc[k].y *= -1; }
        }
        loopk(4) { tc[k].x = tc[k].x/xt - xoff/t->xs; tc[k].y = tc[k].y/yt - yoff/t->ys; } 
        if(slot.loaded) gle::color(vec(color).mul(vslot.colorscale));
        else gle::color(color);
        glBindTexture(GL_TEXTURE_2D, t->id);
        gle::begin(GL_TRIANGLE_STRIP);
        gle::attribf(x,    y);    gle::attrib(tc[0]);
        gle::attribf(x+xs, y);    gle::attrib(tc[1]);
        gle::attribf(x,    y+ys); gle::attrib(tc[3]);
        gle::attribf(x+xs, y+ys); gle::attrib(tc[2]);
        gle::end();
        if(glowtex)
        {
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            glBindTexture(GL_TEXTURE_2D, glowtex->id);
            if(hit || overlaid) gle::color(vec(vslot.glowcolor).mul(color));
            else gle::color(vslot.glowcolor);
            gle::begin(GL_TRIANGLE_STRIP);
            gle::attribf(x,    y);    gle::attrib(tc[0]);
            gle::attribf(x+xs, y);    gle::attrib(tc[1]);
            gle::attribf(x,    y+ys); gle::attrib(tc[3]);
            gle::attribf(x+xs, y+ys); gle::attrib(tc[2]);
            gle::end();
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
        if(layertex)
        {
            glBindTexture(GL_TEXTURE_2D, layertex->id);
            gle::color(vec(color).mul(layer->colorscale));
            gle::begin(GL_TRIANGLE_STRIP);
            gle::attribf(x+xs/2, y+ys/2); gle::attrib(tc[0]);
            gle::attribf(x+xs,   y+ys/2); gle::attrib(tc[1]);
            gle::attribf(x+xs/2, y+ys);   gle::attrib(tc[3]);
            gle::attribf(x+xs,   y+ys);   gle::attrib(tc[2]);
            gle::end();
        }
            
        hudshader->set();
        if(overlaid) 
        {
            if(!overlaytex) overlaytex = textureload("data/guioverlay.png", 3);
            glBindTexture(GL_TEXTURE_2D, overlaytex->id);
            gle::color(light);
            rect_(x, y, xs, ys, 0);
        }
    }

    void line_(int size, float percent = 1.0f)
    {		
        if(visible())
        {
            if(gui2d && percent > 0.99f)
            {
                hudnotextureshader->set();
                gle::colorub(255, 122, 24, 64);
                if(ishorizontal()) rect_(curx + FONTH/2 - 1, cury, 2, ysize);
                else rect_(curx, cury + FONTH/2 - 1, xsize, 2);
                hudshader->set();
            }
            else
            {
                if(!slidertex) slidertex = textureload("data/guislider.png", 3);
                glBindTexture(GL_TEXTURE_2D, slidertex->id);
                if(percent < 0.99f) 
                {
                    gle::colorf(light.x, light.y, light.z, 0.375f);
                    if(ishorizontal()) 
                        rect_(curx + FONTH/2 - size/2, cury, size, ysize, 0);
                    else
                        rect_(curx, cury + FONTH/2 - size/2, xsize, size, 1);
                }
                gle::color(light);
                if(ishorizontal()) 
                    rect_(curx + FONTH/2 - size/2, cury + ysize*(1-percent), size, ysize*percent, 0);
                else 
                    rect_(curx, cury + FONTH/2 - size/2, xsize*percent, size, 1);
            }
        }
        layout(ishorizontal() ? FONTH : 0, ishorizontal() ? 0 : FONTH);
    }

    void textbox(const char *text, int width, int height, int color) 
    {
        width *= FONTW;
        height *= FONTH;
        int w, h;
        text_bounds(text, w, h, width);
        if(h > height) height = h;
        if(visible()) draw_text(text, curx, cury, color>>16, (color>>8)&0xFF, color&0xFF, 0xFF, -1, width);
        layout(width, height);
    }

    static Texture *skintex, *overlaytex, *slidertex;
    static const int skinx[], skiny[];
    static const struct patch { ushort left, right, top, bottom; uchar flags; } patches[];

    static void drawskin(int x, int y, int gapw, int gaph, int start, int n, int passes = 1, const vec &light = vec(1, 1, 1), float alpha = 0.80f)//int vleft, int vright, int vtop, int vbottom, int start, int n) 
    {
        if(!skintex) skintex = textureload("data/guiskin.png", 3);
        glBindTexture(GL_TEXTURE_2D, skintex->id);
        int gapx1 = INT_MAX, gapy1 = INT_MAX, gapx2 = INT_MAX, gapy2 = INT_MAX;
        float wscale = 1.0f/(SKIN_W*SKIN_SCALE), hscale = 1.0f/(SKIN_H*SKIN_SCALE);
        
        loopj(passes)
        {	
            bool quads = false;
            if(passes>1) glDepthFunc(j ? GL_LEQUAL : GL_GREATER);
            gle::color(j ? light : vec(1, 1, 1), passes<=1 || j ? alpha : alpha/2); //ghost when its behind something in depth
            loopi(n)
            {
                const patch &p = patches[start+i];
                int left = skinx[p.left]*SKIN_SCALE, right = skinx[p.right]*SKIN_SCALE,
                    top = skiny[p.top]*SKIN_SCALE, bottom = skiny[p.bottom]*SKIN_SCALE;
                float tleft = left*wscale, tright = right*wscale,
                      ttop = top*hscale, tbottom = bottom*hscale;
                if(p.flags&0x1)
                {
                    gapx1 = left;
                    gapx2 = right;
                }
                else if(left >= gapx2)
                {
                    left += gapw - (gapx2-gapx1);
                    right += gapw - (gapx2-gapx1);
                }
                if(p.flags&0x10)
                {
                    gapy1 = top;
                    gapy2 = bottom;
                }
                else if(top >= gapy2)
                {
                    top += gaph - (gapy2-gapy1);
                    bottom += gaph - (gapy2-gapy1);
                }
               
                //multiple tiled quads if necessary rather than a single stretched one
                int ystep = bottom-top;
                int yo = y+top;
                while(ystep > 0) 
                {
                    if(p.flags&0x10 && yo+ystep-(y+top) > gaph) 
                    {
                        ystep = gaph+y+top-yo;
                        tbottom = ttop+ystep*hscale;
                    }
                    int xstep = right-left;
                    int xo = x+left;
                    float tright2 = tright;
                    while(xstep > 0) 
                    {
                        if(p.flags&0x01 && xo+xstep-(x+left) > gapw) 
                        {
                            xstep = gapw+x+left-xo; 
                            tright = tleft+xstep*wscale;
                        }
                        if(!quads)
                        {
                            quads = true;
                            gle::defvertex(2);
                            gle::deftexcoord0();
                            gle::begin(GL_QUADS);
                        }
                        gle::attribf(xo,       yo);       gle::attribf(tleft,  ttop);
                        gle::attribf(xo+xstep, yo);       gle::attribf(tright, ttop);
                        gle::attribf(xo+xstep, yo+ystep); gle::attribf(tright, tbottom);
                        gle::attribf(xo,       yo+ystep); gle::attribf(tleft,  tbottom);
                        if(!(p.flags&0x01)) break;
                        xo += xstep;
                    }
                    tright = tright2;
                    if(!(p.flags&0x10)) break;
                    yo += ystep;
                }
            }
            if(quads) xtraverts += gle::end();
            else break; //if it didn't happen on the first pass, it won't happen on the second..
        }
        if(passes>1) glDepthFunc(GL_ALWAYS);
    } 

    vec origin, scale, *savedorigin;
    float dist;
    g3d_callback *cb;
    bool gui2d;

    static float basescale, maxscale;
    static bool passthrough;
    static float alpha;
    static vec light;

    void adjustscale()
    {
        int w = xsize + (skinx[2]-skinx[1])*SKIN_SCALE + (skinx[10]-skinx[9])*SKIN_SCALE, h = ysize + (skiny[9]-skiny[7])*SKIN_SCALE;
        if(tcurrent) h += ((skiny[5]-skiny[1])-(skiny[3]-skiny[2]))*SKIN_SCALE + FONTH-2*INSERT;
        else h += (skiny[6]-skiny[3])*SKIN_SCALE;

        float aspect = forceaspect ? 1.0f/forceaspect : float(screenh)/float(screenw);
        if(bannerlayout && gui2d)
        {
            float sx = aspect * basescale, sy = basescale;
            float ndcw = xsize * sx;
            if(ndcw > 0.90f)
            {
                float f = 0.90f / ndcw;
                sx *= f;
                sy *= f;
            }
            float ndch = ysize * sy;
            if(ndch > 0.86f)
            {
                float f = 0.86f / ndch;
                sx *= f;
                sy *= f;
            }
            scale = vec(sx, sy, 1);
            origin = vec(0.32f + xsize * 0.5f * sx, 0.07f + ysize * sy, 0);
            visih = ysize;
            bodyh = ysize;
            bodyw = xsize;
            scrolloff = 0;
            fixedpanel = false;
            if(interactive && bindscrollmax) *bindscrollmax = 0;
            return;
        }
        // Overlay GUIs (Tab scoreboard, etc.) must not steal the menu's
        // height/scroll: they shrink to fit like vanilla instead of clipping.
        if(!interactive)
        {
            float fit = 1.0f;
            if(w*aspect*basescale>1.0f) fit = 1.0f/(w*aspect*basescale);
            if(h*basescale*fit>1.0f) fit *= 1.0f/(h*basescale*fit);
            scale = vec(aspect*basescale*fit, basescale*fit, 1);
            origin = vec(0.5f-((w-xsize)/2 - (skinx[2]-skinx[1])*SKIN_SCALE)*scale.x, 0.5f + (0.5f*h-(skiny[9]-skiny[7])*SKIN_SCALE)*scale.y, 0);
            visih = ysize;
            bodyh = ysize;
            bodyw = xsize;
            scrolloff = 0;
            fixedpanel = false;
            return;
        }
        // Same window for every interactive 2D menu: pad short pages, scroll long ones.
        scale = vec(aspect*basescale, basescale, 1);
        int wantw = max(1, int(GUI_PANEL_W / scale.x));
        if(xsize > wantw)
        {
            float f = float(wantw) / float(max(xsize, 1));
            scale.x *= f;
            scale.y *= f;
        }
        bodyw = max(xsize, max(1, int(GUI_PANEL_W / scale.x)));
        visih = max(FONTH*8, int(GUI_PANEL_H / scale.y));
        visih -= visih % FONTH;
        bodyh = ysize;
        fixedpanel = true;
        guipanelspare = visih - ysize;
        int smax = max(0, ysize - visih);
        if(bindscrollmax) *bindscrollmax = smax;
        scrolloff = 0;
        if(bindscroll)
        {
            *bindscroll = clamp(*bindscroll, 0, smax);
            scrolloff = *bindscroll;
        }
        float ytop = float(-ysize + scrolloff);
        origin = vec(0.5f, GUI_PANEL_TOP - ytop*scale.y, 0);
    }

    void start(int starttime, float initscale, int *tab, bool allowinput)
    {	
        interactive = allowinput;
        if(gui2d) 
        {
            initscale *= 0.025f; 
            if(allowinput) hascursor = true;
        }
        basescale = initscale;
        if(layoutpass)
        {
            bannerlayout = false;
            bannercenter = false;
            bannerw = 0;
            overlayw = overlayh = 0;
            infloating = false;
            floatscale = 1;
            scale.x = scale.y = scale.z = (guifadein && !lastbanner) ? basescale*min((totalmillis-starttime)/300.0f, 1.0f) : basescale;
        }
        alpha = allowinput ? 0.80f : 0.60f;
        passthrough = scale.x<basescale || !allowinput;
        curdepth = -1;
        curlist = -1;
        tpos = 0;
        tx = 0;
        ty = 0;
        tcurrent = tab;
        tcolor = 0xFFFFFF;
        pushlist();
        if(layoutpass) 
        {
            firstlist = nextlist = curlist;
            memset(columns, 0, sizeof(columns));
        }
        else
        {
            if(tcurrent && !*tcurrent) tcurrent = NULL;
            cury = -ysize; 
            curx = -xsize/2;
            
            if(gui2d)
            {
                hudmatrix.ortho(0, 1, 1, 0, -1, 1);
                hudmatrix.translate(origin);        
                hudmatrix.scale(scale);

                light = vec(1, 1, 1);
            }
            else
            {
                float yaw = atan2f(origin.y-camera1->o.y, origin.x-camera1->o.x);
                hudmatrix = camprojmatrix;
                hudmatrix.translate(origin);
                hudmatrix.rotate_around_z(yaw - 90*RAD);
                hudmatrix.rotate_around_x(-90*RAD);
                hudmatrix.scale(-scale.x, scale.y, scale.z);
            
                vec dir;
                lightreaching(origin, light, dir, false, 0, 0.5f); 
                float intensity = vec(yaw, 0.0f).dot(dir);
                light.mul(1.0f + max(intensity, 0.0f));
            }

            resethudmatrix();
            hudshader->set();

            if(gui2d)
            {
                hudnotextureshader->set();
                if(bannerlayout)
                {
                    const int pad = 40;
                    float gy0 = (0.0f - origin.y) / scale.y;
                    float gy1 = (1.0f - origin.y) / scale.y;
                    float bx = -xsize/2.0f - pad;
                    float bw = float((bannerw > 0 ? bannerw : xsize) + pad*2);
                    gle::colorub(8, 10, 14, 248);
                    rect_(bx, gy0, bw, gy1 - gy0);
                    gle::colorub(255, 122, 24, 50);
                    rect_(bx, gy0, 3, gy1 - gy0);
                    gle::colorub(255, 122, 24, 22);
                    rect_(bx + bw - 2, gy0, 2, gy1 - gy0);
                }
                else
                {
                    int pad = bodylpad(), rpad = bodyrpad();
                    int fw = framew();
                    int ytop = -ysize + scrolloff;
                    int px = -fw/2 - pad, py = ytop - pad, pw = fw + pad + rpad, ph = visih + pad*2;
                    if(tcurrent)
                    {
                        py -= TAB2D_H;
                        ph += TAB2D_H;
                    }
                    if(guibackdroptex)
                    {
                        hudshader->set();
                        glBindTexture(GL_TEXTURE_2D, guibackdroptex->id);
                        gle::colorub(255, 255, 255, 255);
                        rect_(px, py, pw, ph, 0);
                        hudnotextureshader->set();
                        gle::colorub(10, 12, 16, 168);
                        rect_(px, py, pw, ph);
                    }
                    else
                    {
                        int bg = allowinput ? 242 : (scoreboardalpha*255)/100;
                        gle::colorub(10, 12, 16, bg);
                        rect_(px, py, pw, ph);
                    }
                    gle::colorub(255, 122, 24, 210);
                    rect_(px, py, pw, 3);
                    gle::colorub(255, 122, 24, 210);
                    rect_(px, py + ph - 3, pw, 3);
                    gle::colorub(255, 122, 24, 210);
                    rect_(px, py, 3, ph);
                    gle::colorub(255, 122, 24, 210);
                    rect_(px + pw - 3, py, 3, ph);
                }
                hudshader->set();
                clipbody();
            }
            else
            {
                drawskin(curx-skinx[2]*SKIN_SCALE, cury-skiny[6]*SKIN_SCALE, xsize, ysize, 0, 9, 2, light, alpha);
                if(!tcurrent) drawskin(curx-skinx[5]*SKIN_SCALE, cury-skiny[6]*SKIN_SCALE, xsize, 0, 9, 1, 2, light, alpha);
            }
        }
    }

    void restoreafterpreview()
    {
        hudshader->set();
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_BLEND);
        if(!gui2d) return;
        hudmatrix.ortho(0, 1, 1, 0, -1, 1);
        hudmatrix.translate(origin);
        hudmatrix.scale(scale);
        resethudmatrix();
        clipbody();
    }

    void clipbody()
    {
        int h = bodyh > 0 ? bodyh : ysize;
        int w = bodyw > 0 ? bodyw : xsize;
        if(!gui2d || visih >= h || visih <= 0) return;
        int pad = bodylpad(), rpad = bodyrpad();
        float y0 = float(-h + scrolloff), y1 = y0 + visih;
        float x0 = float(-w/2 - pad), x1 = float(w/2 + rpad);
        int sx = int(floor(screenw * (origin.x + x0 * scale.x)));
        int sw = int(ceil(screenw * (x1 - x0) * scale.x));
        float ndc0 = origin.y + y0 * scale.y, ndc1 = origin.y + y1 * scale.y;
        int sy = int(floor(screenh * (1 - ndc1)));
        int sh = int(ceil(screenh * (ndc1 - ndc0)));
        if(sw < 1 || sh < 1) return;
        glScissor(sx, sy, sw, sh);
        glEnable(GL_SCISSOR_TEST);
    }

    void drawscrollthumb()
    {
        int h = bodyh > 0 ? bodyh : ysize;
        if(!gui2d || visih <= 0) return;
        bool bar = hasscrollbar() && h > visih;
        if(!bar && !fixedpanel) return;
        float y0 = float(-h + scrolloff), track = float(visih);
        float sx = scrollx();
        hudnotextureshader->set();
        gle::colorub(255, 122, 24, 36);
        rect_(sx, y0, float(SCROLL_THICK), track);
        if(bar)
        {
            float thumb = max(float(FONTH), track * visih / float(h));
            float ty = y0 + (float(scrolloff) / float(max(h - visih, 1))) * (track - thumb);
            gle::colorub(255, 122, 24, 210);
            rect_(sx, ty, float(SCROLL_THICK), thumb);
        }
        hudshader->set();
    }

    void adjusthorizontalcolumn(int col, int i)
    {
        int h = columns[col], dh = 0;
        for(int d = 1; i >= 0; d ^= 1)
        {
            list &p = lists[i];
            if(d&1) { dh = h - p.h; if(dh <= 0) break; p.h = h; }
            else { p.h += dh; h = p.h; }
            i = p.parent;
        }
        ysize += max(dh, 0);
    }

    void adjustverticalcolumn(int col, int i)
    {
        int w = columns[col], dw = 0;
        for(int d = 0; i >= 0; d ^= 1)
        {
            list &p = lists[i];
            if(d&1) { p.w += dw; w = p.w; }
            else { dw = w - p.w; if(dw <= 0) break; p.w = w; }
            i = p.parent;
        }
        xsize = max(xsize, w);
    }
        
    void adjustcolumns()
    {
        if(lists.inrange(curlist))
        {
            list &l = lists[curlist];
            if(l.column >= 0) columns[l.column] = max(columns[l.column], ishorizontal() ? ysize : xsize);
        }
        int parent = -1, depth = 0;
        for(int i = firstlist; i < lists.length(); i++)
        {
            list &l = lists[i];
            if(l.parent > parent) { parent = l.parent; depth++; }
            else if(l.parent < parent)
            {
                while(parent > l.parent && depth > 0)
                {
                    parent = lists[parent].parent;
                    depth--;
                }
            }
            if(l.column >= 0)
            {
                if(depth&1) adjusthorizontalcolumn(l.column, i); 
                else adjustverticalcolumn(l.column, i);
            }
        }
    }

    void end()
    {
        if(layoutpass)
        {	
            adjustcolumns();
            xsize = max(tx, xsize);
            ysize = max(ty, ysize);
            ysize = max(ysize, (skiny[7]-skiny[6])*SKIN_SCALE);
            if(tcurrent) *tcurrent = max(1, min(*tcurrent, tpos));
            if(gui2d) adjustscale();
            lastbanner = bannerlayout;
            if(!windowhit && !passthrough)
            {
                float dist = 0;
                if(gui2d)
                {
                    hitx = (cursorx - origin.x)/scale.x;
                    hity = (cursory - origin.y)/scale.y;
                }
                else
                {
                    plane p;
                    p.toplane(vec(origin).sub(camera1->o).set(2, 0).normalize(), origin);
                    if(p.rayintersect(camera1->o, camdir, dist) && dist>=0)
                    {
                        vec hitpos(camdir);
                        hitpos.mul(dist).add(camera1->o).sub(origin);
                        hitx = vec(-p.y, p.x, 0).dot(hitpos)/scale.x;
                        hity = -hitpos.z/scale.y;
                    }
                }
                if((mousebuttons & G3D_PRESSED) && (fabs(hitx-firstx) > 2 || fabs(hity - firsty) > 2)) mousebuttons |= G3D_DRAGGED;
                int y0 = -ysize + scrolloff, y1 = y0 + (visih > 0 ? visih : ysize);
                if(gui2d) y1 += FONTH + GUI_PAD;
                int fw = framew();
                int hitl = -fw/2 - bodylpad();
                int hitr = fw/2 + bodyrpad();
                if(dist>=0 && hitx>=hitl && hitx<=hitr)
                {
                    if(hity>=y0 && hity<=y1)
                        windowhit = this;
                    else if(tcurrent && hitx<=tx-xsize/2)
                    {
                        int tabtop = gui2d ? y0 - TAB2D_H : -ysize-(FONTH-2*INSERT)-((skiny[6]-skiny[1])-(skiny[3]-skiny[2]))*SKIN_SCALE;
                        if(hity>=tabtop && hity<y0) windowhit = this;
                    }
                }
                if(overlayw > 0 && scale.x != 0 && scale.y != 0)
                {
                    float fs = floatscale > 0 ? floatscale : 1;
                    int ox = int((floatu - origin.x) / scale.x);
                    int oy = int((floatv - origin.y) / scale.y);
                    int ow = int(overlayw * fs), oh = int(overlayh * fs);
                    if(hitx>=ox && hitx<ox+ow && hity>=oy && hity<oy+oh)
                        windowhit = this;
                }
            }
        }
        else
        {
            glDisable(GL_SCISSOR_TEST);
            drawscrollthumb();
            if(tcurrent && tx<xsize) drawskin(curx+tx-skinx[5]*SKIN_SCALE, -ysize-skiny[6]*SKIN_SCALE, xsize-tx, FONTH, 9, 1, gui2d ? 1 : 2, light, alpha);
        }
        poplist();
    }

    void draw()
    {
        cb->gui(*this, layoutpass);
    }
};

Texture *gui::skintex = NULL, *gui::overlaytex = NULL, *gui::slidertex = NULL;

//chop skin into a grid
const int gui::skiny[] = {0, 7, 21, 34, 43, 48, 56, 104, 111, 117, 128},
          gui::skinx[] = {0, 11, 23, 37, 105, 119, 137, 151, 215, 229, 246, 256};
//Note: skinx[3]-skinx[2] = skinx[7]-skinx[6]
//      skinx[5]-skinx[4] = skinx[9]-skinx[8]		 
const gui::patch gui::patches[] = 
{ //arguably this data can be compressed - it depends on what else needs to be skinned in the future
    {1,2,3,6,  0},    // body
    {2,9,5,6,  0x01},
    {9,10,3,6, 0},

    {1,2,6,7,  0x10},
    {2,9,6,7,  0x11},
    {9,10,6,7, 0x10},

    {1,2,7,9,  0},
    {2,9,7,9,  0x01},
    {9,10,7,9, 0},

    {5,6,3,5, 0x01}, // top

    {2,3,1,2, 0},    // selected tab
    {3,4,1,2, 0x01},
    {4,5,1,2, 0},
    {2,3,2,3, 0x10},
    {3,4,2,3, 0x11},
    {4,5,2,3, 0x10},
    {2,3,3,5, 0},
    {3,4,3,5, 0x01},
    {4,5,3,5, 0},

    {6,7,1,2, 0},    // deselected tab
    {7,8,1,2, 0x01},
    {8,9,1,2, 0},
    {6,7,2,3, 0x10},
    {7,8,2,3, 0x11},
    {8,9,2,3, 0x10},
    {6,7,3,5, 0},
    {7,8,3,5, 0x01},
    {8,9,3,5, 0},
};

vector<gui::list> gui::lists;
float gui::basescale, gui::maxscale = 1, gui::hitx, gui::hity, gui::alpha;
bool gui::passthrough, gui::shouldmergehits = false, gui::shouldautotab = true;
bool gui::bannerlayout = false, gui::lastbanner = false, gui::bannercenter = false;
bool gui::pvorbit = false, gui::pvdrag = false;
float gui::pvlastx = 0;
int gui::bannerw = 0;
float gui::floatu = 0, gui::floatv = 0, gui::floatscale = 1;
int gui::overlayw = 0, gui::overlayh = 0, gui::floatsavedx = 0, gui::floatsavedy = 0, gui::floatsavedxs = 0, gui::floatsavedys = 0, gui::floatox = 0, gui::floatoy = 0;
bool gui::infloating = false, gui::floatsavedcenter = false;
int *gui::bindscroll = NULL, *gui::bindscrollmax = NULL;
vec gui::light;
int gui::curdepth, gui::curlist, gui::xsize, gui::ysize, gui::curx, gui::cury;
int gui::ty, gui::tx, gui::tpos, *gui::tcurrent, gui::tcolor;
static vector<gui> guis2d, guis3d;

VARP(guipushdist, 1, 4, 64);

bool g3d_input(const char *str, int len)
{
    editor *e = currentfocus();
    if(fieldmode == FIELDKEY || fieldmode == FIELDSHOW || !e) return false;
    
    e->input(str, len);
    return true;
}

bool g3d_key(int code, bool isdown)
{
    editor *e = currentfocus();
    if(fieldmode == FIELDKEY)
    {
        switch(code)
        {
            case SDLK_ESCAPE:
                if(isdown) fieldmode = FIELDCOMMIT;
                return true;
        }
        const char *keyname = getkeyname(code);
        if(keyname && isdown)
        {
            if(e->lines.length()!=1 || !e->lines[0].empty()) e->insert(" ");
            e->insert(keyname);
        }
        return true;
    }

    if(code==-4 || code==-5)
    {
        if(isdown && gui::bindscroll && gui::bindscrollmax && *gui::bindscrollmax > 0)
        {
            int step = FONTH + FONTH/6;
            if(code==-4) *gui::bindscroll = max(0, *gui::bindscroll - step);
            else *gui::bindscroll = min(*gui::bindscrollmax, *gui::bindscroll + step);
            return true;
        }
        if(windowhit && windowhit->gui2d) return true;
    }

    if(code==-1 && g3d_windowhit(isdown, true)) return true;
    else if(code==-3 && g3d_windowhit(isdown, false)) return true;

    if(fieldmode == FIELDSHOW || !e)
    {
        if(windowhit) switch(code)
        {
            case -4: // window "management" 
                if(isdown)
                {
                    if(windowhit->gui2d) 
                    {
                        vec origin = *guis2d.last().savedorigin;
                        int i = windowhit - &guis2d[0];
                        for(int j = guis2d.length()-1; j > i; j--) *guis2d[j].savedorigin = *guis2d[j-1].savedorigin;
                        *windowhit->savedorigin = origin;
                        if(guis2d.length() > 1)
                        {
                            if(camera1->o.dist(*windowhit->savedorigin) <= camera1->o.dist(*guis2d.last().savedorigin))
                                windowhit->savedorigin->add(camdir);
                        }
                    }
                    else windowhit->savedorigin->add(vec(camdir).mul(guipushdist));
                }
                return true;
            case -5:
                if(isdown)
                {
                    if(windowhit->gui2d)
                    {
                        vec origin = *guis2d[0].savedorigin;
                        loopj(guis2d.length()-1) *guis2d[j].savedorigin = *guis2d[j + 1].savedorigin;
                        *guis2d.last().savedorigin = origin;
                        if(guis2d.length() > 1)
                        {
                            if(camera1->o.dist(*guis2d.last().savedorigin) >= camera1->o.dist(*guis2d[0].savedorigin))
                                guis2d.last().savedorigin->sub(camdir);
                        }
                    }
                    else windowhit->savedorigin->sub(vec(camdir).mul(guipushdist));
                }
                return true;
        }

        return false;
    }
    switch(code)
    {
        case SDLK_ESCAPE: //cancel editing without commit
            if(isdown) fieldmode = FIELDABORT;
            return true;
        case SDLK_RETURN:
        case SDLK_TAB:
            if(e->maxy != 1) break;
        case SDLK_KP_ENTER:
            if(isdown) fieldmode = FIELDCOMMIT; //signal field commit (handled when drawing field)
            return true;
    }
    if(isdown) e->key(code);
    return true;
}

void g3d_cursorpos(float &x, float &y)
{
    if(guis2d.length() || overlaycursor()) { x = cursorx; y = cursory; }
    else x = y = 0.5f;
}

void g3d_setcursor(float x, float y)
{
    cursorx = max(0.0f, min(1.0f, x));
    cursory = max(0.0f, min(1.0f, y));
}

void g3d_resetcursor()
{
    cursorx = cursory = 0.5f;
}

FVARP(guisens, 1e-3f, 1, 1e3f);

bool g3d_movecursor(int dx, int dy)
{
    if(!guis2d.length() || !hascursor) return false;
    const float CURSORSCALE = 500.0f;
    cursorx = max(0.0f, min(1.0f, cursorx+guisens*dx*(screenh/(screenw*CURSORSCALE))));
    cursory = max(0.0f, min(1.0f, cursory+guisens*dy/CURSORSCALE));
    return true;
}

VARNP(guifollow, useguifollow, 0, 1, 1);
VARNP(gui2d, usegui2d, 0, 1, 1);

void g3d_addgui(g3d_callback *cb, vec &origin, int flags)
{
    bool gui2d = flags&GUI_FORCE_2D || (flags&GUI_2D && usegui2d) || mainmenu;
    if(!gui2d && flags&GUI_FOLLOW && useguifollow) origin.z = player->o.z-(player->eyeheight-1);
    gui &g = (gui2d ? guis2d : guis3d).add();
    g.cb = cb;
    g.origin = origin;
    g.savedorigin = &origin;
    g.dist = flags&GUI_BOTTOM && gui2d ? 1e16f : camera1->o.dist(g.origin);
    g.gui2d = gui2d;
    g.visih = 0;
    g.bodyh = 0;
    g.bodyw = 0;
    g.scrolloff = 0;
    g.interactive = true;
    g.fixedpanel = false;
}

void g3d_limitscale(float scale)
{
    gui::maxscale = scale;
}

void g3d_setbackdrop(const char *path)
{
    if(!path || !path[0]) { guibackdropnext = NULL; return; }
    Texture *t = textureload(path, 3, true, false);
    guibackdropnext = (!t || t==notexture) ? NULL : t;
}

void g3d_bindscroll(int *scroll, int *scrollmax)
{
    gui::bindscroll = scroll;
    gui::bindscrollmax = scrollmax;
}

static inline bool g3d_sort(const gui &a, const gui &b) { return a.dist < b.dist; }

bool g3d_windowhit(bool on, bool act)
{
    extern int cleargui(int n);
    if(act) 
    {
        if(actionon || windowhit)
        {
            if(on) { firstx = gui::hitx; firsty = gui::hity; }
            mousebuttons |= (actionon=on) ? G3D_DOWN : G3D_UP;
        }
    } else if(!on && windowhit) cleargui(1);
    return (guis2d.length() && hascursor) || (windowhit && !windowhit->gui2d);
}

void g3d_render()   
{
    windowhit = NULL;    
    if(actionon) mousebuttons |= G3D_PRESSED;
   
    gui::reset(); 
    guis2d.shrink(0);
    guis3d.shrink(0);
 
    // call all places in the engine that may want to render a gui from here, they call g3d_addgui()
    extern void g3d_texturemenu();
    
    if(!mainmenu) g3d_texturemenu();
    g3d_mainmenu();
    if(!mainmenu) game::g3d_gamemenus();

    guis2d.sort(g3d_sort);
    guis3d.sort(g3d_sort);
    
    readyeditors();
    fieldsactive = false;

    hascursor = false;

    layoutpass = true;
    loopv(guis2d) guis2d[i].draw();
    loopv(guis3d) guis3d[i].draw();
    layoutpass = false;

    if(guis3d.length())
    {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_ALWAYS);
        glDepthMask(GL_FALSE);

        loopvrev(guis3d) guis3d[i].draw();

        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        glDisable(GL_DEPTH_TEST);

        glDisable(GL_BLEND);
    }
}

void g3d_render2d()
{
    guibackdroptex = guibackdropnext;
    guibackdropnext = NULL;
    if(guis2d.length())
    {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        loopvrev(guis2d) guis2d[i].draw();

        glDisable(GL_BLEND);
    }

    flusheditors();
    if(!fieldsactive) fieldmode = FIELDSHOW; //didn't draw any fields, so lose focus - mainly for menu closed
    textinput(fieldmode!=FIELDSHOW, TI_GUI);
    keyrepeat(fieldmode!=FIELDSHOW, KR_GUI);

    mousebuttons = 0;
}

void consolebox(int x1, int y1, int x2, int y2)
{
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    float bw = x2 - x1, bh = y2 - y1, aspect = bw/bh, sh = bh, sw = sh*aspect;
    bw *= float(4*FONTH)/(SKIN_H*SKIN_SCALE);
    bh *= float(4*FONTH)/(SKIN_H*SKIN_SCALE);
    sw /= bw + (gui::skinx[2]-gui::skinx[1] + gui::skinx[10]-gui::skinx[9])*SKIN_SCALE;
    sh /= bh + (gui::skiny[9]-gui::skiny[7] + gui::skiny[6]-gui::skiny[4])*SKIN_SCALE;
    pushhudmatrix();
    hudmatrix.translate(x1, y1, 0);
    hudmatrix.scale(sw, sh, 1);
    flushhudmatrix();
    gui::drawskin(-gui::skinx[1]*SKIN_SCALE, -gui::skiny[4]*SKIN_SCALE, int(bw), int(bh), 0, 9, 1, vec(1, 1, 1), 0.60f);
    gui::drawskin((-gui::skinx[1] + gui::skinx[2] - gui::skinx[5])*SKIN_SCALE, -gui::skiny[4]*SKIN_SCALE, int(bw), 0, 9, 1, 1, vec(1, 1, 1), 0.60f);
    pophudmatrix();
}


// live text of a gui field (while typing, before Enter commits it)
ICOMMAND(guifieldtext, "s", (char *name),
{
    loopv(editors) if(!strcmp(editors[i]->name, name)) { result(editors[i]->lines[0].text); return; }
    result(getalias(name));
});
