/* settings.c - user settings, saved as plain text in C:\settings.ini.
 *
 * The file looks like this (you can also edit it in Notepad):
 *     wallpaper=1
 *     picture=/Pictures/Sunset.bmp
 *     accent=2
 *     timezone=2
 *     volume=80
 *     chime=1
 *     mouse=2
 *     explorer_view=1      (File Explorer: 0 details, 1 icons)
 * Unknown lines are ignored and missing ones keep their defaults, so old or
 * hand-edited files never break anything. */
#include <stdint.h>
#include "kernel.h"

#define FILE "/settings.ini"

struct settings cfg = { 0, "", 0, 0, 100, 1, 2, 1 };

static int starts(const char *s, const char *p) { while (*p) if (*s++ != *p++) return 0; return 1; }
static int num(const char *s) { int neg = 0, v = 0; if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++; while (*s >= '0' && *s <= '9') v = v * 10 + *s++ - '0'; return neg ? -v : v; }
static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void settings_load(void) {
    if (!fs_mounted()) return;
    char buf[1024];
    int n = fs_read(FILE, buf, sizeof buf - 1);
    if (n <= 0) return;
    buf[n] = 0;
    for (char *l = buf; *l;) {
        char *e = l; while (*e && *e != '\n' && *e != '\r') e++;
        char save = *e; *e = 0;
        if (starts(l, "wallpaper=")) cfg.wallpaper = clamp(num(l + 10), 0, WALLPAPER_PRESETS);
        else if (starts(l, "picture=")) { int k = 0; for (char *v = l + 8; *v && k < FS_PATH_MAX - 1; v++) cfg.picture[k++] = *v; cfg.picture[k] = 0; }
        else if (starts(l, "accent=")) cfg.accent = clamp(num(l + 7), 0, ACCENT_COUNT - 1);
        else if (starts(l, "timezone=")) cfg.timezone = clamp(num(l + 9), -12, 14);
        else if (starts(l, "volume=")) cfg.volume = clamp(num(l + 7), 0, 100);
        else if (starts(l, "chime=")) cfg.chime = num(l + 6) != 0;
        else if (starts(l, "mouse=")) cfg.mouse = clamp(num(l + 6), 1, 4);
        else if (starts(l, "explorer_view=")) cfg.explorer_view = clamp(num(l + 14), 0, 1);
        *e = save;
        l = e; while (*l == '\n' || *l == '\r') l++;
    }
}

static char *put(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *putn(char *p, int v) {
    if (v < 0) { *p++ = '-'; v = -v; }
    char t[12]; int k = 0; do t[k++] = '0' + v % 10; while (v /= 10);
    while (k) *p++ = t[--k];
    return p;
}

int settings_save(void) {
    if (!fs_mounted()) return 0;
    char buf[1024], *p = buf;
    p = put(p, "; MyOS settings - changed by the Settings app, safe to edit by hand\r\n");
    p = put(p, "wallpaper="); p = putn(p, cfg.wallpaper); p = put(p, "\r\n");
    p = put(p, "picture="); p = put(p, cfg.picture); p = put(p, "\r\n");
    p = put(p, "accent="); p = putn(p, cfg.accent); p = put(p, "\r\n");
    p = put(p, "timezone="); p = putn(p, cfg.timezone); p = put(p, "\r\n");
    p = put(p, "volume="); p = putn(p, cfg.volume); p = put(p, "\r\n");
    p = put(p, "chime="); p = putn(p, cfg.chime); p = put(p, "\r\n");
    p = put(p, "mouse="); p = putn(p, cfg.mouse); p = put(p, "\r\n");
    p = put(p, "explorer_view="); p = putn(p, cfg.explorer_view); p = put(p, "\r\n");
    return fs_write(FILE, buf, p - buf);
}

/* Push the non-visual settings into the rest of the system. */
void settings_apply(void) {
    rtc_set_tz(cfg.timezone);
    sound_set_volume(cfg.volume);
}
