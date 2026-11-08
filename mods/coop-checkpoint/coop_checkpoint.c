/*
 * Coop Checkpoint - Gates of Hell mod DLL (goh_mods\coop_checkpoint.dll)
 * =======================================================================
 *
 * Mid-mission checkpoints in coop: "Save" writes the battle state on every PC at the same
 * simulation step, "Reload" restarts the mission from it.  EVERY player of the match needs this
 * DLL and the coop_checkpoint data mod (mission buttons and scripts, see data/make_coop_checkpoint_mod.py).
 *
 * Save: the mission-script command {"autosave"} (eCmd_autosave::execute) does nothing in network games.
 * In coop the mission script runs only on the dedicated server: the server's DLL saves the game state inside
 * the quant that runs the command and replicates the command to the clients (eSceneActionCommand 0x67), whose
 * DLL saves at the same point of their command stream.  Files: coop_ckpt.sav (clients) and
 * coop_ckpt_srv.sav (local server) in the profile save folder.
 *
 * Reload: the mod's "Reload" button runs {"achievement" {nation coop_ckpt_reload}}, which the
 * game forwards to every client.  Each peer writes a marker file (%TEMP%\coop_checkpoint_reload_<role>.txt)
 * and, when the same mission starts again, the scene loader loads the checkpoint archive instead of the
 * mission (same path block as "load game").  The host's game leaves the match and starts it again by itself.
 * Fixes needed for a reloaded match: quant clock / master scheduler alignment, full trigger serialization
 * on the server, human player ids kept stable, entity-map unregister of ids missing after a reload.
 *
 * Log: %TEMP%\goh_coop_checkpoint_<exe name>.log
 */
#define GOH_MOD_NAME "coop_checkpoint"
#define GOH_MOD_VERSION "1.0.2"   /* this mod's own version (release tag <mod-id>-v<version>) */
#include "../../common/goh_common.h"

/* ------------------------------------------------------------------ coop checkpoint: save */
/*
 * The mission-script command {"autosave"} (eCmd_autosave::execute) starts with
 *   call <is_network_session>; test al,al; jne <return>
 * so in coop it does nothing.  In single player it only raises an event and the game control writes
 * the save one frame later, i.e. after a number of quants that differs per PC.
 * In a network game this DLL replaces the command: it writes the save immediately, inside the quant
 * that runs the command.  Every PC (and the local server) runs that quant identically, so every peer
 * writes the same game state.  Network quants run on the main thread (the mp "game thread" is driven
 * by the frame event), which the DLL checks before saving.
 */
static const int SIG_AUTOSAVE_EXEC[] = {
  0x40,0x55,0x48,0x8D,0xAC,0x24,0x20,0xFF,0xFF,0xFF,0x48,0x81,0xEC,0xE0,0x01,0x00,0x00,
  0xE8,W,W,W,W,0x84,0xC0,0x0F,0x85,W,W,W,W,
  0x48,0x89,0x9C,0x24,0xF0,0x01,0x00,0x00,0x48,0x89,0xB4,0x24,0xF8,0x01,0x00,0x00 };
/* game control "gc:save":  cmp [rip+guard],eax; jg init; lea r8,[rsp+20h]; lea rdx,[rsp+40h];
 * lea rcx,[rip+save_manager]; call Save::CreateSave */
static const int SIG_GC_SAVE_CALL[] = {
  0x39,0x05,W,W,W,W,0x0F,0x8F,W,W,W,W,0x4C,0x8D,0x44,0x24,0x20,0x48,0x8D,0x54,0x24,0x40,
  0x48,0x8D,0x0D,W,W,W,W,0xE8 };


typedef void *(*CreateSaveFn)(void *manager, void *path_out, const MsvcString *name);
static CreateSaveFn g_create_save;
static BYTE *g_save_manager;
static volatile LONG *g_save_manager_guard;   /* MSVC thread-safe static guard: 0 / -1 = not ready */
static const char *g_checkpoint_name;         /* <= 15 chars (inline std::string) */
static volatile LONG g_checkpoints;
/* lazy construction of the save manager (the dedicated server never builds it: no profile) */
typedef void (*InitThreadFn)(volatile LONG *guard);
static InitThreadFn g_init_thread_header, g_init_thread_footer;
static void *g_save_manager_vtable;

static int ensure_save_manager(void)
{
  LONG guard = *g_save_manager_guard;
  if (guard != 0 && guard != -1) return 1;
  if (g_init_thread_header == NULL) return 0;
  g_init_thread_header(g_save_manager_guard);          /* same protocol as the game's own init block */
  if (*g_save_manager_guard == -1) {
    *(void **) g_save_manager = g_save_manager_vtable;  /* eSceneSaveManager: vtable + 3 zeroed fields */
    memset(g_save_manager + 8, 0, 0x18);
    g_init_thread_footer(g_save_manager_guard);
    logf_("checkpoint: save manager constructed by the DLL");
  }
  guard = *g_save_manager_guard;
  return guard != 0 && guard != -1;
}

/* ------------------------------------------------------------------ coop checkpoint: reload state */
static char g_ckpt_archive[512];   /* "@/profiles/<id>/save/coop_ckpt" (archive of the last checkpoint) */
static char g_mission_path[512];   /* scene mission path of the current level (captured at load) */
static const char *g_role = "game"; /* "client" / "server": marker file name */
static volatile ULONGLONG g_busy_since;  /* GetTickCount64 at the start of a checkpoint save / load, 0 = idle */
static volatile DWORD g_busy_tid;
static const char *volatile g_busy_what = "";
static volatile LONG g_full_scripts;  /* > 0 while our checkpoint is saved / loaded */
static volatile LONG g_full_forced;
static volatile LONG g_ctx_logged;  /* SDL context flags logged once per save / load */
static void *volatile *g_exec_thread;   /* Helper::Thread "currently executing thread" global */
/* eBlock serializer: cmp qword [rip+current_thread],0; je <serialize>; ret; int3 */
static const int SIG_EXEC_THREAD_GUARD[] = { 0x48,0x83,0x3D,-1,-1,-1,-1,0x00,0x0F,0x84,-1,-1,-1,-1,0xC3,0xCC };
static DWORD g_force_flags = 0;    /* 0 = only log (the flags the client uses are not known yet) */


static void remember_checkpoint_path(const char *s, unsigned long long size)
{
  if (size == 0 || size >= sizeof g_ckpt_archive) return;
  memcpy(g_ckpt_archive, s, size);
  g_ckpt_archive[size] = 0;
  size_t n = strlen(g_ckpt_archive);
  if (n > 4 && _stricmp(g_ckpt_archive + n - 4, ".sav") == 0) g_ckpt_archive[n - 4] = 0;
  /* the loader appends "map.scn", "mission.scn", "game.scn" directly: the archive path ends with '/' */
  n = strlen(g_ckpt_archive);
  if (n > 0 && g_ckpt_archive[n - 1] != '/' && n + 1 < sizeof g_ckpt_archive) strcat(g_ckpt_archive, "/");
}

static void checkpoint_save(void)
{
  if (!ensure_save_manager()) { logf_("checkpoint: save manager not available, skipped"); return; }
  INT32 clock = 0;
  if (g_quant_control) safe_read(g_quant_control + 0x48, &clock, sizeof clock);
  MsvcString name;
  memset(&name, 0, sizeof name);
  size_t len = strlen(g_checkpoint_name);
  memcpy(name.u.buf, g_checkpoint_name, len);
  name.size = len;
  name.cap = 15;
  BYTE *path = (BYTE *) calloc(1, 0x80);     /* save path object (0x28 bytes); leaked on purpose */
  if (path == NULL) return;
  logf_("checkpoint #%ld: saving '%s' at quant clock %d", (long) InterlockedIncrement(&g_checkpoints),
        g_checkpoint_name, (int) clock);
  ULONGLONG t0 = GetTickCount64();
  LONG forced0 = g_full_forced;
  InterlockedIncrement(&g_full_scripts);
  /* The serializer of a trigger's actions (Helper::Thread::eBlock) writes nothing while a script thread
   * is executing (global "current thread", set by the thread step function).  On the server our save
   * runs inside the autosave command of a script thread: clear it for the duration of the save. */
  void *exec_thread = g_exec_thread ? *g_exec_thread : NULL;
  if (g_exec_thread) *g_exec_thread = NULL;
  g_busy_tid = GetCurrentThreadId(); g_busy_what = "checkpoint save"; g_busy_since = GetTickCount64();
  g_create_save(g_save_manager, path, &name);
  g_busy_since = 0;
  if (g_exec_thread) *g_exec_thread = exec_thread;
  InterlockedDecrement(&g_full_scripts);
  g_ctx_logged = 0;
  if (g_full_forced != forced0) logf_("checkpoint: %ld triggers serialized in the save", (long) (g_full_forced - forced0));
  MsvcString *p = (MsvcString *) path;
  const char *s = p->cap > 15 ? p->u.ptr : p->u.buf;
  logf_("checkpoint written in %llu ms: %.200s", (unsigned long long) (GetTickCount64() - t0),
        (p->size > 0 && p->size < 400) ? s : "?");
  if (p->size > 0 && p->size < 400) remember_checkpoint_path(s, p->size);
}

/* ---- server -> clients: replicate the autosave command ----
 * In coop the mission script runs only on the server.  Commands that clients must see are sent as an
 * eSceneActionCommand (net id 0x67) naming the script command; the client re-creates the command by
 * name and runs its execute (default slot 8 = jmp [vtable+38h]) inside its own command stream.
 * eCmd_entity_manager::execute (and 11 more) does exactly:
 *   call <is_network_server>; test al,al; je skip
 *   lea rax,[eSceneActionCommand vtable]; mov [rsp+28h],67h; mov [rsp+20h],rax; lea rcx,[rsp+20h]
 *   xor eax,eax; mov [rsp+30h],rbx(command); mov [rsp+38h],rax; mov [rsp+80h],rax; mov [rsp+40h],2
 *   call <send>                                                                                     */
static const int SIG_REPLICATE[] = {
  0xE8,W,W,W,W,0x84,0xC0,0x74,W,0x48,0x8D,0x05,W,W,W,W,0xC7,0x44,0x24,0x28,0x67,0x00,0x00,0x00,
  0x48,0x89,0x44,0x24,0x20,0x48,0x8D,0x4C,0x24,0x20,0x33,0xC0,0x48,0x89,0x5C,0x24,0x30,0x48,0x89,
  0x44,0x24,0x38,0x48,0x89,0x84,0x24,0x80,0x00,0x00,0x00,0xC7,0x44,0x24,0x40,0x02,0x00,0x00,0x00,0xE8 };
typedef char (*BoolFn)(void);
typedef void (*SendFn)(void *command);
static BoolFn g_is_net_server;
static SendFn g_send_command;
static void *g_action_vtable;
static int g_replicate;                        /* set on the dedicated server only */
static volatile LONG g_in_autosave;

static int find_replication(void)
{
  BYTE *at = NULL;
  int n = scan(SIG_REPLICATE, COUNT(SIG_REPLICATE), &at);
  if (n < 1) { logf_("ERROR: checkpoint: replication pattern not found"); return 0; }
  g_is_net_server = (BoolFn) (at + 5 + *(INT32 *) (at + 1));
  g_action_vtable = at + 16 + *(INT32 *) (at + 12);
  g_send_command = (SendFn) (at + COUNT(SIG_REPLICATE) + 4 + *(INT32 *) (at + COUNT(SIG_REPLICATE)));
  logf_("checkpoint: replication ready (%d call sites): is_server +0x%llx, action vtable +0x%llx, send +0x%llx", n,
        (unsigned long long) ((BYTE *) g_is_net_server - g_base), (unsigned long long) ((BYTE *) g_action_vtable - g_base),
        (unsigned long long) ((BYTE *) g_send_command - g_base));
  return 1;
}

static void replicate_command(void *command)
{
  if (!g_is_net_server()) { logf_("checkpoint: not the network server, autosave not replicated"); return; }
  __attribute__((aligned(16))) BYTE cmd[0x80];
  memset(cmd, 0, sizeof cmd);
  *(void **) (cmd + 0x00) = g_action_vtable;      /* eSceneActionCommand */
  *(DWORD *) (cmd + 0x08) = 0x67;                 /* net command id */
  *(void **) (cmd + 0x10) = command;              /* the script command (by name on the client) */
  *(DWORD *) (cmd + 0x20) = 2;                    /* plain script command */
  g_send_command(cmd);
  void **holder = *(void ***) (cmd + 0x60);       /* parameter holder, normally none */
  if (holder != NULL) {
    typedef void (*DestroyFn)(void *self, int heap);
    ((DestroyFn) (*(void ***) holder)[4])(holder, (BYTE *) holder != cmd + 0x28);
  }
  logf_("checkpoint: autosave command sent to the clients");
}

/* called by the stub at the start of eCmd_autosave::execute; returns 1 = handled (skip original) */
static int on_autosave_command(void *command)
{
  int net = in_network_game();
  DWORD tid = GetCurrentThreadId();
  logf_("autosave command: network %d, thread %lu (%s)", net, (unsigned long) tid,
        tid == g_main_tid ? "main thread" : "simulation thread");
  if (!net) return 0;                          /* single player: original behaviour */
  if (InterlockedExchange(&g_in_autosave, 1)) { logf_("checkpoint: nested autosave ignored"); return 1; }
  /* The command runs inside the quant on the thread that owns the simulation: saving here captures the
   * game state at a well-defined point of the command stream.  The server then sends the command to the
   * clients, which run it (and so save) at the same point of their own stream. */
  checkpoint_save();
  if (g_replicate) replicate_command(command);
  InterlockedExchange(&g_in_autosave, 0);
  return 1;
}

/* Save::CreateSave: mov rcx,[rbp+0B0h]; call <write screenshot.jpg>; call ...; mov rcx,rax.
 * The dedicated server has no renderer: the screenshot writer dereferences a null pointer. */
static const int SIG_SAVE_SCREENSHOT[] = {
  0x48,0x8B,0x8D,0xB0,0x00,0x00,0x00,0xE8,W,W,W,W,0xE8,W,W,W,W,0x48,0x8B,0xC8 };

static void disable_save_screenshot(void)
{
  BYTE *at = NULL;
  int n = scan(SIG_SAVE_SCREENSHOT, COUNT(SIG_SAVE_SCREENSHOT), &at);
  static const BYTE NOP5[5] = { 0x0F, 0x1F, 0x44, 0x00, 0x00 };
  if (n == 1 && at[7] == 0xE8 && write_code(at + 7, NOP5, 5))
    logf_("checkpoint: save screenshot disabled (+0x%llx)", (unsigned long long) (at + 7 - g_base));
  else
    logf_("ERROR: checkpoint: save screenshot call not found (matches %d)", n);
}

/* Scene::eScreenShotView: on cmSC_SaveStateBegin (0x1011) it builds a texture from the save's
 * screenshot, which the server never wrote -> null dereference while loading the bitmap.
 *   mov r8d,[rdx+8]; mov rdi,rcx; sub r8d,1011h; je <create>; cmp r8d,1; jne <end>
 * With the je removed, 0x1011 falls to "cmp r8d,1; jne end" and is ignored; 0x1012 (end) finds no
 * texture and returns. */
static const int SIG_SCREENSHOT_VIEW[] = {
  0x44,0x8B,0x42,0x08,0x48,0x8B,0xF9,0x41,0x81,0xE8,0x11,0x10,0x00,0x00,0x74,W,0x41,0x83,0xF8,0x01,0x0F,0x85 };

static void disable_screenshot_view(void)
{
  BYTE *at = NULL;
  int n = scan(SIG_SCREENSHOT_VIEW, COUNT(SIG_SCREENSHOT_VIEW), &at);
  static const BYTE NOP2[2] = { 0x66, 0x90 };
  if (n == 1 && at[14] == 0x74 && write_code(at + 14, NOP2, 2))
    logf_("checkpoint: screenshot view ignores save events (+0x%llx)", (unsigned long long) (at + 14 - g_base));
  else
    logf_("ERROR: checkpoint: screenshot view listener not found (matches %d)", n);
}

/* Client: the save screenshot (CreateSave -> 0xa67450 -> 0xa67da0) renders the scene with D3D on the calling
 * thread.  The game saves from the main thread; our checkpoint runs on the simulation thread, where that
 * rendering can block forever (seen on a friend's PC: simulation frozen inside CreateSave, every peer waiting).
 * During our checkpoints the screenshot is skipped and Scene::eScreenShotView (which loads it back on
 * cmSC_SaveStateBegin 0x1011 / End 0x1012) ignores the save events.  Single-player saves are unchanged. */
typedef void (*ShotFn)(void *rcx);
static ShotFn g_shot_orig;
static void *g_view_orig;
static volatile LONG g_shot_skipped;
static void save_screenshot_filter(void *rcx)
{
  if (g_full_scripts > 0) {
    if (InterlockedIncrement(&g_shot_skipped) <= 3) logf_("checkpoint: save screenshot skipped (simulation thread)");
    return;
  }
  g_shot_orig(rcx);
}
static int screenshot_view_filter(void *self, void *event)
{
  (void) self;
  if (g_full_scripts <= 0) return 0;
  DWORD id = 0;
  safe_read((BYTE *) event + 8, &id, sizeof id);
  return id == 0x1011 || id == 0x1012;
}
static void install_save_render_skip(void)
{
  BYTE *at = NULL, *v = NULL;
  int n = scan(SIG_SAVE_SCREENSHOT, COUNT(SIG_SAVE_SCREENSHOT), &at);
  int m = scan(SIG_SCREENSHOT_VIEW, COUNT(SIG_SCREENSHOT_VIEW), &v);
  static const BYTE view_prologue[6] = { 0x40, 0x57, 0x48, 0x83, 0xEC, 0x50 };   /* push rdi; sub rsp,50h */
  if (n != 1 || at[7] != 0xE8 || m != 1 || memcmp(v - 6, view_prologue, 6) != 0) {
    logf_("ERROR: checkpoint: screenshot sites not found (%d, %d): client checkpoints keep the screenshot", n, m);
    return;
  }
  if (!install_entry_hook(v - 6, 6, screenshot_view_filter, &g_view_orig, "checkpoint: screenshot view filter")) return;
  BYTE *call = at + 7;
  BYTE *stub = alloc_near_module(0x20);
  if (stub == NULL) { logf_("ERROR: checkpoint: no memory near the module"); return; }
  stub[0] = 0xFF; stub[1] = 0x25; *(INT32 *) (stub + 2) = 0; *(void **) (stub + 6) = (void *) save_screenshot_filter;
  FlushInstructionCache(GetCurrentProcess(), stub, 0x20);
  g_shot_orig = (ShotFn) (call + 5 + *(INT32 *) (call + 1));
  BYTE c[5] = { 0xE8, 0, 0, 0, 0 };
  *(INT32 *) (c + 1) = rel32(call + 5, stub);
  if (write_code(call, c, 5))
    logf_("checkpoint: save screenshot filtered at +0x%llx (skipped during coop checkpoints)", (unsigned long long) (call - g_base));
}

static int install_checkpoint(const char *name)
{
  g_checkpoint_name = name;
  if (g_quant_control == NULL && !find_speed_globals()) return 0;
  BYTE *call_site = NULL;
  int n = scan(SIG_GC_SAVE_CALL, COUNT(SIG_GC_SAVE_CALL), &call_site);
  if (n != 1) { logf_("ERROR: checkpoint: save call signature matches %d (expected 1)", n); return 0; }
  g_save_manager_guard = (volatile LONG *) (call_site + 6 + *(INT32 *) (call_site + 2));
  g_save_manager = call_site + 29 + *(INT32 *) (call_site + 25);
  g_create_save = (CreateSaveFn) (call_site + 34 + *(INT32 *) (call_site + 30));
  {
    /* jg target = the game's init block: call _Init_thread_header; cmp; jne; lea rax,[vtable]; ...;
     * call atexit; lea rcx,[guard]; call _Init_thread_footer */
    BYTE *blk = call_site + 12 + *(INT32 *) (call_site + 8);
    if (blk[7] == 0xE8 && blk[0x19] == 0x48 && blk[0x1a] == 0x8D && blk[0x1b] == 0x05 && blk[0x4c] == 0xE8) {
      g_init_thread_header = (InitThreadFn) (blk + 12 + *(INT32 *) (blk + 8));
      g_save_manager_vtable = blk + 0x20 + *(INT32 *) (blk + 0x1c);
      g_init_thread_footer = (InitThreadFn) (blk + 0x51 + *(INT32 *) (blk + 0x4d));
    } else
      logf_("WARNING: checkpoint: save manager init block not recognised");
  }

  {
    BYTE *g = NULL;
    if (scan(SIG_EXEC_THREAD_GUARD, COUNT(SIG_EXEC_THREAD_GUARD), &g) == 1) {
      g_exec_thread = (void *volatile *) (g + 8 + *(INT32 *) (g + 3));
      logf_("checkpoint: script thread global at +0x%llx", (unsigned long long) ((BYTE *) g_exec_thread - g_base));
    } else
      logf_("WARNING: checkpoint: script thread global not found");
  }
  BYTE *fn = NULL;
  n = scan(SIG_AUTOSAVE_EXEC, COUNT(SIG_AUTOSAVE_EXEC), &fn);
  if (n != 1) { logf_("ERROR: checkpoint: autosave command signature matches %d (expected 1)", n); return 0; }
  BYTE *page = alloc_near_module(0x1000);
  if (page == NULL) { logf_("ERROR: checkpoint: no memory near the module"); return 0; }
  BYTE *cell = page + 0x80;
  *(void **) cell = (void *) on_autosave_command;
  BYTE *p = page;
  static const BYTE save_regs[] = { 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x48, 0x83, 0xEC, 0x28 };
  memcpy(p, save_regs, sizeof save_regs); p += sizeof save_regs;     /* push rcx,rdx,r8,r9; sub rsp,28h */
  p[0] = 0xFF; p[1] = 0x15; *(INT32 *) (p + 2) = rel32(p + 6, cell); p += 6;   /* call [rip+cell] */
  static const BYTE restore_regs[] = { 0x48, 0x83, 0xC4, 0x28, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59,
                                       0x85, 0xC0, 0x74, 0x01, 0xC3 };   /* ...; test eax,eax; jz +1; ret */
  memcpy(p, restore_regs, sizeof restore_regs); p += sizeof restore_regs;
  memcpy(p, fn, 10); p += 10;                                         /* push rbp; lea rbp,[rsp-0E0h] */
  p[0] = 0xE9; *(INT32 *) (p + 1) = rel32(p + 5, fn + 10); p += 5;    /* jmp back */
  FlushInstructionCache(GetCurrentProcess(), page, 0x80);
  BYTE jmp[5] = { 0xE9, 0, 0, 0, 0 };
  *(INT32 *) (jmp + 1) = rel32(fn + 5, page);
  if (!write_code(fn, jmp, 5)) { logf_("ERROR: checkpoint: cannot patch the autosave command"); return 0; }
  logf_("checkpoint ready: autosave command +0x%llx, CreateSave +0x%llx, manager +0x%llx, save name '%s'",
        (unsigned long long) (fn - g_base), (unsigned long long) ((BYTE *) g_create_save - g_base),
        (unsigned long long) (g_save_manager - g_base), name);
  return 1;
}


/* marker file (one per role and PC, in %TEMP%): line 1 = mission path, line 2 = checkpoint archive */
static void marker_path(wchar_t *out, size_t cap)
{
  DWORD n = GetTempPathW((DWORD) cap, out);
  if (n == 0 || n > cap - 64) { out[0] = 0; return; }
  wcscat(out, L"coop_checkpoint_reload_");
  size_t k = wcslen(out);
  for (const char *r = g_role; *r && k < cap - 8; r++) out[k++] = (wchar_t) *r;
  out[k] = 0;
  wcscat(out, L".txt");
}

/* ---- checkpoint reload: keep the human players' scene ids ----
 * The multiplayer players get the scene ids after the highest actor owner (eActor+0xda8).  A checkpoint holds the
 * humans' units, so each reload moved the humans up by the number of players (4,5 -> 6,7 -> 8,9, max 18) and the
 * mod's autoassign stopped finding their units.  At a normal start the DLL records the start id and the number of
 * players; while a checkpoint loads, owners above the start are mapped back to start + 1 + (id - start - 1) % n. */
static int g_players_base = -1, g_players_count = 0;   /* last normal start of g_players_mission */
static char g_players_mission[512];
static int g_remap_base = -1, g_remap_count = 0;       /* from the reload marker */
static volatile LONG g_remap_active;                   /* > 0 while our checkpoint is loaded */
static volatile LONG g_remapped;
static volatile LONG g_last_load_reload;               /* the current match was started from a checkpoint */
static volatile LONG g_alloc_logged;

static int on_owner_set(void *actor, void *pbyte)
{
  (void) actor;
  if (g_remap_active <= 0 || g_remap_base < 0 || g_remap_count <= 0) return 0;
  BYTE id = 0;
  if (!safe_read(pbyte, &id, 1)) return 0;
  if (id > g_remap_base && id < 0x12) {
    BYTE nid = (BYTE) (g_remap_base + 1 + (id - g_remap_base - 1) % g_remap_count);
    if (nid != id) {
      *(volatile BYTE *) pbyte = nid;
      if (InterlockedIncrement(&g_remapped) <= 4) logf_("RELOAD: actor owner %u -> %u", (unsigned) id, (unsigned) nid);
    }
  }
  return 0;
}

/* called inside the id allocation (r15d = highest actor owner, r12 = eGame): returns the start to use */
static int on_player_alloc(int start, BYTE *game)
{
  BYTE *first = NULL, *last = NULL;
  safe_read(game + 0x160, &first, sizeof first);
  safe_read(game + 0x168, &last, sizeof last);
  int n = (first && last >= first) ? (int) ((last - first) / 8) : 0;
  if (g_last_load_reload) {
    if (InterlockedIncrement(&g_alloc_logged) <= 2)
      logf_("RELOAD: player ids start after %d (normal start: after %d), %d players, %ld actor owners remapped",
            start, g_remap_base, n, (long) g_remapped);
  } else {
    g_players_base = start;
    g_players_count = n;
    strcpy(g_players_mission, g_mission_path);
    if (InterlockedIncrement(&g_alloc_logged) <= 2)
      logf_("players: ids start after %d, %d players (normal start of '%s')", start, n, g_mission_path);
  }
  return start;
}

/* actor owner setter (eActor vtable slot 58): mov [rsp+10h],rbx; push rdi; sub rsp,40h; movzx eax,byte [rdx];
 * mov rbx,rcx; lea rcx,[rsp+50h]; mov [rsp+50h],al; mov rdi,rdx; call; cmp byte [rbx+0DA8h],12h */
static const int SIG_OWNER_SET[] = {
  0x48,0x89,0x5C,0x24,0x10,0x57,0x48,0x83,0xEC,0x40,0x0F,0xB6,0x02,0x48,0x8B,0xD9,0x48,0x8D,0x4C,0x24,0x50,
  0x88,0x44,0x24,0x50,0x48,0x8B,0xFA,0xE8,-1,-1,-1,-1,0x80,0xBB,0xA8,0x0D,0x00,0x00,0x12 };
/* id allocation, after the highest-owner loop: mov rdi,[r12+150h]; mov rbx,[r12+148h]; mov [rsp+60h],r13;
 * lea r13,[r12+1A8h] */
static const int SIG_PLAYER_ALLOC[] = {
  0x49,0x8B,0xBC,0x24,0x50,0x01,0x00,0x00,0x49,0x8B,0x9C,0x24,0x48,0x01,0x00,0x00,0x4C,0x89,0x6C,0x24,0x60,
  0x4D,0x8D,0xAC,0x24,0xA8,0x01,0x00,0x00 };
static void *g_owner_set_orig;
static void install_player_ids(void)
{
  BYTE *os = NULL, *al = NULL;
  int n1 = scan(SIG_OWNER_SET, COUNT(SIG_OWNER_SET), &os);
  int n2 = scan(SIG_PLAYER_ALLOC, COUNT(SIG_PLAYER_ALLOC), &al);
  if (n1 != 1 || n2 != 1) { logf_("ERROR: player ids: owner setter %d, allocation %d matches: not installed", n1, n2); return; }
  BYTE *stub = alloc_near_module(0x80);
  if (stub == NULL) { logf_("ERROR: player ids: no memory near the module"); return; }
  BYTE *s = stub;
  static const BYTE pre[] = { 0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53,  /* push rax..r11 */
                              0x48, 0x83, 0xEC, 0x28,                                          /* sub rsp,28h */
                              0x44, 0x89, 0xF9,                                                /* mov ecx,r15d */
                              0x4C, 0x89, 0xE2 };                                              /* mov rdx,r12 */
  memcpy(s, pre, sizeof pre); s += sizeof pre;
  BYTE *cell = stub + 0x70;
  *(void **) cell = (void *) on_player_alloc;
  s[0] = 0xFF; s[1] = 0x15; *(INT32 *) (s + 2) = rel32(s + 6, cell); s += 6;                 /* call [cell] */
  static const BYTE post[] = { 0x44, 0x8B, 0xF8,                                               /* mov r15d,eax */
                               0x48, 0x83, 0xC4, 0x28,
                               0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58 };
  memcpy(s, post, sizeof post); s += sizeof post;
  memcpy(s, al, 8); s += 8;                                                                    /* mov rdi,[r12+150h] */
  s[0] = 0xE9; *(INT32 *) (s + 1) = rel32(s + 5, al + 8); s += 5;
  FlushInstructionCache(GetCurrentProcess(), stub, 0x80);
  if (!install_entry_hook(os, 5, on_owner_set, &g_owner_set_orig, "player ids: actor owner setter")) return;
  BYTE j[8] = { 0xE9, 0, 0, 0, 0, 0x0F, 0x1F, 0x00 };
  *(INT32 *) (j + 1) = rel32(al + 5, stub);
  if (write_code(al, j, 8))
    logf_("player ids: allocation hooked at +0x%llx", (unsigned long long) (al - g_base));
}

static void arm_reload(void)
{
  if (g_ckpt_archive[0] == 0) { logf_("reload: no checkpoint in this session, nothing armed"); return; }
  if (g_mission_path[0] == 0) { logf_("reload: mission path unknown, nothing armed"); return; }
  wchar_t mp[MAX_PATH];
  marker_path(mp, MAX_PATH);
  FILE *f = _wfopen(mp, L"w");
  if (f == NULL) { logf_("ERROR: reload: cannot write the marker"); return; }
  fprintf(f, "%s\n%s\n", g_mission_path, g_ckpt_archive);
  if (g_players_base >= 0 && _stricmp(g_players_mission, g_mission_path) == 0)
    fprintf(f, "players %d %d\n", g_players_base, g_players_count);
  fclose(f);
  logf_("reload ARMED: next start of '%s' loads '%s' (player ids after %d, %d players)", g_mission_path, g_ckpt_archive,
        g_players_base, g_players_count);
}

static int read_marker(char *mission, char *archive, size_t cap)
{
  wchar_t mp[MAX_PATH];
  marker_path(mp, MAX_PATH);
  /* an armed reload expires after 15 minutes (e.g. left behind by a crash) */
  WIN32_FILE_ATTRIBUTE_DATA fa;
  if (!GetFileAttributesExW(mp, GetFileExInfoStandard, &fa)) return 0;
  FILETIME now;
  GetSystemTimeAsFileTime(&now);
  ULONGLONG t_file = ((ULONGLONG) fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
  ULONGLONG t_now = ((ULONGLONG) now.dwHighDateTime << 32) | now.dwLowDateTime;
  if (t_now > t_file && t_now - t_file > 15ULL * 60 * 10000000) {
    logf_("reload marker older than 15 minutes: ignored and deleted");
    DeleteFileW(mp);
    return 0;
  }
  FILE *f = _wfopen(mp, L"r");
  if (f == NULL) return 0;
  int ok = fgets(mission, (int) cap, f) != NULL && fgets(archive, (int) cap, f) != NULL;
  char line[64];
  int b = -1, c = 0;
  if (ok && fgets(line, sizeof line, f) != NULL && sscanf(line, "players %d %d", &b, &c) == 2 && b >= 0 && b < 0x12 && c > 0) {
    g_remap_base = b; g_remap_count = c;
  } else { g_remap_base = -1; g_remap_count = 0; }
  fclose(f);
  if (!ok) return 0;
  mission[strcspn(mission, "\r\n")] = 0;
  archive[strcspn(archive, "\r\n")] = 0;
  return mission[0] && archive[0];
}

static void delete_marker(void)
{
  wchar_t mp[MAX_PATH];
  marker_path(mp, MAX_PATH);
  DeleteFileW(mp);
}

/* eCmd_achievement::execute: {"achievement" {name coop_ckpt_reload} {nation ...}} is the reload request.
 * The name is a std::string at +0x18.  The server lets the original run (it sends the command to the
 * clients and, in a network game, unlocks nothing); the clients skip it. */
static int g_is_server;
static void *g_achievement_orig;
static void auto_restart_request(void);
static int on_achievement(void *command, void *unused)
{
  (void) unused;
  char name[64];
  msvc_cstr((const MsvcString *) ((BYTE *) command + 0x18), name, sizeof name);
  if (strcmp(name, "coop_ckpt_reload") != 0) return 0;
  logf_("reload request received (%s)", g_is_server ? "server: forwarded to the clients" : "client");
  arm_reload();
  if (!g_is_server && g_ckpt_archive[0] != 0) auto_restart_request();
  return g_is_server ? 0 : 1;
}

/* Scene loader (client 0xa683a0 / server 0x87bc90): load(scene, paths).  paths == NULL = normal mission
 * start (paths derived from the mission at scene+0x18).  When a reload is armed for that mission, the
 * paths are built from the checkpoint archive exactly as "load game" does (builder 0xa6a610). */
typedef void (*LoaderFn)(void *scene, void *paths);
typedef void (*PathBuilderFn)(void *scene, void *paths, const MsvcString *archive);
static LoaderFn g_loader_orig;
static PathBuilderFn g_path_builder;

/* Trigger (de)serializer (server 0x7aa1a0).  On the dedicated server scripts are serialized in the
 * "state only" mode (SDL context +0x170 = 4 or 5): trigger actions are not written, so a reloaded
 * checkpoint has triggers that fire and do nothing.  While our checkpoint is written or loaded the
 * mode is temporarily set to 1 (the game only tests ==0, ==4 and 4..5), i.e. full serialization. */
typedef void *(*CtxGetterFn)(void);
typedef void (*TriggerSerFn)(void *trigger);
static CtxGetterFn g_sdl_context;
static TriggerSerFn g_trigger_ser_orig;

static int on_trigger_serialize(void *trigger, void *unused)
{
  (void) unused;
  (void) trigger;
  if (g_full_scripts <= 0 || g_sdl_context == NULL) return 0;
  BYTE *ctx = (BYTE *) g_sdl_context();
  if (ctx == NULL) return 0;
  InterlockedIncrement(&g_full_forced);
  /* ctx+0x170 is a 32-bit flag word: low byte = mode (4/5 = "state only" on the dedicated server),
   * bit 17 = really write sub-objects such as trigger actions (SDL field writer 0x20b3f0).  The
   * context is a temporary object of the save / load itself: nothing to restore afterwards. */
  DWORD flags = *(volatile DWORD *) (ctx + 0x170);
  if (InterlockedExchange(&g_ctx_logged, 1) == 0)
    logf_("%s: SDL context flags 0x%08lx", g_is_server ? "server" : "client", (unsigned long) flags);
  if (!g_is_server || g_force_flags == 0) return 0;
  BYTE mode = (BYTE) flags;
  DWORD want = flags | 0x20000;
  if (mode == 4 || mode == 5) want = (want & ~0xFFUL) | 1;
  if (want != flags) *(volatile DWORD *) (ctx + 0x170) = want;
  return 0;
}
static const int SIG_TRIGGER_SER[] = {
  0x48,0x89,0x5C,0x24,0x08,0x55,0x48,0x8B,0xEC,0x48,0x83,0xEC,0x60,0x83,0x3D,W,W,W,W,0x01,0x48,0x8B,0xD9,0x0F,0x84 };
static const int SIG_SDL_CONTEXT[] = {
  0x8B,0x0D,W,W,W,W,0x65,0x48,0x8B,0x04,0x25,0x58,0x00,0x00,0x00,0xBA,0xB0,0x40,0x00,0x00,0x48,0x8B,0x04,0xC8,0x48,0x8B,0x04,0x02,0xC3 };

/* Server quant scheduler (Multiplayer::eQuantServerJob) start: resets the last scheduled quant
 * (+0x40) to 0.  After a reload it must start from the checkpoint time instead. */
static volatile INT32 g_master_start = -1;
typedef void (*SchedulerStartFn)(void *job);
static SchedulerStartFn g_scheduler_start_orig;
static int on_scheduler_start(void *job, void *unused)
{
  (void) unused;
  g_scheduler_start_orig(job);
  INT32 start = g_master_start;
  if (start > 0) {
    *(volatile INT32 *) ((BYTE *) job + 0x40) = start;
    logf_("RELOAD: master quant scheduler starts at %d", (int) start);
  } else
    logf_("master quant scheduler started at 0");
  g_master_start = -1;
  return 1;
}
static const int SIG_SCHEDULER_START[] = {
  0x48,0x89,0x5C,0x24,0x10,0x57,0x48,0x83,0xEC,0x20,0x66,0xC7,0x41,0x21,0x00,0x00,0x33,0xC0,0x48,0x89,
  0x41,0x30,0x48,0x8B,0xF9,0x89,0x41,0x40,0xC6,0x41,0x20,0x01 };

static int on_scene_load(void *scene, void *paths)
{
  if (paths != NULL) return 0;                          /* load game / editor: untouched */
  char mission[512];
  msvc_cstr((const MsvcString *) ((BYTE *) scene + 0x18), mission, sizeof mission);
  logf_("scene load: mission '%s'", mission);
  if (mission[0] == 0) return 0;
  strcpy(g_mission_path, mission);
  g_ckpt_archive[0] = 0;                                /* a new level instance: no checkpoint yet */
  char want[512], archive[512];
  int armed = read_marker(want, archive, sizeof want);
  if (armed && _stricmp(want, mission) != 0) { logf_("reload armed for '%s': not this mission", want); armed = 0; }
  if (!armed) {
    /* normal mission start: run it here only to log the quant clock it leaves (reference for reloads) */
    g_last_load_reload = 0; g_alloc_logged = 0;
    g_loader_orig(scene, NULL);
    INT32 clock = -1;
    if (g_quant_control) safe_read(g_quant_control + 0x48, &clock, sizeof clock);
    logf_("scene loaded normally; quant clock %d", (int) clock);
    g_master_start = -1;
    return 1;
  }
  delete_marker();
  /* paths block as built by "load from save": six path objects/strings and a flag byte */
  __attribute__((aligned(16))) BYTE block[0x100];
  memset(block, 0, sizeof block);
  static const int strs[] = { 0x00, 0x28, 0x50, 0x78, 0xA0, 0xC0 };
  for (int i = 0; i < 6; i++) *(unsigned long long *) (block + strs[i] + 0x18) = 15;
  MsvcString arch;
  memset(&arch, 0, sizeof arch);
  size_t len = strlen(archive);
  if (len <= 15) { memcpy(arch.u.buf, archive, len + 1); arch.cap = 15; }
  else {
    char *heap = (char *) malloc(len + 1);               /* only read by the game; leaked on purpose */
    if (heap == NULL) return 0;
    memcpy(heap, archive, len + 1);
    arch.u.ptr = heap;
    arch.cap = len;
  }
  arch.size = len;
  g_path_builder(scene, block, &arch);
  char m[256];
  logf_("RELOAD: loading checkpoint '%s' (map file '%s')", archive, msvc_cstr((MsvcString *) block, m, sizeof m));
  strcpy(g_ckpt_archive, archive);
  /* The checkpoint restores the scene quant clock ({Quant {scenequant N}}, eQuantControl+0x48), but the
   * multiplayer layer numbers the quants of the new match from its own start: the clients then drop the
   * server's commands as "late" (msgQuant << curQuant).  Every peer puts the clock back. */
  /* The checkpoint restores the scene clock (entity timers are absolute, so it must stay), while the
   * multiplayer master numbers the quants of the new match from 0.  Every peer sets its clock to the same
   * value (the server's checkpoint time, a client saves exactly one quant = 20 ms later, rounded down to a
   * whole second) and the server starts its quant scheduler there (see on_scheduler_start). */
  INT32 clock_before = 0, clock_after = 0;
  if (g_quant_control) safe_read(g_quant_control + 0x48, &clock_before, sizeof clock_before);
  LONG forced0 = g_full_forced;
  InterlockedIncrement(&g_full_scripts);
  g_last_load_reload = 1; g_alloc_logged = 0; g_remapped = 0;
  if (g_remap_base < 0) logf_("RELOAD: player ids of the normal start unknown: owners not remapped");
  InterlockedIncrement(&g_remap_active);
  g_busy_tid = GetCurrentThreadId(); g_busy_what = "checkpoint load"; g_busy_since = GetTickCount64();
  g_loader_orig(scene, block);
  InterlockedDecrement(&g_remap_active);
  logf_("RELOAD: %ld actor owners remapped (ids after %d, %d players)", (long) g_remapped, g_remap_base, g_remap_count);
  g_busy_since = 0;
  InterlockedDecrement(&g_full_scripts);
  g_ctx_logged = 0;
  if (g_full_forced != forced0) logf_("RELOAD: %ld triggers deserialized", (long) (g_full_forced - forced0));
  if (g_quant_control) {
    safe_read(g_quant_control + 0x48, &clock_after, sizeof clock_after);
    INT32 base = clock_after - (g_is_server ? 0 : 20);
    if (base < 0) base = 0;
    base -= base % 1000;
    *(volatile INT32 *) (g_quant_control + 0x48) = base;
    g_master_start = base;
    logf_("RELOAD: checkpoint loaded; quant clock %d before, %d after load, set to %d", (int) clock_before,
          (int) clock_after, (int) base);
  }
  return 1;
}

/* load-from-save: lea r8,[rbp+..]; lea rdx,[rsp+20h]; mov rcx,rbx; call <path builder>;
 *                 lea rdx,[rsp+20h]; mov rcx,rbx; call <scene loader> */
static const int SIG_LOAD_FROM_SAVE[] = {
  0x4C,0x8D,0x45,W,0x48,0x8D,0x54,0x24,0x20,0x48,0x8B,0xCB,0xE8,W,W,W,W,
  0x48,0x8D,0x54,0x24,0x20,0x48,0x8B,0xCB,0xE8 };
static const int SIG_ACHIEVEMENT_EXEC[] = {
  0x48,0x8B,0xC4,0x48,0x89,0x70,0x20,0x55,0x48,0x8D,0x68,0xD8,0x48,0x81,0xEC,0x20,0x01,0x00,0x00,
  0x83,0x79,0x10,0xFF,0x48,0x8B,0xF1,0x0F,0x84 };

/* Entity unregistration (Scene::eEntityRegistrator, client 0xa60150): the entity is erased from the
 * id -> entity map keyed by entity+0xc0 without checking that the lookup found it.  After a checkpoint reload
 * some entities are not in the map, the game erases end() (the map's head node) and frees it: heap corruption
 * (c0000374) when the match is left.  Two such sites; the erase call is redirected to a stub that skips
 * erase + delete when the node is the head, and logs the entity. */
static volatile LONG g_unreg_missing = 0;
static void on_unregister_missing(UINT32 id, BYTE *entity)
{
  LONG n = InterlockedIncrement(&g_unreg_missing);
  if (n <= 20 || (n & (n - 1)) == 0) {
    UINT32 key = 0, f30 = 0, f38 = 0;
    safe_read(entity + 0xc0, &key, sizeof key);
    safe_read(entity + 0x30, &f30, sizeof f30);
    safe_read(entity + 0x38, &f38, sizeof f38);
    logf_("entity map: unregister of id %u not in map (entity %p, +0xc0 %u, flags %08x/%08x), skipped [#%ld]",
          id, entity, key, f30, f38, (long) n);
  }
}
/* cmp byte [rdx+19h],0; jne; cmp ebx,[rdx+20h]; jae; mov rdx,rcx; lea rcx,[map]; call erase;
 * mov edx,30h; mov rcx,rax; call delete */
static const int SIG_UNREGISTER_ERASE[] = {
  0x80,0x7A,0x19,0x00,0x75,0x05,0x3B,0x5A,0x20,0x73,0x03,0x48,0x8B,0xD1,0x48,0x8D,0x0D,W,W,W,W,
  0xE8,W,W,W,W,0xBA,0x30,0x00,0x00,0x00,0x48,0x8B,0xC8,0xE8 };
static void install_entity_map_fix(void)
{
  int fixed = 0;
  BYTE *from = g_text;
  SIZE_T left = g_text_size;
  for (int k = 0; k < 4; k++) {
    BYTE *at = NULL;
    int n = scan_in(from, left, SIG_UNREGISTER_ERASE, COUNT(SIG_UNREGISTER_ERASE), &at);
    if (n == 0) break;
    BYTE *call = at + 0x15;
    BYTE *erase = call + 5 + *(INT32 *) (call + 1);
    BYTE *stub = alloc_near_module(0x100);
    if (stub == NULL) { logf_("ERROR: entity map fix: no memory near the module"); break; }
    BYTE *p = stub;
    static const BYTE head[] = { 0x48, 0x3B, 0x11, 0x74, 0x0E };            /* cmp rdx,[rcx]; je skip */
    memcpy(p, head, sizeof head); p += sizeof head;
    p[0] = 0xFF; p[1] = 0x25; *(INT32 *) (p + 2) = 0; *(void **) (p + 6) = erase; p += 14;   /* jmp erase */
    static const BYTE skip[] = { 0x48, 0x83, 0x04, 0x24, 0x0D,             /* add [rsp],0Dh: past delete */
                                 0x8B, 0xCB, 0x48, 0x8B, 0xD7 };           /* mov ecx,ebx; mov rdx,rdi */
    memcpy(p, skip, sizeof skip); p += sizeof skip;
    p[0] = 0xFF; p[1] = 0x25; *(INT32 *) (p + 2) = 0; *(void **) (p + 6) = (void *) on_unregister_missing; p += 14;
    FlushInstructionCache(GetCurrentProcess(), stub, 0x100);
    BYTE c[5] = { 0xE8, 0, 0, 0, 0 };
    *(INT32 *) (c + 1) = rel32(call + 5, stub);
    if (write_code(call, c, 5)) {
      fixed++;
      logf_("entity map fix: unregister erase at +0x%llx", (unsigned long long) (call - g_base));
    }
    left -= (SIZE_T) (at + 1 - from);
    from = at + 1;
  }
  if (fixed == 0) logf_("ERROR: entity map fix: unregister erase site not found");
}

/* Automatic restart after "Reload" (host client only).  The reload is armed on every peer by the
 * replicated command; the host then does what it would do by hand: Esc menu > close / abandon (the confirm
 * handler "gui.gm.leave", client 0x2df480) and, back in the lobby, the start button (0x2f80b0:
 * session service vtable +0x158).  GUI code must run on the main thread: the game window is subclassed and the
 * steps are driven by a timer posted to it. */
typedef void *(*GetterFn)(void);
typedef void *(*SessionOfFn)(void *svc);
typedef char (*BoolOfFn)(void *session);
typedef void (*LeaveFn)(void *capture, int button);
static GetterFn g_session_service;      /* 0xc65bc0 */
static SessionOfFn g_session_of;        /* 0xc80750 */
static BoolOfFn g_can_start;            /* 0xc3cec0 */
static BoolOfFn g_is_campaign;          /* 0xc409d0 */
static GetterFn g_game_client;          /* 0xc65c20 */
static LeaveFn g_leave;                 /* 0x2df480 */
static HWND g_auto_hwnd;
static WNDPROC g_auto_prev_proc;
static volatile LONG g_auto_state;      /* 0 idle, 1 leave pending, 2 waiting for the lobby */
static ULONGLONG g_auto_t0;
#define AUTO_MSG (WM_APP + 0x4C)
#define AUTO_TIMER 0xC0C1

static int vcall_bool(void *obj, int off)
{
  if (obj == NULL) return 0;
  void **vt = *(void ***) obj;
  return ((char (*)(void *)) vt[off / 8])(obj) != 0;
}
static int server_marker_present(void)
{
  wchar_t mp[MAX_PATH];
  DWORD n = GetTempPathW(MAX_PATH, mp);
  if (n == 0 || n > MAX_PATH - 48) return 0;
  wcscat(mp, L"coop_checkpoint_reload_server.txt");
  return GetFileAttributesW(mp) != INVALID_FILE_ATTRIBUTES;
}
static int client_marker_present(void)
{
  char m[512], a[512];
  return read_marker(m, a, sizeof m);
}
static void auto_step(HWND hwnd)
{
  ULONGLONG now = GetTickCount64();
  LONG st = g_auto_state;
  if (st == 1) {
    if (now - g_auto_t0 < 2000) return;              /* let every peer execute the reload command */
    void *gc = g_game_client();
    int host = vcall_bool(gc, 0xb8);
    int srv = server_marker_present();
    if (!host || !srv) {
      logf_("auto restart: not the host (host %d, local server armed %d): waiting for the host", host, srv);
      g_auto_state = 0; KillTimer(hwnd, AUTO_TIMER); return;
    }
    void *svc = g_session_service();
    int campaign = svc ? g_is_campaign(g_session_of(svc)) != 0 : 0;
    __attribute__((aligned(16))) void *capture[4] = { NULL, NULL, (void *) (ULONG_PTR) (campaign ? 1 : 0), NULL };
    logf_("auto restart: leaving the match (%s)", campaign ? "campaign: abandon" : "close game");
    g_auto_state = 2;
    g_auto_t0 = now;
    g_leave(capture, 4);
    return;
  }
  if (st == 2) {
    if (now - g_auto_t0 > 90000) {
      logf_("auto restart: the lobby did not become ready in 90 s: start the mission by hand");
      g_auto_state = 0; KillTimer(hwnd, AUTO_TIMER); return;
    }
    if (!client_marker_present()) {                  /* the mission already started (by hand?) */
      logf_("auto restart: reload no longer armed: done");
      g_auto_state = 0; KillTimer(hwnd, AUTO_TIMER); return;
    }
    if (now - g_auto_t0 < 3000) return;
    void *svc = g_session_service();
    if (svc == NULL || !vcall_bool(svc, 0xb8)) return;
    void *session = g_session_of(svc);
    if (session == NULL || !g_can_start(session)) return;
    logf_("auto restart: lobby ready after %llu ms: starting the mission", (unsigned long long) (now - g_auto_t0));
    g_auto_state = 0;
    KillTimer(hwnd, AUTO_TIMER);
    void **vt = *(void ***) svc;
    ((void (*)(void *)) vt[0x158 / 8])(svc);
  }
}
static LRESULT CALLBACK auto_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
  if (msg == AUTO_MSG) {
    SetTimer(hwnd, AUTO_TIMER, 250, NULL);
    return 0;
  }
  if (msg == WM_TIMER && wp == AUTO_TIMER) {
    auto_step(hwnd);
    return 0;
  }
  return CallWindowProcW(g_auto_prev_proc, hwnd, msg, wp, lp);
}
static BOOL CALLBACK find_main_window(HWND h, LPARAM lp)
{
  if (GetWindowThreadProcessId(h, NULL) == g_main_tid && IsWindowVisible(h) && GetWindow(h, GW_OWNER) == NULL) {
    *(HWND *) lp = h;
    return FALSE;
  }
  return TRUE;
}
static void auto_restart_request(void)
{
  if (g_leave == NULL) return;
  if (g_auto_hwnd == NULL) {
    HWND h = NULL;
    EnumWindows(find_main_window, (LPARAM) &h);
    if (h == NULL) { logf_("auto restart: game window not found"); return; }
    g_auto_prev_proc = (WNDPROC) SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR) auto_wndproc);
    if (g_auto_prev_proc == NULL) { logf_("auto restart: cannot subclass the game window"); return; }
    g_auto_hwnd = h;
  }
  g_auto_t0 = GetTickCount64();
  g_auto_state = 1;
  PostMessageW(g_auto_hwnd, AUTO_MSG, 0, 0);
  logf_("auto restart: scheduled");
}
/* start button: push rbx; sub rsp,20h; mov rbx,rcx; mov rcx,[rcx+100h]; call session_of; mov rcx,rax;
 * call can_start; test al,al; je; mov dl,1; mov rcx,rbx; call; mov rcx,[rbx+198h] ... call service */
static const int SIG_LOBBY_START[] = {
  0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0x48,0x8B,0x89,0x00,0x01,0x00,0x00,0xE8,W,W,W,W,0x48,0x8B,0xC8,
  0xE8,W,W,W,W,0x84,0xC0,0x74,0x3E,0xB2,0x01,0x48,0x8B,0xCB,0xE8,W,W,W,W,0x48,0x8B,0x8B,0x98,0x01,0x00,0x00 };
#define LOBBY_START_SERVICE_CALL 0x45
/* "gui.gm.leave": ... cmp ebx,4; jne; cmp byte [rsi+10h],0; je; mov [rsp+0C0h],rdi; call game_client */
static const int SIG_LEAVE[] = {
  0x48,0x89,0x5C,0x24,0x10,0x56,0x48,0x81,0xEC,0xB0,0x00,0x00,0x00,0x8B,0xDA,0x48,0x8B,0xF1,0xE8,W,W,W,W,
  0x48,0x8B,0xC8,0x48,0x8D,0x15,W,W,W,W,0xE8,W,W,W,W,0x83,0xFB,0x04,0x0F,0x85,W,W,W,W,0x80,0x7E,0x10,0x00,
  0x0F,0x84,W,W,W,W,0x48,0x89,0xBC,0x24,0xC0,0x00,0x00,0x00,0xE8 };
#define LEAVE_GAME_CLIENT_CALL 0x41
/* game menu "close game": call service; mov rcx,rax; call session_of; mov rcx,rax; call is_campaign;
 * test al,al; je; call; lea rdx */
static const int SIG_IS_CAMPAIGN[] = {
  0xE8,W,W,W,W,0x48,0x8B,0xC8,0xE8,W,W,W,W,0x48,0x8B,0xC8,0xE8,W,W,W,W,0x84,0xC0,0x0F,0x84,W,W,W,W,0xE8,W,W,W,W,
  0x48,0x8D,0x15 };
static void install_auto_restart(void)
{
  BYTE *st = NULL, *lv = NULL, *ic = NULL;
  int a = scan(SIG_LOBBY_START, COUNT(SIG_LOBBY_START), &st);
  int b = scan(SIG_LEAVE, COUNT(SIG_LEAVE), &lv);
  int c = scan(SIG_IS_CAMPAIGN, COUNT(SIG_IS_CAMPAIGN), &ic);
  if (a != 1 || b != 1 || c != 1) { logf_("ERROR: auto restart: signatures %d/%d/%d (expected 1/1/1): disabled", a, b, c); return; }
  g_session_of = (SessionOfFn) call_target(st + 0x10);
  g_can_start = (BoolOfFn) call_target(st + 0x18);
  g_session_service = (GetterFn) call_target(st + LOBBY_START_SERVICE_CALL);
  g_game_client = (GetterFn) call_target(lv + LEAVE_GAME_CLIENT_CALL);
  g_is_campaign = (BoolOfFn) call_target(ic + 0x10);
  if ((BYTE *) g_session_service != call_target(ic) || (BYTE *) g_session_of != call_target(ic + 8)) {
    logf_("ERROR: auto restart: inconsistent session functions: disabled"); return;
  }
  g_leave = (LeaveFn) lv;
  logf_("auto restart ready: leave +0x%llx, lobby start via service +0x%llx vtable+0x158",
        (unsigned long long) (lv - g_base), (unsigned long long) ((BYTE *) g_session_service - g_base));
}

static void install_reload(const char *role, int is_server)
{
  g_role = role;
  g_is_server = is_server;
  BYTE *at = NULL;
  int n = scan(SIG_LOAD_FROM_SAVE, COUNT(SIG_LOAD_FROM_SAVE), &at);
  if (n != 1) { logf_("ERROR: reload: load-from-save signature matches %d", n); return; }
  g_path_builder = (PathBuilderFn) (at + 17 + *(INT32 *) (at + 13));
  BYTE *loader = at + 30 + *(INT32 *) (at + 26);
  static const BYTE loader_head[5] = { 0x48, 0x89, 0x5C, 0x24, 0x20 };   /* mov [rsp+20h],rbx */
  if (memcmp(loader, loader_head, 5) != 0) { logf_("ERROR: reload: unexpected scene loader prologue"); return; }
  logf_("reload: path builder +0x%llx", (unsigned long long) ((BYTE *) g_path_builder - g_base));
  if (!install_entry_hook(loader, 5, (EntryHandler) on_scene_load, (void **) &g_loader_orig, "reload: scene loader")) return;
  BYTE *ach = NULL;
  n = scan(SIG_ACHIEVEMENT_EXEC, COUNT(SIG_ACHIEVEMENT_EXEC), &ach);
  if (n != 1) { logf_("ERROR: reload: achievement command signature matches %d", n); return; }
  install_entry_hook(ach, 7, (EntryHandler) on_achievement, &g_achievement_orig, "reload: achievement command");
  {
    BYTE *gt = NULL, *ts = NULL;
    if (scan(SIG_SDL_CONTEXT, COUNT(SIG_SDL_CONTEXT), &gt) == 1 && scan(SIG_TRIGGER_SER, COUNT(SIG_TRIGGER_SER), &ts) == 1) {
      g_sdl_context = (CtxGetterFn) gt;
      install_entry_hook(ts, 5, (EntryHandler) on_trigger_serialize, (void **) &g_trigger_ser_orig,
                         "reload: trigger serializer (full scripts in checkpoints)");
    } else
      logf_("ERROR: reload: trigger serializer / SDL context not found");
  }
  install_player_ids();
  if (is_server) {
    BYTE *st = NULL;
    n = scan(SIG_SCHEDULER_START, COUNT(SIG_SCHEDULER_START), &st);
    if (n != 1) { logf_("ERROR: reload: scheduler start signature matches %d", n); return; }
    install_entry_hook(st, 5, (EntryHandler) on_scheduler_start, (void **) &g_scheduler_start_orig,
                       "reload: master quant scheduler start");
  }
}

/* Heap corruption (c0000374) and other fail-fast errors terminate the process bypassing the game's crash
 * reporter.  ntdll first raises the status as an exception, which a vectored handler sees: log the stack. */
static void describe_address(void *a, char *buf, size_t cap)
{
  HMODULE m = NULL;
  wchar_t path[MAX_PATH];
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         (LPCWSTR) a, &m) && m != NULL && GetModuleFileNameW(m, path, MAX_PATH)) {
    wchar_t *b = wcsrchr(path, L'\\');
    snprintf(buf, cap, "%ls+0x%llx", b ? b + 1 : path, (unsigned long long) ((BYTE *) a - (BYTE *) m));
  } else
    snprintf(buf, cap, "%p", a);
}
static volatile LONG g_fatal_logged = 0;
static LONG WINAPI fatal_exception_logger(EXCEPTION_POINTERS *ep)
{
  DWORD code = ep->ExceptionRecord->ExceptionCode;
  if (code != 0xC0000374 && code != 0xC0000409) return EXCEPTION_CONTINUE_SEARCH;
  if (InterlockedIncrement(&g_fatal_logged) > 3) return EXCEPTION_CONTINUE_SEARCH;
  void *frames[62];
  USHORT n = RtlCaptureStackBackTrace(0, 62, frames, NULL);
  char where[128];
  describe_address((void *) ep->ContextRecord->Rip, where, sizeof where);
  logf_("FATAL? exception 0x%08lx at %s (rip), thread %lu, %u frames:", (unsigned long) code, where,
        (unsigned long) GetCurrentThreadId(), (unsigned) n);
  for (USHORT i = 0; i < n; i++) {
    describe_address(frames[i], where, sizeof where);
    logf_("   #%02u %s", (unsigned) i, where);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}
static void install_failfast_logger(void)
{
  if (AddVectoredExceptionHandler(1, fatal_exception_logger)) logf_("diag: fatal exception logger installed");
}

/* Watchdog (diag thread): a checkpoint save / load that lasts more than 4 s is probably stuck: log the stacks
 * of the busy thread and of the main thread once.  The thread is suspended only while its frames are copied
 * (no logging, no allocation while it is suspended). */
static int capture_stack(DWORD tid, void **frames, int cap)
{
  HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
  if (h == NULL) return 0;
  int n = 0;
  __attribute__((aligned(16))) CONTEXT ctx;
  memset(&ctx, 0, sizeof ctx);
  ctx.ContextFlags = CONTEXT_FULL;
  if (SuspendThread(h) != (DWORD) -1) {
    if (GetThreadContext(h, &ctx)) {
      while (n < cap && ctx.Rip != 0) {
        frames[n++] = (void *) ctx.Rip;
        DWORD64 image = 0;
        PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(ctx.Rip, &image, NULL);
        if (rf == NULL) {                                   /* leaf or code without unwind data */
          DWORD64 ret = 0;
          if (!safe_read((void *) ctx.Rsp, &ret, sizeof ret)) break;
          ctx.Rip = ret;
          ctx.Rsp += 8;
        } else {
          void *handler_data = NULL;
          DWORD64 frame = 0;
          RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, rf, &ctx, &handler_data, &frame, NULL);
        }
      }
    }
    ResumeThread(h);
  }
  CloseHandle(h);
  return n;
}
static void log_stack(const char *who, DWORD tid)
{
  void *frames[40];
  int n = capture_stack(tid, frames, 40);
  char where[128];
  logf_("WATCHDOG: %s thread %lu, %d frames:", who, (unsigned long) tid, n);
  for (int i = 0; i < n; i++) {
    describe_address(frames[i], where, sizeof where);
    logf_("   #%02d %s", i, where);
  }
}
static ULONGLONG g_watchdog_done;
static void watchdog_check(void)
{
  ULONGLONG since = g_busy_since;
  if (since == 0 || since == g_watchdog_done || GetTickCount64() - since < 4000) return;
  g_watchdog_done = since;
  INT32 clock = 0;
  if (g_quant_control) safe_read(g_quant_control + 0x48, &clock, sizeof clock);
  logf_("WATCHDOG: %s running for %llu ms (quant clock %d)", g_busy_what, (unsigned long long) (GetTickCount64() - since), (int) clock);
  DWORD busy = g_busy_tid;
  log_stack("busy", busy);
  if (g_main_tid != 0 && g_main_tid != busy) log_stack("main", g_main_tid);
}

static DWORD WINAPI watchdog_thread(LPVOID unused)
{
  (void) unused;
  for (;;) { Sleep(1000); watchdog_check(); }
  return 0;
}

/* ------------------------------------------------------------------ mission patch at load time */
/*
 * The Save / Reload buttons are added to the mission text when the game reads it, so they work on top of any
 * mod that replaces the missions (the data mod only ships icons and texts).  Every SDL text file (.mi, .set,
 * .def ...) goes through eSDLReader (client 0x66a9f0, same code in the server): it reads the whole file into the
 * std::string at source+0x98 (assign(size + 1, 0) at 0x66bcc0, then the stream read) and then calls the
 * lexer init (0x66b290); the file path is the std::string at source+0xc0.  The init call is redirected: for
 * every .mi under map/single/ the text is replaced by mp_patch() of it (same output as the old generated data mod),
 * using the game's own assign() so the game frees it with its own allocator.  Every peer patches the same file
 * the same way, so the coop scripts stay identical.  Checkpoint archives (mission.scn) are never touched.
 *
 * Multiplayer only: the dedicated server (it only exists in network games) always patches; the game client
 * patches only while a network session exists (is_network_session: the session global != 0, the same test
 * that turns {"autosave"} off in coop), so single player missions have no Save / Reload buttons.  A coop
 * client reads the mission after joining, so it and the server patch the same missions.
 */
#include "mission_patch.h"

typedef unsigned long long (*LexInitFn)(void *source, void *arg);
typedef void *(*StrAssignFn)(void *str, unsigned long long n, char ch);
static LexInitFn g_lex_init;
static StrAssignFn g_str_assign;
static BoolFn g_patch_is_network;   /* client: is_network_session; NULL on the server = always patch */
static int g_patch_client;
static volatile LONG g_missions_patched, g_paths_logged;

static int is_mission_path(const char *p)
{
  char low[512];
  size_t n = strlen(p);
  if (n >= sizeof low) return 0;
  for (size_t i = 0; i <= n; i++) {
    char c = p[i];
    low[i] = (char) (c == '\\' ? '/' : (c >= 'A' && c <= 'Z' ? c + 32 : c));
  }
  return n > 3 && strcmp(low + n - 3, ".mi") == 0 && strstr(low, "map/single/") != NULL;
}

static unsigned long long on_sdl_source(BYTE *source, void *arg)
{
  char path[512];
  msvc_cstr((const MsvcString *) (source + 0xc0), path, sizeof path);
  if (is_mission_path(path) && g_patch_client && !g_patch_is_network()) {
    logf_("mission patch: '%s' left as is (not a network game: no Save / Reload in single player)", path);
  } else if (is_mission_path(path)) {
    MsvcString *text = (MsvcString *) (source + 0x98);
    const char *data = text->cap > 15 ? text->u.ptr : text->u.buf;
    size_t len = (size_t) text->size;
    if (len > 0 && data[len - 1] == 0) len--;        /* the reader keeps one trailing NUL in the string */
    size_t out_len = 0;
    const char *why = NULL;
    char *patched = mp_patch(data, len, &out_len, &why);
    if (patched) {
      g_str_assign(text, out_len + 1, 0);
      char *dst = text->cap > 15 ? text->u.ptr : text->u.buf;
      if ((size_t) text->size == out_len + 1) {
        memcpy(dst, patched, out_len);
        LONG n = InterlockedIncrement(&g_missions_patched);
        logf_("mission patch: '%s' %llu -> %llu bytes [#%ld]", path, (unsigned long long) len,
              (unsigned long long) out_len, (long) n);
      } else {
        logf_("ERROR: mission patch: '%s' string assign failed (size %llu)", path, (unsigned long long) text->size);
      }
      free(patched);
    } else {
      logf_("mission patch: '%s' left as is (%s)", path, why ? why : "?");
    }
  } else if (InterlockedIncrement(&g_paths_logged) <= 3) {
    logf_("mission patch: SDL file '%s' (not a mission)", path);
  }
  return g_lex_init(source, arg);
}

/* eSDLReader source ctor: mov esi,[rbp+28h]; xor r8d,r8d; sub esi,[rbp+30h]; mov rcx,rdi; lea eax,[rsi+1];
 * movsxd rdx,eax; call <assign>; cmp qword [rdi+18h],0Fh; jbe; mov rdi,[rdi]; mov r8d,esi; mov rdx,rdi;
 * mov rcx,rbp; call <stream read>; mov rdx,[rsp+60h]; mov rcx,rbx; call <lexer init> */
static const int SIG_SDL_SOURCE[] = {
  0x8B,0x75,0x28,0x45,0x33,0xC0,0x2B,0x75,0x30,0x48,0x8B,0xCF,0x8D,0x46,0x01,0x48,0x63,0xD0,0xE8,W,W,W,W,
  0x48,0x83,0x7F,0x18,0x0F,0x76,0x03,0x48,0x8B,0x3F,0x44,0x8B,0xC6,0x48,0x8B,0xD7,0x48,0x8B,0xCD,0xE8,W,W,W,W,
  0x48,0x8B,0x54,0x24,0x60,0x48,0x8B,0xCB,0xE8 };
static void install_mission_patch(int kind)
{
  BYTE *at = NULL;
  if (kind == GOH_GAME) {
    /* is_network_session = the first call of eCmd_autosave::execute */
    BYTE *fn = NULL;
    int k = scan(SIG_AUTOSAVE_EXEC, COUNT(SIG_AUTOSAVE_EXEC), &fn);
    if (k != 1) { logf_("ERROR: mission patch: autosave command signature found %d times: buttons disabled", k); return; }
    g_patch_is_network = (BoolFn) (fn + 0x16 + *(INT32 *) (fn + 0x12));
    g_patch_client = 1;
  }
  int n = scan(SIG_SDL_SOURCE, COUNT(SIG_SDL_SOURCE), &at);
  if (n != 1) { logf_("ERROR: mission patch: SDL reader signature found %d times: buttons disabled", n); return; }
  BYTE *assign_call = at + 0x12, *init_call = at + 0x37;
  g_str_assign = (StrAssignFn) (assign_call + 5 + *(INT32 *) (assign_call + 1));
  g_lex_init = (LexInitFn) (init_call + 5 + *(INT32 *) (init_call + 1));
  BYTE *stub = alloc_near_module(0x40);
  if (stub == NULL) { logf_("ERROR: mission patch: no memory near the module"); return; }
  stub[0] = 0xFF; stub[1] = 0x25; *(INT32 *) (stub + 2) = 0; *(void **) (stub + 6) = (void *) on_sdl_source;
  FlushInstructionCache(GetCurrentProcess(), stub, 0x40);
  BYTE c[5] = { 0xE8, 0, 0, 0, 0 };
  *(INT32 *) (c + 1) = rel32(init_call + 5, stub);
  if (!write_code(init_call, c, 5)) { logf_("ERROR: mission patch: cannot patch the SDL reader"); return; }
  logf_("mission patch: SDL reader hooked at +0x%llx (assign +0x%llx, lexer init +0x%llx, %s)",
        (unsigned long long) (init_call - g_base), (unsigned long long) ((BYTE *) g_str_assign - g_base),
        (unsigned long long) ((BYTE *) g_lex_init - g_base), g_patch_client ? "network games only" : "always");
  if (g_patch_client)
    logf_("mission patch: is_network_session +0x%llx", (unsigned long long) ((BYTE *) g_patch_is_network - g_base));
}

static void mod_start(int kind)
{
  install_mission_patch(kind);
  if (kind == GOH_GAME) {
    install_checkpoint("coop_ckpt");
    install_save_render_skip();
    install_reload("client", 0);
    install_entity_map_fix();
    install_auto_restart();
  } else {
    /* the dedicated server has no renderer: no save screenshot at all */
    if (install_checkpoint("coop_ckpt_srv")) { disable_save_screenshot(); disable_screenshot_view(); g_replicate = find_replication(); }
    install_reload("server", 1);
    install_entity_map_fix();
  }
  install_failfast_logger();
  HANDLE th = CreateThread(NULL, 0, watchdog_thread, NULL, 0, NULL);
  if (th) CloseHandle(th);
}

GOH_MOD_ENTRY()
