# Build one title's recompilation.
#
#   scripts\build.ps1 gizmos            then  titles\gizmos\work\gizmos.exe
#   scripts\build.ps1 mathstorm -Trace        adds the function-entry ring tracer
#
# Every title here is the same engine -- Borland C++ PE32 for Win32s, drawing
# through WinG -- so engine\ is compiled once per title alongside that title's
# lifted code, and the bridge table binds whichever imports the binary in front
# of it actually has. A fix found in one game is a fix in all of them.
param(
    [Parameter(Mandatory = $true, Position = 0)] [string]$Title,
    [switch]$Trace
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

$dir = "titles\$Title"
if (-not (Test-Path "$dir\title.json")) {
    Write-Output "no such title: $Title"
    Write-Output ("known: " + ((Get-ChildItem titles -Directory).Name -join ", "))
    exit 1
}

if (-not (Test-Path "$dir\gen\recomp_dispatch.c")) {
    Write-Output "No lifted sources in $dir\gen\. Generate them from your own copy first:"
    Write-Output "  python scripts\run_pipeline.py $Title"
    exit 1
}

$srcs = @(
    "engine\main.c",
    "engine\recomp_runtime.c",
    "engine\image_loader.c",
    "engine\premap.c",
    "engine\iat_bridge.c",
    "engine\audio.c"
)
$srcs += (Get-ChildItem "$dir\gen\recomp_*.c" | ForEach-Object { $_.FullName })

# MSVC. $env:VCVARS overrides it; otherwise take the first install that is
# there, so this is not pinned to one edition on one machine.
$vcvars = $env:VCVARS
if (-not $vcvars) {
    $vcvars = @("Enterprise","Professional","Community","BuildTools") |
        ForEach-Object { "${env:ProgramFiles}\Microsoft Visual Studio\2022\$_\VC\Auxiliary\Build\vcvars64.bat" } |
        Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $vcvars) { Write-Output "no vcvars64.bat found; set `$env:VCVARS"; exit 1 }

# /O1 keeps a quarter-million lines inside a couple of minutes; /W0 because
# mechanically-translated C trips every conversion warning MSVC has.
$opts = "/nologo /O1 /W0 /bigobj /I engine /I $dir\gen"
if ($Trace) { $opts += " /DRECOMP_TRACE=1" }

# The image has to land at 0x400000, so this exe is linked out of the way and
# keeps a small stack and heap -- either one, placed by the loader, will take
# the range before premap can reserve it.
$link = "/link /BASE:0x70000000 /DYNAMICBASE:NO /STACK:1048576 /HEAP:4096,4096 " +
        "user32.lib gdi32.lib winmm.lib psapi.lib ole32.lib"

New-Item -ItemType Directory -Force "$dir\work\obj" | Out-Null
# A previous run still holding the exe fails the link with LNK1104 on a build
# that is otherwise perfectly fine.
Get-Process $Title -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

$cl = "cl $opts " + ($srcs -join " ") + " /Fe:$dir\work\$Title.exe /Fo:$dir\work\obj\ $link"
cmd /c "`"$vcvars`" >nul 2>&1 && $cl"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Output "built $dir\work\$Title.exe"
