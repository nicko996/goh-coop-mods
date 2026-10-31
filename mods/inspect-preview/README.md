# Inspect Preview

Hold **V** (the corpses highlight key) and hover a corpse or a container. A preview of its **inventory grid**
appears, the same grid as in the exchange screen, so you can see what's inside without sending a soldier to
open it.

- **Look only**: the grid is disabled, so nothing can be dragged out of it, and nothing is sent to the
  simulation.
- It works with or without a selected unit. While a real exchange is open, no preview is shown.
- **Corpses** work with this mod alone. For **crates and vehicles** also install
  [Loot Highlight](../loot-highlight/README.md), because that mod lets the V mode hover containers.

Client-side and purely UI: works in single player and in multiplayer, causes no desync, and the other players
don't need it.

## Install

Extract `goh-inspect-preview-v*.zip` into the game folder (see the [main README](../../README.md)). Files:
`binaries\x64\version.dll`, `binaries\x64\goh_mods\inspect_preview.dll`.

## How it works

Found by reverse engineering, client v1.067:

- The exchange screen is the dialog `GameControl::eInventoryExchange` ("inventory_exchange"). The mod creates
  it directly (client 0xa78e10, GUI parent 0x2691b0) instead of through the game's *exchange begin* event:
  - unit at +0x208, target at +0x210;
  - "exchange active" (+0x21c) stays 0, so closing the preview sends no *end examination* operation.
- The dialog build (0xb34540) runs with two calls redirected:
  - the camera call is skipped (it would move the camera to the unit);
  - the button panel is captured so it can be hidden.
- The unit's bar is hidden, and the target's grid, its bar and the dialog are disabled.
- The dialog is marked as already active before the build, so it doesn't take the focus. Taking the focus
  would end the V mode.
- It's driven by `eGameControl` virtual method 13 (0xaf1720, cursor/hover update, main thread), replaced in the
  vtable. The preview opens when the hovered entity (GameControl+0xe8) is a corpse with items or a loot
  container, and closes when the cursor leaves it. The dialog's close and destructor slots are wrapped, so a
  preview closed by the game itself is forgotten cleanly.
- **V held down**: the same release fix as Loot Highlight (installed once if both mods are present).

Log: `%TEMP%\goh_inspect_preview_call_to_arms.exe.log` (the first previews opened).
