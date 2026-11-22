/* ata.c - ATA hard disk driver (PIO mode, 28-bit LBA).
 * Talks to IDE-style disks through I/O ports. It checks all four classic
 * positions (primary/secondary bus, master/slave) and uses the first real hard
 * disk it finds, skipping CD drives. Works in QEMU, VirtualBox and on PCs whose
 * SATA controller is set to IDE/legacy mode. */
#include <stdint.h>
#include "kernel.h"

static uint16_t io_base, ctrl_base;
static int slave;
static uint32_t disk_sectors;
static char disk_model[41];
static int present;

static inline void insw(uint16_t port, void *buf, uint32_t n) { __asm__ volatile("rep insw" : "+D"(buf), "+c"(n) : "d"(port) : "memory"); }
static inline void outsw(uint16_t port, const void *buf, uint32_t n) { __asm__ volatile("rep outsw" : "+S"(buf), "+c"(n) : "d"(port)); }
static void delay400(uint16_t ctrl) { for (int i = 0; i < 4; i++) inb(ctrl); }

/* Wait until the drive is not busy. Returns status, or -1 on timeout. */
static int wait_ready(void) {
    for (int i = 0; i < 2000000; i++) {
        uint8_t s = inb(io_base + 7);
        if (!(s & 0x80)) return s;
    }
    return -1;
}
static int wait_drq(void) {
    for (int i = 0; i < 2000000; i++) {
        uint8_t s = inb(io_base + 7);
        if (s & 0x01 || s & 0x20) return -1;          /* error / drive fault */
        if (!(s & 0x80) && (s & 0x08)) return 0;      /* ready to transfer */
    }
    return -1;
}

static int identify(uint16_t base, uint16_t ctrl, int sl) {
    outb(ctrl, 0x02);                                   /* no interrupts: we poll */
    outb(base + 6, 0xA0 | (sl << 4));
    delay400(ctrl);
    outb(base + 2, 0); outb(base + 3, 0); outb(base + 4, 0); outb(base + 5, 0);
    outb(base + 7, 0xEC);                               /* IDENTIFY DEVICE */
    if (inb(base + 7) == 0 || inb(base + 7) == 0xFF) return 0;   /* nothing there */
    for (int i = 0; i < 2000000; i++) if (!(inb(base + 7) & 0x80)) break;
    if (inb(base + 4) || inb(base + 5)) return 0;      /* ATAPI (CD) or SATA: not ours */
    for (int i = 0; i < 2000000; i++) {
        uint8_t s = inb(base + 7);
        if (s & 0x01) return 0;
        if (s & 0x08) break;
    }
    uint16_t id[256];
    io_base = base;
    insw(base, id, 256);
    disk_sectors = id[60] | ((uint32_t)id[61] << 16);
    for (int i = 0; i < 20; i++) { disk_model[i * 2] = id[27 + i] >> 8; disk_model[i * 2 + 1] = id[27 + i] & 0xFF; }
    disk_model[40] = 0;
    for (int i = 39; i >= 0 && disk_model[i] == ' '; i--) disk_model[i] = 0;
    io_base = base; ctrl_base = ctrl; slave = sl;
    return disk_sectors > 0;
}

int ata_init(void) {
    static const uint16_t bases[2] = { 0x1F0, 0x170 }, ctrls[2] = { 0x3F6, 0x376 };
    for (int b = 0; b < 2; b++)
        for (int s = 0; s < 2; s++)
            if (identify(bases[b], ctrls[b], s)) { present = 1; return 1; }
    return 0;
}

int ata_present(void) { return present; }
uint32_t ata_sectors(void) { return disk_sectors; }
const char *ata_model(void) { return disk_model; }

static int setup(uint32_t lba, uint8_t count, uint8_t cmd) {
    if (!present || lba + count > disk_sectors) return 0;
    if (wait_ready() < 0) return 0;
    outb(io_base + 6, 0xE0 | (slave << 4) | ((lba >> 24) & 0x0F));
    delay400(ctrl_base);
    outb(io_base + 2, count);
    outb(io_base + 3, lba & 0xFF);
    outb(io_base + 4, (lba >> 8) & 0xFF);
    outb(io_base + 5, (lba >> 16) & 0xFF);
    outb(io_base + 7, cmd);
    return 1;
}

/* Read `count` (1-255) 512-byte sectors starting at `lba` into buf. */
int ata_read(uint32_t lba, uint8_t count, void *buf) {
    if (!setup(lba, count, 0x20)) return 0;            /* READ SECTORS */
    uint8_t *p = buf;
    for (int i = 0; i < count; i++, p += 512) {
        if (wait_drq() < 0) return 0;
        insw(io_base, p, 256);
        delay400(ctrl_base);
    }
    return 1;
}

int ata_write(uint32_t lba, uint8_t count, const void *buf) {
    if (!setup(lba, count, 0x30)) return 0;            /* WRITE SECTORS */
    const uint8_t *p = buf;
    for (int i = 0; i < count; i++, p += 512) {
        if (wait_drq() < 0) return 0;
        outsw(io_base, p, 256);
        delay400(ctrl_base);
    }
    outb(io_base + 7, 0xE7);                            /* FLUSH CACHE */
    return wait_ready() >= 0;
}
