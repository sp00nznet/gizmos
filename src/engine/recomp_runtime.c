/*
 * Recompilation runtime for the Gizmos & Gadgets static recompilation.
 *
 * Memory model (fixed-base, no offset fallback):
 *
 *   host-chosen  TIB      simulated TEB, 1 page (fs: base)
 *   host-chosen  stack    1 MB simulated stack
 *   host-chosen  scratch  strings the host hands the game
 *   0x20000000   heap     32-bit-addressable heap for GlobalAlloc/VirtualAlloc
 *   0x00400000   image    the original .CODE/.DATA at their real VAs
 *
 * Offset-based mapping is deliberately NOT supported: it works for data reads
 * and silently breaks every pointer the game stores. If 0x400000 cannot be had,
 * that is a link-base problem in this host exe, not something to work around.
 *
 * Adapted from the gta recomp's engine layer, which is where this shape was
 * worked out. The GTA-specific hooks (its FatalError trap, the MGL mode table,
 * the DirectDraw host-call bridge) are gone; this game needs none of them.
 */

/* windows.h first: winnt.h contains inline asm ("mov eax, ...") that the
 * RECOMP_GENERATED_CODE register aliases below would rewrite. */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#define RECOMP_GENERATED_CODE
#include "recomp_runtime.h"
#include "image_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* ===== global register model (contract: recomp_types.h) ===== */
uint32_t g_eax, g_ecx, g_edx, g_esp;
uint32_t g_ebx, g_esi, g_edi, g_ebp;
double   g_st[8];
int      g_fp_top;
uint16_t g_fpu_cw = 0x037F;
uint16_t g_seg_cs, g_seg_ds, g_seg_es, g_seg_fs, g_seg_gs, g_seg_ss;

ptrdiff_t g_mem_base = 0;      /* fixed-base mapping: VA == host address */
uint32_t  g_fs_base  = 0;
uint32_t  g_gs_base  = 0;

uint32_t g_icall_trace[ICALL_TRACE_SIZE];
uint32_t g_icall_trace_idx;
uint32_t g_icall_count;
uint32_t g_cur_func;

#ifdef RECOMP_TRACE
uint32_t g_enter_trace[RECOMP_ENTER_SIZE];
uint32_t g_enter_idx;

/*
 * GG_WATCH=0x412a40,0x4131b0 prints esp, the callee-saved registers and the
 * first four stack arguments on entry to those functions. Walking an argument
 * down a call chain by hand means recomputing frame offsets at every level and
 * being wrong once; this just shows where the value turns into something it
 * should not be.
 */
#define MAX_WATCHES 16
static uint32_t g_watch[MAX_WATCHES];
static int      g_watch_count = -1;

static int is_watched(uint32_t va) {
    if (g_watch_count < 0) {
        char buf[256], *p;
        g_watch_count = 0;
        if (GetEnvironmentVariableA("GG_WATCH", buf, sizeof(buf))) {
            for (p = strtok(buf, ","); p && g_watch_count < MAX_WATCHES;
                 p = strtok(NULL, ",")) {
                g_watch[g_watch_count++] = (uint32_t)strtoul(p, NULL, 16);
            }
        }
    }
    for (int i = 0; i < g_watch_count; i++)
        if (g_watch[i] == va) return 1;
    return 0;
}

/*
 * esp alongside the address. The ring records order, but nesting is what says
 * which of these frames called which -- and esp is exactly that, for free.
 */
uint32_t g_enter_esp[RECOMP_ENTER_SIZE];
uint32_t g_enter_ecx[RECOMP_ENTER_SIZE];

void recomp_trace_enter(uint32_t va) {
    g_enter_trace[g_enter_idx & (RECOMP_ENTER_SIZE - 1)] = va;
    g_enter_esp[g_enter_idx & (RECOMP_ENTER_SIZE - 1)] = g_esp;
    g_enter_ecx[g_enter_idx & (RECOMP_ENTER_SIZE - 1)] = g_ecx;
    g_enter_idx++;

    if (is_watched(va)) {
        /* ebx/esi/edi as well as the arguments: the callee-saved registers are
         * the ones a mis-lifted callee silently destroys, and a value that
         * changes across a call is the only way to see it. */
        fprintf(stderr, "  WATCH 0x%08X esp=0x%08X ecx=0x%08X ebx=0x%08X esi=0x%08X edi=0x%08X args=[%08X %08X %08X %08X]\n",
                va, g_esp, g_ecx, g_ebx, g_esi, g_edi,
                MEM32(g_esp + 4), MEM32(g_esp + 8),
                MEM32(g_esp + 12), MEM32(g_esp + 16));
    }
}
#endif

/* ===== memory layout ===== */
#define GG_TIB_SIZE     0x00001000u
#define GG_STACK_SIZE   0x00100000u
#define GG_SCRATCH_SIZE 0x00040000u

/* The low heap. 0x20000000 is clear of the image (0x400000..0x461000), of the
 * host exe's link base, and of anything Windows puts in the bottom 4 GB. */
#define GG_HEAP_BASE    0x20000000u
#define GG_HEAP_SIZE    0x04000000u   /* 64 MB */

static void *g_tib_view, *g_stack_view, *g_scratch_view, *g_heap_view;
static uint32_t g_stack_base, g_scratch_base, g_scratch_next;
static uint32_t g_image_span;

/* Bump-allocate scratch VA space for strings the host hands to the game
 * (command line, module path). Never inside the image: that would overwrite
 * .data the game is still using. */
uint32_t recomp_scratch_alloc(uint32_t n) {
    uint32_t va = g_scratch_next;
    n = (n + 15u) & ~15u;
    if (va + n > g_scratch_base + GG_SCRATCH_SIZE) {
        fprintf(stderr, "[scratch] exhausted\n");
        return 0;
    }
    g_scratch_next = va + n;
    return va;
}

uint32_t recomp_scratch_str(const char *s) {
    uint32_t n = (uint32_t)strlen(s) + 1;
    uint32_t va = recomp_scratch_alloc(n);
    if (va) memcpy((void *)(uintptr_t)ADDR(va), s, n);
    return va;
}

/* ===== the low heap =====
 *
 * GlobalAlloc and VirtualAlloc hand the game a pointer it stores in a 32-bit
 * slot. On a 64-bit host the real API returns something that does not survive
 * the truncation, so those two are served from here instead of passed through.
 *
 * ponytail: first-fit over a doubly-linked block list, coalescing on free.
 * That is the simplest thing that can run a session without fragmenting itself
 * to death. If a profile ever shows the walk costing anything, size classes are
 * the upgrade -- but a 1996 game allocating a few hundred blocks will not.
 */
typedef struct gg_block {
    uint32_t size;   /* payload bytes */
    uint32_t free;
    uint32_t prev;   /* VA of header, 0 for the first */
    uint32_t next;   /* VA of header, 0 for the last  */
} gg_block;

#define BLK(va)     ((gg_block *)(uintptr_t)ADDR(va))
#define BLK_HDR     ((uint32_t)sizeof(gg_block))
#define PAYLOAD(va) ((va) + BLK_HDR)
#define HEADER(va)  ((va) - BLK_HDR)

static uint32_t g_heap_first;

static void heap_init(void) {
    g_heap_first = GG_HEAP_BASE;
    BLK(g_heap_first)->size = GG_HEAP_SIZE - BLK_HDR;
    BLK(g_heap_first)->free = 1;
    BLK(g_heap_first)->prev = 0;
    BLK(g_heap_first)->next = 0;
}

int gg_heap_owns(uint32_t va) {
    return va >= GG_HEAP_BASE + BLK_HDR && va < GG_HEAP_BASE + GG_HEAP_SIZE;
}

uint32_t gg_heap_size(uint32_t va) {
    return gg_heap_owns(va) ? BLK(HEADER(va))->size : 0;
}

uint32_t gg_heap_alloc(uint32_t size) {
    uint32_t va;
    if (!size) size = 1;
    size = (size + 15u) & ~15u;

    for (va = g_heap_first; va; va = BLK(va)->next) {
        gg_block *b = BLK(va);
        if (!b->free || b->size < size) continue;

        /* Split, if what is left over can hold a header and something useful. */
        if (b->size >= size + BLK_HDR + 16u) {
            uint32_t rest = va + BLK_HDR + size;
            gg_block *r = BLK(rest);
            r->size = b->size - size - BLK_HDR;
            r->free = 1;
            r->prev = va;
            r->next = b->next;
            if (r->next) BLK(r->next)->prev = rest;
            b->size = size;
            b->next = rest;
        }
        b->free = 0;
        memset((void *)(uintptr_t)ADDR(PAYLOAD(va)), 0, b->size);
        return PAYLOAD(va);
    }
    fprintf(stderr, "[heap] out of memory asking for %u bytes\n", size);
    return 0;
}

void gg_heap_free(uint32_t va) {
    uint32_t h;
    gg_block *b;
    if (!gg_heap_owns(va)) return;
    h = HEADER(va);
    b = BLK(h);
    b->free = 1;

    if (b->next && BLK(b->next)->free) {              /* coalesce forward */
        uint32_t n = b->next;
        b->size += BLK_HDR + BLK(n)->size;
        b->next  = BLK(n)->next;
        if (b->next) BLK(b->next)->prev = h;
    }
    if (b->prev && BLK(b->prev)->free) {              /* and backward */
        uint32_t p = b->prev;
        BLK(p)->size += BLK_HDR + b->size;
        BLK(p)->next  = b->next;
        if (b->next) BLK(b->next)->prev = p;
    }
}

uint32_t gg_heap_realloc(uint32_t va, uint32_t size) {
    uint32_t nva, old;
    if (!va) return gg_heap_alloc(size);
    if (!gg_heap_owns(va)) return 0;
    old = BLK(HEADER(va))->size;
    if (size <= old) return va;
    nva = gg_heap_alloc(size);
    if (!nva) return 0;
    memcpy((void *)(uintptr_t)ADDR(nva), (void *)(uintptr_t)ADDR(va), old);
    gg_heap_free(va);
    return nva;
}

/* ===== dispatch ===== */
recomp_func_t recomp_lookup(uint32_t va) {
    int lo = 0, hi = (int)recomp_dispatch_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t a = recomp_dispatch_table[mid].address;
        if (a == va) return recomp_dispatch_table[mid].func;
        if (a < va) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }
recomp_func_t recomp_lookup_import(uint32_t va) { return iat_bridge_lookup(va); }

void recomp_dump_trace(const char *why) {
    fprintf(stderr, "--- trace (%s): cur_func=0x%08X, %u icalls ---\n",
            why ? why : "?", g_cur_func, g_icall_count);
    for (uint32_t i = 0; i < ICALL_TRACE_SIZE; i++) {
        uint32_t idx = (g_icall_trace_idx - ICALL_TRACE_SIZE + i) & (ICALL_TRACE_SIZE - 1);
        if (g_icall_trace[idx]) fprintf(stderr, "  [%2u] 0x%08X\n", i, g_icall_trace[idx]);
    }
#ifdef RECOMP_TRACE
    fprintf(stderr, "--- last %d functions entered ---\n", RECOMP_ENTER_SIZE);
    for (uint32_t i = 0; i < RECOMP_ENTER_SIZE; i++) {
        uint32_t idx = (g_enter_idx - RECOMP_ENTER_SIZE + i) & (RECOMP_ENTER_SIZE - 1);
        if (g_enter_trace[idx])
            fprintf(stderr, "  0x%08X  esp=0x%08X ecx=0x%08X\n",
                    g_enter_trace[idx], g_enter_esp[idx], g_enter_ecx[idx]);
    }
#endif
    fflush(stderr);
}

/* ===== crash handler ===== */
#ifdef _WIN32
static LONG WINAPI crash_handler(EXCEPTION_POINTERS *ep) {
    fprintf(stderr, "\n=== CRASH: exception 0x%08lX at %p ===\n",
            ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    /*
     * WHICH address, and reading or writing. Without it an access violation in
     * lifted code is eight registers and a guess; with it the answer is usually
     * the first line -- and VirtualQuery says whether the page is merely
     * uncommitted or nobody's at all.
     */
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        ep->ExceptionRecord->NumberParameters >= 2) {
        ULONG_PTR op = ep->ExceptionRecord->ExceptionInformation[0];
        ULONG_PTR va = ep->ExceptionRecord->ExceptionInformation[1];
        MEMORY_BASIC_INFORMATION mbi;
        const char *what = op == 0 ? "reading" : op == 1 ? "writing" : "executing";
        if (VirtualQuery((void *)va, &mbi, sizeof(mbi)))
            fprintf(stderr, "  %s 0x%p -- region %p +%08zX state=%lX protect=%lX\n",
                    what, (void *)va, mbi.BaseAddress, mbi.RegionSize,
                    mbi.State, mbi.Protect);
        else
            fprintf(stderr, "  %s 0x%p -- not in this address space at all\n",
                    what, (void *)va);
    }
    fprintf(stderr, "  eax=%08X ecx=%08X edx=%08X ebx=%08X\n", g_eax, g_ecx, g_edx, g_ebx);
    fprintf(stderr, "  esi=%08X edi=%08X esp=%08X ebp=%08X\n", g_esi, g_edi, g_esp, g_ebp);
    recomp_dump_trace("VEH");
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

/* ===== init / shutdown ===== */
/*
 * A hang looks exactly like success from the outside: no crash, no output, no
 * OS calls. Set GG_WATCHDOG_MS and a thread dumps the trace and bails after
 * that long, turning "it stopped saying anything" into a function address.
 */
static DWORD WINAPI watchdog_thread(LPVOID arg) {
    Sleep((DWORD)(uintptr_t)arg);
    fprintf(stderr, "\n*** watchdog: still running, dumping trace ***\n");
    recomp_dump_trace("watchdog");
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 2);
    return 0;
}

static void start_watchdog(void) {
    char buf[32];
    DWORD ms;
    if (!GetEnvironmentVariableA("GG_WATCHDOG_MS", buf, sizeof(buf))) return;
    ms = (DWORD)atoi(buf);
    if (!ms) return;
    CreateThread(NULL, 0, watchdog_thread, (LPVOID)(uintptr_t)ms, 0, NULL);
    fprintf(stderr, "  watchdog armed: %lu ms\n", ms);
}

int recomp_init(void) {
    fprintf(stderr, "Gizmos & Gadgets -- static recompilation runtime\n");
    start_watchdog();

#ifdef _WIN32
    g_tib_view = VirtualAlloc(NULL, GG_TIB_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_tib_view) { fprintf(stderr, "FATAL: TIB alloc failed (%lu)\n", GetLastError()); return 0; }

    g_stack_view = VirtualAlloc(NULL, GG_STACK_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_stack_view) { fprintf(stderr, "FATAL: stack alloc failed (%lu)\n", GetLastError()); return 0; }

    g_scratch_view = VirtualAlloc(NULL, GG_SCRATCH_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_scratch_view) { fprintf(stderr, "FATAL: scratch alloc failed (%lu)\n", GetLastError()); return 0; }

    g_heap_view = VirtualAlloc((void *)(uintptr_t)GG_HEAP_BASE, GG_HEAP_SIZE,
                               MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_heap_view) {
        fprintf(stderr, "FATAL: low heap alloc at 0x%08X failed (%lu)\n",
                GG_HEAP_BASE, GetLastError());
        return 0;
    }

    AddVectoredExceptionHandler(1, crash_handler);
#else
    fprintf(stderr, "FATAL: fixed-base mapping needs Win32\n");
    return 0;
#endif

    g_fs_base      = (uint32_t)(uintptr_t)g_tib_view;
    g_stack_base   = (uint32_t)(uintptr_t)g_stack_view;
    g_scratch_base = g_scratch_next = (uint32_t)(uintptr_t)g_scratch_view;
    heap_init();

    g_esp = g_stack_base + GG_STACK_SIZE - 64;

    /* Simulated TIB: the Borland CRT's SEH prologue reads and writes fs:[0],
     * and its startup reads the stack bounds out of fs:[4]/fs:[8]. */
    MEM32(g_fs_base + 0x00) = 0xFFFFFFFFu;                   /* ExceptionList (end) */
    MEM32(g_fs_base + 0x04) = g_stack_base + GG_STACK_SIZE; /* StackBase   */
    MEM32(g_fs_base + 0x08) = g_stack_base;                  /* StackLimit  */
    MEM32(g_fs_base + 0x18) = g_fs_base;                     /* Self        */

    g_eax = g_ebx = g_ecx = g_edx = g_esi = g_edi = 0;
    g_fp_top = 0;
    memset(g_st, 0, sizeof(g_st));
    memset(g_icall_trace, 0, sizeof(g_icall_trace));

    fprintf(stderr, "  tib 0x%08X  stack 0x%08X..0x%08X (esp=0x%08X)  scratch 0x%08X  heap 0x%08X+%uM\n",
            g_fs_base, g_stack_base, g_stack_base + GG_STACK_SIZE, g_esp,
            g_scratch_base, GG_HEAP_BASE, GG_HEAP_SIZE >> 20);
    return 1;
}

int load_original_data(const char *exe_path) {
    uint32_t span = recomp_load_image(exe_path, GG_IMAGE_BASE);
    if (!span) {
        fprintf(stderr, "FATAL: could not map %s at 0x%08X -- link this host exe at a high base\n",
                exe_path, GG_IMAGE_BASE);
        {
            extern void premap_report(void);
            premap_report();
        }
        return 0;
    }
    g_image_span = span;
    fprintf(stderr, "  image 0x%08X..0x%08X from %s\n",
            GG_IMAGE_BASE, GG_IMAGE_BASE + span, exe_path);
    return 1;
}

void recomp_shutdown(void) {
#ifdef _WIN32
    if (g_heap_view)    VirtualFree(g_heap_view, 0, MEM_RELEASE);
    if (g_scratch_view) VirtualFree(g_scratch_view, 0, MEM_RELEASE);
    if (g_stack_view)   VirtualFree(g_stack_view, 0, MEM_RELEASE);
    if (g_tib_view)     VirtualFree(g_tib_view, 0, MEM_RELEASE);
#endif
}
