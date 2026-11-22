/* music.c - Music player for .wav files in C:\Music (or one file given to it).
 * Click a song or use the buttons. Keys: Space play/stop, Up/Down choose,
 * Enter play, Left/Right previous/next, + and - volume, Esc quit.
 * The program reads the WAV file, finds its format and sound data, and hands
 * the samples to the kernel's Sound Blaster driver with play(). */
#include "myos.h"

#define W 520
#define H 400
#define LIST_Y 190
#define ROW 22
#define MAXT 32

static struct dirent songs[MAXT];
static int nsongs, sel, cur = -1, is_playing;
static char folder[256] = "/Music";
static uint8_t *file;                 /* the loaded WAV file */
static uint8_t *pcm; static int pcm_bytes, rate, bits, chans, total_ms;
static char msg[80];

static int ends_wav(const char *n) {
    int k = strlen(n);
    return k > 4 && n[k - 4] == '.' && (n[k - 3] | 32) == 'w' && (n[k - 2] | 32) == 'a' && (n[k - 1] | 32) == 'v';
}
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void scopy(char *d, const char *s, int max) { int i = 0; for (; s[i] && i < max - 1; i++) d[i] = s[i]; d[i] = 0; }
static void path_of(int i, char *out) {
    int k = 0;
    for (const char *p = folder; *p; p++) out[k++] = *p;
    if (k > 1) out[k++] = '/';
    for (const char *p = songs[i].name; *p; p++) out[k++] = *p;
    out[k] = 0;
}
static void mmss(int ms, char *b) {
    int s = ms / 1000; char t[12];
    utoa(s / 60, b); int k = strlen(b); b[k++] = ':';
    utoa(s % 60, t); if (s % 60 < 10) b[k++] = '0';
    for (int i = 0; t[i]; i++) b[k++] = t[i];
    b[k] = 0;
}

/* Parse a WAV file: "RIFF" header, a "fmt " chunk with the format, a "data" chunk with samples. */
static int parse_wav(uint8_t *f, int n) {
    if (n < 44 || f[0] != 'R' || f[1] != 'I' || f[2] != 'F' || f[3] != 'F' || f[8] != 'W' || f[9] != 'A') return 0;
    int p = 12, got_fmt = 0;
    while (p + 8 <= n) {
        uint32_t len = rd32(f + p + 4);
        if (f[p] == 'f' && f[p + 1] == 'm' && f[p + 2] == 't') {
            if ((f[p + 8] | f[p + 9] << 8) != 1) return 0;        /* only plain PCM */
            chans = f[p + 10]; rate = rd32(f + p + 12); bits = f[p + 22];
            got_fmt = 1;
        } else if (f[p] == 'd' && f[p + 1] == 'a' && f[p + 2] == 't' && f[p + 3] == 'a') {
            if (!got_fmt) return 0;
            pcm = f + p + 8;
            pcm_bytes = (int)len > n - p - 8 ? n - p - 8 : (int)len;
            total_ms = pcm_bytes / (bits / 8 * chans) * 1000 / rate;
            return 1;
        }
        p += 8 + len + (len & 1);
    }
    return 0;
}

static void play_song(int i) {
    stopsound();
    if (file) { free(file); file = 0; }
    is_playing = 0;
    cur = i; sel = i;
    char path[256]; path_of(i, path);
    int size = songs[i].size;
    file = malloc(size + 1);
    if (!file || readfile(path, file, size) != size) { scopy(msg, "Could not read the file.", 80); return; }
    if (!parse_wav(file, size)) { scopy(msg, "Not a PCM .wav file.", 80); return; }
    if (play(pcm, pcm_bytes, rate, bits, chans) < 0) {
        scopy(msg, "No sound card found (start QEMU with -device sb16).", 80);
        return;
    }
    is_playing = 1;
    char b[16];
    char *m = msg; const char *a = "PCM "; while (*a) *m++ = *a++;
    utoa(rate, b); for (char *q = b; *q; q++) *m++ = *q;
    a = " Hz, "; while (*a) *m++ = *a++;
    utoa(bits, b); for (char *q = b; *q; q++) *m++ = *q;
    a = chans == 2 ? "-bit stereo" : "-bit mono"; while (*a) *m++ = *a++;
    *m = 0;
}

/* ---------- drawing ---------- */
static void btn(int x, int y, int w, const char *t, uint32_t bg) {
    fill(x, y, w, 30, bg);
    text(x + (w - strlen(t) * 8) / 2, y + 7, t, 0xFFFFFF);
}

static void draw_bars(int pos_ms) {          /* little level meter from the actual samples */
    fill(20, 104, W - 40, 40, 0x0F172A);
    if (!is_playing || pos_ms < 0) return;
    int bps = bits / 8 * chans, frames = pcm_bytes / bps;
    int at = pos_ms / 1000 * rate + pos_ms % 1000 * rate / 1000;
    for (int b = 0; b < 30; b++) {
        int start = at + b * 64, sum = 0;
        for (int k = 0; k < 64; k++) {
            int f = start + k; if (f >= frames) break;
            uint8_t *s = pcm + f * bps;
            int v = bits == 16 ? (int16_t)(s[0] | s[1] << 8) : (s[0] - 128) << 8;
            sum += v < 0 ? -v : v;
        }
        int hgt = sum / 64 / 600; if (hgt > 38) hgt = 38;
        fill(24 + b * 16, 142 - hgt, 12, hgt + 1, 0x38BDF8);
    }
}

static void draw_top(void) {
    char b[32], t[32];
    fill(0, 0, W, LIST_Y - 10, 0x1E293B);
    text(20, 16, cur >= 0 ? songs[cur].name : "No song playing", 0xF8FAFC);
    text(20, 38, msg, 0x94A3B8);
    int pos = soundpos();
    if (is_playing && pos < 0) is_playing = 0;           /* song finished */
    fill(20, 66, W - 40, 8, 0x334155);
    if (is_playing && total_ms) fill(20, 66, (W - 40) * (pos > total_ms ? total_ms : pos) / total_ms, 8, 0x38BDF8);
    mmss(is_playing ? pos : 0, b); mmss(total_ms, t);
    text(20, 80, b, 0xCBD5E1); text(W - 20 - strlen(t) * 8, 80, t, 0xCBD5E1);
    draw_bars(pos);
    btn(20, 150, 70, "Prev", 0x334155);
    btn(96, 150, 90, is_playing ? "Stop" : "Play", 0x2563EB);
    btn(192, 150, 70, "Next", 0x334155);
    btn(W - 160, 150, 36, "-", 0x334155);
    char v[16]; utoa(volume(-1), v); int k = strlen(v); v[k] = '%'; v[k + 1] = 0;
    fill(W - 120, 150, 64, 30, 0x1E293B); text(W - 88 - strlen(v) * 4, 157, v, 0xF8FAFC);
    btn(W - 56, 150, 36, "+", 0x334155);
}

static void draw_list(void) {
    fill(0, LIST_Y - 10, W, H - LIST_Y + 10, 0x0F172A);
    text(20, LIST_Y - 6, "Songs in ", 0x64748B); text(92, LIST_Y - 6, folder, 0x64748B);
    for (int i = 0; i < nsongs && LIST_Y + 14 + (i + 1) * ROW <= H; i++) {
        int y = LIST_Y + 14 + i * ROW;
        if (i == sel) fill(12, y - 2, W - 24, ROW, 0x1E3A8A);
        text(24, y + 1, i == cur && is_playing ? ">" : " ", 0x38BDF8);
        text(40, y + 1, songs[i].name, i == cur ? 0x7DD3FC : 0xE2E8F0);
        char b[16]; utoa(songs[i].size / 1024, b); int k = strlen(b); b[k] = ' '; b[k + 1] = 'K'; b[k + 2] = 'B'; b[k + 3] = 0;
        text(W - 30 - strlen(b) * 8, y + 1, b, 0x64748B);
    }
    if (!nsongs) text(40, LIST_Y + 20, "No .wav files here.", 0x94A3B8);
}

static void load_list(void) {
    static struct dirent all[64];
    int n = listdir(folder, all, 64);
    nsongs = 0;
    for (int i = 0; i < n && nsongs < MAXT; i++) if (!all[i].is_dir && ends_wav(all[i].name)) songs[nsongs++] = all[i];
}

int main(void) {
    settitle("Music");
    resize(W, H);
    scopy(msg, "Choose a song.", 80);
    char arg[256];
    int start = -1;
    if (getarg(arg, sizeof arg) > 0) {               /* opened with a file: use its folder */
        int slash = 0; for (int i = 0; arg[i]; i++) if (arg[i] == '/') slash = i;
        if (slash > 0) { for (int i = 0; i < slash; i++) folder[i] = arg[i]; folder[slash] = 0; }
        else scopy(folder, "/", 256);
        load_list();
        for (int i = 0; i < nsongs; i++) {
            const char *a = songs[i].name, *b = arg + slash + 1; int same = 1;
            while (*a || *b) { if ((*a | 32) != (*b | 32)) { same = 0; break; } a++; b++; }
            if (same) start = i;
        }
    } else load_list();
    if (start >= 0) play_song(start);
    draw_list(); draw_top();

    struct event e;
    uint32_t last = 0;
    for (;;) {
        if (!pollevent(&e)) {
            if (ticks() - last >= 100) {               /* refresh progress ~10 times a second */
                last = ticks();
                int was = is_playing;
                draw_top();
                if (was && !is_playing) {             /* finished: go on to the next song */
                    if (cur + 1 < nsongs) play_song(cur + 1);
                    draw_list(); draw_top();
                }
            }
            sleep(20);
            continue;
        }
        int redraw_list = 0;
        if (e.type == EV_KEY) {
            int k = e.key;
            if (k == KEY_ESC) { stopsound(); return 0; }
            if (k == ' ') { if (is_playing) { stopsound(); is_playing = 0; } else if (nsongs) play_song(cur >= 0 ? cur : sel); }
            if (k == KEY_UP && sel > 0) sel--;
            if (k == KEY_DOWN && sel < nsongs - 1) sel++;
            if (k == '\n' && nsongs) play_song(sel);
            if (k == KEY_RIGHT && cur + 1 < nsongs) play_song(cur + 1);
            if (k == KEY_LEFT && cur > 0) play_song(cur - 1);
            if (k == '+' || k == '=') volume(volume(-1) + 10);
            if (k == '-') volume(volume(-1) - 10);
            redraw_list = 1;
        } else if (e.type == EV_MOUSE_DOWN) {
            int x = e.x, y = e.y;
            if (y >= 150 && y < 180) {
                if (x >= 20 && x < 90 && cur > 0) play_song(cur - 1);
                if (x >= 96 && x < 186) { if (is_playing) { stopsound(); is_playing = 0; } else if (nsongs) play_song(cur >= 0 ? cur : sel); }
                if (x >= 192 && x < 262 && cur + 1 < nsongs) play_song(cur + 1);
                if (x >= W - 160 && x < W - 124) volume(volume(-1) - 10);
                if (x >= W - 56 && x < W - 20) volume(volume(-1) + 10);
            }
            int i = (y - LIST_Y - 12) / ROW;
            if (y >= LIST_Y + 12 && i >= 0 && i < nsongs) { if (i == sel) play_song(i); else sel = i; }
            redraw_list = 1;
        }
        if (redraw_list) { draw_list(); draw_top(); }
    }
}
