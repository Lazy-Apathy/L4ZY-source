# L4ZY — source code

L4ZY is a modified client of **Cube 2: Sauerbraten** (2020 edition) for Windows, with:

- **hardware ray-traced lighting** (Vulkan `ray_query` compute next to the OpenGL renderer, shared through GL/Vulkan interop; not path tracing), with a *Classic* mode that keeps the original Sauerbraten lighting;
- **NVIDIA DLAA / DLSS Super Resolution** through the public NGX SDK, and **NVIDIA NRD** denoising of the RT sky visibility;
- an internal FP16 HDR pipeline and **native HDR output** (scRGB through the NVIDIA OpenGL/Direct3D 11 interop);
- optional **3D sound** with Valve **Steam Audio** (off by default);
- **local AI chat translation** and a **settings assistant**, served by a small Python service that runs `llama.cpp` on your own PC;
- a drag-and-drop HUD editor, kill feed, friends list, CTF flag timer, HD-aim demos, automatic match videos, settings search, and more;
- a launcher, a per-user installer and a component updater.

This is **not an official Sauerbraten release**. Binaries, release notes and the in-game update channel live at **<https://github.com/Lazy-Apathy/L4ZY>** ([Releases](https://github.com/Lazy-Apathy/L4ZY/releases)). The player documentation (features, settings, changelog) is in [`distrib/github/README.md`](distrib/github/README.md).

## Versions and releases

- **Binaries** (installers, update components, release notes) are on <https://github.com/Lazy-Apathy/L4ZY/releases>. Most players only need those.
- **From now on, every published version gets a source tag with the same number** in this repository (`v<version>`, for example `v2026.10.2.1` for `L4ZY-Setup-2026.10.2.1.exe`), linked from its release page. A tag never moves.
- **`v2026.9.26.4`** (the first public release) is available as a tag. Its game source was reconstructed after the release and checked: building it gives the released `sauerbraten.exe` byte for byte, apart from the PE timestamp and checksum. See the README of that tag for what it does and does not contain.
- **2026.9.28.1 has no source tag.** Before 30 September the full source tree was not under version control, and three game files (`main.cpp`, `menus.cpp`, `rendergl.cpp`) were edited the day after that release without a copy of their 2026.9.28.1 state being kept. Rather than publish a tree that might not match the binary, it is left out. Its successor will be tagged.
- **`main` is the unreleased development state** (after 2026.9.28.1): it contains changes that are not in any published installer yet (3D sound, low-latency mode, blue-noise sky rays, greyed-out RT option, fixes). Building `main` does not give the EXE of any release; check out a tag for that.

## Repository layout

| Path | What it is |
|---|---|
| `src/` | The game: the Sauerbraten engine and game code, modified. New code: `src/engine/hwrt/` (ray tracing, DLAA/DLSS and NRD front ends, velocity, temporal AA), `src/engine/hdrout.cpp` (HDR output), `son3d*.cpp` (3D sound), `latency.cpp`, `matchrec.cpp`, `hudlayout.cpp`, `src/fpsgame/translate.cpp` (chat translation client and settings assistant), `friends.cpp`, `demohd.cpp`. The original Sauerbraten `readme_source.txt` is kept. |
| `src/ngx_gateway/` | `sauer_ngx.dll`: a small MSVC DLL around the NVIDIA NGX (DLSS) SDK, with a plain C ABI (`src/include/ngx_gateway/sauer_ngx.h`). The game itself is built with MinGW and never links NVIDIA code; it loads this DLL at run time. |
| `src/nrd_gateway/` | `sauer_nrd.dll`: the same idea for NVIDIA NRD (`src/include/nrd_gateway/sauer_nrd.h`). |
| `src/engine/hwrt/shaders/` | GLSL compute shaders and their SPIR-V, embedded as C headers (committed, so building the game needs no shader compiler; regenerate with `tools/compile-hwrt-shaders.py`). |
| `src/include/` | Third-party headers the game compiles against: SDL2, zlib (from the Sauerbraten 2020 source), Vulkan and Vulkan Video (Khronos), Steam Audio `phonon` (Valve). |
| `data/` | Only the files L4ZY changes or adds on top of Sauerbraten 2020: `menus.cfg`, `glsl.cfg`, `defaults.cfg`, `hwrt.cfg`, and the settings-assistant tables. Everything else in `data/` and `packages/` is unmodified Sauerbraten. |
| `traduction.cfg` | The in-game chat-translation menu (CubeScript). |
| `distrib/launcher/` | `L4ZY.exe`: starts the game, then the translation service, and stops both on exit. |
| `distrib/service/` | The Python translation/assistant service and the updater (`updater.py`). No model and no user data. |
| `distrib/installer/` | The NSIS installer script. |
| `distrib/tools/`, `distrib/recipes/`, `distrib/pins.json` | Build and publishing tools: a recipe lists every file of a release; `sauerrt.ps1 release` builds the components, the manifest (SHA-256 of every file) and the installer. |
| `distrib/docs/licenses/` | Licence texts shipped with the game. |
| `third_party/` | Download scripts only (`fetch-*.ps1`). NVIDIA SDKs are never committed here. |
| `tools/` | Developer helpers (shader compiler wrapper, blue-noise generator, settings-assistant docs, shader checks). |

## Building on Windows

Everything below runs from a normal user account and installs nothing system-wide.

### 1. The game (`sauerbraten.exe`) — MinGW

1. Install [w64devkit](https://github.com/skeeto/w64devkit) (L4ZY is built with w64devkit 2.9.1, GCC 16.2.0). The scripts look in `%LOCALAPPDATA%\w64devkit\w64devkit\bin`, or in the folder named by the `W64DEVKIT` variable.
2. Create `bin64\` at the root of this repository and copy `SDL2.dll`, `SDL2_image.dll`, `SDL2_mixer.dll` and `zlib1.dll` from the `bin64\` folder of an official **Sauerbraten 2020** install (<http://sauerbraten.org/>). L4ZY uses these exact files; the game links against them.
3. Build:

   ```
   cd src
   make -j8 PLATFORM=MINGW64 client
   ```

   This writes `bin64\sauerbraten.exe` (or run `src\build-client.bat`, which also strips it). `distrib\tools\build-game.bat` is the build used for releases; it writes `distrib\.cache\game\sauerbraten.exe` instead.

The build is reproducible: rebuilding this tree with w64devkit 2.9.1 gave the developer's EXE byte for byte. Each release manifest lists the SHA-256 of its `sauerbraten.exe`, so you can compare.

### 2. Running what you built

The game needs the Sauerbraten 2020 data. Make a **copy** of a Sauerbraten 2020 folder (or of an L4ZY install), put your `bin64\sauerbraten.exe` and this repository's `data\` files and `traduction.cfg` over it, and start it with its own profile folder, for example `bin64\sauerbraten.exe -qC:\path\to\test-profile`. Without the DLLs below, the game still runs: ray tracing, DLSS, NRD and 3D sound then report themselves as unavailable and stay off.

### 3. Optional DLLs

These are separate DLLs, loaded only if present:

| Feature | Where the game looks | How to get it |
|---|---|---|
| DLAA / DLSS | `bin64\ngx-hdr\sauer_ngx.dll` + `nvngx_dlss.dll` | `powershell -File third_party\fetch-msvc.ps1` (portable MSVC + Windows SDK, needs Python), `powershell -File third_party\fetch-ngx-sdk.ps1` (clones the public NVIDIA DLSS SDK v310.9.1), then `src\ngx_gateway\build-dll-hdr.bat`. `nvngx.dll` itself comes with the NVIDIA driver. |
| NRD sky denoising | `bin64\nrd\sauer_nrd.dll` | `third_party\fetch-msvc.ps1`, `third_party\fetch-cmake.ps1`, `third_party\fetch-nrd.ps1` (NVIDIA NRD v4.17.3), then `src\nrd_gateway\build-dll.bat`. |
| 3D sound | `bin64\phonon.dll` | `powershell -File third_party\fetch-steam-audio.ps1` (official Steam Audio 4.8.1 release, SHA-256 checked). |

The NVIDIA SDKs are under the NVIDIA RTX SDKs licence, which does not allow redistributing their headers or libraries in source form, so this repository only contains the scripts that fetch them from NVIDIA's own GitHub repositories. You can also simply take the prebuilt DLLs from an installed L4ZY (`%LOCALAPPDATA%\Programs\L4ZY\bin64`).

### 4. Launcher, service, installer

- Launcher: `distrib\launcher\build.bat` (w64devkit) writes `distrib\launcher\L4ZY.exe`.
- Translation service: plain Python 3 in `distrib\service\` (`translate_server.py`), run by the launcher with a private embedded Python. Models are downloaded on request from the official Qwen repositories on Hugging Face, with SHA-256 checks.
- Full release (components, manifest, installer): see `distrib\tools\sauerrt.ps1` and the recipes in `distrib\recipes\`. It downloads its own pinned Python, `llama.cpp` and NSIS, each checked against `distrib\pins.json`.

## Linux

Not yet. The engine code still builds the way Sauerbraten does on Linux, but several L4ZY parts are Windows-only today: native HDR output goes through Direct3D 11 (`hdrout.cpp`), the launcher and installer are Windows programs, and the NGX/NRD gateways are built with MSVC. A Linux port is planned; until then, Linux builds are expected to fail.

## What the client connects to

For transparency, these are all the network destinations in this code:

- the normal Sauerbraten master server and game servers, exactly as vanilla Sauerbraten (`src/engine/serverbrowser.cpp`, `src/engine/server.cpp`);
- `127.0.0.1` only, between the game, the launcher and the local translation service (`src/fpsgame/translate.cpp`, `distrib/launcher/launcher.cpp`, `distrib/service/`);
- `raw.githubusercontent.com` and `github.com/Lazy-Apathy/L4ZY` releases, for updates (`distrib/service/updater.py`, `distrib/channels.json`);
- `huggingface.co/Qwen/...` when you click *Install* on a translation model (`distrib/service/model_manager.py`);
- an OpenAI-compatible address **only if you type one in** with your own API key (then chat leaves your PC; the key stays in `%LOCALAPPDATA%\L4ZY\translation\api.secrets`).

There is no telemetry and no analytics.

## Licence

L4ZY's own code is offered under the **zlib licence** (see [`LICENSE`](LICENSE)), the same licence as the Sauerbraten engine it is built on. As the zlib licence requires, this is an **altered** version of the Sauerbraten source and is marked as such; the original Sauerbraten copyright and licence notices are kept in `src/readme_source.txt` and `distrib/docs/licenses/sauerbraten-source-license.txt`.

Third-party code keeps its own licence: see [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md). Sauerbraten game media (maps, textures, models, sounds) are not in this repository and are not covered by the zlib licence.

## Credits

- **Cube 2: Sauerbraten** by Wouter van Oortmerssen, Lee Salzman, Mike Dysart, Robert Pointon, Quinton Reeves and contributors — the engine, the game and its media.
- **ENet** by Lee Salzman.
- **NVIDIA** DLSS (NGX) and NRD SDKs, used under the NVIDIA RTX SDKs licence. NVIDIA, DLSS and RTX are trademarks of NVIDIA Corporation. NVIDIA does not sponsor or endorse L4ZY.
- **Valve** Steam Audio (Apache 2.0). Valve does not sponsor or endorse L4ZY.
- **Khronos Group** Vulkan headers; **SDL** by Sam Lantinga and contributors; **zlib** by Jean-loup Gailly and Mark Adler.
- **llama.cpp** (ggml-org, MIT) and the **Qwen3** models (Alibaba Cloud, Apache 2.0) for chat translation; **Python** (PSF); **NSIS** for the installer.
