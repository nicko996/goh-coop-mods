/*
 * goh_common.h - shared helpers for the Gates of Hell mod DLLs (x64, game v1.067)
 * ================================================================================
 *
 * Every mod is a small DLL loaded by the shared version.dll loader (loader/version_proxy.c) into
 * call_to_arms.exe (the game) and call_to_arms_server.exe (the dedicated server the host's game starts
 * for a multiplayer match).  This header is included by each mod: all functions are static, so every
 * mod has its own copy and its own log.
 *
 * A mod defines
 *     #define GOH_MOD_NAME "loot_highlight"
 *     static void mod_start(int kind);      // kind: GOH_GAME or GOH_SERVER, runs on a worker thread
 * and then writes GOH_MOD_ENTRY() once.
 *
 * Everything is found by byte signatures (W = wildcard).  If a signature is not found (game updated),
 * the part that needs it is skipped and logged; the game is never patched blindly.
 */
#ifndef GOH_COMMON_H
#define GOH_COMMON_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>

#include "goh_version.h"

#define GOH_UNUSED __attribute__((unused))
#define W (-1)
#define COUNT(a) ((int) (sizeof(a) / sizeof((a)[0])))

enum { GOH_OTHER = 0, GOH_GAME = 1, GOH_SERVER = 2 };

/* ------------------------------------------------------------------ log */
static FILE *g_log = NULL;
static DWORD g_main_tid;      /* the game's main thread (the loader runs the mods' DllMain on it) */

static GOH_UNUSED void logf_(const char *fmt, ...)
{
  if (g_log == NULL) return;
  SYSTEMTIME t;
  GetLocalTime(&t);
  fprintf(g_log, "[%02u:%02u:%02u] ", t.wHour, t.wMinute, t.wSecond);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(g_log, fmt, ap);
  va_end(ap);
  fputc('\n', g_log);
  fflush(g_log);
}

/* ------------------------------------------------------------------ module helpers */
static BYTE *g_base;
static SIZE_T g_image_size;
static BYTE *g_text;
static SIZE_T g_text_size;

static GOH_UNUSED int find_text(void)
{
  IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *) g_base;
  IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *) (g_base + dos->e_lfanew);
  g_image_size = nt->OptionalHeader.SizeOfImage;
  IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
  for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
    if (memcmp(s->Name, ".text", 5) == 0) {
      g_text = g_base + s->VirtualAddress;
      g_text_size = s->Misc.VirtualSize;
      return 1;
    }
  }
  return 0;
}

/* pattern: bytes, -1 = wildcard; returns the number of matches in [from, from + size), the first one in *out */
static GOH_UNUSED int scan_in(BYTE *from, SIZE_T size, const int *pat, int len, BYTE **out)
{
  int count = 0;
  for (SIZE_T i = 0; i + len <= size; i++) {
    int k = 0;
    while (k < len && (pat[k] < 0 || from[i + k] == (BYTE) pat[k])) k++;
    if (k == len) {
      if (count == 0) *out = from + i;
      count++;
    }
  }
  return count;
}

/* same, over the whole code section of the game module */
static GOH_UNUSED int scan(const int *pat, int len, BYTE **out)
{
  return scan_in(g_text, g_text_size, pat, len, out);
}

static GOH_UNUSED int write_code(BYTE *at, const BYTE *bytes, SIZE_T n)
{
  DWORD old;
  if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
  memcpy(at, bytes, n);
  VirtualProtect(at, n, old, &old);
  FlushInstructionCache(GetCurrentProcess(), at, n);
  return 1;
}

static GOH_UNUSED int safe_read(const void *addr, void *buf, SIZE_T n)
{
  SIZE_T got = 0;
  return ReadProcessMemory(GetCurrentProcess(), addr, buf, n, &got) && got == n;
}

/* executable memory within +-2 GB of the main module, for rel32 jumps */
static GOH_UNUSED BYTE *alloc_near_module(SIZE_T size)
{
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  ULONG_PTR gran = si.dwAllocationGranularity;
  ULONG_PTR lo = (ULONG_PTR) g_base > 0x70000000ULL ? (ULONG_PTR) g_base - 0x70000000ULL : gran;
  ULONG_PTR hi = (ULONG_PTR) g_base + g_image_size + 0x70000000ULL;
  for (ULONG_PTR a = ((ULONG_PTR) g_base + g_image_size + gran) & ~(gran - 1); a < hi; a += gran) {
    void *p = VirtualAlloc((void *) a, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (p) return (BYTE *) p;
  }
  for (ULONG_PTR a = ((ULONG_PTR) g_base - gran) & ~(gran - 1); a > lo; a -= gran) {
    void *p = VirtualAlloc((void *) a, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (p) return (BYTE *) p;
  }
  return NULL;
}

static GOH_UNUSED INT32 rel32(BYTE *from_next, BYTE *to)
{
  return (INT32) ((LONG_PTR) to - (LONG_PTR) from_next);
}

static GOH_UNUSED BYTE *call_target(BYTE *call) { return call + 5 + *(INT32 *) (call + 1); }

/* jmp [rip+0]; dq to   (14 bytes) */
static GOH_UNUSED BYTE *abs_jmp(BYTE *p, void *to)
{
  p[0] = 0xFF; p[1] = 0x25; *(INT32 *) (p + 2) = 0; *(void **) (p + 6) = to;
  return p + 14;
}

/* a rel32 call is redirected to `to` (through a near stub); *orig = the old target */
static GOH_UNUSED int redirect_call(BYTE *call, void *to, void **orig)
{
  if (call[0] != 0xE8) return 0;
  BYTE *stub = alloc_near_module(0x20);
  if (stub == NULL) return 0;
  abs_jmp(stub, to);
  FlushInstructionCache(GetCurrentProcess(), stub, 0x20);
  *orig = call_target(call);
  BYTE c[5] = { 0xE8, 0, 0, 0, 0 };
  *(INT32 *) (c + 1) = rel32(call + 5, stub);
  return write_code(call, c, 5);
}

static GOH_UNUSED int replace_slot(void **vt, int slot, void *fn, void **orig)
{
  *orig = vt[slot];
  return write_code((BYTE *) &vt[slot], (const BYTE *) &fn, sizeof fn);
}

static const BYTE NOP6[6] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };

/* the 6-byte jne (0F 85) at sig + off becomes NOPs */
static GOH_UNUSED int nop_jne(const int *sig, int len, int off, const char *what)
{
  BYTE *at = NULL;
  int n = scan(sig, len, &at);
  if (n != 1) { logf_("ERROR: %s signature matches %d (expected 1): skipped", what, n); return 0; }
  BYTE *jne = at + off;
  if (jne[0] != 0x0F || jne[1] != 0x85) { logf_("ERROR: %s: unexpected bytes, skipped", what); return 0; }
  if (!write_code(jne, NOP6, 6)) { logf_("ERROR: %s: cannot patch", what); return 0; }
  logf_("%s unlocked at +0x%llx", what, (unsigned long long) (jne - g_base));
  return 1;
}

/* Generic entry hook: the first n bytes of fn (whole instructions, no RIP-relative operands, n >= 5)
 * are replaced by a jmp to a stub that calls handler(rcx, rdx).  If the handler returns 0 the original
 * runs (relocated bytes + jmp back); otherwise the stub returns.  *orig receives a callable trampoline
 * to the original function. */
typedef int (*EntryHandler)(void *rcx, void *rdx);
static GOH_UNUSED int install_entry_hook(BYTE *fn, int n, EntryHandler handler, void **orig, const char *what)
{
  BYTE *page = alloc_near_module(0x1000);
  if (page == NULL) { logf_("ERROR: %s: no memory near the module", what); return 0; }
  BYTE *cell = page + 0x80;
  *(void **) cell = (void *) handler;
  BYTE *p = page;
  static const BYTE save_regs[] = { 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x48, 0x83, 0xEC, 0x28 };
  memcpy(p, save_regs, sizeof save_regs); p += sizeof save_regs;     /* push rcx,rdx,r8,r9; sub rsp,28h */
  p[0] = 0xFF; p[1] = 0x15; *(INT32 *) (p + 2) = rel32(p + 6, cell); p += 6;   /* call [rip+cell] */
  static const BYTE restore_regs[] = { 0x48, 0x83, 0xC4, 0x28, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59,
                                       0x85, 0xC0, 0x74, 0x01, 0xC3 };   /* ...; test eax,eax; jz +1; ret */
  memcpy(p, restore_regs, sizeof restore_regs); p += sizeof restore_regs;
  BYTE *tramp = p;
  memcpy(p, fn, n); p += n;
  p[0] = 0xE9; *(INT32 *) (p + 1) = rel32(p + 5, fn + n); p += 5;
  FlushInstructionCache(GetCurrentProcess(), page, 0x80);
  BYTE jmp[5] = { 0xE9, 0, 0, 0, 0 };
  *(INT32 *) (jmp + 1) = rel32(fn + 5, page);
  if (!write_code(fn, jmp, 5)) { logf_("ERROR: %s: cannot patch", what); return 0; }
  if (orig) *orig = tramp;
  logf_("%s hooked at +0x%llx", what, (unsigned long long) (fn - g_base));
  return 1;
}

/* vtable of an MSVC RTTI class name (".?AVeGameControl@@"), searched in the module image */
static GOH_UNUSED void **find_vtable(const char *rtti)
{
  size_t len = strlen(rtti);
  BYTE *td_name = NULL;
  for (SIZE_T i = 0; i + len < g_image_size; i++)
    if (g_base[i] == '.' && memcmp(g_base + i, rtti, len + 1) == 0) { td_name = g_base + i; break; }
  if (td_name == NULL) return NULL;
  DWORD td_rva = (DWORD) (td_name - 16 - g_base);
  for (SIZE_T i = 0; i + 24 < g_image_size; i += 4) {
    DWORD *col = (DWORD *) (g_base + i);
    if (col[3] != td_rva || col[0] != 1 || col[1] != 0) continue;
    ULONGLONG col_va = (ULONGLONG) (ULONG_PTR) col;
    for (SIZE_T j = 0; j + 16 < g_image_size; j += 8)
      if (*(ULONGLONG *) (g_base + j) == col_va) return (void **) (g_base + j + 8);
  }
  return NULL;
}

/* MSVC RTTI class name of an object (for logs) */
static GOH_UNUSED const char *rtti_name_of(void *obj, char *buf, size_t cap)
{
  BYTE *vt = NULL, *col = NULL;
  DWORD td = 0;
  buf[0] = 0;
  if (!safe_read(obj, &vt, sizeof vt) || !safe_read(vt - 8, &col, sizeof col) ||
      !safe_read(col + 12, &td, sizeof td) || !safe_read(g_base + td + 16, buf, cap - 1)) return "?";
  buf[cap - 1] = 0;
  return buf;
}

/* import-table slot of a KERNEL32 function in the game module */
static GOH_UNUSED ULONGLONG *find_import_slot(const char *name)
{
  IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *) g_base;
  IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *) (g_base + dos->e_lfanew);
  IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (dir.VirtualAddress == 0) return NULL;
  for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *) (g_base + dir.VirtualAddress); d->Name != 0; d++) {
    if (_stricmp((const char *) (g_base + d->Name), "KERNEL32.dll") != 0) continue;
    if (d->OriginalFirstThunk == 0) continue;
    ULONGLONG *names = (ULONGLONG *) (g_base + d->OriginalFirstThunk);
    ULONGLONG *slots = (ULONGLONG *) (g_base + d->FirstThunk);
    for (int i = 0; names[i] != 0; i++) {
      if (names[i] & IMAGE_ORDINAL_FLAG64) continue;
      IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *) (g_base + (names[i] & 0x7fffffff));
      if (strcmp((const char *) ibn->Name, name) == 0) return &slots[i];
    }
  }
  return NULL;
}

/* the import slot already points into a loaded DLL (the Windows loader has filled it) */
static GOH_UNUSED int is_resolved(ULONGLONG value)
{
  HMODULE m = NULL;
  return value != 0 &&
         GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR) (ULONG_PTR) value, &m) && m != NULL && (BYTE *) m != g_base;
}

static GOH_UNUSED int set_slot(ULONGLONG *slot, void *fn)
{
  DWORD old;
  if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old)) return 0;
  InterlockedExchange64((LONG64 volatile *) slot, (LONG64) (ULONG_PTR) fn);
  VirtualProtect(slot, sizeof(*slot), old, &old);
  return 1;
}

typedef struct {          /* MSVC std::string */
  union { char buf[16]; char *ptr; } u;
  unsigned long long size, cap;
} MsvcString;

static GOH_UNUSED const char *msvc_cstr(const MsvcString *s, char *buf, size_t cap)
{
  MsvcString copy;
  buf[0] = 0;
  if (!safe_read(s, &copy, sizeof copy) || copy.size == 0 || copy.size >= cap) return buf;
  const char *src = copy.cap > 15 ? copy.u.ptr : s->u.buf;
  if (!safe_read(src, buf, copy.size)) { buf[0] = 0; return buf; }
  buf[copy.size] = 0;
  return buf;
}

/* ------------------------------------------------------------------ game speed globals */
/* main loop: cmp edx,100; cmovg edx,eax; jmp +6; mov edx,[rip+FIXED]; lea rcx,[rip+QC]; call */
static const int SIG_FIXED_BRANCH[] = {
  0x83,0xFA,0x64,0x0F,0x4F,0xD0,0xEB,0x06,0x8B,0x15,W,W,W,W,0x48,0x8D,0x0D,W,W,W,W,0xE8 };
static BYTE *g_fixed_var;       /* != 0 in network games (fixed 20 ms step) */
static BYTE *g_quant_control;   /* eQuantControl; +0x20/+0x28 = vector of speed requests, +0x48 = quant clock */

static GOH_UNUSED int find_speed_globals(void)
{
  if (g_quant_control != NULL) return 1;
  BYTE *br = NULL;
  int n = scan(SIG_FIXED_BRANCH, COUNT(SIG_FIXED_BRANCH), &br);
  if (n != 1) { logf_("ERROR: fixed-step signature matches %d (expected 1)", n); return 0; }
  g_fixed_var = br + 14 + *(INT32 *) (br + 10);
  g_quant_control = br + 21 + *(INT32 *) (br + 17);
  return 1;
}

static GOH_UNUSED int in_network_game(void)
{
  INT32 fixed = 0;
  return g_fixed_var != NULL && safe_read(g_fixed_var, &fixed, sizeof fixed) && fixed != 0;
}

static GOH_UNUSED INT32 quant_clock(void)
{
  INT32 clock = 0;
  if (g_quant_control) safe_read(g_quant_control + 0x48, &clock, sizeof clock);
  return clock;
}

/* ------------------------------------------------------------------ entry */
static int goh_process_kind(wchar_t *exe_name, size_t cap)
{
  wchar_t path[MAX_PATH];
  DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return GOH_OTHER;
  wchar_t *base = wcsrchr(path, L'\\');
  base = base ? base + 1 : path;
  wcsncpy(exe_name, base, cap - 1);
  exe_name[cap - 1] = 0;
  if (_wcsicmp(base, L"call_to_arms.exe") == 0) return GOH_GAME;
  if (_wcsicmp(base, L"call_to_arms_server.exe") == 0) return GOH_SERVER;
  return GOH_OTHER;
}

static void mod_start(int kind);

static DWORD WINAPI goh_worker(LPVOID kind)
{
  mod_start((int) (ULONG_PTR) kind);
  return 0;
}

/* DllMain: log %TEMP%\goh_<mod>_<exe>.log, then mod_start(kind) on a worker thread */
#define GOH_MOD_ENTRY()                                                                              \
  BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)                                  \
  {                                                                                                  \
    (void) reserved;                                                                                 \
    if (reason != DLL_PROCESS_ATTACH) return TRUE;                                                   \
    DisableThreadLibraryCalls(inst);                                                                 \
    g_main_tid = GetCurrentThreadId();                                                               \
    wchar_t exe[MAX_PATH];                                                                           \
    int kind = goh_process_kind(exe, MAX_PATH);                                                      \
    if (kind == GOH_OTHER) return TRUE;                                                              \
    g_base = (BYTE *) GetModuleHandleW(NULL);                                                        \
    if (!find_text()) return TRUE;                                                                   \
    wchar_t logpath[MAX_PATH + 96];                                                                  \
    DWORD n = GetTempPathW(MAX_PATH, logpath);                                                       \
    if (n > 0 && n < MAX_PATH) {                                                                     \
      _snwprintf(logpath + n, 95, L"goh_%hs_%.40ls.log", GOH_MOD_NAME, exe);                         \
      logpath[MAX_PATH + 95] = 0;                                                                    \
      g_log = _wfopen(logpath, L"w");                                                                \
    }                                                                                                \
    logf_("%s %s loaded into %ls, main thread %lu", GOH_MOD_NAME, GOH_MOD_VERSION, exe,              \
          (unsigned long) g_main_tid);                                                               \
    HANDLE th = CreateThread(NULL, 0, goh_worker, (LPVOID) (ULONG_PTR) kind, 0, NULL);               \
    if (th) CloseHandle(th);                                                                         \
    return TRUE;                                                                                     \
  }

#endif
