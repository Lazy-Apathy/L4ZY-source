# L4ZY

A Cube 2: Sauerbraten (2020) client with **hardware ray-traced lighting**, **NVIDIA DLAA/DLSS** and **AMD FSR**, **native HDR output**, **local AI chat translation** and a **settings assistant**, **3D sound**, a **drag-and-drop HUD editor**, and other additions: friends list, kill feed, CTF flag timer, automatic match recording, HD-aim demos with X-ray, Tiny Apa bots and more.

It plays on normal Sauerbraten servers. The only extra thing it sends is one `__L4ZY <version>` line when you connect, which vanilla servers ignore (see [Online rules](#online-rules)).

This repository only hosts the **installers and updates**. The game updates itself from the in-game **Updates** menu.

- [Installation](#installation)
- [Updates](#updates)
- [Your files](#your-files)
- [Requirements](#requirements)
- [Menu](#menu)
- [Features](#features)
- [Changelog](#changelog)
- [Licences](#licences)

## Installation

### Windows (10/11, 64-bit)

1. Download `L4ZY-Setup-<version>.exe` from the [Releases](https://github.com/Lazy-Apathy/L4ZY/releases) page.
2. Run it. It installs **for your user only**: no admin rights are needed, and there is no Python or anything else to install. The default folder is `%LOCALAPPDATA%\Programs\L4ZY`.
   - Windows may say "unknown publisher" because the installer is not signed. Click "More info", then "Run anyway".
3. Start **L4ZY** from the desktop or the Start menu.

No translation model is included or downloaded automatically. To use chat translation, open **Chat Translation → Model** in game and click **Install** on Small (Qwen3-4B, 2.5 GB), Normal or Large. The model is downloaded from the official Qwen repository, and you can play while it downloads.

`L4ZY.exe` is the only thing you ever launch. It starts the game, then the private translation service once the game is up. When you quit, it stops everything it started, and nothing else.

## Updates

Main menu → **L4ZY `<version>` – Updates**:

- **Check for updates**: asks the update channel now.
- **Update and restart (`<version>`, `<size>`)**: downloads in the background with progress. You can keep playing.
- **Restart now to install**: the game closes, the update is installed, and L4ZY restarts on the new version.
  - If you stay in the Updates menu, outside a game, it restarts automatically once the download is done.
  - If you are in a match, nothing happens until you click.

How updates are applied:

- Only the parts that changed are downloaded: a game fix is much smaller than the installer, and maps and translation models are never re-downloaded.
- Every file is checked (HTTPS, size and SHA-256) before anything is replaced.
- Nothing is replaced while the game runs.
- A backup is kept. If an update is interrupted or fails, the previous version is put back automatically.
- **Go back to the previous version:** Start menu → *L4ZY – go back to the previous version* (with the game closed).
- Updates never touch your settings, translation corrections or models.

## Your files

| What | Where | Updated by the installer? |
|---|---|---|
| Game settings, screenshots, demos, match videos | `Documents\My Games\L4ZY` | never |
| Translation settings, learned corrections, optional API key, logs | `%LOCALAPPDATA%\L4ZY\translation` | never |
| Translation models (GGUF) | `%LOCALAPPDATA%\L4ZY\models` | never |
| The game itself | `%LOCALAPPDATA%\Programs\L4ZY` | yes |

**Your existing settings are imported.** The first time L4ZY starts, it copies your settings from the Sauerbraten client you used most recently: `Documents\My Games\Sauer-RT`, `Sauerbraten-traduction` or `Sauerbraten`. That covers `config.cfg` (binds, name, options), `autoexec.cfg`, `init.cfg`, `servers.cfg`, `friends.cfg` and your downloaded maps. This happens once, and your other clients are left untouched.

Uninstalling (Start menu → *Uninstall L4ZY*) keeps your settings and models.

## Requirements

Windows 10/11 64-bit with an up-to-date graphics driver. Vulkan is only started when you use ray tracing, DLAA/DLSS or FSR.

### Graphics cards

| Graphics card | Classic lighting | Ray-traced lighting | DLAA / DLSS | AMD FSR | Native HDR |
|---|---|---|---|---|---|
| **NVIDIA GeForce RTX 20, 30, 40, 50 series** (desktop and laptop) | yes | **yes** | **yes** | **yes** | **yes** (with Windows HDR on) |
| NVIDIA GeForce GTX (10, 16 series and older) | yes | no (no RT cores) | no | not tested | no |
| AMD Radeon RX 6000 / 7000 / 9000 series | yes | not tested | no (DLSS is NVIDIA only) | not tested | no |
| Intel Arc | yes | not tested | no | not tested | no |
| Other GPUs, integrated graphics | yes, if Vulkan is available | no | no | not tested | no |

- **Ray tracing** needs hardware RT (Vulkan `ray_query`), plus OpenGL/Vulkan memory sharing (`GL_EXT_memory_object`, `GL_EXT_semaphore`).
  - NVIDIA drivers provide both.
  - AMD and Intel cards have hardware RT, but this sharing has not been checked on their drivers.
  - If something is missing, the game stays on *Classic* lighting and says so in the menu.
- **DLAA/DLSS** need NVIDIA Tensor cores (RTX).
- **AMD FSR** uses the same OpenGL/Vulkan memory sharing as ray tracing.
- **Native HDR** uses the NVIDIA OpenGL/Direct3D bridge.
- **Chat translation** runs on any Vulkan GPU, or on the CPU (slower).
  - Small model: about 4 GB of VRAM.
  - Normal model: about 6–7 GB of VRAM.

## Menu

L4ZY ships its own `menus.cfg`: a new main menu, and an Options hub with pages for Graphics, Display, HUD, Scoreboard, Sound, Mouse, Keys, Console, Recording, Friends and Updates.

- **Chat Translation** is on the main menu and in Options → Game.
- `/traduction` also opens it.
- **Search settings…** and **Settings assistant (AI)…** are at the top of Options.
- **Explanations:** rest the mouse on an option to see what it does (Options → Display → Advanced → *Menus*).

| Setting | Values | Default |
|---|---|---|
| `menuhelpdelay` | ms before the explanation shows: `0` never, `500` to `5000` | `1500` |
| `menuhelpplace` | `0` next to the mouse, `1` at the bottom of the screen | `0` |

## Features

### Ray-traced lighting

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

### Ray tracing performance

- Graphics profiles *Ultra*, *High*, *Medium* and *Low* (Options → Graphics → *Performance* tab → *Profile*); changing one of their options on its own gives *Custom*.
- Teleporter lights are gathered once per light update instead of every frame: faster on maps with many teleporters.
- When the model cache is full, models that cannot be added are skipped before any GPU work.
- Sky rays use blue noise with NRD: cleaner sky light at the same cost.
- Item models (boost, armour, quad…) are prepared behind the loading screen, and new skins are added without waiting for the GPU: no more freeze when an item respawns.
- Room for 128 different models in the ray-traced scene (was 64): items no longer go missing on big maps.
- Lamps are sorted per screen tile, so each pixel only tests the lamps near it: faster lighting on maps with many lamps.
- If ray tracing cannot run (no hardware RT, no Vulkan driver, driver too old, failed to start), *Ray tracing* is greyed out in Options → Graphics → *General* → *Lighting*, with the reason. The settings search and the assistant show it greyed out too.

Commands:

- `/hwrtraison`: why ray tracing cannot run (empty when it can).
- `/hwrtstats`, `hwrttimes 1`: RT statistics and GPU time per stage.

### Anti-aliasing and upscaling: DLAA / DLSS / FSR

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

### RT sky denoising: NRD

**Menu:** Options → Graphics → *Performance* tab → *Advanced* → *Sky light*

| Setting | Values | Default |
|---|---|---|
| `hwrtnrd` | `0` built-in filter, `1` NVIDIA NRD | `1` |

- It only applies with ray tracing, and works with Native and DLSS.
- If NRD is unavailable, the built-in filter is used.
- *Rays per pixel* (`hwrtskyrays`) only counts with NRD: the built-in filter always traces 4 rays and smooths over frames (`hwrtskytemporal`).

### HDR output

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

### 3D sound (Steam Audio)

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

### Low latency

The game reads your mouse once the GPU has caught up, instead of queueing frames ahead. Your aim reaches the screen sooner, with the same image. It helps most with ray tracing, native HDR or V-Sync; with classic lighting and no V-Sync there is little to gain.

**Menu:** Options → Display → *Screen* tab → *Speed and latency*

| Setting | Values | Default |
|---|---|---|
| `lowlatency` | `0` off, `1` on, `2` strict (lowest latency, fewer FPS) | `1` |

- *On* keeps the GPU busy, so the frame rate is kept.
- *Strict* waits for the GPU to finish each frame before starting the next: a bit less latency again, but fewer FPS.
- Mouse handling itself (sensitivity, acceleration, raw input) is unchanged; only the moment it is read changes. No frame is generated or reordered.

### Look

**Menu:** Options → Graphics → *General* tab (anti-aliasing) and *Classic* tab (shading, own shadow)

| Setting | What it does | Default |
|---|---|---|
| `lookfxaa` | FXAA in Native (Options → Graphics → *General* → *Anti-Aliasing*): smoother edges, softer textures. Off = as sharp as stock Sauerbraten | `0` |
| `lookselfshadow` | classic lighting: your own body casts a sun shadow in first person (needs Classic shadows, Graphics → Classic). With ray tracing it is `hwrtshadowself` (Graphics → *Ray tracing* → *Shadows*) | `1` |
| `waterwaves` | animated waves on classic water (Options → Graphics → *Classic* → *Detail*): reflection, refraction and shine ripple, the water stays flat for the game. Same strength / speed / detail settings as `hwrtwaterwaves` | `0` |
| `gtao` | Ambient Occlusion, classic lighting only (Options → Graphics → *Classic* → *Shading*): `0` off, `1` Low, `2` High. Slightly darker corners and wall bases; players are never darkened | `1` |

### Chat translation (local AI)

Incoming chat is shown as `name: translation (original)`: the translation in green, the original in grey. Your own messages can be translated before they are sent.

- It runs **on your PC** with llama.cpp and a Qwen3 model.
- An internet API key is optional and must be entered explicitly.
- With no model installed and no API key, chat is shown and sent at once, as typed. Translation starts by itself once you install a model or add a key (unless you muted it with **F7**).

**Menu:** Chat Translation (main menu)

| Setting | Values | Default |
|---|---|---|
| `translatechat` / `translateteamchat` | translate public / team chat | `1` / `1` |
| `translatelang` | language you read in (`en fr de es ru pt it pl nl sv`, or any listed code) | `fr` |
| `translateoutlang` | language you send in | `en` |
| `translateauto` | normal chat (T/Y): `0` as typed, `1` to your send language, `2` to the last speaker's language | `1` |
| `translatepreview` | grey preview of the translation while typing | `0` |
| `translatepreviewdelay` | typing pause before a preview request, in ms (100–2000) | `400` |
| `translatefemme` | feminine grammar for "I" when sending, and "you" when receiving | `0` |
| `showlang` | scoreboard column with the languages each player used | `1` |

Supported codes: `bg ca cs da de en es et fi fr hr hu id it nl no pl pt ro ru sk sl sr sv tr uk` (Latin and Cyrillic, which is what the game font can show). The language of each speaker is detected and remembered.

**Sending**

- `/say <msg>`: sent as typed.
- `/tsay <msg>`, `/tsayteam <msg>`: translated to your send language.
- `/tsay<code> <msg>`, `/tsayt<code> <msg>`: translated once into that language (team version with `t`). Example: `/tsayde hallo`.
- `/tsayr <msg>`, `/tsayteamr <msg>`: careful translation into your send language: the model thinks before translating. Slower (a few seconds, never more than 25 s). While it works the console shows *Translating carefully...*; the line goes away when the message is sent. If it fails or takes too long, nothing is sent (never the untranslated line), and the model's reasoning is never sent. Chat from others is translated once it is done. No preview.
- `/trep <msg>`, `/trepteam <msg>`: reply in the language of the last chat line.

**Addressing a player** (puts `@name` in front; Tab completes the name, Up/Down cycles)

- `/to <name> <msg>`: follows Auto-Translate.
- `/sayto <name> <msg>`: as typed.
- `/tsayto <name> <msg>`: translated to your send language.
- `/trepto <name> <msg>`: translated into that player's language.
- Team versions: `/toteam`, `/saytoteam`, `/tsaytoteam`, `/treptoteam`.

**Fixing translations**

- `/tretry` (**F9**): retranslates the last chat line, ignoring the cache.
- `/tretry <N>`: retranslates line N of the F10 chat window.
- `/tfix <correct text>`: teaches the correct translation of the last received line.
- `/tfixout <correct text>`: does the same for the last line you sent.
- `texamplekind`:
  - `0`: exact translation, instant, costs nothing.
  - `1`: few-shot example. Every example is sent to the AI with each request, so each one makes translation a little slower.
- Typing the original text itself as the correction means "leave untranslated".
- Corrections can be listed, edited and removed in the *Examples* section of the menu.

**Models** (Model section of the menu)

- Small (Qwen3-4B, ~2.5 GB, ~4 GB VRAM), Normal (Qwen3-8B, ~5 GB, ~6–7 GB VRAM) and Large (Qwen3-14B, ~9 GB).
- Each shows its size, download progress, whether it is in use, and any error.
- Downloads are checked with SHA-256 and resume after an interruption.
- Commands: `tmodelinstall <small|normal|large>`, `tmodeluse <id>`, `tmodeldelete <id>`.
- Your own `.gguf` files go in `%LOCALAPPDATA%\L4ZY\models\custom`. The *Open custom model folder* button opens it.
- The status line says whether the model runs on the GPU (Vulkan) or the CPU.

**Internet API (optional)**

- Any OpenAI-compatible address, key and model can be entered in the menu (`tmodelsaveapi` / `tmodelclearapi`).
- While a key is set, **chat leaves your PC**. The key is stored only in your `%LOCALAPPDATA%\L4ZY\translation\api.secrets`.

**Keys and status**

- **F7** `toggletranslate`: mute or unmute translation (chat is still shown). It stays muted after a restart.
- `translatestatus`: shows the service and language state.

### Settings search

**Menu:** Options → *Search settings…*

- Type a word, in English or French: the matching settings of every Options page appear as real checkboxes and sliders you can change right there, grouped by page, with a link to the page.
- It also finds settings by keyword (`son`, `souris`, `viseur`, `lumière`…); accents and capitals do not matter.
- It also finds colour pickers (my colour, friend colours, crosshair colour), text fields (sensitivity, DPI, HUD positions…) and options written on one line.

### Settings assistant (local AI)

Ask in plain words how to find or change a setting, and the translation model answers in plain words: where it is, what it does, what its values mean, the current value. When you ask for a change (or answer its question), it proposes it.

- **Nothing changes until you click Apply** next to a proposal; **Undo** puts the old value back.
- It can change game settings, put actions on keys, including up to 4 actions on one key (e.g. `bind LSHIFT "setweapon RI; attack"`), open a settings page, and open the HUD editor.
- It can never run scripts, quit, connect, chat, record, touch files, servers or keys: every proposal is checked by the game before it is shown and again when you click, whatever the model writes.
- It uses your installed translation model (no internet unless you set an API key).
- It knows the real menu path of every setting (for example Options → HUD → *Crosshair…*) and can open the page for you.

**Menu:** Options → *Settings assistant (AI)…*, or `/assistant`

| Setting / command | What it does | Default |
|---|---|---|
| *Think first* (checkbox in the window) | the model thinks before answering: a few seconds slower, better on open questions; unticked again when you leave the window | off |
| `assistantask "<question>"` | ask from the console | |

### HUD editor (drag and drop)

**Menu:** Options → HUD → *Move HUD parts (drag and drop)…*, or `/hudedit`.

- Every part of the HUD gets a frame: drag inside it to **move** it, drag an **edge** to change its width or height, a **corner** for both (**Shift** keeps the proportions).
- **T** or middle click on a frame: its text follows the new size, or keeps its normal size.
- **Right click** on a frame: back to its original place and size. **Esc** or **Enter** when done.
- Parts: health/armour/ammo icons, radar, flag timer, flag messages, kill feed, killing spree, match clock, score, ammo bar, spectator block, FPS and clock, console, chat.
- Parts can be made as big as you like: they always stay on screen.
- *Put everything back in place* (same page) or `hudlayoutreset` resets them all. The layout is saved in `hudlayout`.

### CTF: flag timer and flag messages

- **Flag timer:** bottom right, how long you (or the player you watch) have carried a flag stolen from the enemy base. Not shown for a flag picked up from the ground.
- **Flag messages:** big lines in the upper middle of the screen, apart from the kill feed, when a flag is stolen, picked up, dropped, returned (or put back by itself) or scored, with the run time of a flag that came from the base.

**Menu:** Options → HUD

| Setting | What it does | Default |
|---|---|---|
| `flagtimer` | flag timer | `1` |
| `flagfeed` | flag messages | `1` |
| `flagfeedfade` | seconds a flag message stays | `4` |

### Friends and clan tag

- Your own local list of nicks (never IPs), plus an optional clan tag: every nick that contains it counts as a friend.
- Friends are coloured on their body, the name above their head, the scoreboard, chat, the kill feed and the minimap.
  - One colour for your team, another for the other team.
  - Also on the first-person arm of the friend you spectate, and in demos.
- *Find friends online* scans the public server list, and lets you join a friend's server.

**Menu:** Options → *Friends and colours* (colours: *Player colours* tab)

| Setting / command | What it does | Default |
|---|---|---|
| `clantag "<text>"` | your clan tag | `""` |
| `friendallycolor` / `friendenemycolor` | colour index (−1 off, 0–9) | `8` / `6` |
| `addfriend`, `delfriend <i>`, `joinfriend <i>`, `friendrefresh` | manage the list, join a friend's server, rescan | |

### Player colours

Tints your body, name, HUD gun and chat/kill-feed name. Click the player preview on the main menu to cycle, or use the colour swatches.

| Setting | Values | Default |
|---|---|---|
| `mycolor` | `-1` off, `0` green, `1` blue, `2` yellow, `3` red, `4` gray, `5` magenta, `6` orange, `7` white, `8` cyan, `9` rose | `-1` |
| `cyclemycolor` | next colour | |

**Team colours (optional):** Options → *Friends and colours* → *Player colours*. One colour for all teammates and one for all enemies (bots included), instead of blue and red. Off by default.

| Setting | Values | Default |
|---|---|---|
| `teamcolorally` / `teamcolorenemy` | `-1` off (normal blue / red), `0`–`9` palette | `-1` |
| `friendsfollowteamcolor` | friends take their side's colour instead of their friend colour (names keep the friend colour) | `0` |

**Custom player model:** Options → Player Model → *Custom model*. Put your own model in the folder that *Open custom folder* opens (`Documents\My Games\L4ZY\packages\models\custom`, with optional `red` / `blue` subfolders; a README there explains it). Nothing is drawn over it: no player, friend or team colour. If the folder is empty, the picked model is used. It is drawn on you (and on everyone with *Force Matching Player Models*); nothing is sent to the server, so other players do not see it.

### Weapon trail colours

Pick the colour of each weapon's trail: for everyone (*All*), and if you want, differently for your own shots (*Me*) and everyone else's (*Others*). Nothing changes until you pick a colour. There is no team-based colour.

**Menu:** Options → HUD → *Weapons and trails…* → *Me…*, *Others…* or *All…*, then one line per weapon (hex code, presets, *same as All* / *stock*).

| Setting / command | What it does | Default |
|---|---|---|
| `trailcolor<weapon>` (*All*) | trail colour of that weapon for everyone, hex. Weapons: `shotgun`, `chaingun`, `pistol`, `rifle`, `rocketlauncher`, `grenadelauncher` | stock (`FFC864` or `404040`) |
| `trailmycolor<weapon>` (*Me*) | your own trails of that weapon; `-1` = same as All | `-1` |
| `trailothercolor<weapon>` (*Others*) | everyone else's trails of that weapon, any team; `-1` = same as All | `-1` |
| `trailbrightness` | brightness of changed colours, in % (25–100) | `100` |
| `settrailcolor [me\|others\|all] <weapon> <#RRGGBB\|stock\|same>` | set one colour | |
| `trailcolorreset [me\|others\|all]` | back to the stock colours | |
| `projcolor<weapon>`, `projmycolor<weapon>`, `projothercolor<weapon>` | rockets and grenades only (`rocketlauncher`, `grenadelauncher`): colour of the projectile glow and of its explosion, for All / Me / Others; `-1` = stock (All) or same as All (Me, Others) | `-1` |
| `setprojcolor [me\|others\|all] <weapon> <#RRGGBB\|stock\|same>`, `projcolorreset` | set one projectile colour / all back to stock | |
| `muzzlelight<weapon>` | light cast by that weapon's muzzle flash, for every player (`shotgun`, `chaingun`, `rifle`, `grenadelauncher`, `pistol`); the flash itself always stays | `1` |

With ray tracing, lights follow your colours: a muzzle flash takes the trail colour, a rocket or grenade and its explosion take the projectile colour (never the smoke colour). Projectile colours and the muzzle flash light boxes are on the same pages (Options → HUD).

### Kill feed, react times and streaks

- **Kill feed:** `killer [weapon] victim`, fading out, with an `xN` tag on your streaks.
- **Kills-deaths against each player:** `[3-1]` next to a kill feed line is how many times you killed that player and they killed you this match.
- **React times:** on your own kills and deaths, the time from the first frame the enemy was really visible on your screen to the killing shot, and how long they were under your crosshair (`view / crosshair ms`).
  - On your deaths the value is approximate (`~`) unless the killer also runs this client on a compatible server.
- **Others' react times** (`reacttimeall`, off by default): on a compatible server, frags between two other players also show the killer's own react times, when the killer runs this client with react times on. Spectators too.
- **Kill streak popups:** display only, no gameplay effect.
- **Enemy visibility in HD demos** (`demohdvis`, off by default, Options → Recording → *Demos (HD aim)*): your HD demos also record which enemies were really on your screen. Nothing changes in game.
- When you spectate someone or watch a demo, "mine" and "my team" (filter, streaks, kills-deaths, react times) are the player you watch. React times of another player are only shown when their aim is exact (HD demo, compatible server).

**Menu:** Options → HUD → *Adjust…* next to *Kill feed* / *Kill streak*; place and size them with the HUD editor

| Setting | Default |
|---|---|
| `killfeed`, `killfeedconsole`, `killfeedfilter` (0 all, 1 team, 2 mine) | `1`, `0`, `0` |
| `killfeedx`, `killfeedy`, `killfeedscale`, `killfeedalign`, `killfeedfade` (s), `killfeedmax` | `0.02`, `0.40`, `0.5`, `-1`, `5`, `5` |
| `reacttime`, `reacttimeall` (frags between other players) | `1`, `0` |
| `demohdvis` (record enemy visibility in HD demos) | `0` |
| `killfeedvs` (kills-deaths against each player) | `1` |
| `killstreak`, `killstreakothers`, `killstreaktk`, `killstreakstep` | `1`, `1`, `1`, `5` |
| `killstreakx`, `killstreaky`, `killstreakscale`, `killstreakalign`, `killstreakfade` (s) | `0.50`, `0.18`, `0.85`, `0`, `3` |

### F8 kills / deaths log and F10 chat window

- **F8** (`togglekilllog`): a full-screen list of **your** kills and deaths for this game and the previous one, with weapon and react times.
  - `fullkilllogsize` sets its height in % (default `75`).
- **F10** (`togglechat`): a full-screen chat and team-chat history from join to leave, up to 20,000 lines. Lines are numbered for `/tretry N`.
  - `fullchatsize` sets its height in % (default `75`); `chatskip <n>` scrolls it.
- Scrolling in both: mouse wheel, PgUp/PgDn, Home/End.
- **Copy** in F10 and in the F11 console:
  - Ctrl+C copies everything, or the selection.
  - Ctrl+Shift+C copies the visible lines.
  - Drag with the mouse to select.

### Automatic match videos (MP4)

- Records one H.264 `.mp4` per map with game audio (no microphone), starting at your first spawn.
- Clips shorter than 8 s are deleted. Only the newest `matchkeep` files are kept.
- Files are saved in `Documents\My Games\L4ZY\recordings`.

**Menu:** Options → Recording

| Setting / command | What it does | Default |
|---|---|---|
| `matchrecord` | record automatically | `1` |
| `matchkeep` | files kept (1–30) | `5` |
| `matchrecquality` | `1`–`3` 720p60 at 3/6/9 Mb/s, `4`–`5` 1080p60 at 8/12 Mb/s | `2` |
| `togglematchrecord` | on or off (off stops the current file) | |
| `matchrecextract <i>` | copy a video somewhere | |
| `openfolder recordings` | open the folder | |

### Local demos with HD aim

- Every match you play is saved as a normal `.dmo`, plus a `.dmohd` file with full-precision aim.
- On playback, the HD aim replaces the 1° steps of vanilla demos and follows the recorder: their team is blue, the other red.
- Demo controls: the exact time, a timeline (click or drag to go anywhere), back / ahead 10 s (0.5 s with Alt held), play/pause, speed (0.25× to 2×), the players you can follow (click a name), and *Hide*.
- **X-ray** (demos played on this computer only, never in a game): players are seen through walls in their colour. Switch it with the *X-RAY* button of the demo controls or a key; it keeps its state when you go back in the demo, and each new demo starts from *X-ray at demo start*.
- While you watch a player whose exact aim is used (HD demo, or live on a compatible server), a small grey **HD** sits next to their name; nothing is shown otherwise.
- Live on a compatible server, the exact aim is shown `hdlivedelay` ms late (200 by default, the same for every player) and drawn along smooth curves, so it plays fluid and complete. The rest of the game (other players, shots, sounds, deaths, kill feed, scores) is replayed with the same delay, so you see exactly what the watched player saw; chat and server messages sent on their own are not delayed. The delay applies as soon as you spectate on a compatible server, the same whoever you watch (with or without exact aim, bots) and in free cam; joining the game puts you back on time. Long HD aim files (over 31 MB) come in parts from compatible servers and are joined automatically.
- On a compatible server, while you record, the server's demo of the match (with the HD aim of every L4ZY player) is downloaded during the map vote and replaces your local one, only if it arrived complete and is the same match. Your own recording is kept for 7 days in `demo\replaced`. One try per match, never retried; if the server demo has no HD aim, your local one is kept.

**Menu:** Options → Recording

| Setting / command | What it does | Default |
|---|---|---|
| `demohdrecord` / `demohdplay` | record / use the HD aim | `1` / `1` |
| `demokeep` | demos kept (1–200) | `5` |
| `hdlivedelay` | live spectating on a compatible server: the HD aim is shown this many ms late, the same for every player, so it plays smooth and complete (0–1000) | `200` |
| `toggledemorecord`, `toggledemohd` | recording on/off, HD/vanilla aim while watching | |
| `demoxraystart` | X-ray at demo start (Options → Recording → *Demos (HD aim)*): `0` Off, `1` On | `0` |
| `demoxraystyle` | X-ray style: `0` outline, `1` outline + veil | `0` |
| `demopov` | followed player's view (Options → Recording → *Demo playback*): `1` as their game showed them, `0` the real positions | `1` |
| `demohitbox` | hitboxes in demos played on this computer: `0` Off, `1` On | `0` |
| `demohitboxshots` | with hitboxes on, the shots with the hitboxes near them: `0` Off, `1` On | `1` |
| `demohitboxdeath` | with hitboxes on, ms the hitbox of a killed player stays where they were hit (`0`–`5000`) | `1500` |
| `demolookkey` | key held to look around with the mouse while watching a demo | `MOUSE3` |
| `toggledemoxray`, `toggledemobar` | X-ray on/off, hide/show the demo controls (no key by default; Options → Keys) | |
| `demorewind [ms]`, `demoforward [ms]` | seek (default 10 s) | |
| `playlocaldemo <i>`, `localdemoextract <i>` | play a demo, copy it with its `.dmohd` | |
| `openfolder demo` | open the folder | |

### Tiny Apa bots

- **Classic:** the normal Sauerbraten bots.
- **Tiny Apa (WIP):** a second kind of bot, with its own aim, reactions and movement; work in progress.

Classic bots stay the default.

**Menu:** main menu → *Bot Match* → *bots play*: *Classic* or *Tiny Apa*; with Tiny Apa, their level and *a different form each game*.

| Setting | Values | Default |
|---|---|---|
| `botperception` | `0` Classic, `2` Tiny Apa | `0` |
| `botstylelevel` | Tiny Apa level in %, `50` to `100`: `100` a strong player, `50` half the accuracy and slower | `100` |
| `botstyleform` | `1` a different form each game, `0` always the same | `1` |
| `botstyletired` | % of games a Tiny Apa plays tired, `0` to `100` (with a different form each game) | `25` |
| `botstylewarmup` | `1` warm up during the first two minutes, `0` full level from the start | `1` |
| `botnavgrid` | `1` find their way on the map's walk grid, `0` the map's waypoints | `1` |
| `botnavsafe` | on the walk grid: `1` keep clear of deadly falls, `0` off | `1` |
| `botnet` | `1` a new way for Tiny Apa to make decisions (work in progress), `0` off | `0` |

### Scoreboard, sound and mouse

- **Scoreboard columns** (Options → HUD → *Scoreboard (Tab)…*): `showfrags` (`1`), `showkd`, `showflags`, `showaccuracy`, `showstatus` (`0`), `showlang` (`1`), `showvs` (`1`: your kills-deaths against each player), and `scoreboardalpha` for the background opacity (`40`).
  - Admin, auth and master are shown as a tag after the name; names are dimmed while a player is dead.
- **Spectating:** a list of the players on the right (`spectatorlist`, `1`, Options → HUD → *Scoreboard (Tab)…*). Hold Tab and click a name to follow that player. A small eye marks the player you watch, in the list, the scoreboard and the Master menu.
- **Master menu** (main menu → *Master*, in a game): each player's client number, and the actions as icons (hover one to see what it does).
- **Crosshair** (Options → HUD → *Crosshair…*, also from Options → Mouse): resting colour in hex (`crosshaircolour`, or `crosshairhex FF8800`; default white), with white/green/cyan/yellow/pink presets. The hit crosshair takes the colour of the player you hit (friend colour, otherwise blue or red); `crosshairreloaddim` dims it while reloading (`0`).
- **Sound mix** (Options → Sound → Mix): `mixweapons`, `mixhits`, `mixpain`, `mixitems`, `mixannounce`, `mixflags`, `mixmove`, `mixworld`.
  - All go from 0 to 200 (`100` = normal). `mixreset` puts them all back to 100.
- **Mouse** (Options → Mouse): `mousedpi` (`800`), `setcm360 <cm>` and `getcm360`, for hipfire cm per 360°.
- **Window** (Options → Display): `windowmode` `0` windowed, `1` exclusive fullscreen, `2` borderless (default).
  - `borderlessres` (`0`): in borderless, play at the resolution chosen below it and stretch it over the whole screen; Windows keeps its own resolution. `borderlessaspect` (`0`) keeps the proportions with black bars.

### Online rules

**Client announce.** When you connect to a server, L4ZY sends it one line, once per connection: `__L4ZY <version>` (for example `__L4ZY 2026.9.28.1`).

- Only a released version number (`YEAR.MONTH.DAY.N`) is sent; any other build sends `__L4ZY dev`.
- Nothing is sent in a local game or a local demo.
- Vanilla servers ignore it. If an older server mod answers "unknown command", that one answer is hidden.

**Diagnostic views are offline only.** On a server (or with other players on your own server), debug views that could show players through walls or without textures are switched back to normal before every frame, with the message `debug view disabled online`: `hwrtdebug` other than `0` and `7`, `rtaodebug`, `hwrtshade`, `hwrtveldebug`, `hwrtnrddbg`, `hwrtdiffvis`, `hwrtskyvisdbg`, `hdrlightdbg`, `hwrtdepthmask 0`, and the `hwrtvel…` test drives. Offline and in demos, nothing changes. Normal settings (lighting, anti-aliasing, HDR, NRD, shadows) are never blocked.

| Command | What it does |
|---|---|
| `l4zyversion` | the version this client announces |

### Updater commands

`tupdatecheck`, `tupdateapply`, `tupdaterestart` and `tupdateinstalled`: the same actions as the Updates menu.

## Changelog

Each release is listed on the [Releases](https://github.com/Lazy-Apathy/L4ZY/releases) page, newest first.

### 2026.10.25.1 (test)

- Bots: Tiny Apa find their way on a walk grid made for each map (jump pads, teleporters, small steps, drops) and keep clear of deadly falls. The grid is built little by little the first time a map is played, then kept on disk and updated after map edits (main menu → Bot Match → walk grid settings..).
- Bots: Tiny Apa aim at the part of a player they can see, keep one opponent in mind, shoot at where they believe you are, and react to sounds without knowing who made them.
- Bots: Tiny Apa keep moving when they shoot, and their view speeds up and slows down smoothly.
- Bots: Tiny Apa can play some games tired and warm up during the first two minutes (main menu → Bot Match → form settings..).
- Bots: new option, off by default: a new way for Tiny Apa to make decisions (work in progress) (main menu → Bot Match).
- Demos: new Demo playback tab (Options → Recording): the followed player's view as their game showed them, or the real positions; hitboxes, off by default, with the shots that touched them. New keys: hitboxes, player's view, next view, go to a typed time, and look around while held (middle mouse button by default).
- Demos: the number of demos to keep is typed in the menu (1 to 200). Old matches are removed only when a recorded match ends.
- Menus: the grey notes are gone; an explanation shows when the mouse rests on an option (after 1.5 s by default, Options → Display → Advanced → Menus).
- CTF: flag messages colour player names like the kill feed (your colour, friends' colours), team colours otherwise.
- Chat translation: the translator is given the nicknames of the players who wrote in the chat, so that it leaves them as they are.
- Network: malformed fragmented packets are now rejected (checks from the official ENet project).

### 2026.10.16.1 (stable)

- Bots: new optional "Tiny Apa" (WIP) bots in Bot Match, with a level and a different form each game (main menu → *Bot Match* → *bots play*). Classic stays the default.
- Anti-aliasing: Temporal AA leaves the menu; if it was on, it is turned off once. New, off by default: sharpening for DLAA/DLSS and *Sharper textures* with DLSS/FSR (Options → Graphics → *Anti-Aliasing*).
- Spectating: a list of the players on the right; hold Tab and click a name to follow that player. A small eye marks the player you watch.
- Master menu: each player's client number, and the actions as icons.
- Your name and body no longer get a pink colour by default on first launch.
- Demos: X-ray view (players seen through walls in their colour) while watching a demo played on this computer, never in a game.
- Demos: new controls with the exact time, a timeline, 0.5 s steps with Alt, speed, and the players to follow.
- Chat translation: with no AI installed, chat is shown and sent at once; translation starts by itself once a model is installed. F7 mute is kept after a restart.
- Native HDR waits until the game window is ready and retries by itself, also when Windows switches HDR off for a moment at start. The *Automatic* choice is gone; if you had it, you are moved to SDR once.

### 2026.10.12.1 (stable)

- New graphics profiles *Ultra*, *High*, *Medium* and *Low* (Options → Graphics → *Performance*); each of their options can also be set on its own.
- Ray tracing: soft shadows, on by default, with their own denoiser (NRD SIGMA).
- Ray tracing: teleporters now light and shadow other players, bots and items; new setting *Shadows of the others*.
- Optional animated waves on water, off by default.
- All settings pages reorganised: cleaner and easier to find your way around.
- Keys page: every game action can now be bound there.
- More accurate kill feed react times; new option to record enemy visibility in HD demos (off by default).
- CTF: big flag messages also when a flag is dropped, returned or reset.
- Chat translation: new `/tsayr` and `/tsayteamr` commands (translation with the model's reasoning).

### 2026.10.8.1 (stable)

- Sharper image in Native: the FXAA filter that softened textures and player models is now off by default. If you liked the smoother edges, tick *FXAA in Native* in Options → Graphics → *Anti-Aliasing*.
- Exclusive fullscreen: choosing a resolution now applies it right away; the *Custom* fields apply at once too.
- Borderless: the Resolution section now says when the game uses the monitor size, and how to play at another resolution (*Borderless at the Resolution below*).

### 2026.10.7.1 (stable)

- Ray tracing: weapon lights redone. Short muzzle flashes sized to each weapon (your gun lights up with them), rockets and grenades light their path in flight, and an explosion's light lasts as long as the explosion itself.
- New projectile colours for rockets and grenades (glow in flight and explosion), separately for *Me*, *Others* and *All*; with ray tracing, the light follows the projectile colour, and muzzle flashes follow the trail colour (Options → HUD → *Weapon Trails: Colours*).
- New boxes to turn off the light of each weapon's muzzle flash; the flash itself stays (Options → HUD).
- Ray tracing: optional soft shadows from weapon smoke, Off by default (Options → Graphics → *Lighting* → *Smoke shadows*).
- New: borderless at the resolution you choose, stretched over the whole screen or with black bars, without changing the Windows resolution (Options → Display).
- Fixed: mouse stuck in the middle of the screen in menus after loading a map, in exclusive fullscreen.
- Fixed: a chat, kill feed, clock or other HUD part made much bigger in the HUD editor could leave the screen; chat messages then only showed in F10. Layouts saved that way are repaired.
- Fixed: greyscale textures shown red with ray tracing.
- Fixed: black minimap when loading maps with waterfalls.
- Fixed: a crash while spectating and switching between players.
- On compatible servers:
  - spectating: you now watch the game 200 ms in the past, so the player's exact aim plays smooth and complete, with shots and the kill feed in sync;
  - HD demos record the exact aim twice as often (200 times per second);
  - if you record your matches, the server's demo (with the exact aim of every L4ZY player, not only yours) is downloaded during the map vote and replaces yours, only when it arrived complete; yours is kept for 7 days;
  - optional: see the react times of frags between other players (Options → HUD → *Kill Feed*, off by default).
- Settings: an unreadable config is never overwritten, a backup `config.cfg.bak` is kept, and *Import settings* is safer and easier to find (top of Options).

### 2026.10.5.1 (stable)

- Ray tracing: lighting is now smooth across faceted floors, slopes and rocks, like classic lighting (Options → Graphics → *RT Normals*, On by default).
- Ray tracing in edit mode: new or removed geometry and texture changes now show up in the ray-traced lighting, shortly after you stop editing (no cost outside edit mode).
- Ray tracing: less shimmer on distant textures in motion with DLAA/DLSS. If you prefer the sharper look, choose *Sharper* in Options → Graphics → *RT Textures*.
- Ray tracing: no more faint dark ring on the floor around you (the old *Creases* effect is no longer applied on top of ray tracing). The *Creases* checkbox is gone from the menu: Ambient Occlusion (Options → Graphics → *Lighting*) does that job in classic lighting.
- Ray tracing: fixed a crash when the player model changes, or when switching between players while spectating, during a ray-traced game.
- Weapon trail colours: pick a colour for each weapon's trail, separately for your own shots (*Me*), everyone else's (*Others*) and both (*All*), with hex codes or presets (Options → HUD → *Weapon Trails: Colours*). Stock colours by default; there is no team-based colour.
- Team colours (optional, off by default): one colour for all teammates and one for all enemies, and friends can follow them (Options → HUD → *Team colours*).
- Custom player model: a *Custom model* choice and a button that opens the folder to drop your own model in; no colour is drawn over it.
- Local games no longer show the client check or "Welcome".
- The console only shows game information; technical lines go to `log.txt`. When ray tracing or an anti-aliasing mode cannot start, one short line says why.

### 2026.10.4.1 (stable)

- AMD FSR 3.1 upscaling, next to DLAA/DLSS: Options → Graphics → *Anti-Aliasing* (FSR Native, Quality, Balanced, Performance, with a sharpening slider). No frame generation.
- Ray tracing: sharp reflections on water and on surfaces the map made reflective, and the shine of lamps and the sun on glossy surfaces. Both On; you can turn them off in Options → Graphics (*RT Reflections*, *RT Specular*).
- Ray tracing: textures no longer shimmer in the distance, and close-up textures are smoothed instead of blocky (Options → Graphics → *RT Textures*: Bicubic by default, Lanczos, or Nearest for the old look).
- Ray tracing: water no longer turns dull and blurry all of a sudden.
- Ray tracing: one sky ray per pixel with NRD (was four): faster, with no visible difference in our tests.
- Ambient Occlusion for classic lighting: slightly darker corners and wall bases, players never darkened. Low by default (Options → Graphics → *Lighting*).
- DLAA/DLSS set up as NVIDIA's integration guide asks.
- Display output is now SDR by default. If yours was on Automatic, it is moved to SDR once; choose *Native HDR* in Options → Graphics → *Display output* if you want it.
- Start-up: Vulkan now starts only when ray tracing, DLAA/DLSS or FSR is used. It is checked first in a separate process; if that check crashes or hangs, the game retries without the faulty Vulkan layer or driver (for example an old AMD integrated-graphics driver on some NVIDIA laptops), remembers what worked, and otherwise falls back to classic lighting + Native with the reason instead of closing. This should help with the start-up crashes and freezes reported on some PCs.
- Start-up: laptops now ask Windows for the high-performance graphics card.

### 2026.10.3.1 (stable)

- 3D sound (Steam Audio): Options → Sound → *3D Sound*. Headphones (HRTF) let you hear whether a sound is ahead or behind, above or below; walls muffle sounds (*Occlusion*). Off by default.
- Low latency mode: the mouse is read once the GPU catches up, for a more direct aim with ray tracing, native HDR or V-Sync. On by default; *Strict* goes further at the cost of some FPS (Options → Display).
- Ray tracing: faster on maps with many teleporters; no more wasted work when the model cache is full.
- Ray tracing: cleaner sky light with NRD (blue-noise sky rays).
- Ray tracing is greyed out in Options → Graphics → *Lighting* when your graphics card or driver cannot run it, with the reason.
- Online: L4ZY tells the server its version once when you connect (`__L4ZY <version>`), so admins can see which client you use. `/l4zyversion` shows it.
- Online: debug and diagnostic views (such as `hwrtdebug 4`) only work offline and in demos.
- Crosshair colour in hex: Options → Mouse → *crosshair:*, or `crosshairhex FF8800`.
- Settings search also finds colour pickers, text fields and one-line options.
- Settings assistant: gives the real menu path of every setting.
- Menu text fields (name, add friend…) now have an orange outline, visible without hovering them.
- Ray tracing: no more freeze when an item (boost, armour, quad) respawns.
- Ray tracing: items and models no longer go missing from the ray-traced scene on big maps.
- Ray tracing: faster lighting on maps with many lamps and a quicker switch to ray tracing.
- CTF: no radar marker for a flag that the server keeps outside the map.
- Fixes: friend names up to 15 characters in *Add friend*, Alt+Tab out of exclusive fullscreen gives the desktop its normal resolution back.

### 2026.9.28.1 (stable)

- Settings search: Options → *Search settings…*, by name or keyword, in English or French.
- Settings assistant: ask the local AI about any setting; it explains, and changes only what you Apply. Optional *Think first*.
- HUD editor: move and resize every part of the HUD with the mouse, width and height apart, text at its size or not.
- CTF: flag timer, and big flag messages (stolen / picked up / scored, with the run time).
- Kills-deaths against each player in the kill feed and the scoreboard.
- Spectating and demos: kill feed filter, streaks, kills-deaths and react times follow the player you watch.
- Hit crosshair in the colour of the player hit; no more dimming while reloading (option).
- Admin / auth / master tags; names dimmed when dead; friend colours on the spectated player's arm and in demos.
- Fixes: loading screen that could stay frozen, languages column mixing players, starting a solo game dead, left click in demos, scoreboard drawn over menus, console frag messages with the same name twice, ray-tracing messages removed from the console (`rtconsole 1` shows them).

### 2026.9.26.4

First public release:

- Standalone Windows installer: no admin rights, and no Python or other tools to install.
- In-game updates.
- Ray-traced lighting, DLAA/DLSS, NRD, native HDR with calibration.
- Local AI chat translation.
- Settings imported from your previous Sauerbraten client.

## Licences

- **Engine:** Cube 2: Sauerbraten (zlib licence), modified.
- **Game media:** original Sauerbraten media, each under its author's licence (some are non-commercial). L4ZY is free.
- **NVIDIA DLSS/NGX and NRD:** NVIDIA RTX SDK licence. Do not reverse engineer or redistribute them separately.
- **Also included:** Python (PSF licence), llama.cpp (MIT) and the Microsoft Visual C++ runtime.
- **Translation models:** Qwen3, Apache 2.0, downloaded from the official Qwen repositories.
- **Steam Audio** (`phonon.dll`, for the optional 3D sound): Apache 2.0, (c) Valve Corporation; it contains third-party parts listed in `docs\licenses\steam-audio\THIRDPARTY.md`. Valve does not endorse L4ZY.
- **AMD FidelityFX FSR 3.1** (`sauer_fsr.dll`): MIT, (c) Advanced Micro Devices. AMD does not endorse L4ZY.
- **Ambient Occlusion** is adapted from Intel's XeGTAO (MIT).
- Full texts are in `docs\licenses` in the installed game.
- Source code: https://github.com/Lazy-Apathy/L4ZY-source (one tag per release)
- L4ZY is not an official Sauerbraten release, and is not endorsed by NVIDIA or Valve.
