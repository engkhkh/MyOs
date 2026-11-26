/* theme.h - dark mode, shared by the kernel's drawing (gfx.c) and the toolkit (apps/ui.c).
 *
 * Instead of a second set of colours for every screen, dark mode maps colours while
 * they are drawn:
 *   - strong colours (the accent blue, red, green...) stay as they are;
 *   - greys turn around: white backgrounds become dark grey, dark text becomes light;
 *   - pale tints (a light-blue selection, a pale yellow...) become dark tints of the same hue.
 * Only the parts that are meant to follow the theme switch it on (window contents,
 * menus, panels); the taskbar, the Terminal, pictures and card games keep their colours. */
#ifndef THEME_H
#define THEME_H
#include <stdint.h>

static inline uint32_t theme_dark_map(uint32_t c) {
    static uint32_t from[64], to[64];
    static uint8_t used[64];
    unsigned slot = (c * 2654435761u) >> 26;
    if (used[slot] && from[slot] == c) return to[slot];
    int r = c >> 16 & 255, g = c >> 8 & 255, b = c & 255;
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int sat = mx - mn, l = (mx + mn) / 2;
    uint32_t out;
    if (sat > 48 && mx < 235) out = c;                          /* a strong colour: keep */
    else if (sat > 20 && l > 140) {                             /* a pale tint: a dark one of the same hue */
        int br = 0x11, bg = 0x18, bb = 0x27, t = 72;
        out = (uint32_t)((br * (256 - t) + r * t) >> 8) << 16 | (uint32_t)((bg * (256 - t) + g * t) >> 8) << 8 | (uint32_t)((bb * (256 - t) + b * t) >> 8);
    } else {                                                    /* grey: turn the lightness around */
        int nl = 232 - l * 82 / 100;
        int nr = nl - 4, ng = nl - 1, nb = nl + 6;              /* with a touch of blue, like the taskbar */
        if (nr < 0) nr = 0;
        if (nb > 255) nb = 255;
        out = (uint32_t)nr << 16 | (uint32_t)ng << 8 | (uint32_t)nb;
    }
    from[slot] = c; to[slot] = out; used[slot] = 1;
    return out;
}
#endif
