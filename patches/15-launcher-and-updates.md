# Launcher, installer and updates

Patch `15-launcher-and-updates.patch`, from the L4ZY 2026.10.16.1 source.

Main menu → **L4ZY `<version>` – Updates**:

- **Check for updates**: asks the update channel now.
- **Update and restart (`<version>`, `<size>`)**: downloads in the background with progress. You can keep playing.
- **Restart now to install**: the game closes, the update is installed, and L4ZY restarts on the new version.
  - If you stay in the Updates menu, outside a game, it restarts automatically once the download is done.
  - If you are in a match, nothing happens until you click.

How updates are applied:

- Only the parts that changed are downloaded: a game fix is a few MB, and maps and translation models are never re-downloaded.
- Every file is checked (HTTPS, size and SHA-256) before anything is replaced.
- Nothing is replaced while the game runs.
- A backup is kept. If an update is interrupted or fails, the previous version is put back automatically.
- **Go back to the previous version:** Start menu → *L4ZY – go back to the previous version* (with the game closed).
- Updates never touch your settings, translation corrections or models.

`tupdatecheck`, `tupdateapply`, `tupdaterestart` and `tupdateinstalled`: the same actions as the Updates menu.

## Applying

Apply first: every patch before it in the series, in order (`00-build-fix` to `14-settings-search-and-assistant`). It was made and checked on top of them and does not apply on its own.

```
git apply patches/15-launcher-and-updates.patch
```

- Checked: applied after the previous patches of the series and compiled.


The menu paths above are those of the full L4ZY menus (patch `16-menus`). Without that patch, the settings and commands exist all the same: use them from the console or your own `.cfg` files.


## Files

- `distrib/docs/licenses/THIRD-PARTY-NOTICES.txt`
- `distrib/docs/licenses/sauerbraten-README.html`
- `distrib/docs/licenses/sauerbraten-source-license.txt`
- `distrib/game/autoexec.cfg`
- `distrib/installer/l4zy.nsi`
- `distrib/launcher/build.bat`
- `distrib/launcher/l4zy.ico`
- `distrib/launcher/launcher.cpp`
- `distrib/launcher/launcher.manifest`
- `distrib/launcher/launcher.rc`
- `distrib/service/updater.py`
- `src/engine/main.cpp`
