/*
 * GoH mod loader - version.dll proxy for Call to Arms: Gates of Hell (x64)
 * =========================================================================
 *
 * Install as <game>\binaries\x64\version.dll.  The game (call_to_arms.exe) and its dedicated server
 * (call_to_arms_server.exe) import version.dll, so Windows loads this file into both.
 *
 *  1) Every export of the real %SystemRoot%\System32\version.dll is forwarded unchanged.
 *  2) In call_to_arms.exe / call_to_arms_server.exe only, every *.dll in the "goh_mods" folder next to
 *     this file is loaded (alphabetical order).  Each mod DLL patches the game by itself; removing a mod
 *     DLL uninstalls that mod.  Other programs that load this version.dll are left alone.
 *
 * The mods are loaded from DllMain, i.e. on the game's main thread before the game code runs, which is
 * what the mods expect (they record the main thread id there).
 *
 * Log: %TEMP%\goh_mod_loader_<exe name>.log
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

#define GOH_LOADER_VERSION "1.0.1"   /* the loader's own version (release tag loader-v<version>) */

/* ------------------------------------------------------------------ version.dll forwarding */
#define VERSION_EXPORTS(X) \
  X(GetFileVersionInfoA) X(GetFileVersionInfoByHandle) X(GetFileVersionInfoExA) \
  X(GetFileVersionInfoExW) X(GetFileVersionInfoSizeA) X(GetFileVersionInfoSizeExA) \
  X(GetFileVersionInfoSizeExW) X(GetFileVersionInfoSizeW) X(GetFileVersionInfoW) \
  X(VerFindFileA) X(VerFindFileW) X(VerInstallFileA) X(VerInstallFileW) \
  X(VerLanguageNameA) X(VerLanguageNameW) X(VerQueryValueA) X(VerQueryValueW)

#define DECLARE_PTR(n) void *p_##n = NULL;
VERSION_EXPORTS(DECLARE_PTR)

#define STUB(n) __asm__(".globl " #n "\n" #n ":\n\tjmp *p_" #n "(%rip)\n" \
                        ".section .drectve\n\t.ascii \" -export:" #n "\"\n.text\n");
VERSION_EXPORTS(STUB)

static void load_real_version(void)
{
  wchar_t path[MAX_PATH];
  UINT n = GetSystemDirectoryW(path, MAX_PATH);
  if (n == 0 || n > MAX_PATH - 20) return;
  wcscat(path, L"\\version.dll");
  HMODULE real = LoadLibraryW(path);
  if (real == NULL) return;
#define RESOLVE(x) p_##x = (void *) GetProcAddress(real, #x);
  VERSION_EXPORTS(RESOLVE)
}

/* ------------------------------------------------------------------ mod loading */
static FILE *g_log = NULL;

static int is_game_process(wchar_t *exe_name, size_t cap)
{
  wchar_t path[MAX_PATH];
  DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return 0;
  wchar_t *base = wcsrchr(path, L'\\');
  base = base ? base + 1 : path;
  wcsncpy(exe_name, base, cap - 1);
  exe_name[cap - 1] = 0;
  return _wcsicmp(base, L"call_to_arms.exe") == 0 || _wcsicmp(base, L"call_to_arms_server.exe") == 0;
}

static int cmp_names(const void *a, const void *b)
{
  return _wcsicmp((const wchar_t *) a, (const wchar_t *) b);
}

static void load_mods(HINSTANCE self)
{
  wchar_t dir[MAX_PATH];
  DWORD n = GetModuleFileNameW(self, dir, MAX_PATH);
  if (n == 0 || n >= MAX_PATH - 32) return;
  wchar_t *slash = wcsrchr(dir, L'\\');
  if (slash == NULL) return;
  wcscpy(slash + 1, L"goh_mods\\");
  size_t dir_len = wcslen(dir);

  wchar_t pattern[MAX_PATH];
  _snwprintf(pattern, MAX_PATH, L"%ls*.dll", dir);
  pattern[MAX_PATH - 1] = 0;
  static wchar_t names[64][MAX_PATH];
  int count = 0;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(pattern, &fd);
  if (h == INVALID_HANDLE_VALUE) {
    if (g_log) fprintf(g_log, "no mods: %ls is empty or missing\n", dir);
    return;
  }
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    if (count < 64 && dir_len + wcslen(fd.cFileName) < MAX_PATH) wcscpy(names[count++], fd.cFileName);
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  qsort(names, count, sizeof names[0], cmp_names);

  for (int i = 0; i < count; i++) {
    wchar_t path[MAX_PATH];
    wcscpy(path, dir);
    wcscat(path, names[i]);
    HMODULE m = LoadLibraryW(path);
    if (g_log) {
      if (m) fprintf(g_log, "loaded %ls\n", names[i]);
      else fprintf(g_log, "ERROR: cannot load %ls (error %lu)\n", names[i], (unsigned long) GetLastError());
      fflush(g_log);
    }
  }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
  (void) reserved;
  if (reason != DLL_PROCESS_ATTACH) return TRUE;
  DisableThreadLibraryCalls(inst);
  load_real_version();
  wchar_t exe[MAX_PATH];
  if (!is_game_process(exe, MAX_PATH)) return TRUE;
  wchar_t logpath[MAX_PATH + 64];
  DWORD n = GetTempPathW(MAX_PATH, logpath);
  if (n > 0 && n < MAX_PATH) {
    _snwprintf(logpath + n, 63, L"goh_mod_loader_%.40ls.log", exe);
    logpath[MAX_PATH + 63] = 0;
    g_log = _wfopen(logpath, L"w");
  }
  if (g_log) { fprintf(g_log, "GoH mod loader %s in %ls\n", GOH_LOADER_VERSION, exe); fflush(g_log); }
  load_mods(inst);
  if (g_log) { fclose(g_log); g_log = NULL; }
  return TRUE;
}
