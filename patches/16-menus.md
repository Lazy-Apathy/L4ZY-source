# Menus

Patch `16-menus.patch`, from the L4ZY 2026.10.16.1 source.

The L4ZY menus (`data/menus.cfg`: main menu, Options hub and every settings page) and the loading screen. It comes last: its pages show the settings of all the other patches.

L4ZY ships its own `menus.cfg`: a new main menu, and an Options hub with pages for Graphics, Display, HUD, Scoreboard, Sound, Mouse, Keys, Console, Recording, Friends and Updates.

- **Chat Translation** is on the main menu and in Options → Game.
- `/traduction` also opens it.
- **Search settings…** and **Settings assistant (AI)…** are at the top of Options.

## Applying

Apply first: every patch before it in the series, in order (`00-build-fix` to `15-launcher-and-updates`). It was made and checked on top of them and does not apply on its own.

```
git apply patches/16-menus.patch
```

- Checked: applied after the previous patches of the series and compiled.


## Files

- `data/menus.cfg`
- `src/engine/main.cpp`
