# Gates of Hell coop mods

Four independent mods for **Call to Arms: Gates of Hell – Ostfront** (x64, tested on game version **1.067.0**).
Install only the ones you want.

| Mod | What it does | Who needs it |
|---|---|---|
| [Coop Game Speed](mods/coop-game-speed/README.md) | The **Backspace** "Game speed" menu works in coop, and the whole match follows the chosen speed | **Only the host** |
| [Coop Checkpoint](mods/coop-checkpoint/README.md) | **Save** / **Reload** buttons for mid-mission saves in coop | **Every player** in the match |
| [Loot Highlight](mods/loot-highlight/README.md) | The **V** key also outlines crates, empty vehicles and other containers with items, and you can open them from there | Only you (client-side, visual) |
| [Inspect Preview](mods/inspect-preview/README.md) | With **V** held down, hovering a corpse or container shows its inventory grid, look only | Only you (client-side, visual) |

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
   If Windows asks, overwrite `version.dll`: it's the same file in every zip.
3. **Coop Checkpoint only:** start the game and enable **coop_checkpoint** in *Options › Mods*.

After installing you should have:

```
<game>\binaries\x64\version.dll                  the mod loader (shared)
<game>\binaries\x64\goh_mods\<mod>.dll           one DLL per installed mod
<game>\mods\coop_checkpoint\                     Coop Checkpoint only (data mod)
```

**Uninstall a mod:** delete its DLL from `binaries\x64\goh_mods` (for Coop Checkpoint also disable or delete
`mods\coop_checkpoint`). **Uninstall everything:** also delete `binaries\x64\version.dll` and the `goh_mods` folder.

> Upgrading from the old single-DLL builds ("Coop Game Speed v2 + checkpoint", build ≤ 38): the new
> `version.dll` replaces the old one. Delete any `mods\coop_checkpoint\install` folder left by older packages.
> Don't enable the old optional `coop_game_speed` data mod (speed buttons) together with `coop_checkpoint`,
> because both change the same mission files.

## How it works

`version.dll` is a proxy of the Windows system DLL of the same name, which the game and its dedicated server
import. It forwards every export to the real `System32\version.dll`. In `call_to_arms.exe` and
`call_to_arms_server.exe` it also loads every DLL in `binaries\x64\goh_mods\`. Other programs that happen to
load it are left alone.

Each mod DLL finds the code it needs by byte signatures and patches it in memory only. The game files on disk
are never modified. If a game update changes the code a mod needs, that mod skips the part it can't find and
writes `ERROR: ...` in its log, and the game runs unmodded for that part.

Logs (in `%TEMP%`, overwritten at every start):

| File | Content |
|---|---|
| `goh_mod_loader_<exe>.log` | which mod DLLs were loaded |
| `goh_<mod>_<exe>.log` | what each mod found and patched, plus runtime messages |

`<exe>` is `call_to_arms.exe` (your game) or `call_to_arms_server.exe` (the local dedicated server the host's
game starts for a multiplayer match).

## Build from source

Requirements: Windows x64, [Zig](https://ziglang.org/) 0.13 or newer (`winget install zig.zig`; tested with
0.17), PowerShell. No Visual Studio or Windows SDK needed: Zig ships its own MinGW headers and linker.

```powershell
.\build.ps1                                   # -> build\binaries\x64\version.dll + goh_mods\*.dll
.\build.ps1 -Install "C:\...\Call to Arms - Gates of Hell"   # build and copy into the game (all four mods)
```

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
and `goh_loot.h` (container filter shared by the two loot mods).

A mingw-w64 GCC works too (`x86_64-w64-mingw32-gcc -shared -O2 -s ...`). MSVC doesn't, because the loader
uses GCC inline assembly for its export stubs.

### Coop Checkpoint data mod

The `coop_checkpoint` data mod (buttons and mission scripts for the 74 campaign and skirmish missions) is
generated from the installed game's own resource packs, so it isn't stored in the repository:

```
python mods/coop-checkpoint/data/make_coop_checkpoint_mod.py "<game folder>" [<output mods folder>]
```

It needs Python 3.8+ and optionally Pillow for the button icons (`pip install pillow`; without it the vanilla
"tactical" icon is used). Regenerate it after a game update that changes the missions. With other mods that replace the same
missions, add `--on-top-of <mod folder>` (see the [Coop Checkpoint README](mods/coop-checkpoint/README.md)).

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

## Repository layout

```
common/                 shared headers (goh_common.h, goh_loot.h)
loader/                 version.dll proxy that loads binaries\x64\goh_mods\*.dll
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
