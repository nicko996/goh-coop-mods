/*
 * Loot Highlight - Gates of Hell mod DLL (goh_mods\loot_highlight.dll)
 * =====================================================================
 *
 * The corpses highlight key (interface/corpses_highlight, V by default) also outlines lootable containers:
 * crates and other static inventories with items, and vehicles / guns that are not the local player's.
 * They can be hovered and opened with the examine cursor while V is active.
 *
 * Client only, purely visual / UI: no desync, the other players do not need it.
 *
 * 1) Highlight decision (client 0xa2d2d0): it marks an entity as "stuff" only if it has an
 *    Entity::eStuffExtender; right after that decision (r8b) a stub asks loot_container_check() (goh_loot.h).
 *    The container gets the stuff highlight (r8b = 1).
 * 2) Cursor pick (GameControl 0xaf3340, called by 0xaf3000) fills the cursor context (+0x20 = hovered entity:
 *    cyan outline, examine cursor).  With V it runs a corpses-only query, so a crate is never hovered.  The call
 *    is redirected to loot_pick(): when the V pick finds nothing, the pick runs again as without highlight
 *    (flag cleared for the call); a loot container found that way is kept, anything else is dropped by
 *    resetting the context (0x793fb0) and redoing the V pick.
 *
 * Log: %TEMP%\goh_loot_highlight_<exe name>.log
 */
#define GOH_MOD_NAME "loot_highlight"
#include "../../common/goh_loot.h"

static void *volatile g_hl_seen[24];
static volatile LONG g_hl_seen_n;

/* called by the highlight stub: loot_container_check() + a log line for the first entities */
static int highlight_check(BYTE *entity, BYTE *actor, int hidden)
{
  if (!loot_container_check(entity, actor, hidden)) return 0;
  LONG n = g_hl_seen_n;
  if (n < COUNT(g_hl_seen)) {
    for (LONG i = 0; i < n; i++) if (g_hl_seen[i] == entity) return 1;
    n = InterlockedIncrement(&g_hl_seen_n) - 1;
    if (n < COUNT(g_hl_seen)) {
      g_hl_seen[n] = entity;
      char name[96];
      logf_("loot highlight: %s %p, %s, %d item(s)", rtti_name_of(entity, name, sizeof name), entity,
            actor ? "actor" : "static", box_items(entity));
    }
  }
  return 1;
}
/* test r15b,r15b; jne; mov r8b,1; jmp; xor r8b,r8b; cmp byte [rbp+X],0 (corpses flag, replaced); je */
static const int SIG_HL_DECISION[] = {
  0x45,0x84,0xFF,0x75,0x05,0x41,0xB0,0x01,0xEB,0x03,0x45,0x32,0xC0,0x80,0xBD,W,W,W,W,0x00,0x74 };
#define HL_DECISION_CMP 13
static void install_loot_highlight(void)
{
  BYTE *site = NULL;
  int ns = scan(SIG_HL_DECISION, COUNT(SIG_HL_DECISION), &site);
  if (ns != 1) { logf_("ERROR: loot highlight: decision matches %d: nothing changed", ns); return; }
  BYTE *cmp = site + HL_DECISION_CMP;
  BYTE *stub = alloc_near_module(0x100);
  if (stub == NULL) { logf_("ERROR: loot highlight: no memory near the module"); return; }
  static const BYTE head[] = {
    0x45, 0x84, 0xC0,                                  /* test r8b,r8b */
    0x0F, 0x85, 0, 0, 0, 0,                            /* jnz tail (patched) */
    0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53,   /* push rax,rcx,rdx,r8..r11 */
    0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00,          /* sub rsp,88h */
    0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x28, 0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x38,   /* movdqu [rsp+28h..78h],xmm0..5 */
    0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x48, 0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x58,
    0xF3, 0x0F, 0x7F, 0x64, 0x24, 0x68, 0xF3, 0x0F, 0x7F, 0x6C, 0x24, 0x78,
    0x48, 0x8B, 0xCE,                                  /* mov rcx,rsi (entity) */
    0x49, 0x8B, 0xD5,                                  /* mov rdx,r13 (actor) */
    0x45, 0x0F, 0xB6, 0xC7,                            /* movzx r8d,r15b (hidden) */
    0x48, 0xB8 };                                      /* mov rax,imm64 */
  static const BYTE tail_call[] = {
    0xFF, 0xD0,                                        /* call rax */
    0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x28, 0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x38,
    0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x48, 0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x58,
    0xF3, 0x0F, 0x6F, 0x64, 0x24, 0x68, 0xF3, 0x0F, 0x6F, 0x6C, 0x24, 0x78,
    0x48, 0x81, 0xC4, 0x88, 0x00, 0x00, 0x00,          /* add rsp,88h */
    0x88, 0x44, 0x24, 0x18,                            /* mov [rsp+18h],al: saved r8b (was 0) */
    0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58 };   /* pop r11..r8,rdx,rcx,rax */
  BYTE *p = stub;
  memcpy(p, head, sizeof head); p += sizeof head;
  void *fn = (void *) highlight_check;
  memcpy(p, &fn, 8); p += 8;
  memcpy(p, tail_call, sizeof tail_call); p += sizeof tail_call;
  *(INT32 *) (stub + 5) = rel32(stub + 9, p);           /* jnz -> tail */
  memcpy(p, cmp, 7); p += 7;                            /* original cmp byte [rbp+X],0 */
  p[0] = 0xFF; p[1] = 0x25; *(INT32 *) (p + 2) = 0; *(void **) (p + 6) = cmp + 7; p += 14;   /* jmp back (je) */
  FlushInstructionCache(GetCurrentProcess(), stub, 0x100);
  BYTE j[7] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90 };
  *(INT32 *) (j + 1) = rel32(cmp + 5, stub);
  if (write_code(cmp, j, 7))
    logf_("loot highlight: decision at +0x%llx, manager global +0x%llx, is_own +0x%llx",
          (unsigned long long) (cmp - g_base), (unsigned long long) ((BYTE *) g_hl_manager - g_base),
          (unsigned long long) ((BYTE *) g_is_own - g_base));
}

/* ------------------------------------------------------------------ cursor pick */
typedef void (*PickFn)(BYTE *gc, void *pos, BYTE *context, void *forced, void *list);
typedef void (*ContextResetFn)(BYTE *context);
static PickFn g_pick;
static ContextResetFn g_context_reset;
static void loot_pick(BYTE *gc, void *pos, BYTE *context, void *forced, void *list)
{
  g_pick(gc, pos, context, forced, list);
  if (gc[0x2b2] == 0 || gc[0x2b0] != 0 || gc[0x2b1] != 0 || *(void **) (context + 0x20) != NULL) return;
  gc[0x2b2] = 0;
  g_pick(gc, pos, context, forced, list);
  gc[0x2b2] = 1;
  if (loot_target(*(BYTE **) (context + 0x20))) return;
  g_context_reset(context);
  g_pick(gc, pos, context, forced, list);
}
/* mov r9,[rcx+3F8h]; mov [rsp+20h],rax; call pick; mov rax,[r13+3F8h] */
static const int SIG_PICK_CALL[] = {
  0x4C,0x8B,0x89,0xF8,0x03,0x00,0x00,0x48,0x89,0x44,0x24,0x20,0xE8,W,W,W,W,0x49,0x8B,0x85,0xF8,0x03,0x00,0x00 };
#define PICK_CALL 12
static const int SIG_CONTEXT_RESET[] = {
  0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x33,0xFF,0x48,0x8B,0xD9,0x89,0x39,0x33,0xD2,0xF2,0x0F,0x10,0x05 };
static void install_loot_pick(void)
{
  BYTE *site = NULL, *reset = NULL;
  int ns = scan(SIG_PICK_CALL, COUNT(SIG_PICK_CALL), &site);
  int nr = scan(SIG_CONTEXT_RESET, COUNT(SIG_CONTEXT_RESET), &reset);
  if (ns != 1 || nr != 1 || g_hl_manager == NULL) {
    logf_("ERROR: loot pick: matches pick call %d, context reset %d: nothing changed", ns, nr);
    return;
  }
  BYTE *call = site + PICK_CALL;
  g_pick = (PickFn) call_target(call);
  g_context_reset = (ContextResetFn) reset;
  BYTE *stub = alloc_near_module(0x20);
  if (stub == NULL) { logf_("ERROR: loot pick: no memory near the module"); return; }
  stub[0] = 0xFF; stub[1] = 0x25; *(INT32 *) (stub + 2) = 0; *(void **) (stub + 6) = (void *) loot_pick;   /* jmp loot_pick */
  FlushInstructionCache(GetCurrentProcess(), stub, 0x20);
  BYTE c[5] = { 0xE8, 0, 0, 0, 0 };
  *(INT32 *) (c + 1) = rel32(call + 5, stub);
  if (write_code(call, c, 5))
    logf_("loot pick: cursor pick call at +0x%llx (pick +0x%llx, context reset +0x%llx)", (unsigned long long) (call - g_base),
          (unsigned long long) ((BYTE *) g_pick - g_base), (unsigned long long) (reset - g_base));
}

static void mod_start(int kind)
{
  if (kind != GOH_GAME) return;
  if (!find_loot_globals("loot highlight")) return;
  install_loot_highlight();
  install_loot_pick();
  install_v_release_hold();
}

GOH_MOD_ENTRY()
