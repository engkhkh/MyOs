/* wm.c - The MyOS desktop: wallpaper, icons, taskbar, Start menu and windows.
 *
 * How it works: whenever something changes (a window moves, a key is typed,
 * the clock ticks) we set `dirty`. The main loop then calls wm_update(), which
 * redraws the whole scene into the back buffer and copies it to the screen.
 * Plain mouse movement only redraws the cursor, so it stays smooth.
 */
#include <stdint.h>
#include <stddef.h>
#include "kernel.h"
#include "image.h"

/* ---------- colours ---------- */
#define C_TASKBAR    0x101827
#define C_TASK_HI    0x2B3445
/* the accent colour is chosen in Settings */
static const uint32_t accents[ACCENT_COUNT][2] = {
    { 0x2563EB, 0x1D4ED8 }, { 0x7C3AED, 0x6D28D9 }, { 0x059669, 0x047857 },
    { 0xEA580C, 0xC2410C }, { 0xDB2777, 0xBE185D }, { 0x475569, 0x334155 } };
static uint32_t c_accent = 0x2563EB, c_accent2 = 0x1D4ED8, c_accent_hi = 0x3B82F6, c_accent_light = 0xDBEAFE;
#define C_ACCENT       c_accent         /* title bars, highlights */
#define C_ACCENT2      c_accent2        /* darker: gradient ends */
#define C_ACCENT_HI    c_accent_hi      /* lighter: hover, indicators */
#define C_ACCENT_LIGHT c_accent_light   /* very light: selected rows */
#define C_TITLE_OFF  0xE5E7EB
#define C_BORDER     0x4B5563
#define C_WHITE      0xFFFFFF
#define C_TEXT       0x111827
#define C_MUTED      0x6B7280
#define C_CLOSE_HOT  0xE81123

static const uint32_t term_palette[16] = {
    0x0C0C0C, 0x3B78FF, 0x16C60C, 0x3A96DD, 0xE74856, 0xB4009E, 0xC19C00, 0xCCCCCC,
    0x767676, 0x5E9CFF, 0x52E052, 0x61D6D6, 0xFF6B6B, 0xD670D6, 0xF9F1A5, 0xF2F2F2,
};

static const int sin60[60] = {
    0,105,208,309,407,500,588,669,743,809,866,914,951,978,995,1000,995,978,951,914,
    866,809,743,669,588,500,407,309,208,105,0,-105,-208,-309,-407,-500,-588,-669,-743,-809,
    -866,-914,-951,-978,-995,-1000,-995,-978,-951,-914,-866,-809,-743,-669,-588,-500,-407,-309,-208,-105,
};
#define COS60(i) sin60[((i) + 15) % 60]

/* ---------- apps & windows ---------- */
/* Built-in apps have fixed window ids 0..APP_COUNT-1; user programs get
 * ids APP_COUNT + their task slot. */
enum { APP_TERMINAL, APP_NOTEPAD, APP_CLOCK, APP_SYSINFO, APP_ABOUT, APP_EXPLORER, APP_TASKMGR, APP_MSG, APP_SETTINGS, APP_COUNT };
#define NWIN (APP_COUNT + MAX_TASKS)
static const char *app_names[APP_COUNT]  = { "terminal", "notepad", "clock", "sysinfo", "about", "explorer", "taskmgr", "", "settings" };
static const char *app_titles[APP_COUNT] = { "Terminal", "Notepad", "Clock", "This PC", "About MyOS", "File Explorer", "Task Manager", "Program error", "Settings" };
static const int app_w[APP_COUNT] = { CON_COLS * 8 + 14, 600, 240, 380, 400, 660, 580, 420 , 560 };
static const int app_h[APP_COUNT] = { CON_ROWS * 16 + 46, 420, 290, 390, 270, 520, 460, 190 , 506 };
static const int app_x[APP_COUNT] = { 150, 240, 720, 330, 300, 190, 400, 300 , 250 };
static const int app_y[APP_COUNT] = { 50, 110, 60, 150, 200, 60, 120, 280 , 60 };

#define TITLE_H   30
#define TASKBAR_H 44

struct window {
    int app, x, y, w, h, minimized;
    int maxed, snapped;          /* filling the screen / half of it */
    int rx, ry, rw, rh;          /* where it was before (to restore) */
};
static struct window wins[NWIN];
static int zorder[NWIN];          /* zorder[0] = bottom ... zorder[nwin-1] = top (focused) */
static int nwin;

static uint32_t *wallpaper;
static int dirty = 1, cursor_moved;
static int mx, my, mbuttons;
static int drag = -1, drag_dx, drag_dy;
static int resizing = -1, rs_mask, rs_mx, rs_my, rs_x, rs_y, rs_w, rs_h;   /* dragging a window edge */
static int snap_zone;                    /* while moving: 1 top (maximise), 2 left half, 3 right half */
static int drag_unsnap;                  /* a maximised / snapped window starts moving: restore its size first */
static int title_click_app = -1; static uint32_t title_click_tick;
static int start_open, start_hover = -1;
static int sel_icon = -1, last_click_icon = -1;
static int desk_focus;                     /* the desktop was clicked last (keys go to its icons) */
static uint32_t last_click_tick;
static int hover_key = -1;             /* identifies the hovered button, to repaint on change */
static int caret_on = 1;


/* ---------- helpers ---------- */
static int inside(int px, int py, int x, int y, int w, int h) { return px >= x && px < x + w && py >= y && py < y + h; }
static struct window *find(int app) { for (int i = 0; i < nwin; i++) if (zorder[i] == app) return &wins[app]; return 0; }

static void focus(int app) {
    desk_focus = 0;
    int i = 0;
    while (i < nwin && zorder[i] != app) i++;
    if (i == nwin) return;
    for (; i < nwin - 1; i++) zorder[i] = zorder[i + 1];
    zorder[nwin - 1] = app;
    wins[app].minimized = 0;
    dirty = 1;
}

static void open_app(int app) {
    if (find(app)) { focus(app); return; }
    wins[app] = (struct window){ app, app_x[app], app_y[app], app_w[app], app_h[app], 0, 0, 0, 0, 0, 0, 0 };
    zorder[nwin++] = app;
    dirty = 1;
}

static void close_app(int app) {
    int i = 0;
    while (i < nwin && zorder[i] != app) i++;
    if (i == nwin) return;
    for (; i < nwin - 1; i++) zorder[i] = zorder[i + 1];
    nwin--;
    if (drag == app) drag = -1;
    dirty = 1;
}

/* Topmost visible window that isn't minimized, used for keyboard focus. */
static int focused(void) {
    for (int i = nwin - 1; i >= 0; i--) if (!wins[zorder[i]].minimized) return zorder[i];
    return -1;
}

int wm_open(const char *name) {
    for (int a = 0; a < APP_COUNT; a++)
        if (app_names[a][0] && !kstrcmp(name, app_names[a])) { open_app(a); return 1; }
    return 0;
}

/* ---------- icons (drawn with code, no image files needed) ---------- */
static void icon(int kind, int x, int y, int s) {      /* s = size in pixels (40 or 20) */
    int u = s / 20;                                      /* 2 for big icons, 1 for small */
    switch (kind) {
    case APP_SYSINFO:                                    /* monitor */
        gfx_rfill(x + 2 * u, y + 3 * u, 16 * u, 11 * u, u, 0x374151);
        gfx_vgradient(x + 3 * u, y + 4 * u, 14 * u, 9 * u, 0x60A5FA, 0x1E40AF);
        gfx_fill(x + 9 * u, y + 14 * u, 2 * u, 3 * u, 0x6B7280);
        gfx_fill(x + 6 * u, y + 17 * u, 8 * u, u, 0x6B7280);
        break;
    case APP_TERMINAL:                                   /* black console with prompt */
        gfx_rfill(x + u, y + 3 * u, 18 * u, 14 * u, 2 * u, 0x111827);
        gfx_fill(x + u, y + 3 * u, 18 * u, 3 * u, 0x374151);
        gfx_line(x + 4 * u, y + 8 * u, x + 7 * u, y + 10 * u, 0x4ADE80);
        gfx_line(x + 4 * u, y + 12 * u, x + 7 * u, y + 10 * u, 0x4ADE80);
        if (u > 1) { gfx_line(x + 4 * u, y + 8 * u + 1, x + 7 * u, y + 10 * u + 1, 0x4ADE80); gfx_line(x + 4 * u, y + 12 * u + 1, x + 7 * u, y + 10 * u + 1, 0x4ADE80); }
        gfx_fill(x + 9 * u, y + 13 * u, 5 * u, u, 0x4ADE80);
        break;
    case APP_NOTEPAD:                                    /* page with lines */
        gfx_fill(x + 4 * u, y + 2 * u, 12 * u, 16 * u, 0xF9FAFB);
        gfx_rect(x + 4 * u, y + 2 * u, 12 * u, 16 * u, 0x9CA3AF);
        gfx_fill(x + 4 * u, y + 2 * u, 12 * u, 3 * u, 0x60A5FA);
        for (int i = 0; i < 4; i++) gfx_fill(x + 6 * u, y + (8 + i * 2) * u, 8 * u, u > 1 ? 2 : 1, 0x9CA3AF);
        break;
    case APP_CLOCK: {                                    /* clock face */
        int c = x + 10 * u, d = y + 10 * u;
        gfx_circle(c, d, 8 * u, 0x1F2937, 1);
        gfx_circle(c, d, 7 * u, 0xF9FAFB, 1);
        gfx_thick_line(c, d, c, d - 5 * u, u, 0x111827);
        gfx_thick_line(c, d, c + 4 * u, d, u, 0x111827);
        gfx_circle(c, d, u, 0xDC2626, 1);
        break; }
    case APP_EXPLORER:                                   /* folder */
        gfx_rfill(x + 2 * u, y + 4 * u, 7 * u, 4 * u, u, 0xD4A017);
        gfx_rfill(x + 2 * u, y + 6 * u, 16 * u, 11 * u, 2 * u, 0xF5C542);
        gfx_fill(x + 2 * u, y + 8 * u, 16 * u, u, 0xFCD34D);
        break;
    case APP_TASKMGR:                                    /* activity graph */
        gfx_rfill(x + 2 * u, y + 3 * u, 16 * u, 14 * u, 2 * u, 0x0F766E);
        gfx_thick_line(x + 4 * u, y + 12 * u, x + 8 * u, y + 8 * u, u, 0x5EEAD4);
        gfx_thick_line(x + 8 * u, y + 8 * u, x + 11 * u, y + 11 * u, u, 0x5EEAD4);
        gfx_thick_line(x + 11 * u, y + 11 * u, x + 16 * u, y + 5 * u, u, 0x5EEAD4);
        break;
    case APP_SETTINGS: {                                 /* gear */
        int c = x + 10 * u, d = y + 10 * u;
        for (int k = 0; k < 8; k++) {
            static const int tx[8] = { 0, 5, 7, 5, 0, -5, -7, -5 }, ty[8] = { -7, -5, 0, 5, 7, 5, 0, -5 };
            gfx_fill(c + tx[k] * u - u, d + ty[k] * u - u, 3 * u, 3 * u, 0x6B7280);
        }
        gfx_circle(c, d, 6 * u, 0x6B7280, 1);
        gfx_circle(c, d, 3 * u, 0xF3F4F6, 1);
        break; }
    case APP_MSG:                                        /* warning */
        for (int j = 0; j < 14 * u; j++) gfx_fill(x + 10 * u - j / 2, y + 3 * u + j, j + 1, 1, 0xF59E0B);
        break;
    default:                                             /* a user program: little window */
        gfx_rfill(x + 2 * u, y + 3 * u, 16 * u, 14 * u, 2 * u, 0x6366F1);
        gfx_fill(x + 3 * u, y + 6 * u, 14 * u, 10 * u, 0xE0E7FF);
        gfx_fill(x + 5 * u, y + 8 * u, 6 * u, u, 0x6366F1);
        gfx_fill(x + 5 * u, y + 11 * u, 9 * u, u, 0x6366F1);
        break;
    case APP_ABOUT:                                      /* info badge */
        gfx_circle(x + 10 * u, y + 10 * u, 8 * u, C_ACCENT, 1);
        gfx_fill(x + 9 * u, y + 9 * u, 2 * u, 6 * u, C_WHITE);
        gfx_fill(x + 9 * u, y + 5 * u, 2 * u, 2 * u, C_WHITE);
        break;
    }
}

/* Desktop icon layout */
#define DESK_BIN (-1)                                          /* the Recycle Bin on the desktop */
static const int desk_icons[] = { APP_SYSINFO, APP_EXPLORER, APP_TERMINAL, APP_NOTEPAD, APP_TASKMGR, APP_CLOCK, APP_ABOUT, DESK_BIN };
#define NICONS 8
/* Files and folders in C:\Desktop are shown on the desktop too, in columns after the system icons */
#define DESK_DIR "/Desktop"
#define DESK_MAX 56
#define DESK_ROWS 8
static struct fs_entry desk_list[DESK_MAX];
static int desk_n;
static uint32_t desk_gen = 0xFFFFFFFF;
static int ci_less(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = *a | (*a >= 'A' && *a <= 'Z' ? 32 : 0), y = *b | (*b >= 'A' && *b <= 'Z' ? 32 : 0);
        if (x != y || !x) return x < y;
    }
}
static void desk_refresh(void) {
    if (desk_gen == fs_generation) return;
    desk_gen = fs_generation;
    int n = fs_mounted() ? fs_list(DESK_DIR, desk_list, DESK_MAX) : 0, k = 0;
    for (int i = 0; i < n; i++) if (desk_list[i].name[0] != '$') desk_list[k++] = desk_list[i];
    for (int i = 1; i < k; i++)                                /* folders first, then by name */
        for (int j = i; j > 0; j--) {
            struct fs_entry *a = &desk_list[j - 1], *b = &desk_list[j];
            int swap = (b->is_dir && !a->is_dir) || (b->is_dir == a->is_dir && ci_less(b->name, a->name));
            if (!swap) break;
            struct fs_entry t = *a; *a = *b; *b = t;
        }
    desk_n = k;
}
static int nicons(void) { desk_refresh(); return NICONS + desk_n; }
static void icon_rect(int i, int *x, int *y) {
    if (i < NICONS) { *x = 16; *y = 10 + i * 88; return; }
    int d = i - NICONS;
    *x = gfx_w - 88 - (d / DESK_ROWS) * 96; *y = 10 + (d % DESK_ROWS) * 88;   /* in columns from the right edge */
}
static void desk_path(int i, char *out) { path_resolve(DESK_DIR, desk_list[i - NICONS].name, out); }

/* ---------- wallpaper (rendered once, then reused) ---------- */
/* Built-in designs, drawn at any size (full screen, or as a thumbnail in Settings). */
static void draw_preset(int kind, int x, int y, int w, int h, int brand) {
    static const uint32_t p[WALLPAPER_PRESETS][5] = {
        /* top,      bottom,   glow 1,   glow 2,   glow 3 */
        { 0x0B1E4A, 0x1679B8, 0x7DD3FC, 0x312E81, 0xA78BFA },   /* Ocean */
        { 0x3B0764, 0xF97316, 0xFDE047, 0xBE185D, 0xFB7185 },   /* Sunset */
        { 0x052E16, 0x15803D, 0xBEF264, 0x064E3B, 0x2DD4BF },   /* Forest */
        { 0x020617, 0x1E293B, 0x6366F1, 0x0F172A, 0x38BDF8 },   /* Midnight */
    };
    const uint32_t *c = p[kind];
    gfx_vgradient(x, y, w, h, c[0], c[1]);
    gfx_glow(x + w * 3 / 4, y + h / 3, h / 2, c[2], 90);
    gfx_glow(x + w / 3, y + h, h / 2, c[3], 120);
    gfx_glow(x + w - w * 6 / 100, y + h - h / 10, h * 29 / 100, c[4], 70);
    if (kind == 3)                                             /* stars */
        for (int i = 0; i < w * h / 4000 + 3; i++) {
            uint32_t r = (uint32_t)i * 2654435761u;
            gfx_pixel(x + (int)(r % (uint32_t)w), y + (int)((r >> 12) % (uint32_t)(h * 2 / 3)), 0xE2E8F0);
        }
    if (brand) {                                               /* faint "MyOS" mark */
        int cx = x + w / 2 + 60, cy = y + h / 2 - 30;
        gfx_glow(cx, cy, 150, 0xFFFFFF, 30);
        gfx_text2x(cx - 32, cy - 16, "MyOS", 0xDCEBFF);
    }
}

/* A .png, .jpg or .bmp picture, scaled to cover the whole screen (cropping the edges). */
static void *img_alloc(uint32_t n) { return kmalloc(n); }
static void img_free(void *p) { kfree(p); }
static int draw_picture(const char *path) {
    uint32_t size;
    if (fs_stat(path, &size) != 1 || size < 16 || size > 32 * 1024 * 1024) return 0;
    uint8_t *f = kmalloc(size);
    if (!f) return 0;
    int ok = 0;
    struct image im;
    if (fs_read(path, f, size) == (int)size && !image_decode(f, size, &im, img_alloc, img_free)) {
        kfree(f); f = 0;
        /* the part of the picture that fills the screen's shape */
        int sw = im.w, sh = im.h, sx = 0, sy = 0;
        if (im.w * gfx_h > im.h * gfx_w) { sw = im.h * gfx_w / gfx_h; sx = (im.w - sw) / 2; }
        else { sh = im.w * gfx_h / gfx_w; sy = (im.h - sh) / 2; }
        uint32_t *scr = kmalloc((uint32_t)gfx_w * gfx_h * 4);
        if (scr) {
            image_resample(&im, sx, sy, sw, sh, scr, gfx_w, gfx_h);
            for (int i = 0; i < gfx_w * gfx_h; i++) {          /* see-through parts show dark blue */
                uint32_t p = scr[i], al = p >> 24;
                if (al != 255) {
                    uint32_t bg = 0x0F172A, o = 0;
                    for (int s = 0; s < 24; s += 8) o |= ((((p >> s) & 255) * al + ((bg >> s) & 255) * (255 - al)) / 255) << s;
                    scr[i] = o;
                }
            }
            for (int y = 0; y < gfx_h; y++) gfx_blit(0, y, gfx_w, 1, scr + y * gfx_w);
            kfree(scr);
            ok = 1;
        }
        kfree(im.px);
    }
    if (f) kfree(f);
    return ok;
}

static int build_wallpaper(void) {
    gfx_noclip();
    if (wallpaper) { kfree(wallpaper); wallpaper = 0; }
    int ok = cfg.wallpaper == WALLPAPER_PRESETS && cfg.picture[0] && draw_picture(cfg.picture);
    if (!ok) draw_preset(cfg.wallpaper < WALLPAPER_PRESETS ? cfg.wallpaper : 0, 0, 0, gfx_w, gfx_h, 1);
    wallpaper = gfx_snapshot();
    return ok;
}

static void kcpy(char *d, const char *s, int max);
/* A program asked for this picture as the wallpaper (Picture viewer). 0 = done. */
int wm_set_wallpaper(const char *path) {
    int old_w = cfg.wallpaper; char old_p[FS_PATH_MAX]; kcpy(old_p, cfg.picture, FS_PATH_MAX);
    cfg.wallpaper = WALLPAPER_PRESETS; kcpy(cfg.picture, path, FS_PATH_MAX);
    if (!build_wallpaper()) {                                   /* not a usable picture: put things back */
        cfg.wallpaper = old_w; kcpy(cfg.picture, old_p, FS_PATH_MAX);
        build_wallpaper();
        dirty = 1;
        return -1;
    }
    settings_save();
    dirty = 1;
    return 0;
}

static void apply_accent(void) {
    int a = cfg.accent >= 0 && cfg.accent < ACCENT_COUNT ? cfg.accent : 0;
    c_accent = accents[a][0]; c_accent2 = accents[a][1];
    c_accent_hi = gfx_blend(c_accent, 0xFFFFFF, 1, 4);
    c_accent_light = gfx_blend(c_accent, 0xFFFFFF, 7, 8);
}

/* ---------- app contents ---------- */
static void draw_terminal(int x, int y, int w, int h) {
    gfx_fill(x, y, w, h, 0x0C0C0C);
    int ox = x + 6, oy = y + 6;
    for (int r = 0; r < CON_ROWS; r++)
        for (int c = 0; c < CON_COLS; c++) {
            struct cell ce = con_grid[r][c];
            if (ce.bg) gfx_fill(ox + c * 8, oy + r * 16, 8, 16, term_palette[ce.bg & 15]);
            if (ce.ch != ' ') gfx_char(ox + c * 8, oy + r * 16, ce.ch, term_palette[ce.fg & 15], 0);
        }
    if (caret_on && focused() == APP_TERMINAL) gfx_fill(ox + con_cx * 8, oy + con_cy * 16 + 13, 8, 2, 0xF2F2F2);
    con_dirty = 0;
}

static void two_digits(char *b, int v) { b[0] = '0' + v / 10 % 10; b[1] = '0' + v % 10; }

static void draw_clock(int x, int y, int w, int h) {
    gfx_vgradient(x, y, w, h, 0xF9FAFB, 0xE5E7EB);
    int hh, mm, ss; rtc_time(&hh, &mm, &ss);
    int cx = x + w / 2, cy = y + 100, r = 82;
    gfx_circle(cx, cy + 3, r + 4, 0xD1D5DB, 1);
    gfx_circle(cx, cy, r + 4, 0x1F2937, 1);
    gfx_circle(cx, cy, r, C_WHITE, 1);
    for (int i = 0; i < 60; i++) {
        int len = i % 5 ? 4 : 10;
        int x0 = cx + sin60[i] * (r - 4) / 1000, y0 = cy - COS60(i) * (r - 4) / 1000;
        int x1 = cx + sin60[i] * (r - 4 - len) / 1000, y1 = cy - COS60(i) * (r - 4 - len) / 1000;
        if (i % 5) gfx_line(x0, y0, x1, y1, 0x9CA3AF); else gfx_thick_line(x0, y0, x1, y1, 3, 0x111827);
    }
    int hi = (hh % 12) * 5 + mm / 12;
    gfx_thick_line(cx, cy, cx + sin60[hi] * 42 / 1000, cy - COS60(hi) * 42 / 1000, 5, 0x111827);
    gfx_thick_line(cx, cy, cx + sin60[mm] * 64 / 1000, cy - COS60(mm) * 64 / 1000, 3, 0x111827);
    gfx_line(cx - sin60[ss] * 14 / 1000, cy + COS60(ss) * 14 / 1000, cx + sin60[ss] * 72 / 1000, cy - COS60(ss) * 72 / 1000, 0xDC2626);
    gfx_circle(cx, cy, 5, 0xDC2626, 1);
    char t[9] = "00:00:00";
    two_digits(t, hh); two_digits(t + 3, mm); two_digits(t + 6, ss);
    gfx_text2x(cx - 64, y + h - 52, t, C_TEXT);
}

static void bar(int x, int y, int w, int h, uint32_t used, uint32_t total, uint32_t col) {
    gfx_rfill(x, y, w, h, 4, 0xE5E7EB);
    int fw = total ? (int)(used * (uint32_t)w / total) : 0;   /* values are KB, so no overflow */
    if (fw > 8) gfx_rfill(x, y, fw, h, 4, col);
}

static char cpu_brand[49];
static void get_cpu_brand(void) {
    uint32_t r[12], a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000));
    if (a < 0x80000004) {
        __asm__ volatile("cpuid" : "=a"(a), "=b"(r[0]), "=c"(r[2]), "=d"(r[1]) : "a"(0));
        for (int i = 0; i < 12; i++) cpu_brand[i] = ((char *)r)[i];
        cpu_brand[12] = 0; return;
    }
    for (int i = 0; i < 3; i++)
        __asm__ volatile("cpuid" : "=a"(r[i * 4]), "=b"(r[i * 4 + 1]), "=c"(r[i * 4 + 2]), "=d"(r[i * 4 + 3]) : "a"(0x80000002 + i));
    char *s = (char *)r; int k = 0;
    while (*s == ' ') s++;
    for (; k < 48 && s[k]; k++) cpu_brand[k] = s[k];
    cpu_brand[k] = 0;
}

static char *utoa(char *b, uint32_t v) { char t[11]; int k = 0; do t[k++] = '0' + v % 10; while (v /= 10); while (k) *b++ = t[--k]; *b = 0; return b; }
static char *cat(char *b, const char *s) { while (*s) *b++ = *s++; *b = 0; return b; }

static void draw_sysinfo(int x, int y, int w, int h) {
    (void)h;
    gfx_fill(x, y, w, h, C_WHITE);
    icon(APP_SYSINFO, x + 16, y + 14, 40);
    gfx_text(x + 70, y + 18, "MyOS 2.3", C_TEXT, 1);
    gfx_text(x + 70, y + 36, "32-bit x86 desktop", C_MUTED, 0);
    char b[80], *p;
    gfx_text(x + 16, y + 70, "Processor", C_MUTED, 0);
    char cb[44]; int k = 0; for (; k < 43 && cpu_brand[k]; k++) cb[k] = cpu_brand[k]; cb[k] = 0;
    gfx_text(x + 16, y + 88, cb, C_TEXT, 0);

    struct mm_stats st; mm_stats(&st);
    gfx_text(x + 16, y + 116, "Memory", C_MUTED, 0);
    p = utoa(b, st.used_kb / 1024); p = cat(p, " MB used of "); p = utoa(p, st.total_kb / 1024); cat(p, " MB");
    gfx_text(x + 16, y + 134, b, C_TEXT, 0);
    bar(x + 16, y + 154, w - 32, 10, st.used_kb, st.total_kb, C_ACCENT);

    gfx_text(x + 16, y + 176, "Kernel heap", C_MUTED, 0);
    p = utoa(b, st.heap_used / 1024); p = cat(p, " KB used of "); p = utoa(p, st.heap_mapped_kb); cat(p, " KB mapped");
    gfx_text(x + 16, y + 194, b, C_TEXT, 0);
    bar(x + 16, y + 214, w - 32, 10, st.heap_used / 1024, st.heap_mapped_kb, 0x16A34A);

    gfx_text(x + 16, y + 236, "Drive C:", C_MUTED, 0);
    if (fs_mounted()) {
        uint32_t used = fs_total_kb() - fs_free_kb();
        p = utoa(b, used / 1024); p = cat(p, " MB used of "); p = utoa(p, fs_total_kb() / 1024); cat(p, " MB (FAT32)");
        gfx_text(x + 16, y + 254, b, C_TEXT, 0);
        bar(x + 16, y + 274, w - 32, 10, used, fs_total_kb(), 0xD97706);
    } else gfx_text(x + 16, y + 254, "No disk attached", C_TEXT, 0);
    gfx_text(x + 16, y + 296, "Network", C_MUTED, 0);
    if (!netcfg.present) gfx_text(x + 90, y + 296, "no network card", C_TEXT, 0);
    else if (!netcfg.ip) gfx_text(x + 90, y + 296, "connecting...", C_TEXT, 0);
    else { char ipb[20]; net_ip_text(netcfg.ip, ipb); p = cat(b, ipb); cat(p, "  (e1000)"); gfx_text(x + 90, y + 296, b, C_TEXT, 0); }

    uint32_t s = ticks / TIMER_HZ;
    p = cat(b, "Screen "); p = utoa(p, gfx_w); p = cat(p, "x"); p = utoa(p, gfx_h);
    p = cat(p, "   Uptime "); p = utoa(p, s / 3600); p = cat(p, "h ");
    p = utoa(p, s / 60 % 60); p = cat(p, "m "); p = utoa(p, s % 60); cat(p, "s");
    gfx_text(x + 16, y + h - 22, b, C_MUTED, 0);
}

static void draw_about(int x, int y, int w, int h) {
    gfx_vgradient(x, y, w, 90, C_ACCENT2, C_ACCENT);
    gfx_text2x(x + 24, y + 20, "MyOS", C_WHITE);
    gfx_text(x + 24, y + 58, "Version 2.3  -  Files on the desktop", 0xDBEAFE, 0);
    gfx_fill(x, y + 90, w, h - 90, C_WHITE);
    gfx_text(x + 24, y + 106, "A Windows-style operating system", C_TEXT, 0);
    gfx_text(x + 24, y + 122, "written from scratch in C and assembly.", C_TEXT, 0);
    gfx_text(x + 24, y + 148, "Kernel: interrupts, timer, paging, heap", C_MUTED, 0);
    gfx_text(x + 24, y + 164, "Desktop: VESA/BGA graphics, PS/2 mouse", C_MUTED, 0);
    gfx_text(x + 24, y + 180, "Storage: ATA disk driver, FAT32 files", C_MUTED, 0);
    gfx_text(x + 24, y + 196, "Programs: preemptive multitasking, ring 3", C_MUTED, 0);
    gfx_text(x + 24, y + 218, "Font: DejaVu Sans Mono (bitmap)", C_MUTED, 0);
}

/* ---------- small UI widgets ---------- */
static void button(int x, int y, int w, const char *label, int hot) {
    gfx_rfill(x, y, w, 24, 4, hot ? C_ACCENT_LIGHT : 0xFFFFFF);
    gfx_rect(x, y, w, 24, hot ? 0x93C5FD : 0xD1D5DB);
    int n = 0; while (label[n]) n++;
    gfx_text(x + (w - n * 8) / 2, y + 4, label, C_TEXT, 0);
}
static void textbox(int x, int y, int w, const char *s, int active) {
    gfx_fill(x, y, w, 24, C_WHITE);
    gfx_rect(x, y, w, 24, active ? C_ACCENT : 0xD1D5DB);
    if (active) gfx_fill(x + 1, y + 22, w - 2, 1, C_ACCENT);
    int n = 0; while (s[n]) n++;
    int maxc = (w - 12) / 8, start = n > maxc ? n - maxc : 0;
    gfx_text(x + 6, y + 4, s + start, C_TEXT, 0);
    if (active && caret_on) gfx_fill(x + 6 + (n - start) * 8, y + 4, 2, 16, C_TEXT);
}
static int kstrlen_(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void kcpy_n(char *d, const char *s, int n) { for (int i = 0; i < n; i++) d[i] = s[i]; }

/* ---------- the system clipboard (text), shared by Notepad, the Terminal and programs ---------- */
static char *clip_buf; static int clip_n;
void clip_set(const char *s, int len) {
    if (len < 0) len = 0;
    if (len > CLIP_MAX) len = CLIP_MAX;
    if (!clip_buf) clip_buf = kmalloc(CLIP_MAX + 1);
    if (!clip_buf) return;
    kcpy_n(clip_buf, s, len); clip_buf[len] = 0; clip_n = len;
}
const char *clip_text(void) { return clip_buf ? clip_buf : ""; }
int clip_len(void) { return clip_n; }

static void kcpy(char *d, const char *s, int max) { int k = 0; for (; s[k] && k < max - 1; k++) d[k] = s[k]; d[k] = 0; }
static const char *base_name(const char *p) { const char *b = p; for (; *p; p++) if (*p == '/') b = p + 1; return *b ? b : "C:"; }

/* Mini folder / document glyphs used in File Explorer */
static void folder_glyph(int x, int y) {
    gfx_rfill(x, y + 2, 8, 4, 1, 0xD4A017);
    gfx_rfill(x, y + 4, 18, 12, 2, 0xF5C542);
}
static void doc_glyph(int x, int y) {
    gfx_fill(x + 3, y, 12, 16, C_WHITE);
    gfx_rect(x + 3, y, 12, 16, 0x9CA3AF);
    for (int i = 0; i < 3; i++) gfx_fill(x + 5, y + 5 + i * 3, 8, 1, 0x9CA3AF);
}

/* ================= Notepad ================= */
#define NOTE_MAX 32768
static char note[NOTE_MAX];
static int note_len, note_modified;
static char note_path[FS_PATH_MAX];       /* "" while untitled */
static char note_field[FS_PATH_MAX];      /* the file-name box (display form, e.g. C:\notes.txt) */
static int note_field_focus;
static char note_status[64];
/* ---- editing: caret, selection, undo ---- */
static int note_caret, note_anchor = -1;  /* selection = anchor..caret when anchor >= 0 */
static int note_top;                      /* first visible row */
static int note_cols = 70, note_rows = 20;/* set by draw_notepad from the window size */
static int note_want_col = -1;            /* column kept while moving up/down */
static int note_dragging;
static uint32_t note_last_click; static int note_clicks;
#define UNDO_LEVELS 6
static char undo_buf[UNDO_LEVELS][NOTE_MAX];
static int undo_len[UNDO_LEVELS], undo_caret[UNDO_LEVELS], undo_n, undo_head;
static int note_typing;                   /* consecutive typed letters share one undo step */


static void note_set_status(const char *s) { kcpy(note_status, s, sizeof note_status); }

static void note_reset_edit(void);
static void note_new(void) {
    note_reset_edit();
    note_len = 0; note_modified = 0; note_path[0] = 0;
    kcpy(note_field, "C:\\untitled.txt", FS_PATH_MAX);
    note_set_status("New document");
}

static int note_load(const char *path) {
    uint32_t size;
    if (fs_stat(path, &size) != 1) { note_set_status("File not found"); return 0; }
    if (size > NOTE_MAX - 1) { note_set_status("File too big for Notepad (32 KB max)"); return 0; }
    int n = fs_read(path, note, size);
    if (n < 0) { note_set_status("Could not read the file"); return 0; }
    int k = 0;
    for (int i = 0; i < n; i++) if (note[i] != '\r') note[k++] = note[i];   /* drop Windows CRs */
    note_len = k; note_modified = 0;
    note_reset_edit(); note_caret = 0;
    if (!fs_canonical(path, note_path)) kcpy(note_path, path, FS_PATH_MAX);
    path_display(note_path, note_field);
    note_set_status("Opened");
    return 1;
}

static void note_save(void) {
    if (!fs_mounted()) { note_set_status("No disk: cannot save"); return; }
    char p[FS_PATH_MAX];
    path_resolve("/", note_field, p);
    if (p[0] == '/' && !p[1]) { note_set_status("Type a file name first"); return; }
    if (fs_write(p, note, note_len)) {
        kcpy(note_path, p, FS_PATH_MAX); path_display(p, note_field);
        note_modified = 0; note_set_status("Saved");
    } else note_set_status("Save failed (bad name, missing folder or disk full)");
}

static void note_open_field(void) {
    char p[FS_PATH_MAX];
    path_resolve("/", note_field, p);
    note_load(p);
}

#define NOTE_TOOL_H 36
static void note_reset_edit(void) { note_caret = note_len; note_anchor = -1; note_top = 0; undo_n = 0; note_typing = 0; note_want_col = -1; }
static int has_sel(void) { return note_anchor >= 0 && note_anchor != note_caret; }
static int sel_lo(void) { return note_anchor < note_caret ? note_anchor : note_caret; }
static int sel_hi(void) { return note_anchor < note_caret ? note_caret : note_anchor; }

static void undo_save(void) {
    kcpy_n(undo_buf[undo_head], note, note_len);
    undo_len[undo_head] = note_len; undo_caret[undo_head] = note_caret;
    undo_head = (undo_head + 1) % UNDO_LEVELS;
    if (undo_n < UNDO_LEVELS) undo_n++;
}
static void note_undo(void) {
    if (!undo_n) { note_set_status("Nothing to undo"); return; }
    undo_head = (undo_head + UNDO_LEVELS - 1) % UNDO_LEVELS; undo_n--;
    note_len = undo_len[undo_head]; kcpy_n(note, undo_buf[undo_head], note_len);
    note_caret = undo_caret[undo_head] <= note_len ? undo_caret[undo_head] : note_len;
    note_anchor = -1; note_modified = 1; note_typing = 0;
    note_set_status("Undone");
}

/* Row/column of the caret placed before character p (same wrapping as drawing). */
static void note_rc(int p, int *row, int *col) {
    int cx = 0, cy = 0, cols = note_cols;
    for (int i = 0; i < p; i++) {
        if (note[i] == '\n') { cx = 0; cy++; continue; }
        if (cx >= cols) { cx = 0; cy++; }
        cx++;
    }
    if (cx >= cols && !(p < note_len && note[p] == '\n') && p < note_len) { cx = 0; cy++; }
    *row = cy; *col = cx;
}
/* Character position nearest to (row, col). */
static int note_pos_at(int row, int col) {
    if (row < 0) return 0;
    int cx = 0, cy = 0, cols = note_cols, best = -1;
    for (int i = 0; i <= note_len; i++) {
        int r = cy, c = cx;
        if (cx >= cols && i < note_len && note[i] != '\n') { r = cy + 1; c = 0; }
        if (r == row && c <= col) best = i;
        if (r > row) break;
        if (i == note_len) break;
        if (note[i] == '\n') { cx = 0; cy++; continue; }
        if (cx >= cols) { cx = 0; cy++; }
        cx++;
    }
    return best >= 0 ? best : note_len;
}

static void note_delete_range(int a, int b) {
    for (int i = b; i < note_len; i++) note[i - (b - a)] = note[i];
    note_len -= b - a; note_caret = a; note_anchor = -1; note_modified = 1;
}
static int note_delete_sel(void) {
    if (!has_sel()) { note_anchor = -1; return 0; }
    note_delete_range(sel_lo(), sel_hi());
    return 1;
}
static void note_insert(const char *s, int n) {
    if (note_len + n > NOTE_MAX - 1) { n = NOTE_MAX - 1 - note_len; note_set_status("Notepad is full (32 KB max)"); }
    if (n <= 0) return;
    for (int i = note_len - 1; i >= note_caret; i--) note[i + n] = note[i];
    for (int i = 0; i < n; i++) note[note_caret + i] = s[i] == '\r' ? ' ' : s[i];
    note_len += n; note_caret += n; note_modified = 1;
}
static void note_copy(void) {
    if (!has_sel()) { note_set_status("Select some text first"); return; }
    clip_set(note + sel_lo(), sel_hi() - sel_lo());
    note_set_status("Copied");
}
static void note_paste(void) {
    int n = clip_len();
    if (!n) { note_set_status("The clipboard is empty"); return; }
    undo_save(); note_typing = 0;
    note_delete_sel();
    const char *c = clip_text();
    int k = 0;                                       /* strip Windows-style CRs */
    static char tmp[NOTE_MAX];
    for (int i = 0; i < n && k < NOTE_MAX; i++) if (c[i] != '\r') tmp[k++] = c[i];
    note_insert(tmp, k);
    note_set_status("Pasted");
}
static int is_word(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

/* ---- Find / Replace bar ---- */
#define FIND_ROW 34
static int find_open, find_replace, find_focus, find_case;   /* find_focus: 1 = find box, 2 = replace box */
static char find_text[64], repl_text[64];
static int note_text_top(void) { return NOTE_TOOL_H + 1 + (find_open ? (find_replace ? 2 : 1) * FIND_ROW : 0); }

static char lower_c(char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int match_at(int i) {
    int n = kstrlen_(find_text);
    if (!n || i < 0 || i + n > note_len) return 0;
    for (int k = 0; k < n; k++) {
        char a = note[i + k], b = find_text[k];
        if (!find_case) { a = lower_c(a); b = lower_c(b); }
        if (a != b) return 0;
    }
    return 1;
}
static void find_report(int at, int wrapped) {          /* "Match 3 of 12" */
    int total = 0, index = 0, n = kstrlen_(find_text);
    for (int i = 0; i + n <= note_len; i++) if (match_at(i)) { total++; if (i <= at) index = total; i += n - 1; }
    char m[64], *q = cat(m, "Match "); q = utoa(q, index); q = cat(q, " of "); q = utoa(q, total);
    if (wrapped) cat(q, " (wrapped)");
    note_set_status(m);
}
static int note_find_from(int start, int dir) {        /* select the next match; 1 if found */
    if (!find_text[0]) { note_set_status("Type something to find"); return 0; }
    int span = note_len + 1;
    for (int k = 0; k < span; k++) {
        int i = ((start + dir * k) % span + span) % span;
        if (match_at(i)) {
            note_anchor = i; note_caret = i + kstrlen_(find_text); note_typing = 0; note_want_col = -1;
            find_report(i, dir > 0 ? i < start : i > start);
            return 1;
        }
    }
    char m[64], t[24]; kcpy(t, find_text, 20);
    char *q = cat(m, "Can't find \""); q = cat(q, t); cat(q, "\"");
    note_set_status(m);
    return 0;
}
static void note_find(int dir) {
    int start = dir > 0 ? (has_sel() ? sel_lo() + 1 : note_caret) : (has_sel() ? sel_lo() - 1 : note_caret - 1);
    note_find_from(start, dir);
}
static int sel_is_match(void) { return has_sel() && sel_hi() - sel_lo() == kstrlen_(find_text) && match_at(sel_lo()); }
static void note_replace_one(void) {
    if (sel_is_match()) {
        undo_save(); note_typing = 0;
        note_delete_sel();
        note_insert(repl_text, kstrlen_(repl_text));
        note_find_from(note_caret, 1);
    } else note_find(1);
}
static void note_replace_all(void) {
    if (!find_text[0]) { note_set_status("Type something to find"); return; }
    int n = kstrlen_(find_text), r = kstrlen_(repl_text), count = 0;
    undo_save(); note_typing = 0;
    for (int i = 0; i + n <= note_len; ) {
        if (match_at(i)) {
            note_delete_range(i, i + n);
            note_caret = i; note_insert(repl_text, r);
            i += r; count++;
        } else i++;
    }
    if (!count) { undo_n--; undo_head = (undo_head + UNDO_LEVELS - 1) % UNDO_LEVELS; }
    note_anchor = -1;
    char m[64], *q = cat(m, "Replaced "); q = utoa(q, count); cat(q, count == 1 ? " match" : " matches");
    note_set_status(m);
}
static void find_open_bar(int replace) {
    find_open = 1; find_replace = replace || find_replace; find_focus = 1; note_field_focus = 0;
    if (has_sel() && sel_hi() - sel_lo() < 63) {                    /* start with the selected text */
        int ok = 1; for (int i = sel_lo(); i < sel_hi(); i++) if (note[i] == '\n') ok = 0;
        if (ok) { kcpy_n(find_text, note + sel_lo(), sel_hi() - sel_lo()); find_text[sel_hi() - sel_lo()] = 0; }
    }
}
static void draw_find_bar(int x, int y, int w) {
    int hot = hover_key >= 1010 && hover_key < 1020 ? hover_key - 1010 : -1;
    int active = focused() == APP_NOTEPAD;
    int rows = find_replace ? 2 : 1, tw = w - 72 - 300;
    gfx_fill(x, y, w, rows * FIND_ROW, 0xF9FAFB);
    gfx_text(x + 10, y + 9, "Find", C_MUTED, 0);
    textbox(x + 72, y + 5, tw, find_text, active && find_focus == 1);
    button(x + w - 292, y + 5, 56, "Next", hot == 0);
    button(x + w - 232, y + 5, 56, "Prev", hot == 1);
    int bx = x + w - 168;                                           /* "Match case" check box */
    gfx_rfill(bx, y + 10, 14, 14, 3, find_case ? C_ACCENT : 0x9CA3AF);
    if (!find_case) gfx_rfill(bx + 1, y + 11, 12, 12, 2, C_WHITE);
    else { gfx_line(bx + 3, y + 17, bx + 6, y + 20, C_WHITE); gfx_line(bx + 6, y + 20, bx + 11, y + 13, C_WHITE); }
    gfx_text(bx + 20, y + 9, "Match case", hot == 2 ? C_ACCENT : C_TEXT, 0);
    gfx_rfill(x + w - 30, y + 7, 22, 20, 4, hot == 3 ? 0xFEE2E2 : 0xF9FAFB);
    gfx_line(x + w - 24, y + 12, x + w - 15, y + 21, hot == 3 ? 0xDC2626 : C_MUTED);
    gfx_line(x + w - 15, y + 12, x + w - 24, y + 21, hot == 3 ? 0xDC2626 : C_MUTED);
    if (find_replace) {
        gfx_text(x + 10, y + FIND_ROW + 9, "Replace", C_MUTED, 0);
        textbox(x + 72, y + FIND_ROW + 5, tw, repl_text, active && find_focus == 2);
        button(x + w - 292, y + FIND_ROW + 5, 76, "Replace", hot == 4);
        button(x + w - 210, y + FIND_ROW + 5, 104, "Replace all", hot == 5);
    }
    gfx_fill(x, y + rows * FIND_ROW - 1, w, 1, 0xE5E7EB);
}
static int find_bar_hit(int lx, int ly, int w) {                    /* hover code for the find bar */
    int top = NOTE_TOOL_H + 1, row = (ly - top) / FIND_ROW, ry = ly - top - row * FIND_ROW;
    if (!find_open || ly < top || row >= (find_replace ? 2 : 1) || ry < 5 || ry >= 29) return -1;
    if (row == 0) {
        if (lx >= w - 292 && lx < w - 236) return 1010;
        if (lx >= w - 232 && lx < w - 176) return 1011;
        if (lx >= w - 168 && lx < w - 70) return 1012;
        if (lx >= w - 30 && lx < w - 8) return 1013;
        if (lx >= 72 && lx < w - 300) return 1016;
    } else {
        if (lx >= w - 292 && lx < w - 216) return 1014;
        if (lx >= w - 210 && lx < w - 106) return 1015;
        if (lx >= 72 && lx < w - 300) return 1017;
    }
    return -1;
}

static void draw_notepad(int x, int y, int w, int h) {
    int hot = hover_key >= 1000 && hover_key < 1010 ? hover_key - 1000 : -1;
    gfx_fill(x, y, w, NOTE_TOOL_H, 0xF3F4F6);
    button(x + 6, y + 6, 52, "New", hot == 0);
    button(x + 62, y + 6, 56, "Open", hot == 1);
    button(x + 122, y + 6, 56, "Save", hot == 2);
    textbox(x + 186, y + 6, w - 194, note_field, note_field_focus && focused() == APP_NOTEPAD);
    gfx_fill(x, y + NOTE_TOOL_H, w, 1, 0xD1D5DB);
    if (find_open) draw_find_bar(x, y + NOTE_TOOL_H + 1, w);
    int ty = y + note_text_top(), th = h - note_text_top() - 22;
    gfx_fill(x, ty, w, th, C_WHITE);
    note_cols = (w - 16) / 8; note_rows = (th - 8) / 16;
    if (note_caret > note_len) note_caret = note_len;

    int crow, ccol; note_rc(note_caret, &crow, &ccol);      /* keep the caret on screen */
    if (crow < note_top) note_top = crow;
    if (crow >= note_top + note_rows) note_top = crow - note_rows + 1;
    int lo = has_sel() ? sel_lo() : -1, hi = has_sel() ? sel_hi() : -1;
    int cx = 0, cy = 0, first = note_top;
    int active = focused() == APP_NOTEPAD && !note_field_focus && !find_focus;
    int hl_until = -1, fn = find_open ? kstrlen_(find_text) : 0;   /* every match gets a yellow mark */
    gfx_clip(x, ty, w, th);
    for (int i = 0; i < note_len && cy < first + note_rows; i++) {
        char ch = note[i];
        int in_sel = i >= lo && i < hi;
        if (ch == '\n') {
            if (in_sel && cy >= first) gfx_fill(x + 8 + cx * 8, ty + 4 + (cy - first) * 16, 6, 16, active ? C_ACCENT_LIGHT : 0xE5E7EB);
            cx = 0; cy++; continue;
        }
        if (cx >= note_cols) { cx = 0; cy++; }
        if (fn && i >= hl_until && cy >= first - 1 && match_at(i)) hl_until = i + fn;
        if (cy >= first && cy < first + note_rows) {
            int px = x + 8 + cx * 8, py = ty + 4 + (cy - first) * 16;
            if (in_sel) gfx_fill(px, py, 8, 16, active || find_focus ? C_ACCENT_LIGHT : 0xE5E7EB);
            else if (i < hl_until) gfx_fill(px, py, 8, 16, 0xFEF08A);
            gfx_char(px, py, ch == '\t' ? ' ' : ch, C_TEXT, 0);
        }
        cx++;
    }
    if (caret_on && active && crow >= first && crow < first + note_rows)
        gfx_fill(x + 8 + ccol * 8, ty + 4 + (crow - first) * 16, 2, 16, C_TEXT);
    gfx_clip(x, y, w, h);

    gfx_fill(x, y + h - 22, w, 22, 0xF3F4F6);
    /* line / column of the caret, counted in real lines (not wrapped rows) */
    int line = 1, col = 1;
    for (int i = 0; i < note_caret; i++) { if (note[i] == '\n') { line++; col = 1; } else col++; }
    char st[128], *p = cat(st, "  ");
    p = cat(p, note_status); p = cat(p, "   |   Ln "); p = utoa(p, line); p = cat(p, ", Col "); p = utoa(p, col);
    p = cat(p, "   |   ");
    if (has_sel()) { p = utoa(p, hi - lo); cat(p, " selected"); }
    else { p = utoa(p, note_len); cat(p, " characters"); }
    gfx_text(x + 4, y + h - 19, st, C_MUTED, 0);
    if (kstrlen_(st) * 8 + 16 < w - 104) gfx_text(x + w - 104, y + h - 19, "Ctrl+S save", C_MUTED, 0);
}

static int note_pos_from_xy(int lx, int ly) {
    int tt = note_text_top();
    int row = note_top + (ly - tt - 4) / 16, col = (lx - 8 + 4) / 8;
    if (ly < tt + 4) row = note_top - (ly < tt ? 1 : 0);
    if (col < 0) col = 0;
    return note_pos_at(row, col);
}

static void notepad_click(int lx, int ly, int w) {
    note_field_focus = 0;
    if (find_open && ly > NOTE_TOOL_H && ly < note_text_top()) {
        switch (find_bar_hit(lx, ly, w)) {
        case 1010: find_focus = 0; note_find(1); break;
        case 1011: find_focus = 0; note_find(-1); break;
        case 1012: find_case = !find_case; break;
        case 1013: find_open = 0; find_focus = 0; break;
        case 1014: note_replace_one(); break;
        case 1015: note_replace_all(); break;
        case 1016: find_focus = 1; break;
        case 1017: find_focus = 2; break;
        }
        return;
    }
    find_focus = 0;
    if (ly < NOTE_TOOL_H) {
        if (ly >= 6 && ly < 30) {
            if (lx >= 6 && lx < 58) { note_new(); return; }
            if (lx >= 62 && lx < 118) { note_open_field(); return; }
            if (lx >= 122 && lx < 178) { note_save(); return; }
            if (lx >= 186) { note_field_focus = 1; return; }
        }
        return;
    }
    int p = note_pos_from_xy(lx, ly);
    note_typing = 0; note_want_col = -1;
    if (ticks - note_last_click < TIMER_HZ / 2 && note_clicks && p == note_caret) note_clicks++; else note_clicks = 1;
    note_last_click = ticks;
    if (note_clicks == 2) {                         /* double-click selects a word */
        int a = p, b = p;
        while (a > 0 && is_word(note[a - 1])) a--;
        while (b < note_len && is_word(note[b])) b++;
        if (b > a) { note_anchor = a; note_caret = b; }
        note_dragging = 0;
        return;
    }
    if (note_clicks >= 3) {                         /* triple-click selects the line */
        int a = p, b = p;
        while (a > 0 && note[a - 1] != '\n') a--;
        while (b < note_len && note[b] != '\n') b++;
        if (b < note_len) b++;
        note_anchor = a; note_caret = b; note_dragging = 0; note_clicks = 0;
        return;
    }
    if (kbd_shift_down()) { if (note_anchor < 0) note_anchor = note_caret; }
    else note_anchor = p;
    note_caret = p;
    note_dragging = 1;
}
static void notepad_drag(int lx, int ly) {           /* mouse moved with the button held */
    int p = note_pos_from_xy(lx, ly);
    if (p != note_caret) { note_caret = p; note_clicks = 0; dirty = 1; }
}

static int notepad_hover(int lx, int ly) {
    if (find_open && ly > NOTE_TOOL_H && ly < note_text_top()) { int h = find_bar_hit(lx, ly, wins[APP_NOTEPAD].w - 2); return h >= 1016 ? -1 : h; }
    if (ly >= 6 && ly < 30) {
        if (lx >= 6 && lx < 58) return 1000;
        if (lx >= 62 && lx < 118) return 1001;
        if (lx >= 122 && lx < 178) return 1002;
    }
    return -1;
}

static void note_move(int p, int keep_col) {          /* move the caret; Shift extends the selection */
    if (kbd_shift_down()) { if (note_anchor < 0) note_anchor = note_caret; }
    else note_anchor = -1;
    if (p < 0) p = 0;
    if (p > note_len) p = note_len;
    note_caret = p; note_typing = 0;
    if (!keep_col) note_want_col = -1;
}

static void notepad_key(int k) {
    if (k == KEY_CTRL('s')) { note_save(); return; }
    if (k == KEY_CTRL('n')) { note_new(); return; }
    if (k == KEY_CTRL('o')) { note_field_focus = 1; find_focus = 0; return; }
    if (k == KEY_CTRL('f')) { find_open_bar(0); return; }
    if (k == KEY_CTRL('h')) { find_open_bar(1); find_replace = 1; return; }
    if (k == KEY_F3) { if (!find_open) find_open_bar(0); note_find(kbd_shift_down() ? -1 : 1); return; }
    if (find_focus) {
        char *f = find_focus == 1 ? find_text : repl_text;
        int n = kstrlen_(f), changed = 0;
        if (k == '\n') { if (find_focus == 1) note_find(kbd_shift_down() ? -1 : 1); else note_replace_one(); }
        else if (k == KEY_ESC) { find_open = 0; find_focus = 0; }
        else if (k == '\t') { if (find_replace) find_focus = 3 - find_focus; }
        else if (k == '\b') { if (n) { f[n - 1] = 0; changed = 1; } }
        else if (k == KEY_CTRL('v')) {
            const char *c = clip_text();
            for (int i = 0; i < clip_len() && n < 63 && c[i] >= 32 && c[i] < 127; i++) f[n++] = c[i];
            f[n] = 0; changed = 1;
        }
        else if (k >= 32 && k < 127 && n < 63) { f[n] = (char)k; f[n + 1] = 0; changed = 1; }
        if (changed && find_focus == 1 && find_text[0])             /* search as you type */
            note_find_from(has_sel() ? sel_lo() : note_caret, 1);
        return;
    }
    if (note_field_focus) {
        int n = 0; while (note_field[n]) n++;
        if (k == '\n') { note_field_focus = 0; note_open_field(); }
        else if (k == KEY_ESC) note_field_focus = 0;
        else if (k == '\b') { if (n) note_field[n - 1] = 0; }
        else if (k == KEY_CTRL('v')) {
            const char *c = clip_text();
            for (int i = 0; i < clip_len() && n < FS_PATH_MAX - 1 && c[i] >= 32 && c[i] < 127; i++) note_field[n++] = c[i];
            note_field[n] = 0;
        }
        else if (k >= 32 && k < 127 && n < FS_PATH_MAX - 1) { note_field[n] = (char)k; note_field[n + 1] = 0; }
        return;
    }
    int row, col; note_rc(note_caret, &row, &col);
    if (note_want_col < 0) note_want_col = col;
    switch (k) {
    case KEY_CTRL('a'): note_anchor = 0; note_caret = note_len; return;
    case KEY_CTRL('c'): note_copy(); return;
    case KEY_CTRL('x'): if (has_sel()) { note_copy(); undo_save(); note_typing = 0; note_delete_sel(); note_set_status("Cut"); } else note_set_status("Select some text first"); return;
    case KEY_CTRL('v'): note_paste(); return;
    case KEY_CTRL('z'): note_undo(); return;
    case KEY_LEFT:  if (has_sel() && !kbd_shift_down()) note_move(sel_lo(), 0); else note_move(note_caret - 1, 0); return;
    case KEY_RIGHT: if (has_sel() && !kbd_shift_down()) note_move(sel_hi(), 0); else note_move(note_caret + 1, 0); return;
    case KEY_UP:    if (row > 0) note_move(note_pos_at(row - 1, note_want_col), 1); else note_move(0, 0); return;
    case KEY_DOWN:  note_move(note_pos_at(row + 1, note_want_col), 1); return;
    case KEY_PGUP:  note_move(note_pos_at(row - note_rows + 1 < 0 ? 0 : row - note_rows + 1, note_want_col), 1); return;
    case KEY_PGDN:  note_move(note_pos_at(row + note_rows - 1, note_want_col), 1); return;
    case KEY_HOME:  note_move(note_pos_at(row, 0), 0); return;
    case KEY_END:   note_move(note_pos_at(row, 1 << 20), 0); return;
    case KEY_ESC:   note_anchor = -1; return;
    }
    note_want_col = -1;
    if (k == '\b' || k == KEY_DEL) {
        undo_save(); note_typing = 0;
        if (note_delete_sel()) return;
        if (k == '\b' && note_caret > 0) note_delete_range(note_caret - 1, note_caret);
        else if (k == KEY_DEL && note_caret < note_len) note_delete_range(note_caret, note_caret + 1);
        else undo_n--, undo_head = (undo_head + UNDO_LEVELS - 1) % UNDO_LEVELS;   /* nothing changed */
        return;
    }
    if (k == '\t' || k == '\n' || (k >= 32 && k < 127)) {
        int typed = k >= 32;                                 /* " word" typed = one undo step */
        int new_word = k == ' ' && note_caret > 0 && note[note_caret - 1] != ' ';
        if (!note_typing || !typed || new_word || has_sel()) undo_save();
        note_typing = typed;
        note_delete_sel();
        if (k == '\t') note_insert("    ", 4);
        else { char c = (char)k; note_insert(&c, 1); }
    }
}

/* ================= File Explorer ================= */
#define EX_MAX 256
#define EX_ROW 24
#define EX_TOP 102                         /* list starts below toolbar + address bar + header */
static struct fs_entry ex_list[EX_MAX];
static int ex_n, ex_sel = -1, ex_scroll;
static int ex_iscroll;                         /* Icons view: first tile row shown */
static char ex_path[FS_PATH_MAX] = "/";
static char ex_status[96];
static uint32_t ex_gen = 0xFFFFFFFF;
static uint32_t ex_last_click;
static int ex_last_row = -1;
static int ex_renaming;                    /* 1 while the selected name is being edited */
static char ex_rename_buf[FS_NAME_MAX];
#define EX_CLIPS 128                       /* most items one Copy / Cut / drag can carry */
static char ex_clip[EX_CLIPS][FS_PATH_MAX];  /* clipboard: files and folders */
static int ex_nclip;
static int ex_clip_cut;                    /* 1 = Cut (move on paste), 0 = Copy */
/* Selection: ex_sel is the focused item; ex_mark[] adds more (Ctrl-click, Shift-click, Ctrl+A, box) */
static unsigned char ex_mark[EX_MAX];
static int ex_anchor = -1;                 /* where a Shift range starts */
static int ex_collapse = -1;               /* pressed on a group: select just this one if no drag follows */
static int band_on, band_x0, band_y0, band_x1, band_y1, band_add;   /* the selection box (window coords) */
static unsigned char band_base[EX_MAX];    /* what was selected before a Ctrl+box */
static int ex_confirm;                     /* 1 while "delete folder and contents?" is shown */
static int ex_confirm_count;

/* ---------- the Recycle Bin: C:\$Recycle holds R1, R2, ... and R1.inf (the original path) ---------- */
#define BIN "/$Recycle"
static int dd_item = -1, dd_on, dd_px, dd_py;                  /* item pressed, dragging?, where pressed */
static char dd_paths[EX_CLIPS][FS_PATH_MAX];                   /* what is being dragged */
static int dd_n, dd_desk;                                      /* dd_desk: dragged from the desktop */
static int ex_in_bin;                          /* Explorer is showing the Recycle Bin */
static char ex_disp[EX_MAX][FS_NAME_MAX];      /* in the bin: the original names ... */
static char ex_from[EX_MAX][FS_PATH_MAX];      /* ... and where they came from */
static const char *ex_name(int i) { return ex_in_bin ? ex_disp[i] : ex_list[i].name; }
static int same_ci(const char *a, const char *b) {
    while (*a && *b) { char x = *a, y = *b; if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; if (x != y) return 0; a++; b++; }
    return !*a && !*b;
}
static int read_small(const char *path, char *out, int max) {  /* a little text file -> string */
    uint32_t size;
    if (fs_stat(path, &size) != 1 || size >= (uint32_t)max) { out[0] = 0; return 0; }
    int n = fs_read(path, out, size);
    out[n > 0 ? n : 0] = 0;
    return n > 0;
}
static void ex_say(const char *a, const char *b);
static int ex_is_sel(int i) { return i >= 0 && i < ex_n && (i == ex_sel || ex_mark[i]); }
static int ex_nsel(void) { int n = 0; for (int i = 0; i < ex_n; i++) n += ex_is_sel(i); return n; }
static void ex_unmark(void) { for (int i = 0; i < EX_MAX; i++) ex_mark[i] = 0; }
static void ex_only(int i) { ex_unmark(); ex_sel = i; ex_anchor = i; }   /* a plain, single selection */
static void ex_range(int a, int b) {                           /* Shift: everything from a to b */
    if (a < 0) a = b;
    ex_unmark();
    for (int i = a < b ? a : b; i <= (a < b ? b : a); i++) if (i >= 0 && i < ex_n) ex_mark[i] = 1;
    ex_sel = b;
}
static int ex_selected(int *out) {                             /* the selected indices, in order */
    int n = 0; for (int i = 0; i < ex_n; i++) if (ex_is_sel(i)) out[n++] = i;
    return n;
}
static void ex_refresh(void) {
    /* remember the selection by name: the list may come back in another order */
    static char keep[EX_MAX][FS_NAME_MAX]; int nkeep = 0;
    char focus[FS_NAME_MAX]; focus[0] = 0;
    for (int i = 0; i < ex_n; i++) if (ex_mark[i] && i != ex_sel) kcpy(keep[nkeep++], ex_list[i].name, FS_NAME_MAX);
    if (ex_sel >= 0 && ex_sel < ex_n) kcpy(focus, ex_list[ex_sel].name, FS_NAME_MAX);
    ex_unmark();
    ex_n = fs_mounted() ? fs_list(ex_path, ex_list, EX_MAX) : 0;
    if (ex_n < 0) { kcpy(ex_path, "/", FS_PATH_MAX); ex_n = fs_list(ex_path, ex_list, EX_MAX); if (ex_n < 0) ex_n = 0; }
    ex_in_bin = same_ci(ex_path, BIN);
    int k = 0;
    for (int i = 0; i < ex_n; i++) {
        struct fs_entry *e = &ex_list[i];
        int n = kstrlen_(e->name);
        if (!ex_in_bin && e->name[0] == '$') continue;                       /* hidden, like Windows */
        if (ex_in_bin && n >= 4 && same_ci(e->name + n - 4, ".inf")) continue;
        if (k != i) ex_list[k] = *e;
        if (ex_in_bin) {                                                      /* name + origin from the .inf note */
            char inf[FS_PATH_MAX], *q = cat(inf, BIN "/"); q = cat(q, e->name); cat(q, ".inf");
            if (!read_small(inf, ex_from[k], FS_PATH_MAX)) cat(ex_from[k], "/unknown");
            kcpy(ex_disp[k], base_name(ex_from[k]), FS_NAME_MAX);
        }
        k++;
    }
    ex_n = k;
    if (ex_sel >= ex_n) ex_sel = ex_n - 1;
    for (int j = 0; j < nkeep; j++)
        for (int i = 0; i < ex_n; i++) if (!kstrcmp(ex_list[i].name, keep[j])) { ex_mark[i] = 1; break; }
    if (focus[0]) for (int i = 0; i < ex_n; i++) if (!kstrcmp(ex_list[i].name, focus)) { ex_sel = i; break; }
    if (ex_anchor >= ex_n) ex_anchor = ex_sel;
    ex_gen = fs_generation;
}
static void ex_select_name(const char *name) {
    for (int i = 0; i < ex_n; i++) if (!kstrcmp(ex_name(i), name)) ex_only(i);
}
static void ex_add_name(const char *name) {                     /* add to the selection */
    for (int i = 0; i < ex_n; i++) if (!kstrcmp(ex_name(i), name)) { ex_mark[i] = 1; if (ex_sel < 0) ex_sel = ex_anchor = i; }
}
/* the clipboard */
static int in_clip(const char *p) { for (int i = 0; i < ex_nclip; i++) if (!kstrcmp(ex_clip[i], p)) return 1; return 0; }
static void clip_forget(const char *p) {
    for (int i = 0; i < ex_nclip; i++) if (!kstrcmp(ex_clip[i], p)) { kcpy(ex_clip[i], ex_clip[--ex_nclip], FS_PATH_MAX); i--; }
}
static void say_count(const char *a, int n, const char *b) {   /* "Moved 3 items to Box" */
    char m[96], *q = cat(m, a); q = utoa(q, n); q = cat(q, n == 1 ? " item" : " items"); if (b) cat(q, b);
    ex_status[0] = 0; ex_say(m, 0);
}
static void ex_go(const char *path) {
    if (same_ci(path, BIN) && fs_stat(BIN, 0) != 2) fs_mkdir(BIN);   /* an empty bin may not exist yet */
    if (!fs_canonical(path, ex_path)) kcpy(ex_path, path, FS_PATH_MAX);
    ex_sel = -1; ex_scroll = 0; ex_iscroll = 0; ex_status[0] = 0; ex_renaming = 0; ex_confirm = 0;
    ex_n = 0; ex_unmark(); ex_anchor = -1;
    ex_refresh();
}
static void ex_child(int i, char *out) { path_resolve(ex_path, ex_list[i].name, out); }
static void ex_say(const char *a, const char *b) { char *q = cat(ex_status, a); if (b) { char t[48]; kcpy(t, b, 48); cat(q, t); } }

static void run_program(const char *path);
static int is_elf(const char *n) {
    int k = 0; while (n[k]) k++;
    return k > 4 && n[k - 4] == '.' && (n[k - 3] | 32) == 'e' && (n[k - 2] | 32) == 'l' && (n[k - 1] | 32) == 'f';
}
static int has_ext(const char *n, const char *ext) {      /* case-insensitive ".bmp" check */
    int k = 0, e = 0; while (n[k]) k++; while (ext[e]) e++;
    if (k <= e) return 0;
    for (int i = 0; i < e; i++) if ((n[k - e + i] | 32) != ext[i]) return 0;
    return n[k - e - 1] == '.';
}
static void open_with(const char *app, const char *file) {
    int r = proc_spawn_arg(app, file);
    if (r < 0) wm_proc_crashed(base_name(app), "Could not start the program for this file.");
}
/* open a file the way its type wants (folders open in File Explorer) */
static void open_path(const char *p, int is_dir) {
    const char *name = base_name(p);
    if (is_dir) { open_app(APP_EXPLORER); ex_go(p); }
    else if (is_elf(name)) run_program(p);                   /* programs run */
    else if (image_is_picture_name(name)) open_with("/Apps/viewer.elf", p);
    else if (has_ext(name, "wav")) open_with("/Apps/music.elf", p);
    else wm_open_file(p);                                    /* everything else opens in Notepad */
}
static void ex_open(int i) {
    if (i < 0 || i >= ex_n) return;
    if (ex_in_bin) { ex_say("Restore it first to open it", 0); return; }
    char p[FS_PATH_MAX]; ex_child(i, p);
    if (ex_list[i].is_dir) ex_go(p);
    else open_path(p, 0);
}
static void ex_up(void) { char p[FS_PATH_MAX]; path_resolve(ex_path, "..", p); ex_go(p); }

static void ex_new(int folder) {
    if (!fs_mounted()) return;
    char name[FS_NAME_MAX], p[FS_PATH_MAX];
    const char *base = folder ? "New folder" : "New text file";
    for (int n = 1; n < 50; n++) {
        char *q = cat(name, base);
        if (n > 1) { q = cat(q, " ("); q = utoa(q, n); q = cat(q, ")"); }
        if (!folder) cat(q, ".txt");
        path_resolve(ex_path, name, p);
        if (fs_stat(p, 0)) continue;
        int ok = folder ? fs_mkdir(p) : fs_write(p, "", 0);
        ex_say(ok ? "Created " : "Could not create ", name);
        ex_refresh();
        ex_select_name(name);
        if (ok) { ex_renaming = 1; kcpy(ex_rename_buf, name, FS_NAME_MAX); }   /* type a name straight away, like Windows */
        return;
    }
}

/* ----- whole-folder helpers (copy, delete, count), depth-limited ----- */
static int tree_count(const char *path, int depth) {
    if (depth > 16) return 0;
    struct fs_entry *l = kmalloc(sizeof(struct fs_entry) * EX_MAX);
    if (!l) return 0;
    int n = fs_list(path, l, EX_MAX), total = 0;
    for (int i = 0; i < n; i++) {
        total++;
        if (l[i].is_dir) { char c[FS_PATH_MAX]; path_resolve(path, l[i].name, c); total += tree_count(c, depth + 1); }
    }
    kfree(l);
    return total;
}
static int tree_delete(const char *path, int depth) {        /* 1 = everything deleted */
    if (depth > 16) return 0;
    if (fs_delete(path) == 1) return 1;                      /* a file, or an empty folder */
    struct fs_entry *l = kmalloc(sizeof(struct fs_entry) * EX_MAX);
    if (!l) return 0;
    int n = fs_list(path, l, EX_MAX), ok = n >= 0;
    for (int i = 0; i < n && ok; i++) { char c[FS_PATH_MAX]; path_resolve(path, l[i].name, c); ok = tree_delete(c, depth + 1); }
    kfree(l);
    return ok && fs_delete(path) == 1;
}
static int copy_file(const char *src, const char *dst) {
    uint32_t size = 0;
    if (fs_stat(src, &size) != 1) return 0;
    char *buf = kmalloc(size ? size : 1);
    if (!buf) return 0;
    int got = fs_read(src, buf, size);
    int ok = got == (int)size && fs_write(dst, buf, size);
    kfree(buf);
    return ok;
}
static int tree_copy(const char *src, const char *dst, int depth) {
    if (depth > 16) return 0;
    if (fs_stat(src, 0) != 2) return copy_file(src, dst);
    if (!fs_mkdir(dst)) return 0;
    struct fs_entry *l = kmalloc(sizeof(struct fs_entry) * EX_MAX);
    if (!l) return 0;
    int n = fs_list(src, l, EX_MAX), ok = n >= 0;
    for (int i = 0; i < n && ok; i++) {
        char s[FS_PATH_MAX], d[FS_PATH_MAX];
        path_resolve(src, l[i].name, s); path_resolve(dst, l[i].name, d);
        ok = tree_copy(s, d, depth + 1);
    }
    kfree(l);
    return ok;
}

static void copy_name(const char *name, int n, int is_dir, char *out);
/* ----- the Recycle Bin ----- */
static int bin_items(void) {                                   /* how many things are in the bin */
    static uint32_t gen = 0xFFFFFFFF; static int count;
    if (gen == fs_generation) return count;
    gen = fs_generation; count = 0;
    struct fs_entry *l = kmalloc(sizeof(struct fs_entry) * EX_MAX);
    if (!l) return 0;
    int n = fs_list(BIN, l, EX_MAX);
    for (int i = 0; i < n; i++) { int k = kstrlen_(l[i].name); if (k < 4 || !same_ci(l[i].name + k - 4, ".inf")) count++; }
    kfree(l);
    return count;
}
/* Move a file or folder into the bin (a note remembers where it was). 1 = done. */
static int bin_put(const char *path) {
    if (fs_stat(BIN, 0) != 2 && !fs_mkdir(BIN)) return 0;
    char dst[FS_PATH_MAX], inf[FS_PATH_MAX];
    for (int n = 1; n < 100000; n++) {
        char *q = cat(dst, BIN "/R"); utoa(q, n);
        q = cat(inf, dst); cat(q, ".inf");
        if (!fs_stat(dst, 0) && !fs_stat(inf, 0)) break;
    }
    if (!tree_copy(path, dst, 0)) { tree_delete(dst, 0); return 0; }
    if (!fs_write(inf, path, kstrlen_(path))) { tree_delete(dst, 0); return 0; }
    if (!tree_delete(path, 0)) return 0;
    return 1;
}
static int make_folders(const char *dir) {                    /* like "mkdir -p" */
    if (!dir[0] || !kstrcmp(dir, "/") || fs_stat(dir, 0) == 2) return 1;
    char parent[FS_PATH_MAX]; path_resolve(dir, "..", parent);
    return make_folders(parent) && fs_mkdir(dir);
}
/* Put bin item i back where it came from. 1 = done. */
static int bin_restore(int i) {
    char src[FS_PATH_MAX], inf[FS_PATH_MAX], dest[FS_PATH_MAX], dir[FS_PATH_MAX];
    ex_child(i, src);
    char *q = cat(inf, src); cat(q, ".inf");
    kcpy(dest, ex_from[i], FS_PATH_MAX);
    path_resolve(dest, "..", dir);
    if (!make_folders(dir)) { kcpy(dir, "/", FS_PATH_MAX); path_resolve(dir, base_name(ex_from[i]), dest); }
    if (fs_stat(dest, 0)) {                                    /* something else has that name now */
        char alt[FS_NAME_MAX];
        copy_name(base_name(ex_from[i]), 1, ex_list[i].is_dir, alt);
        path_resolve(dir, alt, dest);
    }
    if (!tree_copy(src, dest, 0)) return 0;
    tree_delete(src, 0); fs_delete(inf);
    return 1;
}
static void bin_empty(void) {
    struct fs_entry *l = kmalloc(sizeof(struct fs_entry) * EX_MAX);
    if (!l) return;
    int n = fs_list(BIN, l, EX_MAX);
    for (int i = 0; i < n; i++) { char p[FS_PATH_MAX]; path_resolve(BIN, l[i].name, p); tree_delete(p, 0); }
    kfree(l);
}

/* ----- Delete ----- */
static int ex_confirm_kind;                                   /* 1 delete forever, 2 empty the bin */
static void ex_delete_now(void) {
    ex_confirm = 0;
    if (ex_confirm_kind == 2) { int n = ex_n; bin_empty(); ex_refresh(); char m[48], *q = utoa(m, n); cat(q, n == 1 ? " item deleted forever" : " items deleted forever"); ex_say(m, 0); return; }
    int idx[EX_MAX], n = ex_selected(idx), done = 0;
    if (!n) return;
    char name[FS_NAME_MAX]; kcpy(name, ex_name(idx[0]), FS_NAME_MAX);
    for (int j = 0; j < n; j++) {
        char p[FS_PATH_MAX]; ex_child(idx[j], p);
        int ok = tree_delete(p, 0);
        if (ok) clip_forget(p);
        if (ex_in_bin) { cat(p, ".inf"); fs_delete(p); }
        done += ok;
    }
    ex_status[0] = 0;
    if (n == 1) ex_say(done ? "Deleted forever: " : "Could not delete all of ", name);
    else say_count("Deleted forever: ", done, done < n ? " (some could not be deleted)" : 0);
    ex_sel = -1; ex_unmark();
    ex_refresh();
}
/* Delete: into the Recycle Bin. Shift+Del (or Delete inside the bin): forever, after asking. */
static void ex_delete(void) {
    int idx[EX_MAX], n = ex_selected(idx);
    if (!n) { ex_say("Select something to delete first", 0); return; }
    if (ex_in_bin || kbd_shift_down()) {
        ex_confirm_kind = 1;
        char p[FS_PATH_MAX]; ex_child(idx[0], p);
        ex_confirm_count = n == 1 && ex_list[idx[0]].is_dir ? tree_count(p, 0) : 0;
        ex_confirm = 1;
        return;
    }
    char name[FS_NAME_MAX]; kcpy(name, ex_list[idx[0]].name, FS_NAME_MAX);
    int done = 0;
    for (int j = 0; j < n; j++) {
        char p[FS_PATH_MAX]; ex_child(idx[j], p);
        if (bin_put(p)) { done++; clip_forget(p); }
    }
    ex_status[0] = 0;
    if (n == 1) ex_say(done ? "Moved to the Recycle Bin: " : "Could not move to the Recycle Bin: ", name);
    else say_count("Moved to the Recycle Bin: ", done, done < n ? " (some could not be moved)" : 0);
    ex_sel = -1; ex_unmark();
    ex_refresh();
}

/* ----- Rename ----- */
static void ex_rename_start(void) {
    if (ex_sel < 0 || ex_sel >= ex_n) { ex_say("Select something to rename first", 0); return; }
    ex_renaming = 1; ex_confirm = 0;
    ex_only(ex_sel);                                           /* renaming is for one item */
    kcpy(ex_rename_buf, ex_list[ex_sel].name, FS_NAME_MAX);
}
static void ex_rename_commit(void) {
    ex_renaming = 0;
    if (ex_sel < 0 || ex_sel >= ex_n || !ex_rename_buf[0] || !kstrcmp(ex_rename_buf, ex_list[ex_sel].name)) return;
    char p[FS_PATH_MAX], np[FS_PATH_MAX], name[FS_NAME_MAX];
    ex_child(ex_sel, p);
    kcpy(name, ex_rename_buf, FS_NAME_MAX);
    path_resolve(ex_path, name, np);
    int exists = fs_stat(np, 0) != 0;
    int ok = !exists && fs_rename(p, name);
    for (int i = 0; ok && i < ex_nclip; i++) if (!kstrcmp(ex_clip[i], p)) kcpy(ex_clip[i], np, FS_PATH_MAX);
    ex_say(ok ? "Renamed to " : exists ? "A file with this name already exists: " : "Can't use that name: ", name);
    ex_refresh();
    if (ok) ex_select_name(name);
}

/* ----- Copy / Cut / Paste ----- */
static void ex_copy(int cut) {
    int idx[EX_MAX], n = ex_selected(idx);
    if (!n) { ex_say("Select something to copy first", 0); return; }
    if (n > EX_CLIPS) n = EX_CLIPS;
    for (int j = 0; j < n; j++) ex_child(idx[j], ex_clip[j]);
    ex_nclip = n;
    ex_clip_cut = cut;
    ex_status[0] = 0;
    if (n == 1) ex_say(cut ? "Cut: " : "Copied: ", ex_list[idx[0]].name);
    else say_count(cut ? "Cut " : "Copied ", n, 0);
}
static void copy_name(const char *name, int n, int is_dir, char *out) {   /* "a.txt" -> "a - Copy (2).txt" */
    int len = 0, dot = -1; while (name[len]) len++;
    if (!is_dir) for (int i = 1; i < len; i++) if (name[i] == '.') dot = i;
    int stem = dot > 0 ? dot : len;
    int k = 0; for (; k < stem && k < 40; k++) out[k] = name[k];
    char *q = cat(out + k, " - Copy");
    if (n > 1) { q = cat(q, " ("); q = utoa(q, n); q = cat(q, ")"); }
    if (dot > 0) cat(q, name + dot);
}
/* a folder can't be put inside itself (or inside one of its own folders) */
static int is_inside(const char *dir, const char *folder) {
    int sl = kstrlen_(folder);
    for (int i = 0; i < sl; i++) if ((dir[i] | 32) != (folder[i] | 32)) return 0;
    return dir[sl] == 0 || dir[sl] == '/';
}
/* Paste one clipboard item into the open folder. 1 done, 0 failed, -1 skipped (msg says why) */
static int paste_one(const char *src, int moving, char *name, const char **why) {
    int kind = fs_stat(src, 0);
    if (!kind) { *why = "The copied item no longer exists"; return -1; }
    char dst[FS_PATH_MAX];
    kcpy(name, base_name(src), FS_NAME_MAX);
    path_resolve(ex_path, name, dst);
    if (kind == 2 && is_inside(ex_path, src)) { *why = "Can't put a folder inside itself"; return -1; }
    if (moving && !kstrcmp(dst, src)) { *why = "It is already in this folder"; return -1; }
    for (int n = 1; fs_stat(dst, 0) && n < 50; n++) {       /* name taken: "x - Copy", "x - Copy (2)" ... */
        copy_name(base_name(src), n, kind == 2, name);
        path_resolve(ex_path, name, dst);
    }
    int ok = tree_copy(src, dst, 0);
    if (ok && moving) ok = tree_delete(src, 0);               /* move = copy, then delete the original */
    return ok;
}
static void ex_paste(void) {
    if (!ex_nclip) { ex_say("Nothing to paste - use Copy or Cut first", 0); return; }
    int moving = ex_clip_cut, done = 0, n = ex_nclip;
    static char names[EX_CLIPS][FS_NAME_MAX];
    const char *why = 0; int failed = 0;
    for (int j = 0; j < n; j++) {
        int r = paste_one(ex_clip[j], moving, names[done], &why);
        if (r > 0) done++; else if (!r) failed++;
    }
    if (moving) { ex_nclip = 0; ex_clip_cut = 0; }           /* cut items can be pasted once */
    ex_status[0] = 0;
    if (n == 1 && done) ex_say(moving ? "Moved " : "Pasted ", names[0]);
    else if (n == 1) ex_say(why && !failed ? why : "Paste failed (disk full?)", 0);
    else say_count(moving ? "Moved " : "Pasted ", done, failed ? " - some failed (disk full?)" : done < n ? " - some were skipped" : 0);
    ex_refresh();
    ex_sel = -1; ex_unmark();
    for (int j = 0; j < done; j++) ex_add_name(names[j]);
}

static void size_text(char *b, uint32_t s) {
    if (s < 1024) { char *p = utoa(b, s); cat(p, " B"); }
    else if (s < 1024 * 1024) { char *p = utoa(b, (s + 1023) / 1024); cat(p, " KB"); }
    else { char *p = utoa(b, s / (1024 * 1024)); cat(p, " MB"); }
}

/* ----- Properties: a panel over the Explorer window ----- */
static int ex_props;                                           /* 1 = the panel is up */
static char props_t[40], props_k[8][16], props_v[8][48];
static int props_n;
static void tree_stat(const char *path, int depth, uint32_t *bytes, int *files, int *dirs) {
    if (depth > 16) return;
    struct fs_entry *l = kmalloc(sizeof(struct fs_entry) * EX_MAX);
    if (!l) return;
    int n = fs_list(path, l, EX_MAX);
    for (int i = 0; i < n; i++) {
        if (l[i].is_dir) { char c[FS_PATH_MAX]; path_resolve(path, l[i].name, c); (*dirs)++; tree_stat(c, depth + 1, bytes, files, dirs); }
        else { (*files)++; *bytes += l[i].size; }
    }
    kfree(l);
}
static void props_add(const char *k, const char *v) {
    if (props_n >= 8) return;
    kcpy(props_k[props_n], k, 16); kcpy(props_v[props_n], v, 48); props_n++;
}
static void props_contents(char *b, uint32_t bytes, int files, int dirs) {   /* "12 KB - 3 files, 1 folder" */
    size_text(b, bytes); char *q = b + kstrlen_(b);
    q = cat(q, "  ("); q = utoa(q, files); q = cat(q, files == 1 ? " file, " : " files, ");
    q = utoa(q, dirs); cat(q, dirs == 1 ? " folder)" : " folders)");
}
static void date_text(char *b, uint16_t date) {
    two_digits(b, date & 31); b[2] = '/'; two_digits(b + 3, (date >> 5) & 15); b[5] = '/'; utoa(b + 6, 1980 + (date >> 9));
}
static void ex_props_open(int folder_itself) {
    props_n = 0;
    char b[64], show[FS_PATH_MAX];
    int idx[EX_MAX], n = folder_itself ? 0 : ex_selected(idx);
    if (n > 1) {                                               /* several items: totals */
        uint32_t bytes = 0; int files = 0, dirs = 0;
        for (int j = 0; j < n; j++) {
            struct fs_entry *e = &ex_list[idx[j]];
            if (e->is_dir) { char p[FS_PATH_MAX]; ex_child(idx[j], p); dirs++; tree_stat(p, 0, &bytes, &files, &dirs); }
            else { files++; bytes += e->size; }
        }
        char *q = utoa(props_t, n); cat(q, " items");
        props_contents(b, bytes, files, dirs); props_add("Size", b);
        path_display(ex_path, show); show[47] = 0; props_add("Location", ex_in_bin ? "Recycle Bin" : show);
    } else if (n == 1) {
        int i = idx[0]; struct fs_entry *e = &ex_list[i];
        char p[FS_PATH_MAX]; ex_child(i, p);
        kcpy(props_t, ex_name(i), 40);
        const char *nm = ex_name(i), *ext = 0;
        for (const char *c = nm; *c; c++) if (*c == '.') ext = c + 1;
        if (e->is_dir) props_add("Type", "Folder");
        else if (is_elf(nm)) props_add("Type", "Program");
        else if (image_is_picture_name(nm)) props_add("Type", "Picture");
        else if (ext) { char *q = b; for (int k = 0; ext[k] && k < 8; k++) *q++ = ext[k] >= 'a' && ext[k] <= 'z' ? ext[k] - 32 : ext[k]; cat(q, " file"); props_add("Type", b); }
        else props_add("Type", "File");
        if (ex_in_bin) {
            char dir[FS_PATH_MAX]; path_resolve(ex_from[i], "..", dir); path_display(dir, show); show[47] = 0;
            props_add("Location", "Recycle Bin"); props_add("Deleted from", show);
        } else { path_display(ex_path, show); show[47] = 0; props_add("Location", show); }
        if (e->is_dir) { uint32_t bytes = 0; int files = 0, dirs = 0; tree_stat(p, 0, &bytes, &files, &dirs); props_contents(b, bytes, files, dirs); }
        else { size_text(b, e->size); if (e->size >= 1024) { char *q = b + kstrlen_(b); q = cat(q, "  ("); q = utoa(q, e->size); cat(q, " bytes)"); } }
        props_add(e->is_dir ? "Contains" : "Size", b);
        date_text(b, e->date); props_add("Modified", b);
    } else {                                                   /* the open folder itself */
        uint32_t bytes = 0; int files = 0, dirs = 0;
        if (ex_in_bin) kcpy(props_t, "Recycle Bin", 40);
        else kcpy(props_t, ex_path[1] ? base_name(ex_path) : "C:", 40);
        props_add("Type", ex_in_bin ? "Recycle Bin" : ex_path[1] ? "Folder" : "Local disk");
        tree_stat(ex_path, 0, &bytes, &files, &dirs);
        props_contents(b, bytes, files, dirs); props_add("Contains", b);
        if (!ex_path[1] && !ex_in_bin) {
            char *q = utoa(b, fs_free_kb() / 1024); cat(q, " MB"); props_add("Free space", b);
        }
    }
    ex_props = 1; ex_confirm = 0; ex_renaming = 0;
}
static void draw_props(int x, int y, int w, int h) {
    int pw = 400, ph = 92 + props_n * 22, px = x + (w - pw) / 2, py = y + (h - ph) / 2;
    if (py < y + 4) py = y + 4;
    gfx_shade(x, y, w, h, 90);
    gfx_rfill(px + 3, py + 4, pw, ph, 8, 0x9CA3AF);
    gfx_rfill(px, py, pw, ph, 8, C_WHITE);
    gfx_text(px + 16, py + 12, "Properties", C_MUTED, 0);
    char t[40]; kcpy(t, props_t, 34); gfx_text(px + 16, py + 30, t, C_TEXT, 1);
    gfx_fill(px + 16, py + 52, pw - 32, 1, 0xE5E7EB);
    for (int i = 0; i < props_n; i++) {
        gfx_text(px + 16, py + 60 + i * 22, props_k[i], C_MUTED, 0);
        gfx_text(px + 136, py + 60 + i * 22, props_v[i], C_TEXT, 0);
    }
    button(px + pw - 92, py + ph - 34, 76, "OK", hover_key == 1120);
}
static int props_hit(int lx, int ly, int w, int h) {          /* the OK button, in window coords */
    int pw = 400, ph = 92 + props_n * 22, px = (w - pw) / 2, py = (h - ph) / 2;
    if (py < 4) py = 4;
    return inside(lx, ly, px + pw - 92, py + ph - 34, 76, 24);
}

/* toolbar: label, x, width (hover codes 1100 + index) */
static const char *ex_btn_label[] = { "Up", "New folder", "New file", "Rename", "Copy", "Cut", "Paste", "Delete", "View: List" };
static const int ex_btn_x[] = { 6, 54, 154, 238, 306, 362, 410, 470, 548 };
static const int ex_btn_w[] = { 44, 96, 80, 64, 52, 44, 56, 64, 104 };
#define EX_BTNS 9
/* the Recycle Bin has its own buttons */
static const char *bin_btn_label[] = { "Up", "Restore", "Restore all", "Empty Recycle Bin", "Delete", "View: List" };
static const int bin_btn_x[] = { 6, 54, 126, 226, 374, 548 };
static const int bin_btn_w[] = { 44, 68, 96, 144, 64, 104 };
#define BIN_BTNS 6
static int ex_nbtn(void) { return ex_in_bin ? BIN_BTNS : EX_BTNS; }
static int ex_bx(int b) { return ex_in_bin ? bin_btn_x[b] : ex_btn_x[b]; }
static int ex_bw(int b) { return ex_in_bin ? bin_btn_w[b] : ex_btn_w[b]; }
static const char *ex_blabel(int b) {
    if (b == ex_nbtn() - 1) return cfg.explorer_view ? "View: Icons" : "View: List";
    return ex_in_bin ? bin_btn_label[b] : ex_btn_label[b];
}
static int ex_bdim(int b) {
    if (ex_in_bin) return ((b == 1 || b == 4) && ex_sel < 0) || ((b == 2 || b == 3) && !ex_n) || (b == 0 && 0);
    return (b == 6 && !ex_nclip) || ((b == 3 || b == 4 || b == 5 || b == 7) && ex_sel < 0);
}

/* ---------- Icons view: tiles with picture previews ---------- */
#define TILE_W 120
#define TILE_H 128
#define THUMB_W 104
#define THUMB_H 76
#define THUMBS 96
struct thumb { char path[FS_PATH_MAX]; uint32_t size; uint16_t date, time; int w, h, failed; uint32_t *px; };
static struct thumb thumbs[THUMBS];
static int thumb_next;                         /* slot to reuse next */
static int ex_thumbs_todo;                     /* previews still to make in this folder */

static struct thumb *thumb_find(const char *path, const struct fs_entry *e) {
    for (int i = 0; i < THUMBS; i++)
        if (thumbs[i].path[0] && !kstrcmp(thumbs[i].path, path) && thumbs[i].size == e->size && thumbs[i].date == e->date && thumbs[i].time == e->time) return &thumbs[i];
    return 0;
}
/* Make one preview (the first visible picture that has none). Called between frames. */
static int thumb_make_one(int first, int last) {
    for (int i = first; i < last && i < ex_n; i++) {
        struct fs_entry *e = &ex_list[i];
        if (e->is_dir || !image_is_picture_name(ex_name(i))) continue;
        char path[FS_PATH_MAX]; ex_child(i, path);
        if (thumb_find(path, e)) continue;
        struct thumb *t = &thumbs[thumb_next]; thumb_next = (thumb_next + 1) % THUMBS;
        if (t->px) { kfree(t->px); t->px = 0; }
        kcpy(t->path, path, FS_PATH_MAX); t->size = e->size; t->date = e->date; t->time = e->time; t->failed = 1;
        uint8_t *f = e->size && e->size <= 32 * 1024 * 1024 ? kmalloc(e->size) : 0;
        if (f && fs_read(path, f, e->size) == (int)e->size) {
            struct image im;
            if (!image_thumbnail(f, e->size, &im, THUMB_W, THUMB_H, img_alloc, img_free)) {
                for (int k = 0; k < im.w * im.h; k++) {      /* see-through parts: a light checkerboard */
                    uint32_t p = im.px[k], a = p >> 24;
                    if (a == 255) continue;
                    int x = k % im.w, y = k / im.w;
                    uint32_t bg = ((x >> 3) + (y >> 3)) & 1 ? 0xE5E7EB : 0xF9FAFB, o = 0;
                    for (int sh = 0; sh < 24; sh += 8) o |= ((((p >> sh) & 255) * a + ((bg >> sh) & 255) * (255 - a)) / 255) << sh;
                    im.px[k] = o;
                }
                t->px = im.px; t->w = im.w; t->h = im.h; t->failed = 0;
            }
        }
        if (f) kfree(f);
        return 1;
    }
    return 0;
}
static int tile_cols(int w) { int c = (w - 12) / TILE_W; return c < 1 ? 1 : c; }

/* big icons for things that are not pictures */
static void big_folder(int x, int y) {
    gfx_rfill(x, y + 6, 30, 12, 4, 0xD4A017);
    gfx_rfill(x, y + 12, 72, 50, 6, 0xE8B931);
    gfx_rfill(x, y + 18, 72, 44, 6, 0xF5C542);
}
static void big_file(int x, int y, const char *name) {
    gfx_rfill(x + 10, y, 52, 64, 4, 0x9CA3AF);
    gfx_rfill(x + 11, y + 1, 50, 62, 3, C_WHITE);
    gfx_fill(x + 46, y + 1, 15, 15, 0xE5E7EB);                 /* folded corner */
    const char *ext = 0; for (const char *q = name; *q; q++) if (*q == '.') ext = q + 1;
    uint32_t band = 0x6B7280;
    if (ext && (ext[0] | 32) == 'e' && (ext[1] | 32) == 'l') band = C_ACCENT;          /* programs */
    else if (ext && (ext[0] | 32) == 'w' && (ext[1] | 32) == 'a') band = 0xDB2777;     /* sounds */
    else if (ext && (ext[0] | 32) == 't' && (ext[1] | 32) == 'x') band = 0x059669;     /* text */
    for (int i = 0; i < 4; i++) gfx_fill(x + 18, y + 22 + i * 7, 36 - (i == 3 ? 12 : 0), 2, 0xD1D5DB);
    if (ext && *ext) {
        char up[6]; int k = 0;
        for (; ext[k] && k < 4; k++) up[k] = ext[k] >= 'a' && ext[k] <= 'z' ? ext[k] - 32 : ext[k];
        up[k] = 0;
        gfx_rfill(x + 14, y + 46, 44, 14, 3, band);
        gfx_text(x + 36 - k * 4, y + 45, up, C_WHITE, 1);
    }
}

static void draw_tiles(int x, int y, int w, int h) {
    int top = y + 70, lh = h - 70 - 22, cols = tile_cols(w), rows = lh / TILE_H;
    if (rows < 1) rows = 1;
    int xo = x + (w - cols * TILE_W) / 2;
    gfx_fill(x, top, w, lh, C_WHITE);
    int total_rows = (ex_n + cols - 1) / cols;
    if (ex_sel >= 0 && !band_on) {                             /* keep the selected tile in view */
        int r = ex_sel / cols;
        if (r < ex_iscroll) ex_iscroll = r;
        if (r >= ex_iscroll + rows) ex_iscroll = r - rows + 1;
    }
    if (ex_iscroll > total_rows - rows) ex_iscroll = total_rows - rows;
    if (ex_iscroll < 0) ex_iscroll = 0;
    if (!fs_mounted()) gfx_text(x + 40, top + 14, "No FAT32 disk attached.", C_MUTED, 0);
    else if (!ex_n) gfx_text(x + 40, top + 14, "This folder is empty.", C_MUTED, 0);
    gfx_clip(x, top, w, lh);
    int todo = 0;
    for (int r = 0; r <= rows; r++)
        for (int c = 0; c < cols; c++) {
            int i = (ex_iscroll + r) * cols + c;
            if (i >= ex_n) break;
            struct fs_entry *e = &ex_list[i];
            int tx = xo + c * TILE_W, ty = top + 6 + r * TILE_H;
            char full[FS_PATH_MAX]; ex_child(i, full);
            int is_cut = ex_clip_cut && in_clip(full);
            if (ex_is_sel(i)) gfx_rfill(tx + 2, ty, TILE_W - 4, TILE_H - 6, 6, C_ACCENT_LIGHT);
            else if (hover_key == 1200 + i) gfx_rfill(tx + 2, ty, TILE_W - 4, TILE_H - 6, 6, 0xF1F5F9);
            int ax = tx + (TILE_W - THUMB_W) / 2, ay = ty + 6;   /* the picture area */
            if (e->is_dir) big_folder(ax + (THUMB_W - 72) / 2, ay + 4);
            else if (image_is_picture_name(ex_name(i))) {
                struct thumb *t = thumb_find(full, e);
                if (t && !t->failed) {
                    int px = ax + (THUMB_W - t->w) / 2, py = ay + (THUMB_H - t->h) / 2;
                    gfx_fill(px + 2, py + 2, t->w, t->h, 0xD1D5DB);   /* a little shadow */
                    gfx_blit(px, py, t->w, t->h, t->px);
                    if (is_cut) gfx_shade(px, py, t->w, t->h, 180);
                } else {
                    big_file(ax + (THUMB_W - 72) / 2, ay + 4, ex_name(i));
                    if (!t) todo++;
                }
            } else big_file(ax + (THUMB_W - 72) / 2, ay + 4, ex_name(i));
            /* the name: two lines at most */
            int ny = ty + THUMB_H + 12;
            if (i == ex_sel && ex_renaming) { textbox(tx + 2, ny - 4, TILE_W - 4, ex_rename_buf, 1); continue; }
            const char *nm = ex_name(i);
            int len = kstrlen_(nm), per = (TILE_W - 8) / 8, cut = per;
            if (len > per)                                     /* break the line at a space if there is one */
                for (int k = per; k > 2; k--) if (nm[k] == ' ') { cut = k; break; }
            char l1[20], l2[20];
            kcpy(l1, nm, cut + 1);
            if (len > cut) {
                const char *rest = nm + cut + (nm[cut] == ' ');
                kcpy(l2, rest, per + 1);
                if (kstrlen_(rest) > per) { l2[per - 2] = '.'; l2[per - 1] = '.'; l2[per] = 0; }
            } else l2[0] = 0;
            uint32_t col = is_cut ? C_MUTED : C_TEXT;
            gfx_text(tx + (TILE_W - kstrlen_(l1) * 8) / 2, ny, l1, col, 0);
            if (l2[0]) gfx_text(tx + (TILE_W - kstrlen_(l2) * 8) / 2, ny + 16, l2, col, 0);
        }
    gfx_clip(x, y, w, h);
    if (total_rows > rows) {                                   /* scroll indicator */
        int bh = lh * rows / total_rows, by = lh * ex_iscroll / total_rows;
        gfx_rfill(x + w - 6, top + by, 4, bh > 10 ? bh : 10, 2, 0x9CA3AF);
    }
    ex_thumbs_todo = todo;
}
static int tile_at(int lx, int ly, int w, int h) {
    int top = 70, lh = h - 70 - 22, cols = tile_cols(w), xo = (w - cols * TILE_W) / 2;
    if (ly < top || ly >= top + lh || lx < xo || lx >= xo + cols * TILE_W) return -1;
    int r = (ly - top - 6) / TILE_H, c = (lx - xo) / TILE_W;
    if (ly - top - 6 < 0) return -1;
    int i = (ex_iscroll + r) * cols + c;
    return i < ex_n ? i : -1;
}
/* Called between frames: makes one missing preview, so the window stays usable. */
void wm_explorer_background(void) {
    if (!cfg.explorer_view || !find(APP_EXPLORER) || !ex_thumbs_todo) return;
    struct window *wn = &wins[APP_EXPLORER];
    int cols = tile_cols(wn->w - 2), rows = (wn->h - TITLE_H - 70 - 22) / TILE_H + 1;
    if (thumb_make_one(ex_iscroll * cols, (ex_iscroll + rows) * cols)) dirty = 1;
    else ex_thumbs_todo = 0;
}

static void draw_explorer(int x, int y, int w, int h) {
    if (ex_gen != fs_generation) ex_refresh();
    int hot = hover_key >= 1100 && hover_key < 1100 + ex_nbtn() ? hover_key - 1100 : -1;
    gfx_fill(x, y, w, 68, 0xF3F4F6);
    for (int b = 0; b < ex_nbtn(); b++) {
        int dim = ex_bdim(b);
        const char *label = ex_blabel(b);
        button(x + ex_bx(b), y + 6, ex_bw(b), label, hot == b && !dim);
        if (dim) gfx_text(x + ex_bx(b) + (ex_bw(b) - 8 * (int)kstrlen_(label)) / 2, y + 10, label, 0xB0B5BD, 0);
    }
    char d[FS_PATH_MAX];
    if (ex_in_bin) kcpy(d, "Recycle Bin", FS_PATH_MAX); else path_display(ex_path, d);
    gfx_text(x + 8, y + 42, "Address", C_MUTED, 0);
    textbox(x + 72, y + 38, w - 80, d, 0);
    gfx_fill(x, y + 68, w, 1, 0xD1D5DB);
    if (cfg.explorer_view) { draw_tiles(x, y, w, h); goto bottom; }
    /* column headers */
    int hy = y + 69;
    gfx_fill(x, hy, w, 32, C_WHITE);
    gfx_text(x + 40, hy + 9, "Name", C_MUTED, 1);
    if (ex_in_bin) gfx_text(x + w - 260, hy + 9, "Original location", C_MUTED, 1);
    else {
        gfx_text(x + w - 230, hy + 9, "Size", C_MUTED, 1);
        gfx_text(x + w - 140, hy + 9, "Modified", C_MUTED, 1);
    }
    gfx_fill(x + 8, y + EX_TOP - 2, w - 16, 1, 0xE5E7EB);
    int lh = h - EX_TOP - 22, rows = lh / EX_ROW;
    gfx_fill(x, y + EX_TOP - 1, w, lh + 1, C_WHITE);
    if (ex_sel >= 0 && !band_on) { if (ex_sel < ex_scroll) ex_scroll = ex_sel; if (ex_sel >= ex_scroll + rows) ex_scroll = ex_sel - rows + 1; }
    if (!fs_mounted()) gfx_text(x + 40, y + EX_TOP + 10, "No FAT32 disk attached.", C_MUTED, 0);
    else if (!ex_n) gfx_text(x + 40, y + EX_TOP + 10, ex_in_bin ? "The Recycle Bin is empty." : "This folder is empty.", C_MUTED, 0);
    for (int r = 0; r < rows && ex_scroll + r < ex_n; r++) {
        int i = ex_scroll + r, ry = y + EX_TOP + r * EX_ROW;
        struct fs_entry *e = &ex_list[i];
        char full[FS_PATH_MAX]; ex_child(i, full);
        int is_cut = ex_clip_cut && in_clip(full);           /* cut items look faded, like Windows */
        if (ex_is_sel(i)) gfx_rfill(x + 4, ry, w - 8, EX_ROW - 2, 4, C_ACCENT_LIGHT);
        else if (hover_key == 1200 + i) gfx_rfill(x + 4, ry, w - 8, EX_ROW - 2, 4, 0xF1F5F9);
        if (e->is_dir) folder_glyph(x + 14, ry + 3); else doc_glyph(x + 14, ry + 3);
        if (i == ex_sel && ex_renaming) {
            textbox(x + 36, ry - 1, w - 280, ex_rename_buf, 1);
        } else {
            char nm[40]; kcpy(nm, ex_name(i), (w - 280) / 8 < 39 ? (w - 280) / 8 : 39);
            gfx_text(x + 40, ry + 3, nm, is_cut ? C_MUTED : C_TEXT, 0);
        }
        if (ex_in_bin) {                                       /* where it was deleted from */
            char dir[FS_PATH_MAX], show[FS_PATH_MAX]; path_resolve(ex_from[i], "..", dir); path_display(dir, show);
            show[31] = 0; gfx_text(x + w - 260, ry + 3, show, C_MUTED, 0);
            continue;
        }
        char b[24];
        if (!e->is_dir) { size_text(b, e->size); gfx_text(x + w - 230, ry + 3, b, C_MUTED, 0); }
        else gfx_text(x + w - 230, ry + 3, "Folder", C_MUTED, 0);
        b[0] = 0; two_digits(b, e->date & 31); b[2] = '/'; two_digits(b + 3, (e->date >> 5) & 15); b[5] = '/';
        utoa(b + 6, 1980 + (e->date >> 9));
        gfx_text(x + w - 140, ry + 3, b, C_MUTED, 0);
    }
    if (ex_n > rows) {                                         /* simple scroll indicator */
        int bh = lh * rows / ex_n, by = lh * ex_scroll / ex_n;
        gfx_rfill(x + w - 6, y + EX_TOP + by, 4, bh > 10 ? bh : 10, 2, 0x9CA3AF);
    }
bottom:
    if (band_on) {                                             /* the selection box */
        int bx = band_x0 < band_x1 ? band_x0 : band_x1, by = band_y0 < band_y1 ? band_y0 : band_y1;
        int bw = band_x0 < band_x1 ? band_x1 - band_x0 : band_x0 - band_x1;
        int bh = band_y0 < band_y1 ? band_y1 - band_y0 : band_y0 - band_y1;
        int top = cfg.explorer_view ? 70 : EX_TOP;
        gfx_clip(x, y + top, w, h - top - 22);
        gfx_tint(x + bx, y + by, bw, bh, C_ACCENT, 50);
        gfx_fill(x + bx, y + by, bw, 1, C_ACCENT); gfx_fill(x + bx, y + by + bh - 1, bw, 1, C_ACCENT);
        gfx_fill(x + bx, y + by, 1, bh, C_ACCENT); gfx_fill(x + bx + bw - 1, y + by, 1, bh, C_ACCENT);
        gfx_clip(x, y, w, h);
    }
    int nsel = ex_nsel();
    if (ex_confirm && (nsel || ex_confirm_kind == 2)) {        /* "are you sure?" bar */
        int by = y + h - 22 - 48;
        gfx_fill(x, by, w, 48, 0xFEF3C7);
        gfx_fill(x, by, w, 1, 0xF59E0B);
        char m[128], nm[32]; kcpy(nm, ex_name(ex_sel), 32);
        char *q;
        if (ex_confirm_kind == 2) { q = cat(m, "Empty the Recycle Bin? "); q = utoa(q, ex_n); cat(q, ex_n == 1 ? " item goes for good." : " items go for good."); }
        else {
            if (nsel > 1) { q = cat(m, "Delete these "); q = utoa(q, nsel); q = cat(q, " items"); }
            else { q = cat(m, "Delete \""); q = cat(q, nm); }
            if (nsel > 1) ;
            else if (ex_confirm_count) { q = cat(q, "\" and the "); q = utoa(q, ex_confirm_count); q = cat(q, ex_confirm_count == 1 ? " item inside it" : " items inside it"); }
            else q = cat(q, "\"");
            cat(q, " forever?");
        }
        gfx_text(x + 12, by + 6, m, 0x78350F, 1);
        gfx_text(x + 12, by + 26, "This can't be undone.", 0x92400E, 0);
        int hy2 = by + 12;
        gfx_rfill(x + w - 172, hy2, 80, 26, 4, hover_key == 1110 ? 0xB91C1C : 0xDC2626);
        gfx_text(x + w - 172 + 16, hy2 + 5, "Delete", C_WHITE, 1);
        button(x + w - 86, hy2 + 1, 76, "Cancel", hover_key == 1111);
    }
    gfx_fill(x, y + h - 22, w, 22, 0xF3F4F6);
    char st[200], *p = cat(st, "  "); p = utoa(p, ex_n); p = cat(p, ex_n == 1 ? " item" : " items"); if (ex_in_bin) p = cat(p, " in the Recycle Bin");
    if (nsel > 1) { p = cat(p, "   |   "); p = utoa(p, nsel); p = cat(p, " selected"); }
    else if (ex_in_bin && ex_sel >= 0 && !ex_status[0]) {
        char dir[FS_PATH_MAX], show[FS_PATH_MAX]; path_resolve(ex_from[ex_sel], "..", dir); path_display(dir, show); show[40] = 0;
        p = cat(p, "   |   From "); p = cat(p, show);
    }
    if (fs_mounted()) { p = cat(p, "   |   "); p = utoa(p, fs_free_kb() / 1024); p = cat(p, " MB free"); }
    if (ex_status[0]) { p = cat(p, "   |   "); cat(p, ex_status); }
    else if (cfg.explorer_view && ex_thumbs_todo) { p = cat(p, "   |   Making previews..."); }
    gfx_text(x + 4, y + h - 19, st, C_MUTED, 0);
    if (ex_props) draw_props(x, y, w, h);
}

static int explorer_hover(int lx, int ly, int w, int h) {
    if (ex_props) return props_hit(lx, ly, w, h) ? 1120 : -1;
    if (ex_confirm) {
        int by = h - 22 - 48 + 12;
        if (ly >= by && ly < by + 26) {
            if (lx >= w - 172 && lx < w - 92) return 1110;
            if (lx >= w - 86 && lx < w - 10) return 1111;
        }
        if (ly >= h - 70) return -1;
    }
    if (ly >= 6 && ly < 30)
        for (int b = 0; b < ex_nbtn(); b++) if (lx >= ex_bx(b) && lx < ex_bx(b) + ex_bw(b)) return 1100 + b;
    if (cfg.explorer_view) { int t = tile_at(lx, ly, w, h); return t >= 0 ? 1200 + t : -1; }
    int rows = (h - EX_TOP - 22) / EX_ROW;
    if (ly >= EX_TOP && lx < w && ly < EX_TOP + rows * EX_ROW) {
        int i = ex_scroll + (ly - EX_TOP) / EX_ROW;
        if (i < ex_n) return 1200 + i;
    }
    return -1;
}

static void explorer_click_button(int hv);
static void explorer_click_rest(int hv, int lx, int ly, int w, int h);
static void explorer_click(int lx, int ly, int w, int h) {
    int hv = explorer_hover(lx, ly, w, h);
    if (ex_props) { if (hv == 1120) ex_props = 0; return; }   /* only OK counts while Properties is up */
    if (ex_confirm) {                                          /* only the bar's buttons count while it is up */
        if (hv == 1110) ex_delete_now();
        else if (hv == 1111) { ex_confirm = 0; ex_say("Delete cancelled", 0); }
        return;
    }
    if (ex_renaming && hv != 1200 + ex_sel) ex_rename_commit();   /* clicking elsewhere finishes renaming */
    if (ex_in_bin && hv >= 1100 && hv < 1100 + BIN_BTNS) { explorer_click_button(hv); return; }
    explorer_click_rest(hv, lx, ly, w, h);
}
static void explorer_click_button(int hv) {                    /* the Recycle Bin's toolbar */
    {
        if (ex_bdim(hv - 1100)) return;
        switch (hv) {
        case 1100: ex_up(); return;
        case 1101: { int idx[EX_MAX], n = ex_selected(idx), done = 0;
                     char nm[FS_NAME_MAX]; kcpy(nm, ex_name(ex_sel), FS_NAME_MAX);
                     for (int j = n - 1; j >= 0; j--) done += bin_restore(idx[j]);
                     ex_sel = -1; ex_unmark(); ex_status[0] = 0;
                     if (n == 1) ex_say(done ? "Restored " : "Could not restore ", nm); else say_count("Restored ", done, 0);
                     ex_refresh(); return; }
        case 1102: { int n = 0; for (int i = ex_n - 1; i >= 0; i--) n += bin_restore(i);
                     ex_refresh(); char m[40], *q = cat(m, "Restored "); q = utoa(q, n); cat(q, n == 1 ? " item" : " items"); ex_say(m, 0); return; }
        case 1103: ex_confirm_kind = 2; ex_confirm = 1; return;
        case 1104: ex_delete(); return;
        case 1105: cfg.explorer_view = !cfg.explorer_view; settings_save(); return;
        }
    }
}
static void explorer_click_rest(int hv, int lx, int ly, int w, int h) {
    (void)w;
    switch (hv) {
    case 1100: ex_up(); return;
    case 1101: ex_new(1); return;
    case 1102: ex_new(0); return;
    case 1103: ex_rename_start(); return;
    case 1104: ex_copy(0); return;
    case 1105: ex_copy(1); return;
    case 1106: ex_paste(); return;
    case 1107: ex_delete(); return;
    case 1108: cfg.explorer_view = !cfg.explorer_view; settings_save(); ex_say(cfg.explorer_view ? "Icons view" : "List view", 0); return;
    }
    int ctrl = kbd_ctrl_down(), shift = kbd_shift_down();
    if (hv >= 1200) {
        int i = hv - 1200;
        if (ex_renaming) return;
        if (ctrl) {                                            /* Ctrl-click: add or take away one item */
            if (ex_sel >= 0) ex_mark[ex_sel] = 1;
            ex_mark[i] = !ex_mark[i];
            if (ex_mark[i]) ex_sel = ex_anchor = i;
            else { ex_sel = -1; for (int j = 0; j < ex_n; j++) if (ex_mark[j]) { ex_sel = j; break; } }
            ex_last_row = -1; return;
        }
        if (shift) { ex_range(ex_anchor >= 0 ? ex_anchor : ex_sel, i); ex_last_row = -1; return; }
        if (i == ex_last_row && ticks - ex_last_click < TIMER_HZ / 2) { ex_last_row = -1; ex_only(i); ex_open(i); return; }
        if (ex_is_sel(i) && ex_nsel() > 1) { ex_collapse = i; ex_sel = i; }   /* keep the group: it may be dragged */
        else ex_only(i);
        ex_last_row = i; ex_last_click = ticks;
        if (!ex_in_bin) { dd_item = i; dd_px = mx; dd_py = my; }
    } else if (ly >= (cfg.explorer_view ? 70 : EX_TOP) && ly < h - 22) {
        /* empty space: start a selection box */
        if (!ctrl) { ex_unmark(); ex_sel = -1; }
        for (int j = 0; j < EX_MAX; j++) band_base[j] = ex_is_sel(j);
        band_add = ctrl;
        band_on = 1; band_x0 = band_x1 = lx; band_y0 = band_y1 = ly;
    }
}

/* where item i is in the window (Icons or List view) */
static void ex_item_rect(int i, int w, int *rx, int *ry, int *rw, int *rh) {
    if (cfg.explorer_view) {
        int cols = tile_cols(w), xo = (w - cols * TILE_W) / 2;
        *rx = xo + (i % cols) * TILE_W + 2; *ry = 70 + 6 + (i / cols - ex_iscroll) * TILE_H;
        *rw = TILE_W - 4; *rh = TILE_H - 6;
    } else { *rx = 4; *ry = EX_TOP + (i - ex_scroll) * EX_ROW; *rw = w - 8; *rh = EX_ROW - 2; }
}
/* the box moved: select what it touches */
static void band_update(void) {
    struct window *wn = &wins[APP_EXPLORER];
    int w = wn->w - 2, h = wn->h - TITLE_H - 1, top = cfg.explorer_view ? 70 : EX_TOP;
    int lx = mx - wn->x - 1, ly = my - wn->y - TITLE_H;
    if (lx < 0) lx = 0;
    if (lx > w) lx = w;
    if (ly < top) ly = top;
    if (ly > h - 22) ly = h - 22;
    band_x1 = lx; band_y1 = ly;
    int x0 = band_x0 < band_x1 ? band_x0 : band_x1, x1 = band_x0 ^ band_x1 ^ x0;
    int y0 = band_y0 < band_y1 ? band_y0 : band_y1, y1 = band_y0 ^ band_y1 ^ y0;
    ex_unmark(); ex_sel = -1;
    for (int i = 0; i < ex_n; i++) {
        int rx, ry, rw, rh; ex_item_rect(i, w, &rx, &ry, &rw, &rh);
        int hit = rx < x1 && rx + rw > x0 && ry < y1 && ry + rh > y0 && ry + rh > top && ry < h - 22;
        if (band_add ? hit != band_base[i] : hit) ex_mark[i] = 1;
    }
    dirty = 1;
}
static void band_end(void) {
    band_on = 0;
    for (int i = 0; i < ex_n; i++) if (ex_mark[i]) { ex_sel = ex_anchor = i; break; }
    int n = ex_nsel();
    if (n > 1) { ex_status[0] = 0; }
}

static void explorer_key(int k) {
    if (ex_props) { if (k == '\n' || k == KEY_ESC || k == ' ') ex_props = 0; return; }
    if (ex_confirm) {
        if (k == '\n' || k == 'y' || k == 'Y') ex_delete_now();
        else if (k == KEY_ESC || k == 'n' || k == 'N') { ex_confirm = 0; ex_say("Delete cancelled", 0); }
        return;
    }
    if (ex_renaming) {
        int n = 0; while (ex_rename_buf[n]) n++;
        if (k == '\n') ex_rename_commit();
        else if (k == KEY_ESC) ex_renaming = 0;
        else if (k == '\b') { if (n) ex_rename_buf[n - 1] = 0; }
        else if (k >= 32 && k < 127 && n < FS_NAME_MAX - 1 && k != '/' && k != '\\' && k != ':' && k != '*' && k != '?' && k != '"' && k != '<' && k != '>' && k != '|') {
            ex_rename_buf[n] = (char)k; ex_rename_buf[n + 1] = 0;
        }
        return;
    }
    int step = 1, before = ex_sel, moved = 1;
    if (k == KEY_CTRL('a')) {                                  /* select everything */
        for (int i = 0; i < ex_n; i++) ex_mark[i] = 1;
        if (ex_sel < 0 && ex_n) ex_sel = ex_anchor = 0;
        return;
    }
    if (k == KEY_ESC) { if (ex_nsel() > 1) ex_only(ex_sel); return; }
    if (cfg.explorer_view) {                                   /* in the grid, up/down jump a whole row */
        step = tile_cols(wins[APP_EXPLORER].w - 2);
        if (k == KEY_RIGHT || k == KEY_LEFT) {
            if (k == KEY_RIGHT && ex_sel < ex_n - 1) ex_sel++;
            if (k == KEY_LEFT && ex_sel > 0) ex_sel--;
            goto nav;
        }
    }
    if (ex_in_bin && (k == KEY_CTRL('c') || k == KEY_CTRL('x') || k == KEY_CTRL('v') || k == KEY_F2 || k == KEY_CTRL('n'))) {
        ex_say("Restore it first", 0); return;
    }
    if (k == KEY_DOWN) { if (ex_sel < ex_n - 1) { ex_sel = ex_sel < 0 ? 0 : ex_sel + step; if (ex_sel > ex_n - 1) ex_sel = ex_n - 1; } }
    else if (k == KEY_UP) { if (ex_sel > 0) { ex_sel -= step; if (ex_sel < 0) ex_sel = 0; } }
    else if (k == KEY_PGDN && ex_n) { ex_sel += step * 4; if (ex_sel > ex_n - 1) ex_sel = ex_n - 1; }
    else if (k == KEY_PGUP && ex_n) { ex_sel -= step * 4; if (ex_sel < 0) ex_sel = 0; }
    else if (k == KEY_HOME && ex_n) ex_sel = 0;
    else if (k == KEY_END && ex_n) ex_sel = ex_n - 1;
    else moved = 0;
nav:
    if (moved) {                                               /* Shift+arrows grow the selection */
        if (ex_sel < 0) return;
        if (kbd_shift_down()) ex_range(ex_anchor >= 0 ? ex_anchor : (before >= 0 ? before : ex_sel), ex_sel);
        else ex_only(ex_sel);
        return;
    }
    if (k == '\n') { if (ex_sel >= 0) { ex_only(ex_sel); ex_open(ex_sel); } }
    else if (k == '\b') ex_up();
    else if (k == KEY_DEL) ex_delete();
    else if (k == KEY_F2) ex_rename_start();
    else if (k == KEY_CTRL('c')) ex_copy(0);
    else if (k == KEY_CTRL('x')) ex_copy(1);
    else if (k == KEY_CTRL('v')) ex_paste();
    else if (k == KEY_CTRL('n')) ex_new(1);
    else if (k == KEY_CTRL('e')) { cfg.explorer_view = !cfg.explorer_view; settings_save(); }
    else if (k == KEY_ALTENTER) ex_props_open(ex_sel < 0);
    else if (k > ' ' && k < 127 && ex_n) {                     /* type a name to jump to it */
        static char typed[24]; static uint32_t last;
        int n = kstrlen_(typed);
        if (ticks - last > TIMER_HZ) n = 0;                    /* a pause starts a new search */
        last = ticks;
        if (n < 23) { typed[n++] = (char)k; typed[n] = 0; }
        int again = n == 1 && ex_sel >= 0 && (ex_name(ex_sel)[0] | 32) == (k | 32);
        for (int d = again ? 1 : 0; d <= ex_n; d++) {          /* the same letter again: the next match */
            int i = ((ex_sel < 0 ? 0 : ex_sel) + d) % ex_n, ok = 1;
            for (int c = 0; c < n && ok; c++) {
                char a = ex_name(i)[c], b = typed[c];
                if (a >= 'A' && a <= 'Z') a += 32;
                if (b >= 'A' && b <= 'Z') b += 32;
                ok = a == b;
            }
            if (ok) { ex_only(i); break; }
        }
    }
}

void wm_open_file(const char *path) {
    open_app(APP_NOTEPAD);
    note_field_focus = 0; find_focus = 0;
    if (!note_load(path) && !fs_stat(path, 0)) {               /* a name that doesn't exist yet: a new file by that name */
        note_new();
        path_display(path, note_field);
        note_set_status("New file - Ctrl+S saves it");
    }
    dirty = 1;
}
void wm_open_folder(const char *path) {
    open_app(APP_EXPLORER);
    ex_go(path);
    dirty = 1;
}

/* ================= Program windows ================= */
static int is_proc(int id) { return id >= APP_COUNT; }
static int proc_slot(int id) { return id - APP_COUNT; }

void wm_proc_opened(int slot) {
    int id = APP_COUNT + slot;
    static int cascade;
    int x = 230 + (cascade % 6) * 36, y = 70 + (cascade % 6) * 30;
    cascade++;
    wins[id] = (struct window){ id, x, y, PROC_CANVAS_W + 2, PROC_CANVAS_H + TITLE_H + 1, 0, 0, 0, 0, 0, 0, 0 };
    zorder[nwin++] = id;
    dirty = 1;
}
static int mcapture = -1;                       /* program window that owns the mouse while a button is held */
void wm_proc_closed(int slot) { if (mcapture == APP_COUNT + slot) mcapture = -1; close_app(APP_COUNT + slot); }

void wm_proc_resized(int slot) {
    struct window *w = &wins[APP_COUNT + slot];
    w->maxed = w->snapped = 0;                                 /* the program chose a size itself */
    w->w = tasks[slot].cw + 2;
    w->h = tasks[slot].ch + TITLE_H + 1;
    if (w->x + w->w > gfx_w) w->x = gfx_w - w->w > 0 ? gfx_w - w->w : 0;
    if (w->y + w->h > gfx_h - TASKBAR_H) w->y = gfx_h - TASKBAR_H - w->h > 0 ? gfx_h - TASKBAR_H - w->h : 0;
    dirty = 1;
}
void wm_redraw(void) { dirty = 1; }

/* Programs redraw often (animations, counters). Repainting the whole desktop
 * each time is wasteful, so we remember which program windows changed and,
 * when nothing covers them, copy just those windows to the screen. */
static uint32_t canvas_dirty;                   /* one bit per task slot */
void wm_canvas_dirty(int slot) { canvas_dirty |= 1u << slot; }

static int covered(int id) {                    /* is any part of window id hidden? */
    struct window *a = &wins[id];
    int i = 0; while (i < nwin && zorder[i] != id) i++;
    for (i++; i < nwin; i++) {
        struct window *b = &wins[zorder[i]];
        if (b->minimized) continue;
        if (b->x < a->x + a->w + 6 && b->x + b->w + 6 > a->x && b->y < a->y + a->h + 8 && b->y + b->h + 8 > a->y) return 1;
    }
    if (start_open) return 1;
    return a->x < 0 || a->y < 0 || a->x + a->w > gfx_w || a->y + a->h > gfx_h - TASKBAR_H;
}

/* Returns 1 if every changed program window could be updated on its own. */
static int fast_canvas_update(void) {
    for (int s = 1; s < MAX_TASKS; s++) {
        if (!(canvas_dirty & (1u << s))) continue;
        int id = APP_COUNT + s;
        if (!find(id) || wins[id].minimized) continue;
        if (covered(id)) return 0;
    }
    for (int s = 1; s < MAX_TASKS; s++) {
        if (!(canvas_dirty & (1u << s))) continue;
        int id = APP_COUNT + s;
        if (!find(id) || wins[id].minimized || !tasks[s].canvas) continue;
        struct window *w = &wins[id];
        gfx_noclip();
        gfx_blit(w->x + 1, w->y + TITLE_H, tasks[s].cw, tasks[s].ch, tasks[s].canvas);
        gfx_present_rect(w->x + 1, w->y + TITLE_H, tasks[s].cw, tasks[s].ch);
    }
    canvas_dirty = 0;
    gfx_cursor_move(mx, my);
    return 1;
}

static void run_program(const char *path) {
    int r = proc_spawn(path);
    if (r < 0) {
        const char *why = r == -1 ? "File not found." : r == -2 ? "Too many programs are running." :
                          r == -3 ? "Not enough memory." : "This file is not a MyOS program.";
        wm_proc_crashed(base_name(path), why);
    }
}

/* ================= "Program has stopped working" dialog ================= */
static char msg_name[40], msg_text[100];

void wm_proc_crashed(const char *name, const char *msg) {
    kcpy(msg_name, name, sizeof msg_name);
    kcpy(msg_text, msg, sizeof msg_text);
    if (find(APP_MSG)) close_app(APP_MSG);
    wins[APP_MSG] = (struct window){ APP_MSG, gfx_w / 2 - 210, gfx_h / 2 - 110, 420, 190, 0, 0, 0, 0, 0, 0, 0 };
    zorder[nwin++] = APP_MSG;
    dirty = 1;
}

static void draw_msg(int x, int y, int w, int h) {
    gfx_fill(x, y, w, h, C_WHITE);
    /* warning triangle */
    for (int j = 0; j < 30; j++) gfx_fill(x + 40 - j / 2 - 1, y + 20 + j, j + 2, 1, 0xF59E0B);
    gfx_fill(x + 39, y + 30, 3, 11, C_TEXT); gfx_fill(x + 39, y + 44, 3, 3, C_TEXT);
    char t[80], *p = cat(t, msg_name); cat(p, " has stopped working");
    gfx_text(x + 72, y + 22, t, C_TEXT, 1);
    /* wrap the message to the dialog width */
    int cols = (w - 90) / 8, n = 0; while (msg_text[n]) n++;
    for (int i = 0, line = 0; i < n && line < 3; line++) {
        char l[64]; int k = 0;
        int end = i + cols < n ? i + cols : n;
        if (end < n) { int s = end; while (s > i && msg_text[s] != ' ') s--; if (s > i) end = s; }
        while (i < end && k < 63) l[k++] = msg_text[i++];
        l[k] = 0;
        while (msg_text[i] == ' ') i++;
        gfx_text(x + 72, y + 48 + line * 18, l, C_MUTED, 0);
    }
    gfx_text(x + 72, y + 96, "MyOS closed it. Everything else is fine.", C_MUTED, 0);
    button(x + w - 96, y + h - 36, 84, "Close", hover_key == 1300);
}

/* ================= Task Manager ================= */
static int tm_sel = -1;
static uint8_t cpu_hist[60];                     /* total CPU % for the last minute */
static int sys_pct;

static void cpu_sample(void) {
    int total = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->state == T_FREE) continue;
        uint32_t d = t->cpu_ticks - t->cpu_last;
        t->cpu_last = t->cpu_ticks;
        t->cpu_pct = d * 100 / TIMER_HZ > 100 ? 100 : d * 100 / TIMER_HZ;
        total += t->cpu_pct;
    }
    sys_pct = total > 100 ? 100 : total;
    for (int i = 0; i < 59; i++) cpu_hist[i] = cpu_hist[i + 1];
    cpu_hist[59] = (uint8_t)sys_pct;
}

static const char *state_name(int s) {
    switch (s) { case T_READY: return "Running"; case T_WAIT_KEY: return "Waiting for keys"; case T_WAIT_EVENT: return "Waiting for input";
                 case T_SLEEP: return "Sleeping"; case T_ZOMBIE: return "Ending"; }
    return "";
}

#define TM_ROW 24
#define TM_TOP 150
static void draw_taskmgr(int x, int y, int w, int h) {
    gfx_fill(x, y, w, h, C_WHITE);
    /* CPU graph */
    gfx_text(x + 12, y + 10, "CPU", C_TEXT, 1);
    char b[64], *p = utoa(b, sys_pct); cat(p, "%");
    gfx_text(x + 48, y + 10, b, C_ACCENT, 1);
    struct mm_stats st; mm_stats(&st);
    p = cat(b, "Memory "); p = utoa(p, st.used_kb / 1024); p = cat(p, " / "); p = utoa(p, st.total_kb / 1024); cat(p, " MB");
    gfx_text(x + w - 12 - 8 * 20, y + 10, b, C_MUTED, 0);
    int gx = x + 12, gy = y + 32, gw = w - 24, gh = 90;
    gfx_fill(gx, gy, gw, gh, 0xEFF6FF);
    gfx_rect(gx, gy, gw, gh, 0xBFDBFE);
    for (int i = 1; i < 4; i++) gfx_fill(gx + 1, gy + gh * i / 4, gw - 2, 1, 0xDBEAFE);
    for (int i = 0; i < 59; i++) {
        int x0 = gx + i * gw / 59, x1 = gx + (i + 1) * gw / 59;
        int y0 = gy + gh - 1 - cpu_hist[i] * (gh - 2) / 100, y1 = gy + gh - 1 - cpu_hist[i + 1] * (gh - 2) / 100;
        gfx_thick_line(x0, y0, x1, y1, 2, C_ACCENT);
    }
    gfx_text(gx, gy + gh + 4, "60 seconds", C_MUTED, 0);
    gfx_text(gx + gw - 32, gy + gh + 4, "now", C_MUTED, 0);
    /* process table */
    gfx_fill(x, y + TM_TOP - 26, w, 26, 0xF3F4F6);
    gfx_text(x + 16, y + TM_TOP - 21, "Name", C_MUTED, 1);
    gfx_text(x + 200, y + TM_TOP - 21, "PID", C_MUTED, 1);
    gfx_text(x + 250, y + TM_TOP - 21, "Status", C_MUTED, 1);
    gfx_text(x + 410, y + TM_TOP - 21, "CPU", C_MUTED, 1);
    gfx_text(x + 470, y + TM_TOP - 21, "Memory", C_MUTED, 1);
    int row = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->state == T_FREE) continue;
        int ry = y + TM_TOP + row * TM_ROW;
        if (ry + TM_ROW > y + h - 44) break;
        if (tm_sel == i) gfx_rfill(x + 4, ry, w - 8, TM_ROW - 2, 4, C_ACCENT_LIGHT);
        else if (hover_key == 1400 + i) gfx_rfill(x + 4, ry, w - 8, TM_ROW - 2, 4, 0xF1F5F9);
        gfx_text(x + 16, ry + 3, i == 0 ? "System (kernel)" : t->name, C_TEXT, i == 0);
        p = utoa(b, t->pid); gfx_text(x + 200, ry + 3, b, C_MUTED, 0);
        gfx_text(x + 250, ry + 3, i == 0 ? "Running" : state_name(t->state), C_MUTED, 0);
        p = utoa(b, t->cpu_pct); cat(p, "%");
        gfx_text(x + 410, ry + 3, b, t->cpu_pct > 50 ? 0xDC2626 : C_TEXT, t->cpu_pct > 50);
        uint32_t kb = i == 0 ? st.heap_mapped_kb : t->pages * 4 + (uint32_t)(t->cw * t->ch * 4) / 1024 + 16;
        p = utoa(b, kb); cat(p, " KB");
        gfx_text(x + 470, ry + 3, b, C_MUTED, 0);
        row++;
    }
    gfx_fill(x, y + h - 44, w, 44, 0xF3F4F6);
    int nproc = 0; for (int i = 1; i < MAX_TASKS; i++) if (tasks[i].state != T_FREE) nproc++;
    p = cat(b, "Programs running: "); utoa(p, nproc);
    gfx_text(x + 12, y + h - 30, b, C_MUTED, 0);
    button(x + w - 108, y + h - 36, 96, "End task", hover_key == 1450);
}

static int taskmgr_hover(int lx, int ly, int w, int h) {
    if (ly >= h - 36 && ly < h - 12 && lx >= w - 108 && lx < w - 12) return 1450;
    int row = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == T_FREE) continue;
        if (ly >= TM_TOP + row * TM_ROW && ly < TM_TOP + (row + 1) * TM_ROW && TM_TOP + (row + 1) * TM_ROW <= h - 44) return 1400 + i;
        row++;
    }
    return -1;
}

static void taskmgr_click(int lx, int ly, int w, int h) {
    int hv = taskmgr_hover(lx, ly, w, h);
    if (hv >= 1400 && hv < 1450) tm_sel = hv - 1400;
    else if (hv == 1450 && tm_sel > 0 && tasks[tm_sel].state != T_FREE) { proc_kill(tasks[tm_sel].pid); tm_sel = -1; }
}

/* ================= Settings ================= */
static const char *preset_names[WALLPAPER_PRESETS + 1] = { "Ocean", "Sunset", "Forest", "Midnight", "Picture" };
static const char *accent_names[ACCENT_COUNT] = { "Blue", "Purple", "Green", "Orange", "Pink", "Graphite" };
static const char *speed_names[4] = { "Slow", "Normal", "Fast", "Fastest" };

/* hit-test codes inside the Settings window */
#define SH_TILE  0      /* 0..4 wallpaper tiles */
#define SH_PIC   5
#define SH_ACC   10     /* 10..15 */
#define SH_TZM   20
#define SH_TZP   21
#define SH_VOLM  30
#define SH_VOLP  31
#define SH_CHIME 32
#define SH_SPEED 40     /* 40..43 */

static void settings_changed(int visual) {
    settings_apply();
    if (visual) { apply_accent(); build_wallpaper(); }
    settings_save();
    dirty = 1;
}

/* Pick the next picture (.png, .jpg or .bmp) in C:\Pictures as the wallpaper. */
static void next_picture(void) {
    static struct fs_entry list[64];
    int n = fs_list("/Pictures", list, 64), first = -1, pick = -1, seen_cur = 0;
    for (int i = 0; i < n; i++) {
        if (list[i].is_dir || !image_is_picture_name(list[i].name)) continue;
        char p[FS_PATH_MAX]; path_resolve("/Pictures", list[i].name, p);
        if (first < 0) first = i;
        if (seen_cur && pick < 0) pick = i;
        if (!kstrcmp(p, cfg.picture)) seen_cur = 1;
    }
    if (pick < 0) pick = first;
    if (pick < 0) { wm_proc_crashed("Settings", "There are no pictures (.png, .jpg or .bmp) in C:\\Pictures yet."); return; }
    path_resolve("/Pictures", list[pick].name, cfg.picture);
    cfg.wallpaper = WALLPAPER_PRESETS;
    settings_changed(1);
}

static int settings_hit(int lx, int ly) {
    for (int i = 0; i <= WALLPAPER_PRESETS; i++) if (inside(lx, ly, 16 + i * 106, 36, 96, 60)) return SH_TILE + i;
    if (inside(lx, ly, 16, 120, 190, 24)) return SH_PIC;
    for (int i = 0; i < ACCENT_COUNT; i++) if (inside(lx, ly, 14 + i * 46, 190, 32, 32)) return SH_ACC + i;
    if (inside(lx, ly, 16, 262, 32, 24)) return SH_TZM;
    if (inside(lx, ly, 160, 262, 32, 24)) return SH_TZP;
    if (inside(lx, ly, 16, 332, 32, 24)) return SH_VOLM;
    if (inside(lx, ly, 128, 332, 32, 24)) return SH_VOLP;
    if (inside(lx, ly, 260, 332, 200, 24)) return SH_CHIME;
    for (int i = 0; i < 4; i++) if (inside(lx, ly, 16 + i * 100, 402, 92, 24)) return SH_SPEED + i;
    return -1;
}

static void settings_click(int lx, int ly) {
    int h = settings_hit(lx, ly);
    if (h < 0) return;
    if (h <= SH_TILE + WALLPAPER_PRESETS) {
        if (h == SH_TILE + WALLPAPER_PRESETS && !cfg.picture[0]) { next_picture(); return; }
        cfg.wallpaper = h - SH_TILE; settings_changed(1);
    }
    else if (h == SH_PIC) next_picture();
    else if (h >= SH_ACC && h < SH_ACC + ACCENT_COUNT) { cfg.accent = h - SH_ACC; settings_changed(1); }
    else if (h == SH_TZM && cfg.timezone > -12) { cfg.timezone--; settings_changed(0); }
    else if (h == SH_TZP && cfg.timezone < 14) { cfg.timezone++; settings_changed(0); }
    else if (h == SH_VOLM) { cfg.volume = cfg.volume >= 10 ? cfg.volume - 10 : 0; settings_changed(0); }
    else if (h == SH_VOLP) { cfg.volume = cfg.volume <= 90 ? cfg.volume + 10 : 100; settings_changed(0); }
    else if (h == SH_CHIME) { cfg.chime = !cfg.chime; settings_changed(0); if (cfg.chime) { sound_tone(523, 90); sound_tone(784, 140); } }
    else if (h >= SH_SPEED && h < SH_SPEED + 4) { cfg.mouse = h - SH_SPEED + 1; settings_changed(0); }
}

static void small_button(int x, int y, int w, const char *label, int hot, int on) {
    gfx_rfill(x, y, w, 24, 4, on ? C_ACCENT : hot ? C_ACCENT_LIGHT : 0xFFFFFF);
    gfx_rect(x, y, w, 24, on ? C_ACCENT : 0xD1D5DB);
    int n = 0; while (label[n]) n++;
    gfx_text(x + (w - n * 8) / 2, y + 4, label, on ? C_WHITE : C_TEXT, on);
}

static void draw_settings(int x, int y, int w, int h) {
    gfx_fill(x, y, w, h, C_WHITE);
    int hv = hover_key >= 1600 && hover_key < 1700 ? hover_key - 1600 : -1;
    char b[64], *p;
    /* wallpaper */
    gfx_text(x + 16, y + 12, "Wallpaper", C_TEXT, 1);
    for (int i = 0; i <= WALLPAPER_PRESETS; i++) {
        int tx = x + 16 + i * 106, ty = y + 36, sel = cfg.wallpaper == i;
        gfx_fill(tx - 3, ty - 3, 102, 66, sel ? C_ACCENT : hv == SH_TILE + i ? C_ACCENT_LIGHT : C_WHITE);
        gfx_clip(tx, ty, 96, 60);
        if (i < WALLPAPER_PRESETS) draw_preset(i, tx, ty, 96, 60, 0);
        else {
            gfx_fill(tx, ty, 96, 60, 0x374151);
            gfx_fill(tx + 30, ty + 14, 36, 28, 0xF9FAFB);
            gfx_fill(tx + 32, ty + 16, 32, 24, 0x60A5FA);
            for (int k = 0; k < 10; k++) gfx_fill(tx + 36 + k * 2, ty + 38 - k, 2, k, 0x16A34A);
            gfx_circle(tx + 56, ty + 22, 3, 0xFDE047, 1);
        }
        gfx_clip(x, y, w, h);
        int n = 0; while (preset_names[i][n]) n++;
        gfx_text(tx + 48 - n * 4, ty + 64, preset_names[i], sel ? C_ACCENT : C_MUTED, sel);
    }
    small_button(x + 16, y + 120, 190, "Use a picture...", hv == SH_PIC, 0);
    if (cfg.picture[0]) {
        p = cat(b, "Current: "); p = cat(p, base_name(cfg.picture)); (void)p;
        b[50] = 0;
        gfx_text(x + 220, y + 124, b, C_MUTED, 0);
    } else gfx_text(x + 220, y + 124, "from C:\\Pictures (png jpg bmp)", C_MUTED, 0);
    gfx_fill(x + 16, y + 156, w - 32, 1, 0xE5E7EB);
    /* accent colour */
    gfx_text(x + 16, y + 166, "Accent colour", C_TEXT, 1);
    for (int i = 0; i < ACCENT_COUNT; i++) {
        int cx = x + 30 + i * 46, cy = y + 206;
        if (cfg.accent == i) gfx_circle(cx, cy, 17, C_TEXT, 1);
        else if (hv == SH_ACC + i) gfx_circle(cx, cy, 17, 0xD1D5DB, 1);
        gfx_circle(cx, cy, cfg.accent == i ? 14 : 15, C_WHITE, 1);
        gfx_circle(cx, cy, 13, accents[i][0], 1);
    }
    gfx_text(x + 300, y + 198, accent_names[cfg.accent], C_MUTED, 0);
    gfx_fill(x + 16, y + 232, w - 32, 1, 0xE5E7EB);
    /* time zone */
    gfx_text(x + 16, y + 240, "Time zone", C_TEXT, 1);
    small_button(x + 16, y + 262, 32, "-", hv == SH_TZM, 0);
    p = cat(b, "UTC");
    if (cfg.timezone) { *p++ = cfg.timezone > 0 ? '+' : '-'; p = utoa(p, cfg.timezone > 0 ? cfg.timezone : -cfg.timezone); }
    gfx_text(x + 104 - (int)(p - b) * 4, y + 266, b, C_TEXT, 1);
    small_button(x + 160, y + 262, 32, "+", hv == SH_TZP, 0);
    int hh, mm, ss; rtc_time(&hh, &mm, &ss);
    char t[16] = "Now 00:00"; two_digits(t + 4, hh); two_digits(t + 7, mm);
    gfx_text(x + 212, y + 266, t, C_MUTED, 0);
    gfx_fill(x + 16, y + 302, w - 32, 1, 0xE5E7EB);
    /* sound */
    gfx_text(x + 16, y + 310, "Sound", C_TEXT, 1);
    small_button(x + 16, y + 332, 32, "-", hv == SH_VOLM, 0);
    p = utoa(b, cfg.volume); cat(p, "%");
    gfx_text(x + 88 - (int)(p - b + 1) * 4, y + 336, b, C_TEXT, 1);
    small_button(x + 128, y + 332, 32, "+", hv == SH_VOLP, 0);
    gfx_rect(x + 260, y + 336, 16, 16, cfg.chime ? C_ACCENT : 0x9CA3AF);
    if (cfg.chime) { gfx_fill(x + 262, y + 338, 12, 12, C_ACCENT); gfx_thick_line(x + 264, y + 344, x + 267, y + 347, 1, C_WHITE); gfx_thick_line(x + 267, y + 347, x + 272, y + 340, 1, C_WHITE); }
    gfx_text(x + 284, y + 336, "Start-up chime", hv == SH_CHIME ? C_ACCENT : C_TEXT, 0);
    gfx_text(x + 16, y + 360, sound_card_present() ? "Volume is for music (Sound Blaster)." : "No sound card: volume has no effect.", C_MUTED, 0);
    gfx_fill(x + 16, y + 382, w - 32, 1, 0xE5E7EB);
    /* mouse */
    gfx_text(x + 16, y + 386, "Mouse speed", C_TEXT, 1);
    for (int i = 0; i < 4; i++) small_button(x + 16 + i * 100, y + 402 + 4, 92, speed_names[i], hv == SH_SPEED + i, cfg.mouse == i + 1);
    gfx_fill(x, y + h - 30, w, 30, 0xF3F4F6);
    gfx_text(x + 16, y + h - 23, fs_mounted() ? "Changes are saved to C:\\settings.ini right away." : "No disk: changes last until restart.", C_MUTED, 0);
}

/* ---------- windows ---------- */
/* ---------- resizing, maximising and snapping ---------- */
void wm_max_client(int *w, int *h) { *w = gfx_w - 2; *h = gfx_h - TASKBAR_H - TITLE_H - 1; }
static int win_resizable(int a, int *minw, int *minh) {
    int mw, mh; if (!minw) minw = &mw; if (!minh) minh = &mh;
    if (a == APP_NOTEPAD) { *minw = 540; *minh = 260; return 1; }
    if (a == APP_EXPLORER) { *minw = 660; *minh = 330; return 1; }
    if (is_proc(a) && tasks[proc_slot(a)].resizable) {
        *minw = tasks[proc_slot(a)].min_w + 2; *minh = tasks[proc_slot(a)].min_h + TITLE_H + 1; return 1;
    }
    return 0;
}
/* The frame changed size: programs get a new canvas, and the frame follows the canvas. */
static void apply_size(int a) {
    struct window *w = &wins[a];
    if (is_proc(a)) {
        int slot = proc_slot(a);
        proc_set_size(slot, w->w - 2, w->h - TITLE_H - 1);
        w->w = tasks[slot].cw + 2; w->h = tasks[slot].ch + TITLE_H + 1;
    }
    dirty = 1;
}
static void remember_place(struct window *w) {
    if (!w->maxed && !w->snapped) { w->rx = w->x; w->ry = w->y; w->rw = w->w; w->rh = w->h; }
}
static void maximise(int a, int on) {
    struct window *w = &wins[a];
    if (on) {
        remember_place(w);
        w->maxed = 1; w->snapped = 0;
        w->x = 0; w->y = 0; w->w = gfx_w; w->h = gfx_h - TASKBAR_H;
    } else if (w->maxed || w->snapped) {
        w->maxed = w->snapped = 0;
        w->x = w->rx; w->y = w->ry; w->w = w->rw; w->h = w->rh;
    }
    apply_size(a);
}
static void snap_half(int a, int right) {
    struct window *w = &wins[a];
    remember_place(w);
    w->maxed = 0; w->snapped = 1;
    w->x = right ? gfx_w / 2 : 0; w->y = 0; w->w = gfx_w / 2; w->h = gfx_h - TASKBAR_H;
    int mw, mh; win_resizable(a, &mw, &mh);
    if (w->w < mw) { w->w = mw; if (right) w->x = gfx_w - mw; }
    apply_size(a);
}
/* Is (px, py) on the edge of a window that can be resized? Returns the edges (1 left, 2 right, 4 top, 8 bottom). */
static int edge_at(int px, int py, int *app) {
    for (int i = nwin - 1; i >= 0; i--) {
        int a = zorder[i];
        struct window *w = &wins[a];
        if (w->minimized) continue;
        int can = win_resizable(a, 0, 0) && !w->maxed;
        int g = can ? 4 : 0;                                   /* the grab zone reaches a little outside */
        if (px < w->x - g || px >= w->x + w->w + g || py < w->y - g || py >= w->y + w->h + g) continue;
        if (!can) return 0;
        int m = 0, c = 12;                                     /* corners are easier to hit */
        if (px < w->x + 4) m |= 1;
        if (px >= w->x + w->w - 4) m |= 2;
        if (py < w->y + 3) m |= 4;
        if (py >= w->y + w->h - 4) m |= 8;
        if (m & 3 && py >= w->y + w->h - c) m |= 8;
        if (m & 3 && py < w->y + c) m |= 4;
        if (m & 12 && px < w->x + c) m |= 1;
        if (m & 12 && px >= w->x + w->w - c) m |= 2;
        if (m) { *app = a; return m; }
        return 0;
    }
    return 0;
}
static int edge_shape(int m) {
    if ((m & 5) == 5 || (m & 10) == 10) return 3;              /* \ corners */
    if ((m & 6) == 6 || (m & 9) == 9) return 4;                /* / corners */
    if (m & 3) return 1;
    if (m & 12) return 2;
    return 0;
}
static void resize_to_mouse(void) {
    struct window *w = &wins[resizing];
    int minw, minh; win_resizable(resizing, &minw, &minh);
    int dx = mx - rs_mx, dy = my - rs_my, x = rs_x, y = rs_y, ww = rs_w, hh = rs_h;
    if (rs_mask & 2) ww = rs_w + dx;
    if (rs_mask & 8) hh = rs_h + dy;
    if (rs_mask & 1) { ww = rs_w - dx; x = rs_x + dx; }
    if (rs_mask & 4) { hh = rs_h - dy; y = rs_y + dy; }
    if (ww < minw) { if (rs_mask & 1) x -= minw - ww; ww = minw; }
    if (hh < minh) { if (rs_mask & 4) y -= minh - hh; hh = minh; }
    if (ww > gfx_w) ww = gfx_w;
    if (hh > gfx_h - TASKBAR_H) hh = gfx_h - TASKBAR_H;
    if (y < 0) { hh += y; y = 0; }
    if (y + hh > gfx_h - TASKBAR_H) hh = gfx_h - TASKBAR_H - y;
    w->x = x; w->y = y; w->w = ww; w->h = hh;
    w->snapped = 0;
    dirty = 1;
}

static void draw_window(int app, int is_focused) {
    struct window *wd = &wins[app];
    int x = wd->x, y = wd->y, w = wd->w, h = wd->h;
    gfx_noclip();
    int sizable = win_resizable(app, 0, 0);
    if (!wd->maxed) {
        gfx_shade(x + w, y + 8, 6, h, 70);                           /* drop shadow: right edge */
        gfx_shade(x + 6, y + h, w - 6, 8, 70);                       /* and bottom edge */
    }
    gfx_fill(x, y, w, h, C_BORDER);
    if (is_focused) gfx_vgradient(x + 1, y + 1, w - 2, TITLE_H - 1, C_ACCENT, C_ACCENT2);
    else gfx_fill(x + 1, y + 1, w - 2, TITLE_H - 1, C_TITLE_OFF);
    uint32_t tc = is_focused ? C_WHITE : C_TEXT;
    icon(app, x + 7, y + 5, 20);
    char title[80];
    if (app == APP_NOTEPAD) { char *p = cat(title, note_path[0] ? base_name(note_path) : "Untitled"); if (note_modified) p = cat(p, "*"); cat(p, " - Notepad"); }
    else if (app == APP_EXPLORER) { char *p = cat(title, ex_in_bin ? "Recycle Bin" : ex_path[1] ? base_name(ex_path) : "C:"); cat(p, " - File Explorer"); }
    else if (is_proc(app)) kcpy(title, tasks[proc_slot(app)].title, sizeof title);
    else kcpy(title, app_titles[app], sizeof title);
    gfx_clip(x, y, w - (sizable ? 142 : 96), TITLE_H);
    gfx_text(x + 34, y + 7, title, tc, 1);
    gfx_noclip();
    /* caption buttons: minimise, (maximise / restore), close */
    int bx = x + w - (sizable ? 138 : 92);
    if (hover_key == app * 4 + 1) gfx_fill(bx + 1, y + 1, 45, TITLE_H - 1, is_focused ? C_ACCENT_HI : 0xD1D5DB);
    gfx_fill(bx + 17, y + 16, 12, 2, tc);
    bx += 46;
    if (sizable) {
        if (hover_key == app * 4 + 3) gfx_fill(bx, y + 1, 45, TITLE_H - 1, is_focused ? C_ACCENT_HI : 0xD1D5DB);
        if (wd->maxed || wd->snapped) {                              /* restore: two squares */
            gfx_fill(bx + 20, y + 8, 9, 1, tc); gfx_fill(bx + 28, y + 8, 1, 9, tc);
            gfx_fill(bx + 17, y + 11, 9, 1, tc); gfx_fill(bx + 17, y + 19, 9, 1, tc);
            gfx_fill(bx + 17, y + 11, 1, 9, tc); gfx_fill(bx + 25, y + 11, 1, 9, tc);
        } else {                                                     /* maximise: one square */
            gfx_fill(bx + 17, y + 9, 11, 1, tc); gfx_fill(bx + 17, y + 19, 11, 1, tc);
            gfx_fill(bx + 17, y + 9, 1, 11, tc); gfx_fill(bx + 27, y + 9, 1, 11, tc);
        }
        bx += 46;
    }
    if (hover_key == app * 4 + 2) gfx_fill(bx, y + 1, 45, TITLE_H - 1, C_CLOSE_HOT);
    uint32_t xc = hover_key == app * 4 + 2 ? C_WHITE : tc;
    gfx_line(bx + 17, y + 9, bx + 28, y + 20, xc); gfx_line(bx + 18, y + 9, bx + 29, y + 20, xc);
    gfx_line(bx + 28, y + 9, bx + 17, y + 20, xc); gfx_line(bx + 29, y + 9, bx + 18, y + 20, xc);

    int cx = x + 1, cy = y + TITLE_H, cw = w - 2, ch = h - TITLE_H - 1;
    gfx_clip(cx, cy, cw, ch);
    switch (app) {
    case APP_TERMINAL: draw_terminal(cx, cy, cw, ch); break;
    case APP_NOTEPAD:  draw_notepad(cx, cy, cw, ch);  break;
    case APP_CLOCK:    draw_clock(cx, cy, cw, ch);    break;
    case APP_SYSINFO:  draw_sysinfo(cx, cy, cw, ch);  break;
    case APP_ABOUT:    draw_about(cx, cy, cw, ch);    break;
    case APP_EXPLORER: draw_explorer(cx, cy, cw, ch); break;
    case APP_TASKMGR:  draw_taskmgr(cx, cy, cw, ch);  break;
    case APP_MSG:      draw_msg(cx, cy, cw, ch);      break;
    case APP_SETTINGS: draw_settings(cx, cy, cw, ch); break;
    default: {
        struct task *t = &tasks[proc_slot(app)];
        if (t->cw < cw || t->ch < ch) gfx_fill(cx, cy, cw, ch, t->bg);   /* being resized: the program catches up on release */
        if (t->canvas) gfx_blit(cx, cy, t->cw, t->ch, t->canvas);
        break; }
    }
    gfx_noclip();
}

/* ---------- taskbar & start menu ---------- */
#define START_W 300
#define START_ITEM_H 35
static const char *start_items[] = { "File Explorer", "Web", "Terminal", "Notepad", "Paint", "Music", "Pictures", "Calculator", "Snake", "Minesweeper", "Solitaire", "FreeCell", "Task Manager", "This PC", "Settings", "All programs (C:\\Apps)", "", "Restart", "Shut down" };
static const int start_apps[] = { APP_EXPLORER, -15, APP_TERMINAL, APP_NOTEPAD, -10, -13, -14, -16, -11, -12, -17, -18, APP_TASKMGR, APP_SYSINFO, APP_SETTINGS, -4, -1, -2, -3 };
static const char *start_programs[] = { "/Apps/paint.elf", "/Apps/snake.elf", "/Apps/mines.elf", "/Apps/music.elf", "/Apps/viewer.elf", "/Apps/web.elf", "/Apps/calc.elf", "/Apps/solitaire.elf", "/Apps/freecell.elf" };
#define NSTART 19
#define START_SEP 16
static int start_h(void) { return 64 + NSTART * START_ITEM_H - 24; }
static int start_item_y(int i) { int y0 = gfx_h - TASKBAR_H - start_h() + 64; return y0 + i * START_ITEM_H - (i > START_SEP ? 24 : 0); }

static void draw_logo(int x, int y, uint32_t a, uint32_t b) {      /* MyOS mark: two overlapping rings */
    gfx_circle(x + 8, y + 10, 7, a, 0);
    gfx_circle(x + 15, y + 10, 7, b, 0);
}

static void taskbar_button_rect(int slot, int *x, int *w) { *x = 112 + slot * 128; *w = 124; }

static void draw_taskbar(void) {
    int y = gfx_h - TASKBAR_H;
    gfx_fill(0, y, gfx_w, TASKBAR_H, C_TASKBAR);
    gfx_fill(0, y, gfx_w, 1, 0x374151);
    /* Start button */
    if (start_open || hover_key == 100) gfx_rfill(4, y + 4, 100, TASKBAR_H - 8, 6, C_TASK_HI);
    draw_logo(14, y + 12, 0x60A5FA, 0xF472B6);
    gfx_text(44, y + 14, "Start", C_WHITE, 1);
    /* one button per open window, in the order they were opened */
    int slot = 0;
    for (int a = 0; a < NWIN; a++) {
        if (!find(a)) continue;
        int bx, bw; taskbar_button_rect(slot, &bx, &bw);
        if (bx + bw > gfx_w - 100) break;                     /* no more room */
        int active = focused() == a;
        if (active || hover_key == 200 + a) gfx_rfill(bx, y + 4, bw, TASKBAR_H - 8, 6, C_TASK_HI);
        icon(a, bx + 8, y + 12, 20);
        gfx_clip(bx, y, bw - 6, TASKBAR_H);
        gfx_text(bx + 34, y + 14, is_proc(a) ? tasks[proc_slot(a)].name : app_titles[a], wins[a].minimized ? 0x9CA3AF : C_WHITE, 0);
        gfx_noclip();
        gfx_rfill(bx + bw / 2 - (active ? 14 : 4), y + TASKBAR_H - 7, active ? 28 : 8, 3, 1, active ? C_ACCENT_HI : 0x9CA3AF);
        slot++;
    }
    /* clock and date */
    int hh, mm, ss, d, mo, yr; rtc_time(&hh, &mm, &ss); rtc_date(&d, &mo, &yr);
    char t[6] = "00:00", dt[11] = "00/00/2000";
    two_digits(t, hh); two_digits(t + 3, mm);
    two_digits(dt, d); two_digits(dt + 3, mo); two_digits(dt + 8, yr % 100);
    gfx_text(gfx_w - 64, y + 5, t, C_WHITE, 1);
    gfx_text(gfx_w - 88, y + 22, dt, 0xD1D5DB, 0);
}

static void draw_start_menu(void) {
    int h = start_h(), x = 4, y = gfx_h - TASKBAR_H - h - 4;
    gfx_shade(x + START_W, y + 8, 6, h, 80);
    gfx_shade(x + 6, y + h, START_W - 6, 8, 80);
    gfx_rfill(x, y, START_W, h, 8, 0xF9FAFB);
    gfx_vgradient(x, y, START_W, 56, C_ACCENT2, C_ACCENT);
    gfx_circle(x + 30, y + 28, 16, 0xDBEAFE, 1);
    gfx_text(x + 22, y + 20, "U", C_ACCENT2, 1);
    gfx_text(x + 56, y + 12, "User", C_WHITE, 1);
    gfx_text(x + 56, y + 30, "MyOS desktop", 0xBFDBFE, 0);
    for (int i = 0; i < NSTART; i++) {
        if (!*start_items[i]) { gfx_fill(x + 12, start_item_y(i) + 6, START_W - 24, 1, 0xD1D5DB); continue; }
        int iy = start_item_y(i);
        if (start_hover == i) gfx_rfill(x + 6, iy, START_W - 12, START_ITEM_H - 4, 6, C_ACCENT_LIGHT);
        if (start_apps[i] >= 0) icon(start_apps[i], x + 16, iy + 6, 20);
        else if (start_apps[i] == -4 || start_apps[i] <= -10) icon(APP_COUNT, x + 16, iy + 6, 20);
        else {                                                    /* power icons */
            uint32_t pc = start_apps[i] == -3 ? 0xDC2626 : 0xD97706;
            gfx_circle(x + 26, iy + 16, 8, pc, 0);
            gfx_fill(x + 25, iy + 5, 3, 9, pc);
        }
        gfx_text(x + 46, iy + 8, start_items[i], C_TEXT, start_hover == i);
    }
}

/* a desktop file or folder, 40 x 40 */
static void desk_glyph(int x, int y, struct fs_entry *e) {
    if (e->is_dir) {
        gfx_rfill(x, y + 4, 17, 8, 3, 0xD4A017);
        gfx_rfill(x, y + 8, 40, 28, 4, 0xE8B931);
        gfx_rfill(x, y + 12, 40, 24, 4, 0xF5C542);
        return;
    }
    uint32_t band = 0x6B7280;
    if (is_elf(e->name)) band = C_ACCENT;
    else if (image_is_picture_name(e->name)) band = 0x7C3AED;
    else if (has_ext(e->name, "wav")) band = 0xDB2777;
    else if (has_ext(e->name, "txt")) band = 0x059669;
    gfx_rfill(x + 6, y, 28, 38, 3, 0x9CA3AF);
    gfx_rfill(x + 7, y + 1, 26, 36, 2, C_WHITE);
    gfx_fill(x + 25, y + 1, 8, 8, 0xE5E7EB);                   /* folded corner */
    for (int i = 0; i < 3; i++) gfx_fill(x + 11, y + 11 + i * 5, 18 - (i == 2 ? 6 : 0), 1, 0xD1D5DB);
    gfx_rfill(x + 9, y + 27, 22, 7, 2, band);
}
static void shadow_text(int x, int y, const char *t) {
    gfx_text(x + 1, y + 1, t, 0x000000, 0); gfx_text(x, y, t, C_WHITE, 0);
}
static void desk_label(int x, int y, const char *name) {      /* up to two centred lines of 10 */
    int len = kstrlen_(name), cut = len > 10 ? 10 : len;
    if (len > 10) for (int k = 10; k > 0; k--) if (name[k] == ' ') { cut = k; break; }   /* break at a space */
    char l1[12], l2[12]; kcpy(l1, name, cut + 1);
    shadow_text(x + 40 - kstrlen_(l1) * 4, y + 4, l1);
    if (len > cut) {
        const char *r = name + cut + (name[cut] == ' ');
        int rl = kstrlen_(r);
        if (rl > 10) { kcpy(l2, r, 9); l2[8] = '.'; l2[9] = '.'; l2[10] = 0; } else kcpy(l2, r, 11);
        shadow_text(x + 40 - kstrlen_(l2) * 4, y + 18, l2);
    }
}

/* the bin: a grey can, with crumpled paper on top when something is inside */
static void bin_glyph(int x, int y, int full, int hot) {
    uint32_t body = hot ? 0x93C5FD : 0xD1D5DB, edge = hot ? 0x2563EB : 0x6B7280;
    if (full) {
        gfx_rfill(x + 8, y + 1, 12, 9, 3, 0xFFFFFF); gfx_rfill(x + 18, y - 1, 12, 10, 3, 0xF3F4F6);
        gfx_rfill(x + 24, y + 2, 9, 8, 3, 0xE5E7EB);
    }
    gfx_fill(x + 4, y + 9, 32, 4, edge);                       /* lid */
    gfx_fill(x + 15, y + 6, 10, 3, edge);
    gfx_fill(x + 7, y + 13, 26, 27, edge);                     /* can */
    gfx_fill(x + 8, y + 13, 24, 26, body);
    for (int k = 0; k < 3; k++) gfx_fill(x + 13 + k * 6, y + 17, 2, 18, edge);
}

/* ---------- right-click menus ---------- */
#define MENU_MAX 14
#define MENU_ROW 26
#define MENU_SEP 9
struct menu_item { const char *label; int id; int dim; };     /* id 0 = a separator line */
static struct menu_item menu[MENU_MAX];
static int menu_n, menu_on, menu_x, menu_y, menu_w, menu_hot = -1;
static int menu_arg;                                           /* the window or icon the menu is about */
static int menu_h(void) { int h = 8; for (int i = 0; i < menu_n; i++) h += menu[i].id ? MENU_ROW : MENU_SEP; return h; }
static void menu_add(const char *label, int id, int dim) {
    if (menu_n < MENU_MAX) menu[menu_n++] = (struct menu_item){ label, id, dim };
}
static void menu_start(void) { menu_n = 0; menu_hot = -1; }
static void menu_show(int x, int y) {                          /* open at the pointer, kept on screen */
    menu_w = 180;
    for (int i = 0; i < menu_n; i++) { int w = 8 * (int)kstrlen_(menu[i].label) + 48; if (w > menu_w) menu_w = w; }
    int h = menu_h();
    if (x + menu_w > gfx_w - 2) x = gfx_w - 2 - menu_w;
    if (y + h > gfx_h - TASKBAR_H) y = y - h > 0 ? y - h : gfx_h - TASKBAR_H - h;
    menu_x = x; menu_y = y; menu_on = 1; dirty = 1;
}
static int menu_at(int px, int py) {                           /* which row is under the pointer */
    if (!menu_on || !inside(px, py, menu_x, menu_y, menu_w, menu_h())) return -1;
    int y = menu_y + 4;
    for (int i = 0; i < menu_n; i++) {
        int rh = menu[i].id ? MENU_ROW : MENU_SEP;
        if (py >= y && py < y + rh) return menu[i].id && !menu[i].dim ? i : -2;
        y += rh;
    }
    return -2;                                                 /* on the menu, but not on an item */
}
static void draw_menu(void) {
    int h = menu_h();
    gfx_noclip();
    gfx_rfill(menu_x + 3, menu_y + 4, menu_w, h, 7, 0x6B7280);
    gfx_rfill(menu_x, menu_y, menu_w, h, 7, 0xD1D5DB);
    gfx_rfill(menu_x + 1, menu_y + 1, menu_w - 2, h - 2, 6, C_WHITE);
    int y = menu_y + 4;
    for (int i = 0; i < menu_n; i++) {
        if (!menu[i].id) { gfx_fill(menu_x + 10, y + 4, menu_w - 20, 1, 0xE5E7EB); y += MENU_SEP; continue; }
        if (i == menu_hot) gfx_rfill(menu_x + 4, y, menu_w - 8, MENU_ROW - 2, 4, C_ACCENT_LIGHT);
        gfx_text(menu_x + 18, y + 4, menu[i].label, menu[i].dim ? 0xB0B5BD : C_TEXT, 0);
        y += MENU_ROW;
    }
}

/* ---------- drag and drop from File Explorer ---------- */
static int dd_target(char *label, char *dest);
static int dd_over_bin(void) {
    if (!dd_on) return 0;
    char l[64], d[FS_PATH_MAX]; return dd_target(l, d) == 3;
}

/* ---------- full redraw ---------- */
static void render(void) {
    gfx_restore(wallpaper);
    for (int i = 0; i < nicons(); i++) {
        int x, y; icon_rect(i, &x, &y);
        int target = dd_on && hover_key == 300 + i && i >= NICONS && desk_list[i - NICONS].is_dir;
        if (sel_icon == i || target) gfx_rfill(x - 4, y - 4, 88, 84, 6, C_ACCENT_HI);
        else if (hover_key == 300 + i) gfx_rfill(x - 4, y - 4, 88, 84, 6, 0x2A5FA8);
        const char *t;
        if (i >= NICONS) {                                     /* a file or folder in C:\Desktop */
            struct fs_entry *e = &desk_list[i - NICONS];
            int cut = 0;
            if (ex_clip_cut) { char p[FS_PATH_MAX]; desk_path(i, p); cut = in_clip(p); }
            desk_glyph(x + 20, y + 4, e);
            if (cut) gfx_tint(x + 20, y + 4, 40, 40, 0x6B7280, 120);
            desk_label(x, y + 48, e->name);
            continue;
        }
        if (desk_icons[i] == DESK_BIN) { bin_glyph(x + 20, y + 4, bin_items() > 0, dd_over_bin()); t = "Recycle Bin"; }
        else { icon(desk_icons[i], x + 20, y + 4, 40); t = app_titles[desk_icons[i]]; }
        int tw = 0; while (t[tw]) tw++;
        gfx_text(x + 40 - tw * 4 + 1, y + 53, t, 0x000000, 0);   /* text shadow */
        gfx_text(x + 40 - tw * 4, y + 52, t, C_WHITE, 0);
    }
    int f = focused();
    for (int i = 0; i < nwin; i++)
        if (!wins[zorder[i]].minimized) draw_window(zorder[i], zorder[i] == f);
    if (drag >= 0 && snap_zone) {                              /* where the window will go */
        int minw, minh; win_resizable(drag, &minw, &minh);
        int w = snap_zone == 1 ? gfx_w : (gfx_w / 2 > minw ? gfx_w / 2 : minw), h = gfx_h - TASKBAR_H;
        int x = snap_zone == 3 ? gfx_w - w : 0;
        gfx_noclip();
        gfx_tint(x + 8, 8, w - 16, h - 16, 0xFFFFFF, 90);
        gfx_rfill(x + 8, 8, w - 16, 3, 1, C_ACCENT); gfx_rfill(x + 8, h - 11, w - 16, 3, 1, C_ACCENT);
        gfx_rfill(x + 8, 8, 3, h - 16, 1, C_ACCENT); gfx_rfill(x + w - 11, 8, 3, h - 16, 1, C_ACCENT);
    }
    draw_taskbar();
    if (start_open) draw_start_menu();
    if (menu_on) draw_menu();
    if (dd_on) {                                               /* the label that follows the pointer */
        char label[64], dest[FS_PATH_MAX], t[96];
        int kind = dd_target(label, dest);
        char nm[28];
        if (dd_n == 1) kcpy(nm, base_name(dd_paths[0]), 28); else { char *q = utoa(nm, dd_n); cat(q, " items"); }
        char *q = cat(t, nm);
        if (kind) { q = cat(q, "  ->  "); cat(q, label); }
        int tw = 8 * (int)kstrlen_(t) + 16;
        gfx_noclip();
        int gx = mx + 14, gy = my + 18;
        if (gx + tw > gfx_w) gx = gfx_w - tw;
        gfx_rfill(gx, gy, tw, 22, 5, kind ? C_ACCENT : 0x4B5563);
        gfx_text(gx + 8, gy + 3, t, C_WHITE, 0);
    }
    gfx_present();
    gfx_cursor_move(mx, my);
}

/* ---------- hit testing ---------- */
static int hit_window(int px, int py) {
    for (int i = nwin - 1; i >= 0; i--) {
        struct window *w = &wins[zorder[i]];
        if (!w->minimized && inside(px, py, w->x, w->y, w->w, w->h)) return zorder[i];
    }
    return -1;
}

/* Where would the dragged item go if dropped now?
   0 nowhere, 1 into a folder (dest), 3 the Recycle Bin, 4 Notepad */
static int dd_all_in(const char *dir) {                       /* is everything dragged already in dir? */
    for (int k = 0; k < dd_n; k++) {
        char par[FS_PATH_MAX]; path_resolve(dd_paths[k], "..", par);
        if (kstrcmp(par, dir)) return 0;
    }
    return 1;
}
static int dd_target(char *label, char *dest) {
    int a = hit_window(mx, my);
    if (a == APP_EXPLORER && !ex_in_bin) {
        int j = hover_key - 1200;
        int ctrl = kbd_ctrl_down();
        if (j >= 0 && j < ex_n && ex_list[j].is_dir) {
            path_resolve(ex_path, ex_list[j].name, dest);
            for (int k = 0; k < dd_n; k++) if (!kstrcmp(dest, dd_paths[k])) return 0;   /* onto itself */
            char *q = cat(label, ctrl ? "Copy to " : "Move to "); kcpy(q, ex_list[j].name, 24);
            return 1;
        }
        if (hover_key == 1100 && ex_path[1]) {                 /* the Up button: the folder above */
            path_resolve(ex_path, "..", dest);
            char *q = cat(label, ctrl ? "Copy to " : "Move to "); kcpy(q, dest[1] ? base_name(dest) : "C:", 24);
            return 1;
        }
        struct window *wn = &wins[APP_EXPLORER];               /* anywhere else in the file area: this folder */
        int ly = my - wn->y - TITLE_H, ch = wn->h - TITLE_H - 1;
        if (ly >= (cfg.explorer_view ? 70 : EX_TOP) && ly < ch - 22 && !dd_all_in(ex_path)) {
            kcpy(dest, ex_path, FS_PATH_MAX);
            char *q = cat(label, ctrl ? "Copy to " : "Move to "); kcpy(q, ex_path[1] ? base_name(ex_path) : "C:", 24);
            return 1;
        }
        return 0;
    }
    if (a == APP_NOTEPAD && dd_n == 1 && fs_stat(dd_paths[0], 0) == 1) { cat(label, "Open in Notepad"); return 4; }
    if (a >= 0 || my >= gfx_h - TASKBAR_H) return 0;
    int ctrl = kbd_ctrl_down(), i = hover_key - 300;
    if (i >= 0 && i < NICONS && desk_icons[i] == DESK_BIN) { cat(label, "Move to the Recycle Bin"); return 3; }
    if (i >= NICONS && i < nicons()) {                         /* a folder on the desktop */
        if (!desk_list[i - NICONS].is_dir) return 0;
        desk_path(i, dest);
        for (int k = 0; k < dd_n; k++) if (!kstrcmp(dest, dd_paths[k])) return 0;
        char *q = cat(label, ctrl ? "Copy to " : "Move to "); kcpy(q, desk_list[i - NICONS].name, 24);
        return 1;
    }
    if (i >= 0) return 0;
    if (dd_all_in(DESK_DIR)) return 0;                         /* already on the desktop */
    kcpy(dest, DESK_DIR, FS_PATH_MAX); cat(label, ctrl ? "Copy to the desktop" : "Move to the desktop");
    return 1;
}

static void dd_drop(void) {
    char label[64], dest[FS_PATH_MAX];
    int kind = dd_target(label, dest);
    char name[FS_NAME_MAX]; kcpy(name, base_name(dd_paths[0]), FS_NAME_MAX);
    ex_status[0] = 0;
    if (kind == 4) { wm_open_file(dd_paths[0]); return; }
    if (kind != 1 && kind != 3) return;
    int copy = kind == 1 && kbd_ctrl_down(), done = 0;
    const char *why = 0;
    for (int k = 0; k < dd_n; k++) {
        const char *src = dd_paths[k];
        if (kind == 3) { if (bin_put(src)) { done++; clip_forget(src); } continue; }
        if (fs_stat(src, 0) == 2 && is_inside(dest, src)) { why = "Can't put a folder inside itself"; continue; }
        char to[FS_PATH_MAX]; path_resolve(dest, base_name(src), to);
        if (fs_stat(to, 0)) { why = "There is already an item with this name there"; continue; }
        int ok = tree_copy(src, to, 0);
        if (ok && !copy) { ok = tree_delete(src, 0); clip_forget(src); }
        done += ok;
    }
    if (dd_n == 1) {
        if (kind == 3) ex_say(done ? "Moved to the Recycle Bin: " : "Could not delete ", name);
        else if (why) ex_say(why, 0);
        else ex_say(done ? (copy ? "Copied " : "Moved ") : "Could not move ", name);
    } else if (kind == 3) say_count("Moved to the Recycle Bin: ", done, done < dd_n ? " (some could not be moved)" : 0);
    else {
        say_count(copy ? "Copied " : "Moved ", done, 0);
        if (why) { ex_say(" - skipped: ", 0); ex_say(why, 0); }
    }
    ex_sel = -1; ex_unmark(); ex_refresh();
}

/* menu item ids */
enum { M_OPEN = 1, M_NOTEPAD, M_CUT, M_COPY, M_PASTE_INTO, M_DELETE, M_RENAME, M_PROPS,
       M_VIEW, M_NEWFOLDER, M_NEWFILE, M_PASTE, M_FOLDERPROPS,
       M_RESTORE, M_DELFOREVER, M_RESTOREALL, M_EMPTY,
       M_WRESTORE, M_WMIN, M_WMAX, M_WCLOSE,
       M_DESK_OPEN, M_DESK_EMPTY, M_EXPLORER, M_TERMINAL, M_SETTINGS, M_TASKMGR, M_ABOUT,
       M_N_UNDO, M_N_CUT, M_N_COPY, M_N_PASTE, M_N_ALL, M_N_FIND, M_T_PASTE,
       M_D_OPEN, M_D_CUT, M_D_COPY, M_D_PASTEINTO, M_D_DELETE, M_D_RENAME, M_D_PROPS,
       M_D_NEWFOLDER, M_D_NEWFILE, M_D_PASTE };

/* paste the clipboard into another folder; File Explorer stays where it was */
static void paste_into(const char *folder) {
    char keep[FS_PATH_MAX]; kcpy(keep, ex_path, FS_PATH_MAX);
    kcpy(ex_path, folder, FS_PATH_MAX); ex_n = 0; ex_refresh();
    ex_paste();
    char msg[80]; kcpy(msg, ex_status, 80);
    kcpy(ex_path, keep, FS_PATH_MAX); ex_n = 0; ex_sel = -1; ex_refresh();
    ex_status[0] = 0; ex_say(msg, 0);
}
/* File Explorer opened at C:\Desktop with one desktop item selected (for Rename and Properties) */
static void desk_explore(int i) {
    char name[FS_NAME_MAX]; kcpy(name, desk_list[i - NICONS].name, FS_NAME_MAX);
    open_app(APP_EXPLORER); ex_go(DESK_DIR); ex_select_name(name);
}
static void desk_clip(int i, int cut) {
    desk_path(i, ex_clip[0]); ex_nclip = 1; ex_clip_cut = cut; dirty = 1;
}
static void desk_delete(int i) {
    char p[FS_PATH_MAX]; desk_path(i, p);
    if (bin_put(p)) clip_forget(p);
    sel_icon = -1; dirty = 1;
}

static void window_menu(int a, int x, int y) {                 /* title bar / taskbar button */
    struct window *w = &wins[a];
    int rz = win_resizable(a, 0, 0), big = w->maxed || w->snapped;
    menu_start(); menu_arg = a;
    menu_add("Restore", M_WRESTORE, !(big || w->minimized));
    menu_add("Minimise", M_WMIN, w->minimized);
    menu_add("Maximise", M_WMAX, !rz || w->maxed);
    menu_add("", 0, 0);
    menu_add("Close        Alt+F4", M_WCLOSE, 0);
    menu_show(x, y);
}
static void explorer_menu_for(int item, int empty);
static void explorer_menu(int lx, int ly, int cw, int ch) {
    int hv = explorer_hover(lx, ly, cw, ch);
    explorer_menu_for(hv >= 1200 ? hv - 1200 : -1, ly >= (cfg.explorer_view ? 70 : EX_TOP) && ly < ch - 22);
}
static void explorer_menu_for(int item, int empty) {
    if (ex_props || ex_confirm) return;
    if (ex_renaming) ex_rename_commit();
    menu_start();
    if (item >= 0) {                                           /* on an item */
        int i = item;
        if (!ex_is_sel(i)) ex_only(i); else ex_sel = i;
        int n = ex_nsel(), dir = ex_list[i].is_dir;
        if (ex_in_bin) {
            menu_add(n > 1 ? "Restore all selected" : "Restore", M_RESTORE, 0);
            menu_add("Delete forever", M_DELFOREVER, 0);
        } else {
            menu_add("Open", M_OPEN, n > 1);
            if (!dir) menu_add("Open with Notepad", M_NOTEPAD, n > 1);
            menu_add("", 0, 0);
            menu_add("Cut          Ctrl+X", M_CUT, 0);
            menu_add("Copy         Ctrl+C", M_COPY, 0);
            if (dir) menu_add("Paste into folder", M_PASTE_INTO, !ex_nclip || n > 1);
            menu_add("", 0, 0);
            menu_add("Delete       Del", M_DELETE, 0);
            menu_add("Rename       F2", M_RENAME, n > 1);
        }
        menu_add("", 0, 0);
        menu_add("Properties   Alt+Enter", M_PROPS, 0);
    } else if (empty) {                                        /* on empty space */
        if (!kbd_ctrl_down()) { ex_unmark(); ex_sel = -1; }
        menu_add(cfg.explorer_view ? "View: List" : "View: Icons", M_VIEW, 0);
        menu_add("", 0, 0);
        if (ex_in_bin) {
            menu_add("Restore all", M_RESTOREALL, !ex_n);
            menu_add("Empty Recycle Bin", M_EMPTY, !ex_n);
        } else {
            menu_add("New folder   Ctrl+N", M_NEWFOLDER, 0);
            menu_add("New text file", M_NEWFILE, 0);
            menu_add("Paste        Ctrl+V", M_PASTE, !ex_nclip);
        }
        menu_add("", 0, 0);
        menu_add("Properties", M_FOLDERPROPS, 0);
    } else return;
    menu_show(mx, my);
}
static void desktop_menu(void) {
    menu_start();
    desk_focus = 1;
    for (int i = 0; i < nicons(); i++) {
        int x, y; icon_rect(i, &x, &y);
        if (!inside(mx, my, x - 4, y - 4, 88, 84)) continue;
        sel_icon = i; menu_arg = i;
        if (i >= NICONS) {                                     /* a file or folder in C:\Desktop */
            int dir = desk_list[i - NICONS].is_dir;
            menu_add("Open", M_D_OPEN, 0);
            menu_add("", 0, 0);
            menu_add("Cut          Ctrl+X", M_D_CUT, 0);
            menu_add("Copy         Ctrl+C", M_D_COPY, 0);
            if (dir) menu_add("Paste into folder", M_D_PASTEINTO, !ex_nclip);
            menu_add("", 0, 0);
            menu_add("Delete       Del", M_D_DELETE, 0);
            menu_add("Rename       F2", M_D_RENAME, 0);
            menu_add("", 0, 0);
            menu_add("Properties", M_D_PROPS, 0);
            menu_show(mx, my); return;
        }
        menu_add("Open", M_DESK_OPEN, 0);
        if (desk_icons[i] == DESK_BIN) { menu_add("", 0, 0); menu_add("Empty Recycle Bin", M_DESK_EMPTY, !bin_items()); }
        menu_show(mx, my); return;
    }
    sel_icon = -1;
    menu_add("New folder", M_D_NEWFOLDER, 0);
    menu_add("New text file", M_D_NEWFILE, 0);
    menu_add("Paste        Ctrl+V", M_D_PASTE, !ex_nclip);
    menu_add("", 0, 0);
    menu_add("File Explorer", M_EXPLORER, 0);
    menu_add("Terminal", M_TERMINAL, 0);
    menu_add("Task Manager", M_TASKMGR, 0);
    menu_add("", 0, 0);
    menu_add("Change wallpaper...", M_SETTINGS, 0);
    menu_add("About MyOS", M_ABOUT, 0);
    menu_show(mx, my);
}
static void notepad_menu(void) {
    menu_start();
    menu_add("Undo         Ctrl+Z", M_N_UNDO, 0);
    menu_add("", 0, 0);
    menu_add("Cut          Ctrl+X", M_N_CUT, !has_sel());
    menu_add("Copy         Ctrl+C", M_N_COPY, !has_sel());
    menu_add("Paste        Ctrl+V", M_N_PASTE, !clip_len());
    menu_add("", 0, 0);
    menu_add("Select all   Ctrl+A", M_N_ALL, 0);
    menu_add("Find...      Ctrl+F", M_N_FIND, 0);
    menu_show(mx, my);
}
static void menu_run(int id) {
    menu_on = 0; dirty = 1;
    int a = menu_arg;
    switch (id) {
    case M_OPEN: ex_open(ex_sel); break;
    case M_NOTEPAD: { char p[FS_PATH_MAX]; ex_child(ex_sel, p); wm_open_file(p); break; }
    case M_CUT: ex_copy(1); break;
    case M_COPY: ex_copy(0); break;
    case M_PASTE_INTO: {                                       /* paste inside the folder that was clicked */
        char keep[FS_PATH_MAX], name[FS_NAME_MAX]; kcpy(keep, ex_path, FS_PATH_MAX); kcpy(name, ex_list[ex_sel].name, FS_NAME_MAX);
        path_resolve(keep, name, ex_path);
        char msg[80]; ex_paste(); kcpy(msg, ex_status, 80);
        kcpy(ex_path, keep, FS_PATH_MAX); ex_n = 0; ex_refresh(); ex_select_name(name);
        ex_status[0] = 0; ex_say(msg, " (into ");
        { char *q = ex_status + kstrlen_(ex_status); q = cat(q, name); cat(q, ")"); }
        break; }
    case M_DELETE: ex_delete(); break;
    case M_RENAME: ex_rename_start(); break;
    case M_PROPS: ex_props_open(0); break;
    case M_FOLDERPROPS: ex_props_open(1); break;
    case M_VIEW: cfg.explorer_view = !cfg.explorer_view; settings_save(); break;
    case M_NEWFOLDER: ex_new(1); break;
    case M_NEWFILE: ex_new(0); break;
    case M_PASTE: ex_paste(); break;
    case M_RESTORE: explorer_click_button(1101); break;
    case M_DELFOREVER: ex_delete(); break;
    case M_RESTOREALL: explorer_click_button(1102); break;
    case M_EMPTY: ex_confirm_kind = 2; ex_confirm = 1; break;
    case M_WRESTORE:
        if (wins[a].maxed || wins[a].snapped) maximise(a, 0);
        focus(a); break;
    case M_WMIN: wins[a].minimized = 1; break;
    case M_WMAX: focus(a); maximise(a, 1); break;
    case M_WCLOSE: if (is_proc(a)) proc_kill(tasks[proc_slot(a)].pid); else close_app(a); break;
    case M_DESK_OPEN:
        if (desk_icons[a] == DESK_BIN) { open_app(APP_EXPLORER); ex_go(BIN); } else open_app(desk_icons[a]);
        sel_icon = -1; break;
    case M_DESK_EMPTY: open_app(APP_EXPLORER); ex_go(BIN); ex_confirm_kind = 2; ex_confirm = 1; break;
    case M_EXPLORER: open_app(APP_EXPLORER); break;
    case M_TERMINAL: open_app(APP_TERMINAL); break;
    case M_TASKMGR: open_app(APP_TASKMGR); break;
    case M_SETTINGS: open_app(APP_SETTINGS); break;
    case M_ABOUT: open_app(APP_ABOUT); break;
    case M_N_UNDO: notepad_key(KEY_CTRL('z')); break;
    case M_N_CUT: notepad_key(KEY_CTRL('x')); break;
    case M_N_COPY: notepad_key(KEY_CTRL('c')); break;
    case M_N_PASTE: notepad_key(KEY_CTRL('v')); break;
    case M_N_ALL: notepad_key(KEY_CTRL('a')); break;
    case M_N_FIND: notepad_key(KEY_CTRL('f')); break;
    case M_T_PASTE: focus(APP_TERMINAL); wm_key(KEY_CTRL('v')); break;
    case M_D_OPEN: { char p[FS_PATH_MAX]; desk_path(a, p); open_path(p, desk_list[a - NICONS].is_dir); sel_icon = -1; break; }
    case M_D_CUT: desk_clip(a, 1); break;
    case M_D_COPY: desk_clip(a, 0); break;
    case M_D_PASTEINTO: { char p[FS_PATH_MAX]; desk_path(a, p); paste_into(p); break; }
    case M_D_DELETE: desk_delete(a); break;
    case M_D_RENAME: desk_explore(a); ex_rename_start(); break;
    case M_D_PROPS: desk_explore(a); ex_props_open(0); break;
    case M_D_NEWFOLDER: open_app(APP_EXPLORER); ex_go(DESK_DIR); ex_new(1); break;
    case M_D_NEWFILE: open_app(APP_EXPLORER); ex_go(DESK_DIR); ex_new(0); break;
    case M_D_PASTE: paste_into(DESK_DIR); break;
    }
}
/* the right mouse button went down */
static void right_click(void) {
    if (start_open) { start_open = 0; dirty = 1; }
    if (my >= gfx_h - TASKBAR_H) {
        if (hover_key >= 200 && hover_key < 300) window_menu(hover_key - 200, mx, gfx_h - TASKBAR_H);
        return;
    }
    int a = hit_window(mx, my);
    if (a < 0) { desktop_menu(); return; }
    struct window *w = &wins[a];
    if (my < w->y + TITLE_H) { focus(a); window_menu(a, mx, my); return; }
    if (a >= APP_COUNT) return;                                /* programs get the click themselves */
    focus(a);
    int lx = mx - w->x - 1, ly = my - w->y - TITLE_H, cw = w->w - 2, ch = w->h - TITLE_H - 1;
    if (a == APP_EXPLORER) explorer_menu(lx, ly, cw, ch);
    else if (a == APP_NOTEPAD) notepad_menu();
    else if (a == APP_TERMINAL) { menu_start(); menu_add("Paste        Ctrl+V", M_T_PASTE, !clip_len()); menu_show(mx, my); }
}
/* keys while a menu is open */
static void menu_key(int k) {
    if (k == KEY_ESC) { menu_on = 0; dirty = 1; return; }
    if (k == KEY_DOWN || k == KEY_UP) {
        int d = k == KEY_DOWN ? 1 : -1, i = menu_hot;
        for (int t = 0; t < menu_n; t++) {
            i = i < 0 ? (d > 0 ? 0 : menu_n - 1) : (i + d + menu_n) % menu_n;
            if (menu[i].id && !menu[i].dim) break;
        }
        menu_hot = i; dirty = 1; return;
    }
    if (k == '\n' && menu_hot >= 0 && menu[menu_hot].id && !menu[menu_hot].dim) menu_run(menu[menu_hot].id);
}

static int compute_hover(void) {
    if (menu_on) { int r = menu_at(mx, my); if (r != -1) return r >= 0 ? 900 + r : 899; }
    if (start_open) {
        int x = 4;
        for (int i = 0; i < NSTART; i++)
            if (*start_items[i] && inside(mx, my, x, start_item_y(i), START_W, START_ITEM_H - 4)) return 400 + i;
    }
    if (my >= gfx_h - TASKBAR_H) {
        if (mx < 108) return 100;
        int slot = 0;
        for (int a = 0; a < NWIN; a++) {
            if (!find(a)) continue;
            int bx, bw; taskbar_button_rect(slot++, &bx, &bw);
            if (inside(mx, my, bx, gfx_h - TASKBAR_H, bw, TASKBAR_H)) return 200 + a;
        }
        return -1;
    }
    { int ea, em = edge_at(mx, my, &ea); if (em) return 5000 + ea * 16 + em; }
    int a = hit_window(mx, my);
    if (a >= 0) {
        struct window *w = &wins[a];
        if (my < w->y + TITLE_H) {
            int r = w->x + w->w - mx;
            if (r <= 46) return a * 4 + 2;
            if (win_resizable(a, 0, 0)) { if (r <= 92) return a * 4 + 3; if (r <= 138) return a * 4 + 1; }
            else if (r <= 92) return a * 4 + 1;
            return -1;
        }
        int lx = mx - w->x - 1, ly = my - w->y - TITLE_H, cw = w->w - 2, ch = w->h - TITLE_H - 1;
        if (a == APP_NOTEPAD) return notepad_hover(lx, ly);
        if (a == APP_EXPLORER) return explorer_hover(lx, ly, cw, ch);
        if (a == APP_TASKMGR) return taskmgr_hover(lx, ly, cw, ch);
        if (a == APP_SETTINGS) { int s = settings_hit(lx, ly); return s >= 0 ? 1600 + s : -1; }
        if (a == APP_MSG && ly >= ch - 36 && ly < ch - 12 && lx >= cw - 96 && lx < cw - 12) return 1300;
        return -1;
    }
    for (int i = 0; i < nicons(); i++) { int x, y; icon_rect(i, &x, &y); if (inside(mx, my, x - 4, y - 4, 88, 84)) return 300 + i; }
    return -1;
}

static void do_power(int kind) {
    gfx_noclip();
    gfx_fill(0, 0, gfx_w, gfx_h, 0x0B1E4A);
    gfx_text2x(gfx_w / 2 - (kind == -2 ? 104 : 120), gfx_h / 2 - 40, kind == -2 ? "Restarting" : "Shutting down", C_WHITE);
    gfx_present();
    sleep_ms(600);
    if (kind == -2) reboot();
    __asm__ volatile("outw %0,%1" :: "a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));
    __asm__ volatile("outw %0,%1" :: "a"((uint16_t)0x2000), "Nd"((uint16_t)0xB004));
    gfx_fill(0, 0, gfx_w, gfx_h, 0x000000);
    gfx_text2x(gfx_w / 2 - 248, gfx_h / 2 - 16, "It is now safe to turn off", 0xF59E0B);
    gfx_present();
    for (;;) __asm__ volatile("cli; hlt");
}

static void left_click(void) {
    /* Start menu */
    if (start_open) {
        int h = hover_key;
        if (h >= 400) {
            int it = start_apps[h - 400];
            start_open = 0; start_hover = -1; dirty = 1;
            if (it >= 0) open_app(it);
            else if (it == -4) wm_open_folder("/Apps");
            else if (it <= -10) run_program(start_programs[-10 - it]);
            else do_power(it);
            return;
        }
        if (!(my >= gfx_h - TASKBAR_H && mx < 108)) { start_open = 0; dirty = 1; }
    }
    /* Taskbar */
    if (my >= gfx_h - TASKBAR_H) {
        if (mx < 108) { start_open = !start_open; start_hover = -1; dirty = 1; return; }
        if (hover_key >= 200 && hover_key < 300) {
            int a = hover_key - 200;
            if (focused() == a) wins[a].minimized = 1; else focus(a);
            dirty = 1;
        }
        return;
    }
    /* Window edges: start resizing */
    if (hover_key >= 5000) {
        int ra = (hover_key - 5000) / 16;
        focus(ra);
        struct window *w = &wins[ra];
        resizing = ra; rs_mask = (hover_key - 5000) % 16; rs_mx = mx; rs_my = my;
        rs_x = w->x; rs_y = w->y; rs_w = w->w; rs_h = w->h;
        return;
    }
    /* Windows */
    int a = hit_window(mx, my);
    if (a >= 0) {
        focus(a);
        struct window *w = &wins[a];
        if (my < w->y + TITLE_H) {
            if (hover_key == a * 4 + 3) { maximise(a, !(w->maxed || w->snapped)); return; }
            if (hover_key < 0 && win_resizable(a, 0, 0) && title_click_app == a && ticks - title_click_tick < TIMER_HZ / 2) {
                title_click_app = -1; maximise(a, !(w->maxed || w->snapped)); return;   /* double-click the title */
            }
            title_click_app = a; title_click_tick = ticks;
            if (hover_key == a * 4 + 2) {                     /* X: closing a program ends it */
                if (is_proc(a)) proc_kill(tasks[proc_slot(a)].pid); else close_app(a);
                return;
            }
            if (hover_key == a * 4 + 1) { w->minimized = 1; dirty = 1; return; }
            drag = a; drag_dx = mx - w->x; drag_dy = my - w->y; snap_zone = 0;
            drag_unsnap = w->maxed || w->snapped;
        } else {
            int lx = mx - w->x - 1, ly = my - w->y - TITLE_H, cw = w->w - 2, ch = w->h - TITLE_H - 1;
            if (a == APP_NOTEPAD) notepad_click(lx, ly, cw);
            if (a == APP_EXPLORER) explorer_click(lx, ly, cw, ch);
            if (a == APP_TASKMGR) taskmgr_click(lx, ly, cw, ch);
            if (a == APP_SETTINGS) settings_click(lx, ly);
            if (a == APP_MSG && hover_key == 1300) { close_app(APP_MSG); return; }
            dirty = 1;
        }
        sel_icon = -1;
        return;
    }
    /* Desktop icons (double-click to open) */
    int old = sel_icon;
    sel_icon = -1; desk_focus = 1;
    for (int i = 0; i < nicons(); i++) {
        int x, y; icon_rect(i, &x, &y);
        if (inside(mx, my, x - 4, y - 4, 88, 84)) {
            sel_icon = i;
            if (last_click_icon == i && ticks - last_click_tick < TIMER_HZ / 2) {
                if (i >= NICONS) { char p[FS_PATH_MAX]; desk_path(i, p); open_path(p, desk_list[i - NICONS].is_dir); }
                else if (desk_icons[i] == DESK_BIN) { open_app(APP_EXPLORER); ex_go(BIN); }
                else open_app(desk_icons[i]);
                sel_icon = -1; last_click_icon = -1;
                break;
            }
            last_click_icon = i; last_click_tick = ticks;
            if (i >= NICONS) { dd_desk = 1; dd_item = i; dd_px = mx; dd_py = my; }   /* may become a drag */
        }
    }
    if (sel_icon != old || sel_icon < 0) dirty = 1;
}

/* ---------- public entry points ---------- */
void wm_init(void) {
    get_cpu_brand();
    apply_accent();
    build_wallpaper();
    mx = gfx_w / 2; my = gfx_h / 2;
    note_new();
    if (fs_mounted() && fs_stat(DESK_DIR, 0) != 2) fs_mkdir(DESK_DIR);   /* the desktop's own folder */
    ex_go("/");
    open_app(APP_TERMINAL);
    dirty = 1;
}

void wm_mouse(int dx, int dy, int buttons) {
    /* mouse speed (Settings): 1 = half speed ... 4 = double; leftovers carry over */
    static int accx, accy;
    accx += dx * cfg.mouse; accy += dy * cfg.mouse;
    dx = accx / 2; dy = accy / 2;
    accx -= dx * 2; accy -= dy * 2;
    mx += dx; my += dy;
    if (mx < 0) mx = 0;
    if (mx >= gfx_w) mx = gfx_w - 1;
    if (my < 0) my = 0;
    if (my >= gfx_h) my = gfx_h - 1;
    cursor_moved = 1;

    if (resizing >= 0 && (dx || dy)) resize_to_mouse();
    if (drag >= 0 && (dx || dy)) {
        struct window *w = &wins[drag];
        if (drag_unsnap) {                                     /* pulling a maximised window: back to its old size */
            int ox = drag_dx * 1000 / (w->w ? w->w : 1);
            w->maxed = w->snapped = 0;
            w->w = w->rw; w->h = w->rh;
            drag_dx = ox * w->w / 1000;
            if (drag_dy > TITLE_H - 2) drag_dy = TITLE_H / 2;
            drag_unsnap = 0;
            apply_size(drag);
        }
        w->x = mx - drag_dx; w->y = my - drag_dy;
        if (w->y < 0) w->y = 0;
        if (w->y > gfx_h - TASKBAR_H - TITLE_H) w->y = gfx_h - TASKBAR_H - TITLE_H;
        int z = 0;
        if (win_resizable(drag, 0, 0)) z = my <= 0 ? 1 : mx <= 0 ? 2 : mx >= gfx_w - 1 ? 3 : 0;
        snap_zone = z;
        dirty = 1;
    }
    int hv = compute_hover();
    if (hv != hover_key) {
        hover_key = hv; start_hover = hv >= 400 ? hv - 400 : -1; dirty = 1;
        if (menu_on && hv >= 900 && hv < 900 + MENU_MAX) menu_hot = hv - 900;
    }

    int pressed = buttons & ~mbuttons;
    int released = mbuttons & ~buttons;
    mbuttons = buttons;
    if (menu_on && pressed) {                                   /* a menu is open: it takes this click */
        if (hover_key >= 900 && hover_key < 900 + MENU_MAX) { if (pressed & 1) menu_run(menu[hover_key - 900].id); pressed = 0; }
        else if (hover_key == 899) pressed = 0;
        else { menu_on = 0; dirty = 1; if (pressed & 1) pressed = 0; }   /* clicked elsewhere: close (a right click opens a new one) */
    }
    if (pressed & 1) left_click();
    else if (pressed & 2) right_click();
    if (dd_item >= 0 && (buttons & 1) && !dd_on) {            /* moved far enough: it's a drag */
        int ddx = mx - dd_px, ddy = my - dd_py;
        if (ddx * ddx + ddy * ddy > 36) {
            dd_on = 1; ex_last_row = -1; ex_collapse = -1;
            dd_n = 0;                                          /* the whole selection goes, if the item is in it */
            if (dd_desk) desk_path(dd_item, dd_paths[dd_n++]);
            else if (ex_is_sel(dd_item)) { for (int i = 0; i < ex_n && dd_n < EX_CLIPS; i++) if (ex_is_sel(i)) ex_child(i, dd_paths[dd_n++]); }
            else ex_child(dd_item, dd_paths[dd_n++]);
        }
    }
    if (dd_on && (dx || dy)) dirty = 1;
    if (band_on && (buttons & 1) && (dx || dy) && find(APP_EXPLORER)) band_update();
    if (released & 1) {
        if (dd_on) dd_drop();
        else if (ex_collapse >= 0 && ex_collapse < ex_n) ex_only(ex_collapse);   /* a click, not a drag */
        if (band_on) band_end();
        dd_on = 0; dd_item = -1; ex_collapse = -1; dd_desk = 0;
        if (drag >= 0 && snap_zone) { if (snap_zone == 1) maximise(drag, 1); else snap_half(drag, snap_zone == 3); }
        if (resizing >= 0) apply_size(resizing);
        drag = -1; resizing = -1; snap_zone = 0; note_dragging = 0; dirty = 1;
    }
    { int shape = 0;                                           /* the pointer's shape */
      if (resizing >= 0) shape = edge_shape(rs_mask);
      else if (hover_key >= 5000) shape = edge_shape((hover_key - 5000) % 16);
      gfx_cursor_shape(shape); }
    if (note_dragging && (buttons & 1) && (dx || dy) && find(APP_NOTEPAD)) {
        struct window *w = &wins[APP_NOTEPAD];
        notepad_drag(mx - w->x - 1, my - w->y - TITLE_H);
    }

    /* ---- pass the mouse on to programs ---- */
    if (pressed && mcapture < 0 && !start_open && my < gfx_h - TASKBAR_H) {
        int a = hit_window(mx, my);
        if (a >= APP_COUNT && my >= wins[a].y + TITLE_H) {      /* pressed inside a program's area */
            if (!(pressed & 1)) focus(a);                       /* right click focuses too */
            mcapture = a;
        }
    }
    int target = mcapture;
    if (target < 0) {                                           /* plain hovering over the focused program */
        int f = focused();
        if (f >= APP_COUNT && hit_window(mx, my) == f && my >= wins[f].y + TITLE_H) target = f;
    }
    if (target >= APP_COUNT && find(target)) {
        struct window *w = &wins[target];
        int lx = mx - w->x - 1, ly = my - w->y - TITLE_H, slot = proc_slot(target);
        if (pressed) proc_mouse(slot, EV_MOUSE_DOWN, lx, ly, buttons);
        if (released) proc_mouse(slot, EV_MOUSE_UP, lx, ly, buttons);
        if (dx || dy) proc_mouse(slot, EV_MOUSE_MOVE, lx, ly, buttons);
    }
    if (!buttons) mcapture = -1;
}

void wm_key(int k) {
    if (menu_on) { menu_key(k); return; }
    if (k == KEY_MENU && focused() == APP_EXPLORER && find(APP_EXPLORER)) {   /* the Menu key: menu for the selection */
        struct window *w = &wins[APP_EXPLORER];
        int sx = mx, sy = my, cw = w->w - 2;
        if (ex_sel >= 0) {
            int rx, ry, rw, rh; ex_item_rect(ex_sel, cw, &rx, &ry, &rw, &rh);
            mx = w->x + 1 + rx + rw / 2; my = w->y + TITLE_H + ry + rh / 2;
        } else { mx = w->x + 40; my = w->y + TITLE_H + (cfg.explorer_view ? 80 : EX_TOP + 10); }
        explorer_menu_for(ex_sel, 1);
        mx = sx; my = sy; dirty = 1;
        return;
    }
    if (k == KEY_START || k == KEY_F1) { start_open = !start_open; dirty = 1; return; }
    if (k == KEY_ESC && start_open) { start_open = 0; dirty = 1; return; }
    if (k == KEY_ALTF4) {                                      /* close the window in front (programs end) */
        int a = focused();
        if (a >= 0) { if (is_proc(a)) proc_kill(tasks[proc_slot(a)].pid); else close_app(a); }
        dirty = 1; return;
    }
    if (k == KEY_ALTTAB) {                                     /* the window at the back comes to the front */
        if (nwin > 1) focus(zorder[0]);
        return;
    }
    if (desk_focus) {                                          /* the desktop was clicked last */
        int i = sel_icon, file = i >= NICONS && i < nicons();
        if (k == KEY_CTRL('v')) { paste_into(DESK_DIR); dirty = 1; return; }
        if (file && (k == KEY_DEL)) { desk_delete(i); return; }
        if (file && (k == KEY_CTRL('c') || k == KEY_CTRL('x'))) { desk_clip(i, k == KEY_CTRL('x')); return; }
        if (file && k == KEY_F2) { desk_explore(i); ex_rename_start(); dirty = 1; return; }
        if (i >= 0 && i < nicons() && k == '\n') {
            if (file) { char p[FS_PATH_MAX]; desk_path(i, p); open_path(p, desk_list[i - NICONS].is_dir); }
            else if (desk_icons[i] == DESK_BIN) { open_app(APP_EXPLORER); ex_go(BIN); }
            else open_app(desk_icons[i]);
            sel_icon = -1; dirty = 1; return;
        }
        if (k == KEY_DOWN || k == KEY_UP) {
            int n = nicons(); i = i < 0 ? 0 : i + (k == KEY_DOWN ? 1 : -1);
            sel_icon = i < 0 ? 0 : i >= n ? n - 1 : i; dirty = 1; return;
        }
    }
    caret_on = 1;
    switch (focused()) {
    case APP_TERMINAL:
        if (k == KEY_CTRL('v')) {                             /* paste: the first line of the clipboard */
            const char *c = clip_text();
            for (int i = 0; i < clip_len() && c[i] != '\n' && c[i] != '\r'; i++) con_key(c[i] == '\t' ? ' ' : (uint8_t)c[i]);
        } else con_key(k);
        dirty = 1; break;
    case APP_NOTEPAD:  notepad_key(k); dirty = 1; break;
    case APP_EXPLORER: explorer_key(k); dirty = 1; break;
    case APP_MSG: if (k == '\n' || k == KEY_ESC) close_app(APP_MSG); break;
    default:
        if (focused() >= APP_COUNT) proc_key(proc_slot(focused()), k);
    }
}

/* Time how long one full-screen redraw takes, in milliseconds (x100 for precision). */
uint32_t wm_bench(int frames) {
    uint32_t t0 = ticks;
    for (int i = 0; i < frames; i++) render();
    return (ticks - t0) * (1000 / TIMER_HZ) * 100 / frames;
}

void wm_tick(void) { caret_on = !caret_on; cpu_sample(); dirty = 1; }

void wm_update(void) {
    static uint32_t last_render;
    if (con_dirty && find(APP_TERMINAL)) dirty = 1;
    /* Redraw at most ~30 times a second, so animations don't eat the CPU. */
    static int bg_full;               /* a partly hidden program window changed */
    if (!dirty && canvas_dirty && !fast_canvas_update()) bg_full = 1;
    /* Real changes: up to 30 redraws/s. Hidden animations only: 10/s. */
    if ((dirty && ticks - last_render >= TIMER_HZ / 30) || (bg_full && ticks - last_render >= TIMER_HZ / 10)) {
        dirty = 0; bg_full = 0; canvas_dirty = 0; cursor_moved = 0; last_render = ticks; render();
    }
    else if (cursor_moved) { cursor_moved = 0; gfx_cursor_move(mx, my); }
}
