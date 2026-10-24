# Automatic match videos (MP4)

Patch `13-match-videos.patch`, from the L4ZY 2026.10.16.1 source.

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

## Applying

Apply first: every patch before it in the series, in order (`00-build-fix` to `12-scoreboard-and-spectating`). It was made and checked on top of them and does not apply on its own.

```
git apply patches/13-match-videos.patch
```

- Checked: applied after the previous patches of the series and compiled.


The menu paths above are those of the full L4ZY menus (patch `16-menus`). Without that patch, the settings and commands exist all the same: use them from the console or your own `.cfg` files.


## Files

- `src/Makefile`
- `src/engine/main.cpp`
- `src/engine/matchrec.cpp`
- `src/engine/movie.cpp`
- `src/fpsgame/client.cpp`
- `src/fpsgame/fps.cpp`
- `src/fpsgame/game.h`
- `src/shared/iengine.h`
