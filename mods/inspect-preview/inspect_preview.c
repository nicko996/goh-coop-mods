/*
 * Inspect Preview - Gates of Hell mod DLL (goh_mods\inspect_preview.dll)
 * =======================================================================
 *
 * While the corpses highlight key (V) is active and the cursor hovers a corpse with items or a lootable
 * container, the container's inventory grid is shown, as in the exchange screen but look only: nothing can
 * be dragged, nothing is sent to the simulation.  It works without a selected unit.
 * Corpses work on their own; crates and vehicles can be hovered in the V mode only with Loot Highlight.
 *
 * Client only, purely UI: no desync, the other players do not need it.
 *
 * The game's exchange dialog GameControl::eInventoryExchange ("inventory_exchange") is created directly
 * (0xa78e10, GUI parent 0x2691b0) instead of through the exchange begin event: W+0x208 unit, +0x210 target,
 * +0x21c (exchange active) and +0x21d stay 0, so nothing is sent to the simulation and closing it (vfunc 62,
 * 0xb33e80) sends no "end examination" operation.  The build (0xb34540) runs with its camera call (0xa28350, it
 * moves the camera to the unit) skipped; the unit's bar (W+0x1f8) and the button panel (returned by 0xa9d1e0
 * during the build) are hidden, leaving the target's bar (W+0x200).  Driven by eGameControl vfunc 13 (0xaf1720,
 * cursor/hover update, main thread), after the hovered entity (GameControl+0xe8) is known.  A real exchange in
 * progress (GameControl+0x598 [local player] +0x10) suppresses the preview.  The grid, its bar and the dialog
 * are disabled after the build (look only: no drag & drop out of the preview).
 * Creating the dialog activates it (vfunc 7, 0xaa3cb0): activation releases every mouse button (0x4c94a0) and
 * every key (0x4c5540) still held and broadcasts the event 0x412, whose listener clears GameControl's camera key
 * bits (0xb01d80): the keyboard camera, rotation with the middle button and V stopped until pressed again.  The
 * preview is look only and needs no focus: its activation is skipped (exchange vtable slot 7 while creating), and
 * so is the deactivation the close does (0xa9f9c0: flag +0x48 bit 3 set -> cleared + vfunc 7), same broadcast.
 *
 * Log: %TEMP%\goh_inspect_preview_<exe name>.log
 */
#define GOH_MOD_NAME "inspect_preview"
#define GOH_MOD_VERSION "1.0.3"   /* this mod's own version (release tag <mod-id>-v<version>) */
#include "../../common/goh_loot.h"

typedef void *(*GetterFn)(void);
typedef void *(*MethodFn)(void *obj);
typedef void *(*DeleteFn)(void *obj, unsigned flags);
typedef void *(*CreateDialogFn)(void *name, void *parent);
typedef void (*CameraFn)(void *camera, void *pos);
typedef void *(*LayoutFn)(void *dialog, void *path, void *arg);
typedef struct { const char *ptr; UINT64 pad, size, cap; } MsvcHeapString;
static MethodFn g_gc_tick_orig, g_exchange_close_orig, g_selected_unit_fn;
static DeleteFn g_exchange_delete_orig;
static GetterFn g_gui_parent;
static CreateDialogFn g_create_dialog;
static MethodFn g_exchange_build;
static CameraFn g_build_camera_orig;
static LayoutFn g_build_layout_orig;
static void *g_exchange_vtable;
static int g_building, g_creating;
static BYTE *g_closing;   /* preview dialog being closed */
static MethodFn g_exchange_activate_orig;
static LONG g_activate_logged;
static BYTE *g_build_layout;
static BYTE *g_preview, *g_preview_target, *g_preview_buttons;
static int g_preview_failures;
static LONG g_preview_logged;
static ULONGLONG g_preview_tick;     /* last open / close of the preview */
static void show_widget(BYTE *w, int show)
{
  if (w == NULL) return;
  UINT32 *flags = (UINT32 *) (w + 0x48);
  if (((*flags & 2) != 0) != (show != 0)) { *flags ^= 2u; ((MethodFn) (*(void ***) w)[5])(w); }
}
/* widget flags +0x48: bit 1 visible (vfunc +0x28), bit 2 enabled / interactive (vfunc +0x30) */
static void enable_widget(BYTE *w, int enable)
{
  if (w == NULL) return;
  UINT32 *flags = (UINT32 *) (w + 0x48);
  if (((*flags & 4) != 0) != (enable != 0)) { *flags ^= 4u; ((MethodFn) (*(void ***) w)[6])(w); }
}
/* the preview is never activated nor deactivated: both release the held keys / buttons and stop the keyboard camera */
static void *exchange_activate(BYTE *w)
{
  if (!g_creating && w != g_preview && w != g_closing) return g_exchange_activate_orig(w);
  if (InterlockedIncrement(&g_activate_logged) <= 3) logf_("grid preview: activation skipped (held keys and camera kept)");
  return NULL;
}
static void build_camera(void *camera, void *pos)
{
  if (!g_building) g_build_camera_orig(camera, pos);
}
static void *build_layout(void *dialog, void *path, void *arg)
{
  void *r = g_build_layout_orig(dialog, path, arg);
  if (g_building && g_build_layout == NULL) g_build_layout = (BYTE *) r;
  return r;
}
static void *exchange_close(BYTE *w)
{
  if (w != g_preview) return g_exchange_close_orig(w);
  show_widget(g_preview_buttons, 1); g_preview = NULL; g_preview_buttons = NULL; w[0x21c] = 0;
  g_closing = w;
  void *r = g_exchange_close_orig(w);
  g_closing = NULL;
  return r;
}
static void *exchange_delete(BYTE *w, unsigned flags)
{
  if (w != g_preview) return g_exchange_delete_orig(w, flags);
  show_widget(g_preview_buttons, 1); g_preview = NULL; g_preview_buttons = NULL;
  g_closing = w;
  void *r = g_exchange_delete_orig(w, flags);
  g_closing = NULL;
  return r;
}
static int preview_target_ok(BYTE *t)
{
  if (*(UINT32 *) (t + 0x30) & 4) {                     /* human: corpses only */
    BYTE *vitality = *(BYTE **) (*(BYTE **) (t + 0x578) + 0x18);
    return vitality != NULL && (vitality[0x174] & 3) != 0 && box_items(t) > 0;
  }
  return loot_target(t);
}
static void preview_close(void)
{
  BYTE *w = g_preview;
  BYTE *buttons = g_preview_buttons;
  g_preview = NULL; g_preview_target = NULL; g_preview_buttons = NULL;
  if (w == NULL) return;
  g_preview_tick = GetTickCount64();
  show_widget(buttons, 1);   /* the button panel is shared by every exchange dialog */
  w[0x21c] = 0;
  g_closing = w;
  g_exchange_close_orig(w);
  g_closing = NULL;
}
static void preview_open(BYTE *unit, BYTE *target)
{
  static const char name[] = "inventory_exchange";
  MsvcHeapString str = { name, 0, sizeof name - 1, 0x1f };
  g_creating = 1;
  BYTE *w = (BYTE *) g_create_dialog(&str, g_gui_parent());
  if (w == NULL || *(void **) w != g_exchange_vtable) {
    logf_("ERROR: grid preview: dialog %p is not an inventory exchange", (void *) w);
    g_preview_failures++;
    g_creating = 0;
    return;
  }
  *(BYTE **) (w + 0x208) = unit;
  *(BYTE **) (w + 0x210) = target;
  w[0x21c] = 0; w[0x21d] = 0;
  *(UINT32 *) (w + 0x48) |= 8;   /* "already active": the build skips activating the dialog (that ends the V mode) */
  g_building = 1; g_build_layout = NULL; g_preview_tick = GetTickCount64();
  g_exchange_build(w);
  g_building = 0; g_creating = 0;
  show_widget(*(BYTE **) (w + 0x1f8), 0);
  show_widget(g_build_layout, 0);
  /* look only: the grid must not be a source of drag & drop (items would move without an exchange) */
  BYTE *bar = *(BYTE **) (w + 0x200);
  if (bar != NULL) { enable_widget(*(BYTE **) (bar + 0x158), 0); enable_widget(bar, 0); }
  enable_widget(w, 0);
  g_preview = w; g_preview_target = target; g_preview_buttons = g_build_layout;
  g_preview_tick = GetTickCount64();
  if (InterlockedIncrement(&g_preview_logged) <= 5) {
    char tn[96];
    logf_("grid preview: %s %p (%d item(s)), dialog %p, unit bar %p, buttons %p, grid bar %p, V flag %d",
          rtti_name_of(target, tn, sizeof tn), (void *) target, box_items(target), (void *) w, *(void **) (w + 0x1f8),
          (void *) g_build_layout, *(void **) (w + 0x200), (int) (*g_hl_manager)[0x2b2]);
  }
}
static void preview_update(BYTE *gc)
{
  BYTE *target = gc[0x2b2] ? *(BYTE **) (gc + 0xe8) : NULL;
  BYTE *unit = target ? (BYTE *) g_selected_unit_fn(gc) : NULL;
  if (target != NULL && (target == unit || !preview_target_ok(target) || exam_in_progress(gc))) target = NULL;
  if (unit == NULL) unit = target;   /* nobody selected: the (hidden) unit bar shows the target too */
  if (g_preview != NULL && g_preview_target != target) preview_close();
  if (target != NULL && g_preview == NULL && g_preview_failures < 3) preview_open(unit, target);
}
static void *gc_tick(BYTE *gc)
{
  void *r = g_gc_tick_orig(gc);
  preview_update(gc);
  return r;
}
/* mov rdi,[rax]; mov rcx,r13; mov rsi,rax; call selected_unit; lea rcx,[rbp+30h]; mov rbx,rax; call local_player */
static const int SIG_SELECTED_UNIT[] = {
  0x48,0x8B,0x38,0x49,0x8B,0xCD,0x48,0x8B,0xF0,0xE8,W,W,W,W,0x48,0x8D,0x4D,0x30,0x48,0x8B,0xD8,0xE8 };
/* call gui_parent; mov ecx,20h; mov qword [rsp+28h],0; mov rbx,rax; call alloc; movups xmm0,[name]; ... size 12h;
 * the create_dialog call is at +0x53 (several dialogs match: all call the same two functions) */
static const int SIG_DIALOG_CREATE[] = {
  0xE8,W,W,W,W,0xB9,0x20,0x00,0x00,0x00,0x48,0xC7,0x44,0x24,0x28,0,0,0,0,0x48,0x8B,0xD8,0xE8,W,W,W,W,
  0x0F,0x10,0x05,W,W,W,W,0x48,0x89,0x44,0x24,0x20,0x48,0x8B,0xD3,0x48,0xC7,0x44,0x24,0x30,0x12,0,0,0 };
#define DIALOG_CREATE_CALL 0x53
/* begin handler: mov rcx,rdi; mov byte [rdi+21Ch],1; call build */
static const int SIG_EXCHANGE_BUILD_CALL[] = { 0x48,0x8B,0xCF,0xC6,0x87,0x1C,0x02,0x00,0x00,0x01,0xE8,W,W,W,W };
/* in the build: lea rcx,[camera]; add rdx,74h; call camera_to (+11) */
static const int SIG_BUILD_CAMERA[] = { 0x48,0x8D,0x0D,W,W,W,W,0x48,0x83,0xC2,0x74,0xE8,W,W,W,W,0x8B,0x47,0x48 };
/* in the build: lea r8,[rbp+67h]; mov rcx,rdi; lea rdx,[rbp+17h]; call layout (+11) */
static const int SIG_BUILD_LAYOUT[] = { 0x4C,0x8D,0x45,0x67,0x48,0x8B,0xCF,0x48,0x8D,0x55,0x17,0xE8,W,W,W,W };
static void install_grid_preview(void)
{
  if (g_hl_manager == NULL || g_is_own == NULL) return;
  BYTE *su = NULL, *dc = NULL, *bc = NULL, *cam = NULL, *lay = NULL;
  int n1 = scan(SIG_SELECTED_UNIT, COUNT(SIG_SELECTED_UNIT), &su);
  int n2 = scan(SIG_DIALOG_CREATE, COUNT(SIG_DIALOG_CREATE), &dc);
  int n3 = scan(SIG_EXCHANGE_BUILD_CALL, COUNT(SIG_EXCHANGE_BUILD_CALL), &bc);
  int n4 = scan(SIG_BUILD_CAMERA, COUNT(SIG_BUILD_CAMERA), &cam);
  int n5 = scan(SIG_BUILD_LAYOUT, COUNT(SIG_BUILD_LAYOUT), &lay);
  void **gc_vt = find_vtable(".?AVeGameControl@@");
  void **ex_vt = find_vtable(".?AUeInventoryExchange@GameControl@@");
  if (n1 != 1 || n2 < 1 || n3 != 1 || n4 != 1 || n5 != 1 || gc_vt == NULL || ex_vt == NULL || dc[DIALOG_CREATE_CALL] != 0xE8) {
    logf_("ERROR: grid preview: matches unit %d, dialog %d, build %d, camera %d, layout %d, vtables %p %p: nothing changed",
          n1, n2, n3, n4, n5, (void *) gc_vt, (void *) ex_vt);
    return;
  }
  g_selected_unit_fn = (MethodFn) call_target(su + 9);
  g_gui_parent = (GetterFn) call_target(dc);
  g_create_dialog = (CreateDialogFn) call_target(dc + DIALOG_CREATE_CALL);
  g_exchange_build = (MethodFn) call_target(bc + 10);
  g_exchange_vtable = ex_vt;
  BYTE *build = (BYTE *) g_exchange_build, *cam_call = cam + 11, *lay_call = lay + 11;
  if (cam_call < build || cam_call > build + 0x700 || lay_call < build || lay_call > build + 0x700) {
    logf_("ERROR: grid preview: camera / layout calls are not in the build function: nothing changed");
    return;
  }
  if (!redirect_call(cam_call, (void *) build_camera, (void **) &g_build_camera_orig) ||
      !redirect_call(lay_call, (void *) build_layout, (void **) &g_build_layout_orig) ||
      !replace_slot(ex_vt, 62, (void *) exchange_close, (void **) &g_exchange_close_orig) ||
      !replace_slot(ex_vt, 0, (void *) exchange_delete, (void **) &g_exchange_delete_orig) ||
      !replace_slot(ex_vt, 7, (void *) exchange_activate, (void **) &g_exchange_activate_orig) ||
      !replace_slot(gc_vt, 13, (void *) gc_tick, (void **) &g_gc_tick_orig)) {
    logf_("ERROR: grid preview: patching failed (partially installed)");
    return;
  }
  install_v_release_hold();
  logf_("grid preview: hover tick vtable +0x%llx slot 13, dialog create +0x%llx, build +0x%llx, camera call +0x%llx, layout call +0x%llx",
        (unsigned long long) ((BYTE *) gc_vt - g_base), (unsigned long long) ((BYTE *) g_create_dialog - g_base),
        (unsigned long long) (build - g_base), (unsigned long long) (cam_call - g_base),
        (unsigned long long) (lay_call - g_base));
}

static void mod_start(int kind)
{
  if (kind != GOH_GAME) return;
  if (!find_loot_globals("grid preview")) return;
  install_grid_preview();
}

GOH_MOD_ENTRY()
