# Run one title's recompilation and photograph its window, once or repeatedly.
#
#   scripts\shot.ps1 gizmos                        one shot after 6s
#   scripts\shot.ps1 gizmos -At 20,140,165         three shots in ONE run
#   scripts\shot.ps1 gizmos -Clicks "150@415,372"  click a point (client coords)
#   scripts\shot.ps1 gizmos -Type "20@ALEX"        type text, then Enter
#   scripts\shot.ps1 gizmos -Keys "30@39x40"       hold VK 39 for 40 repeats
#   scripts\shot.ps1 mathstorm -Loud               leave the sound on
#
# Shots default to titles\<title>\work\shot.png, and are of the CLIENT area, so
# a pixel in one is a coordinate you can click.
param(
    [Parameter(Mandatory = $true, Position = 0)] [string]$Title,
    [int]$Seconds = 6,
    [int[]]$At,
    [string[]]$Clicks,
    [string[]]$Type,
    [string[]]$Keys,
    [string]$Out,
    [switch]$Loud
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

# Everything is per-title; the harness is not. One engine, one script,
# whichever of these games is in front of it.
$dir = "titles\$Title"
if (-not (Test-Path "$dir\title.json")) {
    Write-Output "no such title: $Title"
    Write-Output ("known: " + ((Get-ChildItem titles -Directory).Name -join ", "))
    exit 1
}
$cfg    = Get-Content "$dir\title.json" -Raw | ConvertFrom-Json
$Exe    = "$dir\work\$Title.exe"
$Target = Join-Path $dir ($cfg.exe -replace "/", "\\")
$log    = "$dir\work\shot.log"
if (-not $Out) { $Out = "$dir\work\shot.png" }

# The game relaunches itself, so every window and process lookup below is by
# image name rather than by the handle we hold.
$proc = $Title

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Shot {
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern IntPtr GetDC(IntPtr h);
    [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr h, IntPtr dc);
    [DllImport("gdi32.dll")] public static extern bool BitBlt(IntPtr d, int x, int y, int w, int h, IntPtr s, int sx, int sy, uint rop);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr PostMessageA(IntPtr h, uint m, IntPtr w, IntPtr l);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
"@

# Quiet by default, but leave an already-set value alone: driving the game and
# reading its full call log at the same time is exactly what chasing a crash
# needs.
if (-not $env:GG_QUIET_BRIDGES) { $env:GG_QUIET_BRIDGES = "1" }
$env:GG_NO_DIALOGS = "1"
# Silent by default: a driven run is minutes of MIDI and narration nobody
# asked for. The game sets its OWN process volume from this (see audio.c), so
# the machine's master volume is left alone. -Loud plays it.
$env:GG_VOLUME = $(if ($Loud) { "100" } else { "0" })
Remove-Item Env:GG_WATCHDOG_MS -ErrorAction SilentlyContinue

# FindWindow by class name does not work here: window classes are per-process,
# and the game runs in the relaunched child (see premap.c). Enumerate instead
# and keep the first top-level window belonging to this title's.
function Get-GameWindow {
    $ids = @(Get-Process $proc -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
    $script:found = [IntPtr]::Zero
    $cb = [Shot+EnumProc]{
        param($h, $lp)
        $wpid = 0
        [void][Shot]::GetWindowThreadProcessId($h, [ref]$wpid)
        # Not just the first window this process owns: it also creates a
        # hidden 'SoundWindow' as an MCI notification sink, and that one has a
        # client rect of -2147483648 square. Take the first VISIBLE window with
        # a real client area instead.
        if ($ids -contains $wpid -and $script:found -eq [IntPtr]::Zero) {
            $r = New-Object Shot+RECT
            if ([Shot]::IsWindowVisible($h) -and [Shot]::GetClientRect($h, [ref]$r) -and $r.Right -gt 0 -and $r.Bottom -gt 0) {
                $script:found = $h
            }
        }
        return $true
    }
    [void][Shot]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:found
}

function Save-Shot([IntPtr]$hwnd, [string]$path) {
    # Read the window's OWN DC, not the screen and not PrintWindow.
    #
    # PrintWindow asks the window to redraw itself, and this game never redraws:
    # it takes a DC with GetDC once a frame and blits its dirty rectangles
    # straight to it, outside WM_PAINT entirely. Asked to print itself it fills
    # the background and returns, so every shot came back flat grey.
    #
    # Grabbing the screen instead needs the window on top, and
    # SetForegroundWindow from a background script does not get it there -- the
    # first shot that way photographed the terminal that launched it.
    #
    # A window DC under DWM reads the window's own redirection surface, which
    # holds exactly the pixels the game blitted, occluded or not.
    $c = New-Object Shot+RECT
    [void][Shot]::GetClientRect($hwnd, [ref]$c)
    $w = $c.Right; $h = $c.Bottom
    if ($w -le 0 -or $h -le 0) { Write-Output "empty window"; return }

    $src = [Shot]::GetDC($hwnd)
    if ($src -eq [IntPtr]::Zero) { Write-Output "no window DC"; return }

    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $dst = $g.GetHdc()
    [void][Shot]::BitBlt($dst, 0, 0, $w, $h, $src, 0, 0, 0x00CC0020)   # SRCCOPY
    $g.ReleaseHdc($dst)
    $g.Dispose()
    [void][Shot]::ReleaseDC($hwnd, $src)

    $bmp.Save((Join-Path $root $path), [System.Drawing.Imaging.ImageFormat]::Png)
    Write-Output "$path  ${w}x${h} (client)"
    $bmp.Dispose()
}


Get-Process $proc -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

$p = Start-Process -FilePath $Exe -ArgumentList $Target -PassThru -RedirectStandardError $log -RedirectStandardOutput "$dir\work\shot.out"



# One timeline: every shot and every click is a moment on it, so a whole run can
# be driven and photographed without restarting the game for each frame.
$plan = @()
foreach ($t in @($At))     { if ($t) { $plan += ,@($t, "shot", "") } }
foreach ($c in @($Clicks)) {
    if (-not $c) { continue }
    $parts = "$c" -split '@'
    $plan += ,@([int]$parts[0], "click", $(if ($parts.Count -gt 1) { $parts[1] } else { "" }))
}
foreach ($k in @($Type)) {
    if (-not $k) { continue }
    $parts = "$k" -split '@'
    $plan += ,@([int]$parts[0], "type", $(if ($parts.Count -gt 1) { $parts[1] } else { "" }))
}
foreach ($k in @($Keys)) {
    if (-not $k) { continue }
    $parts = "$k" -split '@'
    $plan += ,@([int]$parts[0], "key", $(if ($parts.Count -gt 1) { $parts[1] } else { "" }))
}
if ($plan.Count -eq 0) { $plan = ,@($Seconds, "shot", "") }
$plan = $plan | Sort-Object { $_[0] }

$base = $Out -replace '\.png$', ''
$single = @($At).Count -eq 0
$elapsed = 0
foreach ($step in $plan) {
    Start-Sleep -Seconds ([Math]::Max(0, $step[0] - $elapsed))
    $elapsed = $step[0]
    $hwnd = Get-GameWindow
    if ($hwnd -eq [IntPtr]::Zero) { Write-Output "no window at ${elapsed}s"; continue }
    if ($step[1] -eq "click") {
        if ($step[2]) {
            $xy = $step[2] -split ','
            $x = [int]$xy[0]; $y = [int]$xy[1]
        } else {
            $c = New-Object Shot+RECT
            [void][Shot]::GetClientRect($hwnd, [ref]$c)
            $x = [int]($c.Right / 2); $y = [int]($c.Bottom / 2)
        }
        $lp = [IntPtr]((($y -band 0xFFFF) -shl 16) -bor ($x -band 0xFFFF))
        [void][Shot]::PostMessageA($hwnd, 0x0200, [IntPtr]0, $lp)   # WM_MOUSEMOVE
        [void][Shot]::PostMessageA($hwnd, 0x0201, [IntPtr]1, $lp)   # WM_LBUTTONDOWN
        Start-Sleep -Milliseconds 60
        [void][Shot]::PostMessageA($hwnd, 0x0202, [IntPtr]0, $lp)   # WM_LBUTTONUP
        Write-Output "click ($x,$y) at ${elapsed}s"
    } elseif ($step[1] -eq "key") {
        # "39x40" -- virtual key 39, held for 40 repeats. Games read arrow keys
        # as WM_KEYDOWN, and one press moves the sub a pixel.
        $bits = $step[2] -split 'x'
        $vk = [int]$bits[0]
        $n  = $(if ($bits.Count -gt 1) { [int]$bits[1] } else { 1 })
        for ($i = 0; $i -lt $n; $i++) {
            [void][Shot]::PostMessageA($hwnd, 0x0100, [IntPtr]$vk, [IntPtr]1)   # WM_KEYDOWN
            Start-Sleep -Milliseconds 25
        }
        [void][Shot]::PostMessageA($hwnd, 0x0101, [IntPtr]$vk, [IntPtr]1)       # WM_KEYUP
        Write-Output "key $vk x$n at ${elapsed}s"
    } elseif ($step[1] -eq "type") {
        foreach ($ch in $step[2].ToCharArray()) {
            [void][Shot]::PostMessageA($hwnd, 0x0102, [IntPtr][int][char]$ch, [IntPtr]1)  # WM_CHAR
            Start-Sleep -Milliseconds 60
        }
        [void][Shot]::PostMessageA($hwnd, 0x0102, [IntPtr]13, [IntPtr]1)                  # Enter
        Write-Output "typed '$($step[2])' at ${elapsed}s"
    } else {
        Save-Shot $hwnd $(if ($single) { $Out } else { "$base$elapsed.png" })
    }
}

# Kill the whole game, not just the launcher. The exe relaunches itself (see
# premap.c) and Stop-Process on the handle we hold leaves the CHILD running --
# which then owns the window, so the NEXT run's FindWindowA single-instance
# check finds it and the game exits before it draws anything. That reads as
# "the build broke", and it is not.
Get-Process $proc -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
