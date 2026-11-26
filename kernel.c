/* kernel.c - MyOS entry point and main event loop.
 * Boot order: memory -> tasks -> interrupts -> disk -> graphics -> mouse -> desktop.
 * If no graphics adapter is found, MyOS falls back to a text-mode shell. */
#include <stdint.h>
#include <stddef.h>
#include "kernel.h"

/* ---------- timing ---------- */
static volatile int second_flag;
void on_second(void) { second_flag = 1; }      /* called from the timer interrupt */
void sleep_ms(uint32_t ms) { uint32_t end = ticks + ms * TIMER_HZ / 1000; while (ticks < end) __asm__ volatile("hlt"); }
void reboot(void) { while (inb(0x64) & 2) {} outb(0x64, 0xFE); for (;;) __asm__ volatile("hlt"); }

/* ---------- real-time clock ---------- */
static int tz_offset;
static uint8_t cmos(uint8_t r) { outb(0x70, r); return inb(0x71); }
static int bcd(uint8_t v) { return (v & 0x0F) + (v >> 4) * 10; }
void rtc_time(int *h, int *m, int *s) {
    while (cmos(0x0A) & 0x80) {}
    *s = bcd(cmos(0x00)); *m = bcd(cmos(0x02)); *h = (bcd(cmos(0x04)) + tz_offset + 24) % 24;
}
void rtc_date(int *d, int *mo, int *y) {
    while (cmos(0x0A) & 0x80) {}
    *d = bcd(cmos(0x07)); *mo = bcd(cmos(0x08)); *y = 2000 + bcd(cmos(0x09));
    int h = bcd(cmos(0x04)) + tz_offset;                     /* the clock is UTC: the time zone can */
    static const int mdays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };   /* move the date too */
    int leap = (*y % 4 == 0 && *y % 100 != 0) || *y % 400 == 0;
    if (h >= 24) {
        if (++*d > mdays[*mo - 1] + (*mo == 2 && leap)) { *d = 1; if (++*mo > 12) { *mo = 1; ++*y; } }
    } else if (h < 0) {
        if (--*d < 1) {
            if (--*mo < 1) { *mo = 12; --*y; }
            leap = (*y % 4 == 0 && *y % 100 != 0) || *y % 400 == 0;
            *d = mdays[*mo - 1] + (*mo == 2 && leap);
        }
    }
}
int rtc_tz(void) { return tz_offset; }
void rtc_set_tz(int h) { if (h >= -12 && h <= 14) tz_offset = h; }

/* ---------- keyboard: scancode -> key ---------- */
static const char keymap[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,'a','s',
    'd','f','g','h','j','k','l',';','\'','`',0,'\\','z','x','c','v',
    'b','n','m',',','.','/',0,'*',0,' ',0,
};
static const char keymap_shift[128] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+','\b','\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,'A','S',
    'D','F','G','H','J','K','L',':','"','~',0,'|','Z','X','C','V',
    'B','N','M','<','>','?',0,'*',0,' ',0,
};
static int shift, caps, ctrl, alt, extended;
static int win, win_combo;
/* Arabic (101) keyboard: the letter on each key. Alt+Shift or Win+Space switches. */
static int layout_ar, kbd_pending;
static const unsigned short ar_keys[128] = {
    [0x10]=0x636,[0x11]=0x635,[0x12]=0x62B,[0x13]=0x642,[0x14]=0x641,[0x15]=0x63A,[0x16]=0x639,[0x17]=0x647,
    [0x18]=0x62E,[0x19]=0x62D,[0x1A]=0x62C,[0x1B]=0x62F,
    [0x1E]=0x634,[0x1F]=0x633,[0x20]=0x64A,[0x21]=0x628,[0x22]=0x644,[0x23]=0x627,[0x24]=0x62A,[0x25]=0x646,
    [0x26]=0x645,[0x27]=0x643,[0x28]=0x637,[0x29]=0x630,
    [0x2C]=0x626,[0x2D]=0x621,[0x2E]=0x624,[0x2F]=0x631,[0x30]=0xFEFB,[0x31]=0x649,[0x32]=0x629,[0x33]=0x648,
    [0x34]=0x632,[0x35]=0x638,
};
static const unsigned short ar_keys_shift[128] = {
    [0x23]=0x623,[0x15]=0x625,[0x31]=0x622,[0x22]=0xFEF7,[0x14]=0xFEF9,[0x30]=0xFEF5,
    [0x25]=0x60C,[0x35]=0x61F,[0x27]=0x61B,
};
int kbd_layout(void) { return layout_ar; }
int kbd_toggle_layout(void) { layout_ar = !layout_ar; return layout_ar; }
int kbd_shift_down(void) { return shift; }
int kbd_ctrl_down(void) { return ctrl; }

static int decode(uint8_t sc) {
    if (sc == 0xE0) { extended = 1; return -1; }
    int ext = extended; extended = 0;
    if (sc == 0x1D) { ctrl = 1; return -1; }
    if (sc == 0x9D) { ctrl = 0; return -1; }
    if (sc == 0x38) { alt = 1; if (shift) { layout_ar = !layout_ar; return KEY_LAYOUT; } return -1; }   /* Alt (Shift+Alt: language) */
    if (sc == 0xB8) { alt = 0; return -1; }
    if (ext && (sc == 0x2A || sc == 0xAA)) return -1;          /* the "fake shift" around Print Screen */
    if (sc == 0x2A || sc == 0x36) { int was = shift; shift = 1; if (alt && !was && !ctrl) { layout_ar = !layout_ar; return KEY_LAYOUT; } return -1; }
    if (sc == 0xAA || sc == 0xB6) { shift = 0; return -1; }
    /* The Windows key: alone it opens Start (when let go); held with L / E / D it is a shortcut */
    if (ext && (sc == 0x5B || sc == 0x5C)) { win = 1; win_combo = 0; return -1; }
    if (ext && (sc == 0xDB || sc == 0xDC)) { win = 0; return win_combo ? -1 : KEY_START; }
    if (sc & 0x80) return -1;                                  /* key released */
    if (win) {
        win_combo = 1;
        if (sc == 0x0F) return KEY_TASKVIEW;                   /* Win+Tab: Task View */
        if (sc == 0x39) { layout_ar = !layout_ar; return KEY_LAYOUT; }   /* Win+Space: keyboard language */
        if (ctrl && ext && sc == 0x4D) return KEY_DESK_NEXT;   /* Win+Ctrl+Right / Left: next / previous desktop */
        if (ctrl && ext && sc == 0x4B) return KEY_DESK_PREV;
        if (ctrl && sc == 0x20) return KEY_DESK_NEW;           /* Win+Ctrl+D: a new desktop */
        if (ctrl && sc == 0x3E) return KEY_DESK_CLOSE;         /* Win+Ctrl+F4: close this desktop */
        if (sc == 0x26) return KEY_LOCK;                       /* Win+L: lock */
        if (sc == 0x12) return KEY_WIN_E;                      /* Win+E: File Explorer */
        if (sc == 0x20) return KEY_WIN_D;                      /* Win+D: show the desktop */
        return -1;
    }
    if (alt && sc == 0x3E) return KEY_ALTF4;                    /* Alt+F4: close the window */
    if (alt && sc == 0x0F) return KEY_ALTTAB;                   /* Alt+Tab: switch windows */
    if (alt && sc == 0x1C) return KEY_ALTENTER;                 /* Alt+Enter: Properties */
    if (ext) {
        if (sc == 0x48) return KEY_UP;
        if (sc == 0x50) return KEY_DOWN;
        if (sc == 0x4B) return KEY_LEFT;
        if (sc == 0x4D) return KEY_RIGHT;
        if (sc == 0x53) return KEY_DEL;
        if (sc == 0x37) return KEY_PRTSC;                      /* Print Screen */
        if (sc == 0x5D) return KEY_MENU;                       /* the Menu key */
        if (sc == 0x1C) return '\n';                           /* keypad Enter */
        if (sc == 0x49) return KEY_PGUP;
        if (sc == 0x51) return KEY_PGDN;
        if (sc == 0x47) return KEY_HOME;
        if (sc == 0x4F) return KEY_END;
        return -1;
    }
    if (sc == 0x3A) { caps = !caps; return -1; }
    if (sc == 0x3B) return KEY_F1;
    if (sc == 0x3C) return KEY_F2;
    if (sc == 0x3D) return KEY_F3;
    if (sc == 0x01) return KEY_ESC;
    if (sc == 0x53) return KEY_DEL;
    char c = shift ? keymap_shift[sc] : keymap[sc];
    if (layout_ar && !ctrl && !alt) {                          /* Arabic letters */
        int cp = shift ? ar_keys_shift[sc] : ar_keys[sc];
        if (cp >= 0xFEF5) {                                    /* the lam-alef key types two letters */
            int alef = cp == 0xFEF5 ? 0x622 : cp == 0xFEF7 ? 0x623 : cp == 0xFEF9 ? 0x625 : 0x627;
            kbd_pending = KEY_UNI(alef);
            return KEY_UNI(0x644);
        }
        if (cp) return KEY_UNI(cp);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return -1;
    }
    if (ctrl && c >= 'a' && c <= 'z') return KEY_CTRL(c);
    if (ctrl && c >= 'A' && c <= 'Z') return KEY_CTRL(c + 32);
    if (caps && c >= 'a' && c <= 'z') c -= 32;
    else if (caps && c >= 'A' && c <= 'Z') c += 32;
    return c ? c : -1;
}

/* ---------- crash screen ---------- */
static void hexstr(char *b, uint32_t v) {
    const char *d = "0123456789ABCDEF";
    b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 8; i++) b[9 - i] = d[(v >> (i * 4)) & 0xF];
    b[10] = 0;
}

static void text_panic(const char *name, struct regs *r) {
    volatile uint16_t *vga = (uint16_t *)0xB8000;
    for (int i = 0; i < 80 * 25; i++) vga[i] = 0x1F20;
    const char *lines[] = { ":(", "", "Your PC ran into a problem and needs to restart.", "", "Stop code: " };
    for (int l = 0; l < 5; l++) for (int i = 0; lines[l][i]; i++) vga[(3 + l) * 80 + 4 + i] = 0x1F00 | (uint8_t)lines[l][i];
    for (int i = 0; name[i]; i++) vga[7 * 80 + 15 + i] = 0x1F00 | (uint8_t)name[i];
    char b[11]; hexstr(b, r->eip);
    for (int i = 0; b[i]; i++) vga[9 * 80 + 9 + i] = 0x1F00 | (uint8_t)b[i];
    vga[9 * 80 + 4] = 0x1F00 | 'E'; vga[9 * 80 + 5] = 0x1F00 | 'I'; vga[9 * 80 + 6] = 0x1F00 | 'P'; vga[9 * 80 + 7] = 0x1F00 | ':';
}

void panic_screen(const char *name, struct regs *r) {
    __asm__ volatile("cli");
    if (gfx_available()) {
        gfx_panic_begin();
        uint32_t bg = 0x1060C0, fg = 0xFFFFFF;
        gfx_fill(0, 0, gfx_w, gfx_h, bg);
        int x = gfx_w / 8, y = gfx_h / 5;
        gfx_text2x(x, y, ":(", fg);
        gfx_text2x(x, y + 60, "Your PC ran into a problem", fg);
        gfx_text2x(x, y + 96, "and needs to restart.", fg);
        gfx_text(x, y + 150, "MyOS stopped the processor to protect your data.", 0xDBEAFE, 0);
        gfx_text(x, y + 190, "Stop code:", 0xDBEAFE, 0);
        gfx_text(x + 96, y + 190, name, fg, 1);
        char b[11];
        const char *lab[] = { "EIP", "ERR", "EAX", "ESP", "EBP", "EFLAGS" };
        uint32_t val[] = { r->eip, r->err_code, r->eax, r->esp, r->ebp, r->eflags };
        for (int i = 0; i < 6; i++) {
            hexstr(b, val[i]);
            gfx_text(x + (i % 3) * 180, y + 222 + (i / 3) * 20, lab[i], 0xBFDBFE, 0);
            gfx_text(x + (i % 3) * 180 + 56, y + 222 + (i / 3) * 20, b, fg, 0);
        }
        if (r->int_no == 14) {
            uint32_t cr2; __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
            hexstr(b, cr2);
            gfx_text(x, y + 270, "Bad address:", 0xBFDBFE, 0);
            gfx_text(x + 104, y + 270, b, fg, 0);
            gfx_text(x + 200, y + 270, (r->err_code & 2) ? "(write to unmapped page)" : "(read from unmapped page)", 0xBFDBFE, 0);
        }
        gfx_text(x, y + 320, "Press any key to restart...", fg, 1);
        gfx_panic_end();
    } else {
        text_panic(name, r);
    }
    for (volatile uint32_t i = 0; i < 50000000; i++) {}   /* let pending key releases arrive */
    while (inb(0x64) & 1) inb(0x60);
    for (;;) { while (!(inb(0x64) & 1)) {} uint8_t st = inb(0x64); uint8_t v = inb(0x60); if (!(st & 0x20) && !(v & 0x80)) break; }
    reboot();
    for (;;) {}
}

/* ---------- entry ---------- */
void kernel_main(uint32_t magic, struct mb_info *mbi) {
    if (magic != 0x2BADB002) mbi = 0;
    mm_init(mbi);
    outb(0x3D4, 0x0A); outb(0x3D5, 0x20);                   /* hide text cursor */
    proc_init();                                             /* task 0 = the kernel itself */
    interrupts_init();
    if (ata_init()) fs_init();                               /* find the disk, mount drive C: */
    net_init();                                              /* network card (address comes by DHCP) */
    settings_load(); settings_apply();                       /* time zone, volume... from C:\settings.ini */
    con_init();

    int gui = gfx_init(mbi, 1024, 768);
    sound_init();                                            /* Sound Blaster, if there is one */
    if (gui) { mouse_init(); wm_init(); }
    /* start-up chime on the PC speaker */
    if (cfg.chime) { sound_tone(523, 90); sound_tone(659, 90); sound_tone(784, 90); sound_tone(1047, 180); }

    for (;;) {
        proc_reap();                                        /* clean up programs that ended */
        net_poll(); net_timer(); net_job_tick();            /* network: packets, retries, ping */
        if (proc_any_ready()) proc_yield();                 /* let programs run */
        else { kernel_idle = 1; __asm__ volatile("hlt"); kernel_idle = 0; }   /* nothing to do: sleep */
        int sc;
        while ((sc = kb_read_scancode()) >= 0) {
            int k = decode((uint8_t)sc);
            if (k < 0) continue;
            if (gui) wm_key(k); else if (k < 0x10000) con_key(k);
            if (kbd_pending) { k = kbd_pending; kbd_pending = 0; if (gui) wm_key(k); }
        }
        if (gui) {
            struct mouse_event e;
            while (mouse_read(&e)) wm_mouse(e.dx, e.dy, e.buttons);
            if (second_flag) { second_flag = 0; wm_tick(); }
            wm_explorer_background();
            wm_update();
        } else if (con_dirty) {
            con_render_text();
        }
    }
}
