/*
 * IAT bridge -- Win32 pass-through, plus the pieces that cannot pass through.
 *
 * Gizmos & Gadgets is a Win32 program running on Win32, so 105 of its 154
 * imports are answered by calling the same API for real. Four groups are not:
 *
 *   1. Anything returning a pointer. GlobalAlloc, GlobalLock and VirtualAlloc
 *      hand the game an address it stores in a 32-bit slot; on a 64-bit host
 *      the real return does not survive the truncation. These are served from
 *      the runtime's low heap instead.
 *   2. Anything taking a callback or a struct with pointers in it. A window
 *      procedure in the game is a VA with no machine code behind it, and
 *      WNDCLASSA / MSG / PAINTSTRUCT are laid out differently for 32- and
 *      64-bit. Those are translated field by field.
 *   3. Anything whose struct grew with the word size but has no pointers the
 *      game cares about: MEMORYSTATUS (32 bytes there, 56 here) and
 *      CRITICAL_SECTION (24 / 40). Filled in by hand, or no-oped.
 *   4. WING32.DLL. The game loads it at runtime and does most of its drawing
 *      through it -- the only pixel-moving call in the import table is
 *      StretchDIBits. Windows has not shipped WinG since the 90s, so we are
 *      WinG now. See the shim below.
 *
 * The machinery (binding by name out of the image's own import directory, the
 * esp accounting, the unbridged report) comes from the gta recomp, where the
 * lesson was learned that hand-written IAT slot addresses drift out of step.
 */

/* windows.h first: winnt.h contains inline asm ("mov eax, ...") that the
 * RECOMP_GENERATED_CODE register aliases below would rewrite. */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif

#define RECOMP_GENERATED_CODE
#include "recomp_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Audio lives in audio.c: every structure involved -- WAVEHDR and all the MCI
 * parameter blocks -- grew when Windows went 64-bit, so each is copied field
 * by field into a host-side twin there. */
void bridge_waveOutOpen(void);
void bridge_waveOutPrepareHeader(void);
void bridge_waveOutUnprepareHeader(void);
void bridge_waveOutWrite(void);
void bridge_mciSendCommandA(void);
u32  wave_lparam_to_game(uintptr_t lparam);

/*
 * GG_QUIET_* switches. Presence turns a log off, and an explicit "0" turns it
 * back on -- so a driving script can default to quiet and still be overridden
 * from the outside when something needs chasing.
 */
static int gg_quiet(const char *name) {
    char buf[8];
    DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    if (!n) return 0;
    return !(buf[0] == '0' && buf[1] == 0);
}

#define BRIDGE_BASE 0xB0000000u
#define MAX_BRIDGES 512

typedef struct {
    u32   iat_va;
    const char *name;
    void (*handler)(void);
    void *real;      /* resolved host API, for the generic pass-through */
    int   argc;      /* dwords the game pushed; the callee pops them */
} bridge_entry_t;

static bridge_entry_t bridges[MAX_BRIDGES];
static int num_bridges = 0;

/* Which bridge the dispatcher just resolved -- the generic pass-through and the
 * WinG shim both read it to tell themselves apart. */
static u32 g_bridge_hit;

static int g_verbose = 1;

/* ===================================================================
 * Binding: find each import by NAME in the image we mapped
 * =================================================================== */

static int same_import(const char *imported, const char *want) {
    if (*imported == '_' && *want != '_') imported++;
    while (*want && *imported && *imported != '@') {
        if (*want++ != *imported++) return 0;
    }
    return *want == 0 && (*imported == 0 || *imported == '@');
}

static u32 find_iat_slot(const char *want) {
    u32 nt = GG_IMAGE_BASE + MEM32(GG_IMAGE_BASE + 0x3C);
    u32 imports = MEM32(nt + 0x80);            /* DataDirectory[1].VirtualAddress */
    u32 desc;

    if (!imports) return 0;
    for (desc = GG_IMAGE_BASE + imports; MEM32(desc + 12); desc += 20) {
        u32 int_rva = MEM32(desc + 0);         /* OriginalFirstThunk */
        u32 iat_rva = MEM32(desc + 16);        /* FirstThunk         */
        u32 i;
        if (!int_rva) int_rva = iat_rva;       /* some linkers omit the INT */
        for (i = 0; ; i++) {
            u32 thunk = MEM32(GG_IMAGE_BASE + int_rva + i * 4);
            if (!thunk) break;
            if (!(thunk & 0x80000000u)) {
                const char *name = (const char *)(uintptr_t)
                                   ADDR(GG_IMAGE_BASE + thunk + 2);
                if (same_import(name, want))
                    return GG_IMAGE_BASE + iat_rva + i * 4;
            }
        }
    }
    return 0;
}

/* A callable address with no IAT slot behind it -- for the function pointers we
 * hand the game ourselves (GetProcAddress results for WinG). */
static u32 alloc_bridge(const char *name, void (*handler)(void), int argc) {
    if (num_bridges >= MAX_BRIDGES) {
        fprintf(stderr, "  BRIDGE: table full, '%s' unbridged\n", name);
        return 0;
    }
    bridges[num_bridges].iat_va  = 0;
    bridges[num_bridges].name    = name;
    bridges[num_bridges].handler = handler;
    bridges[num_bridges].real    = NULL;
    bridges[num_bridges].argc    = argc;
    return BRIDGE_BASE + num_bridges++;
}

static int g_unbound;

/*
 * Bind one import, if this binary actually has it.
 *
 * The tables here are a SUPERSET. One engine layer serves more than one title
 * out of the same house, and no two of them import the same set: Gizmos &
 * Gadgets and Treasure MathStorm agree on 95 imports and differ on 112 --
 * MathStorm drives its audio through Miles rather than waveOut and MCI, and
 * links WinG straight into its import table instead of loading it by hand.
 *
 * So a name this binary does not import is the normal case, not a warning; they
 * are counted and report_unbridged() prints the total. The line that matters is
 * the other one: an import with no bridge, which would dispatch to nothing.
 */
static void bind(const char *name, void (*handler)(void), void *real, int argc) {
    u32 iat_va = find_iat_slot(name);
    u32 addr;
    if (!iat_va) {
        g_unbound++;
        return;
    }
    addr = alloc_bridge(name, handler, argc);
    if (!addr) return;
    bridges[addr - BRIDGE_BASE].iat_va = iat_va;
    bridges[addr - BRIDGE_BASE].real   = real;
    MEM32(iat_va) = addr;
}

/*
 * The same thing for an import bound by ORDINAL.
 *
 * A DLL can be linked without names at all -- the INT holds the ordinal with
 * the high bit set and there is no string to match. MathStorm links Smacker
 * that way, nine entry points and not one name among them, so find_iat_slot()
 * cannot see them and bind() silently skips the lot.
 *
 * The ordinal alone is not enough either: ordinals are per-DLL and two modules
 * can both export a #14. So this matches the module name as well.
 */
static int same_dll(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a++, cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 0;
    }
    return *a == *b;
}

static u32 find_iat_slot_ordinal(const char *dll_want, u32 ord) {
    u32 nt = GG_IMAGE_BASE + MEM32(GG_IMAGE_BASE + 0x3C);
    u32 imports = MEM32(nt + 0x80);
    u32 desc;

    if (!imports) return 0;
    for (desc = GG_IMAGE_BASE + imports; MEM32(desc + 12); desc += 20) {
        const char *dll = (const char *)(uintptr_t)ADDR(GG_IMAGE_BASE + MEM32(desc + 12));
        u32 int_rva = MEM32(desc + 0), iat_rva = MEM32(desc + 16), i;
        if (!same_dll(dll, dll_want)) continue;
        if (!int_rva) int_rva = iat_rva;
        for (i = 0; ; i++) {
            u32 thunk = MEM32(GG_IMAGE_BASE + int_rva + i * 4);
            if (!thunk) break;
            if ((thunk & 0x80000000u) && (thunk & 0xFFFFu) == ord)
                return GG_IMAGE_BASE + iat_rva + i * 4;
        }
    }
    return 0;
}

static void bind_ordinal(const char *dll, u32 ord, const char *name,
                         void (*handler)(void), int argc) {
    u32 iat_va = find_iat_slot_ordinal(dll, ord);
    u32 addr;
    if (!iat_va) { g_unbound++; return; }
    addr = alloc_bridge(name, handler, argc);
    if (!addr) return;
    bridges[addr - BRIDGE_BASE].iat_va = iat_va;
    bridges[addr - BRIDGE_BASE].real   = NULL;
    MEM32(iat_va) = addr;
}

/* Report any import we never bridged: it would dispatch to nothing at runtime. */
static void report_unbridged(void) {
    u32 nt = GG_IMAGE_BASE + MEM32(GG_IMAGE_BASE + 0x3C);
    u32 imports = MEM32(nt + 0x80);
    u32 desc, missing = 0;

    if (!imports) return;
    for (desc = GG_IMAGE_BASE + imports; MEM32(desc + 12); desc += 20) {
        const char *dll = (const char *)(uintptr_t)ADDR(GG_IMAGE_BASE + MEM32(desc + 12));
        u32 int_rva = MEM32(desc + 0), iat_rva = MEM32(desc + 16), i;
        if (!int_rva) int_rva = iat_rva;
        for (i = 0; ; i++) {
            u32 thunk = MEM32(GG_IMAGE_BASE + int_rva + i * 4);
            u32 slot  = GG_IMAGE_BASE + iat_rva + i * 4;
            if (!thunk) break;
            if (MEM32(slot) >= BRIDGE_BASE && MEM32(slot) < BRIDGE_BASE + (u32)num_bridges)
                continue;
            missing++;
            /* An import can be by ORDINAL, and then the thunk is not an RVA at
             * all -- it is the ordinal with the high bit set. Dereferencing it
             * as a name reads somewhere around 0x80400000 and faults, which is
             * how this surfaced: the first binary here with ordinal imports
             * (MathStorm links Smacker that way) crashed inside this very
             * report, before a single lifted instruction had run. */
            if (thunk & 0x80000000u)
                fprintf(stderr, "  BRIDGE: UNBRIDGED %s!#%u\n", dll, thunk & 0xFFFFu);
            else
                fprintf(stderr, "  BRIDGE: UNBRIDGED %s!%s\n", dll,
                        (const char *)(uintptr_t)ADDR(GG_IMAGE_BASE + thunk + 2));
        }
    }
    fprintf(stderr, "  %d bridges bound, %u imports UNBRIDGED, %d in the table this title does not import\n",
            num_bridges, missing, g_unbound);
}

recomp_func_t iat_bridge_lookup(u32 target_va) {
    if (target_va >= BRIDGE_BASE && target_va < BRIDGE_BASE + (u32)num_bridges) {
        u32 idx = target_va - BRIDGE_BASE;
        g_bridge_hit = idx;
        if (g_verbose)
            fprintf(stderr, "  BRIDGE: %s (esp=0x%08X)\n", bridges[idx].name, esp);
        return bridges[idx].handler;
    }
    return NULL;
}

/* ===================================================================
 * The generic pass-through
 *
 * On x64 there is one calling convention and the CALLER cleans the stack, so
 * calling a real API through a pointer typed with MORE parameters than it takes
 * is harmless: the extras land in shadow space and registers the callee never
 * reads. That means one bridge body serves every API that only moves integers
 * and handles around -- the table below carries just the name and how many
 * dwords the game pushed, which is all the SIMULATED stack needs to unwind.
 *
 * Argument values pass straight through: the image is mapped at its real VA, so
 * a 32-bit pointer the game holds already is a valid host pointer.
 *
 * USER and GDI handles survive the round trip to 32 bits -- Windows guarantees
 * that for WOW64. Anything returning a real pointer does not, and is not in
 * this table.
 * =================================================================== */

typedef u64 (*fnany_t)(u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64);

static void generic_bridge(void) {
    bridge_entry_t *b = &bridges[g_bridge_hit];
    u64 a[12];
    int i;
    for (i = 0; i < 12; i++) a[i] = ARG(i + 1);
    eax = (u32)((fnany_t)b->real)(a[0], a[1], a[2], a[3], a[4], a[5],
                                  a[6], a[7], a[8], a[9], a[10], a[11]);
    esp += 4 + (u32)b->argc * 4;
}

/* Everything the game imports that is answered by simply doing it for real. */
static const struct { const char *dll, *name; int argc; } g_passthrough[] = {
    /* --- GDI32 (all 14 of them) --- */
    {"gdi32",  "CreatePalette",            1},
    {"gdi32",  "CreateSolidBrush",         1},
    {"gdi32",  "DeleteDC",                 1},
    {"gdi32",  "DeleteObject",             1},
    {"gdi32",  "GetStockObject",           1},
    {"gdi32",  "GetSystemPaletteEntries",  4},
    {"gdi32",  "GetSystemPaletteUse",      1},
    {"gdi32",  "SelectObject",             2},
    {"gdi32",  "SelectPalette",            3},
    {"gdi32",  "SetPaletteEntries",        4},
    /* Both pointers it takes -- the bits and the BITMAPINFO -- are in the
     * mapped image or the low heap, so they are host pointers already. */
    {"gdi32",  "StretchDIBits",           13},
    {"gdi32",  "TextOutA",                 5},

    /* --- USER32 --- */
    {"user32", "AppendMenuA",              4},
    {"user32", "CheckMenuItem",            3},
    {"user32", "ClientToScreen",           2},
    {"user32", "ClipCursor",               1},
    {"user32", "CreateMenu",               0},
    {"user32", "DefWindowProcA",           4},
    {"user32", "DeleteMenu",               3},
    {"user32", "DestroyCursor",            1},
    {"user32", "DestroyMenu",              1},
    {"user32", "DestroyWindow",            1},
    {"user32", "DrawMenuBar",              1},
    {"user32", "EnableMenuItem",           3},
    {"user32", "EndDialog",                2},
    {"user32", "FillRect",                 3},
    {"user32", "FindWindowA",              2},
    {"user32", "GetActiveWindow",          0},
    {"user32", "GetAsyncKeyState",         1},
    {"user32", "GetClipCursor",            1},
    {"user32", "GetCursorPos",             1},
    {"user32", "GetDC",                    1},
    {"user32", "GetDesktopWindow",         0},
    {"user32", "GetDlgItemTextA",          4},
    {"user32", "GetKeyboardState",         1},
    {"user32", "GetMenuItemCount",         1},
    {"user32", "GetMenuItemID",            2},
    {"user32", "GetMenuState",             3},
    {"user32", "GetMenuStringA",           5},
    {"user32", "GetSubMenu",               2},
    {"user32", "GetSystemMenu",            2},
    {"user32", "InsertMenuA",              5},
    {"user32", "IntersectRect",            3},
    {"user32", "InvalidateRect",           3},
    {"user32", "IsIconic",                 1},
    {"user32", "LoadCursorA",              2},
    {"user32", "LoadIconA",                2},
    {"user32", "LoadMenuA",                2},
    {"user32", "PostMessageA",             4},
    {"user32", "PostQuitMessage",          1},
    {"user32", "ReleaseDC",                2},
    {"user32", "RemoveMenu",               3},
    {"user32", "ScreenToClient",           2},
    {"user32", "SetActiveWindow",          1},
    {"user32", "SetCursor",                1},
    {"user32", "SetDlgItemTextA",          3},
    {"user32", "SetForegroundWindow",      1},
    {"user32", "SetMenu",                  2},
    {"user32", "SetRect",                  5},
    {"user32", "SetWindowPos",             7},
    {"user32", "ShowCursor",               1},
    {"user32", "ShowWindow",               2},
    {"user32", "UnregisterClassA",         2},
    {"user32", "UpdateWindow",             1},

    /* --- KERNEL32 --- */
    {"kernel32", "CloseHandle",            1},
    {"kernel32", "DeleteFileA",            1},
    {"kernel32", "GetCurrentDirectoryA",   2},
    {"kernel32", "GetCurrentThreadId",     0},
    {"kernel32", "GetDriveTypeA",          1},
    {"kernel32", "GetFileType",            1},
    {"kernel32", "GetFullPathNameA",       4},
    {"kernel32", "GetLastError",           0},
    {"kernel32", "GetLocalTime",           1},
    {"kernel32", "GetLogicalDrives",       0},
    {"kernel32", "GetPrivateProfileIntA",  4},
    {"kernel32", "GetStdHandle",           1},
    {"kernel32", "GetTimeZoneInformation", 1},
    {"kernel32", "GetVersion",             0},
    /* OSVERSIONINFOA is 148 bytes either way -- five DWORDs and a 128-byte
     * string -- so this one struct does pass straight through. */
    {"kernel32", "GetVersionExA",          1},
    {"kernel32", "GetWindowsDirectoryA",   2},
    {"kernel32", "ReadFile",               5},
    {"kernel32", "SetErrorMode",           1},
    {"kernel32", "SetFilePointer",         4},
    {"kernel32", "SetHandleCount",         1},
    {"kernel32", "WriteFile",              5},
    {"kernel32", "WritePrivateProfileStringA", 4},
    {"kernel32", "_lclose",                1},
    {"kernel32", "_llseek",                3},
    {"kernel32", "_lread",                 3},

    /* --- WINMM --- */
    {"winmm",  "midiOutGetDevCapsA",       3},
    {"winmm",  "midiOutGetNumDevs",        0},
    {"winmm",  "timeBeginPeriod",          1},
    {"winmm",  "timeEndPeriod",            1},
    {"winmm",  "timeGetDevCaps",           2},
    {"winmm",  "timeGetTime",              0},
    {"winmm",  "waveOutClose",             1},
    {"winmm",  "waveOutGetErrorTextA",     3},
    {"winmm",  "waveOutGetNumDevs",        0},
    {"winmm",  "waveOutPause",             1},
    {"winmm",  "waveOutReset",             1},
    {"winmm",  "waveOutRestart",           1},
    {"winmm",  "timeKillEvent",            1},

    /* --- Treasure MathStorm's additions ---
     *
     * The same engine three years on: a fuller CRT (the locale and codepage
     * calls), accelerators and a help file, and the pointer-validity checks.
     * IsBad*Ptr passes straight through -- the image is mapped at its own VA
     * and the low heap is below 4 GB, so a pointer the game holds already IS a
     * host pointer, and the answer the host gives is the true one. */
    {"kernel32", "AllocConsole",           0},
    {"kernel32", "CreateDirectoryA",       2},
    {"kernel32", "FlushFileBuffers",       1},
    {"kernel32", "GetACP",                 0},
    {"kernel32", "GetCPInfo",              2},
    {"kernel32", "GetOEMCP",               0},
    {"kernel32", "GetSystemDirectoryA",    2},
    {"kernel32", "IsBadCodePtr",           1},
    {"kernel32", "IsBadReadPtr",           2},
    {"kernel32", "IsBadWritePtr",          2},
    {"kernel32", "MultiByteToWideChar",    6},
    {"kernel32", "SetEndOfFile",           1},
    {"kernel32", "SetStdHandle",           2},
    {"kernel32", "WideCharToMultiByte",    8},
    {"user32",   "CheckRadioButton",       4},
    {"user32",   "GetCapture",             0},
    {"user32",   "GetClientRect",          2},
    {"user32",   "LoadAcceleratorsA",      2},
    {"user32",   "ModifyMenuA",            5},
    {"user32",   "ReleaseCapture",         0},
    {"user32",   "SetWindowTextA",         2},
    {"user32",   "WinHelpA",               4},

    /* --- Treasure Cove's additions ---
     *
     * The earliest of these builds and the closest to Neptune: resource DLLs
     * rather than .DAT archives, CheckSound/CheckDisplay in its .INI, and real
     * GDI blitting alongside WinG. Most of what is new here is what Neptune
     * used and Gizmos & Gadgets did not -- the superset earns its name. */
    {"gdi32",    "BitBlt",                 9},
    {"gdi32",    "CreateBitmap",           5},
    {"gdi32",    "CreateCompatibleDC",     1},
    {"gdi32",    "GetNearestPaletteIndex", 2},
    {"gdi32",    "GetTextMetricsA",        2},
    {"user32",   "CreatePopupMenu",        0},
    {"user32",   "FrameRect",              3},
    {"user32",   "GetMenu",                1},
    {"user32",   "IsDlgButtonChecked",     2},
    {"user32",   "MoveWindow",             5},
    {"user32",   "OffsetRect",             3},
    {"user32",   "SendMessageA",           4},
    {"user32",   "SetFocus",               1},
    {"kernel32", "CreateSemaphoreA",       4},
    {"kernel32", "lstrcatA",               2},
    {"kernel32", "lstrcmpA",               2},
    {"kernel32", "lstrcpyA",               2},
};

/* ===================================================================
 * Memory: served from the runtime's low heap, never from the host's
 * =================================================================== */

/* GMEM_MOVEABLE hands back a "handle"; the game then GlobalLocks it for the
 * pointer. ponytail: handle == pointer, which is what Win32 does for anything
 * not allocated GMEM_DDESHARE, so Lock and Unlock are near no-ops. */
static void bridge_GlobalAlloc(void)   { eax = gg_heap_alloc(ARG(2)); esp += 4 + 8; }
static void bridge_GlobalFree(void)    { gg_heap_free(ARG(1)); eax = 0; esp += 4 + 4; }
static void bridge_GlobalLock(void)    { eax = ARG(1); esp += 4 + 4; }
static void bridge_GlobalUnlock(void)  { eax = 0; esp += 4 + 4; }

static void bridge_VirtualAlloc(void) {
    /* The CRT reserves a region and then commits pages inside it one at a time.
     * Our heap commits everything up front, so a request naming an address we
     * already own is answered with that same address. */
    if (ARG(1) && gg_heap_owns(ARG(1))) eax = ARG(1);
    else                                 eax = gg_heap_alloc(ARG(2));
    esp += 4 + 16;
}
static void bridge_VirtualFree(void) { gg_heap_free(ARG(1)); eax = 1; esp += 4 + 12; }

/* GlobalSize: the heap already tracks it, which is the whole reason the game's
 * allocations come from ours rather than the host's. */
/* LocalAlloc/LocalFree hand back a pointer the same way GlobalAlloc does, so
 * they come from the same low heap for the same reason. Windows has made Local
 * and Global the same heap since NT; this makes them the same here too. */
static void bridge_LocalAlloc(void) { eax = gg_heap_alloc(ARG(2)); esp += 4 + 8; }
static void bridge_LocalFree(void)  { gg_heap_free(ARG(1)); eax = 0; esp += 4 + 4; }

/* Neptune-era memory bookkeeping: a handle IS the pointer here (see
 * GlobalAlloc), so Handle is identity and Compact always has room. */
static void bridge_GlobalHandle(void)  { eax = ARG(1); esp += 4 + 4; }
static void bridge_GlobalCompact(void) { eax = 0x00400000u; esp += 4 + 4; }

/*
 * wsprintfA -- the only varargs import in any of these games, and the only
 * CDECL one, so it is the only place the CALLER cleans the stack. Popping the
 * arguments here as if it were stdcall would unbalance every caller.
 *
 * The arguments themselves cannot be forwarded as they sit: the game pushed
 * dwords and the host's va_list walks 8-byte slots. So they are widened into a
 * host-side array and wvsprintfA is handed that.
 *
 * Widening is safe for every conversion wsprintfA has, because it has no
 * floating point at all -- that is documented, not an assumption -- so each
 * conversion consumes exactly one dword there and one slot here. A %s argument
 * is a 32-bit VA, and the image and heap are both mapped below 4 GB, so it is
 * already a host pointer.
 */
#define WSPRINTF_MAX_ARGS 32

static void bridge_wsprintfA(void) {
    char *out = (char *)(uintptr_t)ADDR(ARG(1));
    const char *fmt = (const char *)(uintptr_t)ADDR(ARG(2));
    uintptr_t slots[WSPRINTF_MAX_ARGS];
    int n = 0;
    const char *p;

    for (p = fmt; *p && n < WSPRINTF_MAX_ARGS; p++) {
        if (*p != '%') continue;
        if (p[1] == '%') { p++; continue; }
        slots[n] = (uintptr_t)ARG(3 + n);
        n++;
    }
    memset(slots + n, 0, sizeof(slots) - n * sizeof(slots[0]));

    eax = (u32)wvsprintfA(out, fmt, (va_list)slots);
    esp += 4;     /* CDECL: the caller pops its own arguments */
}

static void bridge_GlobalSize(void) { eax = gg_heap_size(ARG(1)); esp += 4 + 4; }

/*
 * MEMORYSTATUS is 32 bytes in the game and 56 here, because six of its eight
 * fields are SIZE_T. Filled in by hand, and with numbers from 1996 rather than
 * the host's: the real ones do not fit in a 32-bit field, and a game sizing a
 * cache off "available physical memory" should not be told about 32 GB.
 *
 * ponytail: a fixed 64 MB machine. If something ever needs to scale with the
 * real host, the place to do it is here, clamped below 4 GB.
 */
static void bridge_GlobalMemoryStatus(void) {
    u32 p = ARG(1);
    MEM32(p +  0) = 32;                  /* dwLength         */
    MEM32(p +  4) = 25;                  /* dwMemoryLoad, %  */
    MEM32(p +  8) = 64u   * 1024 * 1024; /* dwTotalPhys      */
    MEM32(p + 12) = 48u   * 1024 * 1024; /* dwAvailPhys      */
    MEM32(p + 16) = 128u  * 1024 * 1024; /* dwTotalPageFile  */
    MEM32(p + 20) = 96u   * 1024 * 1024; /* dwAvailPageFile  */
    MEM32(p + 24) = 2000u * 1024 * 1024; /* dwTotalVirtual   */
    MEM32(p + 28) = 1800u * 1024 * 1024; /* dwAvailVirtual   */
    esp += 4 + 4;
}

/*
 * CRITICAL_SECTION is 24 bytes in the game and 40 here, so it cannot be passed
 * through -- and there is nothing to serialise anyway: the recompiled game runs
 * on one thread, and the lifted code shares one global register set, so a
 * second thread could not run it. No-ops.
 *
 * ponytail: if a real thread ever appears these become the host API over a side
 * table keyed by the game's pointer -- not a widened struct in place.
 */
static void bridge_InitializeCriticalSection(void) { esp += 4 + 4; }
static void bridge_EnterCriticalSection(void)      { esp += 4 + 4; }
static void bridge_LeaveCriticalSection(void)      { esp += 4 + 4; }

/*
 * Which files the game reaches for, and whether it got them. "Data file
 * missing" says nothing about WHICH data file; this does.
 */
static int g_file_trace = -1;
static int file_trace(void) {
    if (g_file_trace < 0)
        g_file_trace = gg_quiet("GG_QUIET_FILES") ? 0 : 1;
    return g_file_trace;
}
static void report_file(const char *api, const char *path, int ok) {
    if (file_trace())
        fprintf(stderr, "  FILE: %-20s %s%s\n", api, path, ok ? "" : "   <-- FAILED");
}

/*
 * The one check from 1996 that stops this game dead.
 *
 * sub_0043801E builds "<windir>\system\midimap.cfg", _lopens it, and if that
 * fails tries "\system32\" as well. MIDIMAP.CFG is the Windows 3.1 MIDI mapper
 * configuration, and no Windows has shipped it in about thirty years -- so both
 * opens fail, the caller reads the -1, and the game puts up dialog 3041 ("The
 * MIDI driver for your sound card could not be found") whose only button is
 * Exit. That was the whole of the first run that got this far.
 *
 * The question it is really asking -- "is there a MIDI device configured?" --
 * already got a yes: midiOutGetNumDevs and midiOutGetDevCapsA ran just before
 * this and passed straight through to the host, which has a synth. Only the
 * file is missing, and the game never reads a byte of it: sub_0043801E does not
 * even close the handle, it just returns what _lopen gave it.
 *
 * So the probe is answered with NUL -- a file that is always there and has
 * nothing in it, which is exactly what is true.
 *
 * Neptune had the same check behind a CheckSound switch in its .INI. This build
 * hardcodes it, so it is answered here instead.
 */
static int is_answered_probe(const char *path) {
    static const char *const names[] = {
        /* The Windows 3.1 MIDI mapper config, above. */
        "midimap.cfg",
        /* "Is WinG installed?", looked for in the system directory. We ARE
         * WinG -- the shim below answers all eight of its entry points -- so
         * the only true answer is yes. MathStorm asks this one and exits if it
         * does not like the answer; Gizmos & Gadgets never asks. */
        "wing32.dll",
    };
    const char *slash = strrchr(path, '\\');
    size_t i;
    if (!slash) slash = strrchr(path, '/');
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (_stricmp(slash ? slash + 1 : path, names[i]) == 0) return 1;
    return 0;
}

/*
 * OpenFile, which is how Treasure Cove -- the oldest build here -- opens
 * everything, including the resource DLLs it will not start without.
 *
 * OFSTRUCT is 136 bytes either way (four words and a 128-byte path), so the
 * struct itself could pass straight through; this exists for the trace. "Could
 * not find Resource File!" says nothing about WHICH file, and the answer turned
 * out to be a path, not a resource.
 */
static void bridge_OpenFile(void) {
    const char *path = (const char *)(uintptr_t)ADDR(ARG(1));
    HFILE f = OpenFile(path, (OFSTRUCT *)(uintptr_t)ADDR(ARG(2)), ARG(3));
    if (f == HFILE_ERROR && is_answered_probe(path)) {
        f = _lopen("NUL", OF_READ);
        if (file_trace())
            fprintf(stderr, "  FILE: OpenFile            %s   <-- answered with NUL "
                            "(a 1996 environment probe this runtime already satisfies)\n", path);
    } else {
        report_file("OpenFile", path, f != HFILE_ERROR);
    }
    eax = (u32)f;
    esp += 4 + 12;
}

static void bridge_lopen(void) {
    const char *path = (const char *)(uintptr_t)ADDR(ARG(1));
    HFILE f = _lopen(path, (int)ARG(2));
    if (f == HFILE_ERROR && is_answered_probe(path)) {
        f = _lopen("NUL", OF_READ);
        if (file_trace())
            fprintf(stderr, "  FILE: _lopen               %s   <-- answered with NUL "
                            "(Windows 3.1 MIDI mapper config; the host has MIDI)\n", path);
        eax = (u32)f;
        esp += 4 + 8;
        return;
    }
    report_file("_lopen", path, f != HFILE_ERROR);
    eax = (u32)f;
    esp += 4 + 8;
}

/*
 * SECURITY_ATTRIBUTES is twelve bytes in the game and twenty-four here, so
 * handing the pointer straight over makes the kernel read a security descriptor
 * out of the middle of the struct. That is ERROR_NOACCESS (998) on a file that
 * exists and is perfectly readable -- which is how this surfaced.
 */
static SECURITY_ATTRIBUTES *sec_attrs(u32 va, SECURITY_ATTRIBUTES *sa) {
    if (!va) return NULL;
    sa->nLength              = sizeof(*sa);
    sa->lpSecurityDescriptor = MEM32(va + 4) ? (void *)(uintptr_t)ADDR(MEM32(va + 4)) : NULL;
    sa->bInheritHandle       = (BOOL)MEM32(va + 8);
    return sa;
}

static void bridge_CreateFileA(void) {
    const char *path = (const char *)(uintptr_t)ADDR(ARG(1));
    SECURITY_ATTRIBUTES sa;
    HANDLE h = CreateFileA(path, ARG(2), ARG(3),
                           sec_attrs(ARG(4), &sa),
                           ARG(5), ARG(6), (HANDLE)(uintptr_t)ARG(7));
    /* MathStorm asks for WinG the same way Gizmos & Gadgets asks for the MIDI
     * mapper, only through CreateFileA rather than _lopen -- and exits if it
     * does not find it. Same answer, same reason. */
    if (h == INVALID_HANDLE_VALUE && is_answered_probe(path)) {
        h = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, 0, NULL);
        if (file_trace())
            fprintf(stderr, "  FILE: CreateFileA          %s   <-- answered with NUL "
                            "(a 1996 environment probe this runtime already satisfies)\n", path);
    } else if (h == INVALID_HANDLE_VALUE && file_trace()) {
        fprintf(stderr, "  FILE: CreateFileA          %s   <-- FAILED %lu "
                        "(access=0x%X share=0x%X disp=%u attr=0x%X)\n",
                path, GetLastError(), ARG(2), ARG(3), ARG(5), ARG(6));
    } else {
        report_file("CreateFileA", path, 1);
    }
    eax = (u32)(uintptr_t)h;
    esp += 4 + 28;
}

static void bridge_GetFileAttributesA(void) {
    const char *path = (const char *)(uintptr_t)ADDR(ARG(1));
    DWORD a = GetFileAttributesA(path);
    report_file("GetFileAttributesA", path, a != INVALID_FILE_ATTRIBUTES);
    eax = a;
    esp += 4 + 4;
}

extern char g_game_module_path[];

/*
 * A bare .INI name resolves against the Windows directory, which is where the
 * installer would have put SSGWINCD.INI. There is a copy of it next to the game
 * on the disc, so look there too rather than silently taking defaults.
 */
static void bridge_GetPrivateProfileStringA(void) {
    const char *sec  = ARG(1) ? (const char *)(uintptr_t)ADDR(ARG(1)) : NULL;
    const char *key  = ARG(2) ? (const char *)(uintptr_t)ADDR(ARG(2)) : NULL;
    const char *def  = ARG(3) ? (const char *)(uintptr_t)ADDR(ARG(3)) : "";
    char *out        = (char *)(uintptr_t)ADDR(ARG(4));
    const char *file = ARG(6) ? (const char *)(uintptr_t)ADDR(ARG(6)) : NULL;

    eax = GetPrivateProfileStringA(sec, key, def, out, ARG(5), file);

    /*
     * CDDrive is where the game looks for its data, and the .INI on the disc
     * still holds the pre-install placeholder. The disc IS wherever the copy we
     * were pointed at lives, so answer with that instead of making everyone
     * hand-edit a file inside original/.
     */
    if (key && _stricmp(key, "CDDrive") == 0) {
        char dir[MAX_PATH];
        char *slash;
        strncpy(dir, g_game_module_path, sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = 0;
        slash = strrchr(dir, '\\');
        if (slash && (u32)(slash - dir) + 2 < ARG(5)) {
            slash[1] = 0;
            strcpy(out, dir);
            eax = (u32)strlen(out);
        } else {
            fprintf(stderr, "  INI:  CDDrive override wants %u bytes, game gave %u\n",
                    slash ? (u32)(slash - dir) + 2 : 0, ARG(5));
        }
    }

    /*
     * The two environment checks the game refuses to start without, both of
     * which are asking about a machine from 1996:
     *
     *   CheckSound   opens C:\WINDOWS\SYSTEM\MIDIMAP.CFG, the Windows 3.1 MIDI
     *                mapper config, which no Windows has shipped in decades.
     *   CheckDisplay wants the desktop in 640x480 at 256 colours.
     *
     * Both are the game's own INI switches, meant to be turned off by anyone
     * whose machine did not match -- which now means everyone. They gate the
     * checks, not the sound or the graphics.
     */
    if (key && ARG(5) > 5 &&
        (_stricmp(key, "CheckSound") == 0 || _stricmp(key, "CheckDisplay") == 0)) {
        strcpy(out, "FALSE");
        eax = 5;
    }
    if (file_trace())
        fprintf(stderr, "  INI:  [%s] %s = '%s'   (%s)\n",
                sec ? sec : "?", key ? key : "?", out, file ? file : "?");
    esp += 4 + 24;
}

/*
 * The GAME's module path, not the host's. It appends SSGWINCD.INI to this to
 * find its settings, and the .INI sits next to SSGWIN32.EXE on the disc.
 */
static void bridge_GetModuleFileNameA(void) {
    char *out = (char *)(uintptr_t)ADDR(ARG(2));
    u32 cch = ARG(3);
    u32 n = (u32)strlen(g_game_module_path);
    if (n > cch - 1) n = cch - 1;
    memcpy(out, g_game_module_path, n);
    out[n] = 0;
    eax = n;
    esp += 4 + 12;
}

/* ===================================================================
 * The screen the game thinks it has
 *
 * "Gizmos & Gadgets! is specifically designed to be played in 640 x 480
 * 256-color mode" -- dialog 3035, and the game means it. It asks three
 * different ways (GetSystemMetrics, GetDeviceCaps, GetWindowRect on the
 * desktop) and sizes both its window and its off-screen buffer from the
 * answers, so all three have to agree or the buffer and the drawing disagree
 * about how wide a row is.
 *
 * Handing it the real desktop got a 1920x1080 window, a 512x384 WinG buffer,
 * and a fault inside the first sprite blit.
 *
 * GG_SCREEN=w,h overrides; GG_SCREEN=0,0 passes the real desktop through.
 * ponytail: one knob, because a 1996 title on a 2026 panel is exactly the kind
 * of thing that needs one.
 * =================================================================== */

static int g_scr_w = -1, g_scr_h;

static void screen_size(void) {
    if (g_scr_w >= 0) return;
    {
        char buf[32];
        g_scr_w = 640; g_scr_h = 480;
        if (GetEnvironmentVariableA("GG_SCREEN", buf, sizeof(buf))) {
            char *comma = strchr(buf, ',');
            g_scr_w = atoi(buf);
            g_scr_h = comma ? atoi(comma + 1) : 0;
        }
    }
}

static void bridge_GetSystemMetrics(void) {
    screen_size();
    if (g_scr_w > 0 && ARG(1) == SM_CXSCREEN)      eax = (u32)g_scr_w;
    else if (g_scr_w > 0 && ARG(1) == SM_CYSCREEN) eax = (u32)g_scr_h;
    else                                           eax = (u32)GetSystemMetrics((int)ARG(1));
    esp += 4 + 4;
}

/*
 * The same screen, told a second way. Colour depth matters as much as size
 * here: at 32bpp the host DC reports no palette, and this game is all palette
 * -- CreatePalette, SelectPalette, RealizePalette, SetPaletteEntries. The
 * pixels it actually draws are 8bpp either way, because they go through the
 * WinG shim below, which is ours.
 */
static void bridge_GetDeviceCaps(void) {
    screen_size();
    switch ((int)ARG(2)) {
    case HORZRES:     eax = g_scr_w > 0 ? (u32)g_scr_w : (u32)GetDeviceCaps((HDC)(uintptr_t)ARG(1), HORZRES); break;
    case VERTRES:     eax = g_scr_w > 0 ? (u32)g_scr_h : (u32)GetDeviceCaps((HDC)(uintptr_t)ARG(1), VERTRES); break;
    case BITSPIXEL:   eax = 8;   break;
    case PLANES:      eax = 1;   break;
    case NUMCOLORS:   eax = 256; break;
    case SIZEPALETTE: eax = 256; break;
    /* RC_PALETTE, so the palette calls above take the path they were written
     * for rather than the one a truecolour driver would send them down. */
    case RASTERCAPS:  eax = (u32)GetDeviceCaps((HDC)(uintptr_t)ARG(1), RASTERCAPS) | RC_PALETTE; break;
    default:          eax = (u32)GetDeviceCaps((HDC)(uintptr_t)ARG(1), (int)ARG(2)); break;
    }
    esp += 4 + 8;
}

/* And a third way: GetWindowRect of the desktop window is how the game measures
 * the screen when it is about to size its own. */
static void bridge_GetWindowRect(void) {
    u32 p = ARG(2);
    screen_size();
    if (g_scr_w > 0 && (HWND)(uintptr_t)ARG(1) == GetDesktopWindow()) {
        MEM32(p + 0) = 0; MEM32(p + 4) = 0;
        MEM32(p + 8) = (u32)g_scr_w; MEM32(p + 12) = (u32)g_scr_h;
        eax = 1;
    } else {
        RECT r;
        eax = GetWindowRect((HWND)(uintptr_t)ARG(1), &r);
        MEM32(p + 0) = (u32)r.left;  MEM32(p + 4)  = (u32)r.top;
        MEM32(p + 8) = (u32)r.right; MEM32(p + 12) = (u32)r.bottom;
    }
    esp += 4 + 8;
}

/* ===================================================================
 * Process and CRT startup
 * =================================================================== */

static void bridge_GetCommandLineA(void) {
    static u32 va = 0;
    if (!va) va = recomp_scratch_str(GetCommandLineA());
    eax = va;
    esp += 4;
}

static void bridge_GetEnvironmentStrings(void) {
    /* An empty block: two NULs. The CRT walks it and finds nothing, which is
     * true enough -- the game reads no environment variables. */
    static u32 va = 0;
    if (!va) { va = recomp_scratch_alloc(4); MEM32(va) = 0; }
    eax = va;
    esp += 4;
}

static void bridge_GetStartupInfoA(void) {
    /* STARTUPINFOA is 68 bytes on 32-bit and 104 on 64-bit, so it is filled in
     * by hand rather than passed through. The CRT reads cb, dwFlags and
     * wShowWindow; the rest being zero is what a plain launch looks like. */
    u32 p = ARG(1);
    memset((void *)(uintptr_t)ADDR(p), 0, 68);
    MEM32(p + 0)  = 68;                 /* cb */
    MEM32(p + 44) = 0;                  /* dwFlags */
    esp += 4 + 4;
}

static void bridge_GetModuleHandleA(void) {
    /*
     * The HOST's module handle, not the image base. It is only ever used as an
     * hInstance, and USER32 checks that the value a window is created with is
     * the one its class was registered with -- so both sides have to agree on
     * something USER32 recognises. This exe is linked at 0x70000000 precisely so
     * that its real handle still fits in eax.
     *
     * Resource lookups ignore the handle: FindResourceA below always reads the
     * mapped original.
     */
    eax = (u32)(uintptr_t)GetModuleHandleA(
              ARG(1) ? (const char *)(uintptr_t)ADDR(ARG(1)) : NULL);
    esp += 4 + 4;
}

static void bridge_ExitProcess(void) {
    fprintf(stderr, "\nExitProcess(0x%X)\n", ARG(1));
    recomp_dump_trace("ExitProcess");
    fflush(NULL);
    /* The real API, not the CRT's exit(): exit() would unwind host static
     * teardown while a lifted frame is still on the host stack with esp
     * pointing into the simulated one. */
    ExitProcess(ARG(1));
}

static void bridge_RaiseException(void)             { fprintf(stderr, "    RaiseException(0x%X)\n", ARG(1)); esp += 4 + 16; }
static void bridge_RtlUnwind(void)                  { esp += 4 + 16; }
static void bridge_UnhandledExceptionFilter(void)   { eax = EXCEPTION_EXECUTE_HANDLER; esp += 4 + 4; }
static void bridge_SetConsoleCtrlHandler(void)      { eax = 1; esp += 4 + 8; }

/* ===================================================================
 * WinG
 *
 * WING32.DLL was Microsoft's 1994 fast-DIB library, and it is how this game
 * draws: it creates a memory DC, asks WinG for a bitmap, gets a raw pointer to
 * the pixels, renders into that with its own code, and blits once per frame.
 * Nothing else in the import table touches pixels.
 *
 * Windows dropped WinG long ago, so the shim below is the renderer. It is also
 * why the drawing has a real chance of being correct on the first run: the game
 * writes the pixels itself, and all we have to do is put them on screen.
 * =================================================================== */

static u32   g_wing_pal[256];

/*
 * The framebuffer has to be readable two ways at once.
 *
 * The game asks WinG for a raw pointer and writes most of its pixels through it
 * directly, so that pointer must be 32 bits. But it also selects the WinG bitmap
 * into the WinG DC and draws text with real GDI -- CreateFontIndirectA, TextOutA,
 * SetTextColor -- so GDI has to be writing into the same pixels. Handing back a
 * plain buffer got the sprites and lost every string: the text went to the 1x1
 * default bitmap of a memory DC with nothing selected.
 *
 * So the pixels live in a pagefile-backed section, mapped twice: once by us at a
 * fixed low address for the game, and once by CreateDIBSection for GDI. Two
 * views, same pages.
 */
#define WING_VA_BASE 0x30000000u

/*
 * Slack either side of the picture, because the game draws outside it.
 *
 * Its sprite blitter (sub_0040280E) clips the starting y against the clip
 * rectangle on entry and then does not check it again: the row loop runs the
 * sprite's full height, stepping edi by the stride each time. Start a 200-row
 * sprite at y=380 on a 384-row surface and it writes 196 rows past the bottom.
 * In 1996 the DIB had neighbours and those rows landed on somebody else's heap,
 * unnoticed. Against a mapping of exactly the right size it is an access
 * violation, and it was: a fault on the first frame, writing the first byte
 * past the end.
 *
 * So the slack is a whole surface at each end rather than a round number --
 * that is the worst case by construction, a sprite no taller than the screen
 * starting on its last row. The other end is Neptune's case, its save-under
 * reading a row above the top.
 *
 * It has to be a multiple of the 64 KB allocation granularity: the same offset
 * is what CreateDIBSection is given into the section.
 */
#define WING_SLACK_MIN  0x10000u
#define WING_SLACK_FOR(span)  \
    ((((span) + 0xFFFFu) & ~0xFFFFu) < WING_SLACK_MIN ? WING_SLACK_MIN \
                                                      : (((span) + 0xFFFFu) & ~0xFFFFu))

/*
 * More than one bitmap at a time.
 *
 * Neptune only ever had one, so the shim it came from kept one section, one
 * HBITMAP and one VA, and every WinGCreateBitmap freed the previous. This game
 * asks for two -- same DC, same 512x384 geometry -- and keeps both pointers. On
 * one buffer they aliased, and a write meant for the second page went off the
 * end of the first and faulted inside the first sprite blit.
 *
 * So each bitmap gets its own slot: its own section, its own DIB section, and
 * its own 4 MB of low VA. Sixteen slots is well past what any WinG program
 * wanted, and 64 MB of reserved-but-mostly-unmapped address space costs nothing
 * at 0x30000000.
 */
#define WING_MAX_BMP  16
#define WING_SLOT     0x00400000u        /* VA per bitmap; 640x480 needs 300 KB */

typedef struct {
    HBITMAP bmp;
    HANDLE  section;
    u32     va;        /* slot base -- the mapping starts here */
    u32     bits;      /* start of the picture (va + SLACK) */
    u32     top;       /* topmost scanline, which is what WinG hands back */
    int     w, h, stride;
    HDC     dc;        /* the DC it was created against */
} wing_bmp_t;

static wing_bmp_t g_wing[WING_MAX_BMP];
static int        g_wing_n;

static void wing_free_slot(wing_bmp_t *b) {
    if (b->dc && b->bmp) SelectObject(b->dc, GetStockObject(DEFAULT_GUI_FONT));
    if (b->bmp)     { DeleteObject(b->bmp); }
    if (b->va)      { UnmapViewOfFile((void *)(uintptr_t)b->va); }
    if (b->section) { CloseHandle(b->section); }
    memset(b, 0, sizeof(*b));
}

/* Which of ours is selected into this DC right now. GDI already knows -- the
 * game selects with a real SelectObject, which passes straight through -- so
 * ask it rather than shadowing the state and getting it wrong. */
static wing_bmp_t *wing_for_dc(HDC dc) {
    HBITMAP cur = (HBITMAP)GetCurrentObject(dc, OBJ_BITMAP);
    int i, last = -1;
    for (i = 0; i < g_wing_n; i++) {
        if (!g_wing[i].bmp) continue;
        if (g_wing[i].bmp == cur) return &g_wing[i];
        if (g_wing[i].dc == dc) last = i;
    }
    /* Nothing of ours selected: fall back to the newest bitmap made against
     * this DC, which is what a caller that never selected would mean. */
    return last >= 0 ? &g_wing[last] : NULL;
}

/* WinGCreateDC(void) -> HDC. A new one each time: the game may want two. */
static void bridge_WinGCreateDC(void) {
    eax = (u32)(uintptr_t)CreateCompatibleDC(NULL);
    esp += 4;
}

/*
 * WinGRecommendDIBFormat(BITMAPINFO*) -> BOOL. The caller reads the SIGN of
 * biHeight and nothing else: this is WinG saying which way up the display would
 * rather be handed its pixels.
 *
 * It has to be +1 here, not Neptune's -1.
 *
 * The answer does not just describe the bitmap, it picks which of the game's
 * two blitters runs. sub_00402F5x compares this against -1 and installs either
 * a top-down set (row stride +512, row address base + y*512, sub_00402984) or a
 * bottom-up set (stride -512, base + (383-y)*512, sub_0040280E, stepping
 * `sub edi, 0x200` per row). Recommend top-down and the game still chose its
 * bottom-up blitter, so the picture was drawn upside down through a top-down
 * DIB and came out as a sheared wedge.
 *
 * Bottom-up is also what WinG really answered on the 8bpp displays this game
 * shipped for; it is the DIB orientation Windows has called native since 3.0.
 *
 * GG_TOPDOWN=1 puts it back, for looking at the other blitter.
 */
static void bridge_WinGRecommendDIBFormat(void) {
    u32 p = ARG(1);
    int topdown = GetEnvironmentVariableA("GG_TOPDOWN", NULL, 0) != 0;
    memset((void *)(uintptr_t)ADDR(p), 0, sizeof(BITMAPINFOHEADER));
    MEM32(p + 0)  = sizeof(BITMAPINFOHEADER);   /* biSize */
    MEM32(p + 8)  = topdown ? (u32)-1 : 1;      /* biHeight: sign is the answer */
    MEM16(p + 12) = 1;                          /* biPlanes */
    MEM16(p + 14) = 8;                          /* biBitCount */
    MEM32(p + 16) = BI_RGB;
    eax = 1;
    esp += 4 + 4;
}

/* WinGCreateBitmap(HDC, BITMAPINFO*, void**) -> HBITMAP */
static void bridge_WinGCreateBitmap(void) {
    HDC hdc = (HDC)(uintptr_t)ARG(1);
    u32 bmi = ARG(2), ppbits = ARG(3);
    int w = (int)MEM32(bmi + 4);
    int raw_h = (int)MEM32(bmi + 8);
    int h = raw_h < 0 ? -raw_h : raw_h;
    int stride, slot;
    DWORD span, slack;
    void *gdi_bits = NULL, *our_view;
    char info[sizeof(BITMAPINFOHEADER) + 256 * sizeof(RGBQUAD)];
    BITMAPINFO *bi = (BITMAPINFO *)info;
    wing_bmp_t *b;

    if (w <= 0 || h <= 0) { w = 640; h = 480; raw_h = -480; }

    for (slot = 0; slot < WING_MAX_BMP; slot++)
        if (!g_wing[slot].bmp) break;
    if (slot == WING_MAX_BMP) {
        fprintf(stderr, "    WinG: all %d bitmap slots in use\n", WING_MAX_BMP);
        eax = 0;
        esp += 4 + 12;
        return;
    }
    if (slot >= g_wing_n) g_wing_n = slot + 1;
    b = &g_wing[slot];
    memset(b, 0, sizeof(*b));

    stride = (w + 3) & ~3;
    span   = (DWORD)(stride * h);
    slack  = WING_SLACK_FOR(span);
    if (span + 2 * slack > WING_SLOT) {
        fprintf(stderr, "    WinG: %dx%d + slack does not fit a %u-byte slot\n", w, h, WING_SLOT);
        eax = 0;
        esp += 4 + 12;
        return;
    }

    b->w = w; b->h = h; b->stride = stride; b->dc = hdc;
    b->va = WING_VA_BASE + (u32)slot * WING_SLOT;

    b->section = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                                    span + 2 * slack, NULL);
    our_view = b->section
             ? MapViewOfFileEx(b->section, FILE_MAP_ALL_ACCESS, 0, 0,
                               span + 2 * slack, (void *)(uintptr_t)b->va)
             : NULL;
    if (!our_view) {
        fprintf(stderr, "    WinG: could not map %u bytes at 0x%08X (%lu)\n",
                span, b->va, GetLastError());
        wing_free_slot(b);
        eax = 0;
        esp += 4 + 12;
        return;
    }
    b->bits = b->va + slack;

    memset(info, 0, sizeof(info));
    bi->bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi->bmiHeader.biWidth       = w;
    bi->bmiHeader.biHeight      = raw_h;       /* whichever way round it asked for */
    bi->bmiHeader.biPlanes      = 1;
    bi->bmiHeader.biBitCount    = 8;
    bi->bmiHeader.biCompression = BI_RGB;
    bi->bmiHeader.biClrUsed     = 256;
    memcpy(bi->bmiColors, g_wing_pal, sizeof(g_wing_pal));

    b->bmp = CreateDIBSection(hdc, bi, DIB_RGB_COLORS, &gdi_bits,
                              b->section, slack);
    if (!b->bmp) {
        fprintf(stderr, "    WinG: CreateDIBSection failed (%lu)\n", GetLastError());
        wing_free_slot(b);
        eax = 0;
        esp += 4 + 12;
        return;
    }
    /* Select it only if the DC has none of ours yet. The game selects for
     * itself when it means to switch pages, and selecting here as well would
     * silently swap the page out from under a caller that did not ask. */
    if (hdc && !wing_for_dc(hdc)) SelectObject(hdc, b->bmp);

    /*
     * The START of the pixel data, both ways round -- which is what real WinG
     * returns, because it is simply the pointer CreateDIBSection gave it.
     *
     * Neptune's shim moved this to the last row for a bottom-up DIB, on the
     * reasoning that WinG hands back the topmost scanline. Neptune only ever
     * asked for top-down surfaces, so that branch never ran there; here it did,
     * and it double-counted. This game's bottom-up row address helper
     * (sub_00402518) computes `base + (383 - y) * 512` for itself, so handing it
     * a base that already had (h-1)*stride added put every row a whole screen
     * past the end of the picture -- inside the slack, faulting nothing, drawing
     * nothing, and leaving a perfectly black 512x384 rectangle on screen with a
     * fully populated palette.
     */
    b->top = b->bits;

    if (ppbits) MEM32(ppbits) = b->top;
    fprintf(stderr, "    WinG: bitmap %d, %dx%d %s at 0x%08X, top row 0x%08X, %u bytes slack each side\n",
            slot, w, h, raw_h > 0 ? "bottom-up" : "top-down",
            b->bits, b->top, slack);
    eax = (u32)(uintptr_t)b->bmp;
    esp += 4 + 12;
}

/* WinGGetDIBPointer(HDC, BITMAPINFO*) -> void*. The pointer belongs to whatever
 * is selected into that DC, so it has to be looked up, not remembered. */
static void bridge_WinGGetDIBPointer(void) {
    wing_bmp_t *b = wing_for_dc((HDC)(uintptr_t)ARG(1));
    u32 out = ARG(2);
    if (!b) {
        fprintf(stderr, "    WinG: GetDIBPointer on DC 0x%X with no WinG bitmap\n", ARG(1));
        eax = 0;
        esp += 4 + 8;
        return;
    }
    if (out) {
        MEM32(out + 0)  = sizeof(BITMAPINFOHEADER);
        MEM32(out + 4)  = (u32)b->w;
        MEM32(out + 8)  = (u32)(-b->h);
        MEM16(out + 12) = 1;
        MEM16(out + 14) = 8;
        MEM32(out + 16) = BI_RGB;
        MEM32(out + 20) = (u32)(b->stride * b->h);
    }
    eax = b->top;
    fprintf(stderr, "    WinG: GetDIBPointer(dc=0x%X) -> 0x%08X (%dx%d)\n",
            ARG(1), eax, b->w, b->h);
    esp += 4 + 8;
}

/* WinGSetDIBColorTable(HDC, UINT start, UINT n, RGBQUAD*) -> UINT.
 * GDI needs the table as well as us: it is what TextOutA maps its colours
 * through when it draws into an 8bpp DIB section. The game sets the palette
 * once and expects every page to have it, so it goes to all of them. */
/* Push g_wing_pal[start..start+n) into every WinG bitmap's colour table.
 *
 * SetDIBColorTable works on whatever is SELECTED into the DC it is given, so
 * reaching every page means having every page selected somewhere. The page the
 * game has selected is done through its own DC -- a bitmap cannot be in two DCs
 * at once, so borrowing it would fail -- and the rest through a scratch DC. */
static void wing_apply_palette(u32 start, u32 n) {
    static HDC scratch;
    int k;
    if (!n) return;
    if (!scratch) scratch = CreateCompatibleDC(NULL);
    for (k = 0; k < g_wing_n; k++) {
        wing_bmp_t *b = &g_wing[k];
        HGDIOBJ prev;
        if (!b->bmp) continue;
        if (b->dc && GetCurrentObject(b->dc, OBJ_BITMAP) == (HGDIOBJ)b->bmp) {
            SetDIBColorTable(b->dc, start, n, (const RGBQUAD *)&g_wing_pal[start]);
            continue;
        }
        if (!scratch) continue;
        prev = SelectObject(scratch, b->bmp);
        if (!prev) continue;
        SetDIBColorTable(scratch, start, n, (const RGBQUAD *)&g_wing_pal[start]);
        SelectObject(scratch, prev);
    }
}

/* How many of the 256 are not black. A title screen fades in by ramping this
 * table up from zero, so "the picture is black" and "the fade has not started"
 * look identical on screen and different here. */
static void wing_report_palette(const char *who, u32 start, u32 n) {
    int nz = 0, i;
    for (i = 0; i < 256; i++) if (g_wing_pal[i] & 0x00FFFFFFu) nz++;
    fprintf(stderr, "    WinG: %s(%u..%u) -> %d of 256 entries lit\n",
            who, start, start + n - 1, nz);
}

static void bridge_WinGSetDIBColorTable(void) {
    u32 start = ARG(2), n = ARG(3), src = ARG(4), i;
    for (i = 0; i < n && start + i < 256; i++) {
        g_wing_pal[start + i] = MEM32(src + i * 4);
    }
    wing_apply_palette(start, n);
    wing_report_palette("SetDIBColorTable", start, n);
    eax = n;
    esp += 4 + 16;
}

static void bridge_WinGGetDIBColorTable(void) {
    u32 start = ARG(2), n = ARG(3), dst = ARG(4), i;
    for (i = 0; i < n && start + i < 256; i++)
        MEM32(dst + i * 4) = g_wing_pal[start + i];
    eax = n;
    esp += 4 + 16;
}

/*
 * RealizePalette, which on the display this was written for is what put the
 * colours on the screen.
 *
 * Treasure Cove -- the oldest build here, and the one closest to Neptune --
 * drives colour through a LOGICAL palette: eleven CreatePalette calls, a
 * SelectPalette, a RealizePalette, and 268 GetNearestPaletteIndex lookups to
 * turn RGB into indices. It tells WinG about only nineteen entries. On an 8bpp
 * display that was enough, because realizing the palette is what the other 237
 * came from.
 *
 * Here the blit is an 8bpp DIB onto a 32bpp screen, and the DIB's own colour
 * table is the only thing that decides what an index looks like -- so the title
 * screen came out correct in every shape and posterised into nineteen colours.
 *
 * So realizing a palette copies it into the DIB tables -- but only a FULL one.
 * Windows' stock palette has twenty entries, and every DC has it selected by
 * default; Gizmos & Gadgets realizes a palette once a frame and would otherwise
 * have had the first twenty entries of its own good table overwritten by it
 * every time. A game that drives colour this way builds all 256.
 */
static void palette_to_wing(HDC dc) {
    HPALETTE pal = (HPALETTE)GetCurrentObject(dc, OBJ_PAL);
    PALETTEENTRY pe[256];
    UINT n, i;
    int changed = 0;

    if (!pal) return;
    n = GetPaletteEntries(pal, 0, 256, pe);
    if (n < 256) return;
    for (i = 0; i < n && i < 256; i++) {
        u32 q;
        /* PALETTEENTRY is R,G,B,flags; RGBQUAD is B,G,R,0. */
        q = ((u32)pe[i].peRed << 16) | ((u32)pe[i].peGreen << 8) | (u32)pe[i].peBlue;
        if (g_wing_pal[i] == q) continue;
        g_wing_pal[i] = q;
        changed = 1;
    }
    if (changed) {
        wing_apply_palette(0, 256);
        wing_report_palette("RealizePalette", 0, 256);
    }
}

static void bridge_RealizePalette(void) {
    HDC dc = (HDC)(uintptr_t)ARG(1);
    eax = (u32)RealizePalette(dc);
    palette_to_wing(dc);
    esp += 4 + 4;
}

static void wing_blit(HDC dst, HDC src, int dx, int dy, int w, int h,
                      int sx, int sy, int sw, int sh) {
    static int said, last_w, last_h;
    if (!dst || !src) return;
    /* Where the picture lands in the window, once -- everything the game draws
     * is at this offset, so it is what turns a pixel in a screenshot back into
     * a coordinate to click. */
    if (!said || w != last_w || h != last_h) {
        said = 1; last_w = w; last_h = h;
        if (!gg_quiet("GG_QUIET_BRIDGES"))
            fprintf(stderr, "    WinG: blitting %dx%d to client (%d,%d) from (%d,%d)\n",
                    w, h, dx, dy, sx, sy);
        /*
         * The game sizes its window as if it were on Windows 3.1, where a
         * 640x480 window on a 640x480 screen had the whole screen inside it.
         * Windows 11 spends sixteen pixels of that on a border and thirty-nine
         * on a caption, so the picture loses its right and bottom edges.
         *
         * The first blit is the right moment to fix it: it is the first time
         * anyone knows how big the picture actually is, and the game has
         * finished moving its window by then.
         */
        HWND hw = WindowFromDC(dst);
        RECT cl, want;
        if (hw && GetClientRect(hw, &cl) &&
            (cl.right < dx + w || cl.bottom < dy + h)) {
            want.left = want.top = 0;
            want.right  = dx + w;
            want.bottom = dy + h;
            if (AdjustWindowRectEx(&want, (DWORD)GetWindowLongPtrA(hw, GWL_STYLE),
                                   GetMenu(hw) != NULL,
                                   (DWORD)GetWindowLongPtrA(hw, GWL_EXSTYLE))) {
                SetWindowPos(hw, NULL, 0, 0, want.right - want.left, want.bottom - want.top,
                             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                fprintf(stderr, "    WinG: client was %ldx%ld, grown to fit %dx%d\n",
                        cl.right, cl.bottom, dx + w, dy + h);
            }
        }
    }
    if (w == sw && h == sh) {
        BitBlt(dst, dx, dy, w, h, src, sx, sy, SRCCOPY);
    } else {
        SetStretchBltMode(dst, COLORONCOLOR);
        StretchBlt(dst, dx, dy, w, h, src, sx, sy, sw, sh, SRCCOPY);
    }
}

/* WinGCreateHalftonePalette(void) -> HPALETTE. The host still has the GDI
 * original of this one. */
static void bridge_WinGCreateHalftonePalette(void) {
    eax = (u32)(uintptr_t)CreateHalftonePalette(NULL);
    esp += 4;
}

/* WinGCreateHalftoneBrush(HDC, COLORREF, WING_DITHER_TYPE) -> HBRUSH.
 * ponytail: a solid brush of the requested colour. WinG dithered because 1994
 * hardware was 8bpp and a solid fill could not hit an arbitrary RGB; the blit
 * path here goes through a palette that can. */
static void bridge_WinGCreateHalftoneBrush(void) {
    eax = (u32)(uintptr_t)CreateSolidBrush((COLORREF)ARG(2));
    esp += 4 + 12;
}

/* WinGBitBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy) */
static void bridge_WinGBitBlt(void) {
    wing_blit((HDC)(uintptr_t)ARG(1), (HDC)(uintptr_t)ARG(6),
              (int)ARG(2), (int)ARG(3), (int)ARG(4), (int)ARG(5),
              (int)ARG(7), (int)ARG(8), (int)ARG(4), (int)ARG(5));
    eax = 1;
    esp += 4 + 32;
}

/* WinGStretchBlt(HDC dst,int x,int y,int w,int h, HDC src,int sx,int sy,int sw,int sh) */
static void bridge_WinGStretchBlt(void) {
    wing_blit((HDC)(uintptr_t)ARG(1), (HDC)(uintptr_t)ARG(6),
              (int)ARG(2), (int)ARG(3), (int)ARG(4), (int)ARG(5),
              (int)ARG(7), (int)ARG(8), (int)ARG(9), (int)ARG(10));
    eax = 1;
    esp += 4 + 40;
}

/* ===================================================================
 * LoadLibrary / GetProcAddress
 *
 * Two kinds of module get loaded at runtime, and neither can go to the host.
 * The game reaches for three modules at runtime and none of them can go to the
 * host. WING32.DLL does not exist any more, so we answer it ourselves (below).
 * WAVEMIX.DLL and DSOUND.DLL were optional even in 1996 -- the game carries its
 * own fallback for each, saying so on stderr ("Problem with WaveMix :
 * Converting all SFX to normal Waves") -- so failing those loads is the
 * historically accurate answer and sends it down a path it already has.
 * =================================================================== */

#define WING_HMODULE 0x571E0000u

static void bridge_LoadLibraryA(void) {
    const char *dll = (const char *)(uintptr_t)ADDR(ARG(1));
    if (_stricmp(dll, "WING32.DLL") == 0) {
        eax = WING_HMODULE;
    } else {
        eax = 0;    /* WAVEMIX / DSOUND: let the game take its own fallback */
    }
    fprintf(stderr, "    LoadLibraryA('%s') -> 0x%X\n", dll, eax);
    esp += 4 + 4;
}

static void bridge_FreeLibrary(void) { eax = 1; esp += 4 + 4; }

/*
 * WING32.DLL's export table, ordinals included.
 *
 * The game resolves all of these by ORDINAL -- there is not one WinG name
 * anywhere in its strings -- so the numbers matter as much as the names. They
 * are WinG 1.0's own, read off the WING32.DLL that ships in the game's own
 * Apps/wing redistributable: alphabetical, which is why WinGBitBlt is 1.
 */
static const struct { int ord; const char *name; void (*fn)(void); int argc; } g_wing_exports[] = {
    { 1, "WinGBitBlt",                bridge_WinGBitBlt,                 8},
    { 2, "WinGCreateBitmap",          bridge_WinGCreateBitmap,           3},
    { 3, "WinGCreateDC",              bridge_WinGCreateDC,               0},
    { 4, "WinGCreateHalftoneBrush",   bridge_WinGCreateHalftoneBrush,    3},
    { 5, "WinGCreateHalftonePalette", bridge_WinGCreateHalftonePalette,  0},
    { 6, "WinGGetDIBColorTable",      bridge_WinGGetDIBColorTable,       4},
    { 7, "WinGGetDIBPointer",         bridge_WinGGetDIBPointer,          2},
    { 8, "WinGRecommendDIBFormat",    bridge_WinGRecommendDIBFormat,     1},
    { 9, "WinGSetDIBColorTable",      bridge_WinGSetDIBColorTable,       4},
    {10, "WinGStretchBlt",            bridge_WinGStretchBlt,            10},
};


static void bridge_GetProcAddress(void) {
    /* A name argument whose high word is zero is an ordinal, not a pointer --
     * MAKEINTRESOURCE. Dereferencing it is a fault, which is how this surfaced:
     * the very first call asks for ordinal 3. */
    u32 want = ARG(2);
    int by_ord = want < 0x10000u;
    const char *name = by_ord ? NULL : (const char *)(uintptr_t)ADDR(want);
    size_t i;

    eax = 0;
    if (ARG(1) == WING_HMODULE) {
        for (i = 0; i < sizeof(g_wing_exports) / sizeof(g_wing_exports[0]); i++) {
            int hit = by_ord ? (g_wing_exports[i].ord == (int)want)
                             : (strcmp(name, g_wing_exports[i].name) == 0);
            if (hit) {
                eax = alloc_bridge(g_wing_exports[i].name, g_wing_exports[i].fn,
                                   g_wing_exports[i].argc);
                break;
            }
        }
    } else if (!by_ord) {
        /*
         * A real Win32 name, resolved at runtime rather than imported.
         *
         * The address has to be one the lifted code can CALL, so it cannot be
         * the host's -- it has to be a bridge. Any name already bound out of
         * the import table has one, with the right argument count on it, so
         * that is what comes back.
         *
         * Treasure Cove asks for "GetProcAddress" itself and then calls the
         * result, which is a perfectly ordinary way to check you are on the
         * Win32 you think you are. Answering 0 sent it to a call through null,
         * and it gave up with "Could not find Resource File!" -- which is what
         * a bad answer here looks like from the outside: nothing to do with
         * resources.
         */
        for (i = 0; i < (size_t)num_bridges; i++) {
            if (bridges[i].name && strcmp(name, bridges[i].name) == 0) {
                eax = BRIDGE_BASE + (u32)i;
                break;
            }
        }
    }
    if (by_ord)
        fprintf(stderr, "    GetProcAddress(0x%X, ordinal %u) -> 0x%X\n", ARG(1), want, eax);
    else
        fprintf(stderr, "    GetProcAddress(0x%X, '%s') -> 0x%X\n", ARG(1), name, eax);
    esp += 4 + 8;
}

/* ===================================================================
 * The image's own resources
 *
 * The first run put up a MessageBox with nothing in it, because the text comes
 * from LoadStringA and LoadStringA was a stub. The game never really becomes a
 * module here -- we mapped its sections ourselves -- so the host cannot answer
 * these, and the resource directory is walked in the mapped image instead.
 *
 * Three levels: type -> name -> language, each an IMAGE_RESOURCE_DIRECTORY of
 * 16 bytes followed by 8-byte entries, high bit of the name meaning "string"
 * and high bit of the offset meaning "subdirectory".
 * =================================================================== */

static u32 res_root(void) {
    u32 nt = GG_IMAGE_BASE + MEM32(GG_IMAGE_BASE + 0x3C);
    u32 rva = MEM32(nt + 0x88);            /* DataDirectory[2] == RESOURCE */
    return rva ? GG_IMAGE_BASE + rva : 0;
}

/* Compare a directory entry's name against what was asked for. `want` is either
 * an integer id (MAKEINTRESOURCE, < 0x10000) or a VA of an ANSI string; entry
 * names in the image are UTF-16 with a leading length word. */
static int res_name_match(u32 root, u32 entry_name, u32 want) {
    if (!(entry_name & 0x80000000u))            /* entry is an id */
        return want < 0x10000u && entry_name == want;
    if (want < 0x10000u) return 0;              /* id asked for, name found */
    {
        u32 p = root + (entry_name & 0x7FFFFFFFu);
        u32 n = MEM16(p), i;
        const char *w = (const char *)(uintptr_t)ADDR(want);
        for (i = 0; i < n; i++) {
            u16 c = MEM16(p + 2 + i * 2);
            char a = w[i];
            if (!a) return 0;
            if (c >= 'a' && c <= 'z') c = (u16)(c - 32);   /* resource names are case-insensitive */
            if (a >= 'a' && a <= 'z') a = (char)(a - 32);
            if (c != (u16)(unsigned char)a) return 0;
        }
        return w[n] == 0;
    }
}

/* Walk one directory looking for `want`; returns the entry's offset field. */
static u32 res_find_entry(u32 root, u32 dir, u32 want, int any) {
    u32 named = MEM16(dir + 12), ids = MEM16(dir + 14), i, n = named + ids;
    for (i = 0; i < n; i++) {
        u32 e = dir + 16 + i * 8;
        if (any || res_name_match(root, MEM32(e), want))
            return MEM32(e + 4);
    }
    return 0;
}

/* Returns the VA of the IMAGE_RESOURCE_DATA_ENTRY, or 0. */
static u32 res_find(u32 type, u32 name) {
    u32 root = res_root(), off;
    if (!root) return 0;
    off = res_find_entry(root, root, type, 0);
    if (!(off & 0x80000000u)) return 0;
    off = res_find_entry(root, root + (off & 0x7FFFFFFFu), name, 0);
    if (!(off & 0x80000000u)) return 0;
    off = res_find_entry(root, root + (off & 0x7FFFFFFFu), 0, 1);   /* first language */
    if (off & 0x80000000u) return 0;
    return root + off;
}

static void bridge_FindResourceA(void) {
    eax = res_find(ARG(3), ARG(2));
    if (!eax) fprintf(stderr, "    FindResourceA(type=0x%X, name=0x%X) -> not found\n", ARG(3), ARG(2));
    esp += 4 + 12;
}
static void bridge_LoadResource(void) {
    eax = ARG(2) ? GG_IMAGE_BASE + MEM32(ARG(2)) : 0;   /* OffsetToData is an RVA */
    esp += 4 + 8;
}
static void bridge_LockResource(void) { eax = ARG(1); esp += 4 + 4; }
static void bridge_FreeResource(void) { eax = 0; esp += 4 + 4; }

/*
 * String tables come in bundles of sixteen under RT_STRING, keyed (id/16)+1,
 * each entry a length word followed by that many UTF-16 characters.
 */
static void bridge_LoadStringA(void) {
    u32 id = ARG(2), buf = ARG(3), cch = ARG(4);
    u32 ent = res_find(6 /* RT_STRING */, id / 16 + 1);
    u32 p, i, n;

    eax = 0;
    if (buf && cch) MEM8(buf) = 0;
    if (!ent || !buf || !cch) { esp += 4 + 16; return; }

    p = GG_IMAGE_BASE + MEM32(ent);
    for (i = 0; i < id % 16; i++) p += 2 + MEM16(p) * 2;
    n = MEM16(p);
    if (n > cch - 1) n = cch - 1;
    for (i = 0; i < n; i++) MEM8(buf + i) = (u8)MEM16(p + 2 + i * 2);
    MEM8(buf + n) = 0;
    eax = n;
    esp += 4 + 16;
}

/* Worth seeing on the console as well as on screen: a message box is where this
 * game says why it will not start, and it blocks until someone clicks it. */
static void bridge_MessageBoxA(void) {
    const char *text = ARG(2) ? (const char *)(uintptr_t)ADDR(ARG(2)) : "";
    const char *cap  = ARG(3) ? (const char *)(uintptr_t)ADDR(ARG(3)) : "";
    fprintf(stderr, "\n*** MessageBox [%s]: %s\n\n", cap, text);
    fflush(stderr);
    if (GetEnvironmentVariableA("GG_NO_DIALOGS", NULL, 0)) eax = IDOK;
    else eax = MessageBoxA((HWND)(uintptr_t)ARG(1), text, cap, ARG(4));
    esp += 4 + 16;
}

/* ===================================================================
 * Callbacks and structures that differ between 32- and 64-bit
 * =================================================================== */

/* The game's window procedure, as a VA into lifted code. */
static u32 g_lifted_wndproc;

static LRESULT CALLBACK gg_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    recomp_func_t fn;
    if (!g_lifted_wndproc) return DefWindowProcA(h, m, w, l);
    fn = recomp_lookup(g_lifted_wndproc);
    if (!fn) {
        fprintf(stderr, "    wndproc 0x%08X was not lifted\n", g_lifted_wndproc);
        return DefWindowProcA(h, m, w, l);
    }
    /* MM_WOM_DONE (0x3BD) hands back the WAVEHDR the driver finished with,
     * which is the host-side twin -- see audio.c. */
    if (m == 0x3BD) l = (LPARAM)wave_lparam_to_game((uintptr_t)l);

    /* stdcall, right to left: the callee pops all four. */
    PUSH32(esp, (u32)l);
    PUSH32(esp, (u32)w);
    PUSH32(esp, (u32)m);
    PUSH32(esp, (u32)(uintptr_t)h);
    PUSH32(esp, RECOMP_RETADDR);
    fn();
    return (LRESULT)(LONG)eax;
}

/* WNDCLASSA is ten 4-byte fields in the game and ten pointer-or-int fields
 * here, so it is copied across by hand -- and lpfnWndProc is replaced, because
 * the game's is a VA with no machine code behind it. */
static void bridge_RegisterClassA(void) {
    u32 p = ARG(1);
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.style         = MEM32(p + 0);
    g_lifted_wndproc = MEM32(p + 4);
    wc.lpfnWndProc   = gg_wndproc;
    wc.cbClsExtra    = (int)MEM32(p + 8);
    wc.cbWndExtra    = (int)MEM32(p + 12);
    wc.hInstance     = (HINSTANCE)(uintptr_t)MEM32(p + 16);
    wc.hIcon         = (HICON)(uintptr_t)MEM32(p + 20);
    wc.hCursor       = (HCURSOR)(uintptr_t)MEM32(p + 24);
    wc.hbrBackground = (HBRUSH)(uintptr_t)MEM32(p + 28);
    wc.lpszMenuName  = MEM32(p + 32) ? (LPCSTR)(uintptr_t)ADDR(MEM32(p + 32)) : NULL;
    wc.lpszClassName = MEM32(p + 36) ? (LPCSTR)(uintptr_t)ADDR(MEM32(p + 36)) : NULL;

    eax = RegisterClassA(&wc);
    fprintf(stderr, "    RegisterClassA('%s') wndproc=0x%08X -> 0x%X\n",
            wc.lpszClassName ? wc.lpszClassName : "(null)", g_lifted_wndproc, eax);
    esp += 4 + 4;
}

/* WNDCLASSEXA: twelve 4-byte fields in the game, the same twelve as pointers
 * here. Same story as RegisterClassA, two fields longer. */
static void bridge_RegisterClassExA(void) {
    u32 p = ARG(1);
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = MEM32(p + 4);
    g_lifted_wndproc = MEM32(p + 8);
    wc.lpfnWndProc   = gg_wndproc;
    wc.cbClsExtra    = (int)MEM32(p + 12);
    wc.cbWndExtra    = (int)MEM32(p + 16);
    wc.hInstance     = (HINSTANCE)(uintptr_t)MEM32(p + 20);
    wc.hIcon         = (HICON)(uintptr_t)MEM32(p + 24);
    wc.hCursor       = (HCURSOR)(uintptr_t)MEM32(p + 28);
    wc.hbrBackground = (HBRUSH)(uintptr_t)MEM32(p + 32);
    wc.lpszMenuName  = MEM32(p + 36) ? (LPCSTR)(uintptr_t)ADDR(MEM32(p + 36)) : NULL;
    wc.lpszClassName = MEM32(p + 40) ? (LPCSTR)(uintptr_t)ADDR(MEM32(p + 40)) : NULL;
    wc.hIconSm       = (HICON)(uintptr_t)MEM32(p + 44);

    eax = RegisterClassExA(&wc);
    fprintf(stderr, "    RegisterClassExA('%s') wndproc=0x%08X -> 0x%X\n",
            wc.lpszClassName ? wc.lpszClassName : "(null)", g_lifted_wndproc, eax);
    esp += 4 + 4;
}

static void bridge_CreateWindowExA(void) {
    const char *cls  = ARG(2) > 0xFFFF ? (const char *)(uintptr_t)ADDR(ARG(2)) : "(atom)";
    const char *name = ARG(3) ? (const char *)(uintptr_t)ADDR(ARG(3)) : NULL;
    HWND h = CreateWindowExA(ARG(1),
                             ARG(2) > 0xFFFF ? (LPCSTR)(uintptr_t)ADDR(ARG(2)) : (LPCSTR)(uintptr_t)ARG(2),
                             name, ARG(4),
                             (int)ARG(5), (int)ARG(6), (int)ARG(7), (int)ARG(8),
                             (HWND)(uintptr_t)ARG(9), (HMENU)(uintptr_t)ARG(10),
                             (HINSTANCE)(uintptr_t)ARG(11), (void *)(uintptr_t)ARG(12));
    if (!h) {
        fprintf(stderr, "    CreateWindowExA('%s', style=0x%X, %dx%d at %d,%d, hInst=0x%X) FAILED (%lu)\n",
                cls, ARG(4), (int)ARG(7), (int)ARG(8), (int)ARG(5), (int)ARG(6),
                ARG(11), GetLastError());
    } else {
        fprintf(stderr, "    CreateWindowExA('%s', style=0x%X) -> 0x%08X, client %dx%d\n",
                cls, ARG(4), (u32)(uintptr_t)h, (int)ARG(7), (int)ARG(8));
    }
    eax = (u32)(uintptr_t)h;
    esp += 4 + 48;
}

/* MSG: 28 bytes in the game, 48 here. */
static void msg_to_game(const MSG *m, u32 p) {
    MEM32(p +  0) = (u32)(uintptr_t)m->hwnd;
    MEM32(p +  4) = m->message;
    MEM32(p +  8) = (u32)m->wParam;
    MEM32(p + 12) = (u32)m->lParam;
    MEM32(p + 16) = m->time;
    MEM32(p + 20) = (u32)m->pt.x;
    MEM32(p + 24) = (u32)m->pt.y;
}

static void msg_from_game(u32 p, MSG *m) {
    memset(m, 0, sizeof(*m));
    m->hwnd    = (HWND)(uintptr_t)MEM32(p + 0);
    m->message = MEM32(p + 4);
    m->wParam  = MEM32(p + 8);
    m->lParam  = (LONG_PTR)(LONG)MEM32(p + 12);
    m->time    = MEM32(p + 16);
    m->pt.x    = (LONG)MEM32(p + 20);
    m->pt.y    = (LONG)MEM32(p + 24);
}

static void bridge_PeekMessageA(void) {
    MSG m;
    eax = PeekMessageA(&m, (HWND)(uintptr_t)ARG(2), ARG(3), ARG(4), ARG(5));
    if (eax) msg_to_game(&m, ARG(1));
    esp += 4 + 20;
}

static void bridge_TranslateMessage(void) {
    MSG m; msg_from_game(ARG(1), &m);
    eax = TranslateMessage(&m);
    esp += 4 + 4;
}

static void bridge_DispatchMessageA(void) {
    MSG m; msg_from_game(ARG(1), &m);
    eax = (u32)DispatchMessageA(&m);
    esp += 4 + 4;
}

/* PAINTSTRUCT: 64 bytes in the game, 72 here (hdc grew). */
static void bridge_BeginPaint(void) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint((HWND)(uintptr_t)ARG(1), &ps);
    u32 p = ARG(2);
    MEM32(p +  0) = (u32)(uintptr_t)dc;
    MEM32(p +  4) = ps.fErase;
    MEM32(p +  8) = ps.rcPaint.left;
    MEM32(p + 12) = ps.rcPaint.top;
    MEM32(p + 16) = ps.rcPaint.right;
    MEM32(p + 20) = ps.rcPaint.bottom;
    MEM32(p + 24) = ps.fRestore;
    MEM32(p + 28) = ps.fIncUpdate;
    eax = (u32)(uintptr_t)dc;
    esp += 4 + 8;
}

static void bridge_EndPaint(void) {
    PAINTSTRUCT ps;
    u32 p = ARG(2);
    memset(&ps, 0, sizeof(ps));
    ps.hdc = (HDC)(uintptr_t)MEM32(p + 0);
    eax = EndPaint((HWND)(uintptr_t)ARG(1), &ps);
    esp += 4 + 8;
}

/* ===================================================================
 * Dialogs
 *
 * The game will not start without one: after the sound and MIDI probes it puts
 * up a dialog, and with DialogBoxParamA stubbed it read the -1 as a refusal,
 * destroyed its window and exited. That is as far as the first run got.
 *
 * Two halves, and each is a trick this file already knows:
 *
 *   - The TEMPLATE is a resource, and resources come out of the image we
 *     mapped, not the host module -- res_find() above already walks it. A
 *     DLGTEMPLATE is version-independent bytes, and it is sitting at a real
 *     address, so DialogBoxIndirectParamA can take it as it stands.
 *   - The PROCEDURE is a VA in lifted code with no machine code behind it, so
 *     it goes through a trampoline, exactly like the window procedure.
 *
 * Modal dialogs nest -- a dialog can put up another -- so the lifted procedure
 * is a small stack rather than one global.
 * =================================================================== */

#define MAX_DLG_DEPTH 8
static u32 g_dlg_stack[MAX_DLG_DEPTH];
static int g_dlg_depth;

static INT_PTR CALLBACK gg_dlgproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    recomp_func_t fn;
    u32 proc = g_dlg_depth > 0 ? g_dlg_stack[g_dlg_depth - 1] : 0;
    if (!proc) return FALSE;
    fn = recomp_lookup(proc);
    if (!fn) {
        fprintf(stderr, "    dlgproc 0x%08X was not lifted\n", proc);
        return FALSE;
    }
    /* stdcall, right to left: the callee pops all four. */
    PUSH32(esp, (u32)l);
    PUSH32(esp, (u32)w);
    PUSH32(esp, (u32)m);
    PUSH32(esp, (u32)(uintptr_t)h);
    PUSH32(esp, RECOMP_RETADDR);
    fn();
    return (INT_PTR)(LONG)eax;
}

/* DialogBoxParamA(HINSTANCE, LPCSTR tmpl, HWND parent, DLGPROC, LPARAM init) */
static void bridge_DialogBoxParamA(void) {
    u32 tmpl_arg = ARG(2);
    u32 ent = res_find(5 /* RT_DIALOG */, tmpl_arg);
    const void *tmpl;

    if (!ent) {
        fprintf(stderr, "    DialogBoxParamA: no RT_DIALOG resource 0x%X in the image\n",
                tmpl_arg);
        eax = (u32)-1;
        esp += 4 + 20;
        return;
    }
    if (g_dlg_depth >= MAX_DLG_DEPTH) {
        fprintf(stderr, "    DialogBoxParamA: nested %d deep, refusing\n", g_dlg_depth);
        eax = (u32)-1;
        esp += 4 + 20;
        return;
    }

    /* OffsetToData is an RVA, and the image is mapped at its own base, so the
     * template bytes are already where a host API can read them. */
    tmpl = (const void *)(uintptr_t)ADDR(GG_IMAGE_BASE + MEM32(ent));

    g_dlg_stack[g_dlg_depth++] = ARG(4);
    eax = (u32)(LONG)DialogBoxIndirectParamA(GetModuleHandleA(NULL),
                                             (LPCDLGTEMPLATEA)tmpl,
                                             (HWND)(uintptr_t)ARG(3),
                                             gg_dlgproc,
                                             (LPARAM)(LONG)ARG(5));
    g_dlg_depth--;

    fprintf(stderr, "    DialogBoxParamA(res=0x%X, dlgproc=0x%08X) -> %d\n",
            tmpl_arg, ARG(4), (int)eax);
    esp += 4 + 20;
}

/* Callback-taking APIs we have not needed yet. Loud, so the first run that
 * reaches one says so instead of quietly doing nothing. */
static void bridge_EnumThreadWindows(void) { fprintf(stderr, "    EnumThreadWindows: unimplemented\n"); eax = 0; esp += 4 + 12; }


/* ===================================================================
 * Miles Sound System (mss32.dll)
 *
 * Treasure MathStorm does not use waveOut or MCI at all: its audio goes
 * through Miles, twenty-four `_AIL_*` entry points, decorated stdcall. A real
 * MSS32.DLL ships on its own disc and is a 32-bit PE, so it could in principle
 * be loaded for real -- but it talks to a 1996 sound stack underneath, and the
 * game only ever asks it four questions: did you start, here is a sample, is it
 * finished, stop.
 *
 * ponytail: so this is a bookkeeping shim. Handles are small integers, a sample
 * is "playing" until asked and then done, and the game's sequencing runs at the
 * right pace because AIL_sample_status answers honestly about a clock rather
 * than about a speaker. Nothing comes out. When it should, this is where a real
 * mixer goes -- the surface is already the right shape, and every entry point
 * that returns a pointer already comes from the low heap.
 * =================================================================== */

#define AIL_MAX_SAMPLES 32
#define AIL_FIRST_HANDLE 0x4110u     /* not 0, and not a plausible pointer */

typedef struct {
    int   used;
    int   playing;
    u32   user_data[8];
    DWORD started_ms;
    DWORD length_ms;
} ail_sample_t;

static ail_sample_t g_ail[AIL_MAX_SAMPLES];
static int g_ail_up;

static ail_sample_t *ail_get(u32 h) {
    u32 i = h - AIL_FIRST_HANDLE;
    return (i < AIL_MAX_SAMPLES && g_ail[i].used) ? &g_ail[i] : NULL;
}

static void bridge_AIL_startup(void)  { g_ail_up = 1; eax = 1; esp += 4; }
static void bridge_AIL_shutdown(void) { g_ail_up = 0; eax = 0; esp += 4; }

/* AIL_allocate_sample_handle(driver) -> HSAMPLE */
static void bridge_AIL_allocate_sample_handle(void) {
    int i;
    eax = 0;
    for (i = 0; i < AIL_MAX_SAMPLES; i++) {
        if (g_ail[i].used) continue;
        memset(&g_ail[i], 0, sizeof(g_ail[i]));
        g_ail[i].used = 1;
        eax = AIL_FIRST_HANDLE + (u32)i;
        break;
    }
    esp += 4 + 4;
}

static void bridge_AIL_release_sample_handle(void) {
    ail_sample_t *smp = ail_get(ARG(1));
    if (smp) memset(smp, 0, sizeof(*smp));
    esp += 4 + 4;
}

/*
 * How long the game thinks a sound lasts.
 *
 * It waits on AIL_sample_status, so answering "done" immediately would run
 * every cutscene at whatever speed the loop happens to spin at. There is no
 * length to read -- the buffer it was handed is raw PCM with a rate set
 * separately -- so the length is taken from the bytes and the playback rate
 * when both are known, and otherwise left at zero, which reads as done.
 */
static void ail_begin(ail_sample_t *smp) {
    smp->playing    = 1;
    smp->started_ms = GetTickCount();
}

/* AIL_start_sample / stop / resume */
static void bridge_AIL_start_sample(void) {
    ail_sample_t *smp = ail_get(ARG(1));
    if (smp) ail_begin(smp);
    esp += 4 + 4;
}
static void bridge_AIL_stop_sample(void) {
    ail_sample_t *smp = ail_get(ARG(1));
    if (smp) smp->playing = 0;
    esp += 4 + 4;
}
static void bridge_AIL_resume_sample(void) {
    ail_sample_t *smp = ail_get(ARG(1));
    if (smp) ail_begin(smp);
    esp += 4 + 4;
}

/* AIL_sample_status(HSAMPLE) -> 2 DONE, 4 PLAYING (SMP_DONE / SMP_PLAYING) */
static void bridge_AIL_sample_status(void) {
    ail_sample_t *smp = ail_get(ARG(1));
    eax = 2;
    if (smp && smp->playing) {
        if (smp->length_ms && GetTickCount() - smp->started_ms < smp->length_ms) eax = 4;
        else smp->playing = 0;
    }
    esp += 4 + 4;
}

/* AIL_set_sample_address(HSAMPLE, void *start, u32 len) -- the PCM itself. */
static void bridge_AIL_set_sample_address(void) {
    ail_sample_t *smp = ail_get(ARG(1));
    if (smp) smp->length_ms = ARG(3) ? ARG(3) / 22u : 0;   /* ~22 kHz, 8-bit mono */
    esp += 4 + 12;
}

/* AIL_set_sample_playback_rate(HSAMPLE, int hz) -- now the length is knowable. */
static void bridge_AIL_set_sample_playback_rate(void) {
    esp += 4 + 8;
}

/* The sample's own scratch word: the game stores an index in it and reads it
 * back, so it has to survive rather than be dropped. */
static void bridge_AIL_set_sample_user_data(void) {
    ail_sample_t *smp = ail_get(ARG(1));
    if (smp && ARG(2) < 8) smp->user_data[ARG(2)] = ARG(3);
    esp += 4 + 12;
}
static void bridge_AIL_sample_user_data(void) {
    ail_sample_t *smp = ail_get(ARG(1));
    eax = (smp && ARG(2) < 8) ? smp->user_data[ARG(2)] : 0;
    esp += 4 + 8;
}

/* Miles' own allocator. Like GlobalAlloc, what it returns has to fit in 32
 * bits, so it comes from the low heap. */
static void bridge_AIL_mem_alloc_lock(void) { eax = gg_heap_alloc(ARG(1)); esp += 4 + 4; }
static void bridge_AIL_mem_free_lock(void)  { gg_heap_free(ARG(1)); eax = 1; esp += 4 + 4; }

/* AIL_allocate_file_sample(void *file, u32 len, int block) -> void*: hands back
 * a pointer INTO the file image, past the header. Nothing here parses the
 * format, so it answers with the buffer it was given. */
static void bridge_AIL_allocate_file_sample(void) { eax = ARG(1); esp += 4 + 12; }

static void bridge_AIL_init_sample(void)         { eax = 1; esp += 4 + 4; }
static void bridge_AIL_load_sample_buffer(void)  { esp += 4 + 16; }
static void bridge_AIL_sample_buffer_ready(void) { eax = 0; esp += 4 + 4; }
static void bridge_AIL_set_sample_loop_count(void) { esp += 4 + 8; }
static void bridge_AIL_set_sample_type(void)     { eax = 1; esp += 4 + 12; }
static void bridge_AIL_set_preference(void)      { eax = 0; esp += 4 + 8; }
static void bridge_AIL_waveOutOpen(void)         { eax = 0; esp += 4 + 16; }
static void bridge_AIL_waveOutClose(void)        { eax = 0; esp += 4 + 4; }

/* TranslateAcceleratorA takes an MSG, 28 bytes in the game and 48 here -- the
 * same translation DispatchMessageA needs, so it cannot be a pass-through. */
static void bridge_TranslateAcceleratorA(void) {
    MSG m;
    msg_from_game(ARG(3), &m);
    eax = (u32)TranslateAcceleratorA((HWND)(uintptr_t)ARG(1),
                                     (HACCEL)(uintptr_t)ARG(2), &m);
    esp += 4 + 12;
}

/* SetUnhandledExceptionFilter: the CRT installs one and we never raise, so the
 * honest answer is "there was none before". */
static void bridge_SetUnhandledExceptionFilter(void) { eax = 0; esp += 4 + 4; }

/* ===================================================================
 * Smacker (smackw32.dll)
 *
 * MathStorm plays four .SMK cutscenes -- the opening, the parachute, the
 * closing -- through RAD's Smacker runtime, linked by ordinal.
 *
 * ponytail: declined, not decoded. SmackOpen answers 0, which is the same thing
 * the game sees on a machine whose disc is not in the drive, and it has a path
 * for that already: the 16-bit build on the same disc ships without the 32-bit
 * runtime and has to cope. Decoding Smacker is a real piece of work and buys a
 * cutscene; if it ever matters, everything below becomes a thin wrapper over a
 * decoder and the rest of the engine does not change.
 * =================================================================== */

static void bridge_SmackOpen(void)         { eax = 0; esp += 4 + 12; }
static void bridge_SmackClose(void)        { esp += 4 + 4; }
static void bridge_SmackDoFrame(void)      { eax = 0; esp += 4 + 4; }
static void bridge_SmackNextFrame(void)    { eax = 0; esp += 4 + 4; }
static void bridge_SmackToBuffer(void)     { eax = 0; esp += 4 + 28; }
static void bridge_SmackToBufferRect(void) { eax = 0; esp += 4 + 8; }
static void bridge_SmackSoundCheck(void)   { eax = 0; esp += 4; }
static void bridge_SmackWait(void)         { eax = 0; esp += 4 + 4; }
static void bridge_SmackSoundUseMSS(void)  { eax = 1; esp += 4 + 4; }

/* Ordinal, name (for the log and the trace), handler, argument dwords. The
 * ordinals are SMACKW32.DLL's own, read off the copy on this game's disc. */
static const struct { u32 ord; const char *name; void (*fn)(void); int argc; }
g_smacker[] = {
    {14, "SmackOpen",        bridge_SmackOpen,         3},
    {18, "SmackClose",       bridge_SmackClose,        1},
    {19, "SmackDoFrame",     bridge_SmackDoFrame,      1},
    {21, "SmackNextFrame",   bridge_SmackNextFrame,    1},
    {23, "SmackToBuffer",    bridge_SmackToBuffer,     7},
    {28, "SmackToBufferRect",bridge_SmackToBufferRect, 2},
    {31, "SmackSoundCheck",  bridge_SmackSoundCheck,   0},
    {32, "SmackWait",        bridge_SmackWait,         1},
    {33, "SmackSoundUseMSS", bridge_SmackSoundUseMSS,  1},
};

/* ===================================================================
 * Wiring
 * =================================================================== */

void setup_iat_bridges(void) {
    HMODULE h;
    size_t i;

    if (gg_quiet("GG_QUIET_BRIDGES")) g_verbose = 0;

    for (i = 0; i < sizeof(g_passthrough) / sizeof(g_passthrough[0]); i++) {
        void *real;
        h = GetModuleHandleA(g_passthrough[i].dll);
        if (!h) h = LoadLibraryA(g_passthrough[i].dll);
        real = h ? (void *)GetProcAddress(h, g_passthrough[i].name) : NULL;
        if (!real) {
            fprintf(stderr, "  BRIDGE: host has no %s!%s\n",
                    g_passthrough[i].dll, g_passthrough[i].name);
            continue;
        }
        bind(g_passthrough[i].name, generic_bridge, real, g_passthrough[i].argc);
    }

    bind("GlobalAlloc",              bridge_GlobalAlloc,              NULL, 2);
    bind("GlobalFree",               bridge_GlobalFree,               NULL, 1);
    bind("GlobalLock",               bridge_GlobalLock,               NULL, 1);
    bind("GlobalUnlock",             bridge_GlobalUnlock,             NULL, 1);
    bind("VirtualAlloc",             bridge_VirtualAlloc,             NULL, 4);
    bind("VirtualFree",              bridge_VirtualFree,              NULL, 3);
    bind("GlobalSize",               bridge_GlobalSize,               NULL, 1);
    bind("GlobalMemoryStatus",       bridge_GlobalMemoryStatus,       NULL, 1);
    bind("GlobalHandle",             bridge_GlobalHandle,             NULL, 1);
    bind("GlobalCompact",            bridge_GlobalCompact,            NULL, 1);
    bind("LocalAlloc",               bridge_LocalAlloc,               NULL, 2);
    bind("LocalFree",                bridge_LocalFree,                NULL, 1);
    bind("wsprintfA",                bridge_wsprintfA,                NULL, 0);

    bind("InitializeCriticalSection", bridge_InitializeCriticalSection, NULL, 1);
    bind("EnterCriticalSection",      bridge_EnterCriticalSection,      NULL, 1);
    bind("LeaveCriticalSection",      bridge_LeaveCriticalSection,      NULL, 1);

    bind("GetSystemMetrics",         bridge_GetSystemMetrics,         NULL, 1);
    bind("GetDeviceCaps",            bridge_GetDeviceCaps,            NULL, 2);
    bind("RealizePalette",           bridge_RealizePalette,           NULL, 1);
    bind("GetWindowRect",            bridge_GetWindowRect,            NULL, 2);
    bind("_lopen",                   bridge_lopen,                    NULL, 2);
    bind("OpenFile",                 bridge_OpenFile,                 NULL, 3);
    bind("GetModuleFileNameA",       bridge_GetModuleFileNameA,       NULL, 3);
    bind("GetPrivateProfileStringA", bridge_GetPrivateProfileStringA, NULL, 6);
    bind("CreateFileA",              bridge_CreateFileA,              NULL, 7);
    bind("GetFileAttributesA",       bridge_GetFileAttributesA,       NULL, 1);
    bind("GetCommandLineA",          bridge_GetCommandLineA,          NULL, 0);
    bind("GetEnvironmentStrings",    bridge_GetEnvironmentStrings,    NULL, 0);
    bind("GetStartupInfoA",          bridge_GetStartupInfoA,          NULL, 1);
    bind("GetModuleHandleA",         bridge_GetModuleHandleA,         NULL, 1);
    bind("ExitProcess",              bridge_ExitProcess,              NULL, 1);
    bind("RaiseException",           bridge_RaiseException,           NULL, 4);
    bind("RtlUnwind",                bridge_RtlUnwind,                NULL, 4);
    bind("UnhandledExceptionFilter", bridge_UnhandledExceptionFilter, NULL, 1);
    bind("SetConsoleCtrlHandler",    bridge_SetConsoleCtrlHandler,    NULL, 2);

    bind("LoadLibraryA",             bridge_LoadLibraryA,             NULL, 1);
    bind("FreeLibrary",              bridge_FreeLibrary,              NULL, 1);
    bind("GetProcAddress",           bridge_GetProcAddress,           NULL, 2);

    bind("FindResourceA",            bridge_FindResourceA,            NULL, 3);
    bind("LoadResource",             bridge_LoadResource,             NULL, 2);
    bind("LockResource",             bridge_LockResource,             NULL, 1);
    bind("FreeResource",             bridge_FreeResource,             NULL, 1);
    bind("LoadStringA",              bridge_LoadStringA,              NULL, 4);
    bind("MessageBoxA",              bridge_MessageBoxA,              NULL, 4);

    bind("RegisterClassA",           bridge_RegisterClassA,           NULL, 1);
    bind("RegisterClassExA",         bridge_RegisterClassExA,         NULL, 1);
    bind("CreateWindowExA",          bridge_CreateWindowExA,          NULL, 12);
    bind("PeekMessageA",             bridge_PeekMessageA,             NULL, 5);
    bind("TranslateMessage",         bridge_TranslateMessage,         NULL, 1);
    bind("DispatchMessageA",         bridge_DispatchMessageA,         NULL, 1);
    bind("BeginPaint",               bridge_BeginPaint,               NULL, 2);
    bind("EndPaint",                 bridge_EndPaint,                 NULL, 2);
    bind("DialogBoxParamA",          bridge_DialogBoxParamA,          NULL, 5);
    bind("EnumThreadWindows",        bridge_EnumThreadWindows,        NULL, 3);

    bind("waveOutOpen",              bridge_waveOutOpen,              NULL, 6);
    bind("waveOutPrepareHeader",     bridge_waveOutPrepareHeader,     NULL, 3);
    bind("waveOutUnprepareHeader",   bridge_waveOutUnprepareHeader,   NULL, 3);
    bind("waveOutWrite",             bridge_waveOutWrite,             NULL, 3);
    bind("mciSendCommandA",          bridge_mciSendCommandA,          NULL, 4);

    bind("TranslateAcceleratorA",    bridge_TranslateAcceleratorA,    NULL, 3);
    bind("SetUnhandledExceptionFilter", bridge_SetUnhandledExceptionFilter, NULL, 1);

    /*
     * WinG, bound by NAME straight out of the import table.
     *
     * Gizmos & Gadgets loads WING32.DLL by hand and asks for each entry point
     * by ordinal, so its eight bridges are handed out by GetProcAddress above.
     * MathStorm links the same DLL statically, so the names are in its IAT and
     * the ordinary path binds them. Same eight functions either way; only how
     * the game reaches them differs.
     */
    {
        size_t k;
        for (k = 0; k < sizeof(g_wing_exports) / sizeof(g_wing_exports[0]); k++)
            bind(g_wing_exports[k].name, g_wing_exports[k].fn, NULL,
                 g_wing_exports[k].argc);
    }

    /* Miles Sound System. Decorated stdcall, so the names carry their @bytes --
     * same_import() already strips a leading underscore and stops at the '@'. */
    bind("_AIL_startup",                  bridge_AIL_startup,                  NULL, 0);
    bind("_AIL_shutdown",                 bridge_AIL_shutdown,                 NULL, 0);
    bind("_AIL_allocate_sample_handle",   bridge_AIL_allocate_sample_handle,   NULL, 1);
    bind("_AIL_release_sample_handle",    bridge_AIL_release_sample_handle,    NULL, 1);
    bind("_AIL_init_sample",              bridge_AIL_init_sample,              NULL, 1);
    bind("_AIL_start_sample",             bridge_AIL_start_sample,             NULL, 1);
    bind("_AIL_stop_sample",              bridge_AIL_stop_sample,              NULL, 1);
    bind("_AIL_resume_sample",            bridge_AIL_resume_sample,            NULL, 1);
    bind("_AIL_sample_status",            bridge_AIL_sample_status,            NULL, 1);
    bind("_AIL_set_sample_address",      bridge_AIL_set_sample_address,       NULL, 3);
    bind("_AIL_set_sample_playback_rate", bridge_AIL_set_sample_playback_rate, NULL, 2);
    bind("_AIL_set_sample_loop_count",    bridge_AIL_set_sample_loop_count,    NULL, 2);
    bind("_AIL_set_sample_type",         bridge_AIL_set_sample_type,          NULL, 3);
    bind("_AIL_set_sample_user_data",    bridge_AIL_set_sample_user_data,     NULL, 3);
    bind("_AIL_sample_user_data",         bridge_AIL_sample_user_data,         NULL, 2);
    bind("_AIL_set_preference",           bridge_AIL_set_preference,           NULL, 2);
    bind("_AIL_mem_alloc_lock",           bridge_AIL_mem_alloc_lock,           NULL, 1);
    bind("_AIL_mem_free_lock",            bridge_AIL_mem_free_lock,            NULL, 1);
    bind("_AIL_allocate_file_sample",    bridge_AIL_allocate_file_sample,     NULL, 3);
    bind("_AIL_load_sample_buffer",      bridge_AIL_load_sample_buffer,       NULL, 4);
    bind("_AIL_sample_buffer_ready",      bridge_AIL_sample_buffer_ready,      NULL, 1);
    bind("_AIL_waveOutOpen",             bridge_AIL_waveOutOpen,              NULL, 4);
    bind("_AIL_waveOutClose",             bridge_AIL_waveOutClose,             NULL, 1);

    {
        size_t k;
        for (k = 0; k < sizeof(g_smacker) / sizeof(g_smacker[0]); k++)
            bind_ordinal("smackw32.dll", g_smacker[k].ord, g_smacker[k].name,
                         g_smacker[k].fn, g_smacker[k].argc);
    }

    report_unbridged();
}
