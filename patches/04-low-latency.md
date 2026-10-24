# Low latency

Patch `04-low-latency.patch`, from the L4ZY 2026.10.16.1 source.

The game reads your mouse once the GPU has caught up, instead of queueing frames ahead. Your aim reaches the screen sooner, with the same image. It helps most with ray tracing, native HDR or V-Sync; with classic lighting and no V-Sync there is little to gain.

**Menu:** Options → Display → *Screen* tab → *Speed and latency*

| Setting | Values | Default |
|---|---|---|
| `lowlatency` | `0` off, `1` on, `2` strict (lowest latency, fewer FPS) | `1` |

- *On* keeps the GPU busy, so the frame rate is kept.
- *Strict* waits for the GPU to finish each frame before starting the next: a bit less latency again, but fewer FPS.
- Mouse handling itself (sensitivity, acceleration, raw input) is unchanged; only the moment it is read changes. No frame is generated or reordered.

## Applying

Apply first: `00-build-fix`, `01-common`. Nothing else is needed: it stands on its own on top of them.

```
git apply patches/04-low-latency.patch
```

- Checked: applied on top of `00-build-fix`, `01-common` only, and compiled.


The menu paths above are those of the full L4ZY menus (patch `16-menus`). Without that patch, the settings and commands exist all the same: use them from the console or your own `.cfg` files.


## Files

- `src/Makefile`
- `src/engine/engine.h`
- `src/engine/latency.cpp`
- `src/engine/main.cpp`
- `src/engine/rendergl.cpp`
- `src/engine/renderva.cpp`
