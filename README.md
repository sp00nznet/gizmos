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

**It runs.** The recompiled binary plays the whole attract sequence: Morty
Maxwell's lab, animated, with its music and narration; the title card; and the
Shady Glen Technology Center sign-in screen, whose buttons respond.

![Morty Maxwell's lab, the opening scene](docs/img/intro.png)

*The opening scene, drawn by Gizmos & Gadgets' own code running natively. The
robots and Morty's arm animate; the blackboard, the lab and every sprite come
out of the game's `.DAT` archives through its own RLE decoder.*

| | |
|---|---|
| ![The title card](docs/img/title.png) | ![Sign in](docs/img/signin.png) |
| The title card, after the intro plays out | Shady Glen Technology Center: Start Game, New Player, Cancel |

### Where it works and where it doesn't

Working:

- **The intro runs** end to end -- Morty's lab, animated, then the title card,
  then sign-in -- with its MIDI score and its speech.
- **Graphics are correct.** 512x384 in 256 colours, both WinG pages, the RLE
  sprite codec, the palette, and real GDI text in the same pixels.
- **Sound.** `MIDI\*.MID` through MCI; speech and effects out through `waveOut`
  at 22 kHz. WaveMix and DirectSound are declined and the game takes its own
  documented fallback for each.
- **Input.** Mouse and keyboard reach the game's own window procedure, so the
  menus respond.
- **All 154 imports are bridged**, including `DialogBoxParamA`, which the game
  will not start without.

Not working yet:

- **No game has been played.** The attract loop runs; nobody has signed in and
  gone into the lab, so the puzzles, the vehicle workshop and the races are all
  still unvisited.
- **Almost nothing is named.** All 1,998 functions are `sub_004xxxxx`. Unlike
  Neptune, this disc ships no linker map.
- **Thirteen instructions did not lift**, all of them in blocks that look like
  data decoded as code (`int1`, `salc`, `bnd`, `insb`, `daa`). Two --
  `cmpsd` and `xlatb` -- might be real and have not been reached.

The lift itself:

| | |
|---|---:|
| Functions recovered | 1,998 (156 thunks) |
| Instructions decoded | 121,459 |
| Code bytes covered | 288,122 of 290,816 -- **99.07%** |
| Lines of generated C | 241,909 |
| Lift errors / unsupported opcodes | 0 / 13 |
| Imports bridged | **154 of 154** |

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
| WinG orientation | top-down | **bottom-up** |
| Linker map on the disc | yes, `ONWIN.MAP` | no |
| Sound refusal | `CheckSound` INI switch | hardcoded `MIDIMAP.CFG` probe |

43 imports are new here and 28 of Neptune's are unused. The rest of the work
was five things.

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

### Which way up, and who adds the offset

`WinGRecommendDIBFormat` does not just describe a bitmap: the caller reads the
sign of `biHeight` and *installs a different blitter*. Recommend top-down and
this game still chose its bottom-up set -- stride -512, row address
`base + (383 - y) * 512`, stepping `sub edi, 0x200` per row -- so it drew upside
down through a top-down DIB and the picture came out as a sheared wedge.
Bottom-up is also what real WinG answered on the 8bpp displays this shipped for.

Then, having got that right, the picture went perfectly black. Neptune's shim
returned the *topmost scanline* from `WinGGetDIBPointer`, which for a bottom-up
surface is the far end of the buffer. Neptune only ever asked for top-down
surfaces, so that branch had never run. This game's row helper computes
`base + (383 - y) * 512` for itself, so a base with `(h-1) * stride` already
added put every row a whole screen past the picture -- landing in the slack,
faulting nothing, drawing nothing, and leaving a perfectly black 512x384
rectangle with a fully populated palette. Real WinG returns the start of the
pixel data, both ways round, because it is just what `CreateDIBSection` gave it.

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

## Six bugs this game found in the toolchain

This title is a Borland build whose drawing code is ported 16-bit assembly, so
it leans on parts of the lifter that a 32-bit MSVC game never touches. All six
are fixed upstream in [pcrecomp](https://github.com/sp00nznet/pcrecomp), with
regression cases in its differential harness, and every project there gets them.

**`loop` was not a branch.** `disasm32.py` derived block leaders from its
conditional-jump set, and `loop`/`loope`/`loopne` were not in it. The jump was
not lost -- the *label* was, so a `loop` back into the middle of its own block
compiled to a `goto` with nothing to go to. Two functions failed to compile,
which is the lucky version of this bug.

**Rotates were 32-bit and silent.** `rol`/`ror`/`rcl`/`rcr` were lifted as
32-bit regardless of operand width, so `rcr cl,1` fed the carry in at bit 31
where the write back to `CL` then dropped it. And they published no flags at
all: a rotate writes CF, but the lazy flag triple still held the *previous*
instruction's operands, so a following `jae` read that instruction's carry.
The RLE sprite decoder says `sub bx,cx; rcr cl,1; rep movsw; jae` -- the `jae`
asking *"was the count odd?"* -- and answering it with the subtract's borrow ran
the decoder off the end of both the sprite and the framebuffer.

**16-bit `push`/`pop` were lifted as 32-bit.** `push bp` / `pop bp` move `esp`
by two, and the pop writes `BP` only. Lifting them as their 32-bit cousins
balances the stack, so it looks fine -- and then the pop replaces the whole of
`ebp` with a zero-extended word. `leave` handed the truncated value to `esp`,
and the next stack access was down at 64 KB.

**Every flag off a narrow operand was derived at 32 bits.** The flag tuple holds
two values and a kind, but no width: `add ax,cx` with `ax=0xFFFF` and `cx=2`
wraps at 16 bits and sets CF, and the same two numbers at 32 bits do not. In a
program whose inner loops count in `bx` and `cx`, that is most of the compares
in it. Narrow operands are now stored **left-aligned** -- shifted up so the
value's own top bit lands on bit 31 -- which makes CF, ZF, SF and OF come out
right through the existing 32-bit macros, at no runtime cost and with no change
at the hundreds of sites that pair a jcc with its setter statically.

**A shift's carry never reached its consumer.** `shl`/`shr`/`sar` recorded their
flags as a logic op, whose carry is zero, so `CMP_AE(result, result)` -- `r >= r`
-- was *always true*. The background blitter is
`shr ecx,1; rep movsw; jae skip; movsb`: copy width/2 words, then the odd byte.
With that `jae` always taken, every row of every background lost its last byte
and the picture came out as a diagonal smear. Shifts now freeze their flags into
an EFLAGS word carrying the real carry, which also retired a divergence the
harness had been carrying about shifts by zero.

**`rep stosw` was not implemented.** `stosw` and `lodsw` were simply missing from
the lifter's string-operation set, so they fell through to `UNIMPLEMENTED` and
emitted nothing -- not even the pointer advance. `rep stosw` is how a run-length
codec writes a run of one colour, which is most of a 1990s background image, so
every fill wrote no pixels and left `edi` where it was, desynchronising the rest
of the row. This was the last bug between a black screen and the picture above.

### And two in this project's own test harness

Both cost real debugging time, so they are worth writing down. The game
**relaunches itself** to claim 0x400000 (see `premap.c`), so stopping the
process handle the script holds leaves the *child* running -- which owns the
window, so the next run's `FindWindowA` single-instance check found it and the
game exited before drawing. That reads as "the build broke", and it was not.

And `PrintWindow` photographs nothing here: the game takes a DC once a frame and
blits its dirty rectangles straight to it, outside `WM_PAINT` entirely, so asked
to print itself it fills the background and returns. Every shot came back flat
grey while the real window was showing the game. Grabbing the screen instead
needs the window on top, and `SetForegroundWindow` from a background script does
not get it there -- the first attempt that way photographed the terminal that
launched it. Reading the window's own DC works, occluded or not.

## Building it

You need your own copy of the game. Put the CD's contents in `original/`, so
that `original/SSGWINCD/SSGWIN32.EXE` exists.

```powershell
# Lift the binary to C (about a minute)
python tools\run_pipeline.py original\SSGWINCD\SSGWIN32.EXE --all `
       --output src\recomp\gen --stubs src\recomp\imports_stub.c

# Build (MSVC; about two minutes for 242k lines)
scripts\build.ps1

# Run
work\gizmos.exe original\SSGWINCD\SSGWIN32.EXE

# Or drive it and photograph it, muted
scripts\shot.ps1 -At 20,140,165 -Out work\s     # intro, title, sign-in
scripts\shot.ps1 -Loud                          # ...with the sound on
scripts\shot.ps1 -At 150 -Clicks "145@415,372"   # and click a button
```

`tools\run_pipeline.py` expects the pcrecomp checkout as a sibling directory
(`../tools`).

### Knobs

| Variable | What |
|---|---|
| `GG_SCREEN=w,h` | What the game is told the screen is. Default `640,480`; `0,0` passes the real desktop through. |
| `GG_VOLUME=0..100` | This process's own audio session. `0` mutes. Leaves the system master alone. |
| `GG_TOPDOWN=1` | Recommend a top-down DIB, which puts the game on its other blitter. Bottom-up is the default and the right one. |
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
