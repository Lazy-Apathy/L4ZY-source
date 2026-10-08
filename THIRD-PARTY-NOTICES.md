# Third-party code in this repository

| Component | Files | Licence | Notes |
|---|---|---|---|
| Cube 2: Sauerbraten (2020) | `src/` (modified), `data/*.cfg` (modified) | zlib | (c) 2001-2020 W. van Oortmerssen, L. Salzman, M. Dysart, R. Pointon, Q. Reeves. Altered version. See `src/readme_source.txt`. |
| ENet | `src/enet/` | MIT-style | (c) 2002-2020 Lee Salzman. See `src/enet/LICENSE`. |
| SDL 2.0.12, SDL_image, SDL_mixer headers | `src/include/SDL*.h`, `begin_code.h`, `close_code.h` | zlib | As shipped in the Sauerbraten 2020 source. |
| zlib 1.2.5 headers | `src/include/zlib.h`, `zconf.h` | zlib | (c) Jean-loup Gailly and Mark Adler. |
| Vulkan headers (VK_HEADER_VERSION 313) | `src/include/vulkan/`, `src/include/vk_video/` | Apache-2.0 (C headers); Apache-2.0 OR MIT (`*.hpp`, `vulkan.cppm`) | (c) 2015-2025 The Khronos Group Inc. Unmodified. Apache-2.0 text: `distrib/docs/licenses/steam-audio/LICENSE.txt` (same text). |
| Steam Audio 4.8.1 headers | `src/include/phonon/` | Apache-2.0 | (c) 2017-2023 Valve Corporation. Unmodified. `phonon.dll` itself is downloaded by `third_party/fetch-steam-audio.ps1`. |
| NVIDIA Streamline 2.12 matrix helpers (`sl_matrix_helpers.h`) | `src/engine/hwrt/dlaa.cpp`: functions `slrowmul` and `slrowinvert`, in tags v2026.9.26.4 to v2026.10.8.1 | MIT | (c) 2022-2023 NVIDIA CORPORATION. Used only by a diagnostic self-test. Replaced by new code in later versions. Licence text below. |

Not included, fetched by scripts in `third_party/`:

| Component | Licence | Why it is not here |
|---|---|---|
| NVIDIA DLSS SDK 310.9.1 (NGX headers, `nvsdk_ngx_s.lib`, `nvngx_dlss.dll`) | NVIDIA RTX SDKs licence | Redistribution only "as incorporated in object code format into a software application"; headers are marked `LicenseRef-NvidiaProprietary`. |
| NVIDIA NRD 4.17.3 | NVIDIA RTX SDKs licence | Same. |
| Portable MSVC + Windows SDK, CMake | Microsoft / BSD-3-Clause | Toolchains, downloaded on demand. |

The binaries an L4ZY installation ships (SDL DLLs and codecs, NVIDIA DLLs, Steam Audio, Python, llama.cpp, Visual C++ runtime) are listed with their licences in `distrib/docs/licenses/THIRD-PARTY-NOTICES.txt`.

## MIT licence (NVIDIA Streamline matrix helpers)

Copyright (c) 2022-2023 NVIDIA CORPORATION. All rights reserved

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
