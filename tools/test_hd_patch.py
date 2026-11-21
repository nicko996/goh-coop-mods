# -*- coding: utf-8 -*-
"""Checks the Historical Doctrine DLL's patch code (mods/historical-doctrine/hd_patch.h) against the generator's
reference apply() (mods/historical-doctrine/data/make_hd_data.py).

    python tools/test_hd_patch.py "<game folder>" [<patch folder>] [<mod folder with a map.pak or resource\\map> ...]

For every .hdp: applies it with C and with Python to the game's own file and to each given mod's copy of the same
file (e.g. Spawn Menu: <game>\\mods\\3295913567), and requires byte-identical results.  On the vanilla file the
result must be patched; on mod copies it reports whether it patched or why not.  Needs zig on PATH.
"""
import glob
import os
import subprocess
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(REPO, "mods", "historical-doctrine", "data"))
import make_hd_data as H  # noqa: E402


def mod_reader(folder):
    paks = []
    for p in glob.glob(os.path.join(folder, "resource", "**", "*.pak"), recursive=True):
        z = zipfile.ZipFile(p)
        paks.append((z, {n.lower(): n for n in z.namelist()}))

    def read(rel):
        f = os.path.join(folder, "resource", rel.replace("/", os.sep))
        if os.path.isfile(f):
            return open(f, "rb").read()
        for z, names in paks:
            if rel.lower() in names:
                return z.read(names[rel.lower()])
        return None
    return read


def main():
    game = sys.argv[1]
    pdir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(REPO, "build", "binaries", "x64", "goh_mods",
                                                                "historical_doctrine")
    mods = sys.argv[3:]
    tmp = tempfile.mkdtemp(prefix="hdtest_")
    exe = os.path.join(tmp, "hd_patch_test.exe")
    subprocess.check_call(["zig", "cc", "-O2", "-Wall", "-Wextra", "-o", exe, os.path.join(HERE, "hd_patch_test.c")])
    vanilla = H.vanilla_reader(game)
    readers = [("vanilla", lambda rel: (vanilla(rel) or "").encode("utf-8") or None)]
    readers += [(os.path.basename(m.rstrip("\\/")), mod_reader(m)) for m in mods]
    fails = 0
    for pf in sorted(glob.glob(os.path.join(pdir, "*.hdp"))):
        patch = H.read_patch(pf)
        for name, read in readers:
            raw = read(patch["target"])
            if raw is None:
                continue
            text = raw.decode("utf-8")
            py, why = H.apply(text, patch)
            inp, outp = os.path.join(tmp, "in.txt"), os.path.join(tmp, "out.txt")
            with open(inp, "wb") as f:
                f.write(raw)
            if os.path.exists(outp):
                os.remove(outp)
            r = subprocess.run([exe, pf, inp, outp], capture_output=True, text=True)
            if r.returncode == 0:
                c = open(outp, "rb").read().decode("utf-8")
                same = py is not None and c == py
                status = "patched" if same else "MISMATCH"
            elif r.returncode == 2:
                same = py is None
                status = ("left as is: " + r.stdout.strip()) if same else "MISMATCH (C refused: %s)" % r.stdout.strip()
            else:
                same, status = False, "ERROR " + r.stderr.strip()
            if not same or (name == "vanilla" and py is None):
                fails += 1
                status = "FAIL " + status + ("" if py is not None else " / python: %s" % why)
            print("%-60s %-12s %s" % (patch["target"], name, status))
    print("\n%s" % ("OK" if fails == 0 else "%d FAILURES" % fails))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
