/* viewer.c - Pictures: a viewer for .png, .jpg and .bmp files.
 * Opens the file it was started with (or the first picture in C:\Pictures).
 * Big pictures are shrunk to fit; "100%" shows the real size (drag to look
 * around). Left / Right flip through the pictures in the same folder.
 * "Set as wallpaper" puts the picture on the desktop. Esc quits. */
#include "ui.h"
#include "../image.h"

#define MAXW 960            /* biggest picture area */
#define MAXH 560
#define MINW 600
#define BAR 40              /* toolbar */
#define STATUS 24
#define BG 0x1F2430

static char folder[256] = "/Pictures", start_name[64];
static struct dirent pics[128];
static int npics, cur = -1;

static struct image img;     /* the picture at full size */
static const char *error;    /* why it couldn't be shown */
static uint32_t *fitbuf;     /* the picture shrunk to fit, already over the checkerboard */
static int fw, fh;           /* size of fitbuf */
static int actual;           /* 1 = 100% (real size) */
static int pan_x, pan_y, dragging, drag_mx, drag_my, drag_px, drag_py;
static int view_w, view_h;   /* the picture area */
static char note[80]; static uint32_t note_until;
static int user_sized;       /* the user resized / maximised the window: keep that size, fit pictures to it */

static void *ualloc(uint32_t n) { return malloc(n); }
static void ufree(void *p) { free(p); }
static void scopy(char *d, const char *s, int max) { int i = 0; for (; s[i] && i < max - 1; i++) d[i] = s[i]; d[i] = 0; }
static int same(const char *a, const char *b) { while (*a || *b) { if ((*a | 32) != (*b | 32)) return 0; a++; b++; } return 1; }
static void say(const char *s) { scopy(note, s, sizeof note); note_until = ticks() + 3000; }

static void path_of(int i, char *path) {
    int k = 0;
    for (const char *p = folder; *p; p++) path[k++] = *p;
    if (k > 1) path[k++] = '/';
    for (const char *p = pics[i].name; *p; p++) path[k++] = *p;
    path[k] = 0;
}
static uint32_t checker(int x, int y) { return ((x >> 3) + (y >> 3)) & 1 ? 0xD1D5DB : 0xF3F4F6; }
static uint32_t over(uint32_t p, uint32_t bg) {             /* a pixel with alpha, on top of bg */
    uint32_t a = p >> 24;
    if (a == 255) return p & 0xFFFFFF;
    if (a == 0) return bg;
    uint32_t o = 0;
    for (int s = 0; s < 24; s += 8) o |= ((((p >> s) & 255) * a + ((bg >> s) & 255) * (255 - a)) / 255) << s;
    return o;
}

/* the picture's shown size, and (unless the user sized the window) the window's size */
static void layout(void) {
    int maxw = user_sized ? ui.w : MAXW, maxh = user_sized ? ui.h - BAR - STATUS : MAXH;
    int w = img.px ? img.w : 400, h = img.px ? img.h : 200;
    if (w > maxw || h > maxh) { if (w * maxh > h * maxw) { h = h * maxw / w; w = maxw; } else { w = w * maxh / h; h = maxh; } }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    fw = w; fh = h;
    if (user_sized) { view_w = ui.w; view_h = ui.h - BAR - STATUS; return; }
    view_w = w < MINW ? MINW : w; view_h = h < 200 ? 200 : h;
    ui_resize(view_w, view_h + BAR + STATUS);
}
/* the picture shrunk to fit, over the checkerboard where it is see-through */
static void make_fit(void) {
    if (fitbuf) { free(fitbuf); fitbuf = 0; }
    if (error || !img.px) return;
    fitbuf = malloc(fw * fh * 4);
    if (!fitbuf) { error = "Not enough memory."; return; }
    image_resample(&img, 0, 0, img.w, img.h, fitbuf, fw, fh);
    if (img.has_alpha) for (int y = 0; y < fh; y++) for (int x = 0; x < fw; x++) fitbuf[y * fw + x] = over(fitbuf[y * fw + x], checker(x, y));
}

static void load(int i) {
    if (img.px) { free(img.px); img.px = 0; }
    if (fitbuf) { free(fitbuf); fitbuf = 0; }
    error = 0; actual = 0; pan_x = pan_y = 0;
    cur = i;
    settitle(pics[i].name);
    /* show "Opening..." while a big picture is unpacked */
    ui_fill(0, BAR, ui.w, ui.h - BAR, BG);
    ui_text_center(0, BAR + (ui.h - BAR - STATUS) / 2 - 8, ui.w, "Opening...", 0x9CA3AF, 0);
    ui_present();
    char path[320]; path_of(i, path);
    int size = pics[i].size;
    uint8_t *f = malloc(size + 1);
    if (!f) error = "Not enough memory.";
    else if (readfile(path, f, size) != size) error = "Could not read the file.";
    else error = image_decode(f, size, &img, ualloc, ufree);
    if (f) free(f);
    layout();
    make_fit();
    ui_redraw();                                   /* the window changed size: draw it all again */
}

static void draw_picture(void) {
    ui_fill(0, BAR, ui.w, view_h, BG);
    if (error) {
        ui_text_center(0, BAR + view_h / 2 - 18, ui.w, "Can't show this picture", 0xF9FAFB, 1);
        ui_text_center(0, BAR + view_h / 2 + 4, ui.w, error, 0x9CA3AF, 0);
        return;
    }
    if (!actual || (img.w <= view_w && img.h <= view_h)) {
        int ox = (view_w - fw) / 2, oy = BAR + (view_h - fh) / 2;
        for (int y = 0; y < fh; y++) {
            uint32_t *d = ui.px + (oy + y) * ui.w + ox, *s = fitbuf + y * fw;
            for (int x = 0; x < fw; x++) d[x] = s[x];
        }
        return;
    }
    /* 100%: the part of the full picture that fits, starting at pan_x / pan_y */
    int vw = img.w < view_w ? img.w : view_w, vh = img.h < view_h ? img.h : view_h;
    if (pan_x > img.w - vw) pan_x = img.w - vw;
    if (pan_y > img.h - vh) pan_y = img.h - vh;
    if (pan_x < 0) pan_x = 0;
    if (pan_y < 0) pan_y = 0;
    int ox = (view_w - vw) / 2, oy = BAR + (view_h - vh) / 2;
    for (int y = 0; y < vh; y++) {
        uint32_t *d = ui.px + (oy + y) * ui.w + ox;
        const uint32_t *s = img.px + (pan_y + y) * img.w + pan_x;
        for (int x = 0; x < vw; x++) d[x] = img.has_alpha ? over(s[x], checker(pan_x + x, pan_y + y)) : s[x] & 0xFFFFFF;
    }
}

static void draw_status(void) {
    ui_fill(0, ui.h - STATUS, ui.w, STATUS, 0x111827);
    char s[160], num[12]; int k = 0;
    #define ADD(str) do { const char *q_ = (str); while (*q_ && k < 150) s[k++] = *q_++; } while (0)
    ADD(pics[cur].name);
    if (img.px) {
        ADD("   "); ADD(utoa(img.w, num)); ADD(" x "); ADD(utoa(img.h, num)); ADD("  "); ADD(img.format);
        if (img.has_alpha) ADD(" (see-through)");
        int pct = actual || (fw == img.w) ? 100 : fw * 100 / img.w;
        ADD("   "); ADD(utoa(pct, num)); ADD("%");
    }
    ADD("   "); ADD(utoa(cur + 1, num)); ADD(" of "); ADD(utoa(npics, num));
    s[k] = 0;
    ui_text(10, ui.h - STATUS + 4, s, 0xD1D5DB);
    if ((int)(note_until - ticks()) > 0) ui_textb(ui.w - 10 - ui_text_w(note), ui.h - STATUS + 4, note, 0x86EFAC);
}

static void next(int d) { if (npics > 1) load((cur + d + npics) % npics); }

int main(void) {
    char arg[256];
    if (getarg(arg, sizeof arg) > 0) {
        int slash = -1; for (int i = 0; arg[i]; i++) if (arg[i] == '/') slash = i;
        if (slash > 0) { for (int i = 0; i < slash; i++) folder[i] = arg[i]; folder[slash] = 0; }
        else if (slash == 0) scopy(folder, "/", 256);
        scopy(start_name, arg + slash + 1, 64);
    }
    ui_init("Pictures", MINW, 200 + BAR + STATUS);
    ui_no_theme();                                        /* pages and pictures keep their colours */
    ui_resizable(MINW, 200 + BAR + STATUS);
    static struct dirent all[256];
    int n = listdir(folder, all, 256);
    for (int i = 0; i < n && npics < 128; i++) if (!all[i].is_dir && image_is_picture_name(all[i].name)) pics[npics++] = all[i];
    if (!npics) {
        for (;;) {
            ui_fill(0, 0, ui.w, ui.h, BG);
            ui_text_center(0, ui.h / 2 - 8, ui.w, "No pictures (.png, .jpg or .bmp) in this folder.", 0xD1D5DB, 0);
            ui_present(); ui_next();
            if (ui.key == KEY_ESC) return 0;
        }
    }
    int first = 0;
    for (int i = 0; i < npics; i++) if (same(pics[i].name, start_name)) first = i;
    load(first);
    for (;;) {
        if (ui.resized) { user_sized = 1; layout(); make_fit(); }
        /* toolbar */
        ui_vgradient(0, 0, ui.w, BAR, 0xFFFFFF, 0xF3F4F6);
        ui_fill(0, BAR - 1, ui.w, 1, 0xD1D5DB);
        if (ui_button(1, 8, 7, 84, 26, "< Prev") || ui.key == KEY_LEFT || ui.key == '\b') next(-1);
        if (ui_button(2, 98, 7, 84, 26, "Next >") || ui.key == KEY_RIGHT || ui.key == ' ') next(1);
        int big = img.px && (img.w > view_w || img.h > view_h);
        if (big && (ui_button(3, 196, 7, 72, 26, actual ? "Fit" : "100%") || ui.key == 'f' || ui.key == 'F')) {
            actual = !actual;
            if (actual) { pan_x = (img.w - view_w) / 2; pan_y = (img.h - view_h) / 2; }
        }
        if (!error && img.px && (ui_button_primary(4, ui.w - 168, 7, 160, 26, "Set as wallpaper") || ui.key == 'w' || ui.key == 'W')) {
            char path[320]; path_of(cur, path);
            say(set_wallpaper(path) == 0 ? "Wallpaper set" : "Could not use it as wallpaper");
        }
        if (ui.key == KEY_HOME) load(0);
        if (ui.key == KEY_END) load(npics - 1);
        if (ui.key == KEY_ESC) return 0;
        /* drag to look around at 100% */
        if (actual && ui.pressed && ui.my >= BAR && ui.my < ui.h - STATUS) { dragging = 1; drag_mx = ui.mx; drag_my = ui.my; drag_px = pan_x; drag_py = pan_y; }
        if (dragging && ui.down) { pan_x = drag_px - (ui.mx - drag_mx); pan_y = drag_py - (ui.my - drag_my); }
        if (!ui.down) dragging = 0;
        draw_picture();
        draw_status();
        ui_present();
        ui_wait((int)(note_until - ticks()) > 0 ? 500 : -1);
    }
}
