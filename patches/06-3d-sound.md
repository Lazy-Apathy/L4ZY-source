# 3D sound (Steam Audio)

Patch `06-3d-sound.patch`, from the L4ZY 2026.10.16.1 source.

The Steam Audio headers are not included: put the `phonon*.h` headers of the official Steam Audio 4.8.1 release into `src/include/phonon/`. `third_party/fetch-steam-audio.ps1` downloads `phonon.dll` from the same release.

- **Headphones (HRTF):** you hear whether a sound is ahead or behind, above or below.
- **Speakers:** the usual left/right sound.
- **Occlusion:** with Headphones or Speakers, walls between you and a sound muffle it (it is never muted).
- Your own sounds, menus, announcements and music are never changed.
- Off by default: nothing changes until you pick Headphones or Speakers.

**Menu:** Options → Sound → *3D Sound*

| Setting | Values | Default |
|---|---|---|
| `son3d` | `0` off, `1` headphones (HRTF), `2` speakers | `0` |
| `son3docclusion` | walls muffle sounds (with headphones or speakers) | `1` |

- Headphones set the sound frequency to 44100 Hz.
- If Steam Audio (`phonon.dll`) cannot load, Headphones and Speakers are greyed out and the menu says why.

## Applying

Apply first: `00-build-fix`, `01-common`, `05-sound-mix`. Nothing else is needed: it stands on its own on top of them.

```
git apply patches/06-3d-sound.patch
```

- Checked: applied on top of `00-build-fix`, `01-common`, `05-sound-mix` only, and compiled.


The menu paths above are those of the full L4ZY menus (patch `16-menus`). Without that patch, the settings and commands exist all the same: use them from the console or your own `.cfg` files.


## Files

- `distrib/docs/licenses/steam-audio/LICENSE.txt`
- `distrib/docs/licenses/steam-audio/THIRDPARTY.md`
- `src/Makefile`
- `src/engine/son3d.cpp`
- `src/engine/son3d.h`
- `src/engine/son3d_dsp.cpp`
- `src/engine/son3d_dsp.h`
- `src/engine/sound.cpp`
- `third_party/fetch-steam-audio.ps1`
