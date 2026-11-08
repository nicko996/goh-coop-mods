# Coop Checkpoint

Mid-mission saves in coop. Vanilla coop has no saving at all: if the mission goes wrong you start over.

- **Save**: every PC saves the current battle state at the same simulation step. A new checkpoint
  overwrites the previous one. The mission's own autosaves become checkpoints too.
- **Reload**: the host's game leaves the match and starts the same mission again automatically, and
  every PC loads the checkpoint instead of the mission start, with units, triggers and objectives restored.

Checkpoints live only for the current mission session: they're meant for "retry from here", not for saving
and continuing another day.

**Every player in the match needs it**: the DLL **and** the `coop_checkpoint` data mod enabled (the mod list
must match anyway to join).

## Install

1. Extract `goh-coop-checkpoint-v*.zip` into the game folder (see the [main README](../../README.md)). Files:
   `binaries\x64\version.dll`, `binaries\x64\goh_mods\coop_checkpoint.dll`, `mods\coop_checkpoint\`.
2. In the game: *Options › Mods* → enable **coop_checkpoint**.

Works with other mods that change the missions (for example a new skirmish doctrine), in any mod-list order:
the buttons are added to whatever mission the game loads.

## Use

In a **multiplayer** (coop) campaign or skirmish mission open the **reinforcement panel**. There are two new
buttons (single-player missions don't get them):

- **Save** (10 s cooldown).
- **Reload**. Any player can press it. After about 2 s the host's game leaves the match, waits for
  the lobby and starts the same mission again. Every PC then loads its own checkpoint.

The saved files are in `Documents\My Games\gates of hell\profiles\<id>\save\`: `coop_ckpt.sav` (each player)
and `coop_ckpt_srv.sav` (the host's local server). A reload request is armed in
`%TEMP%\coop_checkpoint_reload_<client|server>.txt` and expires after 15 minutes.

## How it works

Found by reverse engineering, game v1.067. Every peer of the lockstep simulation (each player's game and the
host's local dedicated server) runs the DLL.

**Save**
- The data mod's button runs the mission-script command `{"autosave"}`. Its `eCmd_autosave::execute` starts
  with an `is_network_session` check, so in coop it does nothing. The DLL hooks it.
- In coop the mission script runs only on the dedicated server. When the command runs there, the server's DLL
  calls `Save::CreateSave` synchronously inside the quant, on the simulation thread. It builds the save
  manager first, because the server never builds it. It then replicates the command to the clients as an
  `eSceneActionCommand` (net id 0x67). Each client runs it at the same point of its own command stream and
  saves too.
- The client's save screenshot renders with D3D on the calling thread, which can hang on the simulation
  thread, so it's skipped during checkpoints. The server has no renderer at all.
- On the server, trigger actions are written only if the "currently executing script thread" global is
  cleared during the save, so the DLL clears it for the duration.

**Reload**
- **Reload** runs `{"achievement" {name rus_skirmish1_easy} {nation coop_ckpt_reload}}`. The game
  forwards achievement commands to every client and unlocks nothing in a network game. The DLL recognises
  the `nation` string and writes the marker file.
- The host's game then leaves the match (the confirm handler of *close game* / *abandon campaign*) and presses
  the lobby's start button itself.
- At the next start of that mission the scene loader is hooked: it builds the load paths from the checkpoint
  archive exactly like *Load game* does.
- Fixes a reloaded match needs:
  - **Quant clock:** every peer puts its clock back to the same whole second, and the server's master quant
    scheduler starts there. Otherwise clients would drop the server's commands as "late".
  - **Player ids:** the game gives the humans the ids after the highest unit owner, so each reload moved them
    up by the number of players (4,5 → 6,7 → 8,9, max 18). The DLL records the ids at a normal start and maps
    the owners back while a checkpoint loads.
  - **Unit control:** after a reload the data mod's script re-runs `autoassign` for the coop player slots.
  - **Entity map:** after a reload some entity ids are missing from the game's id → entity map. Unregistering
    one would erase the map's head node (heap corruption `c0000374` when leaving the match), so that erase is
    skipped.

## Known limitations

- A script sequence that was halfway through when the checkpoint was saved doesn't resume after a reload.
- Checkpoints are session-only: the files stay on disk, but loading them is only automatic through
  **Reload** in the same mission.
- All players need the same DLL version and the same data mod.
- Diagnostics in the log: a watchdog logs the stacks if a checkpoint save or load takes more than 4 s (loading a
  big mission can legitimately take 10–15 s), and a fatal-exception logger records heap corruption.

## Files

- `coop_checkpoint.c`: the DLL.
- `mission_patch.h`: the mission patch the DLL applies in memory to every `map/single/**/*.mi` it reads (buttons,
  events declared in `{Helpers}`, triggers, the mission's own `{"autosave"}` calls). The hook sits in the game's
  SDL text reader (`eSDLReader`, after the whole file is read into a string and before the lexer starts). The
  dedicated server always patches; the game client only while a network session exists.
- `data/make_coop_checkpoint_mod.py`: generates the `coop_checkpoint` data mod (localisation in English and Italian,
  button icons). Its `patch()` is the reference for `mission_patch.h` (`tools/test_mission_patch.py`).
- Logs: `%TEMP%\goh_coop_checkpoint_call_to_arms.exe.log`, `%TEMP%\goh_coop_checkpoint_call_to_arms_server.exe.log`.
