/*
 * Coop Game Speed - Gates of Hell mod DLL (goh_mods\coop_game_speed.dll)
 * =======================================================================
 *
 * Only the player who HOSTS the coop match needs it.  The loader puts it into the host's game
 * (call_to_arms.exe) and into the local dedicated server the game starts for the match
 * (call_to_arms_server.exe).  The other players need nothing.
 *
 * How network game speed works (found by reverse engineering, game v1.067):
 *  - The dedicated server is the match "master": it sends every player ScheduleQuants messages
 *    {target, delta 20 ms, amount, ratio}.  Each player spaces the scheduled quants in real time
 *    by delta * ratio, so ratio is a native pacing factor (0.5 = quants twice as fast).
 *    The server always sends 1.0.  The 20 ms simulation step never changes: no desync possible.
 *
 * What this DLL does:
 *  1) In the host's game (call_to_arms.exe):
 *     a) eQuantControl::set_speed starts with  call <is_network_session>; test al,al; jne.
 *        The jne is NOPed so speed requests are registered in network games too.
 *     b) The Backspace key ("interface/slowdown") opens the original "Game speed" dialog unless
 *        the same network check says otherwise: that jne is NOPed too.
 *     c) A thread reads the most recent speed request every 250 ms and publishes it in shared memory
 *        "Local\CoopGameSpeed_<game pid>".  In single player nothing else happens: the engine
 *        handles speed itself.
 *  2) In the dedicated server (call_to_arms_server.exe, started with -owner <game pid>):
 *     a) It reads the shared factor (fallback: its own speed requests).
 *     b) Outgoing ScheduleQuants: ratio is multiplied by the factor (the serializer's
 *        "movss xmm6,[rdi+0xC]" jumps to a 3-instruction stub that adds "mulss xmm6,[factor]").
 *     c) Its own time runs 1/factor as fast (import-table redirect of QueryPerformanceCounter,
 *        GetTickCount64, GetTickCount, for the server module only), so it produces quants that
 *        much faster.  Players then run them at that pace because of the ratio.
 *     d) Its own game thread (a lockstep peer too) gets the schedule through the relay with the ratio
 *        already applied while its clock is already scaled: it ignores the ratio.
 *
 * Log: %TEMP%\goh_coop_game_speed_<exe name>.log
 */
#define GOH_MOD_NAME "coop_game_speed"
#define GOH_MOD_VERSION "1.0.1"   /* this mod's own version (release tag <mod-id>-v<version>) */
#include "../../common/goh_common.h"

/* ------------------------------------------------------------------ signatures */
/* eQuantControl::set_speed prologue ... call <is_network_session>; test al,al; jne */
static const int SIG_SET_SPEED[] = {
  0x48,0x89,0x5C,0x24,0x10,0x56,0x48,0x81,0xEC,0x90,0x00,0x00,0x00,0x0F,0x29,0xB4,0x24,0x80,0x00,
  0x00,0x00,0x48,0x8B,0xDA,0x0F,0x29,0x7C,0x24,0x70,0x0F,0x28,0xF3,0x0F,0x28,0xFA,0x48,0x8B,0xF1,
  0xE8,W,W,W,W,0x84,0xC0,0x0F,0x85 };
#define SET_SPEED_JNE 0x2d
/* Backspace: action pressed? -> call <is_network_session>; test al,al; jne skip; open "game_speed_dialog" */
static const int SIG_BACKSPACE[] = {
  0x8B,0x96,0xF0,0x01,0x00,0x00,0x41,0x8B,0xCF,0xE8,W,W,W,W,0x84,0xC0,0x0F,0x84,W,W,W,W,
  0xE8,W,W,W,W,0x84,0xC0,0x0F,0x85,W,W,W,W,0xB9,0x20,0x00,0x00,0x00,0x4C,0x89,0x74,0x24,0x48,
  0xE8,W,W,W,W,0x0F,0x10,0x05 };
#define BACKSPACE_JNE 0x1d
/* ScheduleQuants serializer body: delta, amount ... then "movss xmm6,[rdi+0xC]" (ratio) at +0x30 */
static const int SIG_SCHEDULE_WRITER[] = {
  0x0F,0xB7,0x5F,0x04,0xBA,0x02,0x00,0x00,0x00,0x48,0x8B,0xCE,0xE8,W,W,W,W,
  0xBA,0x02,0x00,0x00,0x00,0x48,0x8B,0xCE,0x66,0x89,0x18,0x0F,0xB7,0x5F,0x08 };
#define RATIO_LOAD 0x30
/* game-thread ScheduleQuants: mulss xmm0,xmm7; xorps xmm7,xmm7; mulss xmm0,[r14+0xC] (ratio); cvttss2si eax,xmm0 */
static const int SIG_OWN_RATIO_USE[] = {
  0xF3,0x0F,0x59,0xC7,0x0F,0x57,0xFF,0xF3,0x41,0x0F,0x59,0x46,0x0C,0xF3,0x0F,0x2C,0xC0 };
#define OWN_RATIO_MULSS 7

#define OFF_REQ_BEGIN 0x20
#define OFF_REQ_END   0x28
#define REQ_SIZE      0x18

/* most recent speed request factor (0.5 = fast, 1.8 = slow), 1.0 if none / not a network game */
static float requested_factor(void)
{
  if (!in_network_game()) return 1.0f;
  BYTE *b = NULL, *e = NULL;
  float f = 1.0f;
  if (safe_read(g_quant_control + OFF_REQ_BEGIN, &b, sizeof b) &&
      safe_read(g_quant_control + OFF_REQ_END, &e, sizeof e) &&
      b != NULL && e > b && (e - b) % REQ_SIZE == 0 &&
      safe_read(e - REQ_SIZE, &f, sizeof f) && f > 0.01f && f < 100.0f)
    return f;
  return 1.0f;
}

static float clamp_factor(float f)
{
  if (f < 1.0f / 3.0f) f = 1.0f / 3.0f;   /* at most 3x faster */
  if (f > 4.0f) f = 4.0f;                 /* at most 4x slower */
  return f;
}

/* ------------------------------------------------------------------ shared factor (game -> server) */
typedef struct {
  volatile LONG magic;          /* 'CGS2' */
  volatile float factor;
  volatile LONG updates;
} Shared;
#define SHARED_MAGIC 0x32534743

static void shared_name(wchar_t *buf, size_t cap, DWORD pid)
{
  _snwprintf(buf, cap, L"Local\\CoopGameSpeed_%lu", (unsigned long) pid);
  buf[cap - 1] = 0;
}

/* ------------------------------------------------------------------ server: time scaling */
typedef struct {
  double scale;
  LONGLONG qpc_real0, qpc_virt0;
  ULONGLONG tick_real0, tick_virt0;
} TimeState;

static TimeState *volatile g_state;
static BOOL (WINAPI *real_qpc)(LARGE_INTEGER *) = NULL;
static ULONGLONG (WINAPI *real_tick64)(void) = NULL;

static BOOL WINAPI my_qpc(LARGE_INTEGER *out)
{
  LARGE_INTEGER r;
  BOOL ok = real_qpc(&r);
  if (!ok || out == NULL) return ok;
  TimeState *s = g_state;
  out->QuadPart = s->qpc_virt0 + (LONGLONG) ((double) (r.QuadPart - s->qpc_real0) * s->scale);
  return ok;
}

static ULONGLONG WINAPI my_tick64(void)
{
  ULONGLONG r = real_tick64();
  TimeState *s = g_state;
  return s->tick_virt0 + (ULONGLONG) (LONGLONG) ((double) (LONGLONG) (r - s->tick_real0) * s->scale);
}

static DWORD WINAPI my_tick(void)
{
  return (DWORD) my_tick64();
}

static void publish_scale(double scale)
{
  /* old states are leaked on purpose: a server thread may still be reading one */
  TimeState *n = (TimeState *) VirtualAlloc(NULL, sizeof(TimeState), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (n == NULL) return;
  TimeState *s = g_state;
  LARGE_INTEGER r;
  real_qpc(&r);
  ULONGLONG t = real_tick64();
  n->scale = scale;
  n->qpc_real0 = r.QuadPart;
  n->qpc_virt0 = s->qpc_virt0 + (LONGLONG) ((double) (r.QuadPart - s->qpc_real0) * s->scale);
  n->tick_real0 = t;
  n->tick_virt0 = s->tick_virt0 + (ULONGLONG) (LONGLONG) ((double) (LONGLONG) (t - s->tick_real0) * s->scale);
  InterlockedExchangePointer((PVOID volatile *) &g_state, n);
}

static int install_time_scale(void)
{
  ULONGLONG *q = find_import_slot("QueryPerformanceCounter");
  ULONGLONG *t64 = find_import_slot("GetTickCount64");
  ULONGLONG *t32 = find_import_slot("GetTickCount");
  if (q == NULL) { logf_("ERROR: QueryPerformanceCounter import not found"); return 0; }
  for (int i = 0; i < 400 && !is_resolved(*q); i++) Sleep(25);
  if (!is_resolved(*q)) { logf_("ERROR: QueryPerformanceCounter import never resolved"); return 0; }
  real_qpc = (BOOL (WINAPI *)(LARGE_INTEGER *)) (ULONG_PTR) *q;
  real_tick64 = (t64 != NULL && is_resolved(*t64)) ? (ULONGLONG (WINAPI *)(void)) (ULONG_PTR) *t64 : GetTickCount64;
  TimeState *s = (TimeState *) VirtualAlloc(NULL, sizeof(TimeState), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (s == NULL) return 0;
  s->scale = 1.0;
  g_state = s;
  int ok = set_slot(q, (void *) my_qpc);
  if (t64 != NULL && is_resolved(*t64)) ok &= set_slot(t64, (void *) my_tick64);
  if (t32 != NULL) {
    for (int i = 0; i < 400 && !is_resolved(*t32); i++) Sleep(25);
    if (is_resolved(*t32)) ok &= set_slot(t32, (void *) my_tick);
  }
  logf_("server time source redirected: %s", ok ? "ok" : "PARTIAL");
  return ok;
}

/* ------------------------------------------------------------------ server: ratio stub */
static volatile float *g_ratio_mult = NULL;    /* lives in the near stub page */

static int install_ratio_stub(void)
{
  BYTE *body = NULL;
  int n = scan(SIG_SCHEDULE_WRITER, COUNT(SIG_SCHEDULE_WRITER), &body);
  if (n != 1) { logf_("ERROR: schedule writer signature matches %d (expected 1)", n); return 0; }
  BYTE *site = body + RATIO_LOAD;
  static const BYTE expect[5] = { 0xF3, 0x0F, 0x10, 0x77, 0x0C };   /* movss xmm6,[rdi+0xC] */
  if (memcmp(site, expect, 5) != 0) { logf_("ERROR: unexpected bytes at ratio load"); return 0; }
  BYTE *page = alloc_near_module(0x1000);
  if (page == NULL) { logf_("ERROR: no memory near the server module"); return 0; }
  BYTE *cell = page + 0x40;
  *(float *) cell = 1.0f;
  g_ratio_mult = (volatile float *) cell;
  BYTE *p = page;
  memcpy(p, expect, 5); p += 5;                               /* movss xmm6,[rdi+0xC] */
  p[0] = 0xF3; p[1] = 0x0F; p[2] = 0x59; p[3] = 0x35;         /* mulss xmm6,[rip+cell] */
  *(INT32 *) (p + 4) = rel32(p + 8, cell); p += 8;
  p[0] = 0xE9; *(INT32 *) (p + 1) = rel32(p + 5, site + 5); p += 5;   /* jmp back */
  FlushInstructionCache(GetCurrentProcess(), page, 0x40);
  BYTE jmp[5] = { 0xE9, 0, 0, 0, 0 };
  *(INT32 *) (jmp + 1) = rel32(site + 5, page);
  if (!write_code(site, jmp, 5)) { logf_("ERROR: cannot patch ratio load"); return 0; }
  logf_("outgoing schedule ratio hooked at +0x%llx", (unsigned long long) (site - g_base));
  return 1;
}

/* ------------------------------------------------------------------ server main */
static DWORD owner_pid_from_cmdline(void)
{
  const wchar_t *c = GetCommandLineW();
  const wchar_t *o = c ? wcsstr(c, L"-owner ") : NULL;
  return o ? (DWORD) wcstoul(o + 7, NULL, 10) : 0;
}

static void server_main(void)
{
  nop_jne(SIG_SET_SPEED, COUNT(SIG_SET_SPEED), SET_SPEED_JNE, "speed request lock");   /* fallback: server's own requests */
  int have_globals = find_speed_globals();
  /* The server's own game thread (a lockstep peer too) gets its schedule through the relay, i.e.
   * with the ratio already multiplied (even twice), while its clock is already scaled: it must
   * ignore the ratio, otherwise it falls behind on "slow" and every player waits for it. */
  {
    BYTE *at = NULL;
    int n = scan(SIG_OWN_RATIO_USE, COUNT(SIG_OWN_RATIO_USE), &at);
    static const BYTE expect[6] = { 0xF3, 0x41, 0x0F, 0x59, 0x46, 0x0C };
    if (n == 1 && memcmp(at + OWN_RATIO_MULSS, expect, 6) == 0 && write_code(at + OWN_RATIO_MULSS, NOP6, 6))
      logf_("server game thread ignores the schedule ratio (+0x%llx)", (unsigned long long) (at + OWN_RATIO_MULSS - g_base));
    else {
      logf_("ERROR: server game-thread ratio use not found (matches %d): nothing changed", n);
      return;
    }
  }
  if (!install_ratio_stub()) return;
  if (!install_time_scale()) return;

  DWORD owner = owner_pid_from_cmdline();
  wchar_t name[64];
  shared_name(name, 64, owner);
  logf_("owner game pid %lu; reading %ls", (unsigned long) owner, name);
  HANDLE map = NULL;
  Shared *sh = NULL;
  float current = 1.0f;
  /* the most recently changed source wins: host game (Backspace) or server requests */
  float wanted = 1.0f;
  const char *wanted_src = "none";
  LONG last_updates = -1;
  float last_server_req = 1.0f;
  for (;;) {
    Sleep(250);
    if (sh == NULL && owner != 0) {
      map = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
      if (map) {
        sh = (Shared *) MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(Shared));
        if (sh) logf_("connected to the host game");
      }
    }
    if (sh != NULL && sh->magic == SHARED_MAGIC) {
      LONG u = sh->updates;
      if (u != last_updates) { last_updates = u; wanted = sh->factor; wanted_src = "host game"; }
    } else if (have_globals) {
      float r = requested_factor();
      if (r != last_server_req) { last_server_req = r; wanted = r; wanted_src = "server requests"; }
    }
    float f = wanted;
    if (!(f > 0.0f)) f = 1.0f;
    f = clamp_factor(f);
    f = (float) ((LONGLONG) (f * 1000.0f + 0.5f)) / 1000.0f;
    if (f != current) {
      *g_ratio_mult = f;                      /* players space quants by delta * ratio * f */
      publish_scale(1.0 / (double) f);        /* server produces quants 1/f as fast */
      logf_("game speed factor %.3f (x%.2f) from %s", f, 1.0 / f, wanted_src);
      current = f;
    }
  }
}

/* ------------------------------------------------------------------ game main */
static void game_main(void)
{
  nop_jne(SIG_SET_SPEED, COUNT(SIG_SET_SPEED), SET_SPEED_JNE, "speed request lock");
  nop_jne(SIG_BACKSPACE, COUNT(SIG_BACKSPACE), BACKSPACE_JNE, "Backspace game speed dialog");
  if (!find_speed_globals()) return;

  wchar_t name[64];
  shared_name(name, 64, GetCurrentProcessId());
  HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(Shared), name);
  Shared *sh = map ? (Shared *) MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, sizeof(Shared)) : NULL;
  if (sh == NULL) { logf_("ERROR: cannot create %ls", name); return; }
  sh->factor = 1.0f;
  sh->updates = 0;
  sh->magic = SHARED_MAGIC;
  logf_("publishing speed requests in %ls", name);

  float current = 1.0f;
  for (;;) {
    Sleep(250);
    float f = clamp_factor(requested_factor());
    if (f != current) {
      sh->factor = f;
      InterlockedIncrement(&sh->updates);
      logf_("speed request factor %.3f (x%.2f)", f, 1.0 / f);
      current = f;
    }
  }
}

static void mod_start(int kind)
{
  if (kind == GOH_GAME) game_main();
  else server_main();
}

GOH_MOD_ENTRY()
