/* gfx.c - Linear framebuffer graphics for MyOS.
 *
 * Finding the screen:
 *   1. If the bootloader (e.g. GRUB on a real PC) already set a 32-bit graphics
 *      mode, it tells us the framebuffer address in the Multiboot info.
 *   2. Otherwise we program the Bochs/QEMU/VirtualBox display adapter ("BGA")
 *      directly through its I/O ports and find its memory via the PCI bus.
 *
 * Everything is drawn into an off-screen "back buffer" first and then copied
 * to the screen in one go, so the user never sees half-drawn frames.
 */
#include <stdint.h>
#include <stddef.h>
#include "kernel.h"
#include "theme.h"
#include "font.h"

static uint32_t *fb;            /* the real screen */
static uint32_t *back;          /* off-screen buffer, same size */
static uint32_t fb_pitch;       /* bytes per screen row */
int gfx_w, gfx_h;
static int gfx_ok;

/* ---------- fast memory helpers ---------- */
static inline void memset32(uint32_t *d, uint32_t v, uint32_t n) {
    __asm__ volatile("rep stosl" : "+D"(d), "+c"(n) : "a"(v) : "memory");
}
static inline void memcpy32(uint32_t *d, const uint32_t *s, uint32_t n) {
    __asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(n) :: "memory");
}

/* ---------- PCI: find the display adapter's memory ---------- */
static uint32_t pci_read(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    uint32_t addr = 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC);
    __asm__ volatile("outl %0, %1" :: "a"(addr), "Nd"((uint16_t)0xCF8));
    uint32_t v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"((uint16_t)0xCFC));
    return v;
}
static uint32_t find_display_bar0(void) {
    for (int bus = 0; bus < 8; bus++)
        for (int dev = 0; dev < 32; dev++) {
            uint32_t id = pci_read(bus, dev, 0, 0);
            if ((id & 0xFFFF) == 0xFFFF) continue;
            uint32_t cls = pci_read(bus, dev, 0, 8) >> 24;
            if (cls == 0x03) {                                  /* display controller */
                uint32_t bar = pci_read(bus, dev, 0, 0x10);
                if (!(bar & 1)) return bar & 0xFFFFFFF0u;      /* memory BAR */
            }
        }
    return 0;
}

/* ---------- Bochs Graphics Adapter ---------- */
static void bga_write(uint16_t idx, uint16_t val) {
    __asm__ volatile("outw %0, %1" :: "a"(idx), "Nd"((uint16_t)0x1CE));
    __asm__ volatile("outw %0, %1" :: "a"(val), "Nd"((uint16_t)0x1CF));
}
static uint16_t bga_read(uint16_t idx) {
    __asm__ volatile("outw %0, %1" :: "a"(idx), "Nd"((uint16_t)0x1CE));
    uint16_t v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"((uint16_t)0x1CF));
    return v;
}
static int bga_set_mode(int w, int h) {
    uint16_t id = bga_read(0);
    if (id < 0xB0C0 || id > 0xB0CF) return 0;                  /* no BGA present */
    bga_write(4, 0);                                            /* disable */
    bga_write(1, w); bga_write(2, h); bga_write(3, 32);         /* xres, yres, bpp */
    bga_write(4, 0x41);                                         /* enable + linear framebuffer */
    return 1;
}

int gfx_init(struct mb_info *mbi, int want_w, int want_h) {
    uint32_t phys = 0;
    if (mbi && (mbi->flags & (1 << 12)) && mbi->fb_type == 1 && mbi->fb_bpp == 32) {
        phys = (uint32_t)mbi->fb_addr;
        gfx_w = mbi->fb_width; gfx_h = mbi->fb_height; fb_pitch = mbi->fb_pitch;
    } else {
        phys = find_display_bar0();
        if (!phys || !bga_set_mode(want_w, want_h)) return 0;
        gfx_w = want_w; gfx_h = want_h; fb_pitch = want_w * 4;
    }
    /* Map the framebuffer into our address space (same virtual = physical address). */
    uint32_t bytes = fb_pitch * gfx_h;
    for (uint32_t off = 0; off < bytes; off += 4096)
        if (!vmm_map(phys + off, phys + off)) return 0;
    fb = (uint32_t *)phys;
    back = kmalloc(gfx_w * gfx_h * 4);
    if (!back) return 0;
    gfx_ok = 1;
    return 1;
}
int gfx_available(void) { return gfx_ok; }

/* ---------- drawing into the back buffer ---------- */
static int clip_x0 = 0, clip_y0 = 0, clip_x1 = 1 << 30, clip_y1 = 1 << 30;
void gfx_clip(int x, int y, int w, int h) { clip_x0 = x; clip_y0 = y; clip_x1 = x + w; clip_y1 = y + h; }
void gfx_noclip(void) { clip_x0 = clip_y0 = 0; clip_x1 = clip_y1 = 1 << 30; }

static inline int cx0(void) { return clip_x0 > 0 ? clip_x0 : 0; }
static inline int cy0(void) { return clip_y0 > 0 ? clip_y0 : 0; }
static inline int cx1(void) { return clip_x1 < gfx_w ? clip_x1 : gfx_w; }
static inline int cy1(void) { return clip_y1 < gfx_h ? clip_y1 : gfx_h; }

/* dark mode: while gfx_theme is on, colours go through theme_dark_map() (see theme.h) */
int gfx_theme;
#define THEMED(c) (gfx_theme ? theme_dark_map(c) : (c))
void gfx_pixel(int x, int y, uint32_t c) {
    c = THEMED(c);
    if (x >= cx0() && x < cx1() && y >= cy0() && y < cy1()) back[y * gfx_w + x] = c;
}

void gfx_fill(int x, int y, int w, int h, uint32_t c) {
    int x0 = x < cx0() ? cx0() : x, y0 = y < cy0() ? cy0() : y;
    int x1 = x + w > cx1() ? cx1() : x + w, y1 = y + h > cy1() ? cy1() : y + h;
    if (x0 >= x1 || y0 >= y1) return;
    c = THEMED(c);
    for (int j = y0; j < y1; j++) memset32(&back[j * gfx_w + x0], c, x1 - x0);
}

void gfx_rect(int x, int y, int w, int h, uint32_t c) {
    gfx_fill(x, y, w, 1, c); gfx_fill(x, y + h - 1, w, 1, c);
    gfx_fill(x, y, 1, h, c); gfx_fill(x + w - 1, y, 1, h, c);
}

static uint32_t mix(uint32_t a, uint32_t b, int t, int n) {      /* blend a->b by t/n */
    uint32_t r = ((a >> 16 & 255) * (n - t) + (b >> 16 & 255) * t) / n;
    uint32_t g = ((a >> 8 & 255) * (n - t) + (b >> 8 & 255) * t) / n;
    uint32_t bl = ((a & 255) * (n - t) + (b & 255) * t) / n;
    return r << 16 | g << 8 | bl;
}
uint32_t gfx_blend(uint32_t a, uint32_t b, int t, int n) { return mix(a, b, t, n); }

void gfx_vgradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom) {
    for (int j = 0; j < h; j++) gfx_fill(x, y + j, w, 1, mix(top, bottom, j, h > 1 ? h - 1 : 1));
}

/* Darken the pixels in a rectangle (used for soft window shadows).
 * amount is 0..256; done with multiply + shift, no division, so it is fast. */
/* See-through colour over a rectangle: amount/256 of c (the snap preview, ...). */
void gfx_tint(int x, int y, int w, int h, uint32_t c, int amount) {
    c = THEMED(c);
    int x0 = x < cx0() ? cx0() : x, y0 = y < cy0() ? cy0() : y;
    int x1 = x + w > cx1() ? cx1() : x + w, y1 = y + h > cy1() ? cy1() : y + h;
    for (int j = y0; j < y1; j++) {
        uint32_t *row = &back[j * gfx_w];
        for (int i = x0; i < x1; i++) row[i] = mix(row[i], c, amount, 256);
    }
}
void gfx_shade(int x, int y, int w, int h, int amount) {
    int x0 = x < cx0() ? cx0() : x, y0 = y < cy0() ? cy0() : y;
    int x1 = x + w > cx1() ? cx1() : x + w, y1 = y + h > cy1() ? cy1() : y + h;
    uint32_t keep = 256 - amount;
    for (int j = y0; j < y1; j++) {
        uint32_t *row = &back[j * gfx_w];
        for (int i = x0; i < x1; i++) {
            uint32_t p = row[i];
            uint32_t rb = ((p & 0xFF00FF) * keep >> 8) & 0xFF00FF;
            uint32_t gg = ((p & 0x00FF00) * keep >> 8) & 0x00FF00;
            row[i] = rb | gg;
        }
    }
}

void gfx_line(int x0, int y0, int x1, int y1, uint32_t c) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        gfx_pixel(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void gfx_thick_line(int x0, int y0, int x1, int y1, int t, uint32_t c) {
    for (int a = -t / 2; a <= t / 2; a++)
        for (int b = -t / 2; b <= t / 2; b++) gfx_line(x0 + a, y0 + b, x1 + a, y1 + b, c);
}

void gfx_circle(int cx, int cy, int r, uint32_t c, int filled) {
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++) {
            int d = x * x + y * y;
            if (filled ? d <= r * r : (d <= r * r && d >= (r - 2) * (r - 2))) gfx_pixel(cx + x, cy + y, c);
        }
}

void gfx_char(int x, int y, char ch, uint32_t c, int bold) {
    const uint8_t *g = (bold ? font_bold : font_regular)[(uint8_t)ch & 127];
    for (int j = 0; j < 16; j++) {
        uint8_t row = g[j];
        if (!row) continue;
        for (int i = 0; i < 8; i++) if (row & (0x80 >> i)) gfx_pixel(x + i, y + j, c);
    }
}

void gfx_text(int x, int y, const char *s, uint32_t c, int bold) {
    for (; *s; s++, x += 8) gfx_char(x, y, *s, c, bold);
}

/* Same text drawn at 2x size (used for big headings). */
void gfx_text2x(int x, int y, const char *s, uint32_t c) {
    for (; *s; s++, x += 16) {
        const uint8_t *g = font_bold[(uint8_t)*s & 127];
        for (int j = 0; j < 16; j++)
            for (int i = 0; i < 8; i++)
                if (g[j] & (0x80 >> i)) gfx_fill(x + i * 2, y + j * 2, 2, 2, c);
    }
}

/* Text at any whole-number size (the lock screen's big clock). */
void gfx_text_scaled(int x, int y, const char *s, uint32_t c, int scale) {
    for (; *s; s++, x += 8 * scale) {
        const uint8_t *gl = font_bold[(uint8_t)*s & 127];
        for (int j = 0; j < 16; j++)
            for (int i = 0; i < 8; i++)
                if (gl[j] & (0x80 >> i)) gfx_fill(x + i * scale, y + j * scale, scale, scale, c);
    }
}
/* The finished picture (for screenshots). */
const uint32_t *gfx_backbuffer(void) { return back; }

/* Save / restore the whole back buffer (for a pre-rendered wallpaper). */
uint32_t *gfx_snapshot(void) {
    uint32_t *s = kmalloc(gfx_w * gfx_h * 4);
    if (s) memcpy32(s, back, gfx_w * gfx_h);
    return s;
}
void gfx_restore(const uint32_t *s) { memcpy32(back, s, gfx_w * gfx_h); }

/* ---------- presenting to the screen ---------- */
void gfx_present(void) {
    if (fb_pitch == (uint32_t)gfx_w * 4) memcpy32(fb, back, gfx_w * gfx_h);
    else for (int y = 0; y < gfx_h; y++) memcpy32((uint32_t *)((uint8_t *)fb + y * fb_pitch), &back[y * gfx_w], gfx_w);
}

/* Copy just one rectangle of the back buffer to the screen. */
void gfx_present_rect(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > gfx_w) w = gfx_w - x;
    if (y + h > gfx_h) h = gfx_h - y;
    if (w <= 0 || h <= 0) return;
    for (int j = y; j < y + h; j++) memcpy32((uint32_t *)((uint8_t *)fb + j * fb_pitch) + x, &back[j * gfx_w + x], w);
}

/* ---------- mouse cursor (drawn straight onto the screen) ---------- */
static const char *cursor_img[19] = {
    "X          ", "XX         ", "X.X        ", "X..X       ", "X...X      ",
    "X....X     ", "X.....X    ", "X......X   ", "X.......X  ", "X........X ",
    "X.....XXXXX", "X..X..X    ", "X.X X..X   ", "XX  X..X   ", "X    X..X  ",
    "     X..X  ", "      X..X ", "      XXX  ", "           ",
};
static int cur_x = -100, cur_y = -100;

/* Pointer shapes: 0 arrow, 1 left-right, 2 up-down, 3 diagonal \, 4 diagonal / (window edges).
 * Each is a 19x19 grid (0 = see-through, 1 = black, 2 = white) with a "hot spot". */
static uint8_t shape_px[5][19][19];
static const int shape_hot[5][2] = { { 0, 0 }, { 9, 9 }, { 9, 9 }, { 9, 9 }, { 9, 9 } };
static int cur_shape, shapes_ready;
static const char *harrow[9] = {
    "     X       X     ",
    "    XX       XX    ",
    "   X.X       X.X   ",
    "  X..XXXXXXXXX..X  ",
    " X...............X ",
    "  X..XXXXXXXXX..X  ",
    "   X.X       X.X   ",
    "    XX       XX    ",
    "     X       X     " };
static void build_shapes(void) {
    for (int j = 0; j < 19; j++)
        for (int i = 0; i < 11; i++) { char c = cursor_img[j][i]; shape_px[0][j][i] = c == 'X' ? 1 : c == '.' ? 2 : 0; }
    for (int j = 0; j < 9; j++)
        for (int i = 0; i < 19; i++) {
            char c = harrow[j][i]; uint8_t v = c == 'X' ? 1 : c == '.' ? 2 : 0;
            shape_px[1][j + 5][i] = v;                       /* left-right */
            shape_px[2][i][j + 5] = v;                       /* up-down: the same, turned */
        }
    for (int j = 0; j < 17; j++)                             /* diagonals, from geometry */
        for (int i = 0; i < 17; i++) {
            int d = i - j < 0 ? j - i : i - j, v = 0;
            int head1 = i <= 6 && j <= 6 && i + j <= 7, head2 = i >= 10 && j >= 10 && i + j >= 25;
            if (head1) v = (i == 0 || j == 0 || i + j == 7) ? 1 : 2;
            else if (head2) v = (i == 16 || j == 16 || i + j == 25) ? 1 : 2;
            else if (d <= 1) v = 2;
            else if (d == 2) v = 1;
            shape_px[3][j + 1][i + 1] = (uint8_t)v;
            shape_px[4][j + 1][17 - i] = (uint8_t)v;         /* the other diagonal: mirrored */
        }
    shapes_ready = 1;
}

static void restore_under_cursor(void) {
    int ox = cur_x - shape_hot[cur_shape][0], oy = cur_y - shape_hot[cur_shape][1];
    for (int j = 0; j < 19; j++)
        for (int i = 0; i < 19; i++) {
            int x = ox + i, y = oy + j;
            if (x >= 0 && x < gfx_w && y >= 0 && y < gfx_h)
                ((uint32_t *)((uint8_t *)fb + y * fb_pitch))[x] = back[y * gfx_w + x];
        }
}
static void paint_cursor(void) {
    if (!shapes_ready) build_shapes();
    int ox = cur_x - shape_hot[cur_shape][0], oy = cur_y - shape_hot[cur_shape][1];
    for (int j = 0; j < 19; j++)
        for (int i = 0; i < 19; i++) {
            uint8_t p = shape_px[cur_shape][j][i];
            int x = ox + i, y = oy + j;
            if (!p || x < 0 || x >= gfx_w || y < 0 || y >= gfx_h) continue;
            ((uint32_t *)((uint8_t *)fb + y * fb_pitch))[x] = p == 1 ? 0x000000 : 0xFFFFFF;
        }
}
/* Change the pointer's shape (0 arrow, 1 <->, 2 up-down, 3 \, 4 /). */
void gfx_cursor_shape(int shape) {
    if (shape == cur_shape || shape < 0 || shape > 4) return;
    restore_under_cursor(); cur_shape = shape; paint_cursor();
}
/* Move the cursor without redrawing anything else. */
void gfx_cursor_move(int x, int y) { restore_under_cursor(); cur_x = x; cur_y = y; paint_cursor(); }
/* After gfx_present() the cursor was overwritten; put it back. */
void gfx_cursor_redraw(void) { paint_cursor(); }


/* Rounded-corner filled rectangle. */
void gfx_rfill(int x, int y, int w, int h, int r, uint32_t c) {
    gfx_fill(x + r, y, w - 2 * r, h, c);
    gfx_fill(x, y + r, r, h - 2 * r, c);
    gfx_fill(x + w - r, y + r, r, h - 2 * r, c);
    for (int j = 0; j < r; j++)
        for (int i = 0; i < r; i++) {
            int dx = r - i - 1, dy = r - j - 1;
            if (dx * dx + dy * dy > r * r) continue;
            gfx_pixel(x + i, y + j, c); gfx_pixel(x + w - 1 - i, y + j, c);
            gfx_pixel(x + i, y + h - 1 - j, c); gfx_pixel(x + w - 1 - i, y + h - 1 - j, c);
        }
}

/* Soft light blob: blends colour c into the image, strongest at the centre. */
void gfx_glow(int cx, int cy, int r, uint32_t c, int strength) {
    for (int y = -r; y <= r; y++) {
        int py = cy + y;
        if (py < cy0() || py >= cy1()) continue;
        for (int x = -r; x <= r; x++) {
            int px = cx + x;
            if (px < cx0() || px >= cx1()) continue;
            int d = x * x + y * y;
            if (d >= r * r) continue;
            int a = strength * (r * r - d) / (r * r);
            back[py * gfx_w + px] = mix(back[py * gfx_w + px], c, a, 256);
        }
    }
}

/* Crash screen support: draw straight to the screen if layouts match, since the
 * heap may be damaged; otherwise draw in the back buffer and copy at the end. */
static int panic_direct;
void gfx_panic_begin(void) {
    gfx_noclip();
    panic_direct = (fb_pitch == (uint32_t)gfx_w * 4);
    if (panic_direct) back = fb;
}
void gfx_panic_end(void) { if (!panic_direct) gfx_present(); }

/* Copy a w x h block of pixels (e.g. a program's window canvas) to (x, y). */
void gfx_blit(int x, int y, int w, int h, const uint32_t *src) {
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < cy0() || py >= cy1()) continue;
        int x0 = x < cx0() ? cx0() : x, x1 = x + w > cx1() ? cx1() : x + w;
        if (x0 < x1) memcpy32(&back[py * gfx_w + x0], &src[j * w + (x0 - x)], x1 - x0);
    }
}
