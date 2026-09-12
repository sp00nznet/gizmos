/*
 * Gizmos & Gadgets static recompilation -- entry point.
 *
 * Maps the original SSGWIN32.EXE at 0x400000, patches its IAT to point at our
 * bridges, and calls the PE entry point -- the Borland CRT startup, which then
 * finds its own way to WinMain.
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
#include <stdio.h>
#include <string.h>

/*
 * The PE entry point, read from the header of the image we just mapped, rather
 * than written here as a symbol: the header is in memory anyway, and a
 * hardcoded sub_XXXXXXXX goes stale the moment the lift is regenerated.
 */
static recomp_func_t resolve_entry(uint32_t base, uint32_t *out_va) {
    uint32_t pe  = base + MEM32(base + 0x3C);   /* e_lfanew       */
    uint32_t rva = MEM32(pe + 0x28);            /* AddressOfEntry */
    *out_va = base + rva;
    return recomp_lookup(*out_va);
}

/* premap.c -- how we get the image range at 0x400000 */
int premap_is_child(void);
void gg_set_own_volume(void);
int premap_relaunch(const char *game_exe);

static const char *g_exe_path = "original/SSGWINCD/SSGWIN32.EXE";

/* What the game believes its own module path is. It builds SSGWINCD.INI's
 * location out of this, so handing it the host exe's path sends it looking for
 * the .INI next to gizmos.exe. */
char g_game_module_path[MAX_PATH];

int main(int argc, char *argv[]) {
    if (argc > 1) g_exe_path = argv[1];

    /* First invocation is the launcher: it reserves 0x400000 in a suspended
     * copy of us, because nothing running inside this process can get there. */
    if (!premap_is_child()) return premap_relaunch(g_exe_path);

    if (!GetFullPathNameA(g_exe_path, sizeof(g_game_module_path), g_game_module_path, NULL))
        strncpy(g_game_module_path, g_exe_path, sizeof(g_game_module_path) - 1);

    /* Image first: it is the only mapping that must land at a fixed VA. */
    if (!load_original_data(g_exe_path)) return 1;
    if (!recomp_init()) return 1;
    gg_set_own_volume();   /* GG_VOLUME=0..100, before a note is played */
    setup_iat_bridges();   /* patches IAT slots in the just-loaded image */

    /*
     * The game's working directory has to be where its data files are: it opens
     * GIZMO.DAT and friends by relative name.
     */
    {
        char dir[MAX_PATH];
        const char *slash;
        strncpy(dir, g_exe_path, sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = 0;
        slash = strrchr(dir, '\\');
        if (!slash) slash = strrchr(dir, '/');
        if (slash) {
            dir[slash - dir] = 0;
            if (SetCurrentDirectoryA(dir))
                fprintf(stderr, "  cwd -> %s\n", dir);
        }
    }

    {
        uint32_t entry_va = 0;
        recomp_func_t entry = resolve_entry(GG_IMAGE_BASE, &entry_va);
        if (!entry) {
            fprintf(stderr, "entry point 0x%08X was not lifted\n", entry_va);
            return 1;
        }
        fprintf(stderr, "\n--- entry point (0x%08X) ---\n\n", entry_va);
        PUSH32(esp, RECOMP_RETADDR);   /* CRT startup takes no arguments */
        entry();
    }

    recomp_dump_trace("returned from entry");
    recomp_shutdown();
    return 0;
}
