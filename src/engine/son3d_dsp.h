// son3d_dsp.h: Steam Audio (phonon.dll) processing of one SDL_mixer channel.
// No engine dependency, so that the offline bench (shots/son3d-lot1/outils/banc) runs the same code.

#ifndef SON3D_DSP_H
#define SON3D_DSP_H

#include <stddef.h>

enum { SON3D_OFF = 0, SON3D_CASQUE = 1, SON3D_ENCEINTES = 2 };

struct son3dparams
{
    float dir[3];     // unit vector listener -> source, Steam Audio listener space: +x right, +y up, -z ahead
    float blend;      // 0 = centred (source on the listener), 1 = fully spatialised
    float occlusion;  // 1 = clear line of sight, 0 = every sample point blocked
};

struct son3dstats
{
    long long callbacks;      // audio callbacks seen by the post-mix hook
    long long cbticks, cbmax; // processing time added per callback (performance counter ticks), sum and max
    long long calls, frames;  // effect calls, Steam Audio frames
    long long clipped;        // output samples above 80% of full scale (soft knee)
    long long cbover10;       // callbacks where the added time exceeded 10% of the buffer period
    int maxactive;            // most spatialised channels processed in one callback
    long long freq;           // performance counter frequency
};

// Loads phonon.dll from an explicit path and creates the Steam Audio context. Idempotent.
bool son3d_dsp_load(const char *dllpath, char *err, size_t errlen);
void son3d_dsp_unload();
bool son3d_dsp_loaded();

// Creates the HRTF and the per-channel effects for the opened mixer. Audio must not use the effects yet.
bool son3d_dsp_open(int freq, int buffersamples, int numchans, char *err, size_t errlen);
void son3d_dsp_close();
bool son3d_dsp_isopen();
bool son3d_dsp_hashrtf(); // false at 22050 Hz: only SON3D_ENCEINTES then
int son3d_dsp_framesize();
int son3d_dsp_freq();
int son3d_dsp_numchans();

// Tuning shared by every channel (main thread, takes effect on the next frame).
void son3d_dsp_settransmission(float low, float mid, float high);
void son3d_dsp_sethrtfgain(float gain);
void son3d_dsp_setbilinear(bool on);
void son3d_dsp_setramp(int frames); // 0 = whole callback
void son3d_dsp_setframesize(int frames); // 0 = automatic; takes effect at the next son3d_dsp_open

// Main thread. Resets the channel state; call only while the effect is not registered with SDL_mixer.
void son3d_dsp_prepare(int chan, int mode, bool looping, bool occlusion, const son3dparams &p);
// Main thread, any time: parameters picked up by the next audio callback.
void son3d_dsp_update(int chan, const son3dparams &p);
bool son3d_dsp_attached(int chan);
void son3d_dsp_setattached(int chan, bool on);
int son3d_dsp_mode(int chan);

// SDL_mixer Mix_EffectFunc_t / Mix_EffectDone_t (16-bit stereo interleaved).
void son3d_dsp_effect(int chan, void *stream, int len, void *udata);
void son3d_dsp_done(int chan, void *udata);
// Call once per audio callback after all channels (Mix_RegisterEffect on MIX_CHANNEL_POST).
void son3d_dsp_postmix(int chan, void *stream, int len, void *udata);

void son3d_dsp_getstats(son3dstats &s);
void son3d_dsp_resetstats();
const char *son3d_dsp_lastlog();

#endif
