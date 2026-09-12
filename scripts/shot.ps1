# Run the recompiled game and photograph its window, once or repeatedly.
#
#   scripts\shot.ps1                          one shot after 6s -> work\shot.png
#   scripts\shot.ps1 -At 5,20,60 -Out work\s  three shots in ONE run -> work\s5.png ...
#   scripts\shot.ps1 -At 30 -Clicks 10,20     left-click the centre at 10s and 20s
#   scripts\shot.ps1 -Clicks "12@262,297"     click a point (client coords)
#   scripts\shot.ps1 -Type "20@ALEX"          type text, then Enter
#   scripts\shot.ps1 -Keys "30@39x40"         hold VK 39 (right arrow) for 40 repeats
#   scripts\shot.ps1 -Loud                    leave the sound on (muted by default)
#
# Shots are of the client area, so a pixel in one is a coordinate you can click.
param(
    [int]$Seconds = 6,
    [int[]]$At,
    [string[]]$Clicks,
    [string[]]$Type,
    [string[]]$Keys,
    [string]$Out = "work\shot.png",
    [switch]$Loud
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

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
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr PostMessageA(IntPtr h, uint m, IntPtr w, IntPtr l);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
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
# and keep the first top-level window belonging to any gizmos.exe.
function Get-GameWindow {
    $ids = @(Get-Process gizmos -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
    $script:found = [IntPtr]::Zero
    $cb = [Shot+EnumProc]{
        param($h, $lp)
        $wpid = 0
        [void][Shot]::GetWindowThreadProcessId($h, [ref]$wpid)
        if ($ids -contains $wpid -and $script:found -eq [IntPtr]::Zero) { $script:found = $h }
        return $true
    }
    [void][Shot]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:found
}

function Save-Shot([IntPtr]$hwnd, [string]$path) {
    # The CLIENT area, not the whole window: the game blits its 640x400 picture
    # to client (0,0), so a pixel in the image is exactly a coordinate to click.
    $c = New-Object Shot+RECT
    [void][Shot]::GetClientRect($hwnd, [ref]$c)
    $w = $c.Right; $h = $c.Bottom
    if ($w -le 0 -or $h -le 0) { Write-Output "empty window"; return }

    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [void][Shot]::PrintWindow($hwnd, $hdc, 1)     # PW_CLIENTONLY
    $g.ReleaseHdc($hdc)
    $g.Dispose()

    $bmp.Save((Join-Path $root $path), [System.Drawing.Imaging.ImageFormat]::Png)
    Write-Output "$path  ${w}x${h} (client)"
    $bmp.Dispose()
}


$p = Start-Process -FilePath "work\gizmos.exe" `
                   -ArgumentList "original\SSGWINCD\SSGWIN32.EXE" `
                   -PassThru -RedirectStandardError "work\shot.log" -RedirectStandardOutput "work\shot.out"



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

$p | Stop-Process -Force -ErrorAction SilentlyContinue
