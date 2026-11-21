/*
 * goh_sdl_hook.h - shared hook on the game's SDL text reader, for mods that change text files while they load
 * ===========================================================================================================
 *
 * Every SDL text file the game loads (.mi missions, .set, .def ...) goes through eSDLReader (game v1.067: the
 * same code in call_to_arms.exe and call_to_arms_server.exe).  Its source constructor reads the whole file into
 * the std::string at source+0x98 (assign(size + 1, 0), then the stream read) and then calls the lexer init; the
 * file path is the std::string at source+0xc0.  This header redirects that lexer-init call: the mod's callback
 * gets the path and the text and may replace the text before the game parses it.
 *
 * Several mods can hook it at the same time.  Each one installs under the same named mutex
 * (Local\goh_sdl_reader_hook_<pid>), reads where the call goes at that moment (the game's lexer init or the
 * previous mod's hook) and calls it after its own callback: the hooks form a chain, the last one installed runs
 * first.  Every mod of this repository that changes text files must use this header; a mod that hooks the same
 * call without the mutex can race with it at startup.
 *
 * Use (after goh_common.h):
 *     static void on_text(const char *path, MsvcString *text) { ... goh_sdl_get_text / goh_sdl_set_text ... }
 *     goh_sdl_hook(on_text);          // in mod_start
 */
#ifndef GOH_SDL_HOOK_H
#define GOH_SDL_HOOK_H

#ifndef GOH_COMMON_H
#error "include goh_common.h first"
#endif

typedef void (*GohSdlTextFn)(const char *path, MsvcString *text);
typedef unsigned long long (*GohLexInitFn)(void *source, void *arg);
typedef void *(*GohStrAssignFn)(void *str, unsigned long long n, char ch);

static GohSdlTextFn g_sdl_fn;
static GohLexInitFn g_sdl_next;        /* what the call went to before this mod: the lexer init or another hook */
static GohStrAssignFn g_sdl_assign;    /* the game's std::string::assign(n, ch), so the game frees with its allocator */
static BYTE *g_sdl_call;               /* the hooked call instruction */

/* the file text without the trailing NUL the reader keeps in the string */
static GOH_UNUSED void goh_sdl_get_text(const MsvcString *text, const char **data, size_t *len)
{
  *data = text->cap > 15 ? text->u.ptr : text->u.buf;
  *len = (size_t) text->size;
  if (*len > 0 && (*data)[*len - 1] == 0) (*len)--;
}

/* replaces the text (len bytes, no NUL needed); returns 1 on success */
static GOH_UNUSED int goh_sdl_set_text(MsvcString *text, const char *data, size_t len)
{
  g_sdl_assign(text, len + 1, 0);
  if ((size_t) text->size != len + 1) return 0;
  memcpy(text->cap > 15 ? text->u.ptr : text->u.buf, data, len);
  return 1;
}

static unsigned long long goh_sdl_thunk(BYTE *source, void *arg)
{
  char path[512];
  msvc_cstr((const MsvcString *) (source + 0xc0), path, sizeof path);
  if (path[0]) g_sdl_fn(path, (MsvcString *) (source + 0x98));
  return g_sdl_next(source, arg);
}

/* eSDLReader source ctor: mov esi,[rbp+28h]; xor r8d,r8d; sub esi,[rbp+30h]; mov rcx,rdi; lea eax,[rsi+1];
 * movsxd rdx,eax; call <assign>; cmp qword [rdi+18h],0Fh; jbe; mov rdi,[rdi]; mov r8d,esi; mov rdx,rdi;
 * mov rcx,rbp; call <stream read>; mov rdx,[rsp+60h]; mov rcx,rbx; call <lexer init>
 * (the last call's displacement is not part of the signature: it still matches after another mod hooked it) */
static const int SIG_SDL_SOURCE[] = {
  0x8B,0x75,0x28,0x45,0x33,0xC0,0x2B,0x75,0x30,0x48,0x8B,0xCF,0x8D,0x46,0x01,0x48,0x63,0xD0,0xE8,W,W,W,W,
  0x48,0x83,0x7F,0x18,0x0F,0x76,0x03,0x48,0x8B,0x3F,0x44,0x8B,0xC6,0x48,0x8B,0xD7,0x48,0x8B,0xCD,0xE8,W,W,W,W,
  0x48,0x8B,0x54,0x24,0x60,0x48,0x8B,0xCB,0xE8 };

/* hooks the SDL reader; fn runs for every text file before the game parses it.  Returns 1 on success. */
static GOH_UNUSED int goh_sdl_hook(GohSdlTextFn fn)
{
  BYTE *at = NULL;
  int n = scan(SIG_SDL_SOURCE, COUNT(SIG_SDL_SOURCE), &at);
  if (n != 1) { logf_("ERROR: SDL reader signature found %d times", n); return 0; }
  BYTE *stub = alloc_near_module(0x20);
  if (stub == NULL) { logf_("ERROR: SDL reader: no memory near the module"); return 0; }
  abs_jmp(stub, (void *) goh_sdl_thunk);
  FlushInstructionCache(GetCurrentProcess(), stub, 0x20);
  g_sdl_fn = fn;
  g_sdl_assign = (GohStrAssignFn) call_target(at + 0x12);
  g_sdl_call = at + 0x37;

  wchar_t name[64];
  _snwprintf(name, 64, L"Local\\goh_sdl_reader_hook_%lu", (unsigned long) GetCurrentProcessId());
  HANDLE mx = CreateMutexW(NULL, FALSE, name);
  if (mx) WaitForSingleObject(mx, 10000);
  g_sdl_next = (GohLexInitFn) call_target(g_sdl_call);
  BYTE c[5] = { 0xE8, 0, 0, 0, 0 };
  *(INT32 *) (c + 1) = rel32(g_sdl_call + 5, stub);
  int ok = write_code(g_sdl_call, c, 5);
  if (mx) { ReleaseMutex(mx); CloseHandle(mx); }
  if (!ok) { logf_("ERROR: SDL reader: cannot patch the call"); return 0; }

  BYTE *next = (BYTE *) g_sdl_next;
  logf_("SDL reader hooked at +0x%llx (assign +0x%llx), then calls %s", (unsigned long long) (g_sdl_call - g_base),
        (unsigned long long) ((BYTE *) g_sdl_assign - g_base),
        next >= g_text && next < g_text + g_text_size ? "the game's lexer init" : "another mod's hook");
  return 1;
}

#endif
