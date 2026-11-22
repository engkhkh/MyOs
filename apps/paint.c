/* paint.c - a simple Paint program.
 * Left mouse button paints, right button erases. Pick colours and brush sizes
 * from the toolbar, use the bucket to fill areas, and Save (or Ctrl+S) writes
 * C:\Pictures\painting.bmp - a normal BMP image that Windows can open too.
 * The picture is kept in the program's own memory (allocated with malloc) and
 * copied to the window with blit(). */
#include "myos.h"

#define W      760              /* window size */
#define H      540
#define TOOL_H 44               /* toolbar height */
#define STAT_H 20               /* status bar height */
#define PW     W                /* picture size */
#define PH     (H - TOOL_H - STAT_H)
#define FILE   "/Pictures/painting.bmp"

static uint32_t *pic;
static const uint32_t palette[14] = {
    0x000000, 0x6B7280, 0xFFFFFF, 0xDC2626, 0xF97316, 0xFACC15, 0x84CC16,
    0x16A34A, 0x06B6D4, 0x2563EB, 0x7C3AED, 0xDB2777, 0x92400E, 0xFBCFE8 };
static const int sizes[4] = { 2, 5, 10, 20 };
static int colour = 0, size_i = 1, fill_mode = 0;
static char status[80] = "Left button paints, right button erases.";

/* ---------- toolbar ---------- */
#define SW_X(i)   (8 + (i) * 30)
#define SZ_X(i)   (440 + (i) * 34)
#define FILL_X    580
#define CLEAR_X   636
#define SAVE_X    694

static void button(int x, int w, const char *label, int on) {
    fill(x, 9, w, 26, on ? 0x2563EB : 0xD1D5DB);
    fill(x + 1, 10, w - 2, 24, on ? 0xDBEAFE : 0xF9FAFB);
    text(x + (w - strlen(label) * 8) / 2, 14, label, 0x111827);
}

static void draw_toolbar(void) {
    fill(0, 0, W, TOOL_H, 0xE5E7EB);
    for (int i = 0; i < 14; i++) {
        fill(SW_X(i) - 2, 7, 30, 30, i == colour ? 0x111827 : 0xE5E7EB);
        fill(SW_X(i), 9, 26, 26, 0x9CA3AF);
        fill(SW_X(i) + 1, 10, 24, 24, palette[i]);
    }
    for (int i = 0; i < 4; i++) {
        int on = i == size_i && !fill_mode;
        fill(SZ_X(i), 9, 30, 26, on ? 0x2563EB : 0xD1D5DB);
        fill(SZ_X(i) + 1, 10, 28, 24, on ? 0xDBEAFE : 0xF9FAFB);
        int d = sizes[i] > 20 ? 20 : sizes[i];
        if (d < 3) d = 3;
        fill(SZ_X(i) + 15 - d / 2, 22 - d / 2, d, d, 0x111827);
    }
    button(FILL_X, 50, "Fill", fill_mode);
    button(CLEAR_X, 52, "New", 0);
    button(SAVE_X, 58, "Save", 0);
}

static void draw_status(void) {
    fill(0, H - STAT_H, W, STAT_H, 0xF3F4F6);
    text(6, H - STAT_H + 2, status, 0x4B5563);
}
static void set_status(const char *s) { int i = 0; for (; s[i] && i < 79; i++) status[i] = s[i]; status[i] = 0; draw_status(); }

/* ---------- picture ---------- */
static void show(int x0, int y0, int x1, int y1) {          /* copy a rectangle of the picture to the window */
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > PW) x1 = PW;
    if (y1 > PH) y1 = PH;
    if (x0 >= x1 || y0 >= y1) return;
    blit(x0, TOOL_H + y0, x1 - x0, y1 - y0, pic + y0 * PW + x0, PW);
}

static void dab(int cx, int cy, int r, uint32_t c) {        /* round brush mark */
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x * x + y * y <= r * r) {
                int px = cx + x, py = cy + y;
                if (px >= 0 && py >= 0 && px < PW && py < PH) pic[py * PW + px] = c;
            }
}

static void stroke(int x0, int y0, int x1, int y1, int r, uint32_t c) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, ax = x0, ay = y0;
    for (;;) {
        dab(ax, ay, r, c);
        if (ax == x1 && ay == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; ax += sx; }
        if (e2 <= dx) { err += dx; ay += sy; }
    }
    int lx = x0 < x1 ? x0 : x1, hx = x0 > x1 ? x0 : x1, ly = y0 < y1 ? y0 : y1, hy = y0 > y1 ? y0 : y1;
    show(lx - r, ly - r, hx + r + 1, hy + r + 1);
}

/* Flood fill using a stack of horizontal runs, so it never recurses deeply. */
static void flood(int sx, int sy, uint32_t c) {
    uint32_t old = pic[sy * PW + sx];
    if (old == c) return;
    int cap = PW * PH / 4, top = 0;
    int *stack = malloc(cap * sizeof(int));
    if (!stack) { set_status("Not enough memory to fill."); return; }
    stack[top++] = sy * PW + sx;
    while (top) {
        int p = stack[--top], y = p / PW, x = p % PW;
        if (pic[p] != old) continue;
        int l = x, rr = x;
        while (l > 0 && pic[y * PW + l - 1] == old) l--;
        while (rr < PW - 1 && pic[y * PW + rr + 1] == old) rr++;
        for (int i = l; i <= rr; i++) pic[y * PW + i] = c;
        for (int ny = y - 1; ny <= y + 1; ny += 2) {
            if (ny < 0 || ny >= PH) continue;
            for (int i = l; i <= rr; i++)
                if (pic[ny * PW + i] == old && (i == l || pic[ny * PW + i - 1] != old) && top < cap)
                    stack[top++] = ny * PW + i;
        }
    }
    free(stack);
    show(0, 0, PW, PH);
}

/* ---------- BMP files (24-bit, bottom-up rows) ---------- */
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
#define ROW   ((PW * 3 + 3) & ~3)
#define BMPSZ (54 + ROW * PH)

static void save(void) {
    uint8_t *f = calloc(BMPSZ, 1);
    if (!f) { set_status("Not enough memory to save."); return; }
    f[0] = 'B'; f[1] = 'M'; put32(f + 2, BMPSZ); put32(f + 10, 54);
    put32(f + 14, 40); put32(f + 18, PW); put32(f + 22, PH);
    f[26] = 1; f[28] = 24; put32(f + 34, ROW * PH);
    put32(f + 38, 2835); put32(f + 42, 2835);                 /* 72 dpi */
    for (int y = 0; y < PH; y++) {
        uint8_t *row = f + 54 + (PH - 1 - y) * ROW;
        for (int x = 0; x < PW; x++) {
            uint32_t c = pic[y * PW + x];
            row[x * 3] = c; row[x * 3 + 1] = c >> 8; row[x * 3 + 2] = c >> 16;
        }
    }
    int ok = writefile(FILE, f, BMPSZ) == BMPSZ;
    free(f);
    set_status(ok ? "Saved to C:\\Pictures\\painting.bmp" : "Could not save (is there a C:\\Pictures folder?)");
}

static int load(void) {
    uint8_t *f = malloc(BMPSZ);
    if (!f) return 0;
    int n = readfile(FILE, f, BMPSZ);
    int ok = n == BMPSZ && f[0] == 'B' && f[1] == 'M' && get32(f + 18) == PW && get32(f + 22) == PH && f[28] == 24;
    if (ok)
        for (int y = 0; y < PH; y++) {
            uint8_t *row = f + 54 + (PH - 1 - y) * ROW;
            for (int x = 0; x < PW; x++) pic[y * PW + x] = row[x * 3] | row[x * 3 + 1] << 8 | row[x * 3 + 2] << 16;
        }
    free(f);
    return ok;
}

static void blank(void) { for (int i = 0; i < PW * PH; i++) pic[i] = 0xFFFFFF; show(0, 0, PW, PH); }

int main(void) {
    settitle("Paint");
    resize(W, H);
    pic = malloc(PW * PH * 4);
    if (!pic) { print("Not enough memory.\n"); getkey(); return 1; }
    draw_toolbar();
    if (load()) { show(0, 0, PW, PH); set_status("Opened C:\\Pictures\\painting.bmp"); }
    else { blank(); draw_status(); }

    int last_x = -1, last_y = -1;
    struct event e;
    for (;;) {
        getevent(&e);
        if (e.type == EV_KEY) {
            if (e.key == KEY_CTRL('s')) save();
            else if (e.key == KEY_ESC) return 0;
            continue;
        }
        int px = e.x, py = e.y - TOOL_H;
        if (e.type == EV_MOUSE_DOWN && e.y < TOOL_H) {          /* toolbar clicks */
            for (int i = 0; i < 14; i++) if (e.x >= SW_X(i) && e.x < SW_X(i) + 26) colour = i;
            for (int i = 0; i < 4; i++) if (e.x >= SZ_X(i) && e.x < SZ_X(i) + 30) { size_i = i; fill_mode = 0; }
            if (e.x >= FILL_X && e.x < FILL_X + 50) fill_mode = !fill_mode;
            if (e.x >= CLEAR_X && e.x < CLEAR_X + 52) { blank(); set_status("New picture."); }
            if (e.x >= SAVE_X && e.x < SAVE_X + 58) save();
            draw_toolbar();
            last_x = -1;
            continue;
        }
        if (py < 0 || py >= PH) { last_x = -1; continue; }
        uint32_t c = (e.buttons & 2) ? 0xFFFFFF : palette[colour];
        int r = sizes[size_i] / 2 > 0 ? sizes[size_i] / 2 : 1;
        if (e.type == EV_MOUSE_DOWN) {
            if (fill_mode) { flood(px, py, c); continue; }
            stroke(px, py, px, py, r, c);
            last_x = px; last_y = py;
        } else if (e.type == EV_MOUSE_MOVE && (e.buttons & 3) && !fill_mode) {
            if (last_x < 0) { last_x = px; last_y = py; }
            stroke(last_x, last_y, px, py, r, c);
            last_x = px; last_y = py;
        } else if (e.type == EV_MOUSE_UP) last_x = -1;
    }
}
