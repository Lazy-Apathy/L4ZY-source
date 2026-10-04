# Hardware ray tracing layer

A Vulkan processing layer that runs beside the existing OpenGL renderer and hands
a result texture back to GL for the final compose. The GL renderer, HUD,
particles and UI are untouched; ray tracing is a mode, and baked lightmaps stay
the fallback forever.

Phases 0 (detect and stub), 1 (interop), 2 (world BLAS/TLAS + silhouette
debug view), 3 (RTAO probe from shared GL depth), 4 (hybrid hit shading:
closest-hit samples the existing bake and diffuse), 5 (point lights +
one shadow ray), 6 (mapmodels and dynents in the TLAS), sun / skylight
in the lighting view, an optional visible-sun disk (`hwrtsundisk`, off
by default so the map skybox stays), skipping the GL bake when
that lighting view is showing, and mode-7 lighting of those models
(same lights as the world) are implemented. DLAA / DLSS Super Resolution
(`dlaa.cpp`, public NGX Vulkan SDK through `bin64/ngx-hdr/sauer_ngx.dll`,
see `src/ngx_gateway/VERSIONS.txt`) and AMD FSR 3.1 (`fsr.cpp`) are separate
upscaling paths chosen with `hwrtngxmode`; they also work with classic lighting.

## Requirements

| | |
|---|---|
| GPU | anything with `VK_KHR_external_memory` + `VK_KHR_external_semaphore` for the interop |
| GPU for actual ray tracing | RT cores, i.e. `VK_KHR_acceleration_structure` + `VK_KHR_ray_query` (phase 2 onwards) |
| GPU for DLAA / DLSS | NVIDIA RTX with an NGX runtime in the driver; `nvngx_dlss.dll` and `sauer_ngx.dll` in `bin64/ngx-hdr` |
| Driver | must expose `GL_EXT_memory_object`, `GL_EXT_semaphore` and their `_win32` / `_fd` companions |
| Build | nothing new. The Vulkan loader is opened at runtime and the headers are vendored in `src/include/vulkan` |

Every one of those is checked at startup. A machine that fails any check logs one
line and runs as the vanilla client.

Developed and tested on an RTX 4070 Ti SUPER, NVIDIA 591.86, OpenGL 3.3 core,
Windows.

## What has actually been verified

Phase 1 was signed off against these runs, all on the same machine:

- The compute shader's output reaches GL, and a bare `vkCmdClearColorImage`
  (`hwrtdebug 3`) does too, so the shared allocation is proven independently of
  the shader.
- `screenres` 2560x1440 → 1024x640 → 1600x900, a full `resetgl`, and a
  fullscreen round trip: the shared image follows every time.
- Map changes `box_demo` → `nucleus` (stock DM) → `triforts` (dense community
  map) with the layer on throughout, plus `hwrt` toggled off and on across them.
- Three minimize/restore cycles: no deadlock, and the shared image still reads
  back correctly afterwards.
- Launched with the Vulkan loader pointed at a nonexistent ICD: one line logged,
  `hwrt 1` refused, `hwrtstats` and `hwrtprobe` decline, the client runs as
  vanilla.
- `hwrtstalls` read 0 in every one of those runs.

Phase 4 on the same machine: `hwrtdebug 6` on `box_demo` (72 tris, reserved
lmids only), `nucleus` (103246 tris, 9 lightmaps 1024×512) and `triforts`
(240463 tris, 13 lightmaps 1024×1024). Walls show albedo × the bake, not
barycentric rainbow or AO gray. After `screenres` 1024×640, mode 3 still
probes `255 0 255 255`, then mode 6 still shades. `hwrtstalls` 0. Miss stays
transparent (HUD and sky are GL). Mapmodels and the player are painted over
because they are not in the TLAS.

Phase 5 on the same machine: `hwrtdebug 3` after `screenres` 1024×640 still
probes `255 0 255 255`. Mode 4 barycentrics and `rtaodebug` still run. Mode 6
still shades the bake. `hwrtdebug 7` on `box_demo` (0 light ents → albedo ×
`ambient`, not a bug), `nucleus` (553 lights, 256 uploaded, extras dropped)
and `triforts`. Hits are Sauer textures lit by point lights, not the bake;
`hwrtdlights 4` is slightly brighter than 1 and does not crash. After
`screenres`, mode 3 is magenta again, then mode 7 still lights. `hwrtstalls`
0. `hwrtlight 1` wins over `hwrtdebug 6`. Outdoor views were still dark
(no sun/sky yet).

Phase 6 on the same machine: `hwrtdebug 3` after `screenres` 1024×640 still
probes `255 0 255 255`, then 1600×900 still magenta. Mode 4 on `nucleus`
(7 mapmodel instances) and `triforts` (714) colours crates, pickups and
pillar meshes with barycentrics that match the GL silhouette. Mode 7
leaves those GL models visible and they cast hard shadows on world
floors. `thirdperson 1` puts the local player in the TLAS (occludes and
casts a shadow); `thirdperson 0` does not eat the first-person view.
`hwrtstalls` 0 on `box_demo` → `nucleus` → `triforts`. `hwrt` 0 is still
vanilla. Player BLASes stay rest-pose.

Sun / skylight on the same machine: `hwrtdebug 3` after `screenres` 1600×900
still probes `255 0 255 255`, then mode 7 still has sun/sky. `box_demo`
(sun off, sky off, 0 light ents) stays albedo × ambient — not a fake sun.
`nucleus` logs `sun on, sky off` (the map's `skylight` is 0) and the same
indoor spawn is brighter than the phase-6 point-light shot. `triforts`
logs `sun on, sky on`; courtyard cobbles that face the sun are about
twice the phase-6 luminance, an overhead beam casts a hard sun shadow
on the floor, and under the arch the extra term drops to the old
point-light image. Setting `sunlight` 0 and `skylight` 0 on `triforts`
returns that old image. `hwrtstalls` 0. `hwrt` 0 is still vanilla.

Skipping the bake on the same machine: `hwrtdebug 3` after `screenres`
1024×640 then 1600×900 still probes `255 0 255 255`. Mode 6 still shows
bake × diffuse. Mode 7 on `box_demo` is albedo × ambient (the baked
floor patch from mode 6 is gone). `nucleus` logs `sun on`; mode 7 is
darker than mode 6 at the indoor spawn (RT lights only, not the bake).
`triforts` logs `sun on, sky on`. `academy` logs `sun off, sky on`;
mode 7 stayed dark/grainy while `hwrt` 0 was the sunny bake (that was
before radius-0 omnis were evaluated; see below). Map change
`box_demo` → `nucleus` → `triforts` → `academy` with the layer on,
`hwrtstalls` 0. `hwrt` 0 is still vanilla sky + bake. The map skybox
stays on in the lighting view (`hwrtsundisk` 0). `hwrtsundisk` 1 still
draws a disk that follows `sunlightyaw` when `sunlight` is not 0.

Hudgun after the composite on the same machine: lighting view, first
person, spawn facing an opaque wall on `nucleus` and `triforts`. The
GL pistol is visible against that wall. `hwrt` 0 still shows the gun
in the vanilla place. `hwrtdebug 3` after `screenres` still probes
magenta. `hwrtstalls` 0.

Radius-0 omnis on the same machine, all at 1600×900. `academy` logs
`80 point lights (1 unlimited), sun off, sky on`; that one ent is
`light 0 230 210 190`. In the lighting view an outdoor courtyard spawn
reads mean luma 47 against 75 for `hwrt` 0 on the same view — grainy,
but lit, not a black hole. Coop-edit on that ent drives the image:
`entattr 1 0; entattr 2 255; entattr 3 0` turns the sunlit patch green
(mean R over the patch 38.6 → 23.4, G 28.1 → 30.4); `entattr 0 64`
drops the same patch from luma 29.8 to 18.1 and the log flips to
`0 unlimited`; `entattr 0 0` returns luma 29.8 and `1 unlimited`. Ten
such round trips across ten respawns, `hwrtstalls` 0 throughout. An
on-minus-off difference image shows the ent lighting whole courtyard
walls and floors with shadowed archways, not a flat ambient lift.
`box_demo` still logs `0 point lights (0 unlimited), sun off, sky off`
and stays albedo × ambient, so no sun was invented. `nucleus`
(`256 point lights (0 unlimited), sun on, sky off`) and `triforts`
(`256 (0 unlimited), sun on, sky on`) take the same code path as
before and are unchanged. After `screenres` 1024×640 then 1600×900,
`hwrtdebug 3` probes `255 0 255 255` both times and mode 7 on
`triforts` is the same image as before the round trip, hudgun still
visible against the wall.

Animated player BLASes on the same machine, 2560×1440. `hwrtdebug` 4
while spectating a bot on `academy`: the barycentric silhouette sits on
the GL model in the run cycle, arms and legs included, not a T-pose.
Four bots in that session shared three animated BLASes (the skeleton
cache key). Mode 7 follow-cam was 314 fps; a first-person hallway with
the same four bots out of view was 333 fps, against 372 from the
previous lighting-view run. `hwrtstats` stayed at `fence stalls 0`. An
idle third-person player is one animated BLAS and 0 refits (exact-pose
skip). FFA suicide keeps that one animated BLAS — the old `CS_DEAD`
skip would have dropped it. `hwrtdebug` 3 after `fullscreen 0` then
`screenres` 1024×640 still probes `255 0 255 255`, and again after
returning to 2560×1440. `nucleus` first-person stays 0 animated BLASes
(7 mapmodels + pickups only) at 165 fps; `box_demo` 458–480 against
the previous 487.

RT-lit models on the same machine, 2560×1440. Mode 7 primary rays hit
models (`SHADE_MODELS` 0x200) and shade them with the same lights as
the world (skin albedo × ambient + radius-0 omnis + N nearest finite
lights + sun + sky, one shadow ray each). `customIndex` 0 stays the
world SSBOs; anything else indexes a per-BLAS attribute table reached
by device address, never the world buffers. A fail (overflow, no attrs,
pose not yet known) stores transparent so GL's raster shows through.
On `academy`, spectator-follow of a bot in the courtyard (`light 0
230 210 190`): armour mean RGB went from about (23,40,12) in an indoor
shadow to (42,72,19) in the omni, with 1-spp grain on the skin (patch
variance ~200, same order as the floor). Walking into shadow darkened
the model. Self-shadow stays on the model; the floor shadow still
follows the run cycle. Four bots shared four animated BLASes.
`hwrtmask` 0 no longer punches a glowing T-pose — the hit is shaded,
not `vec4(0)`. The cvar stays as a fallback for instances the shader
could not shade, and mode 6 still uses it. `hwrtdebug` 3 after
`fullscreen 0` + `screenres` 1024×640 probed `255 0 255 255`, then
2560×1440 still magenta. `hwrtstalls` 0. Follow-cam was 283–297 fps
against slice 1's 314 (~5–10%). `nucleus` first-person 159 fps against
165; `box_demo` 483 against 458–480. World lighting on those maps was
unchanged. Modes 4 and 6 still run. `hwrt` 0 is still vanilla.

The world overbright on the same machine, 2560×1440. The bug was found by
measuring `hwrtdebug` 6 against `hwrt` 0 rather than by reading the lighting
code: mode 6 is `albedo × bake`, the same product the rasteriser draws, and it
came back at a median of **0.500** of vanilla on `academy` — a clean factor of
two in a view that does not use the lighting model at all. After the fix,
`box_demo` (no fog, no bump, no lights, so `albedo × bake` and nothing else)
reads mode 6 139.21 against vanilla 137.48, a ratio of 1.013.

Mode 7 on `academy`, both gains captured from one camera without moving:
a shadowed patch went 7.48 → 8.89 (+1.41 of 255, and still far under vanilla's
23.87 for the same patch), a patch lit by a ceiling lamp went 29.37 → 52.52
(×1.79, against vanilla 43.19), so the lit-to-shadow ratio went 3.93 → 5.91.
`box_demo` (0 lights, sun off, sky off) is bit-identical between gain 1 and
gain 2 across the whole world — the only pixels that move are the bobbing
hudgun. `triforts` (sun on, sky on, 256 lights) went 28.70 → 38.95 against
vanilla 35.12 with the share of clipped-to-white pixels unchanged at 0.22%
(vanilla 0.12%) and p99 luma 58 → 102 against vanilla's 103, so nothing blew
out. Frame rate was unchanged (`academy` 223 both ways, `triforts` 174 → 172).
`hwrtdebug` 3 still probed `255 0 255 255` afterwards and `hwrtstalls` stayed 0.

The skyvis workgroup filter on the same machine, 2560x1440, lighting view,
`authentic`, one camera looking down the courtyard: unfiltered 331 / 294 / 224
fps at 1 / 2 / 4 rays (GL 869, `hwrtstalls` 0). `hwrtskyfilter` 1 at one ray
is 275 fps, dispatch 2.25 ms against 2.89 ms for four unfiltered rays. On a
192×192 shadow patch the high-pass luma std went 47.3 (1 ray) → 23.7 (4 rays)
→ 6.8 (filter + 1 ray), against 4.1 for `hwrt` 0. A wall/floor crease kept its
edge (normal gate); a third-person player on the grass did not smear sky onto
the ground around his feet (depth gate). A 2-pixel skyvis apron around each
tile was 207 fps, slower than four rays, so the 16×16 tile stands. `cmvalley`
in this spawn looks at canopy/sky more than open ground; dispatch still dropped
8.91 → 6.52 ms at 1 ray + filter, stalls 0.

The skyvis temporal accumulation on the same machine, 2560×1440, lighting view,
`authentic`, camera held still, two consecutive screenshots per state. Crawl is
the luma std of the frame-to-frame difference in levels out of 255, so it reads
the noise and not the content: 18.04 raw, 6.03 with the spatial filter, 1.68 at
`hwrtskytemporal` 32, against 1.27 for the same shot at `hwrt` 0. Taking GL's
1.27 out in quadrature as the non-stochastic floor (dithering, water, animated
textures) the noise itself is 17.99 → 5.90 → 1.10, i.e. four rays land within
16× of a deterministic renderer, about 1000 effective samples. The share of
pixels that move by more than 2 levels goes 53% → 56% → 1.7% (GL 0.07%): the
spatial filter alone lowers the *amplitude* without settling the frame, which is
exactly the leftover crawl the testers kept seeing. p99 is 72.9 → 21.9 → 2.6. The
gain is uniform, not confined to flat regions — over 13 516 16×16 tiles the
t32/t0 crawl ratio is p10 0.06, p50 0.12, p90 0.16. `hwrtskytemporal` 96 gives
1.74, statistically the same as 32. Frame rate and dispatch are unchanged
(the pass is two image ops in a shader that already runs) and `hwrtstalls`
stayed 0. A second, independent read on the same runs: between two frames of a
motionless camera 41.3% of pixels differ by more than 2 levels with the pass off
and 7.4% with it on, mean signed change nil both ways.

Homemade TAA (`looktaa`, default on) on the same machine, 2560×1440, lighting
view, one ray + filter: `authentic` 317 fps off / 307 on (dispatch 1.85 ms
both ways, `hwrtstalls` 0). Full-frame high-pass luma std 7.45 → 3.71. The
same pass on `flagstone` 9.99 → 4.32 and `academy` 9.53 → 4.27. `hwrt` 0 on
that authentic camera is 4.89 → 2.86, so the resolve is not an RT-only trick.
`cmvalley` from this spawn is an indoor names-wall (129 → 126 fps, dispatch
5.83 ms); the forest look was not recaptured in the same session. TAA costs
about 0.1 ms at 1440p. Four unfiltered rays still cost 2.89 ms of dispatch
from the filter slice, which this keeps.

Not verified yet: the GL depth mask (`hwrtdepthmask`, see below) was written
against the tree-canopy artefact on a community map and compiles clean, but no
run has been captured for it. The checks it needs are a mode 7 screenshot of a
pickup behind foliage against `hwrt 0` from the same camera, `hwrtdepthmask` 0
and 1 from that same camera, `hwrtdebug 3` still magenta after a `screenres`
round trip, `hwrtstalls` 0, and a frame rate on `academy` / `nucleus` /
`triforts` within the usual budget of the numbers above.

Not verified: no RenderDoc or Nsight capture was taken (neither is installed), so
"no CPU wait" rests on the fence-stall counter and on there being no blocking
call on the frame path rather than on a profile. VRAM was not measured, so
"no leaks" means destroy-before-create plus no driver complaints, not a
measurement. The Linux opaque-fd branch has never been compiled as part of a
build; its Vulkan half was syntax-checked against the real headers in isolation,
which catches typos in structures and signatures but proves nothing about
runtime behaviour.

## Reproducing those runs

Everything below is Windows with w64devkit, which is the only environment this
has been built in.

**Build.** From `src/`, with `%LOCALAPPDATA%\w64devkit\w64devkit\bin` on `PATH`:

```
make -j8 PLATFORM=MINGW64 client
```

The output is `bin64/sauerbraten.exe`. The Makefile carries no generated header
dependencies: editing `engine/hwrt/hwrt.h` rebuilds the hwrt objects thanks
to an explicit rule, but editing any *other* engine header rebuilds nothing.
Delete the affected `.o` by hand, or the link will fail with an undefined symbol
that makes no sense.

Shaders are pre-compiled and the SPIR-V is committed, so a build needs no Vulkan
SDK. Only after editing `shaders/debug.comp`, `shaders/silhouette.comp`,
`shaders/rtao.comp`, `shaders/hitshade.comp` or `shaders/hitlight.comp`:

```
python tools/compile-hwrt-shaders.py
```

The sky-ray blue-noise tile `shaders/bluenoise_tab.h` is generated and committed
the same way. It only needs regenerating if the tile itself changes (the
output is deterministic, so a rerun rewrites the same bytes; needs numpy):

```
python tools/generate-hwrt-bluenoise.py
```

**Launch.** Use `sauerbraten.bat`, or pass the home directory yourself — but it
contains a space and *must* stay quoted:

```powershell
Start-Process -FilePath "<tree>\bin64\sauerbraten.exe" `
  -ArgumentList '"-q$HOME\My Games\Sauer-RT"','-glog.txt' `
  -WorkingDirectory "<tree>"
```

`$HOME` is expanded by the engine, not by the shell, so single quotes are
correct. Get the quoting wrong and the game runs against a *different* home
directory: no log, no screenshots, no `once.cfg` picked up. It looks exactly like
a hang and it is not one. Note also that `sauerbraten.bat` uses `start`, so it
returns immediately instead of waiting for the game.

**Driving the client without a keyboard.** The engine executes
`<home>\once.cfg` at startup and then deletes it, so rewrite it before *every*
run. Two traps:

- `map` calls `clearsleep()`, which cancels every pending `sleep`. Any step that
  has to happen after a map load must be registered *inside* the block, after the
  `map` command.
- `screenshot <name>` reads back the previously presented frame, so leave a
  beat between changing a mode and capturing it.

```
sleep 500 [
    map box_demo
    sleep 2500 [ hwrt 1; hwrtdebug 2 ]
    sleep 4000 [ hwrtprobe ]
    sleep 6000 [ hwrtstats; quit ]
]
```

**Where the output goes.** `Documents\My Games\Sauer-RT\log.txt` and
`Documents\My Games\Sauer-RT\screenshot\`. The log is unbuffered from `hwrtinit`
onwards; anything logged before the GL init is still block buffered and is lost
if the process is killed rather than quitting.

**What each check should show.**

`hwrtprobe` samples three points and logs them. Measured values, with the layer
on and a map loaded (the gradient ones shift by a unit or two with resolution):

| `hwrtdebug` | top edge | corner patch | centre |
|---|---|---|---|
| 3, flat clear | `255 0 255 255` | `255 0 255 255` | `255 0 255 255` |
| 2, compute full screen | `127 255 127 255` | `16 16 127 255` | `127 127 127 255` |
| 1, overlay | `255 122 23 255` | `100 99 0 191` | `0 0 0 0` |

In overlay mode the top edge is the orange border, the centre is deliberately
transparent so the GL frame shows through, and the corner patch is the gradient
at 0.75 alpha.

| Check | How | Pass looks like |
|---|---|---|
| Shared allocation | `hwrt 1; hwrtdebug 3; hwrtprobe` | magenta, per the table above. No shader is involved |
| Compute reaches GL | `hwrtdebug 2; hwrtprobe` | the gradient row of the table |
| Composite | `hwrtdebug 1`, screenshot | orange border on the frame edges, a green bar that moves between frames, a gradient patch in the *bottom* left |
| No CPU stall | `hwrtstats` | `fence stalls 0` |
| Resize | `fullscreen 0`, then `screenres`, then `resetgl` | a new `shared texture WxH` line each time, no other output |
| Map change | `map` between two maps with `hwrt 1` | no new shared texture, stats unchanged, no stall |
| Alt-tab | minimize and restore the window | `hwrtprobe` still reads the pattern afterwards |
| Graceful fallback | launch with `VK_DRIVER_FILES` and `VK_ICD_FILENAMES` set to a nonexistent path | one `hwrt:` line at init, `hwrt 1` refused, client fully playable |

A probe reading `0 0 0 0` everywhere with no GL error is the signature failure of
this layer: GL holds a valid, complete texture that is not the Vulkan
allocation. Start from `hwrtdebug 3` to tell that apart from a broken dispatch.

## Variables

Defaults live in `data/hwrt.cfg`, which loads before `config.cfg` so a saved
config always wins.

| Variable | Meaning |
|---|---|
| `hwrt` | master switch. 0 is the vanilla client and costs nothing |
| `hwrtdebug` | what gets composited back: 0 nothing, 1 probe overlay, 2 probe replaces the frame, 3 flat Vulkan clear replaces the frame, 4 TLAS barycentric overlay (world + mapmodels + dynents; miss transparent), 5 RTAO overlay (alias of `rtaodebug 1`), 6 hybrid hit shading (alias of `hwrtshade 1`), 7 lighting view (alias of `hwrtlight 1`, the default): ambient + radius-0 ET_LIGHT (unlimited omni) + glow omnis (every teleport / jumppad / screens / lava nappes) + N nearest finite point lights + sun + sky. Mode 7 also binds `LMID_BRIGHT` on the GL world so the bake does not modulate. World glowmaps add on top of the lit albedo; model `masks.g` mixes toward albedo × `mdlglow`. Playermodels (ENT_PLAYER) also get modelshader spec, the mdlenvmap cubemap (`masks.b`), a studio fill, and a modest visibility floor rather than GL's flattening fullbright |
| `rtaodebug` | 1 = grayscale RTAO overlay from the GL depth buffer. Needs `hwrtrayquery 1` |
| `hwrtshade` | 1 = closest-hit samples baked lightmap × diffuse. Needs `hwrtrayquery 1`. Wins over `rtaodebug`; modes 2/3/4 still win over both |
| `hwrtlight` | 1 = albedo × (ambient + radius-0 omnis + N nearest finite point lights with one shadow ray + sunlight with one shadow ray + cosine-weighted sky rays). Needs `hwrtrayquery 1`. Wins over `rtaodebug` / `hwrtshade`; modes 2/3/4 still win |
| `hwrtmask` | snapshots GL depth around the model passes. Mode 6 hands every model pixel back to GL. Mode 7 keeps a shaded model hit and only drops a world hit that punched through an untraced mesh (flags, alpha cloth). 0 is the old transparent hole |
| `hwrtdepthmask` | mode 7 drops any *world* hit GL rasterised something *nearer* than, using the shared window depth. Foliage and flags are in the TLAS; this still covers grass / water / world alpha. 0 is the old see-through behaviour |
| `hwrtdlights` | how many nearest *finite-radius* `ET_LIGHT` ents to evaluate (default 1, cap 4). Radius 0 is unlimited and is always evaluated on top of this |
| `hwrtshadowself` | what the first-person body occludes. 0 nothing, 1 (default) the sun alone, 2 also the always-evaluated omnis (radius 0, glow), 3 also the nearest-N finite lamps, 4 also the sky. The levels are ordered by how badly each type distorts the body's own shadow. A point lamp magnifies the silhouette by (lamp → lit surface) / (lamp → body), so a body next to a lamp projects its *volume*: shoulders, chest and feet sit at different distances and grow by different factors, and the floor gets the body squashed sideways instead of its outline. Radius 0 is no exception — unlimited reach does not make a light directional. The sun has no position, so magnification is exactly 1 and the shadow is the body's cross-section on any surface; it is the only source that cannot produce the artefact. Level 3 adds a second defect, the per-pixel election that cuts the shadow where the winning lamp changes, and level 4 lets a body that close eat the whole hemisphere (the 3D pit) |
| `hwrtteleportlight` | 1 (default) = every teleport ent projects a lamp. The default ring is still at the hole. Custom models and modelless portals (`attr2 != 0`, including triforts' 54) use the same shadowed always-eval lamp (`flags` 3), a short radius (~40) and a dimmer tint so they light the pad, not the next room through the wall. Colour prefers a world glow pad, then a nearby magic/pad mapmodel tint (blue vs red on triforts), then the vanilla blue. 0 keeps the disk/pad glow and does not light the room |
| `hwrtjumppadlight` | same for jumppads, default 0 (glowing pad, no projected light) |
| `hwrtworldgain` | GL's world overbright, applied to the light mode 7 traces. Default 2, which is what `renderva.cpp` hands the world shader as `colorparams`. Scales point lights, sun and sky; never the `ambient` constant, so raising it opens the gap between lit and shadowed rather than flattening it. 1 is the old half-dark world |
| `hwrtskinbudget` | megabytes of GPU memory allowed for the model-skin array at 1024 before falling back to 512 (default 1024, min 64, max 4096). A ceiling, not a reservation. 192 layers at 1024 need 768 MB, so the default never trips the fallback |
| `hwrtmaxinsts` | how many TLAS instances the scenery may fill (default 4096 = the cap, min 128). Only mapmodels answer to it, and always the nearest ones, so lowering it never removes a player and what it drops is on the horizon. Barely a speed dial: dispatch on `cmvalley` at 1440p in its forest is 5.90 ms for the nearest 256, 6.94 for 1024, 7.46 for all 2614, i.e. 48 fps either way against 62 with `hwrt 0`. What a ray costs is the alpha-tested canopy in front of the camera, which the sort always keeps |
| `hwrtskyrays` | cosine-weighted hemisphere rays for skylight (default 1 since 2026-10-04, cap 4). With NRD + blue noise one ray shows no grain (`shots/rayons-ciel` study, then a tester's test). Whenever NRD is not the effective denoiser (Sauer filter / skyage: NRD off, unavailable or failed), at least 4 rays are traced, without changing the saved value: there one ray is a coin flip and filter + TAA did not reach zero dots. `l4zymigration 2` moves a saved 4 to 1 once; `hwrtskystatus` prints `cvar_rays` and `eff_rays`. `hwrtskyfilter` still averages leftover grain with a 5×5 on the same plane before the bake-style `max()`. Unfiltered on `authentic` at 1440p: 331 / 294 / 224 fps at 1 / 2 / 4 rays (GL 869) |
| `hwrtskyfilter` | 1 (default) = the workgroup skyvis filter. Neighbours on the same plane share leftover grain. 0 is the raw term |
| `hwrtskytemporal` | frames of skyvis to accumulate on top of the spatial filter (default 32, cap 128; 0 or 1 is off). skyvis is view-independent geometry, so a hit recognised as the same surface point inherits the whole running average that point carries — no colour clamp, and therefore not capped by its own neighbourhood the way `looktaa` is. History lives in a ping-pong `RGBA16F` pair (bindings 13/14), 59 MB at 1440p. 96 frames measure the same as 32: the ceiling is the rejection test, not the frame count |
| `hwrtskybluenoise` | 1 (default) = the sky rays of the world pixels NRD denoises come from a 128×128 blue-noise tile (binding 25, two void-and-cluster masks, u1/u2) shifted every frame by the R2 sequence, the four rays of a pixel spread on a (1/2, 1/4) lattice. Same ray count, same estimator, same converged value; less grain into REBLUR. The skyage path (Sauer filter, models, NRD off) keeps the pcg hash. 0 = the hash everywhere. Not saved; a change reseeds the sky history |
| `looktaa` | 0 (default) = FXAA. 1 = homemade temporal AA (Halton jitter + depth reprojection). Default is off: it smears moving players (no velocity) and did not remove 4-ray dots. The shader stays in `data/hwrt.cfg` / `data/glsl.cfg` as the DLAA foundation. `LOOK_apply` installs FXAA while this is 0 |
| `hwrtvelocity` | 0 (default) = no extra work. 1 = write the velocity image every frame (static world, rigid mapmodels, colour-visible skinned players, first-person hudgun). Does not change the presented picture. Not VARP, so it is not saved |
| `hwrtdlaajitter` | 0 (default) = off. 1 = trial Halton(2,3) subpixel offset once per main view, not saved. Independent of looktaa accumulation; looktaa jitter and resolve are skipped while this is on so the offset is not applied twice. The FPS gun gets the same pixel shift despite its own FOV. Turning it off restores the usual looktaa path. The jittered picture can shimmer: that is not DLAA quality |
| `hwrtveldebug` | 0 = off. 1 = coloured vectors (grey = still, magenta = unsupported). 2 = reprojected previous frame. 3 = reprojection error (green = aligned, red = wrong). `hwrtvelcycle` walks 0→1→2→3→0 and uses `setvar`, so returning to off frees the diagnostic textures and drops history. Overlay only; the velocity image itself is unchanged. The overlay is the 3D scene including the first-person gun. Modes 2/3 add `(prev_jitter - curr_jitter)` when warping two jittered colour frames |
| `hwrtvelhud` | 1 (default) = write first-person hudgun vectors (lot 5). 0 = leave those pixels to the world pass |
| `hwrtsundisk` | 0 (default) keeps the map skybox in the lighting view. 1 skips the cubemap and draws a GL disk on `sunlightdir` so a painted sun cannot disagree with the shadow |
| `rtaoradius` | hemisphere ray length in Sauer world units (default 32) |
| `rtaoscale` | 0 = overlay only; 0..1 multiplies AO onto the vanilla lightmapped frame |
| `rtaobias` | origin offset along the reconstructed normal, so the ray does not self-hit |
| `hwrtngxmode` | saved upscaler choice: 0 Native, 1 DLAA, 2-4 DLSS Quality/Balanced/Performance, 5 FSR Native, 6-8 FSR Quality/Balanced/Performance. `hwrtngxstats` prints the state in force |
| `hwrtdlssmipbias` | 0 (default since 2026-10-04) = scene textures keep mip bias 0 in DLAA/DLSS, as before the DLSS guide pass. 1 = the guide's bias log2(render/display) - 1 (DLAA -1, Quality -1.58, Performance -2) on world and model textures; sharper, but a tester saw aliasing/shimmer with it, even in DLAA. FSR and Native always 0. Not saved |
| `hwrtdiffmip` | 2 (default since aliasing-rt). Which mip level the traced world diffuse is read at (primary hits and their blend layer). 0 = level 0 always (before 2026-10-04: magnified close up, but a far or grazing surface aliases and shimmers). 1 = old footprint experiment: `textureGrad` from the pixel footprint, isotropic, no bias (stable but soft, grazing floors blur). 2 (aliasing-rt) = footprint from the neighbour pixels' rays on the face plane (ray differentials, render resolution, same jittered matrix as the primary ray), scaled by 2^`hwrtdiffmipbias`, plus log2(render/display) with `hwrtdiffmipauto` when DLSS/FSR upscale, sampled through an anisotropic sampler (`hwrtdiffaniso`, binding 29, needs `samplerAnisotropy`). Close surfaces stay at level 0 (same image as 0), far and grazing ones get filtered. Reflections keep their own ray-cone level; models keep level 0. Not saved |
| `hwrtdiffmipbias` | -0.5: lod bias of `hwrtdiffmip 2` (negative = sharper). -3..2. Not saved |
| `hwrtdiffmipauto` | 1: add log2(render width / display width) to that bias while DLSS/FSR render below the display (DLSS Quality about -0.58). 0 = footprint of the render pixel only. Not saved |
| `hwrtdiffaniso` | 1: anisotropic filtering (up to 16x, device limit) for `hwrtdiffmip 2`. 0 = isotropic `textureGrad`. Not saved |
| `hwrtdifftexel` | 0: no floor. 1 = never read a layer finer than the texture's own texels (the world array stretches every layer to the largest texture with nearest, so a 512 texture is 2x2 blocks at level 0; `ShadeTri.x` bits 26-31 carry the level): GL-like bilinear close up, much softer than 0. 2 = one level finer than that. For comparison only. Not saved |
| `hwrtavailable`, `hwrtrayquery`, `hwrtngx` | read-only capability flags |
| `hwrtisavailable`, `hwrtraison` | 1 when RT can run; otherwise one short English line saying why (no Vulkan, driver too old, no hardware RT, failed to start). `data/menus.cfg` declares `settinglock hwrt 1 [hwrtraison]`, which greys out the "Ray tracing" radio, the settings search entry and makes the assistant refuse `hwrt 1` |
| `SAUER_HWRT_SIMULATE` (environment) | diagnostic only, read at bring-up: `novulkan`, `driver`, `nort` or `failed` fakes an incompatible GPU through the real fallback path; `hang` makes the Vulkan probe process hang in `vkCreateInstance`, `crash` makes it crash there, `hanglate` lets the probe answer and makes the game's own start-up thread hang there; retry paths (probe only, real drivers and layers otherwise): `crashigpulayer` / `hangigpu` = the integrated GPU's driver layer (first AMD/Intel manifest, else a made-up AMD one) crashes / hangs unless switched off, `crashigpu` = only leaving out the whole driver helps, `crashigpuold` = the same with the loader reported as 1.3.200 (`VK_ICD_FILENAMES` path), `crashoverlay` / `hangoverlay` = the first tool layer installed crashes (traced to its DLL) / hangs unless switched off, `crashsearch` = a crash reported in `amdxc64.dll` that only switching off the last tool layer stops; `hwrtsimulated` returns the mode in force |
| `hwrtvktimeout` | seconds (3-120, default 15) the Vulkan probe, then the game's own Vulkan start-up, may take before Vulkan is given up for the run (classic lighting + Native AA, reason in the menus) |
| `hwrtvkstall` | saved; 1 after a Vulkan timeout or crash: the next start does not start Vulkan by itself, choosing RT/DLAA/DLSS/FSR retries (and clears it on success) |
| `hwrtvkskipdriver` | saved; what a probe retry had to leave out (see below), `;` separated: `fp:<hex>` (fingerprint of the implicit layers and display drivers; when it differs the value is dropped), `layer:<name>`, `layers:all`, `<driver manifest path>`. A value without a path separator is passed to `VK_LOADER_DRIVERS_DISABLE` as is (e.g. `*amd*`). `""` = leave nothing out (default) |
| `hwrtvksearchms` | milliseconds (0-60000, default 10000) the search for the single culprit implicit layer may take after "every implicit layer off" worked; 0 = keep them all off |
| `hwrtvknote` | read-only: the one line shown when something was left out, `""` otherwise |
| `hwrtstalls` | read-only count of frames where the CPU had to wait on a Vulkan fence. Should stay 0 |
| `hwrttimes` | 1 draws a HUD overlay of the last completed interop stage times in milliseconds (GL mask / depth / composite, Vulkan BLAS / TLAS / dispatch, plus CPU skin/gather). Samples lag by a few frames; the CPU does not wait on them. `hwrtstats` always prints the same numbers |

**Vulkan start-up (since 2026-10-04).** `gl_init` no longer creates a Vulkan
instance: `vkCreateInstance` loads every Vulkan driver and implicit layer of the
machine (overlays, capture/monitoring tools, the driver's own), and one of them
hanging or crashing froze or closed the game at every launch. Now Vulkan starts
only when something needs it (`hwrt 1`, or `hwrtngxmode` 1-8), after config.cfg
and autoexec.cfg (`hwrtprefsloaded`). First a probe process (`sauerbraten.exe
-vkprobe <GL device UUID>`, no window) creates an instance and a device and logs
the loader, the implicit layers (registry and loader), the GPUs and the time of
each step (`hwrt vkprobe:` lines); it is killed after `hwrtvktimeout`. Only if it
answered does the game create its own device, on a worker thread the main thread
waits for with the same timeout (`hwrt vk:` lines, timed). Nothing needed: no
Vulkan in the game; the probe runs 3 s after start in the background so the
menus grey RT/DLSS/FSR with the right reason ("checking..." until then). A
timed-out worker cannot be stopped: Vulkan stays off for the run, nothing touches
its objects, and quitting ends the process with `TerminateProcess`.

**When the probe crashes or hangs (since 2026-10-04, `vkplanretry`).** The probe
also lists the Vulkan drivers (`icd vendor=0x.... manifest=...`, from the
Khronos keys and the display drivers), every DLL it loads outside the Windows
folder (`dll loaded`, driver files included) and, if it crashes, the module it
crashed in (`unhandled 0x... at amdxc64.dll+0x... (path)`). Nothing is left out
while the probe works. After a failure it is started again, narrowest first:
(1) the implicit layers of the manifest the crash was traced to (a crash in
`amdxc64.dll` is traced to the AMD driver's manifest, which also declares
`VK_LAYER_AMD_switchable_graphics`: that layer loads the AMD driver into every
Vulkan application even on the NVIDIA GPU, so `VK_LOADER_DRIVERS_DISABLE` alone
does not keep it out), each by its `disable_environment` variable (any loader)
and by name in `VK_LOADER_LAYERS_DISABLE` (loader 1.3.234+); (2) every implicit
layer (plus `~implicit~`, loader 1.3.262+), then, if that works, each implicit
layer alone, most suspect first, within `hwrtvksearchms`; (3) last, and only with
a driver for the GPU the game runs on next to it, the integrated GPU's whole
driver: `VK_LOADER_DRIVERS_DISABLE=<manifest file name>` (loader 1.3.234+), or
`VK_ICD_FILENAMES` listing the drivers to keep for an older loader (ignored when
the game runs elevated). Leaving a driver out also needs the GPU the game runs on
to be a discrete one with ray query. The first step after which the probe
answers stays in the process environment (the game's own Vulkan start-up reads
it), is saved in `hwrtvkskipdriver` with the fingerprint, and one short line
says what was left out and which driver or program to update; everything else
goes to log.txt only. If nothing helps: classic lighting + Native AA with the
reason, as before. The probe and the game ask `vkCreateInstance` for Vulkan 1.2
(1.1 as a fallback), never 1.4.

**Online, diagnostics are off.** On a remote server (or with other players on
our own listen server; demo playback is left alone), `hwrtonlineguard()` runs
before every frame and puts these back to their play value, printing
`debug view disabled online`: `hwrtdebug` other than 0 and 7 (back to 7),
`rtaodebug`, `hwrtshade`, `hwrtveldebug`, `hwrtnrddbg`, `hwrtdiffvis`,
`hwrtskyvisdbg`, `hdrlightdbg` (back to 0), `hwrtdepthmask 0` (back to 1), and
the velocity test drives `hwrtvelwalk`, `hwrtvelholdplayer`, `hwrtvelholdcam`,
`hwrtvelholdothers`, `hwrtvelfreezepose`, `hwrtveldrive`, `hwrtvelspin`,
`hwrtvelpitchspin`, `hwrtvelslide`, `hwrtvelpush`. A view still on when we
connect is switched off. Offline nothing changes. The table is in `hwrt.cpp`.

Commands: `hwrtstats` prints device, resolution, stall count, world triangle
count, TLAS instance counts (world + mapmodels + dynents), animated BLAS
count (rebuilds / refits this frame), point-light count
(with how many are radius-0 unlimited and how many are glow omnis), sun / sky on or off, whether
model shading is on (geom / skin-layer counts), whether the
model mask and the GL depth mask are on and live, and the last completed
GPU / CPU stage times in milliseconds. `hwrtprobe`
reads the shared texture back through GL on the next frame and logs pixels from
three known positions. `hwrtlooksun` aims the camera along `sunlightyaw` /
`sunlightpitch` (the traced sun; with `hwrtsundisk` 1, also the visible disk).
`hwrtvelcycle` walks the velocity overlay (off / vectors / reprojection /
error) through `setvar`, so off stops the extra passes, frees the textures
and drops history; turning it back on starts from an empty pair. Switching
into reprojection from generation-only also drops colour history so a stale
capture cannot pair with the current matrix. `hwrtvelstats` prints the last
GPU times of the depth+stencil copies, the generate pass, and the overlay
(history copy + debug) without a CPU readback. `hwrtvelbench` prints min /
avg / max of the last 32 harvested frames. `hwrtvelprobe` is a one-shot
centre crop of the velocity image, only when asked (it stalls that once).
`hwrttemporalstats` prints the HUD-less capture (colour/depth/velocity
handles, jitter in pixels, reset flag, analytical subpixel check) without
an overlay. `hwrttemporalcurrent()` is the C++ accessor.
`hwrtvelspin` / `hwrtvelpitchspin` / `hwrtvelslide` / `hwrtvelpush` are
test-only camera drives in degrees or world-units per frame so a screenshot
can land on a moving frame; they stay at 0 in play.

## Phase 2: world silhouettes

With `hwrtrayquery 1` (RT cores present):

```
hwrt 1
hwrtdebug 4
```

Each pixel traces a camera primary ray through `invcamprojmatrix` (same view as
GL; row 0 of the shared image is the bottom of the screen) and `rayQueryKHR`s
the world TLAS. Closest-hit is barycentric colour; miss is transparent so the
vanilla frame shows. Success is walls whose coloured overlay matches the GL
silhouette on `box_demo`, `nucleus`, and `triforts`.

`hwrtdebug` 1/2/3 are unchanged and still prove interop. After a `screenres`,
`hwrtdebug 3` must still come back magenta: destroying the shared image now
clears the cached descriptor view, so a recycled Vulkan handle cannot skip the
descriptor write.

The world BLAS is one concatenation of every `valist` entry after subtracting
`va->voffset` from `readva()` indices. Sky, world-alpha and materials stay out.
texlayer blend floors (grass on dirt) are in the BLAS once: the coplanar dirt
twin is dropped and the grass overlay is mixed with the dirt using lightmap
alpha, the way GL's blend pass does.
It rebuilds at the end of `allchanged()` (map load, remip, coop-edit commit),
not every frame. `vkDeviceWaitIdle` is used only around that rebuild. Phase 6
adds further TLAS instances; the world BLAS itself is unchanged.

If `hwrtrayquery` is 0 the phase 1 probe stays; debug 4 logs once and does not
pretend lighting exists.

Phase 2 was exercised on the same machine as phase 1: `hwrtdebug 3` after
`screenres` still probes magenta; `hwrtdebug 4` on `box_demo` (72 tris),
`nucleus` (103246) and `triforts` (240463) with `hwrtstalls` 0. Overlay vs a
vanilla screenshot is still the judge for whether every wall matches.

## Phase 3: RTAO probe

With `hwrtrayquery 1`:

```
hwrt 1
rtaodebug 1
```

`hwrtdebug 5` aliases that. `hwrtdebug` 2/3/4 are not stolen, so magenta and silhouette probes still work while RTAO is on.

After the GL world pass, the window depth buffer is `glCopyTexSubImage2D`'d
into a local `DEPTH_COMPONENT24` texture (same copy `lookao` uses) and blitted
into a second shared image, `VK_FORMAT_R32_SFLOAT` / `GL_R32F`, with the same
NT-handle rules as the colour output. A true D24/D32 import is not used.
`glSignalSemaphoreEXT` is issued *after* that copy, then `glFlush`.

The compute shader reconstructs world position with `invcamprojmatrix` (row 0
is still the bottom of the screen), builds a geometric normal from the depth
neighbourhood facing the camera, and traces one cosine-weighted hemisphere ray
against the existing world TLAS. Closest-hit is occluded (darker with
distance), miss is unoccluded. Sky / no-depth is transparent so vanilla GL
shows. `rtaoradius` (default 32) is `tmax`; `rtaobias` (default 0.25) offsets
the origin along the normal. `rtaoscale` 0 is overlay-only; above 0 multiplies
onto the lightmapped frame. Lights are not changed.

If `hwrtrayquery` is 0, `rtaodebug` logs once and does not pretend AO exists.
Sky / water / alpha are not in the BLAS: a miss is unoccluded, which is intended.

It will look noisy. That is the milestone, not a reason to add a denoiser.

## Phase 4: hybrid hit shading

With `hwrtrayquery 1`:

```
hwrt 1
hwrtdebug 6
```

`hwrtshade 1` aliases that. `hwrtdebug` 2/3/4 are not stolen. If `rtaodebug 1`
and `hwrtdebug 6` are both on, 6 wins.

Each pixel traces the same camera primary ray as the silhouette view. Closest-hit
interpolates `vertex.tc` and `vertex.lm` (lightmap UV as `short/32767`, same as
`lmcoordscale`), samples a Vulkan copy of `slot.sts[0]` and of
`lightmaptexs[lmid]`, and writes `diffuse.rgb * lightmap.rgb * colorparams`,
where `colorparams` is GL's `2 * vslot.colorscale` — Sauerbraten bakes at half
scale and the world shader doubles it. Miss is
transparent so HUD, sky, and anything not in the TLAS stay vanilla GL.
A dynent / mapmodel primary hit is also transparent so the GL model shows.
Expected. No fog, dynlights, bump, envmap, scrolling, or model-skin shading.

The extra data (per-vertex UVs, the index buffer, per-triangle slot/lmid, and
the texture copies) is uploaded at `allchanged()` only, into SSBOs and sampled
2D arrays that survive after the BLAS vertex/index buffers are freed. Copies
are CPU `glGetTexImage` at rebuild, not a new NT-handle import per texture.
Reserved lmids (`LMID_AMBIENT` / `BRIGHT` / `DARK`) are constants in the shader
so a 1×1 layer is never sampled as a packed atlas. GL's `brightengeom` remap is
applied at rebuild so unlit coop-edit maps match the rasteriser. Unique
opaque-world diffuse textures are capped at 256; overflow faces go magenta and
do not crash.

If `hwrtrayquery` is 0, or the shade upload fails, one log line, modes 1–5 and
`rtaodebug` keep working, `hwrt` stays on.

`hwrtdebug` defaults to 7 so that `hwrt 1` is the lighting view. Modes 1–6
remain; a saved `config.cfg` still wins over `data/hwrt.cfg`.

## Phase 5: point lights + one shadow ray

With `hwrtrayquery 1`:

```
hwrt 1
hwrtdebug 7
```

`hwrtlight 1` aliases that. `hwrtdebug` 2/3/4 are not stolen. If `hwrtlight 1`
and `hwrtdebug 6` are both on, 7 wins (and still loses to 2/3/4).

Each pixel traces the same camera primary ray as hit-shade. Closest-hit
interpolates diffuse UV and a vertex normal (uploaded with the surviving shade
SSBO at `allchanged()`, not from the freed BLAS vertex buffer), samples the
existing diffuse copy, evaluates every uploaded radius-0 `ET_LIGHT` (the baker's
unlimited omni: `newent light 0 r g b`, packed first so the 256-cap cannot
drop them) plus the `hwrtdlights` nearest *finite-radius* ents (default 1,
cap 4), Lambert × linear attenuation (`1 - mag/radius` when radius > 0,
intensity 1 when radius is 0, matching `lightreaching()`), and traces one
shadow ray from `hit + 0.25*N` toward that light against the world TLAS. Closest-hit on the shadow ray skips that light.
`ambient` is added as a constant so a map with no nearby lights is not a black
hole. That constant is not sunlight/skylight. Sun and sky are evaluated after
the point-light loop (see below). Miss is transparent. Spotlights
are skipped. `EF_NOSHADOW` still lights, with no shadow ray. Dynlights are not
copied. The light SSBO is updated every frame (it is tiny) and does not rebuild
the BLAS.

Grain is 1 spp.

If `hwrtrayquery` is 0, or the light SSBO / pipeline fails, one log line, modes
1–6 and `rtaodebug` / `hwrtshade` keep working, `hwrt` stays on.

## The world overbright

Sauerbraten stores lightmaps at half scale and the world shader doubles them:
`renderva.cpp` builds `colorparams` as `2 * vslot.colorscale` and
`glsl.cfg`'s `worldshader` ends in `gl_FragColor = diffuse * lm`. The RT packed
`vslot.colorscale` alone, so every ray-traced wall was exactly half of what the
rasteriser draws. The model path had already picked its own copy of that factor
up (`light.rgb *= 2.0` in the model shader, mirrored in `hitlight.comp`), which
is why models matched GL and the world did not.

`hwrtworldgain` (default 2) restores it, and it multiplies the light the RT
*traces* — point lights, sun, sky — not the `ambient` constant. That split is
deliberate: ambient is the floor that sets how dark a shadow is, so leaving it
alone means a pixel no light reaches keeps exactly the value it had, a lit pixel
roughly doubles, and the lit-to-shadow ratio goes up instead of down. Raising
the gain therefore adds contrast; it cannot flatten the image. It rides in the
spare slot of the 64-byte light-env header, so it is live from the console and
the 128-byte push constants do not grow.

Mode 6 takes the same factor as a literal 2, because that view is supposed to
*be* the bake.

## Sun / skylight

Mode 7 also evaluates the map's existing CubeScript sun and sky. No new debug
mode. The values are the same ones `worldio` writes from the `.ogz` header and
the baker already uses (`sunlight`, `sunlightscale`, `sunlightyaw`,
`sunlightpitch`, `skylight`). They ride in a 48-byte header in front of the
point-light SSBO so the 128-byte push constants do not grow. Changing the sun
direction does not rebuild the world BLAS.

Sunlight: `sunlightdir` is the direction *toward* the sun (baker convention).
Lambert is `max(dot(N, sunlightdir), 0)`. N is the triangle's geometric
normal (cube faces), not the view direction: otherwise sunlight sticks to
eye height on walls. One shadow ray from `hit + 0.25*N`
along `sunlightdir`, `tmax` = `farplane`. A miss is lit by
`sunlightcolor/255 * sunlightscale`; a hit is not. If `sunlight` is 0 the sun
ray is skipped entirely — there is no fake directional light.

Skylight: `hwrtskyrays` cosine-weighted hemisphere rays around the world
normal (default 4, cap 4). Misses count toward a visibility fraction; the
cosine pdf already matches a Lambertian sky, so there is no extra weight. A
hit (roof, overhang, another player) contributes nothing. If `skylight` is 0
the sky rays are skipped. Ambient is *not* added again here.
`hwrtskyfilter` (default on) then averages that fraction across the 16×16
workgroup, one 5×5 on the same plane (world vs models kept apart). Four
rays already carry the grain; two 7×7 passes smeared mid/far lighting.
World vs model is the smear gate, not hit-distance along the view: a floor
seen at a graze changes T across a kernel and used to leave leftover dots.
That is the whole spatial denoiser: skyvis is the only stochastic term.
`hwrtskybluenoise` (on) only changes where those rays point for the world
pixels NRD denoises; see "Sky-ray blue noise" below.
The workgroup is 16×16 so a 5×5 kernel has a usable neighbourhood; 8×8
showed as a tile, and an extra skyvis apron around each tile cost more
than four rays.

`hwrtskytemporal` (default 32) then accumulates that filtered fraction across
frames. Nothing about skyvis depends on where the camera is, so a hit that is
recognised as the same surface point can inherit the running average that point
already carries rather than a colour-clamped version of it. The hit position,
stored relative to the camera that wrote it, goes through the previous
dispatch's `viewProj` to find the entry; two tolerances validate it, tight
across the surface normal because that is what tells the two faces of a corner
apart, loose along the plane because a floor seen at a graze covers a lot of
ground per pixel and a camera that moved lands on a neighbour of the same
surface under the same sky. A moving occluder changes real visibility and no
geometric test can see that, so the new estimate is also compared against the
history and a gap wider than 3.5σ restarts the pixel. σ is per-pixel, not a
constant: a Bernoulli mean of *m* samples spreads at most `0.5/sqrt(m)`, and
*m* is the ray count times the taps the 5×5 actually found, so the threshold
opens exactly where the estimate is noisiest — half-occluded sky, p near 0.5,
which is every surface at the foot of a wall. A fixed threshold was the first
attempt and it throttled accumulation in those same pixels, which is why 96
frames measured worse than 32 before this.

On the world that fraction lerps from `ambient` up to `skylightcolor`, the way
`calcskylight` does (`lightmap.cpp:666`), and the result is `max`ed against
sun + lamps instead of summed with it — because that is what the bake next
door holds. `finishlightmap` writes `max(skylight, sun + lamps)`
(`lightmap.cpp:891`) into a lumel that is 8 bit and therefore cannot pass 1
before `colorparams` doubles it. Summing the two and skipping that clip put
open ground at about twice what the rasteriser draws: on `authentic`, where
`sunlightscale` is 2, the sun term alone is 1.32 against GL's clipped 1.0 and
the sky added another 0.63 on top. With both fixed, sunlit surfaces land
within 4-6% of GL.

Ordinary mapmodels take that same combine, and the reason is that something
*does* bake them: `rendermodel.cpp` calls `lightreaching()` at the model's
origin, which samples the very lightmap the world reads, and `finishlightmap`
already wrote `max(skylight, sun + lamps)` into it. So a tree under an open sky
has the sky competing with the lamps in vanilla, not stacking on them, and the
model path summing the two put it above everything around it. Only
`HWRT_GEOM_FULLBRIGHT` (players, bots) keeps the plain sum, which is what its
studio-key material was tuned with.

The other half of the same correction is the unlit floor. `glsl.cfg`'s
modelshader multiplies the *whole* of `lightreaching()`'s colour by the wrap
`clamp(i*(i*0.21 + 0.30) + 0.29, 0, 1)`, so 0.29 is a **fraction of the light
that arrives**, not a level: a tree in a dark corner is dark in vanilla.
Flooring the ambient at 0.29 instead made every mapmodel `1/ambient` too
bright wherever the map is darker than that — ten times on a night map, which
is how a tester found it: trees, plants and rocks glowing out of walls that
had gone darker. Ambient and sky have no direction to feed the wrap, so they
take `MODEL_AMBWRAP` = 0.583, the wrap evaluated at the mean cosine of a
cosine-weighted hemisphere (`E[n·l]` = 2/3). Measured on `authentic` at 1440p,
over pixels lit in both renderers so shadow placement cannot skew it: foliage
lands at 0.87-1.11× GL and courtyard stone at 1.06-1.16×. Before the fix the
same foliage measured 2-3× GL on a night map.

A courtyard on `nucleus` / `triforts` should be brighter than point lights +
ambient alone. A crate, pillar, or thirdperson player casts a hard sun
shadow. Under a roof the sky term drops. `box_demo` (no sun, no sky, no
light ents) stays albedo × ambient.

The lighting view used to replace the skybox with a flat stand-in plus a
disk on `sunlightdir`, because a skybox JPG often paints a sun that does
not move with `sunlightyaw` / `sunlightpitch`. That stand-in looked less
like the map, so the default is now the cubemap. `hwrtsundisk` 1 restores
the aligned disk (the atmo disk is kept if `atmo` is already on).
`sunlight` 0 draws no disk either way. `hwrt` 0 leaves the map sky
untouched. `hwrtlooksun` aims the camera at the traced sun.

When that same lighting view is showing, the GL world pass binds `LMID_BRIGHT`
(the existing 1×1 unlit reserved tex, same remap `brightengeom` uses) instead
of the batch's baked lmid. Bump's companion slot follows (`LMID_BRIGHT1`).
Grass does *not*: it keeps its baked lmid and the plain `grass` shader, which
is to say the grass pass is exactly vanilla's. Neutralising the bake is right
for anything the composite rewrites, and grass is the one pass it does not —
autograss is deliberately outside the TLAS and GL draws it after the composite,
so the bake is the only light a blade can get. Binding `LMID_BRIGHT` there cost
both channels of that texture. Its flat rgb lit a blade identically in full sun
and in a black courtyard, which drew the eight-wedge structure of
`gengrassquads` in high contrast from any dark or elevated spot; and it carries
no alpha plane at all, so the sampled coverage read 1 everywhere and blades grew
straight across a texlayer's bare path, that alpha being where Sauerbraten keeps
blend coverage (`LM_ALPHA`, `lightmap.cpp:1549`). Traced light is in the same
units as the bake, so a bake-lit blade sits correctly against RT-lit ground.
Overlay 1, RTAO (5), hit-shade (6), diagnostics
2/3/4, `hwrt` 0, `hwrtfailed`, and `!hwrtrayquery` keep sampling the bake.
Mode 6 must: that view *is* the bake. If the reserved texs are missing, one
log line and the bake stays on. `calclight` is untouched.

## Sky-ray blue noise

`hwrtskybluenoise` (default on) changes where the sky rays point, nothing
else: same count (`hwrtskyrays`), same cosine-weighted estimator, same 5×5,
same history, same NRD settings. It applies to the world pixels NRD denoises.

The tile is `shaders/bluenoise_tab.h`: two independent 128×128
void-and-cluster masks (Ulichney), 16 bits each, generated from scratch by
`tools/generate-hwrt-bluenoise.py` (deterministic, no third-party data). A
scalar void-and-cluster mask is the property that matters here: every
threshold of it is an even scatter, so an occluder edge that cuts the
hemisphere along u1 or u2 splits neighbouring pixels in the right
proportion and the 5×5 averages less error. A first attempt with a 2D-vector
(Georgiev–Fajardo) tile spread the pairs but not each coordinate and gave
nothing through the filter; it is not used.

Per pixel, the base sample is the tile value, in 0.32 fixed point. Every
frame adds the same R2 step (plastic-number Kronecker sequence) to every pixel:
a constant shift mod 1 keeps the tile's spatial layout, and each pixel walks a
low-discrepancy sequence over time, so its running mean converges to the same
value the pcg rays do (checked: no bias without temporal reuse, raw or
filtered). The four rays of a pixel are the base plus k·(1/2, 1/4), a 4-point
lattice, so a pixel already covers four azimuths and both radius halves.
`hwrtskystable` keeps frame 0. A spatiotemporal (STBN-style) 64×64×32 mask was
built and simulated as well; for 1/age accumulation and REBLUR's history it was
no better than the R2 shift and eight times larger.

Measured (lab, 2026-10-01, RTX 4070 Ti SUPER, 1600×900 window, SDR, still
camera, 11 canon maps, 9 with sky light; reference = same chain with 256
pcg rays per pixel averaged over 24 frames; error = RMSE of the skyvis view
over world pixels, final image after DLAA/DLSS): with NRD the error falls by
10 % on the first frame and 15 % after 64 frames in native, 7 % / 15 % with
DLAA, 7 % / 16 % with DLSS Quality; frame-to-frame flicker of a still camera
falls by 24 / 19 / 19 %. Under a 1.5°/frame camera turn the error is unchanged
to 6 % lower and the lag (trailing bias) identical. No 16-pixel workgroup grid
and no 128-pixel tile repetition above chance. GPU cost of the lighting pass:
no measurable change (≤ 0.01 ms on 1.2–1.7 ms; triforts within its own
block-to-block noise). Classic lighting never runs this shader.

Why the skyage path (Sauer filter, models, NRD off) keeps the pcg hash: the
first frames do improve (about −30 %), but its hold re-reads the frozen
history every frame through a sub-pixel reprojection offset, which slowly
smears and darkens it (−0.005 skyvis over 64 frames with a constant input,
pcg or blue alike, `hwrtskystable 1`). Blue-noise estimates sit within the
hold threshold more often, so they freeze more and inherit more of that drift:
after 64 frames the error was 5–8 % higher, the mean about 0.0025 lower, the
workgroup tile borders stood out more against a cleaner interior, and the
frozen residual repeated faintly with the 128-pixel tile. That drift is a
skyage defect of its own, worth fixing before revisiting this.

## Phase 6: mapmodels and dynents in the TLAS

With `hwrtrayquery 1`:

```
hwrt 1
hwrtdebug 4
```

The world BLAS is unchanged (still rebuilt only at `allchanged()`). Each unique
model gets one BLAS from its BIH triangles (`mesh.xform` baked in), capped at
64. Opaque meshes and `MESH_ALPHA` (tree foliage, CTF flags) share that BLAS
as two geometries: the trunk keeps `VK_GEOMETRY_OPAQUE_BIT_KHR`, the leaves
do not. Lighting and silhouette rays omit `gl_RayFlagsOpaqueEXT` so the
leaves arrive as candidates; `skins.a` is tested against `mdlalphatest`
(default 0.9, stored on the vertex as `pad1`). A hole lets the ray continue,
so a tree casts a foliage shadow and still looks like leaves, not a green
panel. The live TLAS is world instance
`customIndex` 0 plus up to 4095 mapmodel / dynent instances, rebuilt on the
frame command buffer after `glready` with no CPU wait.

Order in that table is not the order the mapper laid the entities out, and
that matters when a map does not fit. Dynents, ragdolls, pickups and flags are
written as they are found and can never be cut; mapmodels queue and go in last,
nearest first, trimmed at `hwrtmaxinsts`. Both halves of that were bugs before:
`cmvalley` posts 2614 mapmodels against the old hard 1024, so the table filled
with scenery in map order and logged `1023 mapmodels + 0 dynents` — no player
in the match was traced — while the 1591 mapmodels it dropped were an arbitrary
selection that included trees a few metres from the camera. A dropped model is
worse than unlit: the ray carries on through it and lights whatever stands
behind, so a missing tree shows the tree behind it through its own trunk.
Instance transform is
translation + Z yaw (players also pitch; `renderclient` yaw+90; first-person
`camera1` is left out unless `thirdperson` is on). CTF flags are not
mapmodels: `cmode->rendergame()` draws them through `rendermodel()`. The
main pass records those `flags/*` calls (skipping `MDL_HUD` / `MDL_GHOST`)
and the same rest-pose BLAS is instanced at that origin + yaw, so a planted
flag casts a shadow. The HUD gun stays GL.

`hwrtdebug` 4 colours every triangle, including dynents — that is the silhouette
proof. Mode 7 shadow rays and mode 5 AO confirm those triangles, so a pillar
mapmodel casts a hard shadow on a world wall.

Player animation: instance transform (origin + yaw + pitch) updates every
frame. Skeletal models (MD5 / IQM) and vertex-animated MD2/MD3 are
CPU-skinned into a per-pose BLAS, keyed like the engine's skeleton cache
(animstate, pitch, partmask, ragdoll pointer) so eight players in the same
run cycle share one or two refits. The BLAS is allocated with
`ALLOW_UPDATE_BIT` and refit with `MODE_UPDATE_KHR` on the frame command
buffer; a full `MODE_BUILD_KHR` is issued every 24 updates of that slot,
capped at 2 rebuilds per frame, round-robin. Cap is 32 animated BLASes
per in-flight frame; overflow logs once and those extras fall back to the
rest pose. Ragdolls are back in the TLAS (world-space instance, identity
transform). `hwrtstalls` must stay 0: there is no `WaitIdle` on this path.

The game may supply `hwrtdynentmdlname` for player / monster / movable
names; a weak fallback returns NULL so mapmodels still work against an
unpatched fpsgame. Pickups bob and spin in the raster pass, so the game
may also supply `hwrtentxform` to hand back the same origin and yaw
`renderentities` used; the weak fallback is the static `e.o`.

### The model mask

A rest-pose BLAS standing where GL draws an animated model is a mismatch, and
mode 7 makes that mismatch loud: the GL world pass is bound to `LMID_BRIGHT`,
so any pixel the composite hands back to GL is *full bright*. The old rule —
write `vec4(0)` on a primary hit whose `customIndex` is not 0 — therefore
punched a rest-pose-shaped hole and lit it up. On a moving player that reads as
a glowing T-pose wrapped around the model. When the mask *is* used, GL
snapshots the depth buffer twice, around the model passes:
`hwrtsnapworlddepth()` before `rendermapmodels()` and
`hwrtsnapscenedepth()` after `rendergame(true)`. The composite discards
every pixel where the second copy is nearer — exactly the pixels GL's
own model raster won.

Mode 7 still takes those snapshots. A shaded model hit is written at
alpha 0.5; a world hit stays at 1. The composite then discards only a
world hit that landed behind a GL model the TLAS never saw — grass
and world-alpha cubes, the same overwrite the hudgun had before it
moved past the composite. A shaded player stays. Discarding every model
pixel would throw the skin away; that is the old mask trap, and it is
why alpha is the split rather than "any nearer scene depth".

`hwrtmask` stays as a cvar. Mode 6 still writes `vec4(0)` on a model
hit and still masks, so the bake view keeps GL's raster of the models.
Mode 7 falls back to the same path for an instance the shader could
not shade (overflow, static OBJ with no attrs, pose not yet known).
`hwrtmask 0` turns the snapshots off.

### The GL depth mask

Two depth snapshots answer "which pixels did GL's models win", and that
is not the same question as "is this traced hit the surface the player
can see". Grass, water and world-alpha cubes are still not in the BLAS,
so a mode 7 primary ray goes straight through them and would commit the
pickup or player standing behind. A *world* hit there was already dropped
by the snapshot pair; a *model* hit was kept unconditionally.

Neither snapshot can decide that case, because neither knows how far
away the traced hit is. So the test moved into the shader, where
`hitT` is: `hwrtdepthmask` copies the window depth into the same shared
`R32F` RTAO uses (binding 9 of the lighting pipeline), reconstructs the
world position with `invcamproj` exactly as `rtao.comp` does, and stores
transparent when GL's surface is nearer than the hit by more than
`max(0.75, 1.5% × hitT)` world units. The composite then hands that
pixel back and GL's own raster of the untraced surface survives. The
slack is what absorbs depth-buffer precision and the drift between a
CPU-skinned BLAS and GL's pose.

`hwrtrender()` runs immediately after `hwrtsnapscenedepth()`, so the
window depth it copies is the world plus both model passes and nothing
GL draws later — the hudgun, decals, water, grass, materials, alpha
geometry and particles are all still to come and cannot mask anything.
The test runs before any attribute fetch or lighting, so a hidden pixel
costs one image load instead of a full shade. Only one mode runs per
frame, so RTAO and the depth mask never want the shared depth at once.

Tree foliage and CTF flags are in the TLAS as non-opaque geometry with
a `skins.a` test in every `hitlight.comp` ray (primary, lamp, sun, sky).
The depth mask is no longer the crutch for the canopy; it stays on for
the holes that are still untraced (grass, water, world alpha).

Primary rays skip models through the instance cull mask only when
shading is *not* live. Opaque BLAS triangles still commit in hardware;
`MESH_ALPHA` is a non-opaque geometry, so lighting rays omit
`gl_RayFlagsOpaqueEXT` and `rayQueryProceedEXT` offers those triangles
as candidates for the `skins.a` test. The world instance carries
`HWRT_RAYMASK_WORLD` and every model instance carries
`HWRT_RAYMASK_MODEL` (`hwrt.h`, mirrored as `RAYMASK_` in
`hitlight.comp`). Shadow, sun and sky rays always ask for
`RAYMASK_ALL`, so models still self-shadow and cast.

If `hwrtrayquery` is 0, or the dynent path fails, one log line, the world-only
TLAS stays, modes 1–7 keep working, `hwrt` stays on.

Phase 6 was exercised on the same machine as phase 5: see “What has actually
been verified” above.

## How a frame works

```
[GL]  render the world (lighting view: lightmap TMU is LMID_BRIGHT)
[GL]  if the model mask: copy window depth (world only)
[GL]  rendermapmodels() + rendergame(true)
[GL]  if the model mask: copy window depth again (world + models)
[GL]  if RTAO or the mode 7 depth mask: copy window depth into the shared R32F
[GL]  first-use rest-pose model BLAS (WaitIdle allowed here, not on the TLAS update)
[GL]  glSignalSemaphoreEXT(glready) + glFlush
[VK]  vkQueueSubmit waiting on glready, signalling vkdone
[VK]    CPU-skin animated dynents; refit their BLASes (MODE_UPDATE, or a
        budgeted MODE_BUILD every 24 updates)
[VK]    rewrite TLAS instances (world + mapmodels + dynents)
[VK]    compute shader writes the shared image
[GL]  glWaitSemaphoreEXT(vkdone)
[GL]  composite the shared texture
[GL]  decals, water, grass, materials, alpha geom, particles
[GL]  velocity image (world from pre-gun depth; FPS gun rasterised in avatar space)
[GL]  motion blur, look AO, looktaa (looktaa skipped while hwrtdlaajitter is on)
[GL]  colour hudgun (avatar FOV + avatardepth; same pixel jitter as the world)
[GL]  hwrttemporalcapture: RGB8 + D24 HUD-less image (gun in, reticle/UI out)
[GL]  glare, fog overlay, postfx
[GL]  HUD, then swap
```

The composite sits at one fixed point in `gl_drawframe`, immediately after the
model passes it has to mask against and before everything GL still draws over
the world. That ordering is not a preference: the composite is an opaque
fullscreen quad, and decals, water, grass, materials, alpha geometry and
particles are all deliberately absent from the BLAS, so a primary ray passes
straight through them and hands back the surface behind. Anything GL rasterises
over the world before the composite is painted out. Everything after it
survives, including the fullscreen passes (`addglare`, `addlookao`,
`addmotionblur`, `renderpostfx`), which used to be erased whenever they were
enabled. The one departure from vanilla ordering is `renderdecals`, which moves
down past the two model passes; decals never write depth
(`glDepthMask(GL_FALSE)` plus a polygon offset), so an opaque model covers them
from either position and `hwrt 0` renders the same image.

`hwrtrender` saves and restores `GL_CULL_FACE`, `GL_DEPTH_TEST`, `GL_BLEND` and
the polygon mode around the composite. It runs mid-frame now, so it borrows GL
state the following vanilla passes still depend on, and in editmode wireframe
the quad would otherwise be drawn as four lines.

The two binary semaphores are the only synchronisation. There is no `glFinish`,
no `vkQueueWaitIdle` and no readback on the per-frame path; `vkDeviceWaitIdle`
appears only in teardown, resize, the world BLAS rebuild, and first-use model
BLAS builds. The live TLAS is rebuilt on the frame command buffer with no CPU
wait. The CPU never waits on the frame path: each in-flight frame owns a
command buffer and a fence, and a fence is only inspected when its slot comes
up for reuse three frames later, by which point it has always been signalled
(`hwrtstalls` measures exactly this and reads 0).

The `glFlush` after the signal is required. Without it the signal can sit in GL's
command buffer while the Vulkan submit that waits on it is already queued, and
the two queues deadlock.

## Files

| File | Contents |
|---|---|
| `hwrt.h` | internal contract: the Vulkan entry points to load, the device and interop structs |
| `hwrt.cpp` | CubeScript variables, `hwrtinit` / `hwrtrender` / `hwrtcleanup`, failure handling |
| `vkdevice.cpp` | runtime loader, instance, physical device selection, logical device |
| `interop.cpp` | GL extension loading, the shared colour + depth images, the semaphores, the composite |
| `trace.cpp` | compute pipelines, per-frame command buffer, submit, and the ping-pong `RGBA16F` skyvis history pair (bindings 13/14, alternated per dispatch, 1×1 fallback if the allocation fails so the pass degrades to off rather than to a crash) |
| `as.cpp` | world BLAS from `valist` / `readva`, identity fallback TLAS, rebuild on `allchanged()`, surviving shade SSBOs (UVs + normals) |
| `dynents.cpp` | rest-pose BLAS per unique model from the BIH; per-pose animated BLAS (CPU skin, ALLOW_UPDATE refit on the frame CB); live TLAS instances; per-BLAS UV/normal/skin-layer SSBOs and the geometry table `customIndex` indexes |
| `rendermodel.cpp` | `hwrtskinmodel`: CPU skin of bind-pose verts with this frame's bones (skelcache / interpverts), keyed like the skeleton cache |
| `lights.cpp` | `ET_LIGHT` enumeration into a 256-cap SSBO plus a 144-byte sun/sky header (which now also carries the previous dispatch's `viewProj`, its camera position and the temporal blend weight), updated every frame |
| `sky.cpp` | optional GL sun disk along `sunlightdir` (`hwrtsundisk` 1; default off so the map skybox stays) |
| `velocity.cpp` | static-scenery, rigid-mapmodel and skinned-player motion vectors (`RGBA16F`, pixels, +Y up, unjittered when the trial is on) plus the reprojection overlay. GL only; off by default |
| `temporal.cpp` | lot 6 HUD-less RGB8+D24 capture after the FPS gun, trial Halton jitter, unjittered/used matrix pair. No Streamline / NGX |
| `shaders/debug.comp` | the phase 1 probe. Compile with `tools/compile-hwrt-shaders.py` |
| `shaders/silhouette.comp` | phase 2 camera primary ray + `rayQueryKHR`. Same compile script |
| `shaders/rtao.comp` | phase 3 depth reconstruct + hemisphere `rayQueryKHR`. Same compile script |
| `shaders/hitshade.comp` | phase 4 camera primary ray + interpolated UV, `diffuse * lightmap`. Same compile script |
| `shaders/hitlight.comp` | camera primary ray + Lambert point lights + sun + sky + shadow rays. Mode 7 also shades model hits (`SHADE_MODELS`) from a parallel attribute SSBO. `hwrtskyfilter` averages skyvis in 16×16 shared memory before the combine, then `hwrtskytemporal` reprojects and blends it against the ping-pong history. Same compile script |
| `shaders/bluenoise_tab.h` | 128×128 sky-ray blue-noise tile (two void-and-cluster masks, 16 bits each), generated by `tools/generate-hwrt-bluenoise.py`, uploaded once into hitlight's binding 25 |

Engine hooks: `gl_init` calls `hwrtinit`, `gl_drawframe` calls `hwrtrender`
right after `hwrtsnapscenedepth()` and before the decals and the first-person
hudgun, and (when `hwrtvelocity` or `hwrtveldebug` is on) stencil-marks
opaque world cubes and rigid mapmodels, unmarks players / water / grass /
alpha / particles / deforming models, then writes the velocity image after
particles (camera motion from depth, rigid object motion from a depth-tested
alphatested pass); the first-person hudgun is then drawn into that image
under the avatar projection (lot 5) so its silhouette carries vectors.
`allchanged` calls `hwrtrebuildworld` and
`hwrtresetvelocity`, `cleanupgl` and the
main `cleanup` call `hwrtcleanup` / `cleanuptemporal`, `drawskybox` keeps the map cubemap
unless `hwrtsundisk` 1 (then it skips the painted sun and calls
`hwrtdrawalignsun`), `renderva` /
`grass` bind `LMID_BRIGHT` on that same view, and `main.cpp` execs
`data/hwrt.cfg`.

## Things worth knowing before extending this

**The Vulkan device is chosen by UUID, not by score.** On a hybrid machine, any
physical device other than the one GL is running on silently produces a shared
allocation that reads back as zeros. If GL will not report a device UUID the
layer disables itself rather than guess.

**Win32 NT handles stay open until teardown.** `glImportMemoryWin32HandleEXT`
does not take ownership, and NVIDIA's GL only resolves the memory handle when
`glTexStorageMem2DEXT` attaches the storage. Closing the handle right after the
import — which the spec appears to allow — produces a texture that reads as
zeros with no GL error anywhere. This cost most of the debugging time on phase 1;
`hwrtdebug 3` exists because of it. The Linux fd path is the opposite: the import
takes ownership and the fd must not be closed.

**Row 0 of the shared image is the bottom of the screen.** The composite samples
with the GL convention, so nothing flips on the way back, and the compute shader
writes with the same convention. The shared depth image uses the same origin:
`glCopyTexSubImage2D` from the window copies with the GL lower-left origin.

**The colour image is `VK_FORMAT_R16G16B16A16_SFLOAT` / `GL_RGBA16F`, optimal tiling,
one mip, dedicated allocation** (it was RGBA8 before the HDR work). Depth is a second
allocation, `VK_FORMAT_R32_SFLOAT` / `GL_R32F`, written by GL and read by the RTAO
shader. The DLAA/DLSS/FSR images are separate shared images owned by `dlaa.cpp`.

**Queue family ownership is not transferred.** Both barriers use
`VK_QUEUE_FAMILY_IGNORED` rather than releasing to and acquiring from
`VK_QUEUE_FAMILY_EXTERNAL`. This matches what the widely used samples do and what
the drivers expect from `EXT_external_objects`, but it is a simplification of
what the spec describes for exclusive sharing across an external boundary.

**The engine log is switched to unbuffered in `hwrtinit`.** A stuck semaphore
takes the process down without unwinding, and the last lines before a hang are
exactly the ones a block-buffered log loses.

## Known holes

- Radius-0 `ET_LIGHT` ents are the baker's unlimited omni (old maps such as
  `academy`: `sunlight` 0, a painted sun on the skybox, one `newent light 0`
  high up). Mode 7 always evaluates those, in addition to the N nearest
  finite lights. Do not guess a directional sun from the skybox JPG.
- Reflections (architecture phase 7) stay blocked until asked. Lighting
  completeness on both map classes (header sun, and radius-0 omni) comes
  first. No last-frame colour, no G-buffer for mirrors.
- Player BLASes follow the GL pose (CPU skin of bind pose + this frame's
  bones, keyed like skelcache). Shadows follow the run cycle and ragdolls
  follow the corpse. Mode 7 shades those hits with the same lights as
  the world.   `hwrtmask` still snapshots in mode 7: a shaded model is
  kept, a world hit behind an untraced GL surface (grass, world
  alpha) is dropped so GL's raster survives, the same way the hudgun does.
- `fullbrightmodels` (Options → Fullbright Player Models) still marks
  `ENT_PLAYER` as `HWRT_GEOM_FULLBRIGHT`. Mode 7 does **not** copy GL's
  flattening floor (at Overbright 150 that is a 1.5 plate with no Lambert).
  The menu preview strips `MDL_FULLBRIGHT` and lights with a studio key +
  spec + the model's envmap cubemap (`masks.b`); in-game those instances get
  the same material: wrap volumes, Blinn-Phong spec (`masks.r`), the
  `mdlenvmap` cubemap (not a world mirror), a studio fill only where world
  lights are weak, costume glow (`masks.g`), and a modest unlit floor (0.32)
  so a dark hall is not a black silhouette. The Overbright option no longer
  copies GL's 1.5 plate. Crates, pickups, mapmodels and monsters stay Lambert. A player standing in a teleporter
  still picks up that colour from the glow omni.
- `game::ragdolls` is in the TLAS, so a corpse GL keeps after its owner
  respawned is not painted over; `hwrtstats` counts those instances separately
  and `hidedead` is answered by the game, not guessed in the engine.
- Everything GL draws over the world lands after the composite, not just the
  hudgun: decals, water, grass, materials, alpha geometry, particles, glare,
  the underwater tint and the fullscreen postfx passes. None of them are in the
  BLAS, so a primary ray goes straight through and the composite hands back the
  lit surface behind; the model mask only covers what rendermapmodels() /
  rendergame() rasterised. The composite writes colour, never depth, so they all
  still sort against the world and the models exactly as in vanilla.
- Still drawn before the composite, so mode 7 paints over it: editmode's
  `renderoutline`. The reflection / refraction targets and the glare source are
  rendered from the GL-lit scene as well, so a water mirror and bloom follow the
  bake rather than mode 7.
- Mode 6 is `albedo × bake × colorparams` and nothing else, so it still sits
  above vanilla wherever GL does more than that: no fog (median 1.31 on
  `triforts`, which is large and outdoor) and no bumped-world lighting, where
  GL modulates the lightmap by a dot product the RT does not compute. It lands
  on 1.01 where neither applies (`box_demo`). Mode 6 also still omits the
  additive world glow term; mode 7 is the one that has it.
- Glow: world `TEX_GLOW` is sampled and added after lighting (same as
  `glowshader`). Model skins mix toward `albedo * curglow` on `masks.g` (same
  as `modelshader`). Every teleport ent is packed as an always-eval lamp.
  The default ring stays `flags` 3 at the hole in the ring, with shadow rays,
  so the metal rim gobos as it spins. Custom models and modelless portals
  (`attr2 != 0`) are also `flags` 3 (Lambert + shadow, short radius, dimmer
  colour) so they do not flood through walls the way the first triforts pass
  did. Tint still comes from a nearby world glow pad or a nearby magic/pad
  mapmodel (triforts' blue vs red hexes). The filled plate stays in GL; the RT
  BLAS still drops the default ring's central triangles. Jumppads (default
  off) stay `flags` 3 when enabled. Clustered glow screens stay `flags` 2.
  Lava (material nappes **and** world faces whose texture name contains
  `lava`) is `flags` 3, coloured with `lavacolour`. `m.skip` is a GL batch
  stride, not a merge, so lava gathering walks every visible face. Lava is
  not in the BLAS (a later GL material) but its faces are clustered at world
  rebuild. `hwrtteleportlight` / `hwrtjumppadlight` turn just that
  projected omni off (defaults 1 / 0); the additive glow on the mesh stays.
  Pulse glow is sampled at rebuild / first fill, not every frame.
- Mode 7's fill is not the bake's fill and is not meant to be. `calcskylight`
  samples 17 fixed directions that all sit at 50° elevation or higher, so the
  bake escapes an enclosed courtyard far more often than a cosine hemisphere
  does, and `generatelumel` sums *every* light that reaches a surface while
  mode 7 takes the radius-0 omnis plus `hwrtdlights` nearest finite ones.
  Both make the bake flatter and brighter in shadow. That is the difference the
  lighting view exists to remove, not a bug to close.
- Alpha, water, sky and clip surfaces of the *world* are left out of the
  BLAS on purpose (sky miss should stay the skybox; `readva` already omits
  alpha). texlayer blend floors are in: one triangle, grass mixed with dirt
  from lightmap alpha. An AO ray that misses is unoccluded; outdoor maps stay
  bright. Autograss blades are a later GL pass and are still untraced.
- Model `MESH_ALPHA` (tree foliage, CTF flags, alpha cloth) is in the
  rest-pose BLAS as a second, non-opaque geometry next to the opaque
  trunk. Lighting rays omit `gl_RayFlagsOpaqueEXT`; opaque triangles
  still commit in hardware, alpha triangles arrive as candidates, and
  `skins.a` is tested against `mdlalphatest` (vertex `pad1`, Sauer default
  0.9). A hole lets the ray continue, so the canopy casts a foliage
  shadow and still looks like leaves rather than a green panel. The same
  test runs on shadow / sun / sky rays from the world, crates, pickups,
  mapmodels and players (same `hwrtshadowself` masks as the walls). A
  player ray skips its own TLAS instance so the body does not self-occlude;
  the look stays bright (floor + studio). Future reflections can use the
  same occlusion, including foliage. `hwrtdebug` 4 confirms every
  alpha candidate (no skin sampler there) so the silhouette shows the
  triangles are in the TLAS; holes are judged in mode 7. Skin-array
  overflow (192 layers) logs once and those extra meshes stay lit by GL
  rather than turning magenta or shading as a white untextured sheet.
  CTF `flags/red|blue|neutral` are pinned at map load so a packed map
  cannot spend the atlas on crates first. World-cube alpha, autograss
  blades and water stay out.
- Depth reaches Vulkan as a shared `R32F` colour image (GL copy of the window
  depth), not a true D24/D32 import. There is still no G-buffer of normals or
  albedo.
- Dedicated velocity image for **static** opaque world cubes and **rigid**
  mapmodels (`hwrtvelocity`, off by default). Format is `RGBA16F`: RG =
  motion in pixels from the current pixel to where that surface sat last
  frame (so `history_uv = uv + motion / resolution`), B = 1 if the pixel is
  covered scenery, a rigid mapmodel, or a colour-visible skinned player.
  Origin is OpenGL (bottom-left, +Y up).
  World cubes use the previous camera matrix on reconstructed depth. Rigid
  mapmodels (translation / `mdlspin`, no vertex or skeletal deformation)
  are rasterised into the same FBO against a blit of the scene depth, with
  the skin alphatest, using this frame's object transform and the matching
  previous one keyed by entity index + model. A vis-culled model can still
  write if it is in front of that depth (the RT composite may have painted
  it); a model behind a wall fails the depth test and cannot punch the wall.
  Missing object or camera history zeroes motion and does not invent a
  vector. Matrices are the main-camera `camprojmatrix` snapped right after
  `setcamprojmatrix()` in `gl_drawframe`. `hwrtdlaajitter` (off by default,
  not saved) applies a Halton(2,3) NDC offset once per main view; motion
  uses a parallel **unjittered** pair so a still camera stays near zero
  while the raster still shifts. `looktaa` jitter is skipped while the
  trial is on (no double offset); with the trial off, looktaa behaves as
  before. History is dropped on map load, resize, turning the
  feature off, toggling the trial jitter, and entering reprojection from a mode that was not copying
  colour. A camera cut zeroes motion for that frame, **keeps coverage**,
  and discards colour history so the diagnostic cannot reproject the pre-cut
  image with null vectors; the next frames reseed from the new view and
  matching matrices.
  Coverage is a stencil mark written while those surfaces rasterise
  (mapmodels that are `EF_ANIM`, skeletal, or vertex-deforming are unmarked,
  as are pickups, flags, water, grass, world-alpha, particles, attached
  vwep/armour, ragdolls, and the first-person body when it is only a shadow
  occluder). Colour-visible `ENT_PLAYER` skeletal meshes (lot 3: `mrfixit`
  as local third-person / bot) write stencil 3 and receive their own
  per-vertex vectors from the dualquats actually stored last frame — not
  from rewinding `lastmillis`. Other playermodels share that path but are
  not claimed. Spinning rigid mapmodels are marked. Sky never receives the
  mark. Decals and the RT composite keep it: they are paint / lighting on
  the same surface. The first-person hudgun (lot 5, `mrfixit/hudguns/*`)
  is rasterised into this image after the world/character passes, with
  the same avatar FOV / `avatardepth` projection the colour gun uses, and
  its own history slots (not the third-person body or vwep). Unsupported
  pixels are written as `(0,0,0)` and do **not** inherit the wall behind
  them. This is not enough for DLAA/DLSS: ragdolls, monsters, water and
  particle motion are still excluded. `hwrtveldebug` 2/3 reprojects last
  frame's colour as a proof. `looktaa` still derives its own motion from
  the final depth buffer and is unchanged.
  Skyvis grain on the lighting view is reduced in-dispatch instead, spatially
  by `hwrtskyfilter` and then across frames by `hwrtskytemporal`, which
  accumulates the scalar visibility rather than the composited colour and so
  needs neither a velocity image nor a neighbourhood clamp. Do not add a
  G-buffer denoiser.
- The Linux opaque-fd branch has never been through a compiler as part of a
  build, and no Unix build of the client was attempted.
- Multi-monitor DPI changes are only handled to the extent that `screenw` /
  `screenh` change; the shared image follows them lazily on the next frame.
- Multiplayer was not tested. Nothing in the layer touches the protocol, but
  that is an argument, not a run.
