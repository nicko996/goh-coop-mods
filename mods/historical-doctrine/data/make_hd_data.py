# -*- coding: utf-8 -*-
"""Historical Doctrine (DLL edition): mission patches + the historical_doctrine data mod.

    python make_hd_data.py "<game folder>" "<source mod folder>" [<output folder>]

<source mod folder> is the full data mod made by the Historical Doctrine generator (the Workshop version, e.g.
<game>\\mods\\3838675132): its resource\\map\\single\\**\\{0.mi,interface.set} are compared with the game's own
files, and every difference becomes a "hunk" of the patch the DLL applies in memory when the game reads the file.

Output (default: <repo>\\build):
    binaries\\x64\\goh_mods\\historical_doctrine\\*.hdp   one patch per mission file
    data\\historical_doctrine\\                           the data mod (goes to <game>\\mods): mod.info + texts

A hunk = lines to delete (often none) + lines to insert, located by the vanilla lines around it.  The DLL tries
three ways to find the spot, each must match exactly once in the file it reads:
    1. the lines before + deleted lines + the lines after (contiguous, at least one line on each side)
    2. deleted lines + the lines after         (another mod inserted something at the same spot: ours go after it)
    3. the lines before + deleted lines        (another mod inserted something right before the lines after)
The context lengths are the shortest that are unique in the vanilla file.  If any hunk can't be placed the file
is left as it is (never half patched).  apply() below is the reference for the DLL (tools/test_hd_patch.py).

Patch file format (UTF-8, LF):
    HDP 1
    target <path relative to resource, forward slashes, lower case>
    hunks <n>
    @ <nb> <na> <nd> <ni> <kbc> <kac> <kbo> <kao>      then nb + na + nd + ni lines: before, after, deleted, inserted
"""
import difflib
import glob
import os
import shutil
import sys
import zipfile

MARKER = '"doctrine_historical"'      # already in the text = already patched (e.g. the Workshop data mod)
MAX_CTX = 400


def norm_lines(text):
    return text.replace("\r\n", "\n").split("\n")


def vanilla_reader(game):
    paks = []
    for p in sorted(glob.glob(os.path.join(game, "resource", "**", "*.pak"), recursive=True)):
        try:
            z = zipfile.ZipFile(p)
        except zipfile.BadZipFile:
            continue
        paks.append((z, {n.lower(): n for n in z.namelist()}))

    def read(rel):
        for z, names in paks:
            n = names.get(rel.lower())
            if n:
                return z.read(n).decode("utf-8")
        return None
    return read


# ------------------------------------------------------------------ matching (reference for the DLL)
class Index:
    """line -> sorted positions, to look for windows quickly"""
    def __init__(self, lines):
        self.lines = lines
        self.pos = {}
        for i, l in enumerate(lines):
            self.pos.setdefault(l, []).append(i)


def find_all(lines, window, limit=2, index=None):
    """positions where window (list of lines) starts, in order; stops after `limit` hits"""
    hits = []
    n = len(window)
    if not n or n > len(lines):
        return hits
    if index is None:
        index = Index(lines)
    # anchor on the rarest line of the window
    best = min(range(n), key=lambda k: len(index.pos.get(window[k], ())))
    for p in index.pos.get(window[best], ()):
        i = p - best
        if i < 0 or i + n > len(lines):
            continue
        if lines[i:i + n] == window:
            hits.append(i)
            if len(hits) >= limit:
                break
    return hits


def locate(lines, h, index=None):
    """start index of the deleted block (= insertion point) in lines, or None"""
    B, A, D = h["B"], h["A"], h["D"]
    tries = []
    if h["kbc"] or h["kac"]:
        # at least one line on each side (when there is one): the spot must still be "between" the same lines
        kb = max(h["kbc"], 1 if B else 0)
        ka = max(h["kac"], 1 if A else 0)
        tries.append((B[len(B) - kb:] + D + A[:ka], kb))
    # after-context before before-context: when another mod inserted lines at the same spot (typically right
    # after a block's opening line), ours go after them, whichever of the two mods patched first
    if h["kao"]:
        tries.append((D + A[:h["kao"]], 0))
    if h["kbo"]:
        tries.append((B[len(B) - h["kbo"]:] + D, h["kbo"]))
    for window, off in tries:
        hits = find_all(lines, window, index=index)
        if len(hits) == 1:
            return hits[0] + off
    return None


def apply(text, patch):
    """patched text, or (None, reason)"""
    if MARKER in text:
        return None, "already has the Historical doctrine"
    crlf = "\r\n" in text
    lines = norm_lines(text)
    index = Index(lines)
    spots = []
    for k, h in enumerate(patch["hunks"]):
        at = locate(lines, h, index)
        if at is None:
            return None, "hunk %d not found" % (k + 1)
        spots.append(at)
    for k in range(1, len(spots)):
        prev_end = spots[k - 1] + len(patch["hunks"][k - 1]["D"])
        if spots[k] < prev_end:
            return None, "hunk %d out of order" % (k + 1)
    out, cur = [], 0
    for at, h in zip(spots, patch["hunks"]):
        out += lines[cur:at]
        out += h["I"]
        cur = at + len(h["D"])
    out += lines[cur:]
    return ("\r\n" if crlf else "\n").join(out), None


# ------------------------------------------------------------------ building the hunks
def unique_len(lines, index, make, maxk):
    """smallest k in 1..maxk with make(k) unique in lines (0 if none)"""
    for k in range(1, maxk + 1):
        w = make(k)
        if w is None:
            return 0
        if len(find_all(lines, w, index=index)) == 1:
            return k
    return 0


def build_hunks(van, new):
    a, b = norm_lines(van), norm_lines(new)
    idx = Index(a)
    hunks = []
    for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(None, a, b, autojunk=False).get_opcodes():
        if tag == "equal":
            continue
        D, I = a[i1:i2], b[j1:j2]
        nb, na = min(MAX_CTX, i1), min(MAX_CTX, len(a) - i2)
        B, A = a[i1 - nb:i1], a[i2:i2 + na]

        def comb(k):
            kb, ka = (k + 1) // 2, k // 2
            if kb > nb or ka > na:
                return None
            return B[nb - kb:] + D + A[:ka]
        k = unique_len(a, idx, comb, nb + na)
        kbc, kac = ((k + 1) // 2, k // 2) if k else (0, 0)
        kbo = unique_len(a, idx, lambda k: B[nb - k:] + D if k <= nb else None, nb)
        kao = unique_len(a, idx, lambda k: D + A[:k] if k <= na else None, na)
        if not (kbc or kac or kbo or kao):
            raise SystemExit("no unique context for the hunk at vanilla line %d" % i1)
        nb2 = max(kbc, kbo)
        na2 = max(kac, kao)
        hunks.append(dict(B=B[nb - nb2:], A=A[:na2], D=D, I=I, kbc=kbc, kac=kac, kbo=kbo, kao=kao))
    return hunks


def write_patch(path, target, hunks):
    out = ["HDP 1", "target " + target, "hunks %d" % len(hunks)]
    for h in hunks:
        out.append("@ %d %d %d %d %d %d %d %d" % (len(h["B"]), len(h["A"]), len(h["D"]), len(h["I"]),
                                                h["kbc"], h["kac"], h["kbo"], h["kao"]))
        out += h["B"] + h["A"] + h["D"] + h["I"]
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out) + "\n")


def read_patch(path):
    lines = open(path, encoding="utf-8", newline="").read().split("\n")
    assert lines[0] == "HDP 1", path
    target = lines[1][len("target "):]
    n = int(lines[2].split()[1])
    pos, hunks = 3, []
    for _ in range(n):
        f = lines[pos].split()
        nb, na, nd, ni, kbc, kac, kbo, kao = map(int, f[1:])
        pos += 1
        take = lambda c: lines[pos:pos + c]
        B = take(nb); pos += nb
        A = take(na); pos += na
        D = take(nd); pos += nd
        I = take(ni); pos += ni
        hunks.append(dict(B=B, A=A, D=D, I=I, kbc=kbc, kac=kac, kbo=kbo, kao=kao))
    return dict(target=target, hunks=hunks)


def patch_name(target):
    return target.replace("map/single/", "").replace("/", "__") + ".hdp"


# ------------------------------------------------------------------ data mod
MOD_INFO = """{mod
\t{name "Historical Doctrine (DLL)"}
\t{tags "Mission,Gamemode"}
\t{desc "Texts of the Historical Doctrine DLL edition. The missions are patched in memory by goh_mods\\\\historical_doctrine.dll, so it works together with other mods that change the skirmish missions. Don't enable it together with the Workshop version."}
\t{minGameVersion "any"}
\t{maxGameVersion "any"}
}
"""


def main(game, src, out):
    read_vanilla = vanilla_reader(game)
    pdir = os.path.join(out, "binaries", "x64", "goh_mods", "historical_doctrine")
    mdir = os.path.join(out, "data", "historical_doctrine")
    for d in (pdir, mdir):
        if os.path.isdir(d):
            shutil.rmtree(d)
        os.makedirs(d)
    res = os.path.join(src, "resource")
    files = sorted(glob.glob(os.path.join(res, "map", "single", "**", "*.*"), recursive=True))
    total = 0
    for f in files:
        target = os.path.relpath(f, res).replace(os.sep, "/").lower()
        van = read_vanilla(target)
        if van is None:
            raise SystemExit("not in the game's packs: " + target)
        new = open(f, encoding="utf-8").read()
        hunks = build_hunks(van, new)
        path = os.path.join(pdir, patch_name(target))
        write_patch(path, target, hunks)
        patch = read_patch(path)
        got, why = apply(van, patch)
        if got is None or norm_lines(got) != norm_lines(new):
            raise SystemExit("self-test failed for %s: %s" % (target, why or "different output"))
        total += os.path.getsize(path)
        print("%-62s %3d hunks %8d bytes" % (target, len(hunks), os.path.getsize(path)))
    loc = os.path.join(src, "localizations")
    if os.path.isdir(loc):
        shutil.copytree(loc, os.path.join(mdir, "localizations"))
    with open(os.path.join(mdir, "mod.info"), "w", encoding="utf-8", newline="\r\n") as f:
        f.write(MOD_INFO)
    print("\n%d patches, %.1f MB -> %s\ndata mod -> %s" % (len(files), total / 1e6, pdir, mdir))


if __name__ == "__main__":
    repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
    main(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else os.path.join(repo, "build"))
