# Loot Highlight

The game's **corpses highlight** key (**V** by default, `interface/corpses_highlight`) also highlights
**lootable containers**:

- crates, boxes and other static inventories that hold items;
- vehicles and guns with items in their inventory, except your own.

Highlighted containers get the outline and the **examine** cursor, so you can open them straight from the V
mode. Empty containers and objects hidden in the fog of war aren't highlighted.

Client-side and purely visual: works in single player and in multiplayer, causes no desync, and the other
players don't need it.

## Install

Extract `goh-loot-highlight-v*.zip` into the game folder (see the [main README](../../README.md)). Files:
`binaries\x64\version.dll`, `binaries\x64\goh_mods\loot_highlight.dll`.

## How it works

Found by reverse engineering, client v1.067:

- **Highlight decision** (per entity, client 0xa2d2d0): the game marks an entity as "stuff" only if it has an
  `Entity::eStuffExtender`. Right after that decision a stub asks `loot_container_check()` (`common/goh_loot.h`).
  The check: V active (manager +0x2b2), not in fog, not a human (corpses have their own highlight), a non-empty
  inventory box (`entity+0x578 → +0x28 Inventory::eManager → +0xa0 Inventory::eBox`, items at +0x10..+0x18),
  and for actors not owned by the local player. A container gets the stuff highlight.
- **Cursor pick** (GameControl 0xaf3340): with V active the game runs a corpses-only query, so a crate is never
  hovered. The call is redirected. When the V pick finds nothing, the pick runs again as if no highlight key were
  pressed, and only a loot container found that way is kept.
- **V held down**: the game sometimes receives a release of V while the key is still physically down, which
  turns the mode off. Releases are ignored while `GetAsyncKeyState` reports the key down. This fix is shared with
  Inspect Preview and installed once.

Log: `%TEMP%\goh_loot_highlight_call_to_arms.exe.log` (lists the first highlighted containers).
