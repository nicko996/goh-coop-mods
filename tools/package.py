"""
Builds the release zips from the output of build.ps1 and of the coop_checkpoint data-mod generator.

Usage
    python tools/package.py "<game_dir>"

Steps
 1. expects build/binaries/x64/version.dll and build/binaries/x64/goh_mods/*.dll (run build.ps1 first)
 2. generates the coop_checkpoint data mod from the game's resource packs into build/data
 3. writes build/release/goh-<mod>-v<mod version>.zip for every mod, plus goh-all-mods.zip (its
    goh_mods_readme/VERSION.txt lists the version of every mod and of the loader)

Versions: every mod has its own (GOH_MOD_VERSION in its .c file), the loader too (GOH_LOADER_VERSION in
loader/version_proxy.c).  Release tags: <mod id>-v<version>, loader-v<version>.

Every zip is laid out like the game folder: extract it into <game_dir> (the folder that contains
binaries/ and mods/).  version.dll (the loader) is in every zip and is the same file in all of them.
"""
import os
import re
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "binaries", "x64")
DATA = os.path.join(ROOT, "build", "data")
OUT = os.path.join(ROOT, "build", "release")

# mod id -> (DLL name, folder with README.md and the source, source file, data mod folder name or None)
MODS = {
    "coop-game-speed": ("coop_game_speed.dll", "mods/coop-game-speed", "coop_game_speed.c", None),
    "coop-checkpoint": ("coop_checkpoint.dll", "mods/coop-checkpoint", "coop_checkpoint.c", "coop_checkpoint"),
    "loot-highlight": ("loot_highlight.dll", "mods/loot-highlight", "loot_highlight.c", None),
    "inspect-preview": ("inspect_preview.dll", "mods/inspect-preview", "inspect_preview.c", None),
}


def read_version(path, macro):
    with open(os.path.join(ROOT, path), encoding="utf-8") as f:
        return re.search(r'#define %s "([^"]+)"' % macro, f.read()).group(1)


def mod_version(mod_id):
    return read_version(MODS[mod_id][1] + "/" + MODS[mod_id][2], "GOH_MOD_VERSION")


def loader_version():
    return read_version("loader/version_proxy.c", "GOH_LOADER_VERSION")


def add_tree(z, src, arc_prefix):
    for d, _, files in os.walk(src):
        for name in sorted(files):
            full = os.path.join(d, name)
            z.write(full, arc_prefix + "/" + os.path.relpath(full, src).replace(os.sep, "/"))


def write_zip(path, mod_ids):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.write(os.path.join(BIN, "version.dll"), "binaries/x64/version.dll")
        for mod_id in mod_ids:
            dll, folder, _, data = MODS[mod_id]
            z.write(os.path.join(BIN, "goh_mods", dll), "binaries/x64/goh_mods/" + dll)
            z.write(os.path.join(ROOT, folder, "README.md"), "goh_mods_readme/%s.md" % mod_id)
            if data:
                add_tree(z, os.path.join(DATA, data), "mods/" + data)
        lines = ["%s v%s" % (mod_id, mod_version(mod_id)) for mod_id in mod_ids] + ["loader v%s" % loader_version()]
        z.writestr("goh_mods_readme/VERSION.txt", "\r\n".join(lines) + "\r\n")
    print("%-40s %6.1f MB" % (os.path.basename(path), os.path.getsize(path) / 1e6))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    game_dir = sys.argv[1]
    for dll in ["version.dll"] + ["goh_mods/" + m[0] for m in MODS.values()]:
        if not os.path.isfile(os.path.join(BIN, dll)):
            sys.exit("missing build/binaries/x64/%s: run build.ps1 first" % dll)
    gen = os.path.join(ROOT, "mods", "coop-checkpoint", "data", "make_coop_checkpoint_mod.py")
    subprocess.run([sys.executable, gen, game_dir, DATA], check=True)
    os.makedirs(OUT, exist_ok=True)
    for mod_id in MODS:
        write_zip(os.path.join(OUT, "goh-%s-v%s.zip" % (mod_id, mod_version(mod_id))), [mod_id])
    write_zip(os.path.join(OUT, "goh-all-mods.zip"), list(MODS))


if __name__ == "__main__":
    main()
