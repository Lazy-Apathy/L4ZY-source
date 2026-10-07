// menus.cpp: ingame menu system (also used for scores and serverlist)

#include "engine.h"

#ifdef WIN32
#include <shellapi.h>
#endif

#define GUI_TITLE_COLOR  0xFF7A18
#define GUI_BUTTON_COLOR 0xE4E6E8
#define GUI_TEXT_COLOR   0x8A9199

static vec menupos;
static int menustart = 0;
static g3d_gui *cgui = NULL;

VAR(guitabnum, 1, 0, 0);

struct menu : g3d_callback
{
    char *name, *header;
    uint *contents, *init, *onclear;
    bool showtab, keeptab;
    int menutab, scroll, scrollmax;

    menu() : name(NULL), header(NULL), contents(NULL), init(NULL), onclear(NULL), showtab(true), keeptab(false), menutab(1), scroll(0), scrollmax(0) {}

    void gui(g3d_gui &g, bool firstpass)
    {
        cgui = &g;
        guitabnum = menutab;
        cgui->start(menustart, 0.034f, showtab ? &menutab : NULL);
        if(showtab) cgui->tab(header ? header : name, GUI_TITLE_COLOR);
        execute(contents);
        cgui->end();
        cgui = NULL;
        guitabnum = 0;
    }

    virtual void clear() 
    {
        if(onclear) { freecode(onclear); onclear = NULL; }
    }
};

struct delayedupdate
{
    enum
    {
        INT,
        FLOAT,
        STRING,
        ACTION
    } type;
    ident *id;
    union
    {
        int i;
        float f;
        char *s;
    } val;
    delayedupdate() : type(ACTION), id(NULL) { val.s = NULL; }
    ~delayedupdate() { if(type == STRING || type == ACTION) DELETEA(val.s); }

    void schedule(const char *s) { type = ACTION; val.s = newstring(s); }
    void schedule(ident *var, int i) { type = INT; id = var; val.i = i; }
    void schedule(ident *var, float f) { type = FLOAT; id = var; val.f = f; }
    void schedule(ident *var, char *s) { type = STRING; id = var; val.s = newstring(s); }

    int getint() const
    {
        switch(type)
        {
            case INT: return val.i;
            case FLOAT: return int(val.f);
            case STRING: return int(strtol(val.s, NULL, 0));
            default: return 0;
        }
    }

    float getfloat() const
    {
        switch(type)
        {
            case INT: return float(val.i);
            case FLOAT: return val.f;
            case STRING: return float(parsefloat(val.s));
            default: return 0;
        }
    }
   
    const char *getstring() const
    {
        switch(type)
        {
            case INT: return intstr(val.i);
            case FLOAT: return intstr(int(floor(val.f)));
            case STRING: return val.s;
            default: return "";
        }
    }

    void run()
    {
        if(type == ACTION) { if(val.s) execute(val.s); }
        else if(id) switch(id->type)
        {
            case ID_VAR: setvarchecked(id, getint()); break;
            case ID_FVAR: setfvarchecked(id, getfloat()); break;
            case ID_SVAR: setsvarchecked(id, getstring()); break;
            case ID_ALIAS: alias(id->name, getstring()); break;
        }
    }
};
     
static hashnameset<menu> guis;
static vector<menu *> guistack;
static vector<delayedupdate> updatelater;
static bool shouldclearmenu = true, clearlater = false;

VARP(menudistance,  16, 40,  256);
VARP(menuautoclose, 32, 120, 4096);

vec menuinfrontofplayer()
{ 
    vec dir;
    vecfromyawpitch(camera1->yaw, 0, 1, 0, dir);
    dir.mul(menudistance).add(camera1->o);
    dir.z -= player->eyeheight-1;
    return dir;
}

void popgui()
{
    menu *m = guistack.pop();
    m->clear();
}

void removegui(menu *m)
{
    loopv(guistack) if(guistack[i]==m)
    {
        guistack.remove(i);
        m->clear();
        return;
    }
}    

void pushgui(menu *m, int pos = -1)
{
    if(guistack.empty())
    {
        menupos = menuinfrontofplayer();
        g3d_resetcursor();
    }
    if(pos < 0)
    {
        m->scroll = 0;
        guistack.add(m);
    }
    else guistack.insert(pos, m);
    if(pos < 0 || pos==guistack.length()-1)
    {
        if(!m->keeptab) m->menutab = 1;
        menustart = totalmillis;
    }
    if(m->init) execute(m->init);
}

void restoregui(int pos)
{
    int clear = guistack.length()-pos-1;
    loopi(clear) popgui();
    menustart = totalmillis;
}

void showgui(const char *name)
{
    menu *m = guis.access(name);
    if(!m) return;
    int pos = guistack.find(m);
    if(pos<0) pushgui(m);
    else restoregui(pos);
}

bool gui2dvisible()
{
    return guistack.length() > 0;
}

void hidegui(const char *name)
{
    menu *m = guis.access(name);
    if(m) removegui(m);
}
 
int cleargui(int n)
{
    int clear = guistack.length();
    if(mainmenu && !isconnected(true) && clear > 0 && guistack[0]->name && !strcmp(guistack[0]->name, "main")) 
    {
        clear--;
        if(!clear) return 1;
    }
    if(n>0) clear = min(clear, n);
    loopi(clear) popgui(); 
    if(!guistack.empty()) restoregui(guistack.length()-1);
    return clear;
}

void clearguis(int level = -1)
{
    if(level < 0) level = guistack.length();
    loopvrev(guistack)
    {
       menu *m = guistack[i];
       if(m->onclear)
       {
           uint *action = m->onclear;
           m->onclear = NULL;
           execute(action);
           freecode(action);
       }
    }
    cleargui(level);
}

// the HUD editor needs the whole screen: every menu goes, the main one too
void closeallguis()
{
    clearguis();
    while(guistack.length()) popgui();
}

void guionclear(char *action)
{
    if(guistack.empty()) return;
    menu *m = guistack.last();
    if(m->onclear) { freecode(m->onclear); m->onclear = NULL; } 
    if(action[0]) m->onclear = compilecode(action);
}

void guistayopen(uint *contents)
{
    bool oldclearmenu = shouldclearmenu;
    shouldclearmenu = false;
    execute(contents);
    shouldclearmenu = oldclearmenu;
}

void guinoautotab(uint *contents)
{
    if(!cgui) return;
    bool oldval = cgui->allowautotab(false);
    execute(contents);
    cgui->allowautotab(oldval);
}

void guimerge(uint *contents)
{
    if(!cgui) return;
    bool oldval = cgui->mergehits(true);
    execute(contents);
    cgui->mergehits(oldval);
}

//@DOC name and icon are optional
void guibutton(char *name, char *action, char *icon)
{
    if(!cgui) return;
    bool hideicon = !icon[0] || !strcmp(icon, "0");
    int ret = cgui->button(name, GUI_BUTTON_COLOR, hideicon ? NULL : icon);
    if(ret&G3D_UP) 
    {
        updatelater.add().schedule(action[0] ? action : name);
        if(shouldclearmenu) clearlater = true;
    }
    else if(ret&G3D_ROLLOVER)
    {
        alias("guirollovername", name);
        alias("guirolloveraction", action);
    }
}

void guiimage(char *path, char *action, float *scale, int *overlaid, char *alt, char *title)
{
    if(!cgui) return;
    Texture *t = textureload(path, 0, true, false);
    if(t==notexture)
    {
        if(alt[0]) t = textureload(alt, 0, true, false);
        if(t==notexture) return;
    }
    int ret = cgui->image(t, *scale, *overlaid!=0 ? title : NULL);
    if(ret&G3D_UP)
    {
        if(*action)
        {
            updatelater.add().schedule(action);
            if(shouldclearmenu) clearlater = true;
        }
    }
    else if(ret&G3D_ROLLOVER)
    {
        alias("guirolloverimgpath", path);
        alias("guirolloverimgaction", action);
    }
}

void guicolor(int *color)
{
    if(cgui) 
    {   
        defformatstring(desc, "0x%06X", *color);
        cgui->text(desc, *color, NULL);
    }
}

void guitextbox(char *text, int *width, int *height, int *color)
{
    if(cgui && text[0]) cgui->textbox(text, *width ? *width : 12, *height ? *height : 1, *color ? *color : 0xFFFFFF);
}

void guitext(char *name, char *icon)
{
    bool hideicon = !icon[0] || !strcmp(icon, "0");
    if(cgui) cgui->text(name, hideicon ? GUI_TEXT_COLOR : GUI_BUTTON_COLOR, hideicon ? NULL : icon);
}

void guititle(char *name)
{
    if(cgui) cgui->title(name, GUI_TITLE_COLOR);
}

void guitab(char *name)
{
    if(cgui) cgui->tab(name, GUI_TITLE_COLOR);
}

void guibar()
{
    if(cgui) cgui->separator();
}

void guibackdrop(char *path)
{
    g3d_setbackdrop(path);
}


void guistrut(float *strut, int *alt)
{
    if(cgui)
    {
        if(*alt) cgui->strut(*strut); else cgui->space(*strut);
    }
}

void guispring(int *weight)
{
    if(cgui) cgui->spring(max(*weight, 1));
}

void guicolumn(int *col)
{
    if(cgui) cgui->column(*col);
}

void guibanner(int *on)
{
    if(cgui) cgui->setbanner(*on!=0);
}

void guibannersplit()
{
    if(cgui) cgui->bannersplit();
}

void guibannercenter(int *on)
{
    if(cgui) cgui->setbannercenter(*on!=0);
}

void guifloat(float *u, float *v, uint *contents)
{
    if(!cgui) return;
    cgui->startfloat(*u, *v);
    execute(contents);
    cgui->endfloat();
}


template<class T> static void updateval(char *var, T val, char *onchange)
{
    ident *id = writeident(var);
    updatelater.add().schedule(id, val);
    if(onchange[0]) updatelater.add().schedule(onchange);
}

static int getval(char *var)
{
    ident *id = readident(var);
    if(!id) return 0;
    switch(id->type)
    {
        case ID_VAR: return *id->storage.i;
        case ID_FVAR: return int(*id->storage.f);
        case ID_SVAR: return parseint(*id->storage.s);
        case ID_ALIAS: return id->getint();
        default: return 0;
    }
}

void guicolorswatch(char *var, int *idx)
{
    if(!cgui) return;
    int i = *idx < 0 ? -1 : clamp(*idx, 0, 9);
    bool selected = getval(var) == i;
    if(cgui->colorswatch(i, selected)&G3D_UP) updateval(var, i, (char *)"");
}

static float getfval(const char *var)
{
    ident *id = readident(var);
    if(!id) return 0;
    switch(id->type)
    {
        case ID_VAR: return *id->storage.i;
        case ID_FVAR: return *id->storage.f;
        case ID_SVAR: return parsefloat(*id->storage.s);
        case ID_ALIAS: return id->getfloat();
        default: return 0;
    }
}

static const char *getsval(char *var)
{
    ident *id = readident(var);
    if(!id) return "";
    switch(id->type)
    {
        case ID_VAR: return intstr(*id->storage.i);
        case ID_FVAR: return floatstr(*id->storage.f);
        case ID_SVAR: return *id->storage.s;
        case ID_ALIAS: return id->getstr();
        default: return "";
    }
}

void guislider(char *var, int *min, int *max, char *onchange)
{
	if(!cgui) return;
    int oldval = getval(var), val = oldval, vmin = *max > INT_MIN ? *min : getvarmin(var), vmax = *max > INT_MIN ? *max : getvarmax(var);
    cgui->slider(val, vmin, vmax, GUI_TITLE_COLOR);
    if(val != oldval) updateval(var, val, onchange);
}

void guilistslider(char *var, char *list, char *onchange)
{
    if(!cgui) return;
    vector<int> vals;
    list += strspn(list, "\n\t ");
    while(*list)
    {
        vals.add(parseint(list));
        list += strcspn(list, "\n\t \0");
        list += strspn(list, "\n\t ");
    }
    if(vals.empty()) return;
    int val = getval(var), oldoffset = vals.length()-1, offset = oldoffset;
    loopv(vals) if(val <= vals[i]) { oldoffset = offset = i; break; }
    cgui->slider(offset, 0, vals.length()-1, GUI_TITLE_COLOR, intstr(val));
    if(offset != oldoffset) updateval(var, vals[offset], onchange);
}

void guinameslider(char *var, char *names, char *list, char *onchange)
{
    if(!cgui) return;
    vector<int> vals;
    list += strspn(list, "\n\t ");
    while(*list)
    {
        vals.add(parseint(list));
        list += strcspn(list, "\n\t \0");
        list += strspn(list, "\n\t ");
    }
    if(vals.empty()) return;
    int val = getval(var), oldoffset = vals.length()-1, offset = oldoffset;
    loopv(vals) if(val <= vals[i]) { oldoffset = offset = i; break; }
    char *label = indexlist(names, offset);
    cgui->slider(offset, 0, vals.length()-1, GUI_TITLE_COLOR, label);
    if(offset != oldoffset) updateval(var, vals[offset], onchange);
    delete[] label;
}

// A value a setting cannot take on this machine right now (e.g. hwrt 1 on a GPU
// without ray tracing): "settinglock VAR VALUE [REASON]". REASON is run each time
// it is needed and gives a short line, "" meaning the value is allowed again;
// an empty REASON removes the lock. The guiradio / guicheckbox that would set
// the value stays on screen but greyed out and inert, the settings search shows
// it the same way, and the assistant refuses it, quoting the reason.
struct settinglockentry
{
    string var;
    float val;
    char *reason;
};
static vector<settinglockentry> settinglocks;

static void settinglock(char *var, float *val, char *reason)
{
    loopv(settinglocks) if(settinglocks[i].val == *val && !strcmp(settinglocks[i].var, var))
    {
        delete[] settinglocks[i].reason;
        if(reason[0]) settinglocks[i].reason = newstring(reason);
        else settinglocks.remove(i);
        return;
    }
    if(!reason[0] || !var[0]) return;
    settinglockentry &l = settinglocks.add();
    copystring(l.var, var);
    l.val = *val;
    l.reason = newstring(reason);
}
COMMAND(settinglock, "sfs");

// why VAR cannot be set to VAL now, or NULL when it can
static const char *settinglocked(const char *var, float val)
{
    loopv(settinglocks) if(settinglocks[i].val == val && !strcmp(settinglocks[i].var, var))
    {
        static string why;
        char *r = executestr(settinglocks[i].reason);
        copystring(why, r ? r : "");
        DELETEA(r);
        if(why[0]) return why;
    }
    return NULL;
}
ICOMMAND(settinglockreason, "sf", (char *var, float *val), { const char *why = settinglocked(var, *val); result(why ? why : ""); });

void guicheckbox(char *name, char *var, float *on, float *off, char *onchange)
{
    if(!cgui) return;
    bool enabled = getfval(var)!=*off;
    float onval = *on || *off ? *on : 1.0f;
    // switching off is always allowed; only turning on can be locked
    if(!enabled && settinglocked(var, onval)) { cgui->disabledbutton(name, "checkbox_off"); return; }
    if(cgui->button(name, GUI_BUTTON_COLOR, enabled ? "checkbox_on" : "checkbox_off")&G3D_UP)
    {
        updateval(var, enabled ? *off : onval, onchange);
    }
}

void guiradio(char *name, char *var, float *n, char *onchange)
{
    if(!cgui) return;
    bool enabled = getfval(var)==*n;
    if(settinglocked(var, *n)) { cgui->disabledbutton(name, enabled ? "radio_on" : "radio_off"); return; }
    if(cgui->button(name, GUI_BUTTON_COLOR, enabled ? "radio_on" : "radio_off")&G3D_UP)
    {
        if(!enabled) updateval(var, *n, onchange);
    }
}

void guibitfield(char *name, char *var, int *mask, char *onchange)
{
    int val = getval(var);
    bool enabled = (val & *mask) != 0;
    if(cgui && cgui->button(name, GUI_BUTTON_COLOR, enabled ? "checkbox_on" : "checkbox_off")&G3D_UP)
    {
        updateval(var, enabled ? val & ~*mask : val | *mask, onchange);
    }
}

//-ve length indicates a wrapped text field of any (approx 260 chars) length, |length| is the field width
void guifield(char *var, int *maxlength, char *onchange)
{   
    if(!cgui) return;
    const char *initval = getsval(var);
	char *result = cgui->field(var, GUI_BUTTON_COLOR, *maxlength ? *maxlength : 12, 0, initval);
    if(result) updateval(var, result, onchange); 
}

// a framed, wrapped text field (at least two lines high), for typing sentences
extern bool guifieldboxed;
void guiinputfield(char *var, int *width, char *onchange)
{
    if(!cgui) return;
    const char *initval = getsval(var);
    guifieldboxed = true;
    char *result = cgui->field(var, GUI_BUTTON_COLOR, -max(abs(*width), 10), 0, initval);
    guifieldboxed = false;
    if(result) updateval(var, result, onchange);
}
COMMAND(guiinputfield, "sis");

//-ve maxlength indicates a wrapped text field of any (approx 260 chars) length, |maxlength| is the field width
void guieditor(char *name, int *maxlength, int *height, int *mode)
{
    if(!cgui) return;
    cgui->field(name, GUI_BUTTON_COLOR, *maxlength ? *maxlength : 12, *height, NULL, *mode<=0 ? EDITORFOREVER : *mode);
    //returns a non-NULL pointer (the currentline) when the user commits, could then manipulate via text* commands
}

//-ve length indicates a wrapped text field of any (approx 260 chars) length, |length| is the field width
void guikeyfield(char *var, int *maxlength, char *onchange)
{
    if(!cgui) return;
    const char *initval = getsval(var);
    char *result = cgui->keyfield(var, GUI_BUTTON_COLOR, *maxlength ? *maxlength : -8, 0, initval);
    if(result) updateval(var, result, onchange);
}

//use text<action> to do more...


void guilist(uint *contents)
{
    if(!cgui) return;
    cgui->pushlist();
    execute(contents);
    cgui->poplist();
}

void guialign(int *align, uint *contents)
{
    if(!cgui) return;
    cgui->pushlist();
    if(*align >= 0) cgui->spring();
    execute(contents);
    if(*align == 0) cgui->spring(); 
    cgui->poplist();
}

void newgui(char *name, char *contents, char *header, char *init)
{
    menu *m = guis.access(name);
    if(!m)
    {
        name = newstring(name);
        m = &guis[name];
        m->name = name;
    }
    else
    {
        DELETEA(m->header);
        freecode(m->contents);
        freecode(m->init);
    }
    if(header && header[0])
    {
        char *end = NULL;
        int val = strtol(header, &end, 0);
        if(end && !*end)
        {
            m->header = NULL;
            m->showtab = val != 0;
        }
        else
        {
            m->header = newstring(header);
            m->showtab = true;
        }
    }
    else
    {
        m->header = NULL;
        m->showtab = true;
    }
    m->contents = compilecode(contents);
    m->init = init && init[0] ? compilecode(init) : NULL;
}

menu *guiserversmenu = NULL;

void guiservers(uint *header, int *pagemin, int *pagemax)
{
    extern const char *showservers(g3d_gui *cgui, uint *header, int pagemin, int pagemax);
    if(cgui) 
    {
        const char *command = showservers(cgui, header, *pagemin, *pagemax > 0 ? *pagemax : INT_MAX);
        if(command)
        {
            updatelater.add().schedule(command);
            if(shouldclearmenu) clearlater = true;
            guiserversmenu = clearlater || guistack.empty() ? NULL : guistack.last();
        }
    }
}

void notifywelcome()
{
    if(guiserversmenu)
    {
        if(guistack.length() && guistack.last() == guiserversmenu) clearguis();
        guiserversmenu = NULL;
    }
}
 
COMMAND(newgui, "ssss");
COMMAND(guibutton, "sss");
COMMAND(guitext, "ss");
COMMAND(guiservers, "eii");
ICOMMAND(cleargui, "i", (int *n), intret(cleargui(*n)));
COMMAND(showgui, "s");
COMMAND(hidegui, "s");
COMMAND(guionclear, "s");
COMMAND(guistayopen, "e");
COMMAND(guinoautotab, "e");
COMMAND(guimerge, "e");
ICOMMAND(guikeeptab, "b", (int *keeptab), if(guistack.length()) guistack.last()->keeptab = *keeptab!=0);
COMMAND(guilist, "e");
COMMAND(guialign, "ie");
COMMAND(guititle, "s");
COMMAND(guibar,"");
COMMAND(guistrut,"fi");
COMMAND(guispring, "i");
COMMAND(guicolumn, "i");
COMMAND(guibanner, "i");
COMMAND(guibannersplit, "");
COMMAND(guibannercenter, "i");
COMMAND(guifloat, "ffe");
COMMAND(guicolorswatch, "si");
COMMAND(guiimage,"ssfiss");
COMMAND(guislider,"sbbs");
COMMAND(guilistslider, "sss");
COMMAND(guinameslider, "ssss");
COMMAND(guiradio,"ssfs");
COMMAND(guibitfield, "ssis");
COMMAND(guicheckbox, "ssffs");
COMMAND(guitab, "s");
COMMAND(guibackdrop, "s");
COMMAND(guifield, "sis");
COMMAND(guikeyfield, "sis");
COMMAND(guieditor, "siii");
COMMAND(guicolor, "i");
COMMAND(guitextbox, "siii");

void guiplayerpreview(int *model, int *team, int *weap, char *action, float *scale, int *overlaid, char *title)
{
    if(!cgui) return;
    int ret = cgui->playerpreview(*model, *team, *weap, *scale, *overlaid!=0 ? title : NULL);
    if(ret&G3D_UP)
    {
        if(*action)
        {
            updatelater.add().schedule(action);
            if(shouldclearmenu) clearlater = true;
        }
    }
}
void guiplayeroverlay(int *model, int *team, int *weap, char *action, float *scale)
{
    if(!cgui) return;
    int ret = cgui->playeroverlay(*model, *team, *weap, *scale);
    if(ret&G3D_UP)
    {
        if(*action)
        {
            updatelater.add().schedule(action);
            if(shouldclearmenu) clearlater = true;
        }
    }
}
COMMAND(guiplayerpreview, "iiisfis");
COMMAND(guiplayeroverlay, "iiisf");

void guimodelpreview(char *model, char *animspec, char *action, float *scale, int *overlaid, char *title, int *throttle)
{
    if(!cgui) return;
    int anim = ANIM_ALL;
    if(animspec[0])
    {
        if(isdigit(animspec[0])) 
        {
            anim = parseint(animspec);
            if(anim >= 0) anim %= ANIM_INDEX;
            else anim = ANIM_ALL;
        }
        else
        {
            vector<int> anims;
            findanims(animspec, anims);
            if(anims.length()) anim = anims[0];
        }
    }
    int ret = cgui->modelpreview(model, anim|ANIM_LOOP, *scale, *overlaid!=0 ? title : NULL, *throttle!=0);
    if(ret&G3D_UP)
    {
        if(*action)
        {
            updatelater.add().schedule(action);
            if(shouldclearmenu) clearlater = true;
        }
    }
    else if(ret&G3D_ROLLOVER)
    {
        alias("guirolloverpreviewname", model);
        alias("guirolloverpreviewaction", action);
    }
}
COMMAND(guimodelpreview, "sssfisi");

void guiprefabpreview(char *prefab, int *color, char *action, float *scale, int *overlaid, char *title, int *throttle)
{
    if(!cgui) return;
    int ret = cgui->prefabpreview(prefab, vec::hexcolor(*color), *scale, *overlaid!=0 ? title : NULL, *throttle!=0);
    if(ret&G3D_UP)
    {
        if(*action)
        {   
            updatelater.add().schedule(action);
            if(shouldclearmenu) clearlater = true;
        }
    }
    else if(ret&G3D_ROLLOVER)
    {
        alias("guirolloverpreviewname", prefab);
        alias("guirolloverpreviewaction", action);
    }
}
COMMAND(guiprefabpreview, "sisfisi");

static void folderstripsep(char *dir)
{
    size_t n = strlen(dir);
    while(n && (dir[n-1]=='/' || dir[n-1]=='\\')) dir[--n] = 0;
}

static void folderensure(char *dir)
{
    path(dir);
    folderstripsep(dir);
    for(char *p = dir; *p; p++)
    {
#ifdef WIN32
        if(p == dir+2 && dir[1]==':') continue;
#endif
        if((*p=='/' || *p=='\\') && p > dir)
        {
            char c = *p;
            *p = 0;
            if(dir[0] && !fileexists(dir, "d")) createdir(dir);
            *p = c;
        }
    }
    if(dir[0] && !fileexists(dir, "d")) createdir(dir);
}

static void foldergamebase(char *dst, int len)
{
    dst[0] = 0;
#ifdef WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, buf, MAX_PATH);
    if(!n || n >= MAX_PATH) return;
    path(buf);
    char *slash = strrchr(buf, PATHDIV);
    if(slash) *slash = 0;
    char *leaf = strrchr(buf, PATHDIV);
    const char *name = leaf ? leaf+1 : buf;
    if(!strcasecmp(name, "bin") || !strcasecmp(name, "bin64"))
    {
        if(leaf) *leaf = 0;
    }
    copystring(dst, buf, len);
#endif
}

static bool folderexplore(const char *dir)
{
    string abs;
    copystring(abs, dir);
    folderensure(abs);
#ifdef WIN32
    INT_PTR r = (INT_PTR)ShellExecuteA(NULL, "open", abs, NULL, NULL, SW_SHOWNORMAL);
    if(r <= 32)
    {
        conoutf(CON_ERROR, "could not open folder");
        return false;
    }
    conoutf("opened %s", abs);
    return true;
#else
    conoutf("folder: %s", abs);
    return false;
#endif
}

ICOMMAND(openfolder, "s", (char *which),
{
    string dir;
    dir[0] = 0;
    if(!which) return;
    if(!strcmp(which, "demo") || !strcmp(which, "demos"))
    {
        if(homedir[0]) formatstring(dir, "%sdemo", homedir);
        else copystring(dir, "demo");
    }
    else if(!strcmp(which, "record") || !strcmp(which, "recordings"))
    {
        if(homedir[0]) formatstring(dir, "%srecordings", homedir);
        else copystring(dir, "recordings");
    }
    else if(!strcmp(which, "playermodel"))
    {
        // the "Custom" player model folder, in the profile: created with a
        // short README the first time
        formatstring(dir, "%spackages/models/custom", homedir);
        static const char * const readme =
            "Custom player model (menu: Player Model > Custom model)\n"
            "\n"
            "Put your own model here, like the shipped ones in packages/models\n"
            "(mrfixit, snoutx10k...):\n"
            "  custom/md5.cfg (or iqm.cfg) + its files : normal look (needed)\n"
            "  custom/blue/md5.cfg                     : your team (optional)\n"
            "  custom/red/md5.cfg                      : other team (optional)\n"
            "Without blue/red, the normal look is used in team games.\n"
            "In blue/red cfg files, md5dir \"custom\" reuses the files of custom/.\n"
            "\n"
            "You see it on yourself, and on everyone with Force Matching Player\n"
            "Models. Other players still see the model you picked. No colour\n"
            "skin is put over it. Arms, icons, armour and quad come from the\n"
            "picked model. Empty or broken folder: the picked model is used.\n"
            "New files: load a map again. Changed files: restart the game.\n";
        folderensure(dir);
        string rm;
        formatstring(rm, "%s/README.txt", dir);
        if(!fileexists(rm, "r"))
        {
            // relative name: openrawfile() puts it in the profile itself
            stream *f = openrawfile("packages/models/custom/README.txt", "w");
            if(f) { f->write(readme, strlen(readme)); delete f; }
        }
    }
    else if(!strcmp(which, "models") || !strcmp(which, "custom"))
    {
        // L4ZY.exe donne le dossier personnel des modeles (hors installation).
        const char *mdir = getenv("L4ZY_MODELS_DIR");
        string base;
        foldergamebase(base, sizeof(base));
        if(mdir && mdir[0]) { formatstring(dir, "%s%ccustom", mdir, PATHDIV); createdir(dir); }
        else if(base[0]) formatstring(dir, "%s%ctraduction%cmodels%ccustom", base, PATHDIV, PATHDIV, PATHDIV);
        else copystring(dir, "traduction/models/custom");
    }
    else
    {
        conoutf(CON_WARN, "unknown folder");
        return;
    }
    folderexplore(dir);
});

struct change
{
    int type;
    const char *desc;

    change() {}
    change(int type, const char *desc) : type(type), desc(desc) {}
};
static vector<change> needsapply;

static struct applymenu : menu
{
    void gui(g3d_gui &g, bool firstpass)
    {
        if(guistack.empty()) return;
        g.start(menustart, 0.03f);
        g.text("the following settings have changed:", GUI_TEXT_COLOR, "info");
        loopv(needsapply) g.text(needsapply[i].desc, GUI_TEXT_COLOR, "info");
        g.separator();
        g.text("apply changes now?", GUI_TEXT_COLOR, "info");
        if(g.button("yes", GUI_BUTTON_COLOR, "action")&G3D_UP)
        {
            int changetypes = 0;
            loopv(needsapply) changetypes |= needsapply[i].type;
            if(changetypes&CHANGE_GFX) updatelater.add().schedule("resetgl");
            if(changetypes&CHANGE_SOUND) updatelater.add().schedule("resetsound");
            clearlater = true;
        }
        if(g.button("no", GUI_BUTTON_COLOR, "action")&G3D_UP)
            clearlater = true;
        g.end();
    }

    void clear()
    {
        menu::clear();
        needsapply.shrink(0);
    }
} applymenu;

VARP(applydialog, 0, 1, 1);

static bool processingmenu = false;

void addchange(const char *desc, int type)
{
    if(!applydialog) return;
    loopv(needsapply) if(!strcmp(needsapply[i].desc, desc)) return;
    needsapply.add(change(type, desc));
    if(needsapply.length() && guistack.find(&applymenu) < 0)
        pushgui(&applymenu, processingmenu ? max(guistack.length()-1, 0) : -1);
}

void clearchanges(int type)
{
    loopv(needsapply)
    {
        if(needsapply[i].type&type)
        {
            needsapply[i].type &= ~type;
            if(!needsapply[i].type) needsapply.remove(i--);
        }
    }
    if(needsapply.empty()) removegui(&applymenu);
}

void menuprocess()
{
    processingmenu = true;
    int wasmain = mainmenu, level = guistack.length();
    loopv(updatelater) updatelater[i].run();
    updatelater.shrink(0);
    if(wasmain > mainmenu || clearlater)
    {
        if(wasmain > mainmenu || level==guistack.length()) clearguis(level); 
        clearlater = false;
    }
    extern bool hudeditactive();
    if(mainmenu && !isconnected(true) && guistack.empty() && !hudeditactive()) showgui("main");
    processingmenu = false;
}

VAR(mainmenu, 1, 1, 0);

void clearmainmenu()
{
    if(mainmenu && isconnected())
    {
        mainmenu = 0;
        if(!processingmenu) cleargui();
    }
}

// scrolls the open menu to its end (clamped to the real height when laid out)
ICOMMAND(guiscrollbottom, "", (), { if(guistack.length()) guistack.last()->scroll = 1<<20; });

void g3d_mainmenu()
{
    if(!guistack.empty()) 
    {   
        extern int usegui2d;
        if(!mainmenu && !usegui2d && camera1->o.dist(menupos) > menuautoclose) cleargui();
        else
        {
            menu *m = guistack.last();
            g3d_bindscroll(&m->scroll, &m->scrollmax);
            g3d_addgui(m, menupos, GUI_2D | GUI_FOLLOW);
        }
    }
    else g3d_bindscroll(NULL, NULL);
}


// ---------------------------------------------------------------------------
// Settings search: indexes the controls of the settings pages straight from
// data/menus.cfg and traduction.cfg (so new options are found without upkeep),
// plus keywords / synonyms from data/settings-keywords.txt (French + English).

extern int unescapestring(char *dst, const char *src, const char *end);

struct settingentry
{
    string page, title, section, label, var, args, words;
    int kind; // 0 checkbox, 1 radio, 2 slider, 3 picker (args = its menu alias)
};
static vector<settingentry> settingindex;
static vector<char *> settingsyn;   // "word=syn syn syn"
static vector<char *> settingvarwords; // "var=words" (also for settings outside the menus)
static bool settingindexed = false;

// lowercase, accents folded (UTF-8 Latin-1 range), colour codes dropped,
// punctuation to spaces
static void settingnorm(const char *in, char *out, int len)
{
    int n = 0;
    for(const uchar *s = (const uchar *)in; *s && n < len-1; s++)
    {
        uchar c = *s;
        if(c == '\f' && s[1]) { s++; continue; }
        if(c == '^' && s[1] == 'f' && s[2]) { s += 2; continue; }
        if(c == 0xC3 && s[1])
        {
            uchar d = s[1] | 0x20; s++;
            if(d >= 0xA0 && d <= 0xA5) c = 'a';
            else if(d == 0xA7) c = 'c';
            else if(d >= 0xA8 && d <= 0xAB) c = 'e';
            else if(d >= 0xAC && d <= 0xAF) c = 'i';
            else if(d == 0xB1) c = 'n';
            else if(d >= 0xB2 && d <= 0xB6) c = 'o';
            else if(d >= 0xB9 && d <= 0xBC) c = 'u';
            else c = ' ';
        }
        else if(c >= 0x80) c = ' ';
        else if(isalnum(c)) c = tolower(c);
        else c = ' ';
        if(c == ' ' && (!n || out[n-1] == ' ')) continue;
        out[n++] = c;
    }
    while(n > 0 && out[n-1] == ' ') n--;
    out[n] = 0;
}

// next token of a cubescript line: "quoted" (unescaped) or bare word
static const char *settingtoken(const char *p, char *out, int len)
{
    while(*p == ' ' || *p == '\t') p++;
    out[0] = 0;
    if(!*p || *p == '\r' || *p == '\n' || (p[0] == '/' && p[1] == '/')) return NULL;
    if(*p == '"')
    {
        const char *e = ++p;
        while(*e && *e != '"' && *e != '\r' && *e != '\n') { if(*e == '^' && e[1]) e++; e++; }
        char tmp[MAXSTRLEN];
        int m = unescapestring(tmp, p, p + min(int(e - p), MAXSTRLEN-1));
        tmp[m] = 0;
        copystring(out, tmp, len);
        return *e == '"' ? e+1 : e;
    }
    const char *e = p;
    while(*e && *e != ' ' && *e != '\t' && *e != '\r' && *e != '\n') e++;
    int l = min(int(e - p), len-1);
    memcpy(out, p, l);
    out[l] = 0;
    return e;
}

static bool settingpageok(const char *page)
{
    static const char * const pages[] = { "hud", "scoreboard", "gfx", "display", "audio", "mouse", "keys", "console", "record",
        "ammobar", "gameclock", "hudscore", "killfeed", "killstreak", "traduction", "crosshair", "friends", "playermodel", "trails", "trailsme", "trailsothers", "trailsall" };
    loopi(sizeof(pages)/sizeof(pages[0])) if(!strcmp(page, pages[i])) return true;
    return false;
}

// how a player reaches each settings page, with the labels the menus show
// (the AI assistant repeats this; it used to invent "Settings > Crosshair")
static const char *settingroute(const char *page)
{
    static const char * const routes[][2] = {
        { "display", "Options > Display" }, { "gfx", "Options > Graphics" }, { "audio", "Options > Sound" },
        { "mouse", "Options > Mouse" }, { "keys", "Options > Keys" }, { "hud", "Options > HUD" },
        { "friends", "Options > Friends" }, { "traduction", "Options > Chat Translation" },
        { "console", "Options > Console" }, { "record", "Options > Recording" },
        { "crosshair", "Options > Mouse > \"crosshair:\" button" },
        { "scoreboard", "Options > HUD > Columns" },
        { "ammobar", "Options > HUD > Adjust (next to Ammo Bar)" },
        { "gameclock", "Options > HUD > Adjust (next to Game Clock)" },
        { "hudscore", "Options > HUD > Adjust (next to HUD Score)" },
        { "killfeed", "Options > HUD > Adjust (next to Kill Feed)" },
        { "killstreak", "Options > HUD > Adjust (next to Kill Streak)" },
        { "playermodel", "main menu: the colour dots under your player" },
        { "trails", "Options > HUD > Colours (next to Weapon Trails)" },
        { "trailsme", "Options > HUD > Colours (next to Weapon Trails) > Me" },
        { "trailsothers", "Options > HUD > Colours (next to Weapon Trails) > Others" },
        { "trailsall", "Options > HUD > Colours (next to Weapon Trails) > All" },
    };
    loopi(sizeof(routes)/sizeof(routes[0])) if(!strcmp(page, routes[i][0])) return routes[i][1];
    return NULL;
}

static void indexsettingsfile(const char *file)
{
    char *buf = loadfile(path(file, true), NULL);
    if(!buf) return;
    string page = "", title = "", section = "", text = "";
    for(char *line = buf; line && *line; )
    {
        char *next = strchr(line, '\n');
        if(next) *next++ = 0;
        char tok[MAXSTRLEN], a[MAXSTRLEN], b[MAXSTRLEN];
        const char *p = settingtoken(line, tok, sizeof(tok));
        if(p && line[0] != ' ' && line[0] != '\t' && !strcmp(tok, "newgui"))
        {
            settingtoken(p, a, sizeof(a));
            copystring(page, a);
            copystring(title, a);
            section[0] = text[0] = 0;
        }
        else if(p && page[0] && settingpageok(page))
        {
            // "guistayopen [ guiradio ... ]" on one line: look inside the brackets
            while(p && (!strcmp(tok, "guistayopen") || !strcmp(tok, "guilist")))
            {
                while(*p == ' ' || *p == '\t') p++;
                if(*p != '[') break;
                p = settingtoken(p + 1, tok, sizeof(tok));
            }
            if(!p) { line = next; continue; }
            if(!strcmp(tok, "guititle")) { if(settingtoken(p, a, sizeof(a))) copystring(title, a); }
            else if(!strcmp(tok, "guisection")) { if(settingtoken(p, a, sizeof(a))) copystring(section, a); text[0] = 0; }
            else if(!strcmp(tok, "guitext"))
            {
                // only a literal label; a computed one ((format ...)) is not a name
                while(*p == ' ' || *p == '\t') p++;
                if(*p == '"' && settingtoken(p, a, sizeof(a))) copystring(text, a);
                else text[0] = 0;
            }
            else if(!strcmp(tok, "guicheckbox") || !strcmp(tok, "guiradio") || !strcmp(tok, "guislider") || !strcmp(tok, "guilistslider"))
            {
                bool slider = !strcmp(tok, "guislider") || !strcmp(tok, "guilistslider");
                const char *q = p;
                a[0] = b[0] = 0;
                if(!slider) q = settingtoken(q, a, sizeof(a));
                if(q) q = settingtoken(q, b, sizeof(b));
                if(q && b[0] && isalpha((uchar)b[0]) && !strchr(b, '@') && !strchr(b, '$'))
                {
                    settingentry &e = settingindex.add();
                    e.kind = slider ? 2 : (!strcmp(tok, "guiradio") ? 1 : 0);
                    copystring(e.page, page);
                    copystring(e.title, title);
                    copystring(e.section, section);
                    const char *lab = slider ? text : a;
                    char nl[MAXSTRLEN];
                    settingnorm(lab, nl, sizeof(nl));
                    copystring(e.label, nl[0] && !isdigit((uchar)nl[0]) ? lab : b);
                    // "HUDScoreScale:^t^t" arrives with real tabs: drop them and the colon
                    {
                        int n = 0;
                        for(const char *c = e.label; *c; c++) if(*c != '\t') e.label[n++] = *c;
                        while(n > 0 && (e.label[n-1] == ' ' || e.label[n-1] == ':')) n--;
                        e.label[n] = 0;
                    }
                    copystring(e.var, b);
                    // extra args on the same line only, without a trailing [block]
                    int n = 0;
                    while(*q == ' ' || *q == '\t') q++;
                    for(; *q && *q != '\r' && *q != '[' && *q != ']' && *q != ';' && !(q[0] == '/' && q[1] == '/') && n < MAXSTRLEN-1; q++) e.args[n++] = *q;
                    while(n > 0 && (e.args[n-1] == ' ' || e.args[n-1] == '\t')) n--;
                    e.args[n] = 0;
                    e.words[0] = 0;
                }
            }
            // colour pickers are menu aliases, found anywhere on the line:
            // friendcolorpicks VAR "Label"
            static const char * const pickers[] = { "friendcolorpicks", "crosshaircolourpicks", "trailcolourpicks" };
            loopi(sizeof(pickers)/sizeof(pickers[0]))
            {
                const char *at = strstr(line, pickers[i]);
                size_t pl = strlen(pickers[i]);
                if(!at || (at[pl] != ' ' && at[pl] != '\t')) continue;
                const char *q = settingtoken(at + pl, b, sizeof(b));
                for(char *c = b; *c; c++) if(*c == ']') { *c = 0; break; }
                if(!q || !b[0] || !isalpha((uchar)b[0]) || strchr(b, '$')) continue;
                if(!settingtoken(q, a, sizeof(a)) || a[0] == ']') a[0] = 0;
                else for(char *c = a; *c; c++) if(*c == ']') { *c = 0; break; }
                settingentry &e = settingindex.add();
                e.kind = 3;
                copystring(e.page, page);
                copystring(e.title, title);
                copystring(e.section, section);
                copystring(e.label, a[0] ? a : b);
                copystring(e.var, b);
                copystring(e.args, pickers[i]);
                e.words[0] = 0;
            }
            // text fields: "guifield VAR N" edits VAR itself; "guifield ALIAS N [VAR $ALIAS]"
            // edits a copy that is written to VAR on Enter (mouse sensitivity, DPI...)
            const char *gf = strstr(line, "guifield");
            if(gf && (gf[8] == ' ' || gf[8] == '\t'))
            {
                char w[MAXSTRLEN], blk[MAXSTRLEN];
                const char *q = settingtoken(gf + 8, b, sizeof(b));
                if(q) q = settingtoken(q, w, sizeof(w));
                string realvar = "";
                if(q && b[0] && isalpha((uchar)b[0]) && !strchr(b, '$') && !strchr(b, '@') && (isdigit((uchar)w[0]) || w[0] == '-'))
                {
                    while(*q == ' ' || *q == '\t') q++;
                    if(*q == '[')
                    {
                        // [VAR $ALIAS]: the block names the real setting
                        const char *r = settingtoken(q + 1, blk, sizeof(blk));
                        defformatstring(want, "$%s]", b);
                        defformatstring(want2, "$%s", b);
                        if(r && settingtoken(r, a, sizeof(a)) && (!strcmp(a, want) || !strcmp(a, want2))) copystring(realvar, blk);
                    }
                    else copystring(realvar, b);
                }
                ident *id = realvar[0] ? getident(realvar) : NULL;
                // any real setting (scr_w lives in init.cfg, not config.cfg)
                if(id && id->type <= ID_SVAR && !(id->flags & (IDF_READONLY | IDF_OVERRIDE)))
                {
                    // label: a literal guitext on the same line, else the last one above
                    string lab = "";
                    const char *gt = strstr(line, "guitext \"");
                    if(gt && gt < gf) settingtoken(gt + 7, lab, sizeof(lab));
                    else copystring(lab, text);
                    // "AmmoBarX:^t^t^t" -> "AmmoBarX"
                    char clean[MAXSTRLEN];
                    int n = 0;
                    for(const char *c = lab; *c && n < MAXSTRLEN-1; c++)
                    {
                        if(*c == '^' && c[1]) { c++; continue; }
                        if(*c == '\t') continue;
                        clean[n++] = *c;
                    }
                    while(n > 0 && (clean[n-1] == ' ' || clean[n-1] == ':')) n--;
                    clean[n] = 0;
                    settingentry &e = settingindex.add();
                    e.kind = 4;
                    copystring(e.page, page);
                    copystring(e.title, title);
                    copystring(e.section, section);
                    copystring(e.label, clean[0] ? clean : realvar);
                    copystring(e.var, realvar);
                    // width, then the alias when the field edits a copy
                    if(strcmp(b, realvar)) formatstring(e.args, "%s %s", w, b);
                    else copystring(e.args, w);
                    e.words[0] = 0;
                }
            }
        }
        line = next;
    }
    delete[] buf;
}

static void loadsettingkeywords()
{
    char *buf = loadfile(path("data/settings-keywords.txt", true), NULL);
    if(!buf) return;
    for(char *line = buf; line && *line; )
    {
        char *next = strchr(line, '\n');
        if(next) *next++ = 0;
        char *colon = strchr(line, ':');
        if(colon && line[0] != '#')
        {
            *colon = 0;
            char key[MAXSTRLEN], words[MAXSTRLEN];
            settingnorm(line, key, sizeof(key));
            settingnorm(colon+1, words, sizeof(words));
            if(line[0] == '=') // synonyms of a query word: "=son: sound volume audio"
            {
                defformatstring(s, "%s=%s", key, words);
                settingsyn.add(newstring(s));
            }
            else
            {
                defformatstring(vw, "%s=%s", key, words);
                settingvarwords.add(newstring(vw));
            }
            if(line[0] != '=') loopv(settingindex) if(!strcasecmp(settingindex[i].var, key))
            {
                concatstring(settingindex[i].words, words);
                concatstring(settingindex[i].words, " ");
            }
        }
        line = next;
    }
    delete[] buf;
}

static void buildsettingindex()
{
    if(settingindexed) return;
    settingindexed = true;
    indexsettingsfile("data/menus.cfg");
    indexsettingsfile("traduction.cfg");
    loadsettingkeywords();
}

ICOMMAND(settingsearchreload, "", (),
{
    settingindex.setsize(0);
    settingsyn.deletearrays();
    settingvarwords.deletearrays();
    settingindexed = false;
    buildsettingindex();
    intret(settingindex.length());
});

// score of one (normalized) query word against an entry, 0 = no match
static int settingwordscore(const settingentry &e, const char *w, const char *lab, const char *var, const char *where)
{
    if(!w[0]) return 0;
    if(strstr(lab, w)) return 4;
    if(strstr(var, w)) return 3;
    if(strstr(e.words, w)) return 2;
    if(strstr(where, w)) return 1;
    return 0;
}

static void settingput(vector<char> &out, const char *s) { out.put(s, strlen(s)); }

static void settingsearchgui(const char *query)
{
    buildsettingindex();
    char q[MAXSTRLEN], uq[MAXSTRLEN];
    size_t ul = encodeutf8((uchar *)uq, sizeof(uq)-1, (const uchar *)query, strlen(query));
    uq[ul] = 0;
    settingnorm(uq, q, sizeof(q));
    vector<char> out;
    if(strlen(q) < 2)
    {
        settingput(out, "guitext \"^f4Type a word (English or French): sound, souris, crosshair, lumiere, HDR...\" 0\n");
        out.add('\0');
        result(out.getbuf());
        return;
    }
    vector<char *> words;
    for(char *w = strtok(q, " "); w; w = strtok(NULL, " ")) words.add(w);
    struct hit { int idx, score; };
    vector<hit> hits;
    loopv(settingindex)
    {
        const settingentry &e = settingindex[i];
        char lab[MAXSTRLEN], var[MAXSTRLEN], where[MAXSTRLEN];
        settingnorm(e.label, lab, sizeof(lab));
        settingnorm(e.var, var, sizeof(var));
        defformatstring(wh, "%s %s %s", e.page, e.title, e.section);
        settingnorm(wh, where, sizeof(where));
        int total = 0;
        bool all = true;
        loopvj(words)
        {
            int best = settingwordscore(e, words[j], lab, var, where);
            size_t wl = strlen(words[j]);
            loopvk(settingsyn)
            {
                const char *s = settingsyn[k];
                if(strncmp(s, words[j], wl) || s[wl] != '=') continue;
                char syn[MAXSTRLEN];
                copystring(syn, s + wl + 1);
                for(char *t = syn, *sp; t && *t; t = sp)
                {
                    sp = strchr(t, ' ');
                    if(sp) *sp++ = 0;
                    best = max(best, settingwordscore(e, t, lab, var, where) - 1);
                }
            }
            if(best <= 0) { all = false; break; }
            total += best;
        }
        if(all) { hit &h = hits.add(); h.idx = i; h.score = total; }
    }
    // best first; equal scores keep the menu order
    loopi(hits.length()) for(int j = i+1; j < hits.length(); j++)
        if(hits[j].score > hits[i].score) { hit t = hits[i]; hits[i] = hits[j]; hits[j] = t; }
    // then keep each place together, places in order of their best result
    vector<hit> grouped;
    loopv(hits)
    {
        if(hits[i].idx < 0) continue;
        const settingentry &a = settingindex[hits[i].idx];
        for(int j = i; j < hits.length(); j++) if(hits[j].idx >= 0)
        {
            const settingentry &b = settingindex[hits[j].idx];
            if(!strcmp(a.page, b.page) && !strcmp(a.section, b.section)) { grouped.add(hits[j]); hits[j].idx = -1; }
        }
    }
    hits.setsize(0);
    loopv(grouped) hits.add(grouped[i]);
    if(hits.empty()) settingput(out, "guitext \"^f4Nothing found. Try another word.\" 0\n");
    int shown = 0;
    string lastloc = "";
    loopv(hits)
    {
        if(shown >= 25) break;
        const settingentry &e = settingindex[hits[i].idx];
        defformatstring(loc, "%s%s%s", e.title, e.section[0] ? " > " : "", e.section);
        char line[3*MAXSTRLEN];
        if(strcmp(loc, lastloc))
        {
            defformatstring(btn, "\f4%s  ->", loc);
            string page;
            copystring(page, escapestring(e.page));
            nformatstring(line, sizeof(line), "guibutton %s [showgui %s] 0\n", escapestring(btn), page);
            settingput(out, line);
            copystring(lastloc, loc);
        }
        if(e.kind == 0) nformatstring(line, sizeof(line), "guicheckbox %s %s %s\n", escapestring(e.label), e.var, e.args);
        else if(e.kind == 1) nformatstring(line, sizeof(line), "guiradio %s %s %s\n", escapestring(e.label), e.var, e.args);
        else if(e.kind == 3) nformatstring(line, sizeof(line), "guitext %s 0\nguistayopen [%s %s]\n", escapestring(e.label), e.args, e.var);
        else if(e.kind == 4)
        {
            string w = "", alias = "";
            sscanf(e.args, "%259s %259s", w, alias);
            if(alias[0]) nformatstring(line, sizeof(line), "guitext %s 0\nif (!=s (textfocus) %s) [%s = $%s]\nguifield %s %s [%s $%s]\n",
                escapestring(e.label), alias, alias, e.var, alias, w, e.var, alias);
            else nformatstring(line, sizeof(line), "guitext %s 0\nguifield %s %s\n", escapestring(e.label), e.var, w);
        }
        else nformatstring(line, sizeof(line), "guitext %s 0\nguislider %s %s\n", escapestring(e.label), e.var, e.args);
        settingput(out, line);
        if(e.kind == 0 || e.kind == 1)
        {
            const char *why = settinglocked(e.var, e.kind == 1 ? parsefloat(e.args) : (e.args[0] ? parsefloat(e.args) : 1.0f));
            if(why && (e.kind == 1 || getfval(e.var) == 0))
            {
                defformatstring(wl, "\f4%s", why);
                nformatstring(line, sizeof(line), "guitext %s 0\n", escapestring(wl));
                settingput(out, line);
            }
        }
        shown++;
    }
    if(hits.length() > shown)
    {
        defformatstring(more, "guitext \"^f4... %d more: add a word to narrow down\" 0\n", hits.length() - shown);
        settingput(out, more);
    }
    out.add('\0');
    result(out.getbuf());
}
COMMAND(settingsearchgui, "s");

// ---------------------------------------------------------------------------
// Settings assistant: the local translation model (service /assistant) is
// asked how to find or change a setting. The model only writes text. Every
// command it proposes is checked here (a setting of the menus or a saved
// setting with a value inside its limits, or a command listed in
// data/assistant-commands.txt) and only runs when the player clicks Apply.
// Anything else is refused: no script, no file, no network, no quitting.

namespace game { extern bool assistanthttp(const char *pathq, const char *body, int bodylen, char *resp, int resplen, int timeoutms); }

// "cmd name ARGS | what it does | words"  or  "action name ARGS | what it does | words"
// (an action is only allowed as what a key does, in bind/specbind)
struct assistcommand { string name, args, desc, words; bool action; };
static vector<assistcommand> assistcommands;
static bool assistcommandsloaded = false;

static void assisttrim(char *s)
{
    char *b = s;
    while(*b == ' ' || *b == '\t') b++;
    int l = strlen(b);
    while(l > 0 && (b[l-1] == ' ' || b[l-1] == '\t')) l--;
    memmove(s, b, l);
    s[l] = 0;
}

// formatted text in a small ring of buffers, for one-off arguments
static const char *assistfmt(const char *fmt, ...) PRINTFARGS(1, 2);
static const char *assistfmt(const char *fmt, ...)
{
    static string bufs[4];
    static int cur = 0;
    cur = (cur + 1)%4;
    va_list v;
    va_start(v, fmt);
    vformatstring(bufs[cur], fmt, v);
    va_end(v);
    return bufs[cur];
}

static void loadassistsettings();

static void loadassistcommands()
{
    if(assistcommandsloaded) return;
    assistcommandsloaded = true;
    loadassistsettings();
    char *buf = loadfile(path("data/assistant-commands.txt", true), NULL);
    if(!buf) return;
    for(char *line = buf; line && *line; )
    {
        char *next = strchr(line, '\n');
        if(next) *next++ = 0;
        char *cr = strchr(line, '\r');
        if(cr) *cr = 0;
        bool action = !strncmp(line, "action ", 7);
        if(action || !strncmp(line, "cmd ", 4))
        {
            char *head = line + (action ? 7 : 4), *desc = strchr(head, '|'), *words = NULL;
            if(desc) { *desc++ = 0; words = strchr(desc, '|'); if(words) *words++ = 0; }
            while(*head == ' ') head++;
            char *sp = strchr(head, ' ');
            if(sp) *sp++ = 0;
            if(head[0])
            {
                assistcommand &c = assistcommands.add();
                c.action = action;
                copystring(c.name, head);
                copystring(c.args, sp ? sp : "");
                copystring(c.desc, desc ? desc : "");
                copystring(c.words, words ? words : "");
                assisttrim(c.args); assisttrim(c.desc); assisttrim(c.words);
            }
        }
        line = next;
    }
    delete[] buf;
}

// what each setting really does (data/assistant-settings.txt):
// "var: description", "var: -" = never offered (leftover or internal)
struct assistsettingdesc { string var; char *desc; };
static vector<assistsettingdesc> assistdescs;

static void loadassistsettings()
{
    char *buf = loadfile(path("data/assistant-settings.txt", true), NULL);
    if(!buf) return;
    for(char *line = buf; line && *line; )
    {
        char *next = strchr(line, '\n');
        if(next) *next++ = 0;
        char *cr = strchr(line, '\r');
        if(cr) *cr = 0;
        char *colon = strchr(line, ':');
        if(line[0] != '#' && colon)
        {
            *colon = 0;
            assisttrim(line);
            assisttrim(colon+1);
            if(line[0] && colon[1])
            {
                assistsettingdesc &d = assistdescs.add();
                copystring(d.var, line);
                d.desc = newstring(colon+1);
            }
        }
        line = next;
    }
    delete[] buf;
}

static const char *assistdesc(const char *var)
{
    loopv(assistdescs) if(!strcmp(assistdescs[i].var, var)) return assistdescs[i].desc;
    return NULL;
}

static bool assisthidden(const char *var)
{
    const char *d = assistdesc(var);
    return d && !strcmp(d, "-");
}

static assistcommand *findassistcommand(const char *name)
{
    loopv(assistcommands) if(!strcmp(assistcommands[i].name, name)) return &assistcommands[i];
    return NULL;
}

// never allowed, whatever data/assistant-commands.txt says
static bool assistforbiddencmd(const char *name)
{
    static const char * const bad[] = { "quit", "exit", "exec", "writecfg", "alias", "do", "doargs", "if", "loop", "while",
        "connect", "lanconnect", "disconnect", "reconnect", "say", "sayteam", "echo", "map", "savemap", "sendmap", "getmap",
        "demo", "recorddemo", "stopdemo", "kick", "ban", "setmaster", "auth", "sauth", "dauth", "servcmd", "rcon",
        "textsave", "textload", "tupdateapply", "tupdaterestart", "tmodelinstall", "tmodeluse", "tmodeldelete",
        "tmodelsaveapi", "tmodelclearapi", "assistantask", "assistantapply", "assistantundo", "importsettingsfrom" };
    loopi(sizeof(bad)/sizeof(bad[0])) if(!strcasecmp(name, bad[i])) return true;
    return false;
}

// settings the assistant never touches (where the game connects, files, keys...)
static bool assistprotectedvar(const char *name)
{
    static const char * const bad[] = { "host", "url", "port", "key", "pass", "dir", "path", "file", "server", "auth", "master",
        "update", "channel", "api", "exec", "script", "cmd" };
    string low;
    int n = 0;
    for(const char *s = name; *s && n < MAXSTRLEN-1; s++) low[n++] = tolower((uchar)*s);
    low[n] = 0;
    loopi(sizeof(bad)/sizeof(bad[0])) if(strstr(low, bad[i])) return true;
    return false;
}

static const settingentry *assistmenuentry(const char *var)
{
    loopv(settingindex) if(!strcmp(settingindex[i].var, var)) return &settingindex[i];
    return NULL;
}

// a setting the player could change: shown in a settings page, or saved in config.cfg
static bool assistsettingvar(ident *id, const char *&why)
{
    if(!id || id->type > ID_SVAR) { why = "unknown setting"; return false; }
    if(id->flags & (IDF_READONLY | IDF_OVERRIDE)) { why = "read-only or map setting"; return false; }
    if(!(id->flags & IDF_PERSIST) && !assistmenuentry(id->name)) { why = "not a player setting"; return false; }
    if(assistprotectedvar(id->name)) { why = "protected setting"; return false; }
    return true;
}

// one proposed command, split in plain words or "quoted text"; refuses
// anything that could run more than one plain command
static int assisttokens(const char *s, char toks[][MAXSTRLEN], int maxtoks, const char *&why)
{
    int n = 0;
    bool quoted = false;
    for(const char *p = s; *p; p++)
    {
        uchar c = *p;
        if(c < 32 || c > 126) { why = "unexpected character"; return -1; }
        if(c == '"') quoted = !quoted;
        // ";" only inside quotes: the actions of one key, each checked on its own (ACTION)
        if(strchr("[]()$@^\\`", c) || (c == ';' && !quoted) || (c == '/' && p[1] == '/')) { why = "not a single plain command"; return -1; }
    }
    const char *p = s;
    while(*p == ' ') p++;
    if(*p == '/') p++;
    for(;;)
    {
        while(*p == ' ' || *p == '\t') p++;
        if(!*p) break;
        if(n >= maxtoks) { why = "too many values"; return -1; }
        int l = 0;
        if(*p == '"')
        {
            for(p++; *p && *p != '"' && l < MAXSTRLEN-1; p++) toks[n][l++] = *p;
            if(*p != '"') { why = "unclosed quote"; return -1; }
            p++;
        }
        else for(; *p && *p != ' ' && *p != '\t' && *p != '"' && l < MAXSTRLEN-1; p++) toks[n][l++] = *p;
        toks[n++][l] = 0;
    }
    if(!n) why = "empty";
    return n;
}

static bool assistisint(const char *s, int &v)
{
    char *end = NULL;
    long l = strtol(s, &end, 0);
    if(!s[0] || *end) return false;
    v = int(l);
    return true;
}

static bool assistisnum(const char *s, float &v)
{
    char *end = NULL;
    double d = strtod(s, &end);
    if(!s[0] || *end) return false;
    v = float(d);
    return true;
}

static bool assistword(const char *s, int maxlen, const char *extra)
{
    int l = strlen(s);
    if(l < 1 || l > maxlen) return false;
    for(; *s; s++) if(!isalnum((uchar)*s) && *s != '_' && !strchr(extra, *s)) return false;
    return true;
}

static bool assistcheck(const char *line, int depth, char *canon, int canonlen, char *show, int showlen, const char *&why);

// ARGS types: KEY ACTION INT NUM WORD TEXT PAGE, "?" = optional
static bool assistargok(const char *type, const char *v, int depth, char *canon, int canonlen, const char *&why)
{
    int iv; float fv;
    string t;
    copystring(t, type);
    int tl = strlen(t);
    if(tl && t[tl-1] == '?') t[tl-1] = 0;
    if(!strcmp(t, "KEY")) { if(!assistword(v, 20, "")) { why = "bad key name"; return false; } concatstring(canon, v, canonlen); return true; }
    if(!strcmp(t, "WORD")) { if(!assistword(v, 24, "-.")) { why = "bad value"; return false; } concatstring(canon, v, canonlen); return true; }
    if(!strcmp(t, "INT")) { if(!assistisint(v, iv)) { why = "expected a whole number"; return false; } concatstring(canon, v, canonlen); return true; }
    if(!strcmp(t, "NUM")) { if(!assistisnum(v, fv)) { why = "expected a number"; return false; } concatstring(canon, v, canonlen); return true; }
    if(!strcmp(t, "TEXT")) { if(strlen(v) > 60 || strchr(v, ';')) { why = "text too long or not plain"; return false; } concatstring(canon, escapestring(v), canonlen); return true; }
    if(!strcmp(t, "PAGE"))
    {
        if(!settingpageok(v) && strcmp(v, "options") && strcmp(v, "settingsearch")) { why = "not a settings page"; return false; }
        concatstring(canon, v, canonlen);
        return true;
    }
    if(!strcmp(t, "ACTION"))
    {
        if(depth > 0) { why = "nested action"; return false; }
        // one key may run up to 4 allowed actions in a row: "setweapon RI; attack"
        string all = "", parts;
        copystring(parts, v);
        int count = 0;
        for(char *part = parts, *next; part; part = next)
        {
            next = strchr(part, ';');
            if(next) *next++ = 0;
            assisttrim(part);
            if(!part[0]) continue;
            if(++count > 4) { why = "more than 4 actions on one key"; return false; }
            char sub[MAXSTRLEN], subshow[MAXSTRLEN];
            if(!assistcheck(part, depth+1, sub, sizeof(sub), subshow, sizeof(subshow), why)) return false;
            if(all[0]) concatstring(all, "; ");
            concatstring(all, sub);
        }
        if(!count) { why = "no action"; return false; }
        concatstring(canon, escapestring(all), canonlen);
        return true;
    }
    why = "unknown argument type";
    return false;
}

// a readable name: the menu label, else the start of the description
static const char *assistlabel(const char *var)
{
    const settingentry *e = assistmenuentry(var);
    if(e && e->label[0] && strcmp(e->label, var)) return e->label;
    const char *d = assistdesc(var);
    if(!d || !strcmp(d, "-")) return var;
    static string name;
    int n = 0;
    while(d[n] && d[n] != '.' && d[n] != '(' && d[n] != ':' && n < 48) n++;
    while(n > 0 && d[n-1] == ' ') n--;
    copystring(name, d, n+1);
    return name[0] ? name : var;
}

// what a value means, from the description ("... 3 red, 4 grey ..."), or ""
static const char *assistvaluename(const char *var, int val)
{
    const char *d = assistdesc(var);
    if(!d) return "";
    defformatstring(num, "%d ", val);
    int nl = strlen(num);
    for(const char *p = d; (p = strstr(p, num)); p++)
    {
        if(p > d && p[-1] != ' ') continue;
        if(p > d+1 && p[-2] != ',' && p[-2] != '.') continue; // only in a list "..., 3 red, ..."
        const char *w = p + nl;
        int n = 0;
        while(w[n] && w[n] != ',' && w[n] != '.' && w[n] != '(' && n < 24) n++;
        while(n > 0 && w[n-1] == ' ') n--;
        if(!n || isdigit((uchar)w[0])) continue;
        static string name;
        name[0] = ' ';
        copystring(name+1, w, n+1);
        return name;
    }
    return "";
}

// checks one proposed command; canon = what will run, show = what the player reads
static bool assistcheck(const char *line, int depth, char *canon, int canonlen, char *show, int showlen, const char *&why)
{
    static char toks[8][MAXSTRLEN];
    char mytoks[8][MAXSTRLEN];
    int n = assisttokens(line, toks, 8, why);
    if(n <= 0) return false;
    memcpy(mytoks, toks, sizeof(mytoks));
    canon[0] = show[0] = 0;
    const char *name = mytoks[0];
    if(assistforbiddencmd(name)) { why = "forbidden command"; return false; }
    ident *id = getident(name);
    if(id && id->type <= ID_SVAR)
    {
        if(depth > 0) { why = "a key can only run an action"; return false; }
        if(!assistsettingvar(id, why)) return false;
        if(assisthidden(id->name)) { why = "internal setting, not offered"; return false; }
        if(n != 2) { why = "a setting needs exactly one value"; return false; }
        const char *v = mytoks[1];
        switch(id->type)
        {
            case ID_VAR:
            {
                int iv;
                if(!assistisint(v, iv)) { why = "expected a whole number"; return false; }
                if(iv < id->minval || iv > id->maxval) { why = "value outside the allowed range"; return false; }
                if(iv != *id->storage.i) { const char *locked = settinglocked(id->name, iv); if(locked) { why = locked; return false; } }
                nformatstring(canon, canonlen, "%s %d", id->name, iv);
                string oldname;
                copystring(oldname, assistvaluename(id->name, *id->storage.i));
                nformatstring(show, showlen, "%s: %d%s -> %d%s", assistlabel(id->name), *id->storage.i, oldname, iv, assistvaluename(id->name, iv));
                return true;
            }
            case ID_FVAR:
            {
                float fv;
                if(!assistisnum(v, fv)) { why = "expected a number"; return false; }
                if(fv < id->minvalf || fv > id->maxvalf) { why = "value outside the allowed range"; return false; }
                if(fv != *id->storage.f) { const char *locked = settinglocked(id->name, fv); if(locked) { why = locked; return false; } }
                nformatstring(canon, canonlen, "%s %s", id->name, floatstr(fv));
                nformatstring(show, showlen, "%s: %s -> %s", assistlabel(id->name), floatstr(*id->storage.f), floatstr(fv));
                return true;
            }
            default:
            {
                if(strlen(v) > 100 || strchr(v, ';')) { why = "text too long or not plain"; return false; }
                nformatstring(canon, canonlen, "%s %s", id->name, escapestring(v));
                nformatstring(show, showlen, "%s: \"%s\" -> \"%s\"", assistlabel(id->name), *id->storage.s, v);
                return true;
            }
        }
    }
    loadassistcommands();
    assistcommand *c = findassistcommand(name);
    if(!c || !id || (id->type != ID_COMMAND && id->type != ID_ALIAS)) { why = "not an allowed command"; return false; }
    if(c->action && depth == 0) { why = "this only goes on a key (bind)"; return false; }
    // match the values against the declared argument types
    char specs[8][MAXSTRLEN];
    const char *swhy = NULL;
    int ns = c->args[0] ? assisttokens(c->args, specs, 8, swhy) : 0;
    if(ns < 0) ns = 0;
    int req = 0;
    loopi(ns) if(specs[i][0] && specs[i][strlen(specs[i])-1] != '?') req++;
    if(n-1 < req || n-1 > ns) { why = "wrong number of values"; return false; }
    copystring(canon, c->name, canonlen);
    for(int i = 1; i < n; i++)
    {
        concatstring(canon, " ", canonlen);
        if(!assistargok(specs[i-1], mytoks[i], depth, canon, canonlen, why)) return false;
    }
    if(!strcmp(c->name, "showgui")) nformatstring(show, showlen, "Open the settings page \"%s\"", mytoks[1]);
    else if(!strcmp(c->name, "bind") || !strcmp(c->name, "specbind"))
        nformatstring(show, showlen, "%s %s: %s", c->name[0] == 's' ? "Spectator key" : "Key", mytoks[1], mytoks[2]);
    else nformatstring(show, showlen, "%s", c->desc[0] ? c->desc : c->name);
    return true;
}

// ---- conversation with the service ----

struct assistproposal
{
    string cmd, canon, show, undo;
    const char *why;
    bool ok, applied;
};
static vector<assistproposal> assistprops;
static vector<char *> assisthistory;        // UTF-8: q, a, q, a (last two exchanges)
static string assistquestion = "";
static char assiststatus[MAXSTRLEN] = "";
static vector<char> assistbody;
static char assistresp[32768];
static bool assistrespok = false;
static SDL_Thread *assistthread = NULL;
VAR(assistantthink, 0, 0, 1); // 1 = the model thinks before answering (slower); not saved, off again once the page is left
static SDL_atomic_t assiststate;            // 0 idle, 1 asking, 2 answer ready
static int assiststarted = 0;

static int assistworker(void *)
{
    assistrespok = game::assistanthttp("/assistant", assistbody.getbuf(), assistbody.length(), assistresp, sizeof(assistresp), 150000);
    SDL_AtomicSet(&assiststate, 2);
    return 0;
}

static void assistjsonstr(vector<char> &out, const char *s)
{
    out.add('"');
    for(; *s; s++)
    {
        uchar c = *s;
        if(c == '"' || c == '\\') { out.add('\\'); out.add(c); }
        else if(c == '\n') { out.add('\\'); out.add('n'); }
        else if(c < 32) out.add(' ');
        else out.add(c);
    }
    out.add('"');
}

static void assistjsonkey(vector<char> &out, const char *key, const char *val, bool comma = true)
{
    if(comma) out.add(',');
    assistjsonstr(out, key);
    out.add(':');
    assistjsonstr(out, val);
}

// stop words: too common to find a setting with
static bool assiststopword(const char *w)
{
    static const char * const stop[] = { "les", "des", "une", "est", "que", "qui", "pour", "pas", "mon", "mes", "ton", "tes", "son",
        "sur", "dans", "avec", "plus", "moins", "comment", "faire", "veux", "voudrais", "peux", "trop", "tres", "bien", "mettre",
        "met", "mets", "enleve", "enlever", "change", "changer", "the", "and", "how", "can", "want", "too", "more", "less", "make",
        "set", "turn", "off", "with", "for", "you", "cette", "cet", "quand", "jeu", "game", "option", "reglage", "parametre",
        "je", "tu", "il", "elle", "on", "nous", "vous", "me", "te", "se", "de", "du", "la", "le", "un", "au", "aux", "en", "et", "ou",
        "mais", "ne", "rien", "vois", "voir", "vu", "tout", "toute", "tous", "fait", "fais", "peu", "beaucoup", "ca", "cela", "ce",
        "moi", "toi", "suis", "ai", "as", "a", "it", "is", "my", "me", "to", "of", "in", "on", "at", "put", "please", "stp", "svp",
        "puis", "aussi", "encore", "trouve", "trouver", "ou", "est", "sont" };
    loopi(sizeof(stop)/sizeof(stop[0])) if(!strcmp(w, stop[i])) return true;
    return false;
}

// does normalized text contain word w? whole words; from 4 letters on, the
// start of a word is enough either way (sombre/sombres, weapon/weapons)
static bool assistwordin(const char *text, const char *w)
{
    int wl = strlen(w);
    if(!wl) return false;
    for(const char *t = text; *t; )
    {
        while(*t == ' ') t++;
        const char *e = t;
        while(*e && *e != ' ') e++;
        int tl = int(e - t);
        if(tl)
        {
            if(tl == wl && !strncmp(t, w, wl)) return true;
            if(wl >= 4 && tl >= 4 && !strncmp(t, w, min(tl, wl))) return true;
        }
        t = e;
    }
    return false;
}

// variable names glue words together (mixweapons, crosshairsize)
static bool assistinvar(const char *var, const char *w)
{
    int wl = strlen(w);
    if(wl >= 4) return strstr(var, w) != NULL;
    return !strcmp(var, w);
}

// one searchable item: a setting (menu or saved) or an allowed command
struct assistitem { const char *var; const assistcommand *cmd; int score; };

static int assistwordscore(const char *w, const char *lab, const char *var, const char *extra, const char *where, bool syn)
{
    if(assistwordin(lab, w)) return syn ? 3 : 4;
    if(assistinvar(var, w)) return 3;
    if(assistwordin(extra, w)) return syn ? 2 : 3;
    if(assistwordin(where, w)) return 1;
    return 0;
}

static int assistitemscore(const assistitem &it, vector<char *> &words)
{
    char lab[MAXSTRLEN], var[MAXSTRLEN], where[MAXSTRLEN], extra[2*MAXSTRLEN];
    extra[0] = where[0] = lab[0] = 0;
    if(it.cmd)
    {
        settingnorm(it.cmd->name, var, sizeof(var));
        settingnorm(it.cmd->desc, lab, sizeof(lab));
        settingnorm(it.cmd->words, extra, sizeof(extra));
    }
    else
    {
        settingnorm(it.var, var, sizeof(var));
        const settingentry *e = assistmenuentry(it.var);
        if(e)
        {
            settingnorm(e->label, lab, sizeof(lab));
            string w;
            formatstring(w, "%s %s %s", e->page, e->title, e->section);
            settingnorm(w, where, sizeof(where));
        }
        const char *desc = assistdesc(it.var);
        if(desc && strcmp(desc, "-"))
        {
            char nd[MAXSTRLEN];
            settingnorm(desc, nd, sizeof(nd));
            concatstring(extra, nd);
            concatstring(extra, " ");
        }
        size_t vl = strlen(var);
        loopv(settingvarwords) if(!strncmp(settingvarwords[i], var, vl) && settingvarwords[i][vl] == '=')
        {
            concatstring(extra, settingvarwords[i] + vl + 1);
            concatstring(extra, " ");
        }
    }
    int total = 0;
    loopvj(words)
    {
        const char *w = words[j];
        int best = assistwordscore(w, lab, var, extra, where, false);
        loopvk(settingsyn)
        {
            const char *s = settingsyn[k];
            const char *eq = strchr(s, '=');
            if(!eq) continue;
            string key;
            copystring(key, s, min(int(eq - s) + 1, MAXSTRLEN));
            if(!assistwordin(key, w)) continue;
            char syn[MAXSTRLEN];
            copystring(syn, eq + 1);
            for(char *t = syn, *sp; t && *t; t = sp)
            {
                sp = strchr(t, ' ');
                if(sp) *sp++ = 0;
                best = max(best, assistwordscore(t, lab, var, extra, where, true));
            }
        }
        total += best;
    }
    return total;
}

// menu label and/or what the setting really does
static const char *assistabout(const char *var, const settingentry *e)
{
    const char *d = assistdesc(var);
    if(d && !strcmp(d, "-")) d = NULL;
    const char *l = e && e->label[0] && strcmp(e->label, var) ? e->label : NULL;
    if(l && d) return assistfmt("%s - %s", l, d);
    return l ? l : (d ? d : "");
}

static void assistcatalogline(vector<char> &out, const assistitem &it)
{
    char line[2*MAXSTRLEN];
    if(it.cmd)
    {
        if(it.cmd->action)
        {
            char *keys = executestr(assistfmt("searchbinds %s", escapestring(it.cmd->name)));
            nformatstring(line, sizeof(line), "(key action) %s%s%s : %s%s%s\n", it.cmd->name, it.cmd->args[0] ? " " : "", it.cmd->args, it.cmd->desc,
                keys && keys[0] ? " (now on keys: " : "", keys && keys[0] ? assistfmt("%s)", keys) : "");
            DELETEA(keys);
        }
        else nformatstring(line, sizeof(line), "(command) %s%s%s : %s\n", it.cmd->name, it.cmd->args[0] ? " " : "", it.cmd->args, it.cmd->desc);
        out.put(line, strlen(line));
        return;
    }
    ident *id = getident(it.var);
    if(!id) return;
    const settingentry *e = assistmenuentry(it.var);
    string where = "";
    if(e)
    {
        const char *route = settingroute(e->page);
        if(route) formatstring(where, " [menu: %s%s%s | showgui %s]", route, e->section[0] ? " > " : "", e->section, e->page);
        else formatstring(where, " [menu: %s%s%s]", e->title, e->section[0] ? " > " : "", e->section);
    }
    // radio buttons: list the choices with their values
    string choices = "";
    loopv(settingindex) if(settingindex[i].kind == 1 && !strcmp(settingindex[i].var, it.var))
    {
        char val[MAXSTRLEN];
        const char *vend = NULL;
        char tk[1][MAXSTRLEN];
        if(assisttokens(settingindex[i].args, tk, 1, vend) == 1) copystring(val, tk[0]); else val[0] = 0;
        if(val[0] && strcmp(settingindex[i].label, it.var))
        {
            const char *locked = settinglocked(it.var, parsefloat(val));
            concformatstring(choices, "%s%s=%s", choices[0] ? ", " : " choices: ", val, settingindex[i].label);
            if(locked) concformatstring(choices, " (unavailable here, do not propose: %s)", locked);
        }
    }
    switch(id->type)
    {
        case ID_VAR:
            if(id->minval == 0 && id->maxval == 1) nformatstring(line, sizeof(line), "%s = %d (0 or 1) : %s%s%s\n", id->name, *id->storage.i, assistabout(it.var, e), choices, where);
            else nformatstring(line, sizeof(line), "%s = %d (%d..%d) : %s%s%s\n", id->name, *id->storage.i, id->minval, id->maxval, assistabout(it.var, e), choices, where);
            break;
        case ID_FVAR:
            nformatstring(line, sizeof(line), "%s = %s (%s..%s) : %s%s%s\n", id->name, floatstr(*id->storage.f), floatstr(id->minvalf), floatstr(id->maxvalf), assistabout(it.var, e), choices, where);
            break;
        default:
            nformatstring(line, sizeof(line), "%s = \"%s\" (text) : %s%s\n", id->name, *id->storage.s, assistabout(it.var, e), where);
            break;
    }
    out.put(line, strlen(line));
}

// the part of the catalogue that matches the question (the model's context is small)
// keys named in the question ("maj", "clic droit", "touche Q", F5...), with
// what they do now, so the model knows the key name and what it replaces
static int assistkeyhints(const char *utf8q, vector<char> &out)
{
    static const char * const names[][2] = {
        { "maj", "LSHIFT" }, { "shift", "LSHIFT" }, { "majuscule", "LSHIFT" }, { "ctrl", "LCTRL" }, { "control", "LCTRL" },
        { "controle", "LCTRL" }, { "alt", "LALT" }, { "espace", "SPACE" }, { "space", "SPACE" }, { "tab", "TAB" },
        { "tabulation", "TAB" }, { "entree", "RETURN" }, { "enter", "RETURN" }, { "molette", "MOUSE3" },
        { "wheel", "MOUSE3" }, { "echap", "ESCAPE" }, { "escape", "ESCAPE" }
    };
    char q[MAXSTRLEN];
    settingnorm(utf8q, q, sizeof(q));
    vector<char *> words;
    for(char *w = strtok(q, " "); w; w = strtok(NULL, " ")) words.add(w);
    vector<const char *> keys;
    string found[8];
    loopv(words)
    {
        const char *w = words[i], *key = NULL;
        loopj(sizeof(names)/sizeof(names[0])) if(!strcmp(w, names[j][0])) key = names[j][1];
        bool clic = !strcmp(w, "clic") || !strcmp(w, "click") || !strcmp(w, "bouton") || !strcmp(w, "button");
        if(clic && words.inrange(i+1))
        {
            const char *n = words[i+1];
            if(!strcmp(n, "gauche") || !strcmp(n, "left")) key = "MOUSE1";
            else if(!strcmp(n, "droit") || !strcmp(n, "right")) key = "MOUSE2";
            else if(!strcmp(n, "milieu") || !strcmp(n, "middle")) key = "MOUSE3";
        }
        // "touche q", "key f5", or a lone f1..f12
        bool afterkey = i > 0 && (!strcmp(words[i-1], "touche") || !strcmp(words[i-1], "key"));
        if(!key && ((afterkey && strlen(w) <= 3 && isalnum((uchar)w[0])) || (w[0] == 'f' && isdigit((uchar)w[1]) && strlen(w) <= 3)))
        {
            if(keys.length() >= 8) break;
            copystring(found[keys.length()], w);
            for(char *c = found[keys.length()]; *c; c++) *c = toupper((uchar)*c);
            key = found[keys.length()];
        }
        if(key && keys.length() < 8 && keys.find(key) < 0) keys.add(key);
    }
    loopv(keys)
    {
        char *now = executestr(assistfmt("getbind %s", keys[i]));
        const char *line = assistfmt("(key) %s now does: %s\n", keys[i], now && now[0] ? now : "nothing");
        out.put(line, strlen(line));
        DELETEA(now);
    }
    return keys.length();
}

// earlier = the previous questions: their words count less than the new one's
static void assistcatalog(const char *utf8q, vector<char> &out, const char *earlier = "")
{
    buildsettingindex();
    loadassistcommands();
    char q[MAXSTRLEN], pq[MAXSTRLEN];
    settingnorm(utf8q, q, sizeof(q));
    settingnorm(earlier, pq, sizeof(pq));
    int keyhints = assistkeyhints(utf8q, out);
    vector<char *> words, pwords;
    for(char *w = strtok(q, " "); w; w = strtok(NULL, " ")) if(strlen(w) >= 2 && !assiststopword(w)) words.add(w);
    for(char *w = strtok(pq, " "); w; w = strtok(NULL, " ")) if(strlen(w) >= 2 && !assiststopword(w)) pwords.add(w);
    vector<assistitem> items;
    // menu settings, once each
    loopv(settingindex)
    {
        const char *v = settingindex[i].var;
        if(assistmenuentry(v) != &settingindex[i]) continue;
        const char *why = NULL;
        if(!assistsettingvar(getident(v), why)) continue;
        if(assisthidden(v)) continue;
        assistitem &it = items.add(); it.var = v; it.cmd = NULL; it.score = 0;
    }
    // saved settings that are not in a menu
    enumerate(idents, ident, id,
    {
        if(id.type > ID_SVAR || !(id.flags & IDF_PERSIST) || assistmenuentry(id.name)) continue;
        const char *why = NULL;
        if(!assistsettingvar(&id, why)) continue;
        if(assisthidden(id.name)) continue;
        assistitem &it = items.add(); it.var = id.name; it.cmd = NULL; it.score = 0;
    });
    loopv(assistcommands) { assistitem &it = items.add(); it.var = NULL; it.cmd = &assistcommands[i]; it.score = 0; }
    loopv(items) items[i].score = 3*assistitemscore(items[i], words) + assistitemscore(items[i], pwords);
    // best first, menu order kept on ties
    vector<int> order;
    loopv(items) if(items[i].score > 0) order.add(i);
    loopi(order.length()) for(int j = i+1; j < order.length(); j++)
        if(items[order[j]].score > items[order[i]].score) { int t = order[i]; order[i] = order[j]; order[j] = t; }
    // settings and commands are picked apart, so a long list of settings
    // never pushes the commands out (the model's context is small)
    // settings outside the menus (no label) only fill a few places
    int shown = 0, nset = 0, nhidden = 0, ncmd = 0;
    bool action = false, hasbind = false, hasgui = false;
    loopv(order)
    {
        const assistitem &it = items[order[i]];
        bool hidden = !it.cmd && !assistmenuentry(it.var);
        if(it.cmd ? ncmd >= 6 : (nset >= 20 || out.length() > 3800 || (hidden && nhidden >= 5))) continue;
        assistcatalogline(out, it);
        if(it.cmd) { ncmd++; action |= it.cmd->action; hasbind |= !strcmp(it.cmd->name, "bind"); hasgui |= !strcmp(it.cmd->name, "showgui"); }
        else { nset++; if(hidden) nhidden++; }
        shown++;
    }
    // a key action is no use without bind; opening a page is always possible
    if((action || keyhints) && !hasbind) { assistcommand *b = findassistcommand("bind"); if(b) { assistitem it = { NULL, b, 0 }; assistcatalogline(out, it); } }
    if(!hasgui) { const char *l = "(command) showgui PAGE : open a settings page\n"; out.put(l, strlen(l)); }
    if(!shown) { const char *l = "(no setting matched the words of the question)\n"; out.put(l, strlen(l)); }
}

static void assistsetstatus(const char *s) { copystring(assiststatus, s, sizeof(assiststatus)); }

// the conversation shown in the window: questions, reply lines, proposals
enum { ASSIST_QUESTION = 0, ASSIST_TEXT, ASSIST_PROPOSAL, ASSIST_ERROR };
struct assistentry { int kind, prop; char *text; };
static vector<assistentry> assistlog;
static vector<int> assiststarts;            // first entry of each exchange
static const int ASSIST_MAXEXCHANGES = 12, ASSIST_MAXPROPS = 200;

static void assistaddentry(int kind, const char *text, int prop = -1)
{
    assistentry &e = assistlog.add();
    e.kind = kind;
    e.prop = prop;
    e.text = newstring(text ? text : "");
}

static void assistclearreply()
{
    loopv(assistlog) delete[] assistlog[i].text;
    assistlog.setsize(0);
    assiststarts.setsize(0);
    assistprops.setsize(0);
}

static void assistnewexchange(const char *question)
{
    if(assiststarts.length() >= ASSIST_MAXEXCHANGES)
    {
        // forget the oldest exchange (its proposals stay valid by index)
        int n = assiststarts.length() > 1 ? assiststarts[1] : assistlog.length();
        loopi(n) delete[] assistlog[i].text;
        assistlog.remove(0, n);
        assiststarts.remove(0);
        loopv(assiststarts) assiststarts[i] -= n;
    }
    assiststarts.add(assistlog.length());
    assistaddentry(ASSIST_QUESTION, question);
}

static void assistaddline(const char *s)
{
    // wrap at ~76 characters on spaces
    const int width = 76;
    while(*s)
    {
        int l = strlen(s);
        if(l <= width) { assistaddentry(ASSIST_TEXT, s); break; }
        int cut = width;
        while(cut > 20 && s[cut] != ' ') cut--;
        if(s[cut] != ' ') cut = width;
        string part;
        copystring(part, s, cut+1);
        assistaddentry(ASSIST_TEXT, part);
        s += cut;
        while(*s == ' ') s++;
    }
}

static void assistparse(const char *raw)
{
    const char *body = strstr(raw, "\r\n\r\n");
    body = body ? body + 4 : "";
    static char cube[sizeof(assistresp)];
    size_t n = decodeutf8((uchar *)cube, sizeof(cube)-1, (const uchar *)body, strlen(body));
    cube[n] = 0;
    // history keeps the model's own text (UTF-8)
    if(assisthistory.length() >= 4) { delete[] assisthistory.remove(0); delete[] assisthistory.remove(0); }
    char uq[MAXSTRLEN];
    size_t ql = encodeutf8((uchar *)uq, sizeof(uq)-1, (const uchar *)assistquestion, strlen(assistquestion));
    uq[ql] = 0;
    assisthistory.add(newstring(uq));
    assisthistory.add(newstring(body, min((int)strlen(body), 1500)));

    logoutf("assistant: question: %s", assistquestion);
    int blank = 0, lines = 0, props = 0;
    for(char *line = cube, *next; line && *line; line = next)
    {
        next = strpbrk(line, "\r\n");
        if(next) { *next++ = 0; while(*next == '\r' || *next == '\n') next++; }
        char *s = line;
        while(*s == ' ' || *s == '\t' || *s == '-' || *s == '*' || *s == '`') s++;
        int l = strlen(s);
        while(l > 0 && (s[l-1] == ' ' || s[l-1] == '`' || s[l-1] == '\t')) s[--l] = 0;
        // markdown emphasis the model may still write (**bold**, __bold__, # title)
        for(char *m; (m = strstr(s, "**")) || (m = strstr(s, "__")); ) memmove(m, m+2, strlen(m+2)+1);
        while(*s == '#') s++;
        while(*s == ' ') s++;
        if(*s) logoutf("assistant: | %s", s);
        if(!strncasecmp(s, "CMD:", 4))
        {
            s += 4;
            while(*s == ' ' || *s == '`') s++;
            if(!*s || props >= 8 || assistprops.length() >= ASSIST_MAXPROPS) continue;
            // a template, not a command ("crosshairsize [value]", "<key>"): the model is asking
            if(strpbrk(s, "[]<>{}")) continue;
            assistproposal &p = assistprops.add();
            copystring(p.cmd, s);
            p.applied = false;
            p.why = NULL;
            p.undo[0] = 0;
            p.ok = assistcheck(s, 0, p.canon, sizeof(p.canon), p.show, sizeof(p.show), p.why);
            if(!p.ok) conoutf(CON_DEBUG, "assistant: refused \"%s\" (%s)", s, p.why);
            assistaddentry(ASSIST_PROPOSAL, NULL, assistprops.length()-1);
            props++;
            continue;
        }
        if(!*s) { if(lines && !blank++) assistaddentry(ASSIST_TEXT, ""); continue; }
        blank = 0;
        if(lines++ < 40) assistaddline(s);
    }
    // several values for the same setting in one answer = a list of choices,
    // not a decision: the model is asking, so no Apply buttons for them
    vector<int> drop;
    for(int i = assistlog.length()-1; i >= 0 && assistlog[i].kind != ASSIST_QUESTION; i--)
    {
        if(assistlog[i].kind != ASSIST_PROPOSAL) continue;
        const char *a = assistprops[assistlog[i].prop].cmd;
        int alen = strcspn(a, " ");
        int same = 0;
        for(int j = assistlog.length()-1; j >= 0 && assistlog[j].kind != ASSIST_QUESTION; j--)
        {
            if(assistlog[j].kind != ASSIST_PROPOSAL) continue;
            const char *b = assistprops[assistlog[j].prop].cmd;
            if(int(strcspn(b, " ")) == alen && !strncmp(a, b, alen) && strncmp(a, "bind", 4)) same++;
        }
        if(same > 1) drop.add(i); // descending order
    }
    loopv(drop) { delete[] assistlog[drop[i]].text; assistlog.remove(drop[i]); }
    while(assistlog.length() && assistlog.last().kind == ASSIST_TEXT && !assistlog.last().text[0]) delete[] assistlog.pop().text;
}

static void assistpoll()
{
    if(SDL_AtomicGet(&assiststate) != 2) return;
    if(assistthread) { SDL_WaitThread(assistthread, NULL); assistthread = NULL; }
    SDL_AtomicSet(&assiststate, 0);
    assistsetstatus("");
    if(assistrespok)
    {
        assistparse(assistresp);
        return;
    }
    // the service's reason, if it gave one
    const char *body = strstr(assistresp, "\r\n\r\n");
    if(!strncmp(assistresp, "HTTP/1.", 7) && body && body[4])
    {
        char cube[MAXSTRLEN];
        size_t n = decodeutf8((uchar *)cube, sizeof(cube)-1, (const uchar *)body+4, strlen(body+4));
        cube[n] = 0;
        char *eol = strpbrk(cube, "\r\n");
        if(eol) *eol = 0;
        assistaddentry(ASSIST_ERROR, cube);
    }
    else if(!strncmp(assistresp, "HTTP/1.", 7) && strstr(assistresp, " 404 "))
        assistaddentry(ASSIST_ERROR, "The translation service is too old for the assistant: update L4ZY.");
    else assistaddentry(ASSIST_ERROR, "The local AI is not running: start the game with L4ZY.exe and install a model (Translation > Model).");
}

static void assistantask(const char *question)
{
    assistpoll();
    string q;
    copystring(q, question);
    assisttrim(q);
    if(strlen(q) < 2) return;
    if(SDL_AtomicGet(&assiststate) != 0) { assistsetstatus("\f2Wait for the answer to the previous question, then ask again."); return; }
    copystring(assistquestion, q);
    char uq[MAXSTRLEN];
    size_t ql = encodeutf8((uchar *)uq, sizeof(uq)-1, (const uchar *)q, strlen(q));
    uq[ql] = 0;
    vector<char> cat;
    // a follow-up ("c'est quoi 5", "plus petit") has no words of its own:
    // the settings of the previous question stay in view
    {
        string earlier = "";
        for(int i = 0; i+1 < assisthistory.length(); i += 2) { concatstring(earlier, " "); concatstring(earlier, assisthistory[i]); }
        assistcatalog(uq, cat, earlier);
    }
    cat.add('\0');
    assistbody.setsize(0);
    assistbody.add('{');
    assistjsonkey(assistbody, "question", uq, false);
    assistjsonkey(assistbody, "catalog", cat.getbuf());
    extern int assistantthink;
    if(assistantthink) assistbody.put(",\"think\":1", 10);
    assistbody.put(",\"history\":[", 12);
    loopv(assisthistory)
    {
        if(i) assistbody.add(',');
        assistjsonstr(assistbody, assisthistory[i]);
    }
    assistbody.put("]}", 2);
    assistsetstatus("");
    assistnewexchange(q);
    assiststarted = totalmillis;
    SDL_AtomicSet(&assiststate, 1);
    assistthread = SDL_CreateThread(assistworker, "assistant", NULL);
    if(!assistthread)
    {
        SDL_AtomicSet(&assiststate, 0);
        assistaddentry(ASSIST_ERROR, "Could not start the request.");
    }
}
COMMAND(assistantask, "s");

// runs a proposal the player clicked; checked again, right now, before it runs
static void assistantapply(int *i)
{
    if(!assistprops.inrange(*i)) return;
    assistproposal &p = assistprops[*i];
    if(!p.ok || p.applied) return;
    const char *why = NULL;
    if(!assistcheck(p.cmd, 0, p.canon, sizeof(p.canon), p.show, sizeof(p.show), why)) { p.ok = false; p.why = why; return; }
    // how to put it back
    char toks[3][MAXSTRLEN];
    const char *twhy = NULL;
    int n = assisttokens(p.cmd, toks, 3, twhy);
    p.undo[0] = 0;
    ident *id = n > 0 ? getident(toks[0][0] == '/' ? toks[0]+1 : toks[0]) : NULL;
    if(id && id->type == ID_VAR) formatstring(p.undo, "%s %d", id->name, *id->storage.i);
    else if(id && id->type == ID_FVAR) formatstring(p.undo, "%s %s", id->name, floatstr(*id->storage.f));
    else if(id && id->type == ID_SVAR) formatstring(p.undo, "%s %s", id->name, escapestring(*id->storage.s));
    else if(id && n >= 2 && (!strcmp(id->name, "bind") || !strcmp(id->name, "specbind")))
    {
        char *old = executestr(assistfmt("%s %s", id->name[0] == 's' ? "getspecbind" : "getbind", toks[1]));
        formatstring(p.undo, "%s %s %s", id->name, toks[1], escapestring(old ? old : ""));
        DELETEA(old);
    }
    conoutf("assistant: %s", p.canon);
    execute(p.canon);
    p.applied = true;
}
COMMAND(assistantapply, "i");

static void assistantundo(int *i)
{
    if(!assistprops.inrange(*i)) return;
    assistproposal &p = assistprops[*i];
    if(!p.applied || !p.undo[0]) return;
    conoutf("assistant: undo %s", p.undo);
    execute(p.undo);
    p.applied = false;
}
COMMAND(assistantundo, "i");

// debug: the catalogue the AI would get for this question (nothing is sent)
ICOMMAND(assistantcatalog, "s", (char *q),
{
    char uq[MAXSTRLEN];
    size_t ql = encodeutf8((uchar *)uq, sizeof(uq)-1, (const uchar *)q, strlen(q));
    uq[ql] = 0;
    vector<char> cat;
    assistcatalog(uq, cat);
    cat.add('\0');
    result(cat.getbuf());
});

ICOMMAND(assistantclear, "", (),
{
    if(SDL_AtomicGet(&assiststate) != 0) return; // an answer is on its way
    assistclearreply();
    assisthistory.deletearrays();
    assistsetstatus("");
});

ICOMMAND(assistantbusy, "", (), { assistpoll(); intret(SDL_AtomicGet(&assiststate) != 0 ? 1 : 0); });

static void assistput(vector<char> &out, const char *s) { out.put(s, strlen(s)); }

static void assistputtext(vector<char> &out, const char *text)
{
    char line[3*MAXSTRLEN];
    nformatstring(line, sizeof(line), "guitext %s 0\n", escapestring(text[0] ? text : " "));
    assistput(out, line);
}

static void assistputproposal(vector<char> &out, int i)
{
    if(!assistprops.inrange(i)) return;
    const assistproposal &p = assistprops[i];
    char line[3*MAXSTRLEN];
    if(!p.ok)
    {
        assistputtext(out, assistfmt("\f4Refused: %s  (%s)", p.cmd, p.why ? p.why : "not allowed"));
        return;
    }
    assistput(out, "guilist [\n");
    if(p.applied) nformatstring(line, sizeof(line), "guibutton \"^f0Undo\" [assistantundo %d] 0\nguistrut 1\n", i);
    else nformatstring(line, sizeof(line), "guibutton \"^f2Apply\" [assistantapply %d] 0\nguistrut 1\n", i);
    assistput(out, line);
    string shown;
    copystring(shown, p.show);
    assistputtext(out, assistfmt("%s%s", p.applied ? "\f0" : "\f7", shown));
    assistput(out, "]\n");
    assistputtext(out, assistfmt("\f4      /%s", p.canon));
}

// the conversation as gui commands (run with "do" in the assistant page),
// like a chat: oldest at the top, newest at the bottom above the input field
VAR(assistlayoutlog, 0, 0, 1);
static int assistscrolluntil = 0, assistlastgui = 0;

static void assistantgui()
{
    // the gui runs this twice a frame (layout, then drawing): anything that
    // changes the page may only change on the first call, or the two passes
    // disagree and the window flickers
    static int assistframe = -1, assistpad = 0, assistthinking = 0;
    static bool busy = false;
    if(totalmillis != assistframe)
    {
        assistframe = totalmillis;
        int before = assistlog.length();
        bool wasbusy = SDL_AtomicGet(&assiststate) != 0;
        assistpoll();
        busy = SDL_AtomicGet(&assiststate) == 1;
        assistthinking = (totalmillis - assiststarted)/1000;
        // follow the end of the conversation: when the page opens, when a
        // question or an answer arrives (for a moment, while the layout settles)
        bool opened = totalmillis - assistlastgui > 500;
        if(opened || assistlog.length() != before || wasbusy != busy)
            assistscrolluntil = totalmillis + 300;
        assistlastgui = totalmillis;
        extern int guipanelspare;
        if(opened) guipanelspare = 0; // measured on another page
        if(opened) assistantthink = 0; // the page was left: thinking is only for the questions you ticked it for
        // whole empty lines above the messages, so they sit at the bottom next
        // to the input field while the conversation is shorter than the window;
        // measured on the last frame, changed only by whole lines (no wobble)
        int spare = guipanelspare;
        assistpad = max(0, assistpad + (spare >= 0 ? spare/FONTH : -((-spare + FONTH-1)/FONTH)));
        if(getvar("assistlayoutlog")) logoutf("assistlayout t=%d spare=%d pad=%d entries=%d busy=%d", totalmillis, spare, assistpad, assistlog.length(), busy ? 1 : 0);
    }
    vector<char> out;
    if(assistpad > 0) assistput(out, assistfmt("guistrut %d\n", assistpad));
    if(assiststarts.empty() && !busy)
    {
        assistput(out, "guitext \"^f4Ask in English or French, for example:\" 0\n");
        assistput(out, "guitext \"^f4  - how do I change the size of my crosshair?\" 0\n");
        assistput(out, "guitext \"^f4  - I can't see anything in dark places\" 0\n");
        assistput(out, "guitext \"^f4  - put the scoreboard on the TAB key\" 0\n");
        assistput(out, "guitext \"^f4Nothing changes until you click Apply.\" 0\n");
    }
    loopv(assiststarts)
    {
        int begin = assiststarts[i], end = i+1 < assiststarts.length() ? assiststarts[i+1] : assistlog.length();
        if(i > 0) assistput(out, "guistrut 0.3\nguibar\nguistrut 0.3\n");
        bool proposals = false;
        for(int j = begin; j < end; j++)
        {
            const assistentry &e = assistlog[j];
            switch(e.kind)
            {
                case ASSIST_QUESTION: assistputtext(out, assistfmt("\f1> %s", e.text)); break;
                case ASSIST_ERROR: assistputtext(out, assistfmt("\f3%s", e.text)); break;
                case ASSIST_PROPOSAL:
                    if(!proposals) { assistput(out, "guistrut 0.2\n"); proposals = true; }
                    assistputproposal(out, e.prop);
                    break;
                default: assistputtext(out, e.text); break;
            }
        }
    }
    if(busy) assistputtext(out, assistfmt("\f2Thinking... %ds", assistthinking));
    if(assiststatus[0]) assistputtext(out, assiststatus);
    if(totalmillis < assistscrolluntil) assistput(out, "guiscrollbottom\n");
    out.add('\0');
    result(out.getbuf());
}
COMMAND(assistantgui, "");

// reloads data/assistant-commands.txt and the keywords
ICOMMAND(assistantreload, "", (),
{
    assistcommands.setsize(0);
    loopv(assistdescs) delete[] assistdescs[i].desc;
    assistdescs.setsize(0);
    assistcommandsloaded = false;
    loadassistcommands();
    intret(assistcommands.length());
});

// what the checks say about one command line, without running it (tests)
static void assistantcheck(const char *line)
{
    string canon, show;
    const char *why = NULL;
    bool ok = assistcheck(line, 0, canon, sizeof(canon), show, sizeof(show), why);
    if(ok) conoutf("assistant check: OK  %s  ->  /%s", show, canon);
    else conoutf("assistant check: REFUSED  %s  (%s)", line, why ? why : "?");
    intret(ok ? 1 : 0);
}
COMMAND(assistantcheck, "s");

// the part of the catalogue sent to the model for a question (tests)
static void assistantcatalog(const char *question)
{
    char uq[MAXSTRLEN];
    size_t ql = encodeutf8((uchar *)uq, sizeof(uq)-1, (const uchar *)question, strlen(question));
    uq[ql] = 0;
    vector<char> cat;
    assistcatalog(uq, cat);
    cat.add('\0');
    for(char *l = cat.getbuf(), *next; l && *l; l = next)
    {
        next = strchr(l, '\n');
        if(next) *next++ = 0;
        conoutf("catalog: %s", l);
    }
}
COMMAND(assistantcatalog, "s");
