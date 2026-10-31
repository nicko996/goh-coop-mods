# Builds the loader (version.dll) and every mod DLL into build\
#   build\binaries\x64\version.dll
#   build\binaries\x64\goh_mods\<mod>.dll
# Requirements: Zig (https://ziglang.org, e.g. `winget install zig.zig`) on PATH.
# Optional: -Install "<game folder>" copies the result into the game.
param(
    [string]$Install = ""
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$out = Join-Path $root "build\binaries\x64"
$mods = Join-Path $out "goh_mods"
New-Item -ItemType Directory -Force $mods | Out-Null

$flags = @("-target", "x86_64-windows-gnu", "-shared", "-O2", "-s", "-Wall", "-Wextra")
$targets = @(
    @{ src = "loader\version_proxy.c";                 dll = Join-Path $out  "version.dll";         libs = @() },
    @{ src = "mods\coop-game-speed\coop_game_speed.c"; dll = Join-Path $mods "coop_game_speed.dll"; libs = @("-luser32") },
    @{ src = "mods\coop-checkpoint\coop_checkpoint.c"; dll = Join-Path $mods "coop_checkpoint.dll"; libs = @("-luser32") },
    @{ src = "mods\loot-highlight\loot_highlight.c";   dll = Join-Path $mods "loot_highlight.dll";  libs = @("-luser32") },
    @{ src = "mods\inspect-preview\inspect_preview.c"; dll = Join-Path $mods "inspect_preview.dll"; libs = @("-luser32") }
)
foreach ($t in $targets) {
    $src = Join-Path $root $t.src
    Write-Host "zig cc $($t.src)"
    & zig cc @flags -o $t.dll $src @($t.libs)
    if ($LASTEXITCODE -ne 0) { throw "build failed: $($t.src)" }
}
Remove-Item -ErrorAction SilentlyContinue (Join-Path $out "*.lib"), (Join-Path $mods "*.lib"), (Join-Path $out "*.pdb"), (Join-Path $mods "*.pdb")
Write-Host "built into $out"

if ($Install -ne "") {
    $dst = Join-Path $Install "binaries\x64"
    if (-not (Test-Path (Join-Path $dst "call_to_arms.exe"))) { throw "not a Gates of Hell folder: $Install" }
    New-Item -ItemType Directory -Force (Join-Path $dst "goh_mods") | Out-Null
    Copy-Item (Join-Path $out "version.dll") $dst -Force
    Copy-Item (Join-Path $mods "*.dll") (Join-Path $dst "goh_mods") -Force
    Write-Host "installed into $dst (all mods; delete the ones you do not want from goh_mods)"
}
