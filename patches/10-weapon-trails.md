# Weapon trail colours

Patch `10-weapon-trails.patch`, from the L4ZY 2026.10.16.1 source.

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

## Applying

Apply first: every patch before it in the series, in order (`00-build-fix` to `09-graphics`). It was made and checked on top of them and does not apply on its own.

```
git apply patches/10-weapon-trails.patch
```

- Checked: applied after the previous patches of the series and compiled.


The menu paths above are those of the full L4ZY menus (patch `16-menus`). Without that patch, the settings and commands exist all the same: use them from the console or your own `.cfg` files.


## Files

- `src/engine/renderparticles.cpp`
- `src/fpsgame/weapon.cpp`
