/* ui.c - the MyOS widget toolkit (see ui.h). */
#include "ui.h"
#include "../theme.h"
#include "../text.h"
static void theme_refresh(void);

struct ui_state ui;
static uint8_t font_r[128][16], font_b[128][16];
static int clip_x0, clip_y0, clip_x1, clip_y1;
static struct event pending;
static int has_pending;
static uint32_t last_press_tick;
static int redraw_soon;
static int last_press_x, last_press_y;

/* ---------------- setup and frames ---------------- */
void ui_resize(int w, int h) {
    if (ui.px && w * h <= ui.w * ui.h) { ui.w = w; ui.h = h; }
    else { if (ui.px) free(ui.px); ui.px = malloc((uint32_t)w * h * 4); ui.w = w; ui.h = h; }
    resize(w, h);
    ui_noclip();
}

void ui_resizable(int min_w, int min_h) { resizable(min_w, min_h); }

void ui_init(const char *title, int w, int h) {
    getfont(&font_r[0][0], 0);
    getfont(&font_b[0][0], 1);
    settitle(title);
    theme_refresh();
    ui_resize(w, h);
    ui_fill(0, 0, w, h, UI_BG);
    ui.mx = ui.my = -10000;
}

static void begin_frame(void) {
    ui.pressed = ui.released = ui.rpressed = ui.dbl = ui.moved = 0;
    ui.key = 0; ui.timeout = 0; ui.resized = 0;
    if (!ui.down) ui.active = 0;
}

static void take(const struct event *e) {
    if (e->type == EV_KEY) { ui.key = e->key; return; }
    if (e->type == EV_RESIZE) {                            /* the kernel already gave the window its new size */
        int w = e->x, h = e->y;
        if (!ui.px || w * h > ui.w * ui.h) { if (ui.px) free(ui.px); ui.px = malloc((uint32_t)w * h * 4); }
        ui.w = w; ui.h = h; ui.resized = 1;
        ui_noclip();
        ui_fill(0, 0, w, h, UI_BG);
        return;
    }
    ui.mx = e->x; ui.my = e->y;
    if (e->type == EV_MOUSE_MOVE) { ui.moved = 1; ui.down = e->buttons & 1; return; }
    if (e->type == EV_MOUSE_DOWN) {
        if ((e->buttons & 1) && !ui.down) {
            ui.down = 1; ui.pressed = 1;
            uint32_t now = ticks();
            int dx = e->x - last_press_x, dy = e->y - last_press_y;
            ui.dbl = now - last_press_tick < 500 && dx * dx + dy * dy < 36;   /* within half a second */
            last_press_tick = ui.dbl ? 0 : now;
            last_press_x = e->x; last_press_y = e->y;
        }
        if (e->buttons & 2) ui.rpressed = 1;
    } else if (e->type == EV_MOUSE_UP) {
        if (!(e->buttons & 1) && ui.down) { ui.down = 0; ui.released = 1; }
    }
}

void ui_redraw(void) { redraw_soon = 1; }

void ui_wait(int ms) {
    begin_frame();
    if (redraw_soon) { redraw_soon = 0; return; }          /* draw again straight away, no input */
    struct event e;
    if (has_pending) { e = pending; has_pending = 0; }
    else if (ms < 0) getevent(&e);
    else {
        uint32_t t0 = ticks();
        for (;;) {
            if (pollevent(&e)) break;
            if ((int)(ticks() - t0) >= ms) { ui.timeout = 1; return; }
            sleep(10);
        }
    }
    take(&e);
    /* many mouse moves in a row: only the newest position matters */
    if (e.type == EV_MOUSE_MOVE)
        while (pollevent(&e)) {
            if (e.type == EV_MOUSE_MOVE) take(&e);
            else { pending = e; has_pending = 1; break; }
        }
}
void ui_next(void) { ui_wait(-1); }

void ui_present(void) { blit(0, 0, ui.w, ui.h, ui.px, ui.w); theme_refresh(); }   /* the next frame follows dark mode */
void ui_present_rect(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > ui.w) w = ui.w - x;
    if (y + h > ui.h) h = ui.h - y;
    if (w > 0 && h > 0) blit(x, y, w, h, ui.px + y * ui.w + x, ui.w);
}

/* ---------------- drawing ---------------- */
void ui_clip(int x, int y, int w, int h) {
    clip_x0 = x < 0 ? 0 : x; clip_y0 = y < 0 ? 0 : y;
    clip_x1 = x + w > ui.w ? ui.w : x + w; clip_y1 = y + h > ui.h ? ui.h : y + h;
}
void ui_noclip(void) { clip_x0 = clip_y0 = 0; clip_x1 = ui.w; clip_y1 = ui.h; }

/* dark mode (theme.h): programs follow it unless they call ui_no_theme() (pictures, cards...) */
static int theme_allowed = 1, dark_now;
void ui_no_theme(void) { theme_allowed = 0; dark_now = 0; }
int ui_dark(void) { return dark_now; }
static void theme_refresh(void) { dark_now = theme_allowed && theme(); }
#define THEMED(c) (dark_now ? theme_dark_map(c) : (c))
void ui_pixel(int x, int y, uint32_t c) {
    c = THEMED(c);
    if (x >= clip_x0 && x < clip_x1 && y >= clip_y0 && y < clip_y1) ui.px[y * ui.w + x] = c;
}
void ui_fill(int x, int y, int w, int h, uint32_t c) {
    int x0 = x < clip_x0 ? clip_x0 : x, y0 = y < clip_y0 ? clip_y0 : y;
    int x1 = x + w > clip_x1 ? clip_x1 : x + w, y1 = y + h > clip_y1 ? clip_y1 : y + h;
    c = THEMED(c);
    for (int j = y0; j < y1; j++) { uint32_t *p = ui.px + j * ui.w; for (int i = x0; i < x1; i++) p[i] = c; }
}
static int isqrt(int v) { int r = 0; while ((r + 1) * (r + 1) <= v) r++; return r; }
static int corner_inset(int j, int r) {        /* how far row j (0 = top) of a rounded box is indented */
    if (j >= r) return 0;
    int dy = r - j;
    return r - isqrt(r * r - dy * dy + r);   /* "+ r" softens the curve a little */
}
void ui_rfill(int x, int y, int w, int h, int r, uint32_t c) {
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    for (int j = 0; j < h; j++) {
        int k = j < r ? corner_inset(j, r) : (j >= h - r ? corner_inset(h - 1 - j, r) : 0);
        ui_fill(x + k, y + j, w - 2 * k, 1, c);
    }
}
void ui_frame(int x, int y, int w, int h, uint32_t c) {
    ui_fill(x, y, w, 1, c); ui_fill(x, y + h - 1, w, 1, c);
    ui_fill(x, y, 1, h, c); ui_fill(x + w - 1, y, 1, h, c);
}
void ui_rframe(int x, int y, int w, int h, int r, uint32_t c) {
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    for (int j = 0; j < h; j++) {
        int top = j < r, bot = j >= h - r;
        int jj = top ? j : (bot ? h - 1 - j : r);
        int k = (top || bot) ? corner_inset(jj, r) : 0;
        if (j == 0 || j == h - 1) { ui_fill(x + k, y + j, w - 2 * k, 1, c); continue; }
        int kn = (top || bot) ? corner_inset(jj - 1, r) : k;      /* row nearer the edge */
        int span = kn - k > 1 ? kn - k : 1;
        ui_fill(x + k, y + j, span, 1, c);
        ui_fill(x + w - k - span, y + j, span, 1, c);
    }
}
void ui_line(int x0, int y0, int x1, int y1, int t, uint32_t c) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        ui_fill(x0 - t / 2, y0 - t / 2, t, t, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
uint32_t ui_mix(uint32_t a, uint32_t b, int t) {
    uint32_t r = (((a >> 16) & 255) * (256 - t) + ((b >> 16) & 255) * t) >> 8;
    uint32_t g = (((a >> 8) & 255) * (256 - t) + ((b >> 8) & 255) * t) >> 8;
    uint32_t bl = ((a & 255) * (256 - t) + (b & 255) * t) >> 8;
    return r << 16 | g << 8 | bl;
}
void ui_vgradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom) {
    for (int j = 0; j < h; j++) ui_fill(x, y + j, w, 1, ui_mix(top, bottom, h > 1 ? j * 256 / (h - 1) : 0));
}
static void draw_char(int x, int y, char ch, uint32_t c, const uint8_t (*f)[16], int scale) {
    const uint8_t *g = f[(uint8_t)ch & 127];
    for (int j = 0; j < 16; j++) {
        uint8_t row = g[j];
        if (!row) continue;
        for (int i = 0; i < 8; i++)
            if (row & (0x80 >> i)) {
                if (scale == 1) ui_pixel(x + i, y + j, c);
                else ui_fill(x + i * scale, y + j * scale, scale, scale, c);
            }
    }
}
/* one character cell: ASCII from the font, Arabic from the Arabic glyphs, anything else a box */
static void ui_cell(int x, int y, int cp, uint32_t c, int bold) {
    if (cp < 128) { draw_char(x, y, (char)cp, c, bold ? font_b : font_r, 1); return; }
    const unsigned char *g = text_arabic_glyph(cp, bold);
    if (!g) { ui_fill(x + 1, y + 3, 6, 1, c); ui_fill(x + 1, y + 12, 6, 1, c); ui_fill(x + 1, y + 3, 1, 10, c); ui_fill(x + 6, y + 3, 1, 10, c); return; }
    for (int j = 0; j < 16; j++) for (int i = 0; i < 8; i++) if (g[j] & (0x80 >> i)) ui_pixel(x + i, y + j, c);
}
static int text_any(const char *s, int bold, int x, int y, uint32_t c) {
    int n = 0, wide = 0;
    for (; s[n]; n++) if ((uint8_t)s[n] >= 0x80) wide = 1;
    if (!wide) { for (int i = 0; i < n; i++) draw_char(x + i * 8, y, s[i], c, bold ? font_b : font_r, 1); return n * 8; }
    static struct tcell cells[TEXT_MAX];
    int rtl, m = text_layout(s, n, cells, TEXT_MAX, &rtl);
    for (int k = 0; k < m; k++) ui_cell(x + k * 8, y, cells[k].cp, c, bold);
    return m * 8;
}
int ui_text(int x, int y, const char *s, uint32_t c) { return text_any(s, 0, x, y, c); }
int ui_textb(int x, int y, const char *s, uint32_t c) { return text_any(s, 1, x, y, c); }
void ui_text_big(int x, int y, const char *s, uint32_t c, int scale) { for (int i = 0; s[i]; i++) draw_char(x + i * 8 * scale, y, s[i], c, font_b, scale); }
int ui_text_w(const char *s) { return text_cells(s) * 8; }
void ui_text_center(int x, int y, int w, const char *s, uint32_t c, int bold) {
    int tx = x + (w - ui_text_w(s)) / 2;
    if (bold) ui_textb(tx, y, s, c); else ui_text(tx, y, s, c);
}

/* ---------------- input helpers ---------------- */
int ui_hit(int x, int y, int w, int h) { return ui.mx >= x && ui.mx < x + w && ui.my >= y && ui.my < y + h; }
int ui_clicked(int x, int y, int w, int h) { return ui.pressed && ui_hit(x, y, w, h); }

/* A press that starts and ends on the same widget counts as a click. */
static int press_logic(int id, int x, int y, int w, int h) {
    int hot = ui_hit(x, y, w, h);
    if (ui.pressed && hot) ui.active = id;
    return ui.released && ui.active == id && hot;
}

/* ---------------- widgets ---------------- */
void ui_label(int x, int y, const char *s) { ui_text(x, y, s, UI_TEXT); }

static int button_common(int id, int x, int y, int w, int h, const char *label, int primary) {
    int clicked = press_logic(id, x, y, w, h);
    int hot = ui_hit(x, y, w, h), held = hot && ui.down && ui.active == id;
    uint32_t bg = primary ? (held ? 0x1E40AF : hot ? 0x1D4ED8 : UI_ACCENT)
                          : (held ? 0xE5E7EB : hot ? 0xF9FAFB : UI_WHITE);
    ui_rfill(x, y, w, h, 5, primary ? bg : (hot ? 0x9CA3AF : UI_BORDER));
    ui_rfill(x + 1, y + 1, w - 2, h - 2, 4, bg);
    ui_text_center(x, y + (h - 16) / 2 + (held ? 1 : 0), w, label, primary ? UI_WHITE : UI_TEXT, primary);
    return clicked;
}
int ui_button(int id, int x, int y, int w, int h, const char *label) { return button_common(id, x, y, w, h, label, 0); }
int ui_button_primary(int id, int x, int y, int w, int h, const char *label) { return button_common(id, x, y, w, h, label, 1); }

int ui_checkbox(int id, int x, int y, const char *label, int *value) {
    int w = 24 + ui_text_w(label);
    int changed = press_logic(id, x, y, w, 20);
    if (changed) *value = !*value;
    int hot = ui_hit(x, y, w, 20);
    if (*value) {
        ui_rfill(x, y + 2, 16, 16, 3, hot ? 0x1D4ED8 : UI_ACCENT);
        ui_line(x + 4, y + 10, x + 7, y + 13, 2, UI_WHITE);
        ui_line(x + 7, y + 13, x + 12, y + 6, 2, UI_WHITE);
    } else {
        ui_rfill(x, y + 2, 16, 16, 3, hot ? UI_ACCENT : 0x9CA3AF);
        ui_rfill(x + 1, y + 3, 14, 14, 2, UI_WHITE);
    }
    ui_text(x + 24, y + 2, label, UI_TEXT);
    return changed;
}

int ui_radio(int id, int x, int y, const char *label, int *value, int mine) {
    int w = 24 + ui_text_w(label);
    int clicked = press_logic(id, x, y, w, 20);
    if (clicked) *value = mine;
    int hot = ui_hit(x, y, w, 20), on = *value == mine;
    ui_rfill(x, y + 2, 16, 16, 8, on || hot ? UI_ACCENT : 0x9CA3AF);
    ui_rfill(x + 1, y + 3, 14, 14, 7, UI_WHITE);
    if (on) ui_rfill(x + 4, y + 6, 8, 8, 4, UI_ACCENT);
    ui_text(x + 24, y + 2, label, UI_TEXT);
    return clicked && !on;
}

static int tb_caret, tb_scroll;                       /* caret: a byte position; scroll: characters */
static int ins_bytes(char *buf, int *len, int max, int at, const char *s, int n) {   /* insert n bytes at at */
    if (*len + n > max - 1) return 0;
    for (int j = *len; j >= at; j--) buf[j + n] = buf[j];
    for (int i = 0; i < n; i++) buf[at + i] = s[i];
    *len += n;
    return n;
}
static int typed_bytes(int k, char *out) {           /* the UTF-8 for a typed key, or 0 */
    if (k >= 32 && k < 127) { out[0] = (char)k; return 1; }
    if (KEY_IS_UNI(k)) return utf8_encode(KEY_CP(k), out);
    return 0;
}
int ui_textbox(int id, int x, int y, int w, char *buf, int max) {
    int h = 26, len = strlen(buf), cols = (w - 12) / 8, enter = 0;
    static struct tcell cells[TEXT_MAX];
    int rtl, m = text_layout_para(buf, len, cells, TEXT_MAX, &rtl);
    int ar = text_has_rtl(buf, len);
    if (ui.pressed) {
        if (ui_hit(x, y, w, h)) {
            if (ui.focus != id) tb_scroll = 0;
            ui.focus = id;
            if (ar && rtl) {                             /* right to left: cells line up on the right */
                int shown = m < cols ? m : cols, x0 = x + w - 6 - shown * 8;
                tb_caret = ui.mx < x0 ? len : text_pos_at_cell(cells, m, rtl, (ui.mx - x0) / 8, len);
            } else if (ar) tb_caret = text_pos_at_cell(cells, m, rtl, (ui.mx - (x + 6)) / 8, len);
            else {
                int c = (ui.mx - (x + 6) + 4) / 8 + tb_scroll;
                tb_caret = utf8_skip(buf, len, 0, c < 0 ? 0 : c);
            }
        } else if (ui.focus == id) ui.focus = 0;
    }
    int focused = ui.focus == id;
    if (focused) {
        if (tb_caret > len) tb_caret = len;
        int k = ui.key;
        if (k) {
            char u[4]; int ul;
            ui.key = 0;                                   /* the text box uses this key */
            if (k == '\n') enter = 1;
            else if (k == '\b') { if (tb_caret > 0) { int p = utf8_prev(buf, tb_caret), d = tb_caret - p; for (int i = p; i <= len - d; i++) buf[i] = buf[i + d]; tb_caret = p; } }
            else if (k == KEY_DEL) { if (tb_caret < len) { int d = utf8_next(buf, len, tb_caret) - tb_caret; for (int i = tb_caret; i <= len - d; i++) buf[i] = buf[i + d]; } }
            else if (k == KEY_LEFT) tb_caret = utf8_prev(buf, tb_caret);
            else if (k == KEY_RIGHT) tb_caret = utf8_next(buf, len, tb_caret);
            else if (k == KEY_HOME) tb_caret = 0;
            else if (k == KEY_END) tb_caret = len;
            else if (k == KEY_CTRL('c')) clipset(buf, len);
            else if (k == KEY_CTRL('x')) { clipset(buf, len); buf[0] = 0; tb_caret = 0; }
            else if (k == KEY_CTRL('v')) {
                char tmp[256]; int n = clipget(tmp, 255);
                if (n > 255) n = 255;
                for (int i = 0; i < n; i++) {
                    if ((uint8_t)tmp[i] < 32 || tmp[i] == 127) { if (tmp[i] == '\n') break; continue; }
                    tb_caret += ins_bytes(buf, &len, max, tb_caret, tmp + i, 1);
                }
            }
            else if (k == KEY_ESC) ui.focus = 0;
            else if ((ul = typed_bytes(k, u)) > 0) tb_caret += ins_bytes(buf, &len, max, tb_caret, u, ul);
            else ui.key = k;                              /* not ours (e.g. Tab, arrows up/down) */
            len = strlen(buf);
            m = text_layout_para(buf, len, cells, TEXT_MAX, &rtl);
            ar = text_has_rtl(buf, len);
        }
        int ci = utf8_count(buf, 0, tb_caret);
        if (ci < tb_scroll) tb_scroll = ci;
        if (ci > tb_scroll + cols) tb_scroll = ci - cols;
    }
    int scroll = focused ? tb_scroll : 0;
    ui_rfill(x, y, w, h, 4, focused ? UI_ACCENT : (ui_hit(x, y, w, h) ? 0x9CA3AF : UI_BORDER));
    ui_rfill(x + 1, y + 1, w - 2, h - 2, 3, UI_WHITE);
    if (focused) ui_fill(x + 3, y + h - 2, w - 6, 1, UI_ACCENT);
    if (ar) {                                             /* Arabic: shaped; right to left lines up on the right */
        int cc = text_caret_cell(cells, m, rtl, tb_caret, len), vs = 0;
        if (m > cols) vs = rtl ? (cc > cols ? cc - cols : 0) : (cc > cols ? cc - cols : 0);
        int shown = m - vs < cols ? m - vs : cols, x0 = rtl ? x + w - 6 - shown * 8 : x + 6;
        for (int k = 0; k < shown; k++) ui_cell(x0 + k * 8, y + 5, cells[vs + k].cp, UI_TEXT, 0);
        if (focused) ui_fill(x0 + (cc - vs) * 8 - (cc > vs ? 1 : 0), y + 5, 2, 16, UI_TEXT);
        return enter;
    }
    int from = utf8_skip(buf, len, 0, scroll), to = utf8_skip(buf, len, from, cols);
    char vis[400]; int n = 0;
    for (int i = from; i < to && n < 399; i++) vis[n++] = buf[i];
    vis[n] = 0;
    ui_text(x + 6, y + 5, vis, UI_TEXT);
    if (focused) ui_fill(x + 6 + (utf8_count(buf, 0, tb_caret) - scroll) * 8, y + 5, 2, 16, UI_TEXT);
    return enter;
}

/* ---------------- multi-line text area ---------------- */
#define TA_LH 18                                      /* line height */
#define TA_MAXL 3000
static int ta_start[TA_MAXL + 1];
/* where each visual line starts, wrapping at cols (at a space when there is one) */
static int ta_wrap(const char *b, int len, int cols) {
    int n = 0, i = 0;
    for (;;) {
        ta_start[n++] = i;
        if (n >= TA_MAXL) break;
        int j = i;
        for (int c = 0; j < len && b[j] != '\n'; j = utf8_next(b, len, j)) if (c++ >= cols) break;   /* cols characters */
        if (j >= len) break;                          /* the last line */
        if (b[j] == '\n') { i = j + 1; continue; }
        int s = j;                                    /* too long: break after the last space */
        while (s > i && b[s - 1] != ' ') s--;
        i = s > i ? s : j;
    }
    ta_start[n] = len + 1;
    return n;
}
static int ta_line_of(int pos, int n) {
    int l = 0;
    while (l + 1 < n && ta_start[l + 1] <= pos) l++;
    return l;
}
static int ta_line_end(const char *b, int len, int l, int n) {   /* last caret position on line l */
    int e = l + 1 < n ? ta_start[l + 1] : len;
    if (l + 1 < n && e > ta_start[l] && (b[e - 1] == '\n' || b[e - 1] == ' ')) e--;
    return e;
}
static int ta_pos_at(const char *b, int len, int n, int line, int col) {
    if (line < 0) return 0;
    if (line >= n) return len;
    int p = utf8_skip(b, len, ta_start[line], col < 0 ? 0 : col), e = ta_line_end(b, len, line, n);
    return p > e ? e : p;
}
/* A line with Arabic is shaped and, when right to left, lined up on the right (x0 is from the box's left edge). */
static struct tcell ta_cells[TEXT_MAX];
static int ta_layout(const char *b, int s, int e, int w, int *m, int *rtl) {
    *m = text_layout_para(b + s, e - s, ta_cells, TEXT_MAX, rtl);
    return *rtl ? w - 16 - *m * 8 : 6;
}
static int ta_click(const char *b, int len, int n, int l, int px, int w) {   /* the position under a click */
    if (l < 0) return 0;
    if (l >= n) return len;
    int s = ta_start[l], e = ta_line_end(b, len, l, n);
    if (text_has_rtl(b + s, e - s)) {
        int m, rtl, x0 = ta_layout(b, s, e, w, &m, &rtl);
        if (px < x0) return rtl ? e : s;
        return s + text_pos_at_cell(ta_cells, m, rtl, (px - x0) / 8, e - s);
    }
    return ta_pos_at(b, len, n, l, (px - 6 + 4) / 8);
}
void ui_textarea_reset(struct ui_textarea *st) { st->caret = st->anchor = st->scroll = st->dragging = 0; }
static void ta_delete(char *buf, int *len, int a, int b) {        /* remove [a, b) */
    if (b <= a) return;
    for (int i = a; i <= *len - (b - a); i++) buf[i] = buf[i + (b - a)];
    *len -= b - a;
}
static int ta_insert(char *buf, int *len, int max, int at, const char *s, int n) {
    if (*len + n > max - 1) n = max - 1 - *len;
    if (n <= 0) return 0;
    for (int i = *len; i >= at; i--) buf[i + n] = buf[i];
    for (int i = 0; i < n; i++) buf[at + i] = s[i];
    *len += n;
    return n;
}
int ui_textarea(int id, int x, int y, int w, int h, char *buf, int max, struct ui_textarea *st) {
    int len = strlen(buf), changed = 0;
    int cols = (w - 22) / 8, rows = (h - 8) / TA_LH;
    if (cols < 4) cols = 4;
    if (rows < 1) rows = 1;
    int n = ta_wrap(buf, len, cols);
    if (st->caret > len) st->caret = len;
    if (st->anchor > len) st->anchor = len;
    int bar_x = x + w - 10;
    /* mouse */
    if (ui.pressed && ui_hit(x, y, w, h)) {
        ui.focus = id;
        if (n > rows && ui.mx >= bar_x) {             /* scroll bar: a page up or down */
            int thumb = y + 4 + (h - 8) * st->scroll / n;
            st->scroll += ui.my < thumb ? -rows : rows;
        } else {
            int l = st->scroll + (ui.my - y - 4) / TA_LH;
            st->caret = ta_click(buf, len, n, l, ui.mx - x, w);
            if (ui.dbl) {                             /* double-click selects a word */
                int a = st->caret, b = st->caret;
                while (a > 0 && buf[a - 1] > ' ') a--;
                while (b < len && buf[b] > ' ') b++;
                st->anchor = a; st->caret = b;
            } else { st->anchor = st->caret; st->dragging = 1; }
        }
    } else if (ui.pressed && ui.focus == id && !ui_hit(x, y, w, h)) ui.focus = 0;
    if (st->dragging) {
        if (!ui.down) st->dragging = 0;
        else if (ui.moved) {
            int my = ui.my;
            if (my < y) { if (st->scroll > 0) st->scroll--; my = y + 4; }
            if (my > y + h) { if (st->scroll < n - rows) st->scroll++; my = y + h - 6; }
            st->caret = ta_click(buf, len, n, st->scroll + (my - y - 4) / TA_LH, ui.mx - x, w);
        }
    }
    /* keys */
    int focused = ui.focus == id;
    if (focused && ui.key) {
        int k = ui.key, used = 1;
        int a = st->anchor < st->caret ? st->anchor : st->caret, b = st->anchor < st->caret ? st->caret : st->anchor;
        int line = ta_line_of(st->caret, n), col = utf8_count(buf, ta_start[line], st->caret);
        char u[4]; int ul;
        if ((ul = typed_bytes(k, u)) > 0) {
            ta_delete(buf, &len, a, b); st->caret = a;
            st->caret += ta_insert(buf, &len, max, st->caret, u, ul); changed = 1;
        } else if (k == '\n' || k == '\t') {
            const char *s = k == '\n' ? "\n" : "    ";
            ta_delete(buf, &len, a, b); st->caret = a;
            st->caret += ta_insert(buf, &len, max, st->caret, s, strlen(s)); changed = 1;
        } else if (k == '\b') {
            if (b > a) { ta_delete(buf, &len, a, b); st->caret = a; changed = 1; }
            else if (st->caret > 0) { int p = utf8_prev(buf, st->caret); ta_delete(buf, &len, p, st->caret); st->caret = p; changed = 1; }
        } else if (k == KEY_DEL) {
            if (b > a) { ta_delete(buf, &len, a, b); st->caret = a; changed = 1; }
            else if (st->caret < len) { ta_delete(buf, &len, st->caret, utf8_next(buf, len, st->caret)); changed = 1; }
        } else if (k == KEY_LEFT) st->caret = b > a ? a : utf8_prev(buf, st->caret);
        else if (k == KEY_RIGHT) st->caret = b > a ? b : utf8_next(buf, len, st->caret);
        else if (k == KEY_UP) st->caret = line > 0 ? ta_pos_at(buf, len, n, line - 1, col) : 0;
        else if (k == KEY_DOWN) st->caret = line < n - 1 ? ta_pos_at(buf, len, n, line + 1, col) : len;
        else if (k == KEY_PGUP) st->caret = ta_pos_at(buf, len, n, line - rows, col);
        else if (k == KEY_PGDN) st->caret = ta_pos_at(buf, len, n, line + rows, col);
        else if (k == KEY_HOME) st->caret = ta_start[line];
        else if (k == KEY_END) st->caret = ta_line_end(buf, len, line, n);
        else if (k == KEY_CTRL('a')) { st->anchor = 0; st->caret = len; used = 2; }
        else if (k == KEY_CTRL('c') || k == KEY_CTRL('x')) {
            if (b > a) clipset(buf + a, b - a);
            if (k == KEY_CTRL('x') && b > a) { ta_delete(buf, &len, a, b); st->caret = a; changed = 1; }
            used = 2;
        } else if (k == KEY_CTRL('v')) {
            static char tmp[4096];
            int m = clipget(tmp, sizeof tmp - 1);
            if (m > (int)sizeof tmp - 1) m = sizeof tmp - 1;
            int q = 0; for (int i = 0; i < m; i++) if (((uint8_t)tmp[i] >= 32 && tmp[i] != 127) || tmp[i] == '\n') tmp[q++] = tmp[i];
            ta_delete(buf, &len, a, b); st->caret = a;
            st->caret += ta_insert(buf, &len, max, st->caret, tmp, q); changed = 1;
        } else if (k == KEY_ESC) { ui.focus = 0; used = 2; }
        else used = 0;
        if (used == 1) st->anchor = st->caret;        /* moving or typing ends the selection */
        if (used) ui.key = 0;
        buf[len] = 0;
        if (changed) n = ta_wrap(buf, len, cols);
        int cl = ta_line_of(st->caret, n);            /* keep the caret in view */
        if (cl < st->scroll) st->scroll = cl;
        if (cl >= st->scroll + rows) st->scroll = cl - rows + 1;
    }
    if (st->scroll > n - rows) st->scroll = n - rows;
    if (st->scroll < 0) st->scroll = 0;
    /* draw */
    ui_rfill(x, y, w, h, 4, focused ? UI_ACCENT : UI_BORDER);
    ui_rfill(x + 1, y + 1, w - 2, h - 2, 3, UI_WHITE);
    int a = st->anchor < st->caret ? st->anchor : st->caret, b = st->anchor < st->caret ? st->caret : st->anchor;
    ui_clip(x + 1, y + 1, w - 2, h - 2);
    for (int r = 0; r < rows && st->scroll + r < n; r++) {
        int l = st->scroll + r, s = ta_start[l], e = l + 1 < n ? ta_start[l + 1] : len, ly = y + 4 + r * TA_LH;
        uint32_t selc = focused ? 0xBFDBFE : 0xE5E7EB;
        int le = ta_line_end(buf, len, l, n);
        if (text_has_rtl(buf + s, le - s)) {          /* Arabic: cell by cell, selection per cell */
            int m, rtl, x0 = x + ta_layout(buf, s, le, w, &m, &rtl);
            for (int c = 0; c < m; c++) {
                int at = s + ta_cells[c].at;
                if (b > a && at >= a && at < b) ui_fill(x0 + c * 8, ly, 8, 16, selc);
                ui_cell(x0 + c * 8, ly, ta_cells[c].cp, UI_TEXT, 0);
            }
            continue;
        }
        char line[1024]; int k = 0;
        for (int i = s; i < e && k < 1023; i++) if (buf[i] != '\n') line[k++] = buf[i];
        line[k] = 0;
        if (b > a && b > s && a <= e) {               /* selection on this line */
            int sa = a > s ? utf8_count(buf, s, a) : 0, sb = utf8_count(buf, s, b < e ? b : e);
            if (b > e && l + 1 < n) sb++;             /* show that the line break is selected too */
            ui_fill(x + 6 + sa * 8, ly, (sb - sa) * 8, 16, selc);
        }
        ui_text(x + 6, ly, line, UI_TEXT);
    }
    if (focused && b == a) {
        int cl = ta_line_of(st->caret, n);
        int s = ta_start[cl], le = ta_line_end(buf, len, cl, n), cx;
        if (text_has_rtl(buf + s, le - s)) {
            int m, rtl, x0 = ta_layout(buf, s, le, w, &m, &rtl);
            int cell = text_caret_cell(ta_cells, m, rtl, st->caret - s, le - s);
            cx = x + x0 + cell * 8 - (cell > 0 ? 1 : 0);
        } else cx = x + 6 + utf8_count(buf, s, st->caret) * 8;
        if (cl >= st->scroll && cl < st->scroll + rows)
            ui_fill(cx, y + 4 + (cl - st->scroll) * TA_LH, 2, 16, UI_TEXT);
    }
    ui_noclip();
    if (n > rows) {                                   /* scroll bar */
        int th = (h - 8) * rows / n; if (th < 16) th = 16;
        int ty = y + 4 + (h - 8 - th) * st->scroll / (n - rows);
        ui_rfill(bar_x, y + 4, 6, h - 8, 3, 0xF3F4F6);
        ui_rfill(bar_x, ty, 6, th, 3, 0x9CA3AF);
    }
    return changed;
}

int ui_list(int id, int x, int y, int w, int h, const char *const *items, int n, int *sel, int *scroll) {
    const int RH = 22;
    int vis = (h - 2) / RH, result = 0, old = *sel;
    int bar = n > vis, lw = bar ? w - 10 : w;
    if (ui.pressed && ui_hit(x, y, w, h)) {
        ui.focus = id;
        if (bar && ui.mx >= x + w - 10) {                  /* scroll bar: page up / down */
            int thumb_y = y + 1 + (h - 2) * *scroll / n;
            *scroll += ui.my < thumb_y ? -vis : vis;
        } else {
            int r = *scroll + (ui.my - y - 1) / RH;
            if (r < n) { *sel = r; if (ui.dbl) result = 2; }
        }
    }
    if (ui.focus == id && ui.key) {
        int k = ui.key, used = 1;
        if (k == KEY_UP) { if (*sel > 0) (*sel)--; }
        else if (k == KEY_DOWN) { if (*sel < n - 1) (*sel)++; }
        else if (k == KEY_HOME) *sel = 0;
        else if (k == KEY_END) *sel = n - 1;
        else if (k == KEY_PGUP) { *sel -= vis; if (*sel < 0) *sel = 0; }
        else if (k == KEY_PGDN) { *sel += vis; if (*sel > n - 1) *sel = n - 1; }
        else if (k == '\n') { if (*sel >= 0) result = 2; }
        else used = 0;
        if (used) { ui.key = 0; if (*sel >= 0 && *sel < *scroll) *scroll = *sel; if (*sel >= *scroll + vis) *scroll = *sel - vis + 1; }
    }
    if (*scroll > n - vis) *scroll = n - vis;
    if (*scroll < 0) *scroll = 0;
    if (!result && *sel != old) result = 1;
    int focused = ui.focus == id;
    ui_rfill(x, y, w, h, 4, focused ? UI_ACCENT : UI_BORDER);
    ui_fill(x + 1, y + 1, w - 2, h - 2, UI_WHITE);
    ui_clip(x + 1, y + 1, w - 2, h - 2);
    for (int r = 0; r < vis + 1 && *scroll + r < n; r++) {
        int i = *scroll + r, ry = y + 1 + r * RH;
        if (i == *sel) { ui_fill(x + 1, ry, lw - 2, RH, focused ? UI_ACCENT_LIGHT : 0xE5E7EB); ui_fill(x + 1, ry + 3, 3, RH - 6, UI_ACCENT); }
        else if (ui_hit(x, ry, lw, RH)) ui_fill(x + 1, ry, lw - 2, RH, 0xF8FAFC);
        char t[128]; int k = 0, maxc = (lw - 20) / 8;
        for (; items[i][k] && k < maxc && k < 127; k++) t[k] = items[i][k];
        t[k] = 0;
        ui_text(x + 10, ry + 3, t, UI_TEXT);
    }
    if (bar) {
        int th = (h - 2) * vis / n; if (th < 16) th = 16;
        int ty = y + 1 + (h - 2 - th) * *scroll / (n - vis > 0 ? n - vis : 1);
        ui_fill(x + w - 10, y + 1, 9, h - 2, 0xF3F4F6);
        ui_rfill(x + w - 8, ty, 5, th, 2, 0x9CA3AF);
    }
    ui_noclip();
    return result;
}

void ui_progress(int x, int y, int w, int h, int value, int max) {
    ui_rfill(x, y, w, h, h / 2, 0xE5E7EB);
    int fw = max > 0 ? w * (value < max ? value : max) / max : 0;
    if (fw > 0) ui_rfill(x, y, fw < h ? h : fw, h, h / 2, UI_ACCENT);
}

/* ---------------- menu bar ---------------- */
static const char *const *m_menus;
static int m_n, m_open = -1, m_hover = -1, m_mx, m_my;
static int m_tx[12], m_tw[12];

/* copy part k of "Title|a|b" (0 = title) into out; returns 0 if there is no such part */
static int menu_part(const char *m, int k, char *out, int max) {
    while (k > 0 && *m) { if (*m == '|') k--; m++; }
    out[0] = 0;
    if (k > 0) return 0;
    int i = 0;
    while (m[i] && m[i] != '|' && i < max - 1) { out[i] = m[i]; i++; }
    out[i] = 0;
    return 1;
}
static int menu_count(const char *m) { int n = 0; for (; *m; m++) if (*m == '|') n++; return n; }

struct mitem { int y, h, sep, dis, tick, index; char text[48], key[24]; };
static int menu_items(int k, struct mitem *it, int max, int *width) {
    int n = menu_count(m_menus[k]), y = UI_MENU_H + 4, idx = 0, cnt = 0, w = 140;
    for (int i = 1; i <= n && cnt < max; i++) {
        char raw[72]; menu_part(m_menus[k], i, raw, sizeof raw);
        struct mitem *m = &it[cnt++];
        m->sep = raw[0] == '-' && !raw[1];
        m->dis = raw[0] == '!'; m->tick = raw[0] == '*';
        const char *s = raw + (m->dis || m->tick);
        int j = 0, q = 0;
        while (s[j] && s[j] != '\t' && j < 47) { m->text[j] = s[j]; j++; }
        m->text[j] = 0;
        if (s[j] == '\t') { j++; while (s[j] && q < 23) m->key[q++] = s[j++]; }
        m->key[q] = 0;
        m->y = y; m->h = m->sep ? 9 : 26;
        m->index = m->sep ? -1 : idx++;
        y += m->h;
        int tw = 40 + ui_text_w(m->text) + (q ? 32 + q * 8 : 0);
        if (tw > w) w = tw;
    }
    *width = w;
    return cnt;
}

int ui_menu_open(void) { return m_open >= 0; }

int ui_menubar(const char *const *menus, int n) {
    m_menus = menus; m_n = n > 12 ? 12 : n;
    int x = 6;
    for (int i = 0; i < m_n; i++) {
        char t[32]; menu_part(menus[i], 0, t, sizeof t);
        m_tx[i] = x; m_tw[i] = ui_text_w(t) + 20; x += m_tw[i];
    }
    m_mx = ui.mx; m_my = ui.my;
    int result = -1, consumed = 0, title = -1;
    if (ui.my >= 0 && ui.my < UI_MENU_H)
        for (int i = 0; i < m_n; i++) if (ui.mx >= m_tx[i] && ui.mx < m_tx[i] + m_tw[i]) title = i;
    if (m_open >= 0) {
        struct mitem it[24]; int w, cnt = menu_items(m_open, it, 24, &w);
        int dx = m_tx[m_open];
        if (title >= 0 && title != m_open && ui.moved) { m_open = title; m_hover = -1; }
        m_hover = -1;
        for (int i = 0; i < cnt; i++)
            if (!it[i].sep && !it[i].dis && ui.mx >= dx && ui.mx < dx + w && ui.my >= it[i].y && ui.my < it[i].y + it[i].h) m_hover = i;
        if (ui.key == KEY_ESC) { m_open = -1; ui.key = 0; }
        if (ui.pressed) {
            if (title == m_open) m_open = -1;
            else if (m_hover < 0 && !(ui.mx >= dx && ui.mx < dx + w && ui.my >= UI_MENU_H && ui.my < it[cnt - 1].y + it[cnt - 1].h + 4)) m_open = title >= 0 ? title : -1;
        }
        if (ui.released && m_open >= 0 && m_hover >= 0) { result = m_open * 100 + it[m_hover].index; m_open = -1; }
        consumed = 1;
    } else if (ui.pressed && title >= 0) { m_open = title; m_hover = -1; consumed = 1; }
    if (ui.my >= 0 && ui.my < UI_MENU_H) consumed = 1;
    if (consumed) { ui.pressed = ui.released = ui.rpressed = ui.dbl = 0; ui.mx = ui.my = -10000; if (m_open >= 0) ui.key = 0; }
    return result;
}

void ui_menubar_draw(void) {
    ui_noclip();
    ui_fill(0, 0, ui.w, UI_MENU_H, UI_WHITE);
    ui_fill(0, UI_MENU_H - 1, ui.w, 1, 0xE5E7EB);
    for (int i = 0; i < m_n; i++) {
        char t[32]; menu_part(m_menus[i], 0, t, sizeof t);
        int hot = m_my >= 0 && m_my < UI_MENU_H && m_mx >= m_tx[i] && m_mx < m_tx[i] + m_tw[i];
        if (i == m_open || hot) ui_rfill(m_tx[i], 2, m_tw[i], UI_MENU_H - 5, 4, i == m_open ? UI_ACCENT_LIGHT : 0xF3F4F6);
        ui_text(m_tx[i] + 10, 3, t, UI_TEXT);
    }
    if (m_open < 0) return;
    struct mitem it[24]; int w, cnt = menu_items(m_open, it, 24, &w);
    int dx = m_tx[m_open], dy = UI_MENU_H, dh = it[cnt - 1].y + it[cnt - 1].h + 4 - dy;
    if (dx + w > ui.w - 2) dx = ui.w - 2 - w;
    for (int s = 1; s <= 3; s++) ui_rfill(dx + s, dy + s, w, dh, 6, ui_mix(0x000000, 0xFFFFFF, 200 + s * 16));  /* soft shadow */
    ui_rfill(dx, dy, w, dh, 6, UI_BORDER);
    ui_rfill(dx + 1, dy + 1, w - 2, dh - 2, 5, UI_WHITE);
    for (int i = 0; i < cnt; i++) {
        struct mitem *m = &it[i];
        if (m->sep) { ui_fill(dx + 10, m->y + 4, w - 20, 1, 0xE5E7EB); continue; }
        int hot = i == m_hover;
        if (hot) ui_rfill(dx + 4, m->y + 1, w - 8, m->h - 2, 4, UI_ACCENT);
        uint32_t fg = m->dis ? 0xB0B5BD : hot ? UI_WHITE : UI_TEXT;
        if (m->tick) { ui_line(dx + 11, m->y + 13, dx + 14, m->y + 16, 2, fg); ui_line(dx + 14, m->y + 16, dx + 20, m->y + 9, 2, fg); }
        ui_text(dx + 28, m->y + 5, m->text, fg);
        if (m->key[0]) ui_text(dx + w - 12 - ui_text_w(m->key), m->y + 5, m->key, hot ? 0xDBEAFE : UI_MUTED);
    }
}

/* ---------------- muting input (while a dialog is open) ---------------- */
static struct ui_state muted;
void ui_mute(void) {
    muted = ui;
    ui.pressed = ui.released = ui.rpressed = ui.dbl = 0; ui.key = 0; ui.mx = ui.my = -10000;
}
void ui_unmute(void) {
    ui.pressed = muted.pressed; ui.released = muted.released; ui.rpressed = muted.rpressed;
    ui.dbl = muted.dbl; ui.key = muted.key; ui.mx = muted.mx; ui.my = muted.my;
}

/* ---------------- dialog ---------------- */
int ui_dialog(const char *title, const char *line1, const char *line2, const char *btn1, const char *btn2) {
    ui_noclip();
    for (int i = 0; i < ui.w * ui.h; i++) { uint32_t p = ui.px[i]; ui.px[i] = (p >> 1) & 0x7F7F7F; }   /* dim the window */
    int w = 380, h = 160;
    int l1 = ui_text_w(line1), l2 = line2 ? ui_text_w(line2) : 0;
    if (l1 + 48 > w) w = l1 + 48;
    if (l2 + 48 > w) w = l2 + 48;
    if (w > ui.w - 20) w = ui.w - 20;
    int x = (ui.w - w) / 2, y = (ui.h - h) / 2;
    ui_rfill(x + 3, y + 4, w, h, 10, 0x202020);
    ui_rfill(x, y, w, h, 10, UI_WHITE);
    ui_textb(x + 24, y + 20, title, UI_TEXT);
    ui_text(x + 24, y + 52, line1, 0x374151);
    if (line2) ui_text(x + 24, y + 72, line2, 0x374151);
    ui_fill(x, y + h - 54, w, 1, 0xE5E7EB);
    ui_rfill(x, y + h - 53, w, 53, 10, 0xF9FAFB);
    ui_fill(x, y + h - 53, w, 10, 0xF9FAFB);
    int r = -1, bw = 100;
    if (btn2) {
        if (ui_button(9002, x + w - 16 - bw, y + h - 42, bw, 30, btn2)) r = 1;
        if (ui_button_primary(9001, x + w - 26 - 2 * bw, y + h - 42, bw, 30, btn1)) r = 0;
    } else if (ui_button_primary(9001, x + w - 16 - bw, y + h - 42, bw, 30, btn1)) r = 0;
    if (ui.key == '\n') { r = 0; ui.key = 0; }
    if (ui.key == KEY_ESC) { r = btn2 ? 1 : 0; ui.key = 0; }
    if (r >= 0) ui_redraw();                                /* show the window without the box at once */
    return r;
}
