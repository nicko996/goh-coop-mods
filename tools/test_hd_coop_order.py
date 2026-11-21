# -*- coding: utf-8 -*-
"""Historical Doctrine + Coop Checkpoint in both hook orders give the same mission text?

    python tools/test_hd_coop_order.py "<game folder>" [<patch folder>]

The two mods chain on the same SDL-reader hook (common/goh_sdl_hook.h); which one runs first depends on which
installed last.  Every coop peer must end up with the same text, so the result must not depend on the order.
For every Historical Doctrine mission: hd(coop(vanilla)) == coop(hd(vanilla)), both with the reference
implementations (make_hd_data.apply, make_coop_checkpoint_mod.patch).
"""
import glob
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(REPO, "mods", "historical-doctrine", "data"))
sys.path.insert(0, os.path.join(REPO, "mods", "coop-checkpoint", "data"))
import make_hd_data as H  # noqa: E402
import make_coop_checkpoint_mod as C  # noqa: E402


def coop(text):
    return C.patch(text.encode("utf-8")).decode("utf-8")


def main():
    game = sys.argv[1]
    pdir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(REPO, "build", "binaries", "x64", "goh_mods",
                                                                "historical_doctrine")
    read = H.vanilla_reader(game)
    fails = 0
    for pf in sorted(glob.glob(os.path.join(pdir, "*.hdp"))):
        patch = H.read_patch(pf)
        if not patch["target"].endswith(".mi"):
            continue
        van = read(patch["target"])
        hd_first, why1 = H.apply(van, patch)
        a = coop(hd_first) if hd_first is not None else None
        c_first = coop(van)
        b, why2 = H.apply(c_first, patch)
        if a is None or b is None:
            status = "FAIL hd on vanilla: %s / hd on coop: %s" % (why1, why2)
        elif a == b:
            status = "same"
        else:
            al, bl = H.norm_lines(a), H.norm_lines(b)
            k = next((i for i, (x, y) in enumerate(zip(al, bl)) if x != y), min(len(al), len(bl)))
            status = "DIFFERENT from line %d: %r / %r" % (k, al[k][:60] if k < len(al) else None,
                                                          bl[k][:60] if k < len(bl) else None)
        if status != "same":
            fails += 1
        print("%-60s %s" % (patch["target"], status))
    print("\n%s" % ("OK" if fails == 0 else "%d FAILURES" % fails))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
