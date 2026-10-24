# Menu drawing (look of the menus, scrolling, new menu controls)

Patch `02-menu-drawing.patch`, from the L4ZY 2026.10.16.1 source.

The way the menus are drawn: colours, layout, scrolling of long pages, and the menu controls the L4ZY menus use (banners, colour swatches, text fields, player preview). It does not change the menus themselves: that is the `menus` patch.

## Applying

Apply first: `00-build-fix`, `01-common`. Nothing else is needed: it stands on its own on top of them.

```
git apply patches/02-menu-drawing.patch
```

- Checked: applied on top of `00-build-fix`, `01-common` only, and compiled.


## Files

- `src/engine/3dgui.cpp`
- `src/engine/engine.h`
- `src/engine/menus.cpp`
- `src/engine/serverbrowser.cpp`
- `src/fpsgame/render.cpp`
- `src/fpsgame/scoreboard.cpp`
- `src/rpggame/rpg.cpp`
- `src/shared/iengine.h`
