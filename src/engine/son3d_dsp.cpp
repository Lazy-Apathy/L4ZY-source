// son3d_dsp.cpp: Steam Audio (phonon.dll) processing of one SDL_mixer channel.
//
// phonon.dll is loaded at runtime from an explicit path, only when 3D sound is switched on:
// with 3D sound off the game never loads it and never registers an effect.
//
// SDL_mixer hands every channel to its effects in the device format (16-bit stereo) before
// applying the channel volume, so the distance attenuation of sound.cpp is kept as is and the
// effect only replaces the stereo panning (headphones) or adds occlusion (speakers).
//
// Steam Audio works on fixed frames. A one-shot sound always starts at the beginning of an audio
// callback and every call but the last covers the whole callback, so frames are processed in
// place without added delay (the last partial frame is zero padded). A looping sound is split at
// the loop point by SDL_mixer, so it goes through a FIFO with a constant delay of one frame.

#ifdef WIN32
#include <windows.h>
typedef HMODULE libhandle;
#define LIBLOAD(path) LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH)
#define LIBSYM(lib, name) GetProcAddress(lib, name)
#define LIBFREE(lib) FreeLibrary(lib)
#else
#include <dlfcn.h>
typedef void *libhandle;
#define LIBLOAD(path) dlopen(path, RTLD_NOW | RTLD_LOCAL)
#define LIBSYM(lib, name) dlsym(lib, name)
#define LIBFREE(lib) dlclose(lib)
#endif
#include <string.h>
#include <math.h>
#include <atomic>
#include "SDL.h"
#include "phonon/phonon.h"
#include "son3d_dsp.h"

#define SON3D_FUNCS \
    F(iplContextCreate) F(iplContextRelease) \
    F(iplHRTFCreate) F(iplHRTFRelease) \
    F(iplBinauralEffectCreate) F(iplBinauralEffectRelease) F(iplBinauralEffectReset) F(iplBinauralEffectApply) \
    F(iplDirectEffectCreate) F(iplDirectEffectRelease) F(iplDirectEffectReset) F(iplDirectEffectApply)

#define F(name) static decltype(&::name) p_##name = NULL;
SON3D_FUNCS
#undef F

enum { MAXFRAME = 512 };

struct chanstate
{
    IPLBinauralEffect binaural;
    IPLDirectEffect directmono, directstereo;
    int mode;
    bool looping, occlusion, first;
    SDL_SpinLock lock;
    son3dparams next, cur;
    std::atomic<int> attached;
    float *inacc[2], *outq[2];
    int inlen, outlen;
};

static libhandle phonon = NULL;
static IPLContext context = NULL;
static IPLHRTF hrtf = NULL;
static chanstate *chans = NULL;
static int samplerate = 0, framesize = 0, maxsamples = 0, numchannels = 0;
static float *scratch[2] = { NULL, NULL };
static float frmono[MAXFRAME], frmid[MAXFRAME], frout[2][MAXFRAME];
static char lastlog[256] = "";

// HRTF make-up gain: same average loudness as the current panning on the 206 game sounds
// (bench 2c, horizontal directions every 30 degrees, 30/09/2026).
static std::atomic<float> transmission[3], hrtfgain(0.758f);
static std::atomic<int> bilinear(1), rampframes(0);
static int frameoverride = 0;

static std::atomic<long long> st_callbacks(0), st_cbticks(0), st_cbmax(0), st_calls(0), st_frames(0), st_clipped(0), st_cbover10(0);
static std::atomic<int> st_maxactive(0);
static long long cbacc = 0; // audio thread only
static int cbactive = 0;    // audio thread only

static void IPLCALL logfunc(IPLLogLevel level, const char *msg)
{
    if(level != IPL_LOGLEVEL_ERROR && level != IPL_LOGLEVEL_WARNING) return;
    strncpy(lastlog, msg, sizeof(lastlog)-1);
    lastlog[sizeof(lastlog)-1] = '\0';
}

const char *son3d_dsp_lastlog() { return lastlog; }

static void seterr(char *err, size_t errlen, const char *msg)
{
    if(!err || !errlen) return;
    strncpy(err, msg, errlen-1);
    err[errlen-1] = '\0';
}

bool son3d_dsp_loaded() { return context != NULL; }

bool son3d_dsp_load(const char *dllpath, char *err, size_t errlen)
{
    if(context) return true;
    if(!phonon)
    {
        phonon = LIBLOAD(dllpath);
        if(!phonon) { seterr(err, errlen, "phonon.dll introuvable"); return false; }
    }
    bool ok = true;
#define F(name) if(!(p_##name = (decltype(&::name))LIBSYM(phonon, #name))) ok = false;
    SON3D_FUNCS
#undef F
    if(!ok) { seterr(err, errlen, "phonon.dll incomplet"); son3d_dsp_unload(); return false; }

    IPLContextSettings settings;
    memset(&settings, 0, sizeof(settings));
    settings.version = STEAMAUDIO_VERSION;
    settings.logCallback = logfunc;
    settings.simdLevel = IPL_SIMDLEVEL_AVX2;
    if(p_iplContextCreate(&settings, &context) != IPL_STATUS_SUCCESS)
    {
        context = NULL;
        seterr(err, errlen, "version de phonon.dll incompatible");
        son3d_dsp_unload();
        return false;
    }
    transmission[0] = 0.55f;
    transmission[1] = 0.25f;
    transmission[2] = 0.12f;
    return true;
}

void son3d_dsp_unload()
{
    son3d_dsp_close();
    if(context) { p_iplContextRelease(&context); context = NULL; }
    if(phonon) { LIBFREE(phonon); phonon = NULL; }
#define F(name) p_##name = NULL;
    SON3D_FUNCS
#undef F
}

bool son3d_dsp_isopen() { return chans != NULL; }
bool son3d_dsp_hashrtf() { return hrtf != NULL; }
int son3d_dsp_framesize() { return framesize; }
int son3d_dsp_freq() { return samplerate; }
int son3d_dsp_numchans() { return numchannels; }

void son3d_dsp_close()
{
    if(chans)
    {
        for(int i = 0; i < numchannels; i++)
        {
            chanstate &c = chans[i];
            if(c.binaural) p_iplBinauralEffectRelease(&c.binaural);
            if(c.directmono) p_iplDirectEffectRelease(&c.directmono);
            if(c.directstereo) p_iplDirectEffectRelease(&c.directstereo);
            for(int k = 0; k < 2; k++) { delete[] c.inacc[k]; delete[] c.outq[k]; }
        }
        delete[] chans;
        chans = NULL;
    }
    for(int k = 0; k < 2; k++) { delete[] scratch[k]; scratch[k] = NULL; }
    if(hrtf) { p_iplHRTFRelease(&hrtf); hrtf = NULL; }
    numchannels = framesize = maxsamples = samplerate = 0;
}

bool son3d_dsp_open(int freq, int buffersamples, int numchans, char *err, size_t errlen)
{
    son3d_dsp_close();
    if(!context) { seterr(err, errlen, "Steam Audio non charge"); return false; }
    // About 2.9 ms per frame (128 samples at 44100 Hz): direction changes are applied in steps
    // as fine as the current panning, for about 1.3 ms of HRTF delay (bench, 30/09/2026).
    // The frame must divide the callback length so that one-shot sounds get no added delay.
    int deffs = freq >= 32000 ? 128 : 64, fs = frameoverride > 0 ? frameoverride : deffs;
    while(fs > 16 && buffersamples % fs) fs /= 2;
    if(buffersamples % fs) fs = deffs;
    samplerate = freq;
    framesize = fs;
    maxsamples = buffersamples;

    IPLAudioSettings audio = { samplerate, framesize };
    IPLHRTFSettings hs;
    memset(&hs, 0, sizeof(hs));
    hs.type = IPL_HRTFTYPE_DEFAULT;
    hs.volume = 1.0f;
    hs.normType = IPL_HRTFNORMTYPE_RMS;
    // The built-in HRTF only exists at 24000, 44100 and 48000 Hz: without it only the
    // speakers mode (occlusion) is available.
    if(p_iplHRTFCreate(context, &audio, &hs, &hrtf) != IPL_STATUS_SUCCESS) hrtf = NULL;

    numchannels = numchans;
    chans = new chanstate[numchannels];
    bool ok = true;
    for(int i = 0; i < numchannels; i++)
    {
        chanstate &c = chans[i];
        c.binaural = NULL;
        c.directmono = c.directstereo = NULL;
        c.mode = SON3D_OFF;
        c.looping = c.occlusion = false;
        c.first = true;
        c.lock = 0;
        memset(&c.next, 0, sizeof(c.next));
        c.cur = c.next;
        c.attached = 0;
        c.inlen = c.outlen = 0;
        for(int k = 0; k < 2; k++)
        {
            c.inacc[k] = new float[framesize];
            c.outq[k] = new float[2*framesize + maxsamples];
        }
        IPLBinauralEffectSettings bs = { hrtf };
        IPLDirectEffectSettings ms = { 1 }, ss = { 2 };
        if(hrtf && p_iplBinauralEffectCreate(context, &audio, &bs, &c.binaural) != IPL_STATUS_SUCCESS) { c.binaural = NULL; ok = false; }
        if(p_iplDirectEffectCreate(context, &audio, &ms, &c.directmono) != IPL_STATUS_SUCCESS) { c.directmono = NULL; ok = false; }
        if(p_iplDirectEffectCreate(context, &audio, &ss, &c.directstereo) != IPL_STATUS_SUCCESS) { c.directstereo = NULL; ok = false; }
    }
    for(int k = 0; k < 2; k++) scratch[k] = new float[maxsamples];
    if(!ok)
    {
        seterr(err, errlen, "effets Steam Audio indisponibles");
        son3d_dsp_close();
        return false;
    }
    return true;
}

void son3d_dsp_settransmission(float low, float mid, float high)
{
    transmission[0] = low;
    transmission[1] = mid;
    transmission[2] = high;
}

void son3d_dsp_sethrtfgain(float gain) { hrtfgain = gain; }
void son3d_dsp_setbilinear(bool on) { bilinear = on ? 1 : 0; }
void son3d_dsp_setramp(int frames) { rampframes = frames; }
void son3d_dsp_setframesize(int frames) { frameoverride = frames > 0 && frames <= MAXFRAME ? frames : 0; }

void son3d_dsp_prepare(int chanid, int mode, bool looping, bool occlusion, const son3dparams &p)
{
    if(!chans || chanid < 0 || chanid >= numchannels) return;
    chanstate &c = chans[chanid];
    c.mode = mode;
    c.looping = looping;
    c.occlusion = occlusion;
    if(c.binaural) p_iplBinauralEffectReset(c.binaural);
    if(mode == SON3D_CASQUE && !c.binaural) mode = c.mode = SON3D_ENCEINTES;
    p_iplDirectEffectReset(c.directmono);
    p_iplDirectEffectReset(c.directstereo);
    c.inlen = 0;
    c.outlen = 0;
    if(looping)
    {
        memset(c.outq[0], 0, framesize*sizeof(float));
        memset(c.outq[1], 0, framesize*sizeof(float));
        c.outlen = framesize;
    }
    SDL_AtomicLock(&c.lock);
    c.next = p;
    SDL_AtomicUnlock(&c.lock);
    c.cur = p;
    c.first = true;
    c.attached = 1;
}

void son3d_dsp_update(int chanid, const son3dparams &p)
{
    if(!chans || chanid < 0 || chanid >= numchannels) return;
    chanstate &c = chans[chanid];
    SDL_AtomicLock(&c.lock);
    c.next = p;
    SDL_AtomicUnlock(&c.lock);
}

bool son3d_dsp_attached(int chanid)
{
    return chans && chanid >= 0 && chanid < numchannels && chans[chanid].attached;
}

void son3d_dsp_setattached(int chanid, bool on)
{
    if(chans && chanid >= 0 && chanid < numchannels) chans[chanid].attached = on ? 1 : 0;
}

int son3d_dsp_mode(int chanid)
{
    return chans && chanid >= 0 && chanid < numchannels && chans[chanid].attached ? chans[chanid].mode : SON3D_OFF;
}

void son3d_dsp_done(int chanid, void *udata)
{
    son3d_dsp_setattached(chanid, false);
}

static void lerpparams(const son3dparams &a, const son3dparams &b, float t, son3dparams &o)
{
    float d[3], len = 0;
    for(int k = 0; k < 3; k++) { d[k] = a.dir[k] + (b.dir[k] - a.dir[k])*t; len += d[k]*d[k]; }
    len = sqrtf(len);
    if(len > 1e-3f) for(int k = 0; k < 3; k++) o.dir[k] = d[k]/len;
    else for(int k = 0; k < 3; k++) o.dir[k] = b.dir[k];
    o.blend = a.blend + (b.blend - a.blend)*t;
    o.occlusion = a.occlusion + (b.occlusion - a.occlusion)*t;
}

static void runframe(chanstate &c, const son3dparams &p, const float *inl, const float *inr, float *outl, float *outr)
{
    IPLDirectEffectParams dp;
    memset(&dp, 0, sizeof(dp));
    dp.flags = (IPLDirectEffectFlags)(IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION | IPL_DIRECTEFFECTFLAGS_APPLYTRANSMISSION);
    dp.transmissionType = IPL_TRANSMISSIONTYPE_FREQDEPENDENT;
    dp.distanceAttenuation = 1;
    dp.directivity = 1;
    dp.occlusion = p.occlusion;
    for(int k = 0; k < 3; k++) { dp.airAbsorption[k] = 1; dp.transmission[k] = transmission[k]; }

    if(c.mode == SON3D_CASQUE)
    {
        for(int i = 0; i < framesize; i++) frmono[i] = 0.5f*(inl[i] + inr[i]);
        float *src = frmono;
        if(c.occlusion)
        {
            float *din[1] = { frmono }, *dout[1] = { frmid };
            IPLAudioBuffer bin = { 1, framesize, din }, bout = { 1, framesize, dout };
            p_iplDirectEffectApply(c.directmono, &dp, &bin, &bout);
            src = frmid;
        }
        IPLBinauralEffectParams bp;
        memset(&bp, 0, sizeof(bp));
        bp.direction.x = p.dir[0];
        bp.direction.y = p.dir[1];
        bp.direction.z = p.dir[2];
        bp.interpolation = bilinear ? IPL_HRTFINTERPOLATION_BILINEAR : IPL_HRTFINTERPOLATION_NEAREST;
        bp.spatialBlend = p.blend;
        bp.hrtf = hrtf;
        float *bin1[1] = { src }, *bout2[2] = { frout[0], frout[1] };
        IPLAudioBuffer bin = { 1, framesize, bin1 }, bout = { 2, framesize, bout2 };
        p_iplBinauralEffectApply(c.binaural, &bp, &bin, &bout);
        float g = hrtfgain;
        for(int i = 0; i < framesize; i++) { outl[i] = g*frout[0][i]; outr[i] = g*frout[1][i]; }
    }
    else
    {
        float *din[2] = { (float *)inl, (float *)inr }, *dout[2] = { outl, outr };
        IPLAudioBuffer bin = { 2, framesize, din }, bout = { 2, framesize, dout };
        p_iplDirectEffectApply(c.directstereo, &dp, &bin, &bout);
    }
}

void son3d_dsp_effect(int chanid, void *stream, int len, void *udata)
{
    if(!chans || chanid < 0 || chanid >= numchannels) return;
    chanstate &c = chans[chanid];
    int n = len/4;
    if(n <= 0 || n > maxsamples || !c.attached) return;
    Uint64 t0 = SDL_GetPerformanceCounter();

    Sint16 *s = (Sint16 *)stream;
    for(int i = 0; i < n; i++)
    {
        scratch[0][i] = s[2*i]*(1.0f/32768.0f);
        scratch[1][i] = s[2*i+1]*(1.0f/32768.0f);
    }

    son3dparams target;
    SDL_AtomicLock(&c.lock);
    target = c.next;
    SDL_AtomicUnlock(&c.lock);
    if(c.first) { c.cur = target; c.first = false; }
    son3dparams from = c.cur, p;

    int nfr = (c.inlen + n)/framesize, pad = c.outlen + nfr*framesize < n ? 1 : 0, total = nfr + pad, k = 0, ip = 0;
    for(;;)
    {
        int take = framesize - c.inlen;
        if(take > n - ip) take = n - ip;
        memcpy(&c.inacc[0][c.inlen], &scratch[0][ip], take*sizeof(float));
        memcpy(&c.inacc[1][c.inlen], &scratch[1][ip], take*sizeof(float));
        c.inlen += take;
        ip += take;
        if(c.inlen < framesize)
        {
            if(ip < n || !pad) break;
            // last call of a one-shot sound: zero pad the partial frame
            memset(&c.inacc[0][c.inlen], 0, (framesize - c.inlen)*sizeof(float));
            memset(&c.inacc[1][c.inlen], 0, (framesize - c.inlen)*sizeof(float));
            pad = 0;
        }
        // direction, blend and occlusion glide to the new value over the frames of the callback
        // (or over rampframes frames when set), instead of jumping once per callback
        int ramp = rampframes > 0 && rampframes < total ? int(rampframes) : total;
        lerpparams(from, target, k+1 >= ramp ? 1.0f : float(k+1)/ramp, p);
        runframe(c, p, c.inacc[0], c.inacc[1], &c.outq[0][c.outlen], &c.outq[1][c.outlen]);
        c.outlen += framesize;
        c.inlen = 0;
        k++;
        if(ip >= n && !pad) break;
    }
    c.cur = target;

    int clipped = 0;
    int avail = c.outlen < n ? c.outlen : n;
    for(int i = 0; i < avail; i++)
    {
        for(int ch = 0; ch < 2; ch++)
        {
            // The HRTF can lift the near ear above full scale on the loudest sounds (bench:
            // 0.004% of samples): soft knee above 80% instead of a hard clip.
            float v = c.outq[ch][i], a = fabsf(v);
            if(a > 0.8f)
            {
                a = 0.8f + 0.2f*tanhf((a - 0.8f)*5.0f);
                v = v < 0 ? -a : a;
                clipped++;
            }
            v *= 32768.0f;
            int iv = int(v >= 0 ? v + 0.5f : v - 0.5f);
            if(iv > 32767) iv = 32767;
            else if(iv < -32768) iv = -32768;
            s[2*i+ch] = Sint16(iv);
        }
    }
    for(int i = avail; i < n; i++) s[2*i] = s[2*i+1] = 0;
    c.outlen -= avail;
    if(c.outlen > 0)
    {
        memmove(c.outq[0], &c.outq[0][avail], c.outlen*sizeof(float));
        memmove(c.outq[1], &c.outq[1][avail], c.outlen*sizeof(float));
    }

    Uint64 t1 = SDL_GetPerformanceCounter();
    cbacc += (long long)(t1 - t0);
    cbactive++;
    st_calls++;
    st_frames += k;
    if(clipped) st_clipped += clipped;
}

void son3d_dsp_postmix(int chan, void *stream, int len, void *udata)
{
    long long period = (long long)SDL_GetPerformanceFrequency() * (len/4) / (samplerate > 0 ? samplerate : 22050);
    st_callbacks++;
    st_cbticks += cbacc;
    if(cbacc > st_cbmax) st_cbmax = cbacc;
    if(cbacc*10 > period) st_cbover10++;
    if(cbactive > st_maxactive) st_maxactive = cbactive;
    cbacc = 0;
    cbactive = 0;
}

void son3d_dsp_getstats(son3dstats &s)
{
    s.callbacks = st_callbacks;
    s.cbticks = st_cbticks;
    s.cbmax = st_cbmax;
    s.calls = st_calls;
    s.frames = st_frames;
    s.clipped = st_clipped;
    s.cbover10 = st_cbover10;
    s.maxactive = st_maxactive;
    s.freq = (long long)SDL_GetPerformanceFrequency();
}

void son3d_dsp_resetstats()
{
    st_callbacks = 0; st_cbticks = 0; st_cbmax = 0; st_calls = 0; st_frames = 0; st_clipped = 0; st_cbover10 = 0;
    st_maxactive = 0;
}
