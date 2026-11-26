/* kernel.h - shared declarations for MyOS. */
#ifndef KERNEL_H
#define KERNEL_H
#include <stdint.h>
#include <stddef.h>
#include "syscalls.h"

#define TIMER_HZ 100

static inline void outb(uint16_t p, uint8_t v) { __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p)); }
static inline uint8_t inb(uint16_t p) { uint8_t v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }

/* Register snapshot pushed by interrupts.asm (order matters). */
struct regs {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags;
    uint32_t useresp, ss;           /* only present when interrupted in ring 3 */
};

extern volatile uint32_t ticks;
void interrupts_init(void);
int kb_read_scancode(void);

/* Multiboot info passed by the bootloader (only the fields we use). */
struct mb_info {
    uint32_t flags, mem_lower, mem_upper, boot_device, cmdline;
    uint32_t mods_count, mods_addr, syms[4];
    uint32_t mmap_length, mmap_addr;
    uint32_t drives_length, drives_addr, config_table, boot_loader_name, apm_table;
    uint32_t vbe_control_info, vbe_mode_info;
    uint16_t vbe_mode, vbe_interface_seg, vbe_interface_off, vbe_interface_len;
    uint64_t fb_addr;
    uint32_t fb_pitch, fb_width, fb_height;
    uint8_t fb_bpp, fb_type;
} __attribute__((packed));

/* mm.c - memory management */
struct mm_stats {
    uint32_t total_kb, used_kb, free_kb;
    uint32_t heap_mapped_kb, heap_used, heap_blocks;
};
void mm_init(struct mb_info *mbi);
void mm_stats(struct mm_stats *s);
uint32_t pmm_alloc(void);
void pmm_free(uint32_t addr);
int vmm_map(uint32_t virt, uint32_t phys);
void vmm_unmap(uint32_t virt);
uint32_t pmm_alloc_low(void);
#define USER_BASE 0x40000000u          /* programs live between 1 GB ... */
#define USER_TOP  0x80000000u          /* ... and 2 GB */
uint32_t *vmm_kernel_dir(void);
uint32_t *vmm_new_space(void);
int vmm_map_user(uint32_t *pd, uint32_t virt, uint32_t phys);
int vmm_user_ok(uint32_t *pd, uint32_t virt, uint32_t len);
uint32_t vmm_free_space(uint32_t *pd);
void vmm_switch(uint32_t *pd);
void *kmalloc(size_t size);
void kfree(void *p);

/* Special key codes (above the ASCII range) */
#define KEY_ESC   27
#define KEY_F1    0x101
#define KEY_UP    0x102
#define KEY_DOWN  0x103
#define KEY_START 0x104
#define KEY_LEFT  0x105
#define KEY_RIGHT 0x106
#define KEY_DEL   0x107
#define KEY_PGUP  0x108
#define KEY_PGDN  0x109
#define KEY_HOME  0x10A
#define KEY_END   0x10B
#define KEY_F2    0x10C
#define KEY_F3    0x10D
#define KEY_ALTF4 0x10E
#define KEY_ALTTAB 0x10F
#define KEY_ALTENTER 0x110            /* Alt+Enter: Properties */
#define KEY_MENU  0x111               /* the Menu key (right-click menu) */
#define KEY_PRTSC 0x112               /* Print Screen: a screenshot */
#define KEY_LOCK  0x113               /* Win+L */
#define KEY_WIN_E 0x114               /* Win+E */
#define KEY_WIN_D 0x115               /* Win+D */
#define KEY_TASKVIEW   0x116          /* Win+Tab */
#define KEY_DESK_NEXT  0x117          /* Win+Ctrl+Right */
#define KEY_DESK_PREV  0x118          /* Win+Ctrl+Left */
#define KEY_DESK_NEW   0x119          /* Win+Ctrl+D */
#define KEY_DESK_CLOSE 0x11A          /* Win+Ctrl+F4 */
#define KEY_CTRL(c) (0x200 | (c))           /* Ctrl + letter, e.g. KEY_CTRL('s') */

/* kernel.c */
void on_second(void);
void panic_screen(const char *name, struct regs *r) __attribute__((noreturn));
void sleep_ms(uint32_t ms);
void reboot(void) __attribute__((noreturn));
void rtc_time(int *h, int *m, int *s);
void rtc_date(int *d, int *mo, int *y);
int rtc_tz(void);
void rtc_set_tz(int hours);

/* shell.c - command shell on a character grid */
#define CON_COLS 76
#define CON_ROWS 22
struct cell { char ch; uint8_t fg, bg; };
extern struct cell con_grid[CON_ROWS][CON_COLS];
extern int con_cx, con_cy;
extern volatile int con_dirty;
void con_init(void);
void con_key(int key);
void con_putc(char c);
void con_print(const char *s);
void con_num(uint32_t n);
void con_render_text(void);
int kstrcmp(const char *a, const char *b);

/* gfx.c - framebuffer graphics */
extern int gfx_w, gfx_h;
int gfx_init(struct mb_info *mbi, int w, int h);
int gfx_available(void);
void gfx_clip(int x, int y, int w, int h);
void gfx_noclip(void);
void gfx_pixel(int x, int y, uint32_t c);
void gfx_fill(int x, int y, int w, int h, uint32_t c);
void gfx_rfill(int x, int y, int w, int h, int r, uint32_t c);
void gfx_rect(int x, int y, int w, int h, uint32_t c);
void gfx_vgradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void gfx_shade(int x, int y, int w, int h, int amount);
void gfx_glow(int cx, int cy, int r, uint32_t c, int strength);
void gfx_line(int x0, int y0, int x1, int y1, uint32_t c);
void gfx_thick_line(int x0, int y0, int x1, int y1, int t, uint32_t c);
void gfx_circle(int cx, int cy, int r, uint32_t c, int filled);
void gfx_char(int x, int y, char ch, uint32_t c, int bold);
void gfx_text(int x, int y, const char *s, uint32_t c, int bold);
void gfx_text2x(int x, int y, const char *s, uint32_t c);
void wm_notify(const char *title, const char *text, const char *path);
void wm_lock(void);
void wm_screenshot(void);
extern int gfx_theme;                    /* 1 = draw in dark mode colours (theme.h) */
void gfx_text_scaled(int x, int y, const char *s, uint32_t c, int scale);
const uint32_t *gfx_backbuffer(void);
/* png_write.c */
uint8_t *png_encode(const uint32_t *px, int w, int h, int *len);
uint32_t gfx_blend(uint32_t a, uint32_t b, int t, int n);
uint32_t *gfx_snapshot(void);
void gfx_restore(const uint32_t *s);
void gfx_present(void);
void gfx_present_rect(int x, int y, int w, int h);
void gfx_blit(int x, int y, int w, int h, const uint32_t *src);
void gfx_cursor_move(int x, int y);
void gfx_cursor_redraw(void);
void gfx_tint(int x, int y, int w, int h, uint32_t c, int amount);
void gfx_cursor_shape(int shape);          /* 0 arrow, 1 <->, 2 up-down, 3 \\, 4 / */
void gfx_panic_begin(void);
void gfx_panic_end(void);

/* mouse.c - PS/2 mouse */
struct mouse_event { int dx, dy, buttons; };
void mouse_init(void);
void mouse_byte(uint8_t b);
int mouse_read(struct mouse_event *e);

/* ata.c - IDE/ATA hard disk */
int ata_init(void);
int ata_present(void);
uint32_t ata_sectors(void);
const char *ata_model(void);
int ata_read(uint32_t lba, uint8_t count, void *buf);
int ata_write(uint32_t lba, uint8_t count, const void *buf);

/* fat32.c - FAT32 file system. Paths look like "/DOCS/notes.txt". */
#define FS_NAME_MAX 64
#define FS_PATH_MAX 256
struct fs_entry { char name[FS_NAME_MAX]; int is_dir; uint32_t size; uint16_t date, time; };
extern volatile uint32_t fs_generation;
int fs_init(void);
int fs_mounted(void);
uint32_t fs_total_kb(void);
uint32_t fs_free_kb(void);
const char *fs_label(void);
int fs_list(const char *path, struct fs_entry *arr, int max);
int fs_stat(const char *path, uint32_t *size);
int fs_read(const char *path, void *buf, uint32_t max);
int fs_write(const char *path, const void *data, uint32_t len);
int fs_mkdir(const char *path);
int fs_delete(const char *path);
int fs_rename(const char *path, const char *newname);
int fs_canonical(const char *path, char *out);   /* fix letter case to match the disk */

/* shell.c path helpers */
void path_resolve(const char *base, const char *in, char *out);   /* -> absolute "/a/b" */
void path_display(const char *path, char *out);                   /* "/a/b" -> "C:\a\b" */

/* idt.c */
void tss_set_kernel_stack(uint32_t esp0);

/* sound.c - PC speaker + Sound Blaster 16 */
void sound_tone(uint32_t freq, uint32_t ms);
void sound_tick(void);
int sound_init(void);
int sound_card_present(void);
void sound_irq(void);
int sound_play(const void *data, uint32_t bytes, uint32_t hz, int bits, int channels, int pid);
void sound_stop(void);
void sound_stop_owner(int pid);
int sound_position(void);
void sound_set_volume(int v);
int sound_get_volume(void);

/* e1000.c - network card */
int e1000_init(void);
int e1000_present(void);
void e1000_mac(uint8_t out[6]);
int e1000_link_up(void);
uint16_t e1000_device_id(void);
int e1000_send(const void *data, uint32_t len);
int e1000_receive(void *buf);

/* net.c - network stack (addresses in normal order: 10.0.2.15 = 0x0A00020F) */
struct net_info { int present; uint8_t mac[6]; uint32_t ip, mask, gateway, dns, rx_packets, tx_packets; };
extern struct net_info netcfg;
int net_init(void);
void net_poll(void);
void net_timer(void);
int net_resolve(const char *name, uint32_t *ip);   /* 1 found, 0 still asking, -1 failed */
int tcp_open(uint32_t ip, uint16_t port, int owner);
int tcp_status(int sock, int owner);
int tcp_send(int sock, int owner, const void *data, uint32_t len);
int tcp_recv(int sock, int owner, void *buf, uint32_t max);
void tcp_close(int sock, int owner);
void net_close_owner(int owner);
void net_ip_text(uint32_t ip, char *buf);
int net_job_start(int kind, const char *host);      /* 1 = ping, 2 = nslookup */
void net_job_tick(void);
void net_job_cancel(void);
int kbd_shift_down(void);
int kbd_ctrl_down(void);                          /* kernel.c: is Ctrl held right now? */
void wm_max_client(int *w, int *h);              /* wm.c: the biggest client area a window can have */
int proc_set_size(int slot, int w, int h);       /* proc.c: the user resized a program window */
int wm_set_wallpaper(const char *path);          /* wm.c: picture file -> desktop background */                         /* kernel.c: is Shift held right now? */
#define CLIP_MAX 65536
void clip_set(const char *s, int len);            /* wm.c: the system clipboard (text) */
const char *clip_text(void);
int clip_len(void);
void shell_job_done(void);                           /* shell.c: background command finished */

/* proc.c - processes and scheduler */
#define MAX_TASKS 16
#define PROC_CANVAS_W 480
#define PROC_CANVAS_H 320
enum { T_FREE, T_READY, T_WAIT_KEY, T_SLEEP, T_ZOMBIE, T_WAIT_EVENT };
struct task {
    int pid, state, is_user, exit_code;
    int resizable, min_w, min_h;     /* the user may resize the window (SYS_RESIZABLE) */
    char name[32], title[48];
    uint32_t esp, kstack, wake;
    uint32_t *pd;
    uint32_t cpu_ticks, cpu_last, cpu_pct, pages, start_tick;
    uint32_t *canvas; int cw, ch, tx, ty; uint32_t fg, bg;
    struct event evq[32]; int eh, et;      /* keyboard + mouse events waiting */
    uint32_t brk, brk_base;                /* end of the program's heap */
    uint8_t fpu[108] __attribute__((aligned(16)));   /* floating-point registers, saved on task switch */
    char arg[FS_PATH_MAX];                 /* what it was started with (e.g. a file to open) */
};
extern struct task tasks[MAX_TASKS];
extern volatile int kernel_idle;
void proc_init(void);
void proc_save(struct regs *r);
uint32_t proc_tick(struct regs *r);
uint32_t proc_syscall(struct regs *r);
uint32_t proc_fault(const char *name, struct regs *r);
int proc_spawn(const char *path);       /* returns PID, or <0: -1 missing, -2 too many, -3 memory, -4 not a program */
int proc_spawn_arg(const char *path, const char *arg);
int proc_kill(int pid);
void proc_reap(void);
void proc_key(int slot, int key);
void proc_mouse(int slot, int type, int x, int y, int buttons);
int proc_any_ready(void);
void proc_yield(void);
int proc_current(void);

/* settings.c - user settings in C:\settings.ini */
#define WALLPAPER_PRESETS 4                  /* wallpaper 0..3 = built-in designs, 4 = a picture */
#define ACCENT_COUNT 6
struct settings {
    int wallpaper; char picture[FS_PATH_MAX];
    int accent, timezone, volume, chime, mouse; int explorer_view; /* mouse speed 1 slow .. 4 fastest */
    int dark;                                /* 1 = dark mode */
    char password[12];                       /* "" = no password; else a hash in hex (see shell "password") */
};
extern struct settings cfg;
void settings_load(void);
int settings_save(void);
void settings_apply(void);
void password_hash(const char *pw, char out[12]);

/* wm.c - desktop / window manager */
void wm_init(void);
void wm_mouse(int dx, int dy, int buttons);
void wm_key(int key);
void wm_tick(void);
void wm_update(void);
void wm_explorer_background(void);              /* wm.c: makes one picture preview if one is needed */
int wm_open(const char *name);
void wm_open_file(const char *path);      /* open a text file in Notepad */
void wm_open_folder(const char *path);    /* open a folder in File Explorer */
void wm_proc_opened(int slot);            /* a program started: give it a window */
void wm_proc_closed(int slot);            /* a program ended: remove its window */
void wm_proc_resized(int slot);           /* a program changed its window size */
void wm_proc_crashed(const char *name, const char *msg);
void wm_redraw(void);
void wm_canvas_dirty(int slot);          /* only this program's window contents changed */
uint32_t wm_bench(int frames);

#endif
