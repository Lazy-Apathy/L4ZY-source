# Tiny Apa bots

Patch `11-tiny-apa-bots.patch`, from the L4ZY 2026.10.16.1 source.

- **Classic:** the normal Sauerbraten bots.
- **Tiny Apa (WIP):** bots modelled on a human player's play.

Tiny Apa (WIP) is the first player profile: bots on a human-like base, tuned on one player's play. Classic bots stay the default.

**Menu:** main menu → *Bot Match* → *bots play*: *Classic* or *Tiny Apa*; with Tiny Apa, their level and *a different form each game*.

| Setting | Values | Default |
|---|---|---|
| `botperception` | `0` Classic, `2` Tiny Apa | `0` |
| `botstylelevel` | Tiny Apa level in %, `50` to `100`: `100` a strong player, `50` half the accuracy and slower | `100` |
| `botstyleform` | `1` a different form each game, `0` always the same | `1` |

## Applying

Apply first: every patch before it in the series, in order (`00-build-fix` to `10-weapon-trails`). It was made and checked on top of them and does not apply on its own.

```
git apply patches/11-tiny-apa-bots.patch
```

- Checked: applied after the previous patches of the series and compiled.


The menu paths above are those of the full L4ZY menus (patch `16-menus`). Without that patch, the settings and commands exist all the same: use them from the console or your own `.cfg` files.


## Files

- `src/fpsgame/ai.cpp`
- `src/fpsgame/ai.h`
- `src/fpsgame/aihuman.h`
- `src/fpsgame/aihumanproc.h`
- `src/fpsgame/aiman.h`
- `src/fpsgame/aistyle.h`
- `src/fpsgame/client.cpp`
- `src/fpsgame/entities.cpp`
- `src/fpsgame/fps.cpp`
- `src/fpsgame/game.h`
- `src/fpsgame/server.cpp`
