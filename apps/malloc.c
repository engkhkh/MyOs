/* malloc.c - memory allocator for MyOS programs.
 * Blocks live in a heap that grows with the sbrk() system call. Each block has
 * a small header; free blocks are reused (first fit) and merged with free
 * neighbours, so programs can allocate and free as they like. */
#include "myos.h"

struct hdr { uint32_t size; uint32_t free; struct hdr *next; uint32_t magic; };
#define MAGIC 0x5AFEB10C
static struct hdr *head, *tail;

void *memset(void *d, int c, uint32_t n) { uint8_t *p = d; while (n--) *p++ = (uint8_t)c; return d; }
void *memcpy(void *d, const void *s, uint32_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; return d; }

void *malloc(uint32_t size) {
    if (!size) return 0;
    size = (size + 15) & ~15u;
    for (struct hdr *h = head; h; h = h->next)
        if (h->free && h->size >= size) {
            if (h->size >= size + sizeof(struct hdr) + 32) {      /* split the leftover off */
                struct hdr *rest = (struct hdr *)((uint8_t *)(h + 1) + size);
                rest->size = h->size - size - sizeof(struct hdr);
                rest->free = 1; rest->next = h->next; rest->magic = MAGIC;
                if (tail == h) tail = rest;
                h->next = rest; h->size = size;
            }
            h->free = 0;
            return h + 1;
        }
    /* nothing free is big enough: ask the kernel for more (in 16 KB+ chunks) */
    uint32_t need = size + sizeof(struct hdr);
    uint32_t grow = need < 16384 ? 16384 : (need + 4095) & ~4095u;
    struct hdr *h = sbrk(grow);
    if ((int)h == -1) return 0;
    h->size = grow - sizeof(struct hdr); h->free = 1; h->next = 0; h->magic = MAGIC;
    if (tail) tail->next = h; else head = h;
    tail = h;
    if (h != head) {                                           /* merge with a free block just before */
        struct hdr *p = head; while (p->next != h) p = p->next;
        if (p->free && (uint8_t *)(p + 1) + p->size == (uint8_t *)h) { p->size += sizeof(struct hdr) + h->size; p->next = 0; tail = p; }
    }
    return malloc(size);
}

void *calloc(uint32_t n, uint32_t size) {
    void *p = malloc(n * size);
    if (p) memset(p, 0, n * size);
    return p;
}

void free(void *p) {
    if (!p) return;
    struct hdr *h = (struct hdr *)p - 1;
    if (h->magic != MAGIC || h->free) return;
    h->free = 1;
    for (struct hdr *b = head; b; b = b->next)               /* merge neighbours that touch */
        while (b->free && b->next && b->next->free && (uint8_t *)(b + 1) + b->size == (uint8_t *)b->next) {
            if (tail == b->next) tail = b;
            b->size += sizeof(struct hdr) + b->next->size;
            b->next = b->next->next;
        }
}
