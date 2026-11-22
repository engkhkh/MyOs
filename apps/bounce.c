/* bounce.c - animation: balls bouncing around the window. Press Q or Esc to quit. */
#include "myos.h"

#define R 12
static int half[2 * R + 1];             /* width of the ball on each row */

static int isqrt(int v) { int r = 0; while ((r + 1) * (r + 1) <= v) r++; return r; }
static void ball(int x, int y, uint32_t c) {
    for (int dy = -R; dy <= R; dy++) fill(x - half[dy + R], y + dy, 2 * half[dy + R] + 1, 1, c);
}

int main(void) {
    settitle("Bouncing balls");
    for (int dy = -R; dy <= R; dy++) half[dy + R] = isqrt(R * R - dy * dy);
    int W = canvas_w(), H = canvas_h() - 24;
    const uint32_t bg = 0x0F172A;
    clear(bg);
    struct { int x, y, dx, dy; uint32_t c; } b[4] = {
        { 60, 60, 4, 3, 0xF87171 }, { 200, 120, -3, 4, 0x60A5FA },
        { 320, 80, 5, -2, 0x4ADE80 }, { 120, 200, -4, -5, 0xFACC15 } };
    srand(ticks());
    uint32_t frames = 0, t0 = ticks();
    char buf[64];
    for (;;) {
        int k = pollkey();
        if (k == 'q' || k == 'Q' || k == KEY_ESC) return 0;
        for (int i = 0; i < 4; i++) {
            ball(b[i].x, b[i].y, bg);                       /* erase */
            b[i].x += b[i].dx; b[i].y += b[i].dy;
            if (b[i].x < R || b[i].x > W - R) { b[i].dx = -b[i].dx; b[i].x += 2 * b[i].dx; }
            if (b[i].y < R || b[i].y > H - R) { b[i].dy = -b[i].dy; b[i].y += 2 * b[i].dy; }
            ball(b[i].x, b[i].y, b[i].c);                   /* draw */
        }
        frames++;
        fill(0, H, W, 24, 0x1E293B);
        char *p = buf; const char *a = "Frames: "; while (*a) *p++ = *a++;
        utoa(frames, p);
        text(8, H + 4, buf, 0xCBD5E1);
        uint32_t secs = (ticks() - t0) / 1000;
        if (secs) { p = buf; a = "FPS: "; while (*a) *p++ = *a++; utoa(frames / secs, p); text(160, H + 4, buf, 0xCBD5E1); }
        text(W - 112, H + 4, "Q = quit", 0x94A3B8);
        sleep(33);                                          /* about 30 frames a second */
    }
}
