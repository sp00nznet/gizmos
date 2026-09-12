# Gizmos & Gadgets! Static Recompilation

Static recompilation of **Super Solvers: Gizmos & Gadgets!** (The Learning
Company, 1993; Windows CD re-release 1996) from its shipping Win32 binary to
native C. `SSGWIN32.EXE` is a 346 KB Borland-compiled PE that holds the whole
game — the lab, the vehicle workshop, the races, the puzzles — and this project
lifts every function in it to C and answers the Win32 API it expects with a
small runtime.

No emulator. The 1996 machine code, translated once and compiled for a machine
that did not exist when it shipped.

Built on the [pcrecomp](https://github.com/sp00nznet/pcrecomp) toolchain, and
directly on the work done for
[operationneptune](https://github.com/sp00nznet/operationneptune) — same
publisher, same era, same engine. Most of this project's engine layer is
Neptune's, renamed and extended.

## Status

**Boots, loads and plays its opening music. Not yet drawing.**

The recompiled binary runs the Borland CRT startup, reaches `WinMain`, passes
the game's environment checks, creates its window, loads WinG, allocates both of
its off-screen pages, opens all 21 of its `.DAT` archives, registers its sound
window, opens `EOPEN.MID` through MCI and starts it playing. It then faults
walking one of its own in-memory data structures, before the first frame
reaches the screen.

| | |
|---|---:|
| Functions recovered | 1,998 (156 thunks) |
| Instructions decoded | 121,459 |
| Code bytes covered | 288,122 of 290,816 — **99.07%** |
| Lines of generated C | 243,310 |
| Lift errors / unsupported opcodes | 0 / 0 |
| Imports bridged | **154 of 154** |

### What works

- **Startup, end to end.** CRT init, `WinMain`, the display and sound probes,
  window creation, WinG, the data files, the sound window, MCI.
- **All 154 imports are bridged.** Nothing is unanswered and nothing is
  unimplemented — including `DialogBoxParamA`, which the game will not start
  without (see below).
- **Sound.** `MIDI\*.MID` plays through MCI. `waveOut` is wired for speech and
  effects; WaveMix and DirectSound are declined, and the game takes its own
  documented fallback for each.
- **All 21 `.DAT` archives open** from the disc, found through the game's own
  `CDDrive` INI key.

### What does not work yet

- **Nothing is on screen.** The fault below happens before the first blit.
- **The current fault** is in `sub_00404570`, a three-deep pointer walk
  (`arg+0x26 → +0x16 → [index*4]`) through a structure loaded from a `.DAT`.
  The index it reads back is two bytes out of alignment, so the table it is
  indexing, or the index itself, is wrong. Not yet diagnosed.
- **Nothing is named.** All 1,998 functions are still `sub_004xxxxx`. Unlike
  Neptune, this disc ships no linker map.

## The engine underneath

Operation Neptune and Gizmos & Gadgets are the same program wearing different
art. Both are Borland C++ PE32 builds for Win32s, both draw through
**WING32.DLL**, both keep their settings in an `.INI` next to the executable,
and both read their assets out of flat archives on the CD by hand.

That is the whole point of doing this one second. Neptune took a full
engine-layer bring-up; this one reused it, and the work reduced to the
differences:

| | Operation Neptune | Gizmos & Gadgets |
|---|---|---|
| Binary | `ONWIN32.EXE`, 176 KB code | `SSGWIN32.EXE`, 284 KB code |
| Imports | 139 | 154 |
| Assets | six 16-bit NE resource DLLs | 21 flat `.DAT` archives |
| WinG | resolved **by name** | resolved **by ordinal** |
| WinG surfaces | one, 640x800 (two pages in one) | **two**, 512x384 each |
| Linker map on the disc | yes, `ONWIN.MAP` | no |
| Sound refusal | `CheckSound` INI switch | hardcoded `MIDIMAP.CFG` probe |

43 imports are new here and 28 of Neptune's are unused. The rest of the work
was four things.

### WinG, by ordinal

There is not one `WinG*` string anywhere in `SSGWIN32.EXE`. It resolves all
eight entry points by **ordinal**, and `GetProcAddress` with an ordinal passes
an integer where a string pointer goes — so dereferencing it faulted on the
very first call, asking for ordinal 3.

The ordinals are WinG 1.0's own, read off the `WING32.DLL` that ships in the
game's own `Apps\wing` redistributable. They are alphabetical, which is why
`WinGBitBlt` is 1 and `WinGCreateDC` is 3.

### Two WinG surfaces, not one

Neptune asked WinG for a single 640x800 buffer and flipped pages by blitting
from a different `y`. This game asks for two 512x384 bitmaps and keeps both
pointers, so the shim had to stop being a single global: each bitmap now gets
its own section, its own DIB section and its own 4 MB of low address space, and
`WinGGetDIBPointer` resolves which one by asking GDI what is selected into the
DC it was handed.

### The sprite blitter draws off the bottom of the screen

`sub_0040280E` clips the starting `y` against the clip rectangle on entry and
then never checks it again — the row loop runs the sprite's full height. Start a
200-row sprite at y=380 on a 384-row surface and it writes 196 rows past the
end. In 1996 those rows landed on somebody else's heap and nobody noticed.
Against a mapping of exactly the right size it is an access violation on the
first frame.

So each WinG surface carries a whole extra surface of slack at each end, which
is that worst case by construction rather than a round number.

### The MIDI driver check from 1996

The game builds `<windir>\system\midimap.cfg`, `_lopen`s it, and if that fails
tries `\system32\` too. `MIDIMAP.CFG` is the Windows 3.1 MIDI mapper
configuration and no Windows has shipped it in thirty years, so both fail, and
the game puts up dialog 3041 — *"The MIDI driver for your sound card could not
be found"* — whose only button is **Exit**.

The question it is really asking already got a yes: `midiOutGetNumDevs` and
`midiOutGetDevCapsA` run immediately before and pass straight through to a host
that has a synth. Only the file is missing, and the game never reads a byte of
it. So the probe is answered with `NUL` — a file that is always there and has
nothing in it, which is exactly what is true.

### Dialogs, for real

Neptune never needed `DialogBoxParamA`. This game will not start without one,
and with the stub returning -1 it destroyed its window and exited — that was as
far as the first run got.

Both halves turned out to be tricks the bridge already knew. The **template** is
a resource, and resources come out of the image we mapped rather than the host
module, which the resource walker already did for `LoadStringA`; a `DLGTEMPLATE`
is version-independent bytes sitting at a real address, so
`DialogBoxIndirectParamA` takes it as it stands. The **procedure** is a VA in
lifted code with no machine code behind it, so it goes through a trampoline,
exactly like the window procedure.

## Three bugs this game found in the toolchain

All three are fixed upstream in [pcrecomp](https://github.com/sp00nznet/pcrecomp)
and every project there gets them.

**`loop` was not a branch.** `disasm32.py` derived block leaders from its
conditional-jump set, and `loop`/`loope`/`loopne` were not in it. The jump was
not lost — the *label* was, so a `loop` back into the middle of its own block
compiled to a `goto` with nothing to go to. Two functions failed to compile,
which is the lucky version of this bug.

**Rotates were 32-bit and silent.** `rol`/`ror`/`rcl`/`rcr` were lifted as
32-bit regardless of operand width, so `rcr cl,1` fed the carry in at bit 31
where the write back to `CL` then dropped it. Worse, they published no flags at
all: a rotate writes CF, but the lazy flag triple still held the *previous*
instruction's operands, so a following `jae` read that instruction's carry.

This game's RLE sprite decoder says `sub bx,cx; rcr cl,1; rep movsw; jae` —
the `jae` asking *"was the count odd?"* and being answered with the subtract's
borrow. It ran the decoder off the end of both the sprite and the framebuffer.

**16-bit `push`/`pop` were lifted as 32-bit.** `push bp` / `pop bp` move `esp`
by two, and the pop writes `BP` only. Lifting them as their 32-bit cousins
balances the stack, so it looks fine — and then the pop replaces the whole of
`ebp` with a zero-extended word. `leave` handed the truncated value to `esp`,
and the next stack access was down at 64 KB.

## Building it

You need your own copy of the game. Put the CD's contents in `original/`, so
that `original/SSGWINCD/SSGWIN32.EXE` exists.

```powershell
# Lift the binary to C (about a minute)
python tools\run_pipeline.py original\SSGWINCD\SSGWIN32.EXE --all `
       --output src\recomp\gen --stubs src\recomp\imports_stub.c

# Build (MSVC; about two minutes for 243k lines)
scripts\build.ps1

# Run
work\gizmos.exe original\SSGWINCD\SSGWIN32.EXE

# Or drive it and photograph it, muted
scripts\shot.ps1 -At 5,20,60 -Out work\s
scripts\shot.ps1 -Loud            # ...with the sound on
```

`tools\run_pipeline.py` expects the pcrecomp checkout as a sibling directory
(`../tools`).

### Knobs

| Variable | What |
|---|---|
| `GG_SCREEN=w,h` | What the game is told the screen is. Default `640,480`; `0,0` passes the real desktop through. |
| `GG_VOLUME=0..100` | This process's own audio session. `0` mutes. Leaves the system master alone. |
| `GG_QUIET_BRIDGES=1` | Stop logging every bridged call. |
| `GG_QUIET_FILES=1` | Stop logging every file the game opens. |
| `GG_NO_DIALOGS=1` | Answer message boxes with OK instead of blocking. |
| `GG_WATCHDOG_MS=n` | Dump the trace and bail if nothing has happened for `n` ms. |

## Layout

```
original/     your copy of the CD (not in this repo)
src/engine/   the runtime: memory model, IAT bridges, WinG shim, audio
src/recomp/   generated C (not in this repo -- regenerate it)
tools/        run_pipeline.py, the lift driver
scripts/      build.ps1, shot.ps1
work/         scratch: logs, screenshots, the built exe
```

## The rest of the family

Gizmos & Gadgets was chosen first because it is the biggest of the Windows
Super Solvers CD re-releases, so whatever it needs the others probably need
too. The same engine shipped, at least, as:

Operation Neptune (done), Treasure Mountain!, Treasure MathStorm!, Treasure
Cove!, Treasure Galaxy!, Midnight Rescue!, OutNumbered!, Spellbound!, and
Mission T.H.I.N.K.

Each is a different `*WIN32.EXE` against the same runtime. Confirming that is
the next step, and it is the reason this project exists.

## License

**MIT** — see [LICENSE](LICENSE).

One thing the licence cannot give you: rights to the software you point this
at. Lifting a binary produces a derivative work of that binary, so the output
carries whatever licence the original does. Gizmos & Gadgets is © 1993-1996 The
Learning Company, long out of print, and the copyright most likely still
subsists somewhere down the TLC → SoftKey → Mattel → Riverdeep → HMH chain.
Bring your own disc.
