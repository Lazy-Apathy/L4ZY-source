# Mouse (DPI, cm/360) and window modes

Patch `08-mouse-and-window.patch`, from the L4ZY 2026.10.16.1 source.

- **Mouse** (Options → Mouse): `mousedpi` (`800`), `setcm360 <cm>` and `getcm360`, for hipfire cm per 360°.

- **Window** (Options → Display): `windowmode` `0` windowed, `1` exclusive fullscreen, `2` borderless (default).
  - `borderlessres` (`0`): in borderless, play at the resolution chosen below it and stretch it over the whole screen; Windows keeps its own resolution. `borderlessaspect` (`0`) keeps the proportions with black bars.

## Applying

Apply first: every patch before it in the series, in order (`00-build-fix` to `07-game-hud-and-demos`). It was made and checked on top of them and does not apply on its own.

```
git apply patches/08-mouse-and-window.patch
```

- Checked: applied after the previous patches of the series and compiled.


The menu paths above are those of the full L4ZY menus (patch `16-menus`). Without that patch, the settings and commands exist all the same: use them from the console or your own `.cfg` files.


## Files

- `data/defaults.cfg`
- `src/engine/main.cpp`
- `src/engine/rendergl.cpp`
