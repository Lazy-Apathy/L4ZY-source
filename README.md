# L4ZY 2026.9.26.4 — source code

This tree is the source code of **L4ZY 2026.9.26.4**, the first public release of L4ZY, a modified **Cube 2: Sauerbraten** (2020 edition) client for Windows with hardware ray-traced lighting (Vulkan `ray_query`), NVIDIA DLAA/DLSS and NRD, native HDR output and local AI chat translation.

Binaries: <https://github.com/Lazy-Apathy/L4ZY/releases/tag/v2026.9.26.4>. This is not an official Sauerbraten release.

## What exactly is here

- `src/`: the game (`sauerbraten.exe`) exactly as built for 2026.9.26.4, plus the NGX and NRD gateway sources. This tree was reconstructed after the release from the developer's build folder and checked: building it gives the released `sauerbraten.exe` (SHA-256 `b67cb069001ec058e235dd554cc875544c810320195d2b6caf8bddd2ae7094d4`) byte for byte, except the 4-byte PE timestamp and the PE checksum that the linker writes at build time.
- `data/glsl.cfg` and `data/menus.cfg`: the two data files this release replaced, identical to the released ones (their SHA-256 are in `distrib/recipes/2026.9.26.4-publique.json`, the recipe the release was built from).
- `third_party/`: scripts that fetch the NVIDIA SDKs (not redistributable in source form).

Not here, because their exact 2026.9.26.4 state could not be recovered: the launcher (`L4ZY.exe`), the translation service and updater (Python), the translation menu (`traduction.cfg`) and the other data files. The Python service, the menus and the data files ship as plain text inside the 2026.9.26.4 installer, so that installer is their exact source. Later versions have all of these in this repository.

## Building the game

1. Install [w64devkit](https://github.com/skeeto/w64devkit) (w64devkit 2.9.1, GCC 16.2.0 was used).
2. Create `bin64\` at the repository root with `SDL2.dll`, `SDL2_image.dll`, `SDL2_mixer.dll` and `zlib1.dll` from the `bin64\` folder of an official Sauerbraten 2020 install.
3. `cd src`, then `make -j8 PLATFORM=MINGW64 client`, then `strip ..\bin64\sauerbraten.exe`.

Optional DLLs (DLSS, NRD): see `third_party\fetch-*.ps1`, `src\ngx_gateway\build-dll-hdr.bat` and `src\nrd_gateway\build-dll.bat`. Windows only; Linux is not supported yet.

## Licence and credits

L4ZY's own code: zlib licence (`LICENSE`), like Sauerbraten. This is an altered version of the Sauerbraten source; the original notice is in `src/readme_source.txt`. Third-party code: `THIRD-PARTY-NOTICES.md`. Sauerbraten by Wouter van Oortmerssen, Lee Salzman, Mike Dysart, Robert Pointon, Quinton Reeves and contributors; ENet by Lee Salzman; NVIDIA DLSS/NGX and NRD SDKs (NVIDIA RTX SDKs licence; NVIDIA does not sponsor or endorse L4ZY); Khronos Vulkan headers; SDL; zlib.
