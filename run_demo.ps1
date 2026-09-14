<#
  run_demo.ps1 - start (or stop) the Ramble Explorer demo mesh.

  Spins up the handoff sample mesh: 6 nodes (perception, planner, lidar-driver,
  camera-driver, controller, logger) declaring a hierarchical topic set, one node
  per process, each publishing a small payload on its topics so the explorer's live
  feed shows real messages once you subscribe. See explore/demo_scene.c.

  Usage (from the repo root):
    powershell -ExecutionPolicy Bypass -File .\explore\run_demo.ps1            # start
    powershell -ExecutionPolicy Bypass -File .\explore\run_demo.ps1 -Stop      # stop
    powershell -ExecutionPolicy Bypass -File .\explore\run_demo.ps1 -Explorer  # start + open the explorer
    powershell -ExecutionPolicy Bypass -File .\explore\run_demo.ps1 -Build     # (re)build demo_scene.exe, then start
    ... -Domain 7 -Interface 127.0.0.1

  First run pops a one-time Windows Firewall prompt for demo_scene.exe -- click Allow.
#>
[CmdletBinding()]
param(
    [int]    $Domain    = 0,
    [string] $Interface = "127.0.0.1",
    [switch] $Stop,
    [switch] $Build,
    [switch] $Explorer
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot          # repo root (parent of explore/)
$exe  = Join-Path $repo "demo_scene.exe"
$src  = Join-Path $PSScriptRoot "demo_scene.c"
$profiles = @("perception","planner","lidar-driver","camera-driver","controller","logger")

if ($Stop) {
    $p = Get-Process demo_scene -ErrorAction SilentlyContinue
    if ($p) { $p | Stop-Process -Force; Write-Host "stopped $(@($p).Count) demo node(s)" }
    else    { Write-Host "no demo nodes running" }
    return
}

# Rebuild if forced, missing or stale: a demo_scene built against an older dist/ speaks the
# old announce wire and shows as nodes found but never joining. This gcc path is self contained.
$dist  = Join-Path $repo "dist\ramble.h"
$stale = $false
if (Test-Path $exe) {
    $exeTime = (Get-Item $exe).LastWriteTime
    foreach ($dep in @($src, $dist)) {
        if ((Test-Path $dep) -and (Get-Item $dep).LastWriteTime -gt $exeTime) { $stale = $true; break }
    }
}
if ($Build -or -not (Test-Path $exe) -or $stale) {
    if (-not (Get-Command gcc -ErrorAction SilentlyContinue)) {
        throw "demo_scene.exe missing or stale and gcc (MinGW) is not on PATH to (re)build it"
    }
    Write-Host "building demo_scene.exe ..."
    & gcc -std=c99 -Wall -I"$repo\dist" "$src" -o "$exe" -lws2_32 -lbcrypt -lwinmm
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}

# restart cleanly: stop any that are already up so we don't double them
Get-Process demo_scene -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300

foreach ($name in $profiles) {
    Start-Process -FilePath $exe -ArgumentList $name,"--domain","$Domain","--if",$Interface -WindowStyle Hidden
}
Start-Sleep -Milliseconds 500
$n = @(Get-Process demo_scene -ErrorAction SilentlyContinue).Count
Write-Host "started $n demo nodes on domain $Domain ($Interface): $($profiles -join ', ')"

if ($Explorer) {
    $dexe = Join-Path $repo "Debug\ramble_explorer.exe"
    if (-not (Test-Path $dexe)) { $dexe = Join-Path $repo "ramble_explorer.exe" }
    if (Test-Path $dexe) { Start-Process -FilePath $dexe -ArgumentList "--domain","$Domain","--if",$Interface }
    else { Write-Host "explorer not built (cmake --build build --target ramble_explorer)" }
} else {
    Write-Host "open the explorer:  .\Debug\ramble_explorer.exe --domain $Domain --if $Interface"
}
Write-Host "stop them:          powershell -ExecutionPolicy Bypass -File .\explore\run_demo.ps1 -Stop"
