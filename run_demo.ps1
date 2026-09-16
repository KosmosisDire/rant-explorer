<#
  Start or stop the explorer demo mesh: 6 nodes (perception, planner, lidar-driver,
  camera-driver, controller, logger), one per process, each publishing a small payload
  on its topics so the live feed shows real messages. See demo_scene.c.

  powershell -ExecutionPolicy Bypass -File .\run_demo.ps1             start
  powershell -ExecutionPolicy Bypass -File .\run_demo.ps1 -Stop       stop
  powershell -ExecutionPolicy Bypass -File .\run_demo.ps1 -Explorer   start and open the explorer
  powershell -ExecutionPolicy Bypass -File .\run_demo.ps1 -Build      build first, then start
  ... -Domain 7 -Interface 127.0.0.1

  The first run asks once to allow demo_scene.exe through Windows Firewall.
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
$bin      = Join-Path $PSScriptRoot "bin"
$exe      = Join-Path $bin "demo_scene.exe"
$profiles = @("perception","planner","lidar-driver","camera-driver","controller","logger")

if ($Stop) {
    $p = Get-Process demo_scene -ErrorAction SilentlyContinue
    if ($p) { $p | Stop-Process -Force; Write-Host "stopped $(@($p).Count) demo node(s)" }
    else    { Write-Host "no demo nodes running" }
    return
}

# The build is incremental, so rebuilding keeps demo_scene on the explorer's announce wire.
if ($Build -or -not (Test-Path $exe)) {
    Push-Location $PSScriptRoot
    try {
        if (-not (Test-Path "build\windows\CMakeCache.txt")) {
            & cmake --preset windows
            if ($LASTEXITCODE -ne 0) { throw "configure failed" }
        }
        & cmake --build --preset windows
        if ($LASTEXITCODE -ne 0) { throw "build failed" }
    } finally {
        Pop-Location
    }
}

# Restart cleanly so the nodes are not doubled.
Get-Process demo_scene -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300

foreach ($name in $profiles) {
    Start-Process -FilePath $exe -ArgumentList $name,"--domain","$Domain","--if",$Interface -WindowStyle Hidden
}
Start-Sleep -Milliseconds 500
$n = @(Get-Process demo_scene -ErrorAction SilentlyContinue).Count
Write-Host "started $n demo nodes on domain $Domain ($Interface): $($profiles -join ', ')"

$explorerExe = Join-Path $bin "rant_explorer.exe"
if ($Explorer) {
    if (Test-Path $explorerExe) { Start-Process -FilePath $explorerExe -ArgumentList "--domain","$Domain","--if",$Interface }
    else { Write-Host "explorer not built (run with -Build)" }
} else {
    Write-Host "open the explorer:  .\bin\rant_explorer.exe --domain $Domain --if $Interface"
}
Write-Host "stop them:          powershell -ExecutionPolicy Bypass -File .\run_demo.ps1 -Stop"
