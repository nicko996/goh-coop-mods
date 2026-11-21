# Gates of Hell coop mods

Five independent mods for **Call to Arms: Gates of Hell – Ostfront** (x64, tested on game version **1.069.0**).
Install only the ones you want. They also work together with
[GOHSE](#together-with-gohse-gates-of-hell-script-extender), the Gates of Hell Script Extender.

| Mod | What it does | Who needs it |
|---|---|---|
| [Coop Game Speed](mods/coop-game-speed/README.md) | The **Backspace** "Game speed" menu works in coop, and the whole match follows the chosen speed | **Only the host** |
| [Coop Checkpoint](mods/coop-checkpoint/README.md) | **Save** / **Reload** buttons for mid-mission saves in coop | **Every player** in the match |
| [Loot Highlight](mods/loot-highlight/README.md) | The **V** key also outlines crates, empty vehicles and other containers with items, and you can open them from there | Only you (client-side, visual) |
| [Inspect Preview](mods/inspect-preview/README.md) | With **V** held down, hovering a corpse or container shows its inventory grid, look only | Only you (client-side, visual) |
| [Historical Doctrine](mods/historical-doctrine/README.md) | A **Historical** doctrine in the 20 skirmish missions (only the units that were really there), compatible with other mission mods such as Spawn Menu | **Every player** in the match |

The mods don't depend on each other. Inspect Preview works on corpses by itself. To preview crates and
vehicles too, install Loot Highlight as well, because that mod lets V hover them.

Every mod has its own version and its own releases (tag `<mod>-v<version>`, e.g. `loot-highlight-v1.0.2`).
What changed in each version is in the [changelog](CHANGELOG.md).

## Install

1. Download the zip of each mod you want from [Releases](../../releases): the newest release of each mod has
   `goh-<mod>-v<version>.zip`, and `goh-all-mods.zip` in the [latest release](../../releases/latest) has the
   current version of all four.
2. Extract each zip into the game folder, the one that contains `binaries` and `mods`, e.g.
   `C:\Program Files (x86)\Steam\steamapps\common\Call to Arms - Gates of Hell`.
   If Windows asks, overwrite `version.dll`: it's the same file in every zip. **If you use GOHSE, don't
   overwrite it**: see [Together with GOHSE](#together-with-gohse-gates-of-hell-script-extender).
3. **Coop Checkpoint only:** start the game and enable **coop_checkpoint** in *Options › Mods*.

After installing you should have:

```
<game>\binaries\x64\version.dll                  the mod loader (shared)
<game>\binaries\x64\goh_mods\<mod>.dll           one DLL per installed mod
<game>\gohse\mods\goh_dll_mods\                  loads the mods when GOHSE is installed (see below), otherwise unused
<game>\mods\coop_checkpoint\                     Coop Checkpoint only (data mod)
```

**Uninstall a mod:** delete its DLL from `binaries\x64\goh_mods` (for Coop Checkpoint also disable or delete
`mods\coop_checkpoint`). **Uninstall everything:** also delete `binaries\x64\version.dll`, the `goh_mods` folder
and `gohse\mods\goh_dll_mods`.

### Together with GOHSE (Gates of Hell Script Extender)

[GOHSE](https://github.com/nature2468/gohse) is a `binaries\x64\version.dll` too, so it can't be installed next to
this loader: the `version.dll` you copy last replaces the other one. The mods work with either of them.

- **With GOHSE:** keep GOHSE's `version.dll`. When you extract a mod's zip, **skip `version.dll`** (answer *no*
  when Windows asks to replace it). If you replaced it by mistake, extract GOHSE's zip again. The mods are then
  loaded by the script every zip puts in `<game>\gohse\mods\goh_dll_mods\`: GOHSE runs it at every start, in the
  game and in the dedicated server, and there is nothing to enable. GOHSE's console and its logs in
  `<game>\gohse\logs` list the DLLs it loaded (lines with `[nicko996.goh-dll-mods]`).
- **Without GOHSE:** nothing changes. `version.dll` is this loader and the `gohse` folder does nothing.

This needs Coop Game Speed 1.0.2, Coop Checkpoint 1.0.4, Loot Highlight 1.0.3, Inspect Preview 1.0.4,
Historical Doctrine 1.0.0, or newer. Under GOHSE the mods are loaded a few seconds later than with this loader,
right after the game has applied its mods. Made for GOHSE 0.1.2 (Lua API v1) on game version 1.069.0.

> Upgrading from the old single-DLL builds ("Coop Game Speed v2 + checkpoint", build ≤ 38): the new
> `version.dll` replaces the old one. Delete any `mods\coop_checkpoint\install` folder left by older packages.
> Don't enable the old optional `coop_game_speed` data mod (speed buttons) together with `coop_checkpoint`,
> because both change the same mission files.

## How it works

`version.dll` is a proxy of the Windows system DLL of the same name, which the game and its dedicated server
import. It forwards every export to the real `System32\version.dll`. In `call_to_arms.exe` and
`call_to_arms_server.exe` it also loads every DLL in `binaries\x64\goh_mods\`. Other programs that happen to
load it are left alone.

With GOHSE's `version.dll` installed instead, the same DLLs are loaded by the GOHSE script mod in
`gohse\mods\goh_dll_mods` (`loader/gohse_bridge`: a `mod.json` and a `bootstrap.lua` that calls `LoadLibrary`
through LuaJIT's FFI). GOHSE runs it right after the game has applied its mods, on the game's own thread, so the
script waits until every DLL has installed its patches before the game goes on. A DLL that is already loaded is
left alone, and the DLLs stay loaded until the game exits.

Each mod DLL finds the code it needs by byte signatures and patches it in memory only. The game files on disk
are never modified. If a game update changes the code a mod needs, that mod skips the part it can't find and
writes `ERROR: ...` in its log, and the game runs unmodded for that part.

Logs (in `%TEMP%`, overwritten at every start):

| File | Content |
|---|---|
| `goh_mod_loader_<exe>.log` | which mod DLLs were loaded (with GOHSE: its console and `<game>\gohse\logs` instead) |
| `goh_<mod>_<exe>.log` | what each mod found and patched, plus runtime messages |

`<exe>` is `call_to_arms.exe` (your game) or `call_to_arms_server.exe` (the local dedicated server the host's
game starts for a multiplayer match).

## Build from source

Requirements: Windows x64, [Zig](https://ziglang.org/) 0.13 or newer (`winget install zig.zig`; tested with
0.17), PowerShell. No Visual Studio or Windows SDK needed: Zig ships its own MinGW headers and linker.

```powershell
.\build.ps1                                   # -> build\binaries\x64\version.dll + goh_mods\*.dll, build\gohse
.\build.ps1 -Install "C:\...\Call to Arms - Gates of Hell"   # build and copy into the game (all the mods)
```

`-Install` keeps a `version.dll` that isn't this loader (GOHSE's, for example) and says so; `-ReplaceLoader`
overwrites it.

Each DLL is a single C file, compiled as:

```
zig cc -target x86_64-windows-gnu -shared -O2 -s -Wall -Wextra -o <out>.dll <source>.c -luser32
```

| Output | Source |
|---|---|
| `version.dll` | `loader/version_proxy.c` |
| `goh_mods/coop_game_speed.dll` | `mods/coop-game-speed/coop_game_speed.c` |
| `goh_mods/coop_checkpoint.dll` | `mods/coop-checkpoint/coop_checkpoint.c` |
| `goh_mods/loot_highlight.dll` | `mods/loot-highlight/loot_highlight.c` |
| `goh_mods/inspect_preview.dll` | `mods/inspect-preview/inspect_preview.c` |

Shared code is in `common/`: `goh_common.h` (signature scanning, code patching, hooks, logging, DLL entry),
`goh_loot.h` (container filter shared by the two loot mods) and `goh_sdl_hook.h` (the hook on the game's text
reader for mods that change files while they load, such as Coop Checkpoint and Historical Doctrine: every mod
that uses it installs under the same mutex and calls the previous one, so they chain).

A mingw-w64 GCC works too (`x86_64-w64-mingw32-gcc -shared -O2 -s ...`). MSVC doesn't, because the loader
uses GCC inline assembly for its export stubs.

### Coop Checkpoint data mod

The `coop_checkpoint` data mod (button icons and texts; the DLL adds the buttons to the missions while the game
reads them) is generated from the installed game's own resource packs, so it isn't stored in the repository:

```
python mods/coop-checkpoint/data/make_coop_checkpoint_mod.py "<game folder>" [<output mods folder>]
```

It needs Python 3.8+ and Pillow for the button icons (`pip install pillow`). After changing the mission patch,
run `python tools/test_mission_patch.py "<game folder>" [<mod folder> ...]` (needs zig): it compares the DLL's
patch (`mods/coop-checkpoint/mission_patch.h`) with the generator's reference `patch()` on every mission.

### GOHSE bridge

`loader/gohse_bridge` is not compiled: `build.ps1` copies it to `build\gohse\mods\goh_dll_mods\resource\script\gohse`.
After changing `bootstrap.lua`, run `python tools/test_gohse_bridge.py <lua51.dll>` with the `lua51.dll` of any
64-bit LuaJIT (needs zig). It runs the script twice in a real LuaJIT against a fake game folder and checks the load
order, the error and warning lines, and that a second run loads nothing again.

### Release zips

```
.\build.ps1
python tools/package.py "<game folder>"      # -> build\release\goh-<mod>-v<mod version>.zip, goh-all-mods.zip
```

Every mod has its own version: `GOH_MOD_VERSION` next to `GOH_MOD_NAME` in the mod's `.c` file. The loader's is
`GOH_LOADER_VERSION` in `loader/version_proxy.c`. Bump only what changed; a change to `common/goh_loot.h`
concerns both loot mods. Release tags are per mod, `<mod id>-v<version>` (e.g. `loot-highlight-v1.0.2`), and
`loader-v<version>`. `goh-all-mods.zip` lists every version in `goh_mods_readme/VERSION.txt`. Up to v1.0.1 the
repository used a single version and tag for everything.

### Nexus Mods

Each mod also has a page on [Nexus Mods](https://www.nexusmods.com/calltoarmsgatesofhellostfront/mods/22)
(pages 22-25, listed in `.github/nexus-mods.json`). Publishing a GitHub release `<mod>-v<version>` runs
`.github/workflows/nexus-upload.yml`. It uploads that release's `goh-<mod>-v<version>.zip` as a new version of
the mod's main file on Nexus, updates the page version, and adds the mod's section of `CHANGELOG.md` as the Nexus
changelog. Older versions stay listed. It needs the repository secret `NEXUSMODS_API_KEY` (the personal API key
from Nexus Mods › Site preferences › API keys). To upload an existing release again, run the workflow by hand
(*Actions › Upload to Nexus Mods › Run workflow*) with its tag.

The page descriptions are edited through `.github/workflows/nexus-description.yml` (manual run, same secret):
`get` saves a mod's current BBCode description as a workflow artifact, `update` sends
`.github/nexus-descriptions/<mod>.bbcode` as the new description (API v3 `PATCH /v3/mods/{id}`).

## Repository layout

```
common/                 shared headers (goh_common.h, goh_loot.h, goh_sdl_hook.h)
loader/                 version.dll proxy that loads binaries\x64\goh_mods\*.dll
loader/gohse_bridge/    GOHSE script mod that loads the same DLLs when GOHSE's version.dll is installed
mods/historical-doctrine/  Historical Doctrine DLL + README, data/ = patch and data-mod generator
mods/coop-game-speed/   Coop Game Speed DLL + README
mods/coop-checkpoint/   Coop Checkpoint DLL + README, data/ = data-mod generator
mods/loot-highlight/    Loot Highlight DLL + README
mods/inspect-preview/   Inspect Preview DLL + README
tools/package.py        release zips
build.ps1               builds every DLL with Zig
CHANGELOG.md            what changed in each mod version
.github/                Nexus Mods upload workflow + the Nexus page of each mod
```

## Disclaimer

Unofficial fan mods, not affiliated with Barbedwire Studios or 1C Entertainment. They patch the running game
in memory. Use at your own risk, and turn them off before reporting game bugs to the developers.
