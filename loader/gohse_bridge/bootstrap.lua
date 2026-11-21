-- GoH DLL mods bridge for GOHSE (Gates of Hell Script Extender, https://github.com/nature2468/gohse)
-- =================================================================================================
--
-- GOHSE and the goh-coop-mods loader are both a version.dll in binaries\x64, so only one of them can be
-- installed.  With GOHSE's version.dll in place, this script does the loader's job: it loads every
-- binaries\x64\goh_mods\*.dll (alphabetical order) into the game and into the dedicated server.
--
-- Install as <game>\gohse\mods\goh_dll_mods\resource\script\gohse\ (mod.json + bootstrap.lua).  GOHSE runs
-- the scripts of that folder at every start, right after the game has applied its mods; nothing to enable.
-- Without GOHSE the folder does nothing.
--
-- The DLLs patch the game by themselves and stay loaded until the game exits: disabling this script in a
-- running game does not unload them.  A DLL that is already loaded (the script runs again every time the
-- mods are applied) is left alone.
--
-- Log: GOHSE's console and <game>\gohse\logs, lines marked [nicko996.goh-dll-mods].  Each DLL also writes its
-- own %TEMP%\goh_<mod>_<exe>.log.

local ffi = require("ffi")
local bit = require("bit")

ffi.cdef [[
typedef struct { uint32_t low, high; } GOHB_FILETIME;
typedef struct {
  uint32_t attributes;
  GOHB_FILETIME created, accessed, written;
  uint32_t size_high, size_low, reserved0, reserved1;
  wchar_t name[260];
  wchar_t short_name[14];
} GOHB_FIND_DATAW;
uint32_t GetModuleFileNameW(void *module, wchar_t *path, uint32_t size);
void *FindFirstFileW(const wchar_t *pattern, GOHB_FIND_DATAW *data);
int FindNextFileW(void *find, GOHB_FIND_DATAW *data);
int FindClose(void *find);
void *LoadLibraryW(const wchar_t *path);
void *GetModuleHandleW(const wchar_t *path);
void *GetProcAddress(void *module, const char *name);
uint32_t GetLastError(void);
]]

local kernel32 = ffi.load("kernel32")
local MAX = 32768
local READY_TIMEOUT_MS = 5000
local INVALID_HANDLE = ffi.cast("void *", ffi.cast("intptr_t", -1))

-- wchar_t string -> Lua string, for the log only (non-ASCII characters become '?')
local function text(w, len)
  local t = {}
  for i = 0, len - 1 do
    local c = w[i]
    t[#t + 1] = string.char(c < 128 and c or 63)
  end
  return table.concat(t)
end

local function wlen(w)
  local n = 0
  while w[n] ~= 0 do n = n + 1 end
  return n
end

-- <folder of the game exe>\goh_mods\ as wchar_t[MAX] plus its length
local function mods_dir()
  local dir = ffi.new("wchar_t[?]", MAX)
  local n = kernel32.GetModuleFileNameW(nil, dir, MAX)
  if n == 0 or n >= MAX then error("cannot read the game exe path") end
  local len = 0
  for i = n - 1, 0, -1 do
    if dir[i] == 92 then len = i + 1 break end      -- '\'
  end
  if len == 0 then error("unexpected game exe path") end
  local tail = "goh_mods\\"
  if len + #tail + 300 >= MAX then error("game path too long") end
  for i = 1, #tail do dir[len + i - 1] = tail:byte(i) end
  len = len + #tail
  dir[len] = 0
  return dir, len
end

-- dir .. name, where name is a Lua (ASCII) string or a zero-terminated wchar_t string
local function join(dir, dir_len, name)
  local path = ffi.new("wchar_t[?]", MAX)
  ffi.copy(path, dir, dir_len * 2)
  local n = dir_len
  if type(name) == "string" then
    for i = 1, #name do path[n] = name:byte(i) n = n + 1 end
  else
    local len = wlen(name)
    if n + len + 1 >= MAX then error("path too long") end
    ffi.copy(path + n, name, len * 2)
    n = n + len
  end
  path[n] = 0
  return path
end

local function load_mods()
  local dir, dir_len = mods_dir()
  local data = ffi.new("GOHB_FIND_DATAW")
  local find = kernel32.FindFirstFileW(join(dir, dir_len, "*.dll"), data)
  if find == INVALID_HANDLE then
    Ext.Log("no DLL mods: " .. text(dir, dir_len) .. " is empty or missing")
    return
  end
  local mods = {}
  repeat
    if bit.band(data.attributes, 0x10) == 0 then      -- not a folder
      local len = wlen(data.name)
      local name = ffi.new("wchar_t[?]", len + 1)
      ffi.copy(name, data.name, len * 2)
      name[len] = 0
      mods[#mods + 1] = { name = name, label = text(name, len) }
    end
  until kernel32.FindNextFileW(find, data) == 0
  kernel32.FindClose(find)
  table.sort(mods, function(a, b) return a.label:lower() < b.label:lower() end)

  local loaded, present, failed = 0, 0, 0
  local started = {}
  for _, mod in ipairs(mods) do
    local path = join(dir, dir_len, mod.name)
    if kernel32.GetModuleHandleW(path) ~= nil then
      present = present + 1
    else
      local module = kernel32.LoadLibraryW(path)
      if module == nil then
        failed = failed + 1
        Ext.Log("ERROR: cannot load " .. mod.label .. " (error " .. tonumber(kernel32.GetLastError()) .. ")")
      else
        loaded = loaded + 1
        mod.module = module
        started[#started + 1] = mod
        Ext.Log("loaded " .. mod.label)
      end
    end
  end
  -- The DLLs install their patches on their own threads.  Wait for them here, so the game (which goes on
  -- reading files as soon as this script returns) finds the hooks in place.  Older DLLs have no goh_mod_wait.
  for _, mod in ipairs(started) do
    local wait = kernel32.GetProcAddress(mod.module, "goh_mod_wait")
    if wait ~= nil and ffi.cast("int (*)(unsigned int)", wait)(READY_TIMEOUT_MS) == 0 then
      Ext.Log("WARNING: " .. mod.label .. " is still starting after " .. READY_TIMEOUT_MS .. " ms")
    end
  end
  Ext.Log(string.format("%d DLL mods in %s: %d loaded now, %d already loaded, %d failed",
    #mods, text(dir, dir_len), loaded, present, failed))
end

load_mods()

return {
  OnUnload = function()
    Ext.Log("the DLL mods stay loaded until the game exits")
  end,
}
