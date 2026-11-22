/* mouse.c - PS/2 mouse driver.
 * The mouse sends 3-byte packets on IRQ12: buttons + X movement + Y movement.
 * Finished packets go into a small queue that the desktop reads. */
#include <stdint.h>
#include "kernel.h"

static void wait_write(void) { for (int i = 0; i < 1000000 && (inb(0x64) & 2); i++) {} }
static int read_byte(void) {                  /* returns -1 on timeout */
    for (int i = 0; i < 3000000; i++) if (inb(0x64) & 1) return inb(0x60);
    return -1;
}

static void mouse_cmd(uint8_t c) {
    wait_write(); outb(0x64, 0xD4);       /* next byte goes to the mouse */
    wait_write(); outb(0x60, c);
    read_byte();                          /* ACK (0xFA) */
}

/* Setup runs with interrupts off and IRQ12 disabled, so the replies to our
 * commands come back to us instead of being swallowed by the IRQ handler. */
void mouse_init(void) {
    __asm__ volatile("cli");
    wait_write(); outb(0x64, 0xA8);       /* enable the mouse port */
    wait_write(); outb(0x64, 0x20);       /* read controller config */
    uint8_t cfg = (uint8_t)read_byte();
    cfg &= ~0x22;                         /* IRQ12 off for now, mouse clock on */
    wait_write(); outb(0x64, 0x60);
    wait_write(); outb(0x60, cfg);
    while (inb(0x64) & 1) inb(0x60);      /* flush anything pending */

    mouse_cmd(0xFF);                      /* full reset: back to plain 3-byte packets */
    read_byte();                          /* self-test passed (0xAA) */
    read_byte();                          /* device ID (0x00) */
    mouse_cmd(0xF6);                      /* defaults */
    mouse_cmd(0xF4);                      /* start streaming packets */
    while (inb(0x64) & 1) inb(0x60);

    cfg |= 0x02;                          /* now enable IRQ12 */
    wait_write(); outb(0x64, 0x60);
    wait_write(); outb(0x60, cfg);
    __asm__ volatile("sti");
}

static struct mouse_event q[64];
static volatile uint8_t qh = 0, qt = 0;
static uint8_t pkt[3];
static int pi = 0;

/* Called from the IRQ12 handler with one byte. */
void mouse_byte(uint8_t b) {
    if (pi == 0 && !(b & 0x08)) return;    /* first byte always has bit 3 set: resync */
    pkt[pi++] = b;
    if (pi < 3) return;
    pi = 0;
    if (pkt[0] & 0xC0) return;             /* overflow: drop */
    int dx = pkt[1] - ((pkt[0] << 4) & 0x100);
    int dy = pkt[2] - ((pkt[0] << 3) & 0x100);
    /* Same buttons as the newest waiting event: add the movement to it, so a busy
     * moment never loses movement (the pointer would land in the wrong place).
     * The oldest waiting event is left alone: the main loop may be reading it. */
    uint8_t last = (qh + 63) % 64;
    if (qh != qt && last != qt && q[last].buttons == (pkt[0] & 7)) { q[last].dx += dx; q[last].dy -= dy; return; }
    uint8_t n = (qh + 1) % 64;
    if (n == qt) return;
    q[qh].dx = dx; q[qh].dy = -dy;         /* screen Y grows downward */
    q[qh].buttons = pkt[0] & 7;
    qh = n;
}

int mouse_read(struct mouse_event *e) {
    if (qh == qt) return 0;
    *e = q[qt]; qt = (qt + 1) % 64;
    return 1;
}
