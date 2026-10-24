# Game HUD, chat and demos: kill feed, react times, friends and colours, crosshair, CTF flag messages, F8/F10 windows, HUD editor, chat translation, HD-aim demos, client announce

Patch `07-game-hud-and-demos.patch`, from the L4ZY 2026.10.16.1 source.

The kill feed, react times, kill streaks, friends and player colours, the crosshair colours, the CTF flag timer and flag messages, the F8 kills/deaths log, the F10 chat window, the HUD editor, the chat translation client, the HD-aim demos and the client announce come as one patch. They call each other everywhere: react times use the exact aim of HD demos and of compatible servers (client announce), the feeds and the crosshair use the friend and player colours, the HUD editor places each of these parts and they draw themselves through it, the chat window and the translation share the chat log, and the demo controls show the same feeds. Splitting them would mean rewriting that code, not just cutting the patch.

The translation itself runs in a separate local service (`distrib/service/`, Python, llama.cpp); this patch contains the game side and that service.

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

- **F8** (`togglekilllog`): a full-screen list of **your** kills and deaths for this game and the previous one, with weapon and react times.
  - `fullkilllogsize` sets its height in % (default `75`).

- **F10** (`togglechat`): a full-screen chat and team-chat history from join to leave, up to 20,000 lines. Lines are numbered for `/tretry N`.
  - `fullchatsize` sets its height in % (default `75`); `chatskip <n>` scrolls it.

- Scrolling in both: mouse wheel, PgUp/PgDn, Home/End.

- **Copy** in F10 and in the F11 console:
  - Ctrl+C copies everything, or the selection.
  - Ctrl+Shift+C copies the visible lines.
  - Drag with the mouse to select.

- **Flag timer:** bottom right, how long you (or the player you watch) have carried a flag stolen from the enemy base. Not shown for a flag picked up from the ground.
- **Flag messages:** big lines in the upper middle of the screen, apart from the kill feed, when a flag is stolen, picked up, dropped, returned (or put back by itself) or scored, with the run time of a flag that came from the base.

**Menu:** Options → HUD

| Setting | What it does | Default |
|---|---|---|
| `flagtimer` | flag timer | `1` |
| `flagfeed` | flag messages | `1` |
| `flagfeedfade` | seconds a flag message stays | `4` |

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
| `demokeep` | demos kept (1–30) | `5` |
| `hdlivedelay` | live spectating on a compatible server: the HD aim is shown this many ms late, the same for every player, so it plays smooth and complete (0–1000) | `200` |
| `toggledemorecord`, `toggledemohd` | recording on/off, HD/vanilla aim while watching | |
| `demoxraystart` | X-ray at demo start (Options → Recording → *Demos (HD aim)*): `0` Off, `1` On | `0` |
| `demoxraystyle` | X-ray style: `0` outline, `1` outline + veil | `0` |
| `toggledemoxray`, `toggledemobar` | X-ray on/off, hide/show the demo controls (no key by default; Options → Keys) | |
| `demorewind [ms]`, `demoforward [ms]` | seek (default 10 s) | |
| `playlocaldemo <i>`, `localdemoextract <i>` | play a demo, copy it with its `.dmohd` | |
| `openfolder demo` | open the folder | |

**Menu:** Options → HUD → *Move HUD parts (drag and drop)…*, or `/hudedit`.

- Every part of the HUD gets a frame: drag inside it to **move** it, drag an **edge** to change its width or height, a **corner** for both (**Shift** keeps the proportions).
- **T** or middle click on a frame: its text follows the new size, or keeps its normal size.
- **Right click** on a frame: back to its original place and size. **Esc** or **Enter** when done.
- Parts: health/armour/ammo icons, radar, flag timer, flag messages, kill feed, killing spree, match clock, score, ammo bar, spectator block, FPS and clock, console, chat.
- Parts can be made as big as you like: they always stay on screen.
- *Put everything back in place* (same page) or `hudlayoutreset` resets them all. The layout is saved in `hudlayout`.

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

- **Crosshair** (Options → HUD → *Crosshair…*, also from Options → Mouse): resting colour in hex (`crosshaircolour`, or `crosshairhex FF8800`; default white), with white/green/cyan/yellow/pink presets. The hit crosshair takes the colour of the player you hit (friend colour, otherwise blue or red); `crosshairreloaddim` dims it while reloading (`0`).

**Client announce.** When you connect to a server, L4ZY sends it one line, once per connection: `__L4ZY <version>` (for example `__L4ZY 2026.9.28.1`).

- Only a released version number (`YEAR.MONTH.DAY.N`) is sent; any other build sends `__L4ZY dev`.
- Nothing is sent in a local game or a local demo.
- Vanilla servers ignore it. If an older server mod answers "unknown command", that one answer is hidden.

| Command | What it does |
|---|---|
| `l4zyversion` | the version this client announces |

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
- `/tsayr <msg>`, `/tsayteamr <msg>`: careful translation into your send language: the model thinks before translating. Slower (never more than 25 s). While it works the console shows *Translating carefully...*; the line goes away when the message is sent. If it fails or takes too long, nothing is sent (never the untranslated line), and the model's reasoning is never sent. Chat from others is translated once it is done. No preview.
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

## Applying

Apply first: every patch before it in the series, in order (`00-build-fix` to `06-3d-sound`). It was made and checked on top of them and does not apply on its own.

```
git apply patches/07-game-hud-and-demos.patch
```

- Checked: applied after the previous patches of the series and compiled.


The menu paths above are those of the full L4ZY menus (patch `16-menus`). Without that patch, the settings and commands exist all the same: use them from the console or your own `.cfg` files.


## Files

- `data/demobar_back.png`
- `data/demobar_forward.png`
- `data/demobar_hide.png`
- `data/demobar_pause.png`
- `data/demobar_play.png`
- `data/demobar_show.png`
- `data/demobar_xray.png`
- `data/glsl.cfg`
- `distrib/service/exemples-modele-de.txt`
- `distrib/service/exemples-modele-en.txt`
- `distrib/service/exemples-modele-es.txt`
- `distrib/service/exemples-modele-it.txt`
- `distrib/service/exemples-modele-nl.txt`
- `distrib/service/exemples-modele-pl.txt`
- `distrib/service/exemples-modele-pt.txt`
- `distrib/service/exemples-modele-ru.txt`
- `distrib/service/llama_runtime.py`
- `distrib/service/model_manager.py`
- `distrib/service/sauerrt_paths.py`
- `distrib/service/translate_server.py`
- `src/Makefile`
- `src/engine/3dgui.cpp`
- `src/engine/animmodel.h`
- `src/engine/client.cpp`
- `src/engine/console.cpp`
- `src/engine/engine.h`
- `src/engine/hudlayout.cpp`
- `src/engine/main.cpp`
- `src/engine/menus.cpp`
- `src/engine/rendergl.cpp`
- `src/engine/rendermodel.cpp`
- `src/engine/rendertext.cpp`
- `src/engine/serverbrowser.cpp`
- `src/fpsgame/aiman.h`
- `src/fpsgame/capture.h`
- `src/fpsgame/client.cpp`
- `src/fpsgame/collect.h`
- `src/fpsgame/ctf.h`
- `src/fpsgame/demohd.cpp`
- `src/fpsgame/fps.cpp`
- `src/fpsgame/friends.cpp`
- `src/fpsgame/game.h`
- `src/fpsgame/monster.cpp`
- `src/fpsgame/movable.cpp`
- `src/fpsgame/render.cpp`
- `src/fpsgame/scoreboard.cpp`
- `src/fpsgame/server.cpp`
- `src/fpsgame/translate.cpp`
- `src/fpsgame/weapon.cpp`
- `src/shared/cube.h`
- `src/shared/iengine.h`
- `src/shared/igame.h`
- `tools/recolor_friend_skins.py`
- `traduction.cfg`
