/*
 * goh_loot.h - what counts as a lootable container, shared by loot_highlight and inspect_preview
 * ===============================================================================================
 *
 * Client only, purely visual / UI: nothing here touches the simulation.
 *
 * Facts (client v1.067):
 *  - The highlight manager (global pointer, client 0x159e218) holds the key flags: +0x2b0 / +0x2b1 stuff
 *    highlight (C), +0x2b2 corpses highlight (V).  The same object is the GameControl.
 *  - Inventory of an entity: entity+0x578 -> +0x28 = Inventory::eManager, +0xa0 = Inventory::eBox, whose
 *    eItem* vector is +0x10..+0x18.
 *  - entity+0x30: bit 1 = actor (soldiers, vehicles, guns), bit 2 = human.
 *  - is_own (client 0x7fa430): the entity belongs to the local player (entity+0xda8, actors only;
 *    static entities such as crates have no owner field).
 */
#ifndef GOH_LOOT_H
#define GOH_LOOT_H

#include "goh_common.h"

/* stuff highlight state "is active": mov rax,[manager]; cmp byte [rax+2B0h],0; jne; cmp byte [rax+2B1h],0 ... */
static const int SIG_HL_FLAGS[] = {
  0x48,0x8B,0x05,W,W,W,W,0x80,0xB8,0xB0,0x02,0x00,0x00,0x00,0x75,0x0C,0x80,0xB8,0xB1,0x02,0x00,0x00,0x00,0x75,0x03,0x32,0xC0,0xC3 };
/* is the entity the local player's: entity+0xda8 == local player */
static const int SIG_IS_OWN[] = {
  0x48,0x83,0xEC,0x28,0x48,0x85,0xC9,0x74,0x26,0x48,0x89,0x5C,0x24,0x20,0x0F,0xB6,0x99,0xA8,0x0D,0x00,0x00,
  0x48,0x8D,0x4C,0x24,0x30,0xE8,W,W,W,W,0x3A,0x18 };
/* inventory getter: entity+0x578 -> +0x28 (layout check only) */
static const int SIG_INVENTORY_OF[] = {
  0x48,0x85,0xC9,0x74,0x1A,0x48,0x8B,0x81,0x78,0x05,0x00,0x00,0x48,0x8B,0x48,0x28 };

typedef char (*IsOwnFn)(void *entity);
static BYTE **g_hl_manager;
static IsOwnFn g_is_own;

/* finds the manager global and is_own; 0 (and a log line) if the game layout is not the expected one */
static GOH_UNUSED int find_loot_globals(const char *who)
{
  BYTE *flags = NULL, *own = NULL, *inv = NULL;
  int nf = scan(SIG_HL_FLAGS, COUNT(SIG_HL_FLAGS), &flags);
  int no = scan(SIG_IS_OWN, COUNT(SIG_IS_OWN), &own);
  int ni = scan(SIG_INVENTORY_OF, COUNT(SIG_INVENTORY_OF), &inv);
  if (nf != 1 || no != 1 || ni != 1) {
    logf_("ERROR: %s: matches flags %d, is_own %d, inventory %d: nothing changed", who, nf, no, ni);
    return 0;
  }
  g_hl_manager = (BYTE **) (flags + 7 + *(INT32 *) (flags + 3));
  g_is_own = (IsOwnFn) own;
  return 1;
}

/* number of items in the entity's inventory box (0 without one) */
static GOH_UNUSED int box_items(BYTE *entity)
{
  BYTE *extenders = *(BYTE **) (entity + 0x578);
  BYTE *inventory = extenders ? *(BYTE **) (extenders + 0x28) : NULL;
  BYTE *box = inventory ? *(BYTE **) (inventory + 0xa0) : NULL;
  return box ? (int) (*(void ***) (box + 0x18) - *(void ***) (box + 0x10)) : 0;
}

/* a lootable container while V is active: not hidden in the fog, not a human (corpses have their own
 * highlight), a non-empty inventory box, and, for actors (vehicles, guns), not the local player's own */
static GOH_UNUSED int loot_container_check(BYTE *entity, BYTE *actor, int hidden)
{
  if (entity == NULL || hidden) return 0;
  BYTE *manager = *g_hl_manager;
  if (manager == NULL || manager[0x2b2] == 0) return 0;
  if (*(UINT32 *) (entity + 0x30) & 4) return 0;
  if (box_items(entity) == 0) return 0;
  if (actor != NULL && g_is_own(entity)) return 0;
  return 1;
}

static GOH_UNUSED int loot_target(BYTE *target)
{
  return target != NULL && loot_container_check(target, (*(UINT32 *) (target + 0x30) & 2) ? target : NULL, 0);
}

/* ------------------------------------------------------------------ V held down */
/* V (corpses_highlight) is a hold key.  While it is held, eGameControl's input handler (client 0xaff6d4) can
 * receive a release of the binding (GameControl+0x23c, a Windows virtual key in the low byte) although the key
 * is still down (seen as soon as the cursor reaches a container): the mode switches off.  A stub asks
 * v_release_hold() first and skips any release while the key is physically down; a real release, or the UI
 * toggle button, behaves as usual.  Both loot mods want this: the first one loaded installs it (named mutex). */
static LONG g_hold_logged;
static int v_release_hold(BYTE *gc)
{
  UINT32 binding = *(UINT32 *) (gc + 0x23c);
  int vk = (int) (binding & 0xff);
  if (vk == 0 || !(GetAsyncKeyState(vk) & 0x8000)) return 0;
  if (InterlockedIncrement(&g_hold_logged) <= 3) logf_("V release ignored (key still down, binding 0x%08x)", binding);
  return 1;
}
/* input handler, release of the corpses highlight key: cmp [r14+2B2h],sil; je end; mov rax,[r14]; mov rcx,r14;
 * mov [r14+2B2h],sil (13 bytes at +13, replaced); call [rax+0A8h] */
static const int SIG_V_RELEASE[] = {
  0x41,0x38,0xB6,0xB2,0x02,0x00,0x00,0x0F,0x84,W,W,W,W,0x49,0x8B,0x06,0x49,0x8B,0xCE,0x41,0x88,0xB6,0xB2,0x02,0x00,0x00,0xFF,0x90,0xA8,0x00,0x00,0x00 };
static GOH_UNUSED void install_v_release_hold(void)
{
  wchar_t name[64];
  _snwprintf(name, 64, L"Local\\goh_v_release_hold_%lu", (unsigned long) GetCurrentProcessId());
  name[63] = 0;
  HANDLE once = CreateMutexW(NULL, FALSE, name);   /* kept open for the life of the process */
  if (once == NULL) return;
  if (GetLastError() == ERROR_ALREADY_EXISTS) { logf_("V release hold: already installed by another mod"); return; }
  BYTE *at = NULL;
  int n = scan(SIG_V_RELEASE, COUNT(SIG_V_RELEASE), &at);
  if (n != 1) { logf_("ERROR: V release site matches %d: V may switch off while it is held", n); return; }
  BYTE *site = at + 13, *back = at + 26, *end = at + 13 + *(INT32 *) (at + 9);
  BYTE *stub = alloc_near_module(0x100);
  if (stub == NULL) return;
  BYTE *p = stub;
  static const BYTE head[] = {
    0x40, 0x84, 0xF6,                                  /* test sil,sil */
    0x0F, 0x85, 0x00, 0x00, 0x00, 0x00,                /* jnz orig (rel32, patched) */
    0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53,   /* push rax,rcx,rdx,r8..r11 */
    0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00,          /* sub rsp,88h */
    0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x28, 0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x38,
    0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x48, 0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x58,
    0xF3, 0x0F, 0x7F, 0x64, 0x24, 0x68, 0xF3, 0x0F, 0x7F, 0x6C, 0x24, 0x78,
    0x49, 0x8B, 0xCE,                                  /* mov rcx,r14 (GameControl) */
    0x48, 0xB8 };                                      /* mov rax,imm64 */
  static const BYTE tail[] = {
    0xFF, 0xD0, 0x84, 0xC0,                            /* call rax; test al,al */
    0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x28, 0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x38,
    0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x48, 0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x58,
    0xF3, 0x0F, 0x6F, 0x64, 0x24, 0x68, 0xF3, 0x0F, 0x6F, 0x6C, 0x24, 0x78,
    0x48, 0x8D, 0xA4, 0x24, 0x88, 0x00, 0x00, 0x00,    /* lea rsp,[rsp+88h] (keeps the flags) */
    0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58,   /* pop r11..r8,rdx,rcx,rax */
    0x75, 0x00 };                                      /* jnz skip (patched) */
  memcpy(p, head, sizeof head); p += sizeof head;
  void *fn = (void *) v_release_hold;
  memcpy(p, &fn, 8); p += 8;
  memcpy(p, tail, sizeof tail); p += sizeof tail;
  BYTE *jnz_skip = p - 2;
  *(INT32 *) (stub + 5) = rel32(stub + 9, p);          /* orig: */
  memcpy(p, site, 13); p += 13;                        /* mov rax,[r14]; mov rcx,r14; mov [r14+2B2h],sil */
  p = abs_jmp(p, back);
  jnz_skip[1] = (BYTE) (p - (jnz_skip + 2));           /* skip: */
  p = abs_jmp(p, end);
  FlushInstructionCache(GetCurrentProcess(), stub, 0x100);
  BYTE j[13] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
  *(INT32 *) (j + 1) = rel32(site + 5, stub);
  if (write_code(site, j, 13)) logf_("V release hold at +0x%llx", (unsigned long long) (site - g_base));
}

#endif
