// son3d.h: optional 3D sound (Steam Audio) for positional sounds, see son3d.cpp.

#ifndef SON3D_H
#define SON3D_H

#include "son3d_dsp.h"

// Per sound channel state, kept in soundchannel (sound.cpp).
struct son3dchan
{
    int mode;            // mode applied to the channel, SON3D_OFF when it is not processed
    bool looping;        // the sound loops (SDL_mixer splits it at the loop point)
    int raymillis;       // last occlusion rays, -1 before the first
    int lastmillis;      // last update (smoothing)
    float occl, occltarget;
    son3dparams p;       // last parameters sent to the audio thread

    void reset() { mode = SON3D_OFF; looping = false; raymillis = lastmillis = -1; occl = occltarget = 1; }
};

extern int son3deffectif; // effective mode for the whole game, SON3D_OFF when 3D sound is off or unavailable

// Main thread. Returns the mode of this channel for this frame (SON3D_OFF without a position).
extern int son3dupdate(int chanid, son3dchan &s, const vec *loc);
// Before Mix_PlayChannel: registers the effect on the still silent channel.
extern void son3dstart(int chanid, son3dchan &s);
// Replaces Mix_SetPanning in syncchannel when the channel is or was processed.
extern void son3dsync(int chanid, son3dchan &s, int pan);
// Mixer opened (end of initsound) / closed (after Mix_CloseAudio).
extern void son3daudioopen();
extern void son3daudioclose();

#endif
