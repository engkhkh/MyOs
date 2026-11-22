/* e1000.c - Intel PRO/1000 (82540EM "e1000") network card driver.
 *
 * QEMU and VirtualBox emulate this card, and it was common in real PCs.
 * The card is controlled through memory-mapped registers. Packets move with
 * DMA: we give the card two rings of "descriptors", each pointing at a 2 KB
 * buffer in memory.
 *   - Receive ring: the card writes each arriving packet into the next free
 *     buffer and marks its descriptor "done". We poll for done descriptors.
 *   - Transmit ring: we copy a packet into a buffer, fill in its descriptor
 *     and bump the tail register; the card sends it on its own.
 */
#include <stdint.h>
#include "kernel.h"

#define REG_CTRL   0x0000
#define REG_STATUS 0x0008
#define REG_EERD   0x0014
#define REG_IMC    0x00D8
#define REG_RCTL   0x0100
#define REG_TCTL   0x0400
#define REG_TIPG   0x0410
#define REG_RDBAL  0x2800
#define REG_RDBAH  0x2804
#define REG_RDLEN  0x2808
#define REG_RDH    0x2810
#define REG_RDT    0x2818
#define REG_TDBAL  0x3800
#define REG_TDBAH  0x3804
#define REG_TDLEN  0x3808
#define REG_TDH    0x3810
#define REG_TDT    0x3818
#define REG_MTA    0x5200
#define REG_RAL    0x5400
#define REG_RAH    0x5404

#define NRX 32
#define NTX 8
#define BUFSZ 2048

struct rx_desc { uint64_t addr; uint16_t len, csum; uint8_t status, errors; uint16_t special; } __attribute__((packed));
struct tx_desc { uint64_t addr; uint16_t len; uint8_t cso, cmd, status, css; uint16_t special; } __attribute__((packed));

/* Rings and buffers live in the kernel image, which is identity-mapped,
 * so their virtual address is also the physical address the card needs. */
static struct rx_desc rx[NRX] __attribute__((aligned(16)));
static struct tx_desc tx[NTX] __attribute__((aligned(16)));
static uint8_t rxbuf[NRX][BUFSZ] __attribute__((aligned(16)));
static uint8_t txbuf[NTX][BUFSZ] __attribute__((aligned(16)));
static volatile uint8_t *mmio;
static int rx_next, tx_next, present;
static uint8_t mac_addr[6];
static uint32_t pci_loc;
static uint16_t dev_id;

static uint32_t rd(uint32_t r) { return *(volatile uint32_t *)(mmio + r); }
static void wr(uint32_t r, uint32_t v) { *(volatile uint32_t *)(mmio + r) = v; }

static uint32_t pci_cfg(uint32_t loc, uint8_t off) {
    __asm__ volatile("outl %0, %1" :: "a"(loc | (off & 0xFC)), "Nd"((uint16_t)0xCF8));
    uint32_t v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"((uint16_t)0xCFC));
    return v;
}
static void pci_cfg_write(uint32_t loc, uint8_t off, uint32_t v) {
    __asm__ volatile("outl %0, %1" :: "a"(loc | (off & 0xFC)), "Nd"((uint16_t)0xCF8));
    __asm__ volatile("outl %0, %1" :: "a"(v), "Nd"((uint16_t)0xCFC));
}

static uint16_t eeprom_read(uint8_t addr) {
    wr(REG_EERD, ((uint32_t)addr << 8) | 1);
    for (int i = 0; i < 100000; i++) { uint32_t v = rd(REG_EERD); if (v & (1 << 4)) return v >> 16; }
    return 0;
}

int e1000_init(void) {
    /* find the card on the PCI bus */
    for (int bus = 0; bus < 8 && !pci_loc; bus++)
        for (int dev = 0; dev < 32; dev++) {
            uint32_t loc = 0x80000000u | (bus << 16) | (dev << 11);
            uint32_t id = pci_cfg(loc, 0);
            uint16_t ven = id & 0xFFFF, did = id >> 16;
            if (ven == 0x8086 && (did == 0x100E || did == 0x100F || did == 0x1004 || did == 0x100C || did == 0x1015)) {
                pci_loc = loc; dev_id = did; break;
            }
        }
    if (!pci_loc) return 0;
    uint32_t bar = pci_cfg(pci_loc, 0x10) & 0xFFFFFFF0u;
    pci_cfg_write(pci_loc, 0x04, pci_cfg(pci_loc, 0x04) | 0x6);   /* memory access + bus mastering (DMA) */
    for (uint32_t off = 0; off < 0x20000; off += 4096)
        if (!vmm_map(bar + off, bar + off)) return 0;
    mmio = (volatile uint8_t *)bar;

    wr(REG_IMC, 0xFFFFFFFF);                                       /* no interrupts: we poll */
    wr(REG_CTRL, rd(REG_CTRL) | (1u << 26));                       /* reset */
    for (volatile int i = 0; i < 200000; i++) {}
    wr(REG_IMC, 0xFFFFFFFF);
    wr(REG_CTRL, (rd(REG_CTRL) | (1 << 6)) & ~(1u << 3));          /* set link up */

    /* MAC address: from the receive-address register, or the EEPROM */
    uint32_t lo = rd(REG_RAL), hi = rd(REG_RAH);
    if (lo == 0 && (hi & 0xFFFF) == 0) {
        for (int i = 0; i < 3; i++) { uint16_t w = eeprom_read(i); mac_addr[i * 2] = w & 0xFF; mac_addr[i * 2 + 1] = w >> 8; }
        wr(REG_RAL, mac_addr[0] | mac_addr[1] << 8 | mac_addr[2] << 16 | (uint32_t)mac_addr[3] << 24);
        wr(REG_RAH, mac_addr[4] | mac_addr[5] << 8 | (1u << 31));
    } else {
        for (int i = 0; i < 4; i++) mac_addr[i] = lo >> (i * 8);
        mac_addr[4] = hi & 0xFF; mac_addr[5] = (hi >> 8) & 0xFF;
        wr(REG_RAH, hi | (1u << 31));                              /* address valid */
    }
    for (int i = 0; i < 128; i++) wr(REG_MTA + i * 4, 0);

    /* receive ring */
    for (int i = 0; i < NRX; i++) { rx[i].addr = (uint32_t)rxbuf[i]; rx[i].status = 0; }
    wr(REG_RDBAL, (uint32_t)rx); wr(REG_RDBAH, 0);
    wr(REG_RDLEN, sizeof rx);
    wr(REG_RDH, 0); wr(REG_RDT, NRX - 1);
    wr(REG_RCTL, (1 << 1) | (1 << 15) | (1 << 26));               /* enable, accept broadcast, strip CRC, 2 KB buffers */
    /* transmit ring */
    for (int i = 0; i < NTX; i++) { tx[i].addr = (uint32_t)txbuf[i]; tx[i].status = 1; tx[i].cmd = 0; }
    wr(REG_TDBAL, (uint32_t)tx); wr(REG_TDBAH, 0);
    wr(REG_TDLEN, sizeof tx);
    wr(REG_TDH, 0); wr(REG_TDT, 0);
    wr(REG_TCTL, (1 << 1) | (1 << 3) | (0x10 << 4) | (0x40 << 12)); /* enable, pad short packets */
    wr(REG_TIPG, 0x0060200A);
    present = 1;
    return 1;
}

int e1000_present(void) { return present; }
void e1000_mac(uint8_t out[6]) { for (int i = 0; i < 6; i++) out[i] = mac_addr[i]; }
int e1000_link_up(void) { return present && (rd(REG_STATUS) & 2); }
uint16_t e1000_device_id(void) { return dev_id; }

int e1000_send(const void *data, uint32_t len) {
    if (!present || len > BUFSZ) return 0;
    struct tx_desc *d = &tx[tx_next];
    for (int i = 0; i < 100000 && !(d->status & 1); i++) {}       /* wait until this slot is free */
    if (!(d->status & 1)) return 0;
    const uint8_t *s = data;
    for (uint32_t i = 0; i < len; i++) txbuf[tx_next][i] = s[i];
    d->len = (uint16_t)len;
    d->cmd = 0x01 | 0x02 | 0x08;                                   /* end of packet, add CRC, report status */
    d->status = 0;
    tx_next = (tx_next + 1) % NTX;
    wr(REG_TDT, tx_next);                                          /* go */
    return 1;
}

/* Copy the next received packet into buf; returns its length, or 0 if none. */
int e1000_receive(void *buf) {
    if (!present) return 0;
    struct rx_desc *d = &rx[rx_next];
    if (!(d->status & 1)) return 0;
    int len = d->len;
    if (len > BUFSZ) len = BUFSZ;
    uint8_t *o = buf;
    for (int i = 0; i < len; i++) o[i] = rxbuf[rx_next][i];
    d->status = 0;
    wr(REG_RDT, rx_next);                                          /* hand the buffer back */
    rx_next = (rx_next + 1) % NRX;
    return len;
}
