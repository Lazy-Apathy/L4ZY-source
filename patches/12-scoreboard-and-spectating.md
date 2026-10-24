# Scoreboard columns, spectator list and Master menu

Patch `12-scoreboard-and-spectating.patch`, from the L4ZY 2026.10.16.1 source.

- **Scoreboard columns** (Options → HUD → *Scoreboard (Tab)…*): `showfrags` (`1`), `showkd`, `showflags`, `showaccuracy`, `showstatus` (`0`), `showlang` (`1`), `showvs` (`1`: your kills-deaths against each player), and `scoreboardalpha` for the background opacity (`40`).
  - Admin, auth and master are shown as a tag after the name; names are dimmed while a player is dead.

- **Spectating:** a list of the players on the right (`spectatorlist`, `1`, Options → HUD → *Scoreboard (Tab)…*). Hold Tab and click a name to follow that player. A small eye marks the player you watch, in the list, the scoreboard and the Master menu.

- **Master menu** (main menu → *Master*, in a game): each player's client number, and the actions as icons (hover one to see what it does).

## Applying

Apply first: every patch before it in the series, in order (`00-build-fix` to `11-tiny-apa-bots`). It was made and checked on top of them and does not apply on its own.

```
git apply patches/12-scoreboard-and-spectating.patch
```

- Checked: applied after the previous patches of the series and compiled.


The menu paths above are those of the full L4ZY menus (patch `16-menus`). Without that patch, the settings and commands exist all the same: use them from the console or your own `.cfg` files.


## Files

- `data/following.png`
- `data/master_give.png`
- `data/master_kick.png`
- `data/master_spectator.png`
- `data/master_take.png`
- `data/master_team.png`
- `src/fpsgame/entities.cpp`
- `src/fpsgame/game.h`
- `src/fpsgame/render.cpp`
- `src/fpsgame/scoreboard.cpp`
