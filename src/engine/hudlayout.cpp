// HUD layout: every part of the HUD can be dragged to another place and
// resized.
//
// A HUD part draws itself between hudbegin("id", "Label") and hudend()
// (or with a hudscope object), and says where it drew with hudrect() in its
// own coordinates. That is all a new part needs to become movable: the
// offset and size the player chose are applied around its drawing, whatever
// scale it uses, and the editor (/hudedit, Settings > HUD) shows a frame for
// it. Drag inside a frame to move it, drag an edge or a corner to resize it.
//
// Offsets are fractions of the screen (so they follow resolution changes),
// the width and height are factors (each edge changes its own direction,
// Shift on a corner keeps the proportions); saved in the "hudlayout"
// setting: "id dx dy width height keeptext ..." (older saves: "id dx dy
// [size]"). keeptext 1 = the letters keep their normal size when the part is
// resized (T or middle click on a frame in the editor).

#include "engine.h"

struct hudpart
{
    string id, label;
    float dx, dy;           // chosen offset, fraction of the screen width / height
    float sx, sy;           // chosen width / height factors (1 = as drawn)
    bool keeptext;          // the letters keep their size when the part is resized
    float x0, y0, x1, y1;   // where it drew last, in pixels, without the offset and size
    int drawn;              // totalmillis of the last frame it drew
    float fx0, fy0, fx1, fy1; // rect gathered during the current frame (on screen)
    float sxused, syused;   // factors applied this frame (1 until its place is known)
    float oxused, oyused;   // offset applied this frame, in pixels (kept on the screen)
    bool gathering;
};
static vector<hudpart *> hudparts;
static bool hudlayoutloaded = false;

SVARFP(hudlayout, "", { hudlayoutloaded = false; });

// where each part was last seen ("id label x0 y0 x1 y1", fractions of the
// screen, "_" for spaces in the label): the editor shows a frame for parts
// that are not on screen now (main menu, another game mode)
SVARP(hudboxes, "");
static bool hudboxesloaded = false, hudboxesdirty = false;
static int hudboxessaved = 0;

static const float HUDMINSIZE = 0.3f, HUDMAXSIZE = 4.0f;

static hudpart *findhudpart(const char *id)
{
    loopv(hudparts) if(!strcmp(hudparts[i]->id, id)) return hudparts[i];
    return NULL;
}

static hudpart *newhudpart(const char *id, const char *label);

static const char *hudword(const char *p, char *out, int len)
{
    while(*p == ' ') p++;
    int n = 0;
    while(*p && *p != ' ' && n < len-1) out[n++] = *p++;
    out[n] = 0;
    return p;
}

static void loadhudboxes()
{
    if(hudboxesloaded || screenw <= 0 || screenh <= 0) return;
    hudboxesloaded = true;
    const char *p = hudboxes;
    for(;;)
    {
        char id[MAXSTRLEN], label[MAXSTRLEN];
        p = hudword(p, id, sizeof(id));
        p = hudword(p, label, sizeof(label));
        if(!id[0] || !label[0]) break;
        float v[4];
        loopi(4) { while(*p == ' ') p++; v[i] = strtod(p, (char **)&p); }
        for(char *c = label; *c; c++) if(*c == '_') *c = ' ';
        hudpart *h = findhudpart(id);
        if(!h) h = newhudpart(id, label);
        if(h->drawn < 0) { h->x0 = v[0]*screenw; h->y0 = v[1]*screenh; h->x1 = v[2]*screenw; h->y1 = v[3]*screenh; }
    }
}

static void savehudboxes()
{
    if(screenw <= 0 || screenh <= 0) return;
    vector<char> buf;
    loopv(hudparts)
    {
        hudpart *h = hudparts[i];
        if(h->x1 <= h->x0) continue;
        string label;
        copystring(label, h->label);
        for(char *c = label; *c; c++) if(*c == ' ') *c = '_';
        defformatstring(e, "%s %s %.4f %.4f %.4f %.4f ", h->id, label, h->x0/screenw, h->y0/screenh, h->x1/screenw, h->y1/screenh);
        buf.put(e, strlen(e));
    }
    while(buf.length() && buf.last() == ' ') buf.pop();
    buf.add('\0');
    if(strcmp(hudboxes, buf.getbuf())) setsvar("hudboxes", buf.getbuf());
}

// one entry of hudlayout: "id dx dy width height", or older "id dx dy [size]"
static const char *hudlayoutentry(const char *p, char *id, int idlen, float v[5])
{
    p = hudword(p, id, idlen);
    v[0] = v[1] = 0; v[2] = v[3] = 1; v[4] = 0;
    int n = 0;
    loopi(5)
    {
        while(*p == ' ') p++;
        char *end = NULL;
        float f = strtod(p, &end);
        if(end == p) break; // the next id
        v[i] = f;
        p = end;
        n++;
    }
    if(n == 3) v[3] = v[2]; // one size for both
    return p;
}

static void savedlayout(const char *id, float &dx, float &dy, float &sx, float &sy, bool &keeptext)
{
    dx = dy = 0; sx = sy = 1; keeptext = false;
    const char *p = hudlayout;
    char tok[MAXSTRLEN];
    float v[5];
    for(;;)
    {
        p = hudlayoutentry(p, tok, sizeof(tok), v);
        if(!tok[0]) break;
        if(!strcmp(tok, id)) { dx = v[0]; dy = v[1]; sx = clamp(v[2], HUDMINSIZE, HUDMAXSIZE); sy = clamp(v[3], HUDMINSIZE, HUDMAXSIZE); keeptext = v[4] != 0; return; }
    }
}

static void loadhudlayout()
{
    if(hudlayoutloaded) return;
    hudlayoutloaded = true;
    loopv(hudparts) savedlayout(hudparts[i]->id, hudparts[i]->dx, hudparts[i]->dy, hudparts[i]->sx, hudparts[i]->sy, hudparts[i]->keeptext);
}

static bool hudmoved(const hudpart *p) { return p->dx || p->dy || p->sx != 1 || p->sy != 1 || p->keeptext; }

static void savehudlayout()
{
    vector<char> buf;
    // keep the entries of parts not seen this session (another mode, another HUD option)
    const char *p = hudlayout;
    char tok[MAXSTRLEN];
    float v[5];
    for(;;)
    {
        p = hudlayoutentry(p, tok, sizeof(tok), v);
        if(!tok[0]) break;
        if(findhudpart(tok) || (!v[0] && !v[1] && v[2] == 1 && v[3] == 1 && !v[4])) continue;
        defformatstring(e, "%s %.4f %.4f %.3f %.3f %d ", tok, v[0], v[1], v[2], v[3], v[4] ? 1 : 0);
        buf.put(e, strlen(e));
    }
    loopv(hudparts) if(hudmoved(hudparts[i]))
    {
        defformatstring(e, "%s %.4f %.4f %.3f %.3f %d ", hudparts[i]->id, hudparts[i]->dx, hudparts[i]->dy, hudparts[i]->sx, hudparts[i]->sy, hudparts[i]->keeptext ? 1 : 0);
        buf.put(e, strlen(e));
    }
    while(buf.length() && buf.last() == ' ') buf.pop();
    buf.add('\0');
    setsvar("hudlayout", buf.getbuf());
    hudlayoutloaded = true; // the values in memory are the ones just written
}

// the part being drawn (parts do not nest; a nested begin is ignored)
static hudpart *curhudpart = NULL;
static int hudnesting = 0;

static hudpart *newhudpart(const char *id, const char *label)
{
    hudpart *p = hudparts.add(new hudpart);
    copystring(p->id, id);
    copystring(p->label, label && label[0] ? label : id);
    savedlayout(id, p->dx, p->dy, p->sx, p->sy, p->keeptext);
    p->x0 = p->y0 = p->x1 = p->y1 = 0;
    p->drawn = -1;
    p->sxused = p->syused = 1;
    p->oxused = p->oyused = 0;
    p->gathering = false;
    return p;
}

// the offset in pixels, keeping the part entirely on the screen (another
// resolution, a size set by hand)
static void hudoffset(const hudpart *p, float sx, float sy, float &ox, float &oy)
{
    ox = p->dx*screenw;
    oy = p->dy*screenh;
    if(p->x1 <= p->x0) return;
    float w = (p->x1 - p->x0)*sx, h = (p->y1 - p->y0)*sy;
    ox = clamp(ox, -p->x0, max(screenw - w - p->x0, -p->x0));
    oy = clamp(oy, -p->y0, max(screenh - h - p->y0, -p->y0));
}

void hudbegin(const char *id, const char *label)
{
    if(hudnesting++) return;
    loadhudboxes();
    hudpart *p = findhudpart(id);
    if(!p) p = newhudpart(id, label);
    loadhudlayout();
    curhudpart = p;
    // start a new rect on the first draw of this frame (a part may draw in several calls)
    if(p->drawn != totalmillis || !p->gathering) { p->gathering = true; p->fx0 = p->fy0 = 1e9f; p->fx1 = p->fy1 = -1e9f; }
    p->drawn = totalmillis;
    // move and size it in normalized device space, so it does not depend on
    // the scale the part draws with: the size grows from the part's top left
    // corner (known from the last frame), then the offset moves it
    bool known = p->x1 > p->x0;
    float sx = known ? p->sx : 1, sy = known ? p->sy : 1;
    p->sxused = sx;
    p->syused = sy;
    float ox, oy;
    hudoffset(p, sx, sy, ox, oy);
    p->oxused = ox;
    p->oyused = oy;
    float ax = 2*p->x0/screenw - 1, ay = 1 - 2*p->y0/screenh;
    float tx = ax*(1-sx) + 2*ox/screenw, ty = ay*(1-sy) - 2*oy/screenh;
    pushhudmatrix();
    vec4 *cols[4] = { &hudmatrix.a, &hudmatrix.b, &hudmatrix.c, &hudmatrix.d };
    loopi(4)
    {
        vec4 &c = *cols[i];
        c.x = sx*c.x + tx*c.w;
        c.y = sy*c.y + ty*c.w;
    }
    // letters at their normal size inside a resized part (draw_text undoes the stretch)
    extern float hudtextscalex, hudtextscaley;
    if(p->keeptext) { hudtextscalex = 1/sx; hudtextscaley = 1/sy; }
    flushhudmatrix();
}

void hudend()
{
    if(--hudnesting > 0) return;
    hudnesting = 0;
    if(!curhudpart) return;
    pophudmatrix();
    extern float hudtextscalex, hudtextscaley;
    hudtextscalex = hudtextscaley = 1;
    hudpart *p = curhudpart;
    if(p->fx0 <= p->fx1)
    {
        // keep the rect without the offset and size: the editor applies the current ones
        float ox = p->oxused, oy = p->oyused;
        float nx0 = p->fx0 - ox, ny0 = p->fy0 - oy;
        float nx1 = nx0 + (p->fx1 - p->fx0)/p->sxused, ny1 = ny0 + (p->fy1 - p->fy0)/p->syused;
        if(fabs(nx0 - p->x0) > 2 || fabs(ny0 - p->y0) > 2 || fabs(nx1 - p->x1) > 2 || fabs(ny1 - p->y1) > 2) hudboxesdirty = true;
        p->x0 = nx0; p->y0 = ny0;
        p->x1 = nx1; p->y1 = ny1;
    }
    curhudpart = NULL;
}

// where the current part drew, in its own coordinates
void hudrect(float x, float y, float w, float h)
{
    hudpart *p = curhudpart;
    if(!p || w <= 0 || h <= 0) return;
    loopi(4)
    {
        vec4 o;
        hudmatrix.transform(vec(i&1 ? x+w : x, i&2 ? y+h : y, 0), o);
        if(o.w == 0) continue;
        float sx = (o.x/o.w + 1)*0.5f*screenw, sy = (1 - o.y/o.w)*0.5f*screenh;
        p->fx0 = min(p->fx0, sx); p->fy0 = min(p->fy0, sy);
        p->fx1 = max(p->fx1, sx); p->fy1 = max(p->fy1, sy);
    }
}

// ---- the editor ----

VARF(hudediting, 0, 0, 1, { if(!hudediting) savehudlayout(); });
static hudpart *hudgrab = NULL;
static int grabhx = 0, grabhy = 0; // edge / corner held: -1 left/top, 1 right/bottom, 0 none (move)
static float grabcx, grabcy, grabdx, grabdy, grabsx, grabsy;

bool hudeditactive() { return hudediting != 0; }

// known place (drawn this session, or remembered from an earlier one)
static bool hudvisible(hudpart *p) { return p->x1 > p->x0; }
static bool hudshownnow(hudpart *p) { return p->drawn >= 0 && totalmillis - p->drawn < 2000; }

// the part on screen: moved and sized
static void hudscreenrect(const hudpart *p, float &x0, float &y0, float &x1, float &y1)
{
    float ox, oy;
    hudoffset(p, p->sx, p->sy, ox, oy);
    x0 = p->x0 + ox;
    y0 = p->y0 + oy;
    x1 = x0 + (p->x1 - p->x0)*p->sx;
    y1 = y0 + (p->y1 - p->y0)*p->sy;
}

static float hudhandle() { return max(10.0f, screenh/90.0f); }

// part under the cursor (the smallest, so a part inside another stays
// reachable) and which edge or corner, if the cursor is on one
static hudpart *hudhit(float cx, float cy, int &hx, int &hy)
{
    hudpart *best = NULL;
    float bestarea = 1e18f, m = hudhandle();
    hx = hy = 0;
    loopv(hudparts)
    {
        hudpart *p = hudparts[i];
        if(!hudvisible(p)) continue;
        float x0, y0, x1, y1;
        hudscreenrect(p, x0, y0, x1, y1);
        if(cx < x0-m || cx > x1+m || cy < y0-m || cy > y1+m) continue;
        float area = (x1-x0)*(y1-y0);
        if(area >= bestarea) continue;
        best = p;
        bestarea = area;
        // small parts: the handles would cover them, keep their middle for moving
        float mx = min(m, (x1-x0)/3), my = min(m, (y1-y0)/3);
        hx = cx < x0+mx ? -1 : (cx > x1-mx ? 1 : 0);
        hy = cy < y0+my ? -1 : (cy > y1-my ? 1 : 0);
    }
    return best;
}

static void hudcursorpx(float &cx, float &cy)
{
    g3d_cursorpos(cx, cy);
    cx *= screenw;
    cy *= screenh;
}

static void huddragto(float cx, float cy);

static void huddrag()
{
    if(!hudgrab) return;
    float cx, cy;
    hudcursorpx(cx, cy);
    huddragto(cx, cy);
}

static void huddragto(float cx, float cy)
{
    hudpart *p = hudgrab;
    float bw = p->x1 - p->x0, bh = p->y1 - p->y0;
    if(!grabhx && !grabhy)
    {
        float dx = grabdx + (cx - grabcx)/screenw, dy = grabdy + (cy - grabcy)/screenh;
        // keep it on the screen
        dx = clamp(dx, -p->x0/screenw, (screenw - bw*p->sx - p->x0)/screenw);
        dy = clamp(dy, -p->y0/screenh, (screenh - bh*p->sy - p->y0)/screenh);
        p->dx = dx;
        p->dy = dy;
        return;
    }
    // resize: a left / right edge changes the width, a top / bottom edge the
    // height, a corner both (Shift on a corner: the same factor, proportions
    // kept); the opposite edge stays where it is
    float w0 = bw*grabsx, h0 = bh*grabsy;
    float X0 = p->x0 + grabdx*screenw, Y0 = p->y0 + grabdy*screenh; // on screen at the grab
    float mx = cx - grabcx, my = cy - grabcy;
    float rw = grabhx ? max(w0 + grabhx*mx, 1.0f)/w0 : 1, rh = grabhy ? max(h0 + grabhy*my, 1.0f)/h0 : 1;
    if(grabhx && grabhy && (SDL_GetModState() & KMOD_SHIFT)) rw = rh = max(rw, rh);
    float sx = clamp(grabsx*rw, HUDMINSIZE, HUDMAXSIZE), sy = clamp(grabsy*rh, HUDMINSIZE, HUDMAXSIZE);
    float w1 = bw*sx, h1 = bh*sy;
    float nx0 = grabhx < 0 ? X0 + w0 - w1 : X0;
    float ny0 = grabhy < 0 ? Y0 + h0 - h1 : Y0;
    p->sx = sx;
    p->sy = sy;
    p->dx = (nx0 - p->x0)/screenw;
    p->dy = (ny0 - p->y0)/screenh;
}

// mouse and keys while editing; true = used
bool hudeditkey(int code, bool isdown)
{
    if(!hudeditactive()) return false;
    if(code == SDLK_ESCAPE || code == SDLK_RETURN || code == SDLK_KP_ENTER)
    {
        if(isdown) { hudgrab = NULL; hudediting = 0; savehudlayout(); }
        return true;
    }
    if(code == -1)
    {
        float cx, cy;
        hudcursorpx(cx, cy);
        if(isdown)
        {
            hudgrab = hudhit(cx, cy, grabhx, grabhy);
            if(hudgrab) { grabcx = cx; grabcy = cy; grabdx = hudgrab->dx; grabdy = hudgrab->dy; grabsx = hudgrab->sx; grabsy = hudgrab->sy; }
        }
        else if(hudgrab) { huddrag(); hudgrab = NULL; savehudlayout(); }
        return true;
    }
    if(code == -2 || code == SDLK_t)
    {
        if(isdown)
        {
            float cx, cy;
            int hx, hy;
            hudcursorpx(cx, cy);
            if(hudpart *p = hudhit(cx, cy, hx, hy)) { p->keeptext = !p->keeptext; savehudlayout(); }
            else if(code == SDLK_t) return false; // T elsewhere: the usual key (chat)
        }
        return true;
    }
    if(code == -3)
    {
        if(isdown)
        {
            float cx, cy;
            int hx, hy;
            hudcursorpx(cx, cy);
            if(hudpart *p = hudhit(cx, cy, hx, hy)) { p->dx = p->dy = 0; p->sx = p->sy = 1; p->keeptext = false; savehudlayout(); }
        }
        return true;
    }
    return false; // other keys keep working (to leave with a bound key, chat...)
}

static void hudfill(float x0, float y0, float x1, float y1) { hudquad(x0, y0, x1-x0, y1-y0); }

static void hudframe(float x0, float y0, float x1, float y1, const vec &c, float a, bool handles, int hx, int hy)
{
    hudnotextureshader->set();
    gle::colorf(c.x, c.y, c.z, a*0.25f);
    hudfill(x0, y0, x1, y1);
    gle::colorf(c.x, c.y, c.z, a);
    float t = max(2.0f, screenh/720.0f);
    hudfill(x0, y0, x1, y0+t);
    hudfill(x0, y1-t, x1, y1);
    hudfill(x0, y0, x0+t, y1);
    hudfill(x1-t, y0, x1, y1);
    if(handles)
    {
        // corner squares, and the edge or corner under the cursor in white
        float m = hudhandle()*0.8f;
        loopi(4) hudfill(i&1 ? x1-m : x0, i&2 ? y1-m : y0, i&1 ? x1 : x0+m, i&2 ? y1 : y0+m);
        if(hx || hy)
        {
            gle::colorf(1, 1, 1, 0.9f);
            float T = 2*t;
            if(hx < 0) hudfill(x0, y0, x0+T, y1);
            if(hx > 0) hudfill(x1-T, y0, x1, y1);
            if(hy < 0) hudfill(x0, y0, x1, y0+T);
            if(hy > 0) hudfill(x0, y1-T, x1, y1);
        }
    }
    hudshader->set();
    gle::colorf(1, 1, 1);
}

// frames, names and help on top of the HUD (screen pixels)
void drawhudeditor(int w, int h)
{
    // remember where the parts are, now and then (saved with the settings)
    if(hudboxesdirty && totalmillis - hudboxessaved > 5000) { savehudboxes(); hudboxesdirty = false; hudboxessaved = totalmillis; }
    if(!hudeditactive()) { hudgrab = NULL; return; }
    loadhudboxes();
    huddrag();
    float cx, cy;
    hudcursorpx(cx, cy);
    int hx = grabhx, hy = grabhy;
    hudpart *hover = hudgrab;
    if(!hover) hover = hudhit(cx, cy, hx, hy);
    hudmatrix.ortho(0, screenw, screenh, 0, -1, 1);
    resethudmatrix();
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    float fs = max(screenh/1800.0f, 0.4f); // label scale
    loopv(hudparts)
    {
        hudpart *p = hudparts[i];
        if(!hudvisible(p)) continue;
        float x0, y0, x1, y1;
        hudscreenrect(p, x0, y0, x1, y1);
        x0 -= 4; y0 -= 4; x1 += 4; y1 += 4;
        bool hot = p == hover, now = hudshownnow(p);
        hudframe(x0, y0, x1, y1, hot ? vec(1, 0.6f, 0.1f) : (now ? vec(0.3f, 0.7f, 1) : vec(0.6f, 0.6f, 0.6f)), hot ? 0.9f : (now ? 0.6f : 0.4f),
                 hot, hot ? hx : 0, hot ? hy : 0);
        string label;
        int pw = int(p->sx*100 + 0.5f), ph = int(p->sy*100 + 0.5f);
        if(pw != ph) formatstring(label, "%s  %d%% x %d%%", p->label, pw, ph);
        else if(pw != 100) formatstring(label, "%s  %d%%", p->label, pw);
        else copystring(label, p->label);
        if(p->keeptext) concatstring(label, "  (text: normal size)");
        int lw, lh;
        text_bounds(label, lw, lh);
        pushhudmatrix();
        hudmatrix.translate(clamp(x0, 0.0f, max(screenw - lw*fs, 0.0f)), max(y0 - FONTH*fs, 0.0f), 0);
        hudmatrix.scale(fs, fs, 1);
        flushhudmatrix();
        draw_text(label, 0, 0, hot ? 255 : 150, hot ? 180 : 210, hot ? 60 : 255, 230);
        pophudmatrix();
    }
    const char *help = "HUD editor: drag inside a frame to move it, drag an edge or a corner to resize it (Shift: keep proportions), T or middle click = text keeps its size or not, right click = original, Esc or Enter when done";
    int tw, th;
    text_bounds(help, tw, th);
    float hs = min(fs, (screenw - 40.0f)/max(tw, 1)); // fits the width
    pushhudmatrix();
    hudmatrix.translate((screenw - tw*hs)/2, screenh*0.8f, 0); // low in the middle, where no part sits by default
    hudmatrix.scale(hs, hs, 1);
    flushhudmatrix();
    draw_text(help, 0, 0, 255, 220, 120, 255);
    pophudmatrix();
}

ICOMMAND(hudedit, "", (),
{
    hudediting = hudediting ? 0 : 1;
    extern void closeallguis();
    if(hudediting) closeallguis(); // the menus would hide the HUD (the main menu comes back after)
    else { savehudlayout(); savehudboxes(); }
});

ICOMMAND(hudlayoutreset, "", (),
{
    loopv(hudparts) { hudparts[i]->dx = hudparts[i]->dy = 0; hudparts[i]->sx = hudparts[i]->sy = 1; hudparts[i]->keeptext = false; }
    setsvar("hudlayout", "");
    hudlayoutloaded = true;
});

// tests: the same grab and drag as the mouse, from and to points given as
// fractions of the screen (hudtestdrag fx0 fy0 fx1 fy1 [shift])
static void hudtestdrag(float *fx0, float *fy0, float *fx1, float *fy1)
{
    float cx = *fx0*screenw, cy = *fy0*screenh;
    hudgrab = hudhit(cx, cy, grabhx, grabhy);
    if(!hudgrab) { conoutf("hudtestdrag: nothing there"); return; }
    grabcx = cx; grabcy = cy; grabdx = hudgrab->dx; grabdy = hudgrab->dy; grabsx = hudgrab->sx; grabsy = hudgrab->sy;
    huddragto(*fx1*screenw, *fy1*screenh);
    conoutf("hudtestdrag: %s handle %d %d -> offset %.3f %.3f size %.2f x %.2f", hudgrab->id, grabhx, grabhy, hudgrab->dx, hudgrab->dy, hudgrab->sx, hudgrab->sy);
    hudgrab = NULL;
    savehudlayout();
}
COMMAND(hudtestdrag, "ffff");
