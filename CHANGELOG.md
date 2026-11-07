# Changelog

Every mod has its own version since 2026-11-01. Release tags are `<mod>-v<version>` (and `loader-v<version>` for
`version.dll`). Before that, the whole repository used a single version (`v1.0.0`, `v1.0.1`).

Current versions: Coop Game Speed 1.0.1, Coop Checkpoint 1.0.1, Loot Highlight 1.0.2, Inspect Preview 1.0.3,
loader 1.0.1.

## Inspect Preview 1.0.3 (2026-11-07)

- The camera **no longer stops** when the preview opens or closes while you move it with the keyboard (WASD) or
  rotate it with the middle mouse button. The game activated the preview window like any dialog, and activating
  or deactivating a window releases every held key and mouse button and resets the keyboard camera. The preview
  is look only and is no longer activated; every other window behaves as before.

## Loot Highlight 1.0.2 (2026-11-01)

- Vehicles and guns with a **crew on board** are no longer highlighted. Only empty ones are, for example
  abandoned vehicles, and still not your own. The test is the game's own (`eActor+0xdb4` bit 8, used by the
  examine action).
- While V is held, **examine comes before board** on empty vehicles, for the cursor and on click. Release V to
  board them as usual.
- When a real exchange opens while V is held, **V switches off** as in the game. Before, it stayed on until V
  was pressed again.

## Inspect Preview 1.0.2 (2026-11-01)

- No preview on vehicles and guns with a crew on board (same filter as Loot Highlight 1.0.2).
- When a real exchange opens while V is held, V switches off as in the game (shared fix).

## Repository (2026-11-01)

- Per-mod versions: `GOH_MOD_VERSION` in each mod's `.c` file, `GOH_LOADER_VERSION` in the loader;
  `common/goh_version.h` is gone. `tools/package.py` names each zip after its mod's version and writes
  `goh-all-mods.zip` with a `VERSION.txt` listing every version.

## v1.0.1 (2026-10-31, all mods)

- **Coop Checkpoint**: minimal buttons that show only **Save** and **Reload**, with no icon and no label.
- The other mods were unchanged.

## v1.0.0 (2026-10-31, all mods)

- First release: the loader (`version.dll`) and four independent mods split from the old single DLL
  (build 38): Coop Game Speed, Coop Checkpoint, Loot Highlight, Inspect Preview.
