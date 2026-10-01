// son3d.cpp: optional 3D sound with Steam Audio (Valve, Apache 2.0) for positional sounds.
//
// son3d 0 (default): sound.cpp behaves exactly as before, phonon.dll is never loaded and no
//   effect is registered with SDL_mixer.
// son3d 1, headphones: HRTF binaural rendering replaces the stereo pan of positional sounds, so
//   ahead/behind and above/below can be heard. The built-in HRTF only exists at 24000, 44100 and
//   48000 Hz: the mixer is switched to 44100 Hz when needed.
// son3d 2, speakers: the stereo pan of the game is kept, only occlusion is added.
// son3docclusion: rays against the map geometry (raycube) from the listener to a few points
//   around the source; no precomputation, so any map, online or in a demo. Blocked sounds are
//   muffled (low 0.55, mid 0.25, high 0.12 through a wall), not muted.
// No reverb: it would blur the position of shots and jumps (readability first).
// The distance attenuation stays the one of sound.cpp (channel volume). Sounds without a
// position (menus, announcements, music, the player's own sounds) are not touched.

#include "engine.h"
#include "SDL_mixer.h"
#include "son3d.h"

extern bool nosound;
extern int soundfreq, soundbufferlen, maxchannels;
extern void resetsound();
extern vec hitsurface;

int son3deffectif = SON3D_OFF;
static string son3derreur = "";
static bool son3dapplying = false;

void son3dapply();
VARFP(son3d, 0, 0, 2, son3dapply());
VARFP(son3docclusion, 0, 1, 1, son3dapply());
VAR(son3drayms, 10, 50, 1000);       // occlusion rays of one source at most every son3drayms
VAR(son3dvolumesource, 0, 6, 32);    // radius of the sampled source volume (world units)

ICOMMAND(son3deffectif, "", (), intret(son3deffectif));
ICOMMAND(son3derreur, "", (), result(son3derreur));

static void phononpath(char *dst, size_t len)
{
#ifdef WIN32
    char exe[MAX_PATH] = "";
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    char *slash = strrchr(exe, '\\');
    if(slash) slash[1] = '\0'; else exe[0] = '\0';
    copystring(dst, exe, len);
    concatstring(dst, "phonon.dll", len);
#else
    copystring(dst, "libphonon.so", len);
#endif
}

// why 3D sound cannot be chosen, "" when it can (menu settinglock)
ICOMMAND(son3draison, "", (),
{
    if(son3derreur[0] && !son3d_dsp_loaded()) { result(son3derreur); return; }
    string path;
    phononpath(path, sizeof(path));
    stream *f = openrawfile(path, "rb");
    if(!f) { result("phonon.dll (Steam Audio) is missing"); return; }
    delete f;
    result("");
});

void son3dapply()
{
    son3deffectif = SON3D_OFF;
    son3derreur[0] = '\0';
    if(!son3d || nosound) return;
    char err[256] = "";
    if(!son3d_dsp_loaded())
    {
        string path;
        phononpath(path, sizeof(path));
        if(!son3d_dsp_load(path, err, sizeof(err)))
        {
            copystring(son3derreur, err);
            conoutf(CON_ERROR, "3D sound unavailable: %s", err);
            return;
        }
    }
    int freq = 0, chans = 0;
    Uint16 fmt = 0;
    if(!Mix_QuerySpec(&freq, &fmt, &chans)) return;
    if(fmt != AUDIO_S16SYS || chans != 2)
    {
        copystring(son3derreur, "unsupported audio format");
        conoutf(CON_ERROR, "3D sound unavailable: %s", son3derreur);
        return;
    }
    if(son3d == SON3D_CASQUE && freq != 24000 && freq != 44100 && freq != 48000)
    {
        if(son3dapplying) { copystring(son3derreur, "needs a sound frequency of 44100 Hz"); return; }
        conoutf("\f23D sound (headphones): sound frequency set to 44100 Hz, required by the HRTF");
        soundfreq = 44100;
        son3dapplying = true;
        resetsound(); // initsound -> son3daudioopen -> son3dapply at 44100 Hz
        son3dapplying = false;
        return;
    }
    if(!son3d_dsp_isopen() || son3d_dsp_freq() != freq)
    {
        // SDL_mixer calls the effects with at most one callback (soundbufferlen samples)
        if(!son3d_dsp_open(freq, max(soundbufferlen, 8192), maxchannels, err, sizeof(err)))
        {
            copystring(son3derreur, err);
            conoutf(CON_ERROR, "3D sound unavailable: %s", err);
            return;
        }
    }
    if(son3d == SON3D_CASQUE && !son3d_dsp_hashrtf())
    {
        copystring(son3derreur, "no HRTF at this sound frequency");
        conoutf(CON_ERROR, "3D sound unavailable: %s", son3derreur);
        return;
    }
    son3deffectif = son3d;
}

// Diagnostics: son3dmesure 1, play a while, then son3drapport (audio and main thread cost).
static long long mainticks = 0, raycount = 0, updates = 0;
static Uint64 statsstart = 0;
void son3dstatschanged();
VARF(son3dmesure, 0, 0, 1, son3dstatschanged());

void son3dstatschanged()
{
    if(nosound) return;
    Mix_UnregisterEffect(MIX_CHANNEL_POST, son3d_dsp_postmix);
    son3d_dsp_resetstats();
    mainticks = raycount = updates = 0;
    statsstart = SDL_GetPerformanceCounter();
    if(son3dmesure) Mix_RegisterEffect(MIX_CHANNEL_POST, son3d_dsp_postmix, NULL, NULL);
}

void son3drapport()
{
    son3dstats s;
    son3d_dsp_getstats(s);
    double f = double(s.freq > 0 ? s.freq : 1), secs = double(SDL_GetPerformanceCounter() - statsstart)/f;
    int freq = 0, chans = 0;
    Uint16 fmt = 0;
    Mix_QuerySpec(&freq, &fmt, &chans);
    double period = freq > 0 ? 1000.0*soundbufferlen/freq : 0;
    double mean = s.callbacks ? 1000.0*s.cbticks/f/s.callbacks : 0, mx = 1000.0*s.cbmax/f;
    conoutf("son3d rapport: mode %d occlusion %d, %.1f s, %d Hz tampon %d (%.1f ms), rappels %lld",
        son3deffectif, son3docclusion, secs, freq, soundbufferlen, period, s.callbacks);
    conoutf("son3d rapport: fil audio moyenne %.3f ms max %.3f ms (%.2f%% / %.2f%% du tampon), >10%%: %lld, voies max %d, appels %lld, trames %lld, genou %lld",
        mean, mx, period > 0 ? 100*mean/period : 0, period > 0 ? 100*mx/period : 0, s.cbover10, s.maxactive, s.calls, s.frames, s.clipped);
    conoutf("son3d rapport: fil principal %.3f ms/s (%.1f us par mise a jour, %lld mises a jour), %lld rayons (%.2f us/rayon compris)",
        secs > 0 ? 1000.0*mainticks/f/secs : 0, updates ? 1e6*mainticks/f/updates : 0, updates, raycount, raycount ? 1e6*mainticks/f/raycount : 0);
}
COMMAND(son3drapport, "");

static bool clearpath(const vec &from, const vec &to)
{
    vec dir = vec(to).sub(from);
    float dist = dir.magnitude();
    if(dist < 2) return true;
    dir.div(dist);
    raycount++;
    // mode 0: world geometry only; clip (invisible walls) and glass let the sound through
    return raycube(from, dir, dist, 0) >= dist - 1.5f;
}

static const vec son3doffsets[6] = { vec(1, 0, 0), vec(-1, 0, 0), vec(0, 1, 0), vec(0, -1, 0), vec(0, 0, 1), vec(0, 0, -1) };

// fraction of the source volume the listener can see, 1 = clear
static float occlusion(const vec &listener, const vec &src)
{
    // very close, or outside the map (nothing to hide behind there): no occlusion
    if(listener.dist(src) < 16 || !insideworld(src) || !insideworld(listener)) return 1;
    vec savedhit = hitsurface; // raycube writes the global hit normal
    float result = 1;
    // a spectator or an editor inside the geometry hears everything as before
    if(raycube(listener, vec(0, 0, 1), 1, 0) > 0 || raycube(listener, vec(0, 0, -1), 1, 0) > 0)
    {
        // The centre ray decides first: a source in plain sight is never muffled by more than 40%
        // (its feet behind a railing, a ledge), a source behind a wall keeps at most 60% when
        // part of it shows around the edge. The six points around the source smooth the change.
        bool centre = clearpath(listener, src);
        int valid = 0, clear = 0;
        if(son3dvolumesource > 0) loopi(6)
        {
            vec p = vec(son3doffsets[i]).mul(son3dvolumesource).add(src);
            if(!clearpath(src, p)) continue; // inside a wall or the floor: not part of the source
            valid++;
            if(clearpath(listener, p)) clear++;
        }
        float around = valid ? float(clear)/valid : (centre ? 1.0f : 0.0f);
        result = centre ? 0.6f + 0.4f*around : 0.6f*around;
    }
    hitsurface = savedhit;
    return result;
}

int son3dupdate(int chanid, son3dchan &s, const vec *loc)
{
    if(!son3deffectif || !loc) return SON3D_OFF;
    int mode = son3deffectif;
    bool occl = son3docclusion != 0;
    if((mode == SON3D_ENCEINTES && !occl) || chanid >= son3d_dsp_numchans()) return SON3D_OFF;
    Uint64 t0 = SDL_GetPerformanceCounter();

    son3dparams p;
    vec d = vec(*loc).sub(camera1->o);
    float dist = d.magnitude();
    if(dist > 1e-3f)
    {
        // Sauer: x left, y ahead, z up. Steam Audio listener space: +x right, +y up, -z ahead.
        vec fwd(camera1->yaw*RAD, camera1->pitch*RAD), up(camera1->yaw*RAD, (camera1->pitch+90)*RAD), left;
        left.cross(fwd, up);
        d.div(dist);
        p.dir[0] = -d.dot(left);
        p.dir[1] = d.dot(up);
        p.dir[2] = -d.dot(fwd);
    }
    else { p.dir[0] = p.dir[1] = 0; p.dir[2] = -1; }
    // a source on the listener has no direction: centred, then fully spatialised from 10 units
    p.blend = clamp((dist - 2)/8, 0.0f, 1.0f);

    if(occl)
    {
        if(s.raymillis < 0 || totalmillis - s.raymillis >= son3drayms)
        {
            s.occltarget = occlusion(camera1->o, *loc);
            if(s.raymillis < 0) s.occl = s.occltarget;
            s.raymillis = totalmillis;
        }
        if(s.lastmillis >= 0) s.occl += (s.occltarget - s.occl)*clamp((totalmillis - s.lastmillis)/60.0f, 0.0f, 1.0f);
        p.occlusion = s.occl;
    }
    else p.occlusion = 1;
    s.lastmillis = totalmillis;
    s.p = p;
    if(s.mode == mode) son3d_dsp_update(chanid, p);

    mainticks += SDL_GetPerformanceCounter() - t0;
    updates++;
    return mode;
}

void son3dstart(int chanid, son3dchan &s)
{
    if(son3d_dsp_attached(chanid)) Mix_UnregisterEffect(chanid, son3d_dsp_effect);
    son3d_dsp_prepare(chanid, s.mode, s.looping, son3docclusion != 0, s.p);
    if(!Mix_RegisterEffect(chanid, son3d_dsp_effect, son3d_dsp_done, NULL)) son3d_dsp_setattached(chanid, false);
}

void son3dsync(int chanid, son3dchan &s, int pan)
{
    int cur = son3d_dsp_mode(chanid);
    if(s.mode != cur)
    {
        if(cur) Mix_UnregisterEffect(chanid, son3d_dsp_effect);
        if(s.mode)
        {
            if(s.mode == SON3D_CASQUE) Mix_SetPanning(chanid, 255, 255); // the HRTF replaces the pan
            son3dstart(chanid, s);
        }
    }
    if(s.mode != SON3D_CASQUE) Mix_SetPanning(chanid, 255-pan, pan);
}

void son3daudioopen()
{
    if(son3d) son3dapply();
    if(son3dmesure) son3dstatschanged();
}

void son3daudioclose()
{
    son3d_dsp_close();
    son3deffectif = SON3D_OFF;
}
