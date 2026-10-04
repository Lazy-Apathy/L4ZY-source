# L4ZY

A Cube 2: Sauerbraten (2020) client with **hardware ray-traced lighting**, **NVIDIA DLAA/DLSS** and **AMD FSR**, **native HDR output**, **local AI chat translation** and a **settings assistant**, **3D sound**, a **drag-and-drop HUD editor**, and a lot of quality-of-life additions: friends list, kill feed, CTF flag timer, automatic match recording, HD-aim demos and more.

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

- Only the parts that changed are downloaded: a game fix is a few MB, and maps and translation models are never re-downloaded.
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

| Graphics card | Classic lighting | Ray-traced lighting | DLAA / DLSS | Native HDR |
|---|---|---|---|---|
| **NVIDIA GeForce RTX 20, 30, 40, 50 series** (desktop and laptop) | yes | **yes** | **yes** | **yes** (with Windows HDR on) |
| NVIDIA GeForce GTX (10, 16 series and older) | yes | no (no RT cores) | no | no |
| AMD Radeon RX 6000 / 7000 / 9000 series | yes | not tested | no (DLSS is NVIDIA only) | no |
| Intel Arc | yes | not tested | no | no |
| Other GPUs, integrated graphics | yes, if Vulkan is available | no | no | no |

- Only an **NVIDIA RTX 4070 Ti SUPER** has been tested. The other RTX cards meet the same requirements, but have not been tried.
- **Ray tracing** needs hardware RT (Vulkan `ray_query`), plus OpenGL/Vulkan memory sharing (`GL_EXT_memory_object`, `GL_EXT_semaphore`).
  - NVIDIA drivers provide both.
  - AMD and Intel cards have hardware RT, but this sharing has not been checked on their drivers.
  - If something is missing, the game stays on *Classic* lighting and says so in the menu.
- **DLAA/DLSS** need NVIDIA Tensor cores (RTX).
- **AMD FSR** uses the same OpenGL/Vulkan memory sharing as ray tracing. It has only been tested on the RTX 4070 Ti SUPER.
- **Native HDR** uses the NVIDIA OpenGL/Direct3D bridge.
- **Chat translation** runs on any Vulkan GPU, or on the CPU (slower).
  - Small model: about 4 GB of VRAM.
  - Normal model: about 6–7 GB of VRAM.

## Menu

L4ZY ships its own `menus.cfg`: a new main menu, and an Options hub with pages for Graphics, Display, HUD, Scoreboard, Sound, Mouse, Keys, Console, Recording, Friends and Updates.

- **Chat Translation** is on the main menu and in Options → Game.
- `/traduction` also opens it.
- **Search settings…** and **Settings assistant (AI)…** are at the top of Options.

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

**Menu:** Options → Graphics → *Lighting*

| Setting | Values | Default |
|---|---|---|
| `hwrt` | `0` Classic (no RT), `1` ray tracing | `0` |
| `hwrtshadowself` | own-body RT shadow: `0` off, `1` sun, `2` + small/glow lights, `3` + nearest lamps, `4` + sky | `1` |
| `hwrtteleportlight` / `hwrtjumppadlight` | light cast by teleporters / jump pads | `1` / `0` |
| `hwrtsmokeshadow` | rifle/rocket smoke casts RT shadows | `0` |
| `hwrtsundisk` | hide the skybox and draw a sun disk | `0` |
| `hwrtreflections` | sharp RT reflections on water and on surfaces the map made reflective (*RT Reflections*) | `1` |
| `hwrtspecular` | shine of lamps and the sun on glossy surfaces (*RT Specular*) | `1` |
| `hwrtdiffupscale` | RT texture upscaling (Options → Graphics → *RT Textures*): `2` Bicubic, `3` Lanczos, `0` Nearest (old blocky look); `1` bilinear (console only) | `2` |

Useful commands:

- `/hwrtstats`: RT statistics.
- `hwrttoggle`: switch RT on or off (only if RT is available).
- `hwrttimes 1`: GPU time per stage.

Menu status lines tell you whether RT is really running.

If your graphics card or driver cannot run ray tracing, *Ray tracing* is greyed out with the reason.

### Ray tracing performance

- Teleporter lights are gathered once per light update instead of every frame: about twice the FPS on maps with many teleporters.
- When the model cache is full, models that cannot be added are skipped before any GPU work.
- Sky rays use blue noise with NRD: cleaner sky light at the same cost.
- Item models (boost, armour, quad…) are prepared behind the loading screen, and new skins are added without waiting for the GPU: no more freeze when an item respawns.
- Room for 128 different models in the ray-traced scene (was 64): items no longer go missing on big maps.
- Lamps are sorted per screen tile, so each pixel only tests the lamps near it: in our tests, about +40 to +80 % FPS on the heaviest maps at 1600×900, less on lighter ones.
- If ray tracing cannot run (no hardware RT, no Vulkan driver, driver too old, failed to start), *Ray tracing* is greyed out in Options → Graphics → *Lighting*, with the reason. The settings search and the assistant show it greyed out too.

Commands:

- `/hwrtraison`: why ray tracing cannot run (empty when it can).
- `/hwrtstats`, `hwrttimes 1`: RT statistics and GPU time per stage.

### Anti-aliasing and upscaling: DLAA / DLSS / FSR

**Menu:** Options → Graphics → *Anti-Aliasing*

| Setting | Values | Default |
|---|---|---|
| `hwrtngxmode` | `0` Native, `1` DLAA, `2` DLSS Quality, `3` DLSS Balanced, `4` DLSS Performance, `5` FSR Native, `6` FSR Quality, `7` FSR Balanced, `8` FSR Performance | `0` |
| `hwrtfsrsharpness` | FSR sharpening after upscaling (`0` off, `10` close to DLSS) | `10` |

- FXAA and Temporal AA are not stacked with DLAA/DLSS/FSR. They come back in Native.
- If DLAA/DLSS or FSR cannot start, the game falls back to Native and says so.
- No frame generation.

### RT sky denoising: NRD

**Menu:** Options → Graphics → *RT Sky Denoise*

| Setting | Values | Default |
|---|---|---|
| `hwrtnrd` | `0` Sauer filter, `1` NVIDIA NRD | `1` |

- It only applies with ray tracing, and works with Native and DLSS.
- If NRD is unavailable, the Sauer filter is used.

### HDR output

**Menu:** Options → Graphics → *Display output* and *HDR calibration*

| Setting | Values | Default |
|---|---|---|
| `hdroutpref` | `-1` Automatic, `0` SDR, `1` Native HDR | `0` |
| `hwrthdrexp` | exposure, in EV (−8…8; menu steps ±0.1 / ±0.5) | `0` |
| `hdroutref` | reference white, in nits (40–480) | `80` |
| `hdroutmax` | peak, in nits; `0` = automatic, from the display | `0` |
| `hdrouthud` | HUD brightness, in nits; `0` = follows reference white | `0` |

- Native HDR needs Windows HDR to be on. The game never changes Windows or driver settings.
- If you turn Windows HDR on after starting the game, choose *Native HDR* again.
- In native HDR, SSAO (Creases), Temporal AA and motion blur are not applied.

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

**Menu:** Options → Display → *Low Latency*

| Setting | Values | Default |
|---|---|---|
| `lowlatency` | `0` off, `1` on, `2` strict (lowest latency, fewer FPS) | `1` |

- *On* keeps the GPU busy, so the frame rate is kept.
- *Strict* waits for the GPU to finish each frame before starting the next: a bit less latency again, but fewer FPS.
- Mouse handling itself (sensitivity, acceleration, raw input) is unchanged; only the moment it is read changes. No frame is generated or reordered.

### Look

**Menu:** Options → Graphics → *Look*

| Setting | What it does | Default |
|---|---|---|
| `lookao` | "Creases" (SSAO) | `1` |
| `looktaa` | Temporal AA (otherwise FXAA in Native) | `0` |
| `lookselfshadow` | your own body casts a sun shadow in first person (classic shadows) | `1` |
| `gtao` | Ambient Occlusion, classic lighting only (Options → Graphics → *Lighting*): `0` off, `1` Low, `2` High. Slightly darker corners and wall bases; players are never darkened | `1` |

### Chat translation (local AI)

Incoming chat is shown as `name: translation (original)`: the translation in green, the original in grey. Your own messages can be translated before they are sent.

- It runs **on your PC** with llama.cpp and a Qwen3 model.
- An internet API key is optional and must be entered explicitly.

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

- **F7** `toggletranslate`: mute or unmute translation (chat is still shown).
- `translatestatus`: shows the service and language state.

### Settings search

**Menu:** Options → *Search settings…*

- Type a word, in English or French: the matching settings of every Options page appear as real checkboxes and sliders you can change right there, grouped by page, with a link to the page.
- It also finds settings by keyword (`son`, `souris`, `viseur`, `lumière`…); accents and capitals do not matter.
- It also finds colour pickers (my colour, friend colours, crosshair colour), text fields (sensitivity, DPI, HUD positions…) and options written on one line.

### Settings assistant (local AI)

Ask in plain words how to find or change a setting, and the translation model answers like a person: where it is, what it does, what its values mean, the current value. When you ask for a change (or answer its question), it proposes it.

- **Nothing changes until you click Apply** next to a proposal; **Undo** puts the old value back.
- It can change game settings, put actions on keys, including up to 4 actions on one key (e.g. `bind LSHIFT "setweapon RI; attack"`), open a settings page, and open the HUD editor.
- It can never run scripts, quit, connect, chat, record, touch files, servers or keys: every proposal is checked by the game before it is shown and again when you click, whatever the model writes.
- It uses your installed translation model (no internet unless you set an API key).
- It knows the real menu path of every setting (for example Options → Mouse → *crosshair:*) and can open the page for you.

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
- *Put everything back in place* (same page) or `hudlayoutreset` resets them all. The layout is saved in `hudlayout`.

### CTF: flag timer and flag messages

- **Flag timer:** bottom right, how long you (or the player you watch) have carried a flag stolen from the enemy base. Not shown for a flag picked up from the ground.
- **Flag messages:** big lines in the upper middle of the screen, apart from the kill feed, when a flag is stolen, picked up or scored, with the run time of a flag that came from the base.

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

**Menu:** Options → Friends; colours in Options → HUD → Friends

| Setting / command | What it does | Default |
|---|---|---|
| `clantag "<text>"` | your clan tag | `""` |
| `friendallycolor` / `friendenemycolor` | colour index (−1 off, 0–9) | `8` / `6` |
| `addfriend`, `delfriend <i>`, `joinfriend <i>`, `friendrefresh` | manage the list, join a friend's server, rescan | |

### Player colours

Tints your body, name, HUD gun and chat/kill-feed name. Click the player preview on the main menu to cycle, or use the colour swatches.

| Setting | Values | Default |
|---|---|---|
| `mycolor` | `-1` off, `0` green, `1` blue, `2` yellow, `3` red, `4` gray, `5` magenta, `6` orange, `7` white, `8` cyan, `9` rose | `9` |
| `cyclemycolor` | next colour | |

### Kill feed, react times and streaks

- **Kill feed:** `killer [weapon] victim`, fading out, with an `xN` tag on your streaks.
- **Kills-deaths against each player:** `[3-1]` next to a kill feed line is how many times you killed that player and they killed you this match.
- **React times:** on your own kills and deaths, how long the target had been in view and under your crosshair (`view / crosshair ms`).
  - On your deaths the value is approximate (`~`) unless the killer also runs this client on a compatible server.
- **Kill streak popups:** display only, no gameplay effect.
- When you spectate someone or watch a demo, "mine" and "my team" (filter, streaks, kills-deaths, react times) are the player you watch. React times of another player are only shown when their aim is exact (HD demo, compatible server).

**Menu:** Options → HUD → *Kill Feed* / *Kill Streak*; place and size them with the HUD editor

| Setting | Default |
|---|---|
| `killfeed`, `killfeedconsole`, `killfeedfilter` (0 all, 1 team, 2 mine) | `1`, `0`, `0` |
| `killfeedx`, `killfeedy`, `killfeedscale`, `killfeedalign`, `killfeedfade` (s), `killfeedmax` | `0.02`, `0.40`, `0.5`, `-1`, `5`, `5` |
| `reacttime` | `1` |
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
- A playback bar has −10 s, play/pause, +10 s, a seek slider, 50%/100% speed, and hide.

**Menu:** Options → Recording

| Setting / command | What it does | Default |
|---|---|---|
| `demohdrecord` / `demohdplay` | record / use the HD aim | `1` / `1` |
| `demokeep` | demos kept (1–30) | `5` |
| `toggledemorecord`, `toggledemohd` | recording on/off, HD/vanilla aim while watching | |
| `demorewind [ms]`, `demoforward [ms]` | seek (default 10 s) | |
| `playlocaldemo <i>`, `localdemoextract <i>` | play a demo, copy it with its `.dmohd` | |
| `openfolder demo` | open the folder | |

### Scoreboard, sound and mouse

- **Scoreboard columns** (Options → HUD → Scoreboard): `showfrags` (`1`), `showkd`, `showflags`, `showaccuracy`, `showstatus` (`0`), `showlang` (`1`), `showvs` (`1`: your kills-deaths against each player), and `scoreboardalpha` for the background opacity (`40`).
  - Admin, auth and master are shown as a tag after the name; names are dimmed while a player is dead.
- **Crosshair** (Options → Mouse → *crosshair:*): resting colour in hex (`crosshaircolour`, or `crosshairhex FF8800`; default white), with white/green/cyan/yellow/pink presets. The hit crosshair takes the colour of the player you hit (friend colour, otherwise blue or red); `crosshairreloaddim` dims it while reloading (`0`).
- **Sound mix** (Options → Sound → Mix): `mixweapons`, `mixhits`, `mixpain`, `mixitems`, `mixannounce`, `mixflags`, `mixmove`, `mixworld`.
  - All go from 0 to 200 (`100` = normal). `mixreset` puts them all back to 100.
- **Mouse** (Options → Mouse): `mousedpi` (`800`), `setcm360 <cm>` and `getcm360`, for hipfire cm per 360°.
- **Window** (Options → Display): `windowmode` `0` windowed, `1` exclusive fullscreen, `2` borderless (default).

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

### 2026.10.4.1 (test)

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
- Ray tracing: about twice the FPS on maps with many teleporters; no more wasted work when the model cache is full.
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
- Ray tracing: faster lighting on maps with many lamps (in our tests, about +40 to +80 % FPS on the heaviest maps, less on lighter ones) and a quicker switch to ray tracing.
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
