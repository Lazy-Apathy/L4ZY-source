# L4ZY features as patches

Every feature of L4ZY 2026.10.16.1 as its own patch on top of the **Cube 2: Sauerbraten 2020 Edition** source (the `src/` folder shipped with the 2020_11_29 release, plus `data/menus.cfg`, `data/glsl.cfg` and `data/defaults.cfg`). Applied in the order below, they give the L4ZY 2026.10.16.1 source (https://github.com/Lazy-Apathy/L4ZY-source/tree/v2026.10.16.1), with one change: real player names are removed from a few texts (the chat translator's instructions and examples, the Tiny Apa bot texts and their search keywords); instead, the translator is told which names seen in the game appear in the line it translates.

## Order and dependencies

| # | Patch | What it is | Applies on top of |
|---|---|---|---|
| 00 | [`00-build-fix.patch`](00-build-fix.patch) ([notes](00-build-fix.md)) | Build fix for current MinGW-w64 | the 2020 source |
| 01 | [`01-common.patch`](01-common.patch) ([notes](01-common.md)) | Common groundwork | `00-build-fix` (stands on its own) |
| 02 | [`02-menu-drawing.patch`](02-menu-drawing.patch) ([notes](02-menu-drawing.md)) | Menu drawing (look of the menus, scrolling, new menu controls) | `00-build-fix`, `01-common` (stands on its own) |
| 03 | [`03-safe-settings.patch`](03-safe-settings.patch) ([notes](03-safe-settings.md)) | Safer settings files | `00-build-fix`, `01-common` (stands on its own) |
| 04 | [`04-low-latency.patch`](04-low-latency.patch) ([notes](04-low-latency.md)) | Low latency | `00-build-fix`, `01-common` (stands on its own) |
| 05 | [`05-sound-mix.patch`](05-sound-mix.patch) ([notes](05-sound-mix.md)) | Sound mix | `00-build-fix`, `01-common` (stands on its own) |
| 06 | [`06-3d-sound.patch`](06-3d-sound.patch) ([notes](06-3d-sound.md)) | 3D sound (Steam Audio) | `00-build-fix`, `01-common`, `05-sound-mix` (stands on its own) |
| 07 | [`07-game-hud-and-demos.patch`](07-game-hud-and-demos.patch) ([notes](07-game-hud-and-demos.md)) | Game HUD, chat and demos: kill feed, react times, friends and colours, crosshair, CTF flag messages, F8/F10 windows, HUD editor, chat translation, HD-aim demos, client announce | all the patches before it |
| 08 | [`08-mouse-and-window.patch`](08-mouse-and-window.patch) ([notes](08-mouse-and-window.md)) | Mouse (DPI, cm/360) and window modes | all the patches before it |
| 09 | [`09-graphics.patch`](09-graphics.patch) ([notes](09-graphics.md)) | Graphics: ray-traced lighting, DLAA/DLSS/FSR, NRD, HDR output, ambient occlusion and FXAA | all the patches before it |
| 10 | [`10-weapon-trails.patch`](10-weapon-trails.patch) ([notes](10-weapon-trails.md)) | Weapon trail colours | all the patches before it |
| 11 | [`11-tiny-apa-bots.patch`](11-tiny-apa-bots.patch) ([notes](11-tiny-apa-bots.md)) | Tiny Apa bots | all the patches before it |
| 12 | [`12-scoreboard-and-spectating.patch`](12-scoreboard-and-spectating.patch) ([notes](12-scoreboard-and-spectating.md)) | Scoreboard columns, spectator list and Master menu | all the patches before it |
| 13 | [`13-match-videos.patch`](13-match-videos.patch) ([notes](13-match-videos.md)) | Automatic match videos (MP4) | all the patches before it |
| 14 | [`14-settings-search-and-assistant.patch`](14-settings-search-and-assistant.patch) ([notes](14-settings-search-and-assistant.md)) | Settings search and settings assistant (local AI) | all the patches before it |
| 15 | [`15-launcher-and-updates.patch`](15-launcher-and-updates.patch) ([notes](15-launcher-and-updates.md)) | Launcher, installer and updates | all the patches before it |
| 16 | [`16-menus.patch`](16-menus.patch) ([notes](16-menus.md)) | Menus | all the patches before it |

"Applies on top of" lists what a patch needs. "(stands on its own)": nothing else is needed, it was applied and compiled on top of those patches only. "all the patches before it": it calls code added by earlier patches, so the series is applied in order, as one stack. Two patches group several features (`09-graphics`, `07-game-hud-and-demos`) because their code calls each other everywhere; their notes explain why.

Each patch has a header with its title; `git apply` ignores it.

## How to apply

1. Take the Sauerbraten 2020 Edition source (`src/` and `data/` of the 2020_11_29 release).
2. From the root of that tree, apply `00-build-fix.patch`, then the patches you want, in the order of the table (each one together with the patches listed next to it):

   ```
   git apply patches/00-build-fix.patch
   git apply patches/<NN-name>.patch
   ```

   Use `git apply` (also outside a git repository): some patches add small PNG icons, which plain `patch` cannot do.
3. Some patches need third-party headers that are not copied here (see each patch's notes):
   - ray tracing: the Khronos **Vulkan-Headers** 1.4.313 (`include/vulkan` and `include/vk_video` into `src/include/`);
   - 3D sound: the **Steam Audio 4.8.1** headers (`phonon*.h` into `src/include/phonon/`), from the official Valve release;
   - DLSS/DLAA and NRD run through small DLLs built from the NVIDIA SDKs, and FSR through a DLL built from AMD FidelityFX: the build scripts (`src/ngx_gateway`, `src/nrd_gateway`, `src/fsr_gateway`) and `third_party/fetch-*.ps1` download the SDKs from their official sources. The game itself links none of them.

## How to build

Windows, MinGW-w64 (checked with w64devkit 2.9.1, GCC 16.2.0), from `src/`:

```
make -j8 PLATFORM=MINGW64 client
```

The `bin64/` DLLs come from a Sauerbraten 2020 install (see the L4ZY source README for the full list).

## How they were checked

- Each patch was applied with `git apply` and compiled with the patches listed next to it.
- The whole series, applied in order, compiles at every step, and gives the L4ZY 2026.10.16.1 source with the change above (line endings and the third-party headers above aside).
- They were compiled, not play-tested one by one: the tested game is the full L4ZY release.

## Licences

- Sauerbraten source: zlib licence (`src/readme_source.txt`). These patches are an altered version of it.
- Third-party components keep their own licences; they are listed in the L4ZY source repository (`THIRD-PARTY-NOTICES.md`).
