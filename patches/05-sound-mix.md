# Sound mix

Patch `05-sound-mix.patch`, from the L4ZY 2026.10.16.1 source.

- **Sound mix** (Options → Sound → Mix): `mixweapons`, `mixhits`, `mixpain`, `mixitems`, `mixannounce`, `mixflags`, `mixmove`, `mixworld`.
  - All go from 0 to 200 (`100` = normal). `mixreset` puts them all back to 100.

## Applying

Apply first: `00-build-fix`, `01-common`. Nothing else is needed: it stands on its own on top of them.

```
git apply patches/05-sound-mix.patch
```

- Checked: applied on top of `00-build-fix`, `01-common` only, and compiled.


The menu paths above are those of the full L4ZY menus (patch `16-menus`). Without that patch, the settings and commands exist all the same: use them from the console or your own `.cfg` files.


## Files

- `src/engine/sound.cpp`
- `src/fpsgame/fps.cpp`
- `src/rpggame/rpg.cpp`
- `src/shared/igame.h`
