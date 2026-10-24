# Settings search and settings assistant (local AI)

Patch `14-settings-search-and-assistant.patch`, from the L4ZY 2026.10.16.1 source.

**Menu:** Options → *Search settings…*

- Type a word, in English or French: the matching settings of every Options page appear as real checkboxes and sliders you can change right there, grouped by page, with a link to the page.
- It also finds settings by keyword (`son`, `souris`, `viseur`, `lumière`…); accents and capitals do not matter.
- It also finds colour pickers (my colour, friend colours, crosshair colour), text fields (sensitivity, DPI, HUD positions…) and options written on one line.

Ask in plain words how to find or change a setting, and the translation model answers: where it is, what it does, what its values mean, the current value. When you ask for a change (or answer its question), it proposes it.

- **Nothing changes until you click Apply** next to a proposal; **Undo** puts the old value back.
- It can change game settings, put actions on keys, including up to 4 actions on one key (e.g. `bind LSHIFT "setweapon RI; attack"`), open a settings page, and open the HUD editor.
- It can never run scripts, quit, connect, chat, record, touch files, servers or keys: every proposal is checked by the game before it is shown and again when you click, whatever the model writes.
- It uses your installed translation model (no internet unless you set an API key).
- It knows the real menu path of every setting (for example Options → HUD → *Crosshair…*) and can open the page for you.

**Menu:** Options → *Settings assistant (AI)…*, or `/assistant`

| Setting / command | What it does | Default |
|---|---|---|
| *Think first* (checkbox in the window) | the model thinks before answering: a few seconds slower; unticked again when you leave the window | off |
| `assistantask "<question>"` | ask from the console | |

## Applying

Apply first: every patch before it in the series, in order (`00-build-fix` to `13-match-videos`). It was made and checked on top of them and does not apply on its own.

```
git apply patches/14-settings-search-and-assistant.patch
```

- Checked: applied after the previous patches of the series and compiled.


The menu paths above are those of the full L4ZY menus (patch `16-menus`). Without that patch, the settings and commands exist all the same: use them from the console or your own `.cfg` files.


## Files

- `data/assistant-commands.txt`
- `data/assistant-menus.txt`
- `data/assistant-settings.txt`
- `data/settings-keywords.txt`
- `distrib/service/assistant-knowledge.txt`
- `distrib/service/assistant-prompt.txt`
- `distrib/service/menu_plan.py`
- `src/engine/menus.cpp`
- `tools/assistant-settings-docs.py`
