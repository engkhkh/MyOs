/* myos.h - the MyOS programming library for user programs.
 * Each function is a thin wrapper around one system call (see syscalls.h). */
#ifndef MYOS_H
#define MYOS_H
#include <stdint.h>
#include "../syscalls.h"

static inline int sys(int n, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e) : "memory");
    return r;
}

/* keys above 255 */
#define KEY_ESC   27
#define KEY_UP    0x102
#define KEY_DOWN  0x103
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
#define KEY_ALTENTER 0x110
#define KEY_MENU 0x111
#define KEY_CTRL(c) (0x200 | (c))

static inline void exit(int code)            { sys(SYS_EXIT, code, 0, 0, 0, 0); }
static inline void print(const char *s)      { sys(SYS_PRINT, (uint32_t)s, 0, 0, 0, 0); }
static inline int  getkey(void)              { return sys(SYS_GETKEY, 0, 0, 0, 0, 0); }
static inline int  pollkey(void)             { return sys(SYS_POLLKEY, 0, 0, 0, 0, 0); }
static inline void sleep(int ms)             { sys(SYS_SLEEP, ms, 0, 0, 0, 0); }
static inline uint32_t ticks(void)           { return sys(SYS_TICKS, 0, 0, 0, 0, 0); }
static inline void fill(int x, int y, int w, int h, uint32_t c) { sys(SYS_FILL, x, y, w, h, c); }
static inline void text(int x, int y, const char *s, uint32_t c) { sys(SYS_TEXT, x, y, (uint32_t)s, c, 0); }
static inline void bigtext(int x, int y, const char *s, uint32_t c, int scale) { sys(SYS_TEXT, x, y, (uint32_t)s, c, scale); }   /* scale 2-4 */
static inline void clear(uint32_t c)         { sys(SYS_CLEAR, c, 0, 0, 0, 0); }
static inline void setcolor(uint32_t fg, uint32_t bg) { sys(SYS_SETCOLOR, fg, bg, 0, 0, 0); }
static inline int  getpid(void)              { return sys(SYS_GETPID, 0, 0, 0, 0, 0); }
static inline int  readfile(const char *p, void *buf, int max) { return sys(SYS_READFILE, (uint32_t)p, (uint32_t)buf, max, 0, 0); }
static inline int  writefile(const char *p, const void *buf, int n) { return sys(SYS_WRITEFILE, (uint32_t)p, (uint32_t)buf, n, 0, 0); }
static inline void gettime(int t[6])         { sys(SYS_TIME, (uint32_t)t, 0, 0, 0, 0); }
static inline void yield(void)               { sys(SYS_YIELD, 0, 0, 0, 0, 0); }
static inline int  canvas_w(void)            { return (unsigned)sys(SYS_CANVAS, 0, 0, 0, 0, 0) >> 16; }
static inline int  canvas_h(void)            { return sys(SYS_CANVAS, 0, 0, 0, 0, 0) & 0xFFFF; }
static inline void settitle(const char *s)   { sys(SYS_SETTITLE, (uint32_t)s, 0, 0, 0, 0); }
static inline void *sbrk(int bytes)          { return (void *)sys(SYS_SBRK, bytes, 0, 0, 0, 0); }
static inline int  resize(int w, int h)      { return sys(SYS_RESIZE, w, h, 0, 0, 0); }
/* Copy a w x h block of pixels to (x, y) in the window. stride = pixels per source row. */
static inline void blit(int x, int y, int w, int h, const uint32_t *px, int stride) {
    sys(SYS_BLIT, (x & 0xFFFF) | ((uint32_t)y << 16), (w & 0xFFFF) | ((uint32_t)h << 16), (uint32_t)px, stride, 0);
}
/* Wait for the next key or mouse event (see struct event in syscalls.h). */
static inline void getevent(struct event *e) { while (sys(SYS_GETEVENT, (uint32_t)e, 0, 0, 0, 0) != 1) {} }
static inline int  pollevent(struct event *e) { return sys(SYS_POLLEVENT, (uint32_t)e, 0, 0, 0, 0) == 1; }

/* Sound */
static inline void tone(int freq, int ms)    { sys(SYS_TONE, freq, ms, 0, 0, 0); }     /* PC speaker */
static inline int  play(const void *pcm, int bytes, int rate, int bits, int channels) {
    return sys(SYS_PLAY, (uint32_t)pcm, bytes, rate, bits | channels << 8, 0);         /* Sound Blaster */
}
static inline void stopsound(void)           { sys(SYS_STOPSOUND, 0, 0, 0, 0, 0); }
static inline int  soundpos(void)            { return sys(SYS_SOUNDPOS, 0, 0, 0, 0, 0); }
static inline int  volume(int v)             { return sys(SYS_VOLUME, v, 0, 0, 0, 0); }
/* Files & arguments */
static inline int  getarg(char *buf, int max) { return sys(SYS_GETARG, (uint32_t)buf, max, 0, 0, 0); }
static inline int  listdir(const char *path, struct dirent *out, int max) { return sys(SYS_LISTDIR, (uint32_t)path, (uint32_t)out, max, 0, 0); }

/* Network */
static inline int resolve(const char *name, unsigned *ip) { return sys(SYS_RESOLVE, (uint32_t)name, (uint32_t)ip, 0, 0, 0); }
static inline int tcpopen(unsigned ip, int port)   { return sys(SYS_TCPOPEN, ip, port, 0, 0, 0); }
static inline int tcpstatus(int c)                 { return sys(SYS_TCPSTATUS, c, 0, 0, 0, 0); }
static inline int tcpsend(int c, const void *d, int n) { return sys(SYS_TCPSEND, c, (uint32_t)d, n, 0, 0); }
static inline int tcprecv(int c, void *b, int max) { return sys(SYS_TCPRECV, c, (uint32_t)b, max, 0, 0); }
static inline void tcpclose(int c)                 { sys(SYS_TCPCLOSE, c, 0, 0, 0, 0); }
static inline int netinfo(unsigned out[4])         { return sys(SYS_NETINFO, (uint32_t)out, 0, 0, 0, 0); }
static inline void clipset(const char *s, int len)  { sys(SYS_CLIPSET, (uint32_t)s, (uint32_t)len, 0, 0, 0); }
static inline void getfont(uint8_t *buf, int bold) { sys(SYS_FONT, (uint32_t)buf, (uint32_t)bold, 0, 0, 0); }
static inline int set_wallpaper(const char *path) { return sys(SYS_WALLPAPER, (uint32_t)path, 0, 0, 0, 0); }
static inline void resizable(int min_w, int min_h) { sys(SYS_RESIZABLE, (uint32_t)min_w, (uint32_t)min_h, 0, 0, 0); }
static inline int clipget(char *buf, int max)       { return sys(SYS_CLIPGET, (uint32_t)buf, (uint32_t)max, 0, 0, 0); }

/* Memory: provided by malloc.c, which is linked into every program. */
void *malloc(uint32_t size);
void *calloc(uint32_t n, uint32_t size);
void free(void *p);
void *memset(void *d, int c, uint32_t n);
void *memcpy(void *d, const void *s, uint32_t n);

/* ---- small helpers (no C library here!) ---- */
static inline int strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static inline char *utoa(unsigned v, char *buf) {          /* number -> text */
    char t[12]; int k = 0;
    do t[k++] = '0' + v % 10; while (v /= 10);
    int i = 0; while (k) buf[i++] = t[--k];
    buf[i] = 0; return buf;
}
static inline void printnum(int v) { char b[12]; if (v < 0) { print("-"); v = -v; } print(utoa(v, b)); }
static inline int atoi(const char *s) { int v = 0, neg = 0; while (*s == ' ') s++; if (*s == '-') { neg = 1; s++; } while (*s >= '0' && *s <= '9') v = v * 10 + *s++ - '0'; return neg ? -v : v; }

/* Read a line of typed text (with backspace), echoing it. Returns its length. */
static inline int readline(char *buf, int max) {
    int n = 0;
    for (;;) {
        int k = getkey();
        if (k == '\n') { print("\n"); buf[n] = 0; return n; }
        if (k == '\b') { if (n) { n--; print("\b"); } continue; }
        if (k >= 32 && k < 127 && n < max - 1) { char s[2] = { (char)k, 0 }; buf[n++] = (char)k; print(s); }
    }
}

static uint32_t rand_state __attribute__((unused)) = 12345;
static inline void srand(uint32_t s) { rand_state = s ? s : 1; }
static inline uint32_t rand(void) { rand_state ^= rand_state << 13; rand_state ^= rand_state >> 17; rand_state ^= rand_state << 5; return rand_state; }
#endif
