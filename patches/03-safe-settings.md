# Safer settings files

Patch `03-safe-settings.patch`, from the L4ZY 2026.10.16.1 source.

An unreadable `config.cfg` is never overwritten, and a backup `config.cfg.bak` is kept.

## Applying

Apply first: `00-build-fix`, `01-common`. Nothing else is needed: it stands on its own on top of them.

```
git apply patches/03-safe-settings.patch
```

- Checked: applied on top of `00-build-fix`, `01-common` only, and compiled.


## Files

- `src/engine/command.cpp`
- `src/engine/main.cpp`
