#ifndef GG_RECOMP_RUNTIME_H
#define GG_RECOMP_RUNTIME_H

/*
 * Gizmos & Gadgets runtime = the canonical pcrecomp recomp32 contract plus the
 * few things this game needs on top.
 *
 * recomp_types.h is a verbatim copy of pcrecomp/runtime/recomp32/recomp_types.h
 * and MUST stay that way: it is the interface the lifter generates against.
 * Re-sync it whenever the toolkit's lifter changes; game-specific things go here.
 */

#include <stdint.h>
#include "recomp_types.h"

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#define GG_IMAGE_BASE 0x00400000u

/* Runtime lifecycle */
int  recomp_init(void);
void recomp_shutdown(void);
int  load_original_data(const char *exe_path);

/*
 * Stack arguments for anything the lifted dispatcher calls. RECOMP_ICALL pushes
 * a dummy return address before the call, so it sits at esp+0 and the first
 * argument is at esp+4. A stdcall callee cleans up 4 + 4*argc.
 */
#define ARG(n) MEM32(esp + 4 * (n))

/* IAT bridge system (iat_bridge.c) */
void setup_iat_bridges(void);
recomp_func_t iat_bridge_lookup(u32 target_va);

/* Scratch VA space for strings the host hands the game (never inside the image) */
u32 recomp_scratch_alloc(u32 n);
u32 recomp_scratch_str(const char *s);

/*
 * A heap that lives below 4 GB, for the memory the game asks for and then keeps
 * a 32-bit pointer to. GlobalAlloc and VirtualAlloc cannot be passed through to
 * the host on a 64-bit build: the pointer they return does not fit in eax.
 */
u32  gg_heap_alloc(u32 size);
u32  gg_heap_realloc(u32 va, u32 size);
void gg_heap_free(u32 va);
u32  gg_heap_size(u32 va);
int  gg_heap_owns(u32 va);

#endif /* GG_RECOMP_RUNTIME_H */
