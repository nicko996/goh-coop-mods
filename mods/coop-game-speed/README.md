# Coop Game Speed

Changes the game speed in coop (campaign and skirmish missions played in multiplayer). In vanilla the game
speed is locked in every network game.

- **Only the host** (the player who creates the match) needs it. The other players need nothing: their games
  follow the host's speed through the game's own network protocol.
- The host presses **Backspace** during the mission to open the game's original **Game speed** menu and
  picks a speed. Everyone in the match switches to it.
- No data mod, no mod list change: it's only the DLL.

| Menu entry | Effective speed |
|---|---|
| 1 (slowest) | ×0.28 |
| 2 | ×0.56 |
| 3 | ×1 (normal) |
| 4 | ×2 |
| 5 (fastest) | ×3 (capped; the vanilla value ×3.3 is limited to ×3) |

## Install

Extract `goh-coop-game-speed-v*.zip` into the game folder (see the [main README](../../README.md)). Files:
`binaries\x64\version.dll`, `binaries\x64\goh_mods\coop_game_speed.dll`.

## How it works

Found by reverse engineering, game v1.067:

- In a multiplayer match the **dedicated server** that the host's game starts (`call_to_arms_server.exe`) is
  the master. It sends every player `ScheduleQuants` messages `{target, delta = 20 ms, amount, ratio}`. Each
  player spaces the scheduled simulation steps ("quants") in real time by `delta × ratio`, so `ratio` is a
  native pacing factor that every client honours. The server always sends 1.0. The 20 ms simulation step itself
  never changes, so there's no desync.
- **Host game** (`call_to_arms.exe`):
  - `eQuantControl::set_speed` and the Backspace handler both start with an `is_network_session` check. Both
    `jne` instructions are NOPed, so the speed menu opens and speed requests are registered in coop.
  - Every 250 ms a thread reads the most recent speed request and publishes the factor in shared memory
    `Local\CoopGameSpeed_<game pid>`.
- **Local dedicated server**, started by the host's game with `-owner <game pid>`:
  - It reads that factor.
  - The `ScheduleQuants` serializer's ratio load (`movss xmm6,[rdi+0xC]`) jumps to a stub that also multiplies
    by the factor, so all players run quants at the new pace.
  - The server's own clock runs `1/factor` as fast: its import-table entries for `QueryPerformanceCounter`,
    `GetTickCount64` and `GetTickCount` are redirected for the server module only. That way it produces quants
    at the same pace.
  - The server's own game thread receives the relayed schedule with the ratio already applied while its clock
    is already scaled, so its ratio multiplication (`mulss xmm0,[r14+0xC]`) is NOPed.

In single player nothing changes: the engine handles speed itself.

## Notes

- While the Game speed menu is open the host's game is paused, so the other players may briefly see
  "waiting for server".
- Logs: `%TEMP%\goh_coop_game_speed_call_to_arms.exe.log` (speed requests) and
  `%TEMP%\goh_coop_game_speed_call_to_arms_server.exe.log` (`game speed factor ... from host game`).
