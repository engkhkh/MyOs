/* idt.c - GDT + TSS, Interrupt Descriptor Table, PIC remapping, PIT timer, IRQs.
 *
 * Segments (GDT):  0x08 kernel code  0x10 kernel data   (ring 0)
 *                  0x18 user code    0x20 user data     (ring 3, used as 0x1B / 0x23)
 *                  0x28 TSS - tells the CPU which kernel stack to use when a
 *                       user program is interrupted or makes a system call. */
#include <stdint.h>
#include "kernel.h"

struct idt_entry { uint16_t base_lo, sel; uint8_t zero, flags; uint16_t base_hi; } __attribute__((packed));
struct idt_ptr   { uint16_t limit; uint32_t base; } __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr idtp;
extern uint32_t isr_stub_table[48];
extern void isr128(void);
extern void idt_load(struct idt_ptr *);

/* ---------- GDT and TSS ---------- */
struct gdt_ptr { uint16_t limit; uint32_t base; } __attribute__((packed));
static uint64_t gdt[6];
static struct gdt_ptr gdtp;
static struct tss {
    uint32_t prev, esp0, ss0, esp1, ss1, esp2, ss2, cr3, eip, eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi, es, cs, ss, ds, fs, gs, ldt;
    uint16_t trap, iomap;
} __attribute__((packed)) tss;
extern void gdt_flush(struct gdt_ptr *);
extern void tss_flush(void);

static uint64_t seg(uint32_t base, uint32_t limit, uint8_t access, uint8_t flags) {
    uint64_t d = limit & 0xFFFF;
    d |= (uint64_t)(base & 0xFFFFFF) << 16;
    d |= (uint64_t)access << 40;
    d |= (uint64_t)((limit >> 16) & 0xF) << 48;
    d |= (uint64_t)(flags & 0xF) << 52;
    d |= (uint64_t)(base >> 24) << 56;
    return d;
}

static void gdt_init(void) {
    gdt[0] = 0;
    gdt[1] = seg(0, 0xFFFFF, 0x9A, 0xC);    /* kernel code */
    gdt[2] = seg(0, 0xFFFFF, 0x92, 0xC);    /* kernel data */
    gdt[3] = seg(0, 0xFFFFF, 0xFA, 0xC);    /* user code (DPL 3) */
    gdt[4] = seg(0, 0xFFFFF, 0xF2, 0xC);    /* user data (DPL 3) */
    tss.ss0 = 0x10;
    tss.iomap = sizeof(tss);                /* no I/O port access for user programs */
    gdt[5] = seg((uint32_t)&tss, sizeof(tss) - 1, 0x89, 0x0);
    gdtp.limit = sizeof(gdt) - 1;
    gdtp.base = (uint32_t)gdt;
    gdt_flush(&gdtp);
    tss_flush();
}

/* Kernel stack the CPU switches to when a ring-3 task is interrupted. */
void tss_set_kernel_stack(uint32_t esp0) { tss.esp0 = esp0; }

static void idt_set(int n, uint32_t base, uint8_t flags) {
    idt[n].base_lo = base & 0xFFFF;
    idt[n].base_hi = base >> 16;
    idt[n].sel = 0x08;
    idt[n].zero = 0;
    idt[n].flags = flags;         /* 0x8E: ring-0 interrupt gate, 0xEE: callable from ring 3 */
}

/* Move IRQs 0-15 from vectors 8-15/0x70-0x77 to 32-47 so they don't clash with CPU exceptions. */
static void pic_remap(void) {
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 0x20); outb(0xA1, 0x28);
    outb(0x21, 0x04); outb(0xA1, 0x02);
    outb(0x21, 0x01); outb(0xA1, 0x01);
    outb(0x21, 0xD8);              /* unmask IRQ0 (timer), IRQ1 (keyboard), IRQ2 (cascade), IRQ5 (sound) */
    outb(0xA1, 0xEF);              /* unmask IRQ12 (PS/2 mouse) */
}

/* Programmable Interval Timer: fire IRQ0 at TIMER_HZ. */
static void pit_init(void) {
    uint32_t div = 1193182 / TIMER_HZ;
    outb(0x43, 0x36);
    outb(0x40, div & 0xFF);
    outb(0x40, div >> 8);
}

/* ---- Keyboard ring buffer filled by IRQ1 ---- */
static volatile uint8_t kbuf[64];
static volatile uint8_t khead = 0, ktail = 0;
int kb_read_scancode(void) {
    if (khead == ktail) return -1;
    uint8_t sc = kbuf[ktail];
    ktail = (ktail + 1) % sizeof(kbuf);
    return sc;
}

volatile uint32_t ticks = 0;

static const char *exc_names[32] = {
    "DIVIDE_BY_ZERO", "DEBUG", "NMI", "BREAKPOINT", "OVERFLOW", "BOUND_RANGE",
    "INVALID_OPCODE", "DEVICE_NOT_AVAILABLE", "DOUBLE_FAULT", "COPROCESSOR_SEGMENT",
    "INVALID_TSS", "SEGMENT_NOT_PRESENT", "STACK_SEGMENT_FAULT", "GENERAL_PROTECTION_FAULT",
    "PAGE_FAULT", "RESERVED", "X87_FLOATING_POINT", "ALIGNMENT_CHECK", "MACHINE_CHECK",
    "SIMD_FLOATING_POINT", "VIRTUALIZATION", "CONTROL_PROTECTION",
};

/* Called from isr_common for every interrupt. Returns the stack to resume. */
uint32_t isr_handler(struct regs *r) {
    proc_save(r);                               /* remember where this task stopped */
    if (r->int_no < 32) {
        const char *name = exc_names[r->int_no] ? exc_names[r->int_no] : "RESERVED";
        if ((r->cs & 3) == 3) return proc_fault(name, r);   /* a program crashed: kill only it */
        panic_screen(name, r);                  /* the kernel itself crashed */
    }
    if (r->int_no == 128) return proc_syscall(r);
    int preempt = 0;
    if (r->int_no == 32) {
        ticks++;
        if (ticks % TIMER_HZ == 0) on_second();
        sound_tick();
        preempt = 1;
    } else if (r->int_no == 37) {
        sound_irq();
    } else if (r->int_no == 44 || r->int_no == 33) {
        /* Keyboard and mouse share one data port. The status port says whose byte is
         * waiting (bit 5 = mouse), so neither handler can steal the other's byte -
         * a stolen mouse byte used to throw a whole movement away. */
        for (int n = 0; n < 4; n++) {
            uint8_t st = inb(0x64);
            if (!(st & 1)) break;
            uint8_t v = inb(0x60);
            if (st & 0x20) mouse_byte(v);
            else {
                uint8_t next = (khead + 1) % sizeof(kbuf);
                if (next != ktail) { kbuf[khead] = v; khead = next; }
            }
        }
    }
    if (r->int_no >= 40) outb(0xA0, 0x20);   /* EOI to slave PIC */
    outb(0x20, 0x20);                         /* EOI to master PIC */
    return preempt ? proc_tick(r) : (uint32_t)r;
}

void interrupts_init(void) {
    gdt_init();
    for (int i = 0; i < 48; i++) idt_set(i, isr_stub_table[i], 0x8E);
    idt_set(128, (uint32_t)isr128, 0xEE);
    idtp.limit = sizeof(idt) - 1;
    idtp.base = (uint32_t)&idt;
    idt_load(&idtp);
    pic_remap();
    pit_init();
    __asm__ volatile("sti");
}
