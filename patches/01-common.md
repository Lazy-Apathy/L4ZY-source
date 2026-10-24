# Common groundwork

Patch `01-common.patch`, from the L4ZY 2026.10.16.1 source.

Small engine changes that several features use, gathered here so that each feature patch stays small. On its own it changes almost nothing you can see: console messages from other threads and the filter that keeps technical lines in `log.txt`, MSAA depth and colour read-back, the colour slot of weapon lights, shader helpers, and small changes of the 2020 code that several features build on.

## Applying

Apply first: `00-build-fix`. Nothing else is needed: it stands on its own on top of it.

```
git apply patches/01-common.patch
```

- Checked: applied on top of `00-build-fix` only, and compiled.


## Files

- `data/glsl.cfg`
- `src/engine/animmodel.h`
- `src/engine/command.cpp`
- `src/engine/console.cpp`
- `src/engine/dynlight.cpp`
- `src/engine/engine.h`
- `src/engine/main.cpp`
- `src/engine/menus.cpp`
- `src/engine/rendergl.cpp`
- `src/engine/rendermodel.cpp`
- `src/engine/skelmodel.h`
- `src/engine/sound.cpp`
- `src/engine/texture.cpp`
- `src/engine/texture.h`
- `src/engine/water.cpp`
- `src/engine/worldio.cpp`
- `src/fpsgame/ai.h`
- `src/fpsgame/aiman.h`
- `src/fpsgame/client.cpp`
- `src/fpsgame/entities.cpp`
- `src/fpsgame/fps.cpp`
- `src/fpsgame/game.h`
- `src/fpsgame/render.cpp`
- `src/fpsgame/server.cpp`
- `src/fpsgame/waypoint.cpp`
- `src/fpsgame/weapon.cpp`
- `src/shared/iengine.h`
- `src/shared/igame.h`
