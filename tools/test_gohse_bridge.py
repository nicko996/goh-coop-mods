"""
Runs the GOHSE bridge script (loader/gohse_bridge/bootstrap.lua) in a real LuaJIT, outside the game.

Usage
    python tools/test_gohse_bridge.py <lua51.dll of a 64-bit LuaJIT>

Any 64-bit LuaJIT build works (GOHSE embeds LuaJIT 2.1); several programs ship one as lua51.dll.  The script is
run twice, each time in a new Lua state as GOHSE does when the mods are applied again, with a stub of GOHSE's
Ext table, against a fake game folder (with a non-ASCII name) that holds
 - the mod DLLs of build/binaries/x64/goh_mods (run build.ps1 first; outside the game they do nothing),
 - a DLL whose goh_mod_wait answers "ready" after 300 ms and one that answers "not ready" (built here, needs zig),
 - a file that is not a DLL, and a folder.
It checks the load order, the error and warning lines and that the second run loads nothing again.

The game exe path is faked by wrapping ffi.load, so the script under test is the unmodified file.
"""
import ctypes
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(ROOT, "loader", "gohse_bridge", "bootstrap.lua")
MODS = os.path.join(ROOT, "build", "binaries", "x64", "goh_mods")

WAIT_DLL = r"""
#include <windows.h>
__declspec(dllexport) int goh_mod_wait(unsigned ms)
{
  Sleep(SLOW_MS < ms ? SLOW_MS : ms);
  return READY;
}
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
  (void) inst; (void) reason; (void) reserved;
  return TRUE;
}
"""

PREAMBLE = r"""
local ffi = require("ffi")
local log_path = [==[%s]==]
local fake_exe = { %s }
Ext = { ApiVersion = 1, Log = function(...)
  local parts = {}
  for i = 1, select("#", ...) do parts[#parts + 1] = tostring((select(i, ...))) end
  local f = assert(io.open(log_path, "a"))
  f:write(table.concat(parts, " "), "\n")
  f:close()
end }
local real_load = ffi.load
ffi.load = function(name)
  local lib = real_load(name)
  return setmetatable({
    GetModuleFileNameW = function(module, buf, size)
      assert(module == nil)
      for i = 1, #fake_exe do buf[i - 1] = fake_exe[i] end
      buf[#fake_exe] = 0
      return #fake_exe
    end }, { __index = function(_, k) return lib[k] end })
end
"""


def build_wait_dll(work, out, slow_ms, ready):
    src = os.path.join(work, "wait_dll.c")
    with open(src, "w") as f:
        f.write(WAIT_DLL)
    subprocess.run(["zig", "cc", "-target", "x86_64-windows-gnu", "-shared", "-O2", "-s", "-DSLOW_MS=%d" % slow_ms,
                    "-DREADY=%d" % ready, "-o", out, src], check=True)
    lib = os.path.splitext(out)[0] + ".lib"
    if os.path.exists(lib):
        os.remove(lib)


def run(lua, log_path, fake_exe):
    units = memoryview(fake_exe.encode("utf-16-le")).cast("H")
    preamble = PREAMBLE % (log_path, ", ".join(str(u) for u in units))
    state = lua.luaL_newstate()
    lua.luaL_openlibs(state)
    if lua.luaL_loadstring(state, preamble.encode()) or lua.lua_pcall(state, 0, 0, 0):
        sys.exit("FAILED: preamble: %s" % lua.lua_tolstring(state, -1, None).decode())
    with open(SCRIPT, "rb") as f:
        code = f.read()
    if lua.luaL_loadbuffer(state, code, len(code), b"@bootstrap.lua"):
        sys.exit("FAILED: syntax: %s" % lua.lua_tolstring(state, -1, None).decode())
    if lua.lua_pcall(state, 0, 1, 0):
        sys.exit("FAILED: runtime: %s" % lua.lua_tolstring(state, -1, None).decode(errors="replace"))
    if lua.lua_type(state, -1) != 5:
        sys.exit("FAILED: the entry must return its lifecycle table")
    lua.lua_getfield(state, -1, b"OnUnload")
    if lua.lua_pcall(state, 0, 0, 0):
        sys.exit("FAILED: OnUnload: %s" % lua.lua_tolstring(state, -1, None).decode())
    lua.lua_close(state)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    dlls = sorted(n for n in os.listdir(MODS) if n.lower().endswith(".dll")) if os.path.isdir(MODS) else []
    if not dlls:
        sys.exit("no DLLs in %s: run build.ps1 first" % MODS)
    lua = ctypes.CDLL(sys.argv[1])
    if not hasattr(lua, "luaJIT_setmode"):
        sys.exit("%s is not LuaJIT" % sys.argv[1])
    lua.luaL_newstate.restype = ctypes.c_void_p
    lua.luaL_openlibs.argtypes = [ctypes.c_void_p]
    lua.luaL_loadstring.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    lua.luaL_loadbuffer.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p]
    lua.lua_pcall.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int]
    lua.lua_tolstring.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p]
    lua.lua_tolstring.restype = ctypes.c_char_p
    lua.lua_type.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lua.lua_getfield.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p]
    lua.lua_close.argtypes = [ctypes.c_void_p]

    # not removed at the end: the DLLs stay loaded in this process
    work = tempfile.mkdtemp(prefix="goh_bridge_test_")
    fake_bin = os.path.join(work, "Gàme (x86)", "binaries", "x64")
    fake_mods = os.path.join(fake_bin, "goh_mods")
    os.makedirs(os.path.join(fake_mods, "historical_doctrine"))
    for name in dlls:
        shutil.copy(os.path.join(MODS, name), fake_mods)
    build_wait_dll(work, os.path.join(fake_mods, "Zeta_slow.dll"), 300, 1)
    build_wait_dll(work, os.path.join(fake_mods, "b_notready.dll"), 0, 0)
    with open(os.path.join(fake_mods, "broken.dll"), "w") as f:
        f.write("not a dll")
    log_path = os.path.join(work, "bridge.log")
    fake_exe = os.path.join(fake_bin, "call_to_arms.exe")

    expected = sorted(dlls + ["Zeta_slow.dll", "b_notready.dll", "broken.dll"], key=str.lower)
    failures = []

    def check(ok, what):
        print(("ok      " if ok else "FAILED  ") + what)
        if not ok:
            failures.append(what)

    run(lua, log_path, fake_exe)
    with open(log_path) as f:
        first = f.read().splitlines()
    order = [line.split()[-3 if line.startswith("ERROR") else -1] for line in first
             if line.startswith("loaded ") or line.startswith("ERROR: cannot load ")]
    check(order == expected, "every DLL tried once, in the loader's order: %s" % ", ".join(order))
    check("ERROR: cannot load broken.dll (error 193)" in first, "a file that is not a DLL is reported")
    check(sum(line.startswith("WARNING") for line in first) == 1 and
          any(line.startswith("WARNING: b_notready.dll") for line in first), "only the DLL that is not ready is reported")
    check(any("%d DLL mods" % len(expected) in line and "%d loaded now, 0 already loaded, 1 failed" %
              (len(expected) - 1) in line for line in first), "summary of the first run")

    os.remove(log_path)
    run(lua, log_path, fake_exe)
    with open(log_path) as f:
        second = f.read().splitlines()
    check(not any(line.startswith("loaded ") for line in second), "second run: nothing loaded again")
    check(any("0 loaded now, %d already loaded, 1 failed" % (len(expected) - 1) in line for line in second),
          "summary of the second run")
    print("\n".join(["", "log of the first run:"] + ["  " + line for line in first]))
    if failures:
        sys.exit("%d checks FAILED" % len(failures))
    print("all checks passed")


if __name__ == "__main__":
    main()
