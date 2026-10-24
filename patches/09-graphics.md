# Graphics: ray-traced lighting, DLAA/DLSS/FSR, NRD, HDR output, ambient occlusion and FXAA

Patch `09-graphics.patch`, from the L4ZY 2026.10.16.1 source.

Ray tracing, DLAA/DLSS/FSR, NRD, the HDR pipeline and the classic-lighting look (ambient occlusion, FXAA, own shadow, water waves) come as one patch: they share the frame-drawing code, which was rewritten for all of them together. Splitting them would mean rewriting that code, not just cutting the patch.

NVIDIA DLSS/NGX and NRD are used through small DLLs (`src/ngx_gateway`, `src/nrd_gateway`) built from the official NVIDIA SDKs, and AMD FSR through `src/fsr_gateway` built from AMD FidelityFX; `third_party/fetch-*.ps1` download the SDKs from their official sources. The game itself links none of them. The Khronos Vulkan headers are not included: put Vulkan-Headers 1.4.313 (`include/vulkan`, `include/vk_video`) into `src/include/`. Ambient occlusion is adapted from Intel's XeGTAO (MIT licence, text included in `distrib/docs/licenses/xegtao/LICENSE.txt`).

- Hardware ray tracing (Vulkan `ray_query`, computed next to the OpenGL renderer). It draws:
  - point lights with a shadow ray
  - glowing lamps
  - the sun with a shadow ray
  - sky light
  - lit player models
- This is not full path tracing.
- *Classic* keeps the original Sauerbraten lighting and builds no RT scene.

**Menu:** Options → Graphics → *General* tab → *Lighting*

| Setting | Values | Default |
|---|---|---|
| `hwrt` | `0` Classic (no RT), `1` ray tracing | `0` |
| `hwrtshadowself` | own-body RT shadow: `0` off, `1` sun, `2` + small/glow lights, `3` + nearest lamps, `4` + sky | `1` |
| `hwrtshadowothers` | RT shadows of the others (players, bots, corpses, projectiles, flags, pickups): `0` none, `1` sun and sky, `2` + teleporters / lava / jump pads, `3` + the map's lamps | `2` |
| `hwrtsoftshadows` | RT soft shadows (penumbra that widens with the distance from the object; Options → Graphics → *Ray tracing* tab → *Shadows*, *Off* / *On*): `2` On, the sun and the lamps (lamps, fires, teleporters, lava), `0` Off (hard shadows), `1` the sun only (console). The *Low* profile turns them off | `2` |
| `hwrtteleportlight` / `hwrtjumppadlight` | light cast by teleporters / jump pads | `1` / `0` |
| `hwrtsmokeshadow` | weapon smoke (rifle trail, rockets, grenades, any trail colour) casts soft RT shadows; `hwrtsmokeshadowdensity` sets how dark (10–400, `100`) | `0` |
| `hwrtdynlights` | muzzle flashes, rockets and grenades in flight and explosions light the scene (*RT Weapon Lights*); custom colours tint the light | `1` |
| `hwrtsundisk` | hide the skybox and draw a sun disk | `0` |
| `hwrtreflections` | sharp RT reflections on water and on surfaces the map made reflective (*RT Reflections*) | `1` |
| `hwrtwaterwaves` | animated waves on ray-traced water (Options → Graphics → *Ray tracing* → *Surfaces*): the RT reflections ripple like real water, the water stays flat for the game; needs `hwrtreflections`. Strength `waterwaveamp` (0.25–3, `1`), speed `waterwavespeed` (0.25–3, `1`), detail `waterwavesize` (64/128/256, `128`), shared with `waterwaves` | `0` |
| `hwrtspecular` | shine of lamps and the sun on glossy surfaces (*RT Specular*) | `1` |
| `hwrtdiffupscale` | RT texture upscaling (Options → Graphics → *Performance* tab → *Advanced* → *Lamps and textures*): `2` Bicubic, `3` Lanczos, `0` Nearest (old blocky look); `1` bilinear (console only) | `2` |
| `hwrtdiffmipbias` | distant RT texture detail (Options → Graphics → *Performance* tab → *Advanced* → *Lamps and textures*): `0` Stable (less shimmer in motion), `-0.5` Sharper | `0` |
| `hwrtsmoothnormals` | smooth RT lighting across faceted floors, slopes and rocks, like classic lighting (Options → Graphics → *Ray tracing* tab → *Surfaces*): `1` On, `0` Off (flat faces, old RT look) | `1` |

Useful commands:

- `/hwrtstats`: RT statistics.
- `hwrttoggle`: switch RT on or off (only if RT is available).
- `hwrttimes 1`: GPU time per stage.

Menu status lines tell you whether RT is really running.

If your graphics card or driver cannot run ray tracing, *Ray tracing* is greyed out with the reason.

- Graphics profiles *Ultra*, *High*, *Medium* and *Low* (Options → Graphics → *Performance* tab → *Profile*); changing one of their options on its own gives *Custom*.
- Teleporter lights are gathered once per light update instead of every frame.
- When the model cache is full, models that cannot be added are skipped before any GPU work.
- Sky rays use blue noise with NRD.
- Item models (boost, armour, quad…) are prepared behind the loading screen, and new skins are added without waiting for the GPU: no more freeze when an item respawns.
- Room for 128 different models in the ray-traced scene (was 64): items no longer go missing on big maps.
- Lamps are sorted per screen tile, so each pixel only tests the lamps near it.
- If ray tracing cannot run (no hardware RT, no Vulkan driver, driver too old, failed to start), *Ray tracing* is greyed out in Options → Graphics → *General* → *Lighting*, with the reason. The settings search and the assistant show it greyed out too.

Commands:

- `/hwrtraison`: why ray tracing cannot run (empty when it can).
- `/hwrtstats`, `hwrttimes 1`: RT statistics and GPU time per stage.

**Menu:** Options → Graphics → *General* tab → *Anti-Aliasing*

| Setting | Values | Default |
|---|---|---|
| `hwrtngxmode` | `0` Native, `1` DLAA, `2` DLSS Quality, `3` DLSS Balanced, `4` DLSS Performance, `5` FSR Native, `6` FSR Quality, `7` FSR Balanced, `8` FSR Performance | `0` |
| `hwrtfsrsharpness` | FSR sharpening after upscaling (`0` off to `100`) | `10` |
| `hwrtdlsssharpness` | Sharpening after DLAA/DLSS with AMD's open RCAS filter (`0` off to `100`) | `0` |
| `hwrtupscalemipbias` | Sharper textures with DLSS/FSR Quality, Balanced and Performance (`1` on) | `0` |
| `fsaa` | MSAA, Classic lighting with Native only: `-1` default (off), `2`, `4`, `8`; a change restarts the graphics | `-1` |

- FXAA is not stacked with DLAA/DLSS/FSR. It comes back in Native.
- If DLAA/DLSS or FSR cannot start, the game falls back to Native and says so.
- No frame generation.

**Menu:** Options → Graphics → *Performance* tab → *Advanced* → *Sky light*

| Setting | Values | Default |
|---|---|---|
| `hwrtnrd` | `0` built-in filter, `1` NVIDIA NRD | `1` |

- It only applies with ray tracing, and works with Native and DLSS.
- If NRD is unavailable, the built-in filter is used.
- *Rays per pixel* (`hwrtskyrays`) only counts with NRD: the built-in filter always traces 4 rays and smooths over frames (`hwrtskytemporal`).

**Menu:** Options → Graphics → *HDR screen* tab

| Setting | Values | Default |
|---|---|---|
| `hdroutpref` | `0` SDR, `1` Native HDR | `0` |
| `hwrthdrexp` | exposure, in EV (−8…8; menu steps ±0.1 / ±0.5) | `0` |
| `hdroutref` | reference white, in nits (40–480) | `80` |
| `hdroutmax` | peak, in nits; `0` = automatic, from the display | `0` |
| `hdrouthud` | HUD brightness, in nits; `0` = follows reference white | `0` |

- Native HDR needs Windows HDR to be on. The game never changes Windows or driver settings.
- Native HDR starts once the game window is ready, and retries by itself if it fails.
- If Windows switches HDR off for a moment while the game starts, the game waits up to a minute and turns native HDR on when it is back, with a message meanwhile.
- If you turn Windows HDR on after starting the game, choose *Native HDR* again.
- In native HDR, SSAO (Creases) and motion blur are not applied.

**Menu:** Options → Graphics → *General* tab (anti-aliasing) and *Classic* tab (shading, own shadow)

| Setting | What it does | Default |
|---|---|---|
| `lookfxaa` | FXAA in Native (Options → Graphics → *General* → *Anti-Aliasing*): smoother edges, softer textures. Off = as sharp as stock Sauerbraten | `0` |
| `lookselfshadow` | classic lighting: your own body casts a sun shadow in first person (needs Classic shadows, Graphics → Classic). With ray tracing it is `hwrtshadowself` (Graphics → *Ray tracing* → *Shadows*) | `1` |
| `waterwaves` | animated waves on classic water (Options → Graphics → *Classic* → *Detail*): reflection, refraction and shine ripple, the water stays flat for the game. Same strength / speed / detail settings as `hwrtwaterwaves` | `0` |
| `gtao` | Ambient Occlusion, classic lighting only (Options → Graphics → *Classic* → *Shading*): `0` off, `1` Low, `2` High. Slightly darker corners and wall bases; players are never darkened | `1` |

**Diagnostic views are offline only.** On a server (or with other players on your own server), debug views that could show players through walls or without textures are switched back to normal before every frame, with the message `debug view disabled online`: `hwrtdebug` other than `0` and `7`, `rtaodebug`, `hwrtshade`, `hwrtveldebug`, `hwrtnrddbg`, `hwrtdiffvis`, `hwrtskyvisdbg`, `hdrlightdbg`, `hwrtdepthmask 0`, and the `hwrtvel…` test drives. Offline and in demos, nothing changes. Normal settings (lighting, anti-aliasing, HDR, NRD, shadows) are never blocked.

## Applying

Apply first: every patch before it in the series, in order (`00-build-fix` to `08-mouse-and-window`). It was made and checked on top of them and does not apply on its own.

```
git apply patches/09-graphics.patch
```

- Checked: applied after the previous patches of the series and compiled.


The menu paths above are those of the full L4ZY menus (patch `16-menus`). Without that patch, the settings and commands exist all the same: use them from the console or your own `.cfg` files.


## Files

- `data/glsl.cfg`
- `data/hwrt.cfg`
- `distrib/docs/licenses/NVIDIA-RTX-SDKs-LICENSE.txt`
- `distrib/docs/licenses/xegtao/LICENSE.txt`
- `src/Makefile`
- `src/engine/animmodel.h`
- `src/engine/console.cpp`
- `src/engine/decal.cpp`
- `src/engine/dynlight.cpp`
- `src/engine/engine.h`
- `src/engine/explosion.h`
- `src/engine/grass.cpp`
- `src/engine/hdrout.cpp`
- `src/engine/hwrt/README.md`
- `src/engine/hwrt/as.cpp`
- `src/engine/hwrt/dlaa.cpp`
- `src/engine/hwrt/dynents.cpp`
- `src/engine/hwrt/fsr.cpp`
- `src/engine/hwrt/hwrt.cpp`
- `src/engine/hwrt/hwrt.h`
- `src/engine/hwrt/interop.cpp`
- `src/engine/hwrt/lights.cpp`
- `src/engine/hwrt/nrd.cpp`
- `src/engine/hwrt/shaders/bluenoise_tab.h`
- `src/engine/hwrt/shaders/debug.comp`
- `src/engine/hwrt/shaders/debug_comp.h`
- `src/engine/hwrt/shaders/hitlight.comp`
- `src/engine/hwrt/shaders/hitlight_comp.h`
- `src/engine/hwrt/shaders/hitlight_half_comp.h`
- `src/engine/hwrt/shaders/hitlight_half_norefl_comp.h`
- `src/engine/hwrt/shaders/hitlight_norefl_comp.h`
- `src/engine/hwrt/shaders/hitshade.comp`
- `src/engine/hwrt/shaders/hitshade_comp.h`
- `src/engine/hwrt/shaders/rtao.comp`
- `src/engine/hwrt/shaders/rtao_comp.h`
- `src/engine/hwrt/shaders/silhouette.comp`
- `src/engine/hwrt/shaders/silhouette_comp.h`
- `src/engine/hwrt/shaders/skycompose.comp`
- `src/engine/hwrt/shaders/skycompose_comp.h`
- `src/engine/hwrt/sky.cpp`
- `src/engine/hwrt/temporal.cpp`
- `src/engine/hwrt/trace.cpp`
- `src/engine/hwrt/velocity.cpp`
- `src/engine/hwrt/vkdevice.cpp`
- `src/engine/lensflare.h`
- `src/engine/lightmap.h`
- `src/engine/main.cpp`
- `src/engine/md5.h`
- `src/engine/model.h`
- `src/engine/normal.cpp`
- `src/engine/octaedit.cpp`
- `src/engine/octarender.cpp`
- `src/engine/rendergl.cpp`
- `src/engine/rendermodel.cpp`
- `src/engine/renderparticles.cpp`
- `src/engine/rendersky.cpp`
- `src/engine/rendertarget.h`
- `src/engine/renderva.cpp`
- `src/engine/shader.cpp`
- `src/engine/skelmodel.h`
- `src/engine/texture.cpp`
- `src/engine/texture.h`
- `src/engine/vertmodel.h`
- `src/engine/water.cpp`
- `src/fpsgame/client.cpp`
- `src/fpsgame/entities.cpp`
- `src/fpsgame/fps.cpp`
- `src/fpsgame/render.cpp`
- `src/fpsgame/weapon.cpp`
- `src/fsr_gateway/build-dll.bat`
- `src/fsr_gateway/sauer_fsr.cpp`
- `src/fsr_gateway/sauer_fsr.def`
- `src/include/fsr_gateway/sauer_fsr.h`
- `src/include/ngx_gateway/sauer_ngx.h`
- `src/include/nrd_gateway/sauer_nrd.h`
- `src/ngx_gateway/REQUIREMENTS.txt`
- `src/ngx_gateway/VERSIONS.txt`
- `src/ngx_gateway/build-dll-hdr.bat`
- `src/ngx_gateway/build-dll-sr.bat`
- `src/ngx_gateway/build-dll.bat`
- `src/ngx_gateway/find-msvc.bat`
- `src/ngx_gateway/probe_caller.c`
- `src/ngx_gateway/sauer_ngx.cpp`
- `src/ngx_gateway/sauer_ngx.def`
- `src/ngx_gateway/setup-runtime.bat`
- `src/nrd_gateway/build-dll.bat`
- `src/nrd_gateway/build-nrd-lib.bat`
- `src/nrd_gateway/sauer_nrd.cpp`
- `src/nrd_gateway/sauer_nrd.def`
- `src/shared/iengine.h`
- `third_party/fetch-cmake.ps1`
- `third_party/fetch-fidelityfx-sdk.ps1`
- `third_party/fetch-msvc.ps1`
- `third_party/fetch-ngx-sdk.ps1`
- `third_party/fetch-nrd.ps1`
- `tools/check-fxaa.py`
- `tools/check-looktaa.py`
- `tools/compile-hwrt-shaders.py`
- `tools/generate-hwrt-bluenoise.py`
