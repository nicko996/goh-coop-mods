# Builds the loader (version.dll) and every mod DLL into build\
#   build\binaries\x64\version.dll
#   build\binaries\x64\goh_mods\<mod>.dll
#   build\gohse\mods\goh_dll_mods\...          the GOHSE bridge script (loader\gohse_bridge), copied as it is
# Requirements: Zig (https://ziglang.org, e.g. `winget install zig.zig`) on PATH.
# Optional: -Install "<game folder>" copies the result into the game.  A version.dll that is not this loader
# (GOHSE's, for example) is kept: the GOHSE bridge loads the mods then.  -ReplaceLoader overwrites it anyway.
param(
    [string]$Install = "",
    [switch]$ReplaceLoader
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
    @{ src = "mods\inspect-preview\inspect_preview.c"; dll = Join-Path $mods "inspect_preview.dll"; libs = @("-luser32") },
    @{ src = "mods\historical-doctrine\historical_doctrine.c"; dll = Join-Path $mods "historical_doctrine.dll"; libs = @("-luser32") }
)
foreach ($t in $targets) {
    $src = Join-Path $root $t.src
    Write-Host "zig cc $($t.src)"
    & zig cc @flags -o $t.dll $src @($t.libs)
    if ($LASTEXITCODE -ne 0) { throw "build failed: $($t.src)" }
}
Remove-Item -ErrorAction SilentlyContinue (Join-Path $out "*.lib"), (Join-Path $mods "*.lib"), (Join-Path $out "*.pdb"), (Join-Path $mods "*.pdb")
# GOHSE bridge: the folder GOHSE reads local script mods from
$bridgeRel = "gohse\mods\goh_dll_mods\resource\script\gohse"
$bridge = Join-Path $root "build\$bridgeRel"
New-Item -ItemType Directory -Force $bridge | Out-Null
Copy-Item (Join-Path $root "loader\gohse_bridge\mod.json"), (Join-Path $root "loader\gohse_bridge\bootstrap.lua") $bridge -Force
Write-Host "built into $out"

if ($Install -ne "") {
    $dst = Join-Path $Install "binaries\x64"
    if (-not (Test-Path (Join-Path $dst "call_to_arms.exe"))) { throw "not a Gates of Hell folder: $Install" }
    New-Item -ItemType Directory -Force (Join-Path $dst "goh_mods") | Out-Null
    # the loader, unless another tool's version.dll is installed (this loader's file contains its log title)
    $installed = Join-Path $dst "version.dll"
    $foreign = $false
    if ((Test-Path $installed) -and -not $ReplaceLoader) {
        $bytes = [System.IO.File]::ReadAllBytes($installed)
        $foreign = [System.Text.Encoding]::ASCII.GetString($bytes).IndexOf("GoH mod loader") -lt 0
    }
    if ($foreign) {
        Write-Host "note: $installed is not this loader (GOHSE?): kept. The mods are loaded by the GOHSE bridge; use -ReplaceLoader to overwrite it"
    } else {
        Copy-Item (Join-Path $out "version.dll") $dst -Force
    }
    Copy-Item (Join-Path $mods "*.dll") (Join-Path $dst "goh_mods") -Force
    $bridgeDst = Join-Path $Install $bridgeRel
    New-Item -ItemType Directory -Force $bridgeDst | Out-Null
    Copy-Item (Join-Path $bridge "*") $bridgeDst -Force
    # Historical Doctrine: mission patches next to the DLL + data mod (made by mods\historical-doctrine\data\make_hd_data.py)
    $hdp = Join-Path $mods "historical_doctrine"
    if (Test-Path $hdp) {
        $hdDst = Join-Path $dst "goh_mods\historical_doctrine"
        if (Test-Path $hdDst) { Remove-Item -Recurse -Force $hdDst }
        Copy-Item -Recurse $hdp $hdDst
    } else {
        Write-Host "note: no Historical Doctrine patches in build (run make_hd_data.py): its DLL will do nothing"
    }
    $hdData = Join-Path $root "build\data\historical_doctrine"
    if (Test-Path $hdData) {
        $modDst = Join-Path $Install "mods\historical_doctrine"
        if (Test-Path $modDst) { Remove-Item -Recurse -Force $modDst }
        Copy-Item -Recurse $hdData $modDst
    }
    Write-Host "installed into $dst (all mods; delete the ones you do not want from goh_mods)"
}
