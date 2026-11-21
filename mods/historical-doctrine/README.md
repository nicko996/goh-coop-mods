# Historical Doctrine (DLL edition)

Adds a fifth doctrine, **"Doctrine: Historical"**, to all 20 skirmish missions (single player and coop): only the
units that were really there for that battle, at that date, with those formations, unlocked in mixed packages for
every sector you hold, and free deployment anywhere on the map from 4 sectors. The four vanilla doctrines are
unchanged.

This is the same doctrine as the Steam Workshop mod
[Historical Doctrine for Skirmishes](https://steamcommunity.com/sharedfiles/filedetails/?id=3838675132), unit
lists in the [unit guide](https://steamcommunity.com/sharedfiles/filedetails/?id=3840541746). The difference:

| | Workshop version | DLL edition (this) |
|---|---|---|
| How | ships modified copies of the 20 missions | adds the doctrine in memory to whatever mission the game reads |
| Other mods that change the skirmish missions (e.g. **Spawn Menu**) | conflict: only one of the two wins | work together, in any mod-list order |
| Install | subscribe on Steam | extract the zip into the game folder + enable the data mod |

Use **one or the other**, not both. (If both are active the DLL sees that the mission already has the doctrine
and leaves it alone.)

**Every player in a coop match needs it**: the DLL, its patch folder and the `historical_doctrine` data mod
enabled (the mod list must match anyway to join).

## Install

1. Extract `goh-historical-doctrine-v*.zip` into the game folder (see the [main README](../../README.md)). Files:
   `binaries\x64\version.dll`, `binaries\x64\goh_mods\historical_doctrine.dll`,
   `binaries\x64\goh_mods\historical_doctrine\*.hdp` (the mission patches), `mods\historical_doctrine\` (texts).
2. In the game: *Options › Mods* → enable **Historical Doctrine (DLL)**. If you had the Workshop version,
   unsubscribe from it (or disable it).
3. In a skirmish, choose **Doctrine: Historical (...)** in the mission options.

With [GOHSE](https://github.com/nature2468/gohse) installed, keep GOHSE's `version.dll` (don't overwrite it):
the script the zip puts in `gohse\mods\goh_dll_mods` loads the mod. See
[Together with GOHSE](../../README.md#together-with-gohse-gates-of-hell-script-extender).

## How it works

Found by reverse engineering, game v1.067. Every peer (each player's game and the host's local dedicated server)
runs the DLL and patches the same files the same way, so coop stays in sync.

- Every text file the game loads (`.mi`, `.set`, ...) goes through its SDL reader, which reads the whole file into a
  string and then calls the lexer. The DLL redirects that call (same hook point as Coop Checkpoint). For the
  20 skirmish missions' `0.mi` and `interface.set` it applies the matching patch before the lexer runs.
- A patch (`*.hdp`, made by `data/make_hd_data.py` from the Workshop mod) is a list of *hunks*: lines to insert
  (and, in `interface.set`, one line to replace), each located by the vanilla lines around it rather than by line
  numbers. Each spot must be found exactly once, first with the lines before and after it, then with only the
  lines after, then only the lines before, so a mod that added something at the same place doesn't break it.
  Lines another mod added at the same spot always end up before ours, whichever mod patched first, so every
  coop peer gets the same text (`tools/test_hd_coop_order.py` checks this with Coop Checkpoint on all 20 missions).
  The Historical doctrine itself only **adds** to the mission: new reinforcement entries, triggers, menus and the
  mission option.
- If any hunk can't be placed (a mod changed that part of the mission too much), the file is left exactly as it
  is and the log says which hunk: the mission then simply has no Historical doctrine. A file that already has the
  doctrine (the Workshop version) is left alone.
- The hook is the shared one of `common/goh_sdl_hook.h`: every mod that uses it installs under the same named
  mutex and calls the previous hook after its own work, so they chain instead of overwriting each other. Coop
  Checkpoint uses it from **1.0.3**: with Coop Checkpoint 1.0.2 or older, update it (the old version hooks the same
  call on its own and one of the two can be lost at startup).

## Known limitations

- Only the 20 skirmish missions listed in the [Workshop page](https://steamcommunity.com/sharedfiles/filedetails/?id=3838675132).
- A mod that rewrites the same reinforcement triggers of a mission can make that mission's patch fail (logged,
  mission left vanilla). Spawn Menu only adds one line at the end of each mission, so it works.
- All players need the same DLL version and patches.

## Files

- `historical_doctrine.c`: the DLL (hook, patch index, logging).
- `hd_patch.h`: applies a patch to a text; plain C, also built into `tools/hd_patch_test.c`.
- `data/make_hd_data.py`: makes the patches and the data mod from the Workshop mod and the game's own files. Its
  `apply()` is the reference for `hd_patch.h` (`tools/test_hd_patch.py` compares the two, also on other mods' copies
  of the missions).
- Log: `%TEMP%\goh_historical_doctrine_call_to_arms.exe.log` (and `..._call_to_arms_server.exe.log` on the host).
