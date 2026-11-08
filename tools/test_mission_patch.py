"""Compare the DLL's in-memory mission patch (mods/coop-checkpoint/mission_patch.h) with patch() of the
data-mod generator on every mission of the game, plus the 0.mi of the mods given as extra arguments.
    python tools/test_mission_patch.py "<game folder>" [<mod folder> ...]
Needs zig on PATH."""
import importlib.util, os, subprocess, sys, tempfile, glob

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("gen", os.path.join(HERE, "..", "mods", "coop-checkpoint", "data",
                                                                  "make_coop_checkpoint_mod.py"))
gen = importlib.util.module_from_spec(spec); spec.loader.exec_module(gen)
game = sys.argv[1]
work = tempfile.mkdtemp(prefix="mptest_")
exe = os.path.join(work, "t.exe")
subprocess.check_call(["zig", "cc", "-O2", "-o", exe, os.path.join(HERE, "test_mission_patch.c")])
files = {}
for k, v in gen.load_missions(game).items():
    files["vanilla:" + k] = v
for mod in sys.argv[2:]:
    root = os.path.join(game, "mods", mod, "resource")
    for p in glob.glob(os.path.join(root, "map", "single", "**", "*.mi"), recursive=True):
        files[mod + ":" + os.path.relpath(p, root)] = open(p, "rb").read()
paths = []
for i, (k, v) in enumerate(sorted(files.items())):
    p = os.path.join(work, "%03d.mi" % i); open(p, "wb").write(v); paths.append((k, p, v))
for i in range(0, len(paths), 40):
    subprocess.check_call([exe] + [p for _, p, _ in paths[i:i + 40]])
ok = bad = 0
for k, p, v in paths:
    c = open(p + ".c_patched", "rb").read()
    try:
        py = gen.patch(v)
    except RuntimeError as e:
        py = b"NULL: " + str(e).encode()
    if c == py: ok += 1
    else:
        bad += 1; print("MISMATCH", k, len(c), len(py))
print("identical: %d, different: %d" % (ok, bad))
sys.exit(1 if bad else 0)
