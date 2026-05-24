/*
 * wolf_heap.c - Dedicated byte-level heap for vendored wolfSSL +
 * wolfSSH.
 *
 * Why a separate heap and not the shared `lua_stubs.c` allocator:
 * scrypt at the OWASP / RFC 7914 floor (N=2^15, r=8, p=1) needs a
 * single 32 MB working buffer per derivation. The lua_stubs heap is
 * 1 MB total — wolfssl's XMALLOC redirected there returns
 * MEMORY_E (-125) and `adduser` / `passwd_set` / `passwd_verify`
 * all fail. Discovered on pi-5-2 during the #199e hardware
 * validation (see PR #918 thread).
 *
 * Dedicating a 48 MB BSS pool to wolfssl gives scrypt its working
 * buffer plus headroom for the smaller per-handshake KEX + cipher
 * + hash + hmac state. Gated on `NET_SSHD` so the kernel image
 * doesn't consume the RAM when the SSH daemon is compiled out.
 *
 * The freelist allocator below is a literal port of the design in
 * `kernel/src/lua_stubs.c::malloc/free` — same magic-stamp header,
 * same forward/back merge on free. The implementation is duplicated
 * rather than refactored because:
 *   1. Sharing would require either re-sizing the lua_stubs pool
 *      (affects every platform's BSS footprint) or refactoring
 *      lua_stubs into a generic allocator the rest of the kernel
 *      reuses (out of scope for #199e).
 *   2. Per-subsystem heap isolation prevents wolfssl fragmentation
 *      from starving Lua and vice versa.
 *
 * Concurrency: one spinlock around alloc/free, IRQ-disabled. All
 * SSH-side callers run from task context (the sshd session task
 * during accept / shell, the console shell for adduser/passwd), so
 * the IRQ-disable interval is short relative to scrypt's compute
 * dominance.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pmm.h"
#include "spinlock.h"
#include "uart.h"

/* Sized for scrypt at PASSWD_SCRYPT_LOG2_N=15: 32 MB working
 * buffer + ~16 MB headroom for wolfSSH session state. Allocated
 * lazily from PMM on first use so an `NET_SSHD=ON` build that
 * never calls into wolfssl (e.g. operator never runs `sshd start`
 * + `adduser`) doesn't consume the RAM, and so the kernel image
 * stays under the platform linker caps.
 *
 * 48 MB ÷ 4 KB = 12288 pages. That's order=14 in the buddy
 * allocator. Pi 5 / Jetson / x86-64 all have GB-scale PMM
 * regions; QEMU's 1 GB build has ~700 MB of usable PMM after
 * model memory, comfortably accommodating the request. */
#define WOLF_HEAP_SIZE   (48u * 1024u * 1024u)
#define WOLF_HEAP_PAGES  (WOLF_HEAP_SIZE / 4096u)

#define WOLF_HEAP_MAGIC 0x57B10C4Bu   /* "WBlOCK" — used for use-after-
                                       * free / wrong-pool detection. */

struct wolf_block {
    uint32_t           magic;     /* WOLF_HEAP_MAGIC when live */
    uint32_t           is_free;
    size_t             size;      /* user-visible bytes (excludes header) */
    struct wolf_block *prev;
    struct wolf_block *next;
};

static uint8_t          *g_wolf_heap;
static struct wolf_block *g_head;
static bool              g_inited;
static spinlock_t        g_lock = SPINLOCK_INIT;

static bool wolf_heap_init_locked(void)
{
    if (g_inited) return true;
    g_wolf_heap = (uint8_t *)pmm_alloc_pages(WOLF_HEAP_PAGES);
    if (g_wolf_heap == NULL) {
        uart_printf("[WOLFHEAP] pmm_alloc_pages(%u) failed — "
                    "SSH crypto operations will fail with MEMORY_E\r\n",
                    (unsigned)WOLF_HEAP_PAGES);
        return false;
    }
    g_head = (struct wolf_block *)g_wolf_heap;
    g_head->magic   = WOLF_HEAP_MAGIC;
    g_head->is_free = 1u;
    g_head->size    = WOLF_HEAP_SIZE - sizeof(struct wolf_block);
    g_head->prev    = NULL;
    g_head->next    = NULL;
    g_inited        = true;
    uart_printf("[WOLFHEAP] %u MB wolfssl heap online at %p\r\n",
                (unsigned)(WOLF_HEAP_SIZE / (1024u * 1024u)),
                (void *)g_wolf_heap);
    return true;
}

/* Round up to 16 bytes so the returned pointer is 16-byte aligned
 * (wolfcrypt SIMD-y paths assume it). */
static size_t align_up(size_t n)
{
    return (n + 15u) & ~(size_t)15u;
}

static void *alloc_locked(size_t size)
{
    if (!g_inited && !wolf_heap_init_locked()) return NULL;
    if (size == 0u) return NULL;
    size = align_up(size);

    struct wolf_block *b = g_head;
    while (b != NULL) {
        if (b->is_free && b->size >= size) {
            /* Split if the slack would hold another header + 16 B
             * payload — otherwise hand over the whole block. */
            if (b->size >= size + sizeof(struct wolf_block) + 16u) {
                struct wolf_block *rest = (struct wolf_block *)
                    ((uint8_t *)b + sizeof(struct wolf_block) + size);
                rest->magic   = WOLF_HEAP_MAGIC;
                rest->is_free = 1u;
                rest->size    = b->size - size - sizeof(struct wolf_block);
                rest->prev    = b;
                rest->next    = b->next;
                if (b->next) b->next->prev = rest;
                b->next       = rest;
                b->size       = size;
            }
            b->is_free = 0u;
            return (void *)((uint8_t *)b + sizeof(struct wolf_block));
        }
        b = b->next;
    }
    return NULL;   /* OOM */
}

static void free_locked(void *p)
{
    if (p == NULL) return;
    struct wolf_block *b = (struct wolf_block *)
        ((uint8_t *)p - sizeof(struct wolf_block));
    if (b->magic != WOLF_HEAP_MAGIC) {
        uart_printf("[WOLFHEAP] free of non-wolf-heap pointer %p (magic=0x%x)\r\n",
                    p, (unsigned)b->magic);
        return;
    }
    b->is_free = 1u;

    /* Merge with successor + predecessor where possible. */
    if (b->next && b->next->is_free) {
        b->size += sizeof(struct wolf_block) + b->next->size;
        b->next = b->next->next;
        if (b->next) b->next->prev = b;
    }
    if (b->prev && b->prev->is_free) {
        b->prev->size += sizeof(struct wolf_block) + b->size;
        b->prev->next = b->next;
        if (b->next) b->next->prev = b->prev;
    }
}

void *wolf_heap_alloc(size_t size)
{
    irq_flags_t flags = spin_lock_irqsave(&g_lock);
    void *p = alloc_locked(size);
    spin_unlock_irqrestore(&g_lock, flags);
    return p;
}

void wolf_heap_free(void *p)
{
    irq_flags_t flags = spin_lock_irqsave(&g_lock);
    free_locked(p);
    spin_unlock_irqrestore(&g_lock, flags);
}

void *wolf_heap_realloc(void *p, size_t size)
{
    if (p == NULL)  return wolf_heap_alloc(size);
    if (size == 0u) { wolf_heap_free(p); return NULL; }

    irq_flags_t flags = spin_lock_irqsave(&g_lock);
    struct wolf_block *b = (struct wolf_block *)
        ((uint8_t *)p - sizeof(struct wolf_block));
    if (b->magic != WOLF_HEAP_MAGIC) {
        spin_unlock_irqrestore(&g_lock, flags);
        uart_printf("[WOLFHEAP] realloc of non-wolf-heap pointer %p\r\n", p);
        return NULL;
    }

    /* In-place shrink/keep is fine. */
    if (b->size >= size) {
        spin_unlock_irqrestore(&g_lock, flags);
        return p;
    }

    /* Grow: allocate fresh, copy, free old. */
    void *np = alloc_locked(size);
    if (np != NULL) {
        size_t copy = b->size;
        for (size_t i = 0; i < copy; i++) {
            ((uint8_t *)np)[i] = ((uint8_t *)p)[i];
        }
        free_locked(p);
    }
    spin_unlock_irqrestore(&g_lock, flags);
    return np;
}
