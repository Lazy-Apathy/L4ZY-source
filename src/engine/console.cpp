// console.cpp: the console buffer, its display, and command line control

#include "engine.h"

#define MAXCONLINES 1000
#define MAXCHATLINES 20000
#define CHATLOG_FILTER 0x300  // CON_CHAT | CON_TEAMCHAT (fpsgame/game.h)
struct cline { char *line; int type, outtime, uid; };
reversequeue<cline, MAXCONLINES> conlines;
static vector<cline> chatlog;
static int nextconuid = 1, lastconuid = 0;
static int chatskip = 0;

int commandmillis = -1;
string commandbuf;
char *commandaction = NULL, *commandprompt = NULL;
enum { CF_COMPLETE = 1<<0, CF_EXECUTE = 1<<1 };
int commandflags = 0, commandpos = -1;

VARFP(maxcon, 10, 200, MAXCONLINES, { while(conlines.length() > maxcon) delete[] conlines.pop().line; });

#define CONSTRLEN 512

VARP(contags, 0, 3, 3);

void conline(int type, const char *sf)        // add a line to the console buffer
{
    char *buf = NULL;
    if(type&CON_TAG_MASK) for(int i = conlines.length()-1; i >= max(conlines.length()-contags, 0); i--)
    {
        int prev = conlines.removing(i).type;
        if(!(prev&CON_TAG_MASK)) break;
        if(type == prev)
        {
            buf = conlines.remove(i).line;
            break;
        }
    }
    if(!buf) buf = conlines.length() >= maxcon ? conlines.remove().line : newstring("", CONSTRLEN-1);
    cline &cl = conlines.add();
    cl.line = buf;
    cl.type = type;
    cl.outtime = totalmillis;                // for how long to keep line on screen
    cl.uid = nextconuid++;
    if(nextconuid <= 0) nextconuid = 1;
    lastconuid = cl.uid;
    copystring(cl.line, sf, CONSTRLEN);
    if(type & CHATLOG_FILTER)
    {
        while(chatlog.length() >= MAXCHATLINES)
        {
            game::chatlogdropped(chatlog[0].uid);
            delete[] chatlog[0].line;
            chatlog.remove(0);
        }
        cline &ch = chatlog.add();
        ch.line = newstring("", CONSTRLEN-1);
        copystring(ch.line, sf, CONSTRLEN);
        ch.type = type;
        ch.outtime = cl.outtime;
        ch.uid = cl.uid;
    }
}

static bool chatlogreplace(int id, int type, const char *sf)
{
    loopvrev(chatlog) if(chatlog[i].uid == id)
    {
        copystring(chatlog[i].line, sf, CONSTRLEN);
        chatlog[i].type = type;
        chatlog[i].outtime = totalmillis;
        return true;
    }
    return false;
}

void clearchatlog()
{
    loopv(chatlog) delete[] chatlog[i].line;
    chatlog.setsize(0);
    chatskip = 0;
    extern int fullchat;
    fullchat = 0;
    game::chatlogcleared();
}

int chatloglen()
{
    return chatlog.length();
}

bool chatlogfind(int num, int &uid, int &type)
{
    int i = num - 1;
    if(!chatlog.inrange(i)) return false;
    uid = chatlog[i].uid;
    type = chatlog[i].type;
    return true;
}

void conoutfv(int type, const char *fmt, va_list args)
{
    static char buf[CONSTRLEN];
    vformatstring(buf, fmt, args, sizeof(buf));
    conline(type, buf);
    logoutf("%s", buf);
}

int conoutfid(int type, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    conoutfv(type, fmt, args);
    va_end(args);
    return lastconuid;
}

bool conreplaceid(int id, int type, const char *fmt, ...)
{
    static char buf[CONSTRLEN];
    va_list args;
    va_start(args, fmt);
    vformatstring(buf, fmt, args, sizeof(buf));
    va_end(args);
    bool inchat = chatlogreplace(id, type, buf);
    loopvrev(conlines) if(conlines[i].uid == id)
    {
        copystring(conlines[i].line, buf, CONSTRLEN);
        conlines[i].type = type;
        conlines[i].outtime = totalmillis;
        logoutf("%s", buf);
        return true;
    }
    if(inchat)
    {
        logoutf("%s", buf);
        return true;
    }
    conline(type, buf);
    logoutf("%s", buf);
    return false;
}

VAR(fullconsole, 0, 0, 1);
VAR(fullchat, 0, 0, 1);
VARP(fullchatsize, 0, 75, 100);

struct overlayhit { int index, y, h; };
static vector<overlayhit> overlayhits;
static int overlaykind = 0; // 0 none, 1 chat, 2 console
static int overlayselstart = -1, overlayselend = -1;
static bool overlaydragging = false;

static void clearoverlaysel()
{
    overlayselstart = overlayselend = -1;
    overlaydragging = false;
}

ICOMMAND(toggleconsole, "", (), { fullconsole ^= 1; if(fullconsole) { fullchat = 0; game::fullkilllog = 0; } clearoverlaysel(); });
ICOMMAND(togglechat, "", (), { fullchat ^= 1; if(fullchat) { fullconsole = 0; game::fullkilllog = 0; chatskip = 0; } clearoverlaysel(); });

bool overlaycursor()
{
    return ((fullchat || fullconsole) && commandmillis < 0) || game::demobarcursor();
}

#ifdef __APPLE__
#define OVERLAY_MOD (KMOD_LGUI|KMOD_RGUI)
#else
#define OVERLAY_MOD (KMOD_LCTRL|KMOD_RCTRL)
#endif

static bool overlayselected(int idx)
{
    if(overlayselstart < 0 || overlayselend < 0) return false;
    int a = min(overlayselstart, overlayselend), b = max(overlayselstart, overlayselend);
    return idx >= a && idx <= b;
}

static void overlaybar(int x, int y, int w, int h)
{
    hudnotextureshader->set();
    gle::colorf(0.2f, 0.45f, 0.85f, 0.35f);
    hudquad(x, y, w, h);
    hudshader->set();
}

static int overlayhitat()
{
    extern float conscale;
    if(conscale <= 0) return -1;
    float cx, cy;
    g3d_cursorpos(cx, cy);
    int my = int(cy * (screenh/conscale));
    loopv(overlayhits)
        if(my >= overlayhits[i].y && my < overlayhits[i].y + overlayhits[i].h)
            return overlayhits[i].index;
    return -1;
}

static void overlayappend(vector<char> &buf, const char *src)
{
    char plain[CONSTRLEN];
    filtertext(plain, src, true, false, sizeof(plain)-1);
    uchar utf[CONSTRLEN*4];
    size_t n = encodeutf8(utf, sizeof(utf)-1, (const uchar *)plain, strlen(plain));
    utf[n] = 0;
    if(buf.length()) buf.add('\n');
    buf.put((char *)utf, (int)n);
}

extern int fullconfilter;

static void copyoverlay(int mode) // 0 all, 1 visible, 2 selection
{
    vector<char> buf;
    int n = 0;
    if(fullchat)
    {
        int a = 0, b = chatlog.length()-1;
        if(mode == 1 && overlayhits.length())
        {
            a = overlayhits[0].index;
            b = overlayhits.last().index;
        }
        else if(mode == 2)
        {
            if(overlayselstart < 0 || overlayselend < 0 || !chatlog.length())
            {
                conoutf("no chat lines selected");
                return;
            }
            a = min(overlayselstart, overlayselend);
            b = max(overlayselstart, overlayselend);
        }
        a = clamp(a, 0, chatlog.length()-1);
        b = clamp(b, 0, chatlog.length()-1);
        if(chatlog.length()) for(int i = a; i <= b; i++)
        {
            overlayappend(buf, chatlog[i].line);
            n++;
        }
    }
    else if(fullconsole)
    {
        int filter = fullconfilter & CON_FLAGS;
        if(mode == 1 && overlayhits.length())
        {
            loopv(overlayhits)
            {
                overlayappend(buf, conlines[overlayhits[i].index].line);
                n++;
            }
        }
        else if(mode == 2)
        {
            if(overlayselstart < 0 || overlayselend < 0)
            {
                conoutf("no console lines selected");
                return;
            }
            int a = min(overlayselstart, overlayselend), b = max(overlayselstart, overlayselend);
            for(int i = b; i >= a; i--) if(conlines.inrange(i) && (conlines[i].type&filter))
            {
                overlayappend(buf, conlines[i].line);
                n++;
            }
        }
        else
        {
            for(int i = conlines.length()-1; i >= 0; i--) if(conlines[i].type&filter)
            {
                overlayappend(buf, conlines[i].line);
                n++;
            }
        }
    }
    else return;

    if(!n)
    {
        conoutf(fullchat ? "no chat lines to copy" : "no console lines to copy");
        return;
    }
    buf.add('\0');
    if(SDL_SetClipboardText(buf.getbuf()) != 0)
    {
        conoutf("clipboard copy failed");
        return;
    }
    conoutf("copied %d %s", n, fullchat ? "chat lines" : "console lines");
}

int rendercommand(int x, int y, int w)
{
    if(commandmillis < 0) return 0;

    defformatstring(s, "%s %s", commandprompt ? commandprompt : ">", commandbuf);
    int width, height;
    text_bounds(s, width, height, w);
    int extra = 0;
    const char *preview = game::translatepreviewline();
    if(preview && preview[0])
    {
        int pw, ph;
        text_bounds(preview, pw, ph, w);
        extra = ph;
    }
    y -= height + extra;
    if(preview && preview[0]) draw_text(preview, x, y, 0xFF, 0xFF, 0xFF, 0xFF, -1, w);
    draw_text(s, x, y + extra, 0xFF, 0xFF, 0xFF, 0xFF, (commandpos>=0) ? (commandpos+1+(commandprompt?strlen(commandprompt):1)) : strlen(s), w);
    return height + extra;
}

VARP(consize, 0, 5, 100);
VARP(miniconsize, 0, 5, 100);
VARP(miniconwidth, 0, 40, 100);
VARP(confade, 0, 30, 60);
VARP(miniconfade, 0, 30, 60);
VARP(fullconsize, 0, 75, 100);
HVARP(confilter, 0, 0x7FFFFFF, 0x7FFFFFF);
HVARP(fullconfilter, 0, 0x7FFFFFF, 0x7FFFFFF);
HVARP(miniconfilter, 0, 0, 0x7FFFFFF);

int conskip = 0, miniconskip = 0;

void setconskip(int &skip, int filter, int n)
{
    filter &= CON_FLAGS;
    int offset = abs(n), dir = n < 0 ? -1 : 1;
    skip = clamp(skip, 0, conlines.length()-1);
    while(offset)
    {
        skip += dir;
        if(!conlines.inrange(skip))
        {
            skip = clamp(skip, 0, conlines.length()-1);
            return;
        }
        if(conlines[skip].type&filter) --offset;
    }
}

ICOMMAND(conskip, "i", (int *n), { if(game::fullkilllog) game::skipkilllog(*n); else if(fullchat) { int numl = chatlog.length(); chatskip = numl ? clamp(chatskip + *n, 0, numl-1) : 0; } else setconskip(conskip, fullconsole ? fullconfilter : confilter, *n); });
ICOMMAND(miniconskip, "i", (int *n), setconskip(miniconskip, miniconfilter, *n));
ICOMMAND(chatskip, "i", (int *n), { int numl = chatlog.length(); chatskip = numl ? clamp(chatskip + *n, 0, numl-1) : 0; });

ICOMMAND(clearconsole, "", (), { while(conlines.length()) delete[] conlines.pop().line; });

int drawconlines(int conskip, int confade, int conwidth, int conheight, int conoff, int filter, int y = 0, int dir = 1, bool recordhits = false)
{
    filter &= CON_FLAGS;
    int numl = conlines.length(), offset = min(conskip, numl);
    if(recordhits)
    {
        overlayhits.setsize(0);
        overlaykind = 2;
    }

    if(confade)
    {
        if(!conskip)
        {
            numl = 0;
            loopvrev(conlines) if(totalmillis-conlines[i].outtime < confade*1000) { numl = i+1; break; }
        }
        else offset--;
    }

    int totalheight = 0;
    loopi(numl) //determine visible height
    {
        // shuffle backwards to fill if necessary
        int idx = offset+i < numl ? offset+i : --offset;
        if(!(conlines[idx].type&filter)) continue;
        char *line = conlines[idx].line;
        int width, height;
        text_bounds(line, width, height, conwidth);
        if(totalheight + height > conheight) { numl = i; if(offset == idx) ++offset; break; }
        totalheight += height;
    }
    if(dir > 0) y = conoff;
    loopi(numl)
    {
        int idx = offset + (dir > 0 ? numl-i-1 : i);
        if(!(conlines[idx].type&filter)) continue;
        char *line = conlines[idx].line;
        int width, height;
        text_bounds(line, width, height, conwidth);
        if(dir <= 0) y -= height;
        if(recordhits && overlayselected(idx)) overlaybar(conoff, y, conwidth, height);
        draw_text(line, conoff, y, 0xFF, 0xFF, 0xFF, 0xFF, -1, conwidth);
        if(recordhits)
        {
            overlayhit &oh = overlayhits.add();
            oh.index = idx;
            oh.y = y;
            oh.h = height;
        }
        if(dir > 0) y += height;
    }
    return y+conoff;
}

static void setchatskip(int n)
{
    int numl = chatlog.length();
    chatskip = numl ? clamp(chatskip + n, 0, numl-1) : 0;
}

static int chatnumwidth(int numl)
{
    int digits = 1;
    for(int v = max(numl, 1); v >= 10; v /= 10) digits++;
    defformatstring(sample, "%*d ", digits, numl);
    int w, h;
    text_bounds(sample, w, h);
    return w;
}

static int drawchatlog(int conwidth, int conheight, int conoff)
{
    overlayhits.setsize(0);
    overlaykind = 1;
    int numl = chatlog.length();
    if(!numl) return conoff;
    int numw = chatnumwidth(numl);
    int textw = max(conwidth - numw, FONTW);
    int digits = 1;
    for(int v = numl; v >= 10; v /= 10) digits++;
    int skip = clamp(chatskip, 0, numl-1);
    int last = numl - 1 - skip;
    int totalheight = 0, first = last;
    for(int i = last; i >= 0; i--)
    {
        int width, height;
        text_bounds(chatlog[i].line, width, height, textw);
        if(totalheight + height > conheight) break;
        totalheight += height;
        first = i;
    }
    int y = conoff;
    for(int i = first; i <= last; i++)
    {
        int width, height;
        text_bounds(chatlog[i].line, width, height, textw);
        if(overlayselected(i)) overlaybar(conoff, y, conwidth, height);
        defformatstring(num, "\f4%*d ", digits, i+1);
        draw_text(num, conoff, y, 0xFF, 0xFF, 0xFF, 0xFF, -1, numw);
        draw_text(chatlog[i].line, conoff + numw, y, 0xFF, 0xFF, 0xFF, 0xFF, -1, textw);
        overlayhit &oh = overlayhits.add();
        oh.index = i;
        oh.y = y;
        oh.h = height;
        y += height;
    }
    return y+conoff;
}

static bool chatkey(int code, bool isdown)
{
    if(!fullchat || !isdown) return false;
    switch(code)
    {
        case -4:
            setchatskip(3);
            return true;
        case -5:
            setchatskip(-3);
            return true;
        case SDLK_PAGEUP:
            setchatskip(8);
            return true;
        case SDLK_PAGEDOWN:
            setchatskip(-8);
            return true;
        case SDLK_HOME:
            if(commandmillis >= 0) return false;
            chatskip = chatlog.length() ? chatlog.length()-1 : 0;
            return true;
        case SDLK_END:
            if(commandmillis >= 0) return false;
            chatskip = 0;
            return true;
        default:
            return false;
    }
}

static bool overlaykey(int code, bool isdown)
{
    if(!fullchat && !fullconsole) return false;

    if(isdown && code == SDLK_c && (SDL_GetModState() & OVERLAY_MOD))
    {
        bool visonly = (SDL_GetModState() & KMOD_SHIFT) != 0;
        if(visonly) copyoverlay(1);
        else if(overlayselstart >= 0) copyoverlay(2);
        else copyoverlay(0);
        return true;
    }

    if(commandmillis >= 0) return false;
    if(code != -1) return false;

    if(isdown)
    {
        int hit = overlayhitat();
        if(hit >= 0)
        {
            overlayselstart = overlayselend = hit;
            overlaydragging = true;
        }
        else clearoverlaysel();
    }
    else overlaydragging = false;
    return true;
}

int renderconsole(int w, int h, int abovehud)                   // render buffer taking into account time & scrolling
{
    extern void consolebox(int x1, int y1, int x2, int y2);
    if(overlaydragging && overlaycursor())
    {
        int hit = overlayhitat();
        if(hit >= 0) overlayselend = hit;
    }
    if(game::fullkilllog)
    {
        int size = clamp(game::fullkilllogsize, 0, 100);
        int conpad = 0, conoff = FONTH,
            conheight = min(max(((h*size/100)/FONTH)*FONTH, FONTH), h - 2*(conpad + conoff)),
            conwidth = w - 2*(conpad + conoff);
        consolebox(conpad, conpad, conwidth+conpad+2*conoff, conheight+conpad+2*conoff);
        game::drawkilllog(conwidth, conheight, conpad+conoff);
        return conheight + 2*(conpad + conoff);
    }
    if(fullchat)
    {
        int conpad = 0, conoff = FONTH,
            conheight = min(max(((h*fullchatsize/100)/FONTH)*FONTH, FONTH), h - 2*(conpad + conoff)),
            conwidth = w - 2*(conpad + conoff);
        consolebox(conpad, conpad, conwidth+conpad+2*conoff, conheight+conpad+2*conoff);
        drawchatlog(conwidth, conheight, conpad+conoff);
        return conheight + 2*(conpad + conoff);
    }
    int conpad = fullconsole ? 0 : FONTH/4,
        conoff = fullconsole ? FONTH : FONTH/3,
        conheight = min(fullconsole ? ((h*fullconsize/100)/FONTH)*FONTH : FONTH*consize, h - 2*(conpad + conoff)),
        conwidth = w - 2*(conpad + conoff) - (fullconsole ? 0 : game::clipconsole(w, h));
    
    if(fullconsole) consolebox(conpad, conpad, conwidth+conpad+2*conoff, conheight+conpad+2*conoff);
    
    int y = drawconlines(conskip, fullconsole ? 0 : confade, conwidth, conheight, conpad+conoff, fullconsole ? fullconfilter : confilter, 0, 1, fullconsole!=0);
    if(!fullconsole && (miniconsize && miniconwidth))
        drawconlines(miniconskip, miniconfade, (miniconwidth*(w - 2*(conpad + conoff)))/100, min(FONTH*miniconsize, abovehud - y), conpad+conoff, miniconfilter, abovehud, -1);
    return fullconsole ? conheight + 2*(conpad + conoff) : y;
}

// keymap is defined externally in keymap.cfg

struct keym
{
    enum
    {
        ACTION_DEFAULT = 0,
        ACTION_SPECTATOR,
        ACTION_EDITING,
        NUMACTIONS
    };
    
    int code;
    char *name;
    char *actions[NUMACTIONS];
    bool pressed;

    keym() : code(-1), name(NULL), pressed(false) { loopi(NUMACTIONS) actions[i] = newstring(""); }
    ~keym() { DELETEA(name); loopi(NUMACTIONS) DELETEA(actions[i]); }
};

hashtable<int, keym> keyms(128);

void keymap(int *code, char *key)
{
    if(identflags&IDF_OVERRIDDEN) { conoutf(CON_ERROR, "cannot override keymap %d", *code); return; }
    keym &km = keyms[*code];
    km.code = *code;
    DELETEA(km.name);
    km.name = newstring(key);
}
    
COMMAND(keymap, "is");

keym *keypressed = NULL;
char *keyaction = NULL;

const char *getkeyname(int code)
{
    keym *km = keyms.access(code);
    return km ? km->name : NULL;
}

void searchbinds(char *action, int type)
{
    vector<char> names;
    enumerate(keyms, keym, km,
    {
        if(!strcmp(km.actions[type], action))
        {
            if(names.length()) names.add(' ');
            names.put(km.name, strlen(km.name));
        }
    });
    names.add('\0');
    result(names.getbuf());
}

keym *findbind(char *key)
{
    enumerate(keyms, keym, km,
    {
        if(!strcasecmp(km.name, key)) return &km;
    });
    return NULL;
}   
    
void getbind(char *key, int type)
{
    keym *km = findbind(key);
    result(km ? km->actions[type] : "");
}   

void bindkey(char *key, char *action, int state, const char *cmd)
{
    if(identflags&IDF_OVERRIDDEN) { conoutf(CON_ERROR, "cannot override %s \"%s\"", cmd, key); return; }
    keym *km = findbind(key);
    if(!km) { conoutf(CON_ERROR, "unknown key \"%s\"", key); return; }
    char *&binding = km->actions[state];
    if(!keypressed || keyaction!=binding) delete[] binding;
    // trim white-space to make searchbinds more reliable
    while(iscubespace(*action)) action++;
    int len = strlen(action);
    while(len>0 && iscubespace(action[len-1])) len--;
    binding = newstring(action, len);
}

ICOMMAND(bind,     "ss", (char *key, char *action), bindkey(key, action, keym::ACTION_DEFAULT, "bind"));
ICOMMAND(specbind, "ss", (char *key, char *action), bindkey(key, action, keym::ACTION_SPECTATOR, "specbind"));
ICOMMAND(editbind, "ss", (char *key, char *action), bindkey(key, action, keym::ACTION_EDITING, "editbind"));
ICOMMAND(getbind,     "s", (char *key), getbind(key, keym::ACTION_DEFAULT));
ICOMMAND(getspecbind, "s", (char *key), getbind(key, keym::ACTION_SPECTATOR));
ICOMMAND(geteditbind, "s", (char *key), getbind(key, keym::ACTION_EDITING));
ICOMMAND(searchbinds,     "s", (char *action), searchbinds(action, keym::ACTION_DEFAULT));
ICOMMAND(searchspecbinds, "s", (char *action), searchbinds(action, keym::ACTION_SPECTATOR));
ICOMMAND(searcheditbinds, "s", (char *action), searchbinds(action, keym::ACTION_EDITING));

void inputcommand(char *init, char *action = NULL, char *prompt = NULL, char *flags = NULL) // turns input to the command line on or off
{
    commandmillis = init ? totalmillis : -1;
    textinput(commandmillis >= 0, TI_CONSOLE);
    keyrepeat(commandmillis >= 0, KR_CONSOLE);
    copystring(commandbuf, init ? init : "");
    DELETEA(commandaction);
    DELETEA(commandprompt);
    commandpos = -1;
    if(action && action[0]) commandaction = newstring(action);
    if(prompt && prompt[0]) commandprompt = newstring(prompt);
    commandflags = 0;
    if(flags) while(*flags) switch(*flags++)
    {
        case 'c': commandflags |= CF_COMPLETE; break;
        case 'x': commandflags |= CF_EXECUTE; break;
        case 's': commandflags |= CF_COMPLETE|CF_EXECUTE; break;
    }
    else if(init) commandflags |= CF_COMPLETE|CF_EXECUTE;
}

ICOMMAND(saycommand, "C", (char *init), inputcommand(init));
COMMAND(inputcommand, "ssss");

void pasteconsole()
{
    if(!SDL_HasClipboardText()) return;
    char *cb = SDL_GetClipboardText();
    if(!cb) return;
    size_t cblen = strlen(cb),
           commandlen = strlen(commandbuf),
           decoded = decodeutf8((uchar *)&commandbuf[commandlen], sizeof(commandbuf)-1-commandlen, (const uchar *)cb, cblen);
    commandbuf[commandlen + decoded] = '\0';
    SDL_free(cb);
}

struct hline
{
    char *buf, *action, *prompt;
    int flags;

    hline() : buf(NULL), action(NULL), prompt(NULL), flags(0) {}
    ~hline()
    {
        DELETEA(buf);
        DELETEA(action);
        DELETEA(prompt);
    }

    void restore()
    {
        copystring(commandbuf, buf);
        if(commandpos >= (int)strlen(commandbuf)) commandpos = -1;
        DELETEA(commandaction);
        DELETEA(commandprompt);
        if(action) commandaction = newstring(action);
        if(prompt) commandprompt = newstring(prompt);
        commandflags = flags;
    }

    bool shouldsave()
    {
        return strcmp(commandbuf, buf) ||
               (commandaction ? !action || strcmp(commandaction, action) : action!=NULL) ||
               (commandprompt ? !prompt || strcmp(commandprompt, prompt) : prompt!=NULL) ||
               commandflags != flags;
    }
    
    void save()
    {
        buf = newstring(commandbuf);
        if(commandaction) action = newstring(commandaction);
        if(commandprompt) prompt = newstring(commandprompt);
        flags = commandflags;
    }

    void run()
    {
        if(flags&CF_EXECUTE && buf[0]=='/')
        {
            if(!game::trytranslatedcmd(buf)) execute(buf+1);
        }
        else if(game::trytranslatedsay(buf, action)) {}
        else if(action)
        {
            alias("commandbuf", buf);
            execute(action);
        }
        else game::toserver(buf);
    }
};
vector<hline *> history;
int histpos = 0;

VARP(maxhistory, 0, 1000, 10000);

void history_(int *n)
{
    static bool inhistory = false;
    if(!inhistory && history.inrange(*n))
    {
        inhistory = true;
        history[history.length()-*n-1]->run();
        inhistory = false;
    }
}

COMMANDN(history, history_, "i");

struct releaseaction
{
    keym *key;
    char *action;
};
vector<releaseaction> releaseactions;

const char *addreleaseaction(char *s)
{
    if(!keypressed) { delete[] s; return NULL; }
    releaseaction &ra = releaseactions.add();
    ra.key = keypressed;
    ra.action = s;
    return keypressed->name;
}

void onrelease(const char *s)
{
    addreleaseaction(newstring(s));
}

COMMAND(onrelease, "s");

void execbind(keym &k, bool isdown)
{
    loopv(releaseactions)
    {
        releaseaction &ra = releaseactions[i];
        if(ra.key==&k)
        {
            if(!isdown) execute(ra.action);
            delete[] ra.action;
            releaseactions.remove(i--);
        }
    }
    if(isdown)
    {
        int state = keym::ACTION_DEFAULT;
        if(!mainmenu)
        {
            if(editmode) state = keym::ACTION_EDITING;
            else if(player->state==CS_SPECTATOR) state = keym::ACTION_SPECTATOR;
        }
        char *&action = k.actions[state][0] ? k.actions[state] : k.actions[keym::ACTION_DEFAULT];
        keyaction = action;
        keypressed = &k;
        execute(keyaction);
        keypressed = NULL;
        if(keyaction!=action) delete[] keyaction;
    }
    k.pressed = isdown;
}

bool consoleinput(const char *str, int len)
{
    if(commandmillis < 0) return false;

    resetcomplete();
    int cmdlen = (int)strlen(commandbuf), cmdspace = int(sizeof(commandbuf)) - (cmdlen+1);
    len = min(len, cmdspace);
    if(commandpos<0)
    {
        memcpy(&commandbuf[cmdlen], str, len);
    }
    else
    {
        memmove(&commandbuf[commandpos+len], &commandbuf[commandpos], cmdlen - commandpos);
        memcpy(&commandbuf[commandpos], str, len);
        commandpos += len;
    }
    commandbuf[cmdlen + len] = '\0';

    return true;
}

bool consolekey(int code, bool isdown)
{
    if(commandmillis < 0) return false;

    #ifdef __APPLE__
        #define MOD_KEYS (KMOD_LGUI|KMOD_RGUI)
    #else
        #define MOD_KEYS (KMOD_LCTRL|KMOD_RCTRL)
    #endif

    if(isdown)
    {
        switch(code)
        {
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                break;

            case SDLK_HOME:
                if(strlen(commandbuf)) commandpos = 0;
                break;

            case SDLK_END:
                commandpos = -1;
                break;

            case SDLK_DELETE:
            {
                int len = (int)strlen(commandbuf);
                if(commandpos<0) break;
                memmove(&commandbuf[commandpos], &commandbuf[commandpos+1], len - commandpos);
                resetcomplete();
                if(commandpos>=len-1) commandpos = -1;
                break;
            }

            case SDLK_BACKSPACE:
            {
                int len = (int)strlen(commandbuf), i = commandpos>=0 ? commandpos : len;
                if(i<1) break;
                memmove(&commandbuf[i-1], &commandbuf[i], len - i + 1);
                resetcomplete();
                if(commandpos>0) commandpos--;
                else if(!commandpos && len<=1) commandpos = -1;
                break;
            }

            case SDLK_LEFT:
                if(commandpos>0) commandpos--;
                else if(commandpos<0) commandpos = (int)strlen(commandbuf)-1;
                break;

            case SDLK_RIGHT:
                if(commandpos>=0 && ++commandpos>=(int)strlen(commandbuf)) commandpos = -1;
                break;

            case SDLK_UP:
                if(game::completeplayername(commandbuf, sizeof(commandbuf), -1, false))
                {
                    commandpos = -1;
                    break;
                }
                if(histpos > history.length()) histpos = history.length();
                if(histpos > 0) history[--histpos]->restore(); 
                break;

            case SDLK_DOWN:
                if(game::completeplayername(commandbuf, sizeof(commandbuf), 1, false))
                {
                    commandpos = -1;
                    break;
                }
                if(histpos + 1 < history.length()) history[++histpos]->restore();
                break;

            case SDLK_TAB:
                if(commandflags&CF_COMPLETE)
                {
                    if(!game::completeplayername(commandbuf, sizeof(commandbuf), 1, true))
                        complete(commandbuf, sizeof(commandbuf), commandflags&CF_EXECUTE ? "/" : NULL);
                    if(commandpos>=0 && commandpos>=(int)strlen(commandbuf)) commandpos = -1;
                }
                break;

            case SDLK_v:
                if(SDL_GetModState()&MOD_KEYS) pasteconsole();
                break;
        }
    }
    else
    {
        if(code==SDLK_RETURN || code==SDLK_KP_ENTER)
        {
            hline *h = NULL;
            if(commandbuf[0])
            {
                if(history.empty() || history.last()->shouldsave())
                {
                    if(maxhistory && history.length() >= maxhistory)
                    {
                        loopi(history.length()-maxhistory+1) delete history[i];
                        history.remove(0, history.length()-maxhistory+1);
                    }
                    history.add(h = new hline)->save();
                }
                else h = history.last();
            }
            histpos = history.length();
            inputcommand(NULL);
            if(h) h->run();
        }
        else if(code==SDLK_ESCAPE)
        {
            histpos = history.length();
            inputcommand(NULL);
        }
    }

    return true;
}

void processtextinput(const char *str, int len)
{
    if(!g3d_input(str, len))
        consoleinput(str, len);
}

void processkey(int code, bool isdown, int modstate)
{
    switch(code)
    {
        case SDLK_LGUI: case SDLK_RGUI:
            return;
    }
    keym *haskey = keyms.access(code);
    if(haskey && haskey->pressed) execbind(*haskey, isdown); // allow pressed keys to release
    else if(!g3d_key(code, isdown)) // 3D GUI mouse button intercept   
    {
        if(overlaykey(code, isdown)) {}
        else if(game::demobarkey(code, isdown)) {}
        else if(chatkey(code, isdown)) {}
        else if(game::killlogkey(code, isdown)) {}
        else if(!consolekey(code, isdown))
        {
            if(modstate&KMOD_GUI) return;
            if(haskey) execbind(*haskey, isdown);
        }
    }
}

void clear_console()
{
    keyms.clear();
}

void writebinds(stream *f)
{
    static const char * const cmds[3] = { "bind", "specbind", "editbind" };
    vector<keym *> binds;
    enumerate(keyms, keym, km, binds.add(&km));
    binds.sortname();
    loopj(3)
    {
        loopv(binds)
        {
            keym &km = *binds[i];
            if(*km.actions[j]) 
            {
                if(validateblock(km.actions[j])) f->printf("%s %s [%s]\n", cmds[j], escapestring(km.name), km.actions[j]);
                else f->printf("%s %s %s\n", cmds[j], escapestring(km.name), escapestring(km.actions[j]));
            }
        }
    }
}

// tab-completion of all idents and base maps

enum { FILES_DIR = 0, FILES_VAR, FILES_LIST };

struct fileskey
{
    int type;
    const char *dir, *ext;

    fileskey() {}
    fileskey(int type, const char *dir, const char *ext) : type(type), dir(dir), ext(ext) {}
};

static void cleanfilesdir(char *dir)
{
    int dirlen = (int)strlen(dir);
    while(dirlen > 0 && (dir[dirlen-1] == '/' || dir[dirlen-1] == '\\'))
        dir[--dirlen] = '\0';
}

struct filesval
{
    int type;
    char *dir, *ext;
    vector<char *> files;
    int millis;
    
    filesval(int type, const char *dir, const char *ext) : type(type), dir(newstring(dir)), ext(ext && ext[0] ? newstring(ext) : NULL), millis(-1) {}
    ~filesval() { DELETEA(dir); DELETEA(ext); files.deletearrays(); }

    void update()
    {
        if((type!=FILES_DIR && type!=FILES_VAR) || millis >= commandmillis) return;
        files.deletearrays();        
        if(type==FILES_VAR)
        {
            string buf;
            buf[0] = '\0';
            if(ident *id = readident(dir)) switch(id->type)
            {
                case ID_SVAR: copystring(buf, *id->storage.s); break;
                case ID_ALIAS: copystring(buf, id->getstr()); break;
            }
            if(!buf[0]) copystring(buf, ".");
            cleanfilesdir(buf);
            listfiles(buf, ext, files);
        }
        else listfiles(dir, ext, files);
        files.sort();
        loopv(files) if(i && !strcmp(files[i], files[i-1])) delete[] files.remove(i--);
        millis = totalmillis;
    }
};

static inline bool htcmp(const fileskey &x, const fileskey &y)
{
    return x.type==y.type && !strcmp(x.dir, y.dir) && (x.ext == y.ext || (x.ext && y.ext && !strcmp(x.ext, y.ext)));
}

static inline uint hthash(const fileskey &k)
{
    return hthash(k.dir);
}

static hashtable<fileskey, filesval *> completefiles;
static hashtable<char *, filesval *> completions;

int completesize = 0;
char *lastcomplete = NULL;

void resetcomplete() { completesize = 0; game::resetplayercomplete(); }

void addcomplete(char *command, int type, char *dir, char *ext)
{
    if(identflags&IDF_OVERRIDDEN)
    {
        conoutf(CON_ERROR, "cannot override complete %s", command);
        return;
    }
    if(!dir[0])
    {
        filesval **hasfiles = completions.access(command);
        if(hasfiles) *hasfiles = NULL;
        return;
    }
    if(type==FILES_DIR) cleanfilesdir(dir);
    if(ext)
    {
        if(strchr(ext, '*')) ext[0] = '\0';
        if(!ext[0]) ext = NULL;
    }
    fileskey key(type, dir, ext);
    filesval **val = completefiles.access(key);
    if(!val)
    {
        filesval *f = new filesval(type, dir, ext);
        if(type==FILES_LIST) explodelist(dir, f->files); 
        val = &completefiles[fileskey(type, f->dir, f->ext)];
        *val = f;
    }
    filesval **hasfiles = completions.access(command);
    if(hasfiles) *hasfiles = *val;
    else completions[newstring(command)] = *val;
}

void addfilecomplete(char *command, char *dir, char *ext)
{
    addcomplete(command, FILES_DIR, dir, ext);
}

void addvarcomplete(char *command, char *var, char *ext)
{
    addcomplete(command, FILES_VAR, var, ext);
}

void addlistcomplete(char *command, char *list)
{
    addcomplete(command, FILES_LIST, list, NULL);
}

COMMANDN(complete, addfilecomplete, "sss");
COMMANDN(varcomplete, addvarcomplete, "sss");
COMMANDN(listcomplete, addlistcomplete, "ss");

void complete(char *s, int maxlen, const char *cmdprefix)
{
    int cmdlen = 0;
    if(cmdprefix)
    {
        cmdlen = strlen(cmdprefix);
        if(strncmp(s, cmdprefix, cmdlen)) prependstring(s, cmdprefix, maxlen);
    }
    if(!s[cmdlen]) return;
    if(!completesize) { completesize = (int)strlen(&s[cmdlen]); DELETEA(lastcomplete); }

    filesval *f = NULL;
    if(completesize)
    {
        char *end = strchr(&s[cmdlen], ' ');
        if(end) f = completions.find(stringslice(&s[cmdlen], end), NULL);
    }

    const char *nextcomplete = NULL;
    if(f) // complete using filenames
    {
        int commandsize = strchr(&s[cmdlen], ' ')+1-s;
        f->update();
        loopv(f->files)
        {
            if(strncmp(f->files[i], &s[commandsize], completesize+cmdlen-commandsize)==0 &&
               (!lastcomplete || strcmp(f->files[i], lastcomplete) > 0) && (!nextcomplete || strcmp(f->files[i], nextcomplete) < 0))
                nextcomplete = f->files[i];
        }
        cmdprefix = s;
        cmdlen = commandsize;
    }
    else // complete using command names
    {
        enumerate(idents, ident, id,
            if(strncmp(id.name, &s[cmdlen], completesize)==0 &&
               (!lastcomplete || strcmp(id.name, lastcomplete) > 0) && (!nextcomplete || strcmp(id.name, nextcomplete) < 0))
                nextcomplete = id.name;
        );
    }
    DELETEA(lastcomplete);
    if(nextcomplete)
    {
        cmdlen = min(cmdlen, maxlen-1);
        if(cmdlen) memmove(s, cmdprefix, cmdlen);
        copystring(&s[cmdlen], nextcomplete, maxlen-cmdlen);
        lastcomplete = newstring(nextcomplete);
    }
}

void writecompletions(stream *f)
{
    vector<char *> cmds;
    enumeratekt(completions, char *, k, filesval *, v, { if(v) cmds.add(k); });
    cmds.sort();
    loopv(cmds)
    {
        char *k = cmds[i];
        filesval *v = completions[k];
        if(v->type==FILES_LIST) 
        {
            if(validateblock(v->dir)) f->printf("listcomplete %s [%s]\n", escapeid(k), v->dir);
            else f->printf("listcomplete %s %s\n", escapeid(k), escapestring(v->dir));
        }
        else f->printf("%s %s %s %s\n", v->type==FILES_VAR ? "varcomplete" : "complete", escapeid(k), escapestring(v->dir), escapestring(v->ext ? v->ext : "*"));
    }
}

