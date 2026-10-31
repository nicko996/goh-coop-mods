"""
Builds the release zips from the output of build.ps1 and of the coop_checkpoint data-mod generator.

Usage
    python tools/package.py "<game_dir>"

Steps
 1. expects build/binaries/x64/version.dll and build/binaries/x64/goh_mods/*.dll (run build.ps1 first)
 2. generates the coop_checkpoint data mod from the game's resource packs into build/data
 3. writes build/release/goh-<mod>-v<version>.zip for every mod plus goh-all-mods-v<version>.zip

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

# mod id -> (DLL name, folder with README.md, data mod folder name or None)
MODS = {
    "coop-game-speed": ("coop_game_speed.dll", "mods/coop-game-speed", None),
    "coop-checkpoint": ("coop_checkpoint.dll", "mods/coop-checkpoint", "coop_checkpoint"),
    "loot-highlight": ("loot_highlight.dll", "mods/loot-highlight", None),
    "inspect-preview": ("inspect_preview.dll", "mods/inspect-preview", None),
}


def version():
    with open(os.path.join(ROOT, "common", "goh_version.h"), encoding="utf-8") as f:
        return re.search(r'GOH_MOD_VERSION "([^"]+)"', f.read()).group(1)


def add_tree(z, src, arc_prefix):
    for d, _, files in os.walk(src):
        for name in sorted(files):
            full = os.path.join(d, name)
            z.write(full, arc_prefix + "/" + os.path.relpath(full, src).replace(os.sep, "/"))


def write_zip(path, mod_ids, ver):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.write(os.path.join(BIN, "version.dll"), "binaries/x64/version.dll")
        for mod_id in mod_ids:
            dll, folder, data = MODS[mod_id]
            z.write(os.path.join(BIN, "goh_mods", dll), "binaries/x64/goh_mods/" + dll)
            z.write(os.path.join(ROOT, folder, "README.md"), "goh_mods_readme/%s.md" % mod_id)
            if data:
                add_tree(z, os.path.join(DATA, data), "mods/" + data)
        z.writestr("goh_mods_readme/VERSION.txt", "GoH coop mods v%s\r\n" % ver)
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
    ver = version()
    for mod_id in MODS:
        write_zip(os.path.join(OUT, "goh-%s-v%s.zip" % (mod_id, ver)), [mod_id], ver)
    write_zip(os.path.join(OUT, "goh-all-mods-v%s.zip" % ver), list(MODS), ver)


if __name__ == "__main__":
    main()
