# Build the Gizmos & Gadgets recompilation.
#   scripts\build.ps1        then  work\gizmos.exe original\SSGWINCD\SSGWIN32.EXE
#   scripts\build.ps1 -Trace       adds the function-entry ring tracer
param([switch]$Trace)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

if (-not (Test-Path "src\recomp\gen\recomp_dispatch.c")) {
    Write-Output "No lifted sources in src\recomp\gen\. Generate them from your own copy first:"
    Write-Output "  python tools\run_pipeline.py original\SSGWINCD\SSGWIN32.EXE --all --output src\recomp\gen --stubs src\recomp\imports_stub.c"
    exit 1
}

$srcs = @(
    "src\engine\main.c",
    "src\engine\recomp_runtime.c",
    "src\engine\image_loader.c",
    "src\engine\premap.c",
    "src\engine\iat_bridge.c",
    "src\engine\audio.c"
)
$srcs += (Get-ChildItem "src\recomp\gen\recomp_*.c" | ForEach-Object { $_.FullName })

# MSVC. $env:VCVARS overrides it; otherwise take the first install that is
# there, so this is not pinned to one edition on one machine.
$vcvars = $env:VCVARS
if (-not $vcvars) {
    $vcvars = @("Enterprise","Professional","Community","BuildTools") |
        ForEach-Object { "${env:ProgramFiles}\Microsoft Visual Studio\2022\$_\VC\Auxiliary\Build\vcvars64.bat" } |
        Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $vcvars) { Write-Output "no vcvars64.bat found; set `$env:VCVARS"; exit 1 }

# /O1 keeps the 189k-line build inside a couple of minutes; /W0 because
# mechanically-translated C trips every conversion warning MSVC has.
$opts = "/nologo /O1 /W0 /bigobj /I src\engine /I src\recomp\gen"
if ($Trace) { $opts += " /DRECOMP_TRACE=1" }

# The image has to land at 0x400000, so this exe is linked out of the way and
# keeps a small stack and heap -- either one, placed by the loader, will take
# the range before premap can reserve it.
$link = "/link /BASE:0x70000000 /DYNAMICBASE:NO /STACK:1048576 /HEAP:4096,4096 user32.lib gdi32.lib winmm.lib psapi.lib ole32.lib"

New-Item -ItemType Directory -Force work\obj | Out-Null
$cl = "cl $opts " + ($srcs -join " ") + " /Fe:work\gizmos.exe /Fo:work\obj\ $link"
cmd /c "`"$vcvars`" >nul 2>&1 && $cl"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Output "built work\gizmos.exe"
