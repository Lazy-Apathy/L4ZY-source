#include "engine.h"

VARP(maxdynlights, 0, min(3, MAXDYNLIGHTS), MAXDYNLIGHTS);
VARP(dynlightdist, 0, 1024, 10000);

struct dynlight
{
    vec o, hud;
    float radius, initradius, curradius, dist;
    vec color, initcolor, curcolor;
    int fade, peak, expire, flags;
    physent *owner;
    // RT weapon lights: custom tint (-1 none) and the explosion fireball the
    // light follows (state 0 not looked up yet, 1 found, 2 none: own life).
    int rtcolor, fbstate, fbmillis, fbfade;
    vec fbcolor;

    void calcradius()
    {
        if(fade + peak > 0)
        {
            int remaining = expire - lastmillis;
            if(flags&DL_EXPAND)
                curradius = initradius + (radius - initradius) * (1.0f - remaining/float(fade + peak));
            else if(!(flags&DL_FLASH) && remaining > fade)
                curradius = initradius + (radius - initradius) * (1.0f - float(remaining - fade)/peak);
            else if(flags&DL_SHRINK)
                curradius = (radius*remaining)/fade;
            else curradius = radius;
        }
        else curradius = radius;
    }

    void calccolor()
    {
        if(flags&DL_FLASH || peak <= 0) curcolor = color;
        else
        {
            int peaking = expire - lastmillis - fade;
            if(peaking <= 0) curcolor = color;
            else curcolor.lerp(initcolor, color, 1.0f - float(peaking)/peak);
        }

        float intensity = 1.0f;
        if(fade > 0)
        {
            int fading = expire - lastmillis;
            if(fading < fade) intensity = float(fading)/fade;
        }
        curcolor.mul(intensity);
        // KLUGE: this prevents nvidia drivers from trying to recompile dynlight fragment programs
        loopk(3) if(fmod(curcolor[k], 1.0f/256) < 0.001f) curcolor[k] += 0.001f;
    }
};

vector<dynlight> dynlights;
vector<dynlight *> closedynlights;

void adddynlight(const vec &o, float radius, const vec &color, int fade, int peak, int flags, float initradius, const vec &initcolor, physent *owner, int rtcolor)
{
    if(!maxdynlights) return;
    if(o.dist(camera1->o) > dynlightdist || radius <= 0) return;

    int insert = 0, expire = fade + peak + lastmillis;
    loopvrev(dynlights) if(expire>=dynlights[i].expire) { insert = i+1; break; }
    dynlight d;
    d.o = d.hud = o;
    d.radius = radius;
    d.initradius = initradius;
    d.color = color;
    d.initcolor = initcolor;
    d.fade = fade;
    d.peak = peak;
    d.expire = expire;
    d.flags = flags;
    d.owner = owner;
    d.rtcolor = rtcolor;
    d.fbstate = d.fbmillis = d.fbfade = 0;
    d.fbcolor = vec(1, 1, 1);
    dynlights.insert(insert, d);
}

void cleardynlights()
{
    int faded = -1;
    loopv(dynlights) if(lastmillis<dynlights[i].expire) { faded = i; break; }
    if(faded<0) dynlights.setsize(0);
    else if(faded>0) dynlights.remove(0, faded);
}

void removetrackeddynlights(physent *owner)
{
    loopvrev(dynlights) if(owner ? dynlights[i].owner == owner : dynlights[i].owner != NULL) dynlights.remove(i);
}

void updatedynlights()
{
    cleardynlights();
    game::adddynlights();

    loopv(dynlights)
    {
        dynlight &d = dynlights[i];
        if(d.owner) game::dynlighttrack(d.owner, d.o, d.hud);
        d.calcradius();
        d.calccolor();
    }
}

int finddynlights()
{
    closedynlights.setsize(0);
    if(!maxdynlights) return 0;
    physent e;
    e.type = ENT_CAMERA;
    loopvj(dynlights)
    {
        dynlight &d = dynlights[j];
        if(d.curradius <= 0) continue;
        d.dist = camera1->o.dist(d.o) - d.curradius;
        if(d.dist > dynlightdist || isfoggedsphere(d.curradius, d.o) || pvsoccludedsphere(d.o, d.curradius))
            continue;
        if(reflecting || refracting > 0)
        {
            if(d.o.z + d.curradius < reflectz) continue;
        }
        else if(refracting < 0 && d.o.z - d.curradius > reflectz) continue;
        e.o = d.o;
        e.radius = e.xradius = e.yradius = e.eyeheight = e.aboveeye = d.curradius;
        if(!collide(&e, vec(0, 0, 0), 0, false)) continue;

        int insert = 0;
        loopvrev(closedynlights) if(d.dist >= closedynlights[i]->dist) { insert = i+1; break; }
        closedynlights.insert(insert, &d);
        if(closedynlights.length() >= DYNLIGHTMASK) break;
    }
    return closedynlights.length();
}

// The game's dynamic lights as the RT lighting pass sees them (hwrt/lights.cpp,
// hwrtdynlights). GL keeps its own curves; with RT lighting on, the RT image and
// the HUD gun's own flash (dynlightreaching below) take these instead.
// kind: 0 a muzzle flash (follows its owner), 1 re-added every frame (a rocket
// or grenade in flight), 2 a timed burst (explosion, flag event).
// hwrtdynshape 2 (default): shape 1, plus: the rifle (DL_SHARP) has its own
// shorter, whiter, tighter flash (hwrtdynrifle); a grenade in flight is a little
// brighter and wider; an explosion's light follows the game's fireball sphere
// (particle_fireball): lit as long as the sphere is drawn, fading like its
// opacity, coloured like it, with its own radius; a custom trail colour of the
// shooter (adddynlight rtcolor: trail colour for a muzzle flash, projectile
// colour for a rocket or grenade) tints the flash and the projectile; an
// explosion follows its fireball, in the projectile colour when one is chosen.
// 1: a muzzle flash peaks for a few milliseconds and dies out exponentially in
// tens of milliseconds, white-hot to orange; a projectile keeps its light with a
// slight flicker; a burst rises in about 30 ms, decays smoothly and cools. 0:
// GL's own radius, colour and fade (the first version). Not saved.
VAR(hwrtdynshape, 0, 2, 2);
// Lab tuning, not saved: muzzle peak over GL's colour, ms at full intensity and
// decay constant; projectile intensity, radius and flicker depth; burst
// intensity and rise; scale of the light source radius (soft shadows, 0 = hard).
FVAR(hwrtdynmuzzle, 0, 1.8f, 8);
VAR(hwrtdynmuzzlehold, 0, 8, 100);
VAR(hwrtdynmuzzletau, 1, 22, 200);
FVAR(hwrtdynproj, 0, 1.2f, 8);
FVAR(hwrtdynprojradius, 0.5f, 1.6f, 4);
FVAR(hwrtdynprojpulse, 0, 0.12f, 0.5f);
FVAR(hwrtdynburst, 0, 1, 8);
VAR(hwrtdynburstrise, 1, 30, 200);
FVAR(hwrtdynsoft, 0, 1, 4);
// Shape 2, lab, not saved: rifle flash variant (1) or the shape-1 flash (0), its
// peak over GL's colour, ms at full intensity, decay constant and radius scale;
// grenade in flight intensity and radius over the projectile ones.
VAR(hwrtdynrifle, 0, 1, 1);
FVAR(hwrtdynriflepeak, 0, 1.6f, 8);
VAR(hwrtdynriflehold, 0, 6, 100);
VAR(hwrtdynrifletau, 1, 14, 200);
FVAR(hwrtdynrifleradius, 0.25f, 0.8f, 2);
FVAR(hwrtdyngrenade, 0, 1.25f, 4);
FVAR(hwrtdyngrenaderadius, 0.5f, 1.2f, 4);

static inline float dynmax(const vec &c) { return max(c.x, max(c.y, c.z)); }
static inline float dynluma(const vec &c) { return 0.2126f*c.x + 0.7152f*c.y + 0.0722f*c.z; }

// A custom trail colour as a light: its hue (strongest channel 1), lifted when
// the hue is dark (a deep blue has a tenth of the luminance of orange) towards
// the luminance of the stock hue, at most x1.8, so a dark or dimmed choice
// (trailbrightness) still lights the room. False: no tint (stock colour, black).
static bool dyntint(const dynlight &d, const vec &stockhue, vec &hue)
{
    if(d.rtcolor < 0) return false;
    vec c((d.rtcolor>>16)&0xFF, (d.rtcolor>>8)&0xFF, d.rtcolor&0xFF);
    float m = dynmax(c);
    if(m < 8) return false;
    hue = c.div(m);
    hue.mul(clamp(sqrtf(dynluma(stockhue)/max(dynluma(hue), 0.01f)), 1.0f, 1.8f));
    return true;
}

extern bool findfireball(const vec &o, int &millis, int &fade, vec &color, bool &blue);

// Shape 2: an explosion's light lives as long as its fireball (fade = growth x 20
// ms: 720 for a rocket, 615 for a grenade) and fades like its opacity (linear,
// 1 to 0). Colour: the fireball texture's mean colour (alpha weighted:
// packages/particles/explosion.png, plasma.png) times the sphere colour,
// over-driven like the explosion shader (x2.5, clipped); a dimmer sphere
// (explodebright) dims the light, down to 35 %. Looked up once, when the light
// starts. No fireball (particles off, a flag event, a test light): the light's
// own life (90 %), its own hue.
static void matchfireball(dynlight &d)
{
    if(d.fbstate) return;
    int millis = 0, fade = 0;
    vec col;
    bool blue = false;
    if(findfireball(d.o, millis, fade, col, blue))
    {
        static const vec texmean[2] = { vec(0.726f, 0.412f, 0.172f), vec(0.329f, 0.626f, 1.0f) };
        vec seen = vec(texmean[blue ? 1 : 0]).mul(col).mul(2.5f).min(1.0f);
        float m = dynmax(seen);
        d.fbcolor = m > 0 ? seen.div(m).mul(max(dynmax(col), 0.35f)) : vec(1, 1, 1);
        d.fbmillis = millis;
        d.fbfade = max(fade, 1);
        d.fbstate = 1;
    }
    else
    {
        float m = dynmax(d.color);
        d.fbcolor = m > 0 ? vec(d.color).div(m) : vec(1, 1, 1);
        d.fbmillis = d.expire - (d.fade + d.peak);
        d.fbfade = max(int(0.9f*(d.fade + d.peak)), 1);
        d.fbstate = 2;
    }
}

// Cooling of a burst: k 0 is the hot peak (towards white), 1 the dying ember
// (each channel squared over the strongest, so orange goes red, blue goes deep blue).
static vec dyncool(const vec &c, float k)
{
    float m = max(c.x, max(c.y, c.z));
    if(m <= 0) return c;
    vec hot = vec(c).lerp(vec(m, m, m), 0.35f);
    vec cold = vec(c).mul(c).div(m);
    return hot.lerp(cold, clamp(k, 0.0f, 1.0f));
}

static int dynkind(const dynlight &d) { return d.owner ? 0 : (d.fade + d.peak <= 0 ? 1 : 2); }

static bool shapedynlight(dynlight &d, vec &color, float &radius, float &src)
{
    int kind = dynkind(d), life = d.fade + d.peak;
    bool v3 = hwrtdynshape >= 2;
    float t = float(lastmillis - (d.expire - life));
    // The game only flashes (DL_FLASH) from a muzzle; an ownerless flash is a
    // steady test light: GL's colour and radius, a burst's source size.
    if(kind == 2 && (d.flags&DL_FLASH))
    {
        color = d.curcolor;
        radius = d.curradius;
        src = 0.15f*radius*hwrtdynsoft;
        return radius > 0;
    }
    if(kind == 0)
    {
        // Rifle variant: a crack rather than a bloom (about 6 ms full, gone in
        // ~60 ms), a little weaker at the peak, near-white, on a tighter radius.
        bool sharp = v3 && hwrtdynrifle && (d.flags&DL_SHARP);
        float hold = float(sharp ? hwrtdynriflehold : hwrtdynmuzzlehold);
        float tau = float(sharp ? hwrtdynrifletau : hwrtdynmuzzletau);
        float env = t <= hold ? 1.0f : expf(-(t - hold)/tau);
        if(env < 0.02f) return false;
        float m = dynmax(d.color);
        vec hot = sharp ? vec(1.0f, 0.95f, 0.88f) : vec(1.0f, 0.80f, 0.55f);
        vec cold = sharp ? vec(1.0f, 0.82f, 0.62f) : vec(1.0f, 0.52f, 0.20f);
        vec tint;
        if(v3 && dyntint(d, hot, tint))
        {
            float w = dynmax(tint);
            cold = tint;
            hot = vec(tint).lerp(vec(w, w, w), sharp ? 0.5f : 0.35f);
        }
        color = hot.mul(m).lerp(cold.mul(m), 1 - env).mul(env*(sharp ? hwrtdynriflepeak : hwrtdynmuzzle));
        radius = d.radius*(0.85f + 0.25f*env)*(sharp ? hwrtdynrifleradius : 1.0f);
        src = sharp ? 1.0f : 1.5f;
    }
    else if(kind == 1)
    {
        float ts = lastmillis/1000.0f;
        float ph = 0.013f*d.o.x + 0.017f*d.o.y + 0.011f*d.o.z;
        float p = 1 + hwrtdynprojpulse*(0.65f*sinf(56.5487f*ts + ph) + 0.35f*sinf(144.513f*ts + 2.3f*ph));
        // The game's grenade light is the small one (8 units; a rocket's is 20).
        bool grenade = v3 && d.radius < 12;
        vec base = d.color, tint;
        float m = dynmax(base);
        if(v3 && m > 0 && dyntint(d, vec(base).div(m), tint)) base = tint.mul(m);
        color = base.mul(hwrtdynproj*p*(grenade ? hwrtdyngrenade : 1.0f));
        radius = d.radius*hwrtdynprojradius*(grenade ? hwrtdyngrenaderadius : 1.0f);
        src = 2.0f;
    }
    else if(v3)
    {
        // Follows the fireball sphere: same start and life, linear fade like its
        // opacity, its colour; radius as shape 1 (the light's own, not the sphere's).
        matchfireball(d);
        float u = float(lastmillis - d.fbmillis)/float(d.fbfade);
        float env = 1 - clamp(u, 0.0f, 1.0f);
        if(env < 0.01f) return false;
        // A projectile colour (rtcolor) gives the hue exactly (the sphere's own
        // mean is pushed towards cyan / white by the shader's over-drive); the
        // sphere still gives the brightness (explodebright).
        vec hue = d.fbcolor, tint;
        if(dyntint(d, d.fbcolor, tint)) hue = tint.mul(dynmax(d.fbcolor));
        color = hue.mul(dynmax(d.color)*env*hwrtdynburst);
        float grow = clamp(t/60.0f, 0.0f, 1.0f);
        radius = (d.initradius + (d.radius - d.initradius)*grow)*(1 + 0.1f*clamp(u, 0.0f, 1.0f));
        src = 0.15f*radius;
    }
    else
    {
        float rise = float(hwrtdynburstrise);
        float up = clamp(t/rise, 0.0f, 1.0f);
        up = up*up*(3 - 2*up);
        float span = max(float(life) - rise, 1.0f);
        float env = t <= rise ? up : expf(-(t - rise)/(max(life, 200)/4.0f))*clamp(1 - (t - rise)/span, 0.0f, 1.0f);
        if(env < 0.01f) return false;
        float grow = clamp(t/60.0f, 0.0f, 1.0f);
        radius = (d.initradius + (d.radius - d.initradius)*grow)*(1 + 0.1f*clamp(t/max(float(life), 1.0f), 0.0f, 1.0f));
        color = dyncool(d.color, t <= rise ? 0 : 1 - env).mul(env*hwrtdynburst);
        src = 0.15f*radius;
    }
    src *= hwrtdynsoft;
    return radius > 0 && max(color.x, max(color.y, color.z)) >= 0.004f;
}

int numalldynlights() { return dynlights.length(); }

bool getalldynlight(int n, vec &o, float &radius, vec &color, int &kind, float &src)
{
    if(!dynlights.inrange(n)) return false;
    dynlight &d = dynlights[n];
    if(d.curradius <= 0) return false;
    o = d.o;
    kind = dynkind(d);
    src = 0;
    if(hwrtdynshape) return shapedynlight(d, color, radius, src);
    radius = d.curradius;
    color = d.curcolor;
    return true;
}

bool getdynlight(int n, vec &o, float &radius, vec &color)
{
    if(!closedynlights.inrange(n)) return false;
    dynlight &d = *closedynlights[n];
    o = d.o;
    radius = d.curradius;
    color = d.curcolor;
    return true;
}

void dynlightreaching(const vec &target, vec &color, vec &dir, bool hud)
{
    vec dyncolor(0, 0, 0);//, dyndir(0, 0, 0);
    // The HUD gun stays GL in RT: with the RT weapon lights on it takes the same
    // shaped flash as the RT image, so the hand flashes with the muzzle light.
    extern int hwrtavailable, hwrtdynlights;
    extern bool hwrtfailed;
    bool shaped = hud && hwrt && hwrtavailable && !hwrtfailed && hwrtdynlights && hwrtdynshape;
    loopv(dynlights)
    {
        dynlight &d = dynlights[i];
        if(d.curradius<=0) continue;
        float radius = d.curradius, src = 0;
        vec color = d.curcolor;
        if(shaped && !shapedynlight(d, color, radius, src)) continue;

        vec ray(hud ? d.hud : d.o);
        ray.sub(target);
        float mag = ray.squaredlen();
        if(mag >= radius*radius) continue;

        color.mul(1 - sqrtf(mag)/radius);
        dyncolor.add(color);
        //dyndir.add(ray.mul(intensity/mag));
    }
#if 0
    if(!dyndir.iszero())
    {
        dyndir.normalize();
        float x = dyncolor.magnitude(), y = color.magnitude();
        if(x+y>0)
        {
            dir.mul(x);
            dyndir.mul(y); 
            dir.add(dyndir).div(x+y);
            if(dir.iszero()) dir = vec(0, 0, 1);
            else dir.normalize();
        }
    }
#endif
    color.add(dyncolor);
}

void calcdynlightmask(vtxarray *va)
{
    uint mask = 0;
    int offset = 0;
    loopv(closedynlights)
    {
        dynlight &d = *closedynlights[i];
        if(d.o.dist_to_bb(va->geommin, va->geommax) >= d.curradius) continue;

        mask |= (i+1)<<offset;
        offset += DYNLIGHTBITS;
        if(offset >= maxdynlights*DYNLIGHTBITS) break;
    }
    va->dynlightmask = mask;
}

int setdynlights(vtxarray *va)
{
    if(closedynlights.empty() || !va->dynlightmask) return 0;

    extern bool minimizedynlighttcusage();

    static vec4 posv[MAXDYNLIGHTS];
    static vec colorv[MAXDYNLIGHTS];

    int index = 0;
    for(uint mask = va->dynlightmask; mask; mask >>= DYNLIGHTBITS, index++)
    {
        dynlight &d = *closedynlights[(mask&DYNLIGHTMASK)-1];

        float scale = 1.0f/d.curradius;
        vec origin = vec(d.o).mul(-scale);

        if(index>0 && minimizedynlighttcusage())
        {
            scale /= posv[0].w;
            origin.sub(vec(posv[0]).mul(scale));
        }

        posv[index] = vec4(origin, scale);
        colorv[index] = d.curcolor;
    }

    GLOBALPARAMV(dynlightpos, posv, index);
    GLOBALPARAMV(dynlightcolor, colorv, index);

    return index;
}

