/* mm.c - Memory management for MyOS.
 *
 *  1. Physical memory manager (PMM): a bitmap with one bit per 4 KB page frame,
 *     built from the BIOS memory map that the bootloader hands us.
 *  2. Paging (virtual memory): identity-maps the first 16 MB (kernel + screen),
 *     then turns on the CPU's paging unit. Any access to an unmapped address now
 *     raises a page fault instead of silently corrupting memory.
 *  3. Kernel heap: kmalloc/kfree at virtual address 0xD0000000. The heap grows on
 *     demand by grabbing free physical frames and mapping them in.
 */
#include <stdint.h>
#include <stddef.h>
#include "kernel.h"

#define PAGE_SIZE      4096
#define MAX_MEM        (4ULL * 1024 * 1024 * 1024)   /* 4 GB address space */
#define MAX_FRAMES     (MAX_MEM / PAGE_SIZE)
#define IDENTITY_MB    16
#define HEAP_START     0xD0000000u
#define HEAP_MAX       0xE0000000u                    /* up to 256 MB of heap */

extern char _kernel_end[];                            /* from linker.ld */

/* ================= Physical memory manager ================= */
static uint32_t frame_bitmap[MAX_FRAMES / 32];        /* 1 = used, 0 = free (128 KB) */
static uint32_t total_frames = 0, used_frames = 0;
static uint32_t next_hint = 0;

static inline void frame_set(uint32_t f)   { frame_bitmap[f >> 5] |=  (1u << (f & 31)); }
static inline void frame_clear(uint32_t f) { frame_bitmap[f >> 5] &= ~(1u << (f & 31)); }
static inline int  frame_test(uint32_t f)  { return frame_bitmap[f >> 5] & (1u << (f & 31)); }

static uint32_t alloc_range(uint32_t from, uint32_t to) {
    for (uint32_t f = from; f < to && f < total_frames; f++) {
        if (frame_bitmap[f >> 5] == 0xFFFFFFFFu) { f |= 31; continue; }   /* skip full words */
        if (!frame_test(f)) { frame_set(f); used_frames++; return f * PAGE_SIZE; }
    }
    return 0;
}

/* General frames: prefer memory above 16 MB so low memory stays free for page tables. */
uint32_t pmm_alloc(void) {
    uint32_t low_end = IDENTITY_MB * 1024 * 1024 / PAGE_SIZE;
    uint32_t a = alloc_range(next_hint > low_end ? next_hint : low_end, total_frames);
    if (a) { next_hint = a / PAGE_SIZE + 1; return a; }
    return alloc_range(0, total_frames);        /* fall back to anything free */
}

/* Frames that must be reachable through the identity map (page tables). */
uint32_t pmm_alloc_low(void) {
    return alloc_range(0, IDENTITY_MB * 1024 * 1024 / PAGE_SIZE);
}

void pmm_free(uint32_t addr) {
    uint32_t f = addr / PAGE_SIZE;
    if (f < total_frames && frame_test(f)) { frame_clear(f); used_frames--; }
}

static void mark_region(uint64_t start, uint64_t len, int used) {
    uint64_t first = start / PAGE_SIZE;
    uint64_t last = (start + len + PAGE_SIZE - 1) / PAGE_SIZE;
    if (!used) { first = (start + PAGE_SIZE - 1) / PAGE_SIZE; last = (start + len) / PAGE_SIZE; } /* only whole pages are free */
    for (uint64_t f = first; f < last && f < MAX_FRAMES; f++) {
        if (used && !frame_test(f)) { frame_set(f); used_frames++; }
        if (!used && frame_test(f)) { frame_clear(f); used_frames--; }
    }
}

struct mmap_entry { uint32_t size; uint64_t addr, len; uint32_t type; } __attribute__((packed));

static void pmm_init(struct mb_info *mbi) {
    /* Start with everything "used", then free only what the BIOS says is RAM. */
    for (uint32_t i = 0; i < MAX_FRAMES / 32; i++) frame_bitmap[i] = 0xFFFFFFFFu;
    total_frames = MAX_FRAMES; used_frames = MAX_FRAMES;
    uint64_t top = 0;

    if (mbi && (mbi->flags & (1 << 6))) {
        uint32_t p = mbi->mmap_addr, end = mbi->mmap_addr + mbi->mmap_length;
        while (p < end) {
            struct mmap_entry *e = (struct mmap_entry *)p;
            if (e->type == 1 && e->addr < MAX_MEM) {           /* 1 = usable RAM */
                uint64_t len = e->len;
                if (e->addr + len > MAX_MEM) len = MAX_MEM - e->addr;
                mark_region(e->addr, len, 0);
                if (e->addr + len > top) top = e->addr + len;
            }
            p += e->size + 4;
        }
    } else if (mbi && (mbi->flags & 1)) {                      /* fallback: simple size info */
        top = 0x100000 + (uint64_t)mbi->mem_upper * 1024;
        mark_region(0x100000, top - 0x100000, 0);
    }

    total_frames = (uint32_t)(top / PAGE_SIZE);
    used_frames = 0;
    for (uint32_t f = 0; f < total_frames; f++) if (frame_test(f)) used_frames++;

    /* Reserve the first 1 MB (BIOS, VGA) and the kernel image itself. */
    mark_region(0, (uint32_t)_kernel_end, 1);
}

/* ================= Paging ================= */
static uint32_t page_dir[1024] __attribute__((aligned(PAGE_SIZE)));
static uint32_t ident_tables[IDENTITY_MB / 4][1024] __attribute__((aligned(PAGE_SIZE)));

#define PG_PRESENT 1
#define PG_WRITE   2
#define PG_USER    4

/* Map one 4 KB virtual page to a physical frame. Page tables for addresses
 * above the identity region are allocated from the PMM (they live below 16 MB
 * only if the PMM hands out a low frame, so we keep tables in identity space). */
int vmm_map(uint32_t virt, uint32_t phys) {
    uint32_t pdi = virt >> 22, pti = (virt >> 12) & 0x3FF;
    if (!(page_dir[pdi] & PG_PRESENT)) {
        uint32_t t = pmm_alloc_low();                          /* table must be reachable */
        if (!t) return 0;
        uint32_t *tbl = (uint32_t *)t;
        for (int i = 0; i < 1024; i++) tbl[i] = 0;
        page_dir[pdi] = t | PG_PRESENT | PG_WRITE;
    }
    uint32_t *tbl = (uint32_t *)(page_dir[pdi] & ~0xFFFu);
    tbl[pti] = (phys & ~0xFFFu) | PG_PRESENT | PG_WRITE;
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
    return 1;
}

void vmm_unmap(uint32_t virt) {
    uint32_t pdi = virt >> 22, pti = (virt >> 12) & 0x3FF;
    if (!(page_dir[pdi] & PG_PRESENT)) return;
    uint32_t *tbl = (uint32_t *)(page_dir[pdi] & ~0xFFFu);
    tbl[pti] = 0;
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
}

static void paging_init(void) {
    for (int i = 0; i < 1024; i++) page_dir[i] = 0;
    for (int t = 0; t < IDENTITY_MB / 4; t++) {
        for (int i = 0; i < 1024; i++)
            ident_tables[t][i] = ((t * 1024 + i) * PAGE_SIZE) | PG_PRESENT | PG_WRITE;
        page_dir[t] = (uint32_t)ident_tables[t] | PG_PRESENT | PG_WRITE;
    }
    next_hint = 0;
    /* Create every kernel-heap page table now, so all address spaces (which copy
     * the kernel's page directory) automatically see heap growth later on. */
    for (uint32_t pdi = HEAP_START >> 22; pdi < HEAP_MAX >> 22; pdi++) {
        uint32_t t = pmm_alloc_low();
        uint32_t *tbl = (uint32_t *)t;
        for (int i = 0; i < 1024; i++) tbl[i] = 0;
        page_dir[pdi] = t | PG_PRESENT | PG_WRITE;
    }

    __asm__ volatile("mov %0, %%cr3" :: "r"(page_dir));
    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000u;                                  /* PG bit */
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));
}

/* ================= Address spaces for user programs ================= */
/* A program sees: the kernel (supervisor-only, so it can't touch it) and its own
 * private pages between USER_BASE and USER_TOP. */
uint32_t *vmm_kernel_dir(void) { return page_dir; }

uint32_t *vmm_new_space(void) {
    uint32_t *pd = (uint32_t *)pmm_alloc_low();
    if (!pd) return 0;
    for (int i = 0; i < 1024; i++) pd[i] = page_dir[i];          /* share kernel mappings */
    for (uint32_t i = USER_BASE >> 22; i < USER_TOP >> 22; i++) pd[i] = 0;
    return pd;
}

/* Map a user page (readable/writable from ring 3) into address space pd. */
int vmm_map_user(uint32_t *pd, uint32_t virt, uint32_t phys) {
    uint32_t pdi = virt >> 22, pti = (virt >> 12) & 0x3FF;
    if (virt < USER_BASE || virt >= USER_TOP) return 0;
    if (!(pd[pdi] & PG_PRESENT)) {
        uint32_t t = pmm_alloc_low();
        if (!t) return 0;
        uint32_t *tbl = (uint32_t *)t;
        for (int i = 0; i < 1024; i++) tbl[i] = 0;
        pd[pdi] = t | PG_PRESENT | PG_WRITE | PG_USER;
    }
    uint32_t *tbl = (uint32_t *)(pd[pdi] & ~0xFFFu);
    tbl[pti] = (phys & ~0xFFFu) | PG_PRESENT | PG_WRITE | PG_USER;
    return 1;
}

/* Is [virt, virt+len) fully mapped as user memory in pd? (checks syscall pointers) */
int vmm_user_ok(uint32_t *pd, uint32_t virt, uint32_t len) {
    if (virt < USER_BASE || virt >= USER_TOP || len > USER_TOP - virt) return 0;
    uint32_t end = virt + (len ? len : 1);
    for (uint32_t p = virt & ~0xFFFu; p < end; p += PAGE_SIZE) {
        uint32_t pde = pd[p >> 22];
        if (!(pde & PG_PRESENT) || !(pde & PG_USER)) return 0;
        uint32_t pte = ((uint32_t *)(pde & ~0xFFFu))[(p >> 12) & 0x3FF];
        if (!(pte & PG_PRESENT) || !(pte & PG_USER)) return 0;
    }
    return 1;
}

/* Give back every page a program owned, its page tables and directory. */
uint32_t vmm_free_space(uint32_t *pd) {
    uint32_t pages = 0;
    for (uint32_t i = USER_BASE >> 22; i < USER_TOP >> 22; i++) {
        if (!(pd[i] & PG_PRESENT)) continue;
        uint32_t *tbl = (uint32_t *)(pd[i] & ~0xFFFu);
        for (int j = 0; j < 1024; j++) if (tbl[j] & PG_PRESENT) { pmm_free(tbl[j] & ~0xFFFu); pages++; }
        pmm_free((uint32_t)tbl);
    }
    pmm_free((uint32_t)pd);
    return pages;
}

void vmm_switch(uint32_t *pd) {
    uint32_t cur;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cur));
    if (cur != (uint32_t)pd) __asm__ volatile("mov %0, %%cr3" :: "r"(pd) : "memory");
}

/* ================= Kernel heap ================= */
/* Each block has a header; free blocks are found by walking the list (first fit)
 * and neighbouring free blocks are merged on kfree. */
struct block { uint32_t size; uint32_t free; struct block *next; uint32_t magic; };
#define BLOCK_MAGIC 0xC0FFEE42u
#define HDR sizeof(struct block)

static struct block *heap_head = 0;
static uint32_t heap_end = HEAP_START;                   /* first unmapped heap address */

static int heap_grow(uint32_t bytes) {
    uint32_t target = heap_end + ((bytes + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
    if (target > HEAP_MAX) return 0;
    while (heap_end < target) {
        uint32_t f = pmm_alloc();
        if (!f || !vmm_map(heap_end, f)) return 0;
        heap_end += PAGE_SIZE;
    }
    return 1;
}

void *kmalloc(size_t size) {
    if (!size) return 0;
    size = (size + 15) & ~15u;                           /* 16-byte alignment */
    struct block *b = heap_head, *last = 0;
    for (; b; last = b, b = b->next) {
        if (b->free && b->size >= size) {
            if (b->size >= size + HDR + 32) {            /* split off the remainder */
                struct block *rest = (struct block *)((uint8_t *)b + HDR + size);
                rest->size = b->size - size - HDR; rest->free = 1;
                rest->next = b->next; rest->magic = BLOCK_MAGIC;
                b->next = rest; b->size = size;
            }
            b->free = 0;
            return (uint8_t *)b + HDR;
        }
    }
    /* No fit: extend the heap. */
    uint32_t start = last ? (uint32_t)last + HDR + last->size : HEAP_START;
    if (last && last->free) start = (uint32_t)last;      /* reuse trailing free block */
    if (start + HDR + size > heap_end && !heap_grow(start + HDR + size - heap_end)) return 0;
    if (last && last->free) {
        last->size = size; last->free = 0;
        return (uint8_t *)last + HDR;
    }
    b = (struct block *)start;
    b->size = size; b->free = 0; b->next = 0; b->magic = BLOCK_MAGIC;
    if (last) last->next = b; else heap_head = b;
    return (uint8_t *)b + HDR;
}

void kfree(void *p) {
    if (!p) return;
    struct block *b = (struct block *)((uint8_t *)p - HDR);
    if (b->magic != BLOCK_MAGIC || b->free) return;      /* ignore bad/double frees */
    b->free = 1;
    for (b = heap_head; b; b = b->next)                  /* merge adjacent free blocks */
        while (b->free && b->next && b->next->free) {
            b->size += HDR + b->next->size;
            b->next = b->next->next;
        }
}

/* ================= Stats / init ================= */
void mm_stats(struct mm_stats *s) {
    s->total_kb = total_frames * 4;
    s->used_kb = used_frames * 4;
    s->free_kb = (total_frames - used_frames) * 4;
    s->heap_mapped_kb = (heap_end - HEAP_START) / 1024;
    s->heap_used = 0; s->heap_blocks = 0;
    for (struct block *b = heap_head; b; b = b->next) {
        s->heap_blocks++;
        if (!b->free) s->heap_used += b->size;
    }
}

void mm_init(struct mb_info *mbi) {
    pmm_init(mbi);
    paging_init();
}
