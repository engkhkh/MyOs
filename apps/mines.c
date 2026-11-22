/* mines.c - Minesweeper. Left click opens a square, right click plants a flag.
 * Numbers tell how many mines touch that square. Open every safe square to win.
 * Click the face to start a new game. */
#include "myos.h"

#define N     16               /* board is N x N */
#define MINES 40
#define CS    26               /* cell size in pixels */
#define TOP   56
#define PAD   12
#define WIN_W (N * CS + 2 * PAD)
#define WIN_H (TOP + N * CS + PAD)

static int mine[N][N], open_[N][N], flag[N][N], near_[N][N];
static int state, opened, flags, first_click;    /* state: 0 playing, 1 lost, 2 won */
static uint32_t start_ms, shown_secs;

static const uint32_t numcol[9] = { 0, 0x2563EB, 0x16A34A, 0xDC2626, 0x1E3A8A, 0x7F1D1D, 0x0E7490, 0x111827, 0x6B7280 };

static void draw_cell(int x, int y) {
    int px = PAD + x * CS, py = TOP + y * CS;
    if (!open_[y][x]) {
        fill(px, py, CS, CS, 0x9CA3AF);
        fill(px, py, CS - 1, CS - 1, 0xF3F4F6);
        fill(px + 2, py + 2, CS - 4, CS - 4, 0xD1D5DB);
        if (flag[y][x]) {
            fill(px + 9, py + 6, 2, 14, 0x111827);
            fill(px + 11, py + 6, 8, 6, 0xDC2626);
        } else if (state == 1 && mine[y][x]) {
            fill(px + 7, py + 7, 12, 12, 0x111827);
        }
        return;
    }
    fill(px, py, CS, CS, 0xBDC3CB);
    fill(px + 1, py + 1, CS - 1, CS - 1, mine[y][x] ? 0xEF4444 : 0xE5E7EB);
    if (mine[y][x]) fill(px + 7, py + 7, 12, 12, 0x111827);
    else if (near_[y][x]) { char s[2] = { '0' + near_[y][x], 0 }; text(px + 9, py + 5, s, numcol[near_[y][x]]); }
}

static void draw_header(void) {
    char b[16];
    fill(0, 0, WIN_W, TOP - 6, 0x374151);
    fill(PAD, 12, 60, 28, 0x111827);
    int left = MINES - flags; if (left < 0) left = 0;
    text(PAD + 10, 18, utoa(left, b), 0xF87171);
    fill(WIN_W - PAD - 60, 12, 60, 28, 0x111827);
    text(WIN_W - PAD - 50, 18, utoa(shown_secs, b), 0xF87171);
    int fx = WIN_W / 2 - 16;                         /* the face button */
    fill(fx, 9, 32, 32, 0xFACC15);
    fill(fx + 9, 18, 4, 4, 0x111827); fill(fx + 19, 18, 4, 4, 0x111827);
    if (state == 1) { fill(fx + 10, 29, 12, 3, 0x111827); }                       /* sad */
    else if (state == 2) { fill(fx + 8, 27, 16, 3, 0x111827); fill(fx + 6, 17, 20, 3, 0x111827); } /* cool */
    else { fill(fx + 9, 28, 14, 3, 0x111827); fill(fx + 8, 26, 2, 2, 0x111827); fill(fx + 22, 26, 2, 2, 0x111827); }
}

static void draw_all(void) {
    fill(0, 0, WIN_W, WIN_H, 0x4B5563);
    draw_header();
    for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) draw_cell(x, y);
}

static void new_game(void) {
    memset(mine, 0, sizeof mine); memset(open_, 0, sizeof open_);
    memset(flag, 0, sizeof flag); memset(near_, 0, sizeof near_);
    state = 0; opened = 0; flags = 0; first_click = 1; shown_secs = 0;
    draw_all();
}

/* Mines are placed after the first click, so the first square is always safe. */
static void lay_mines(int sx, int sy) {
    for (int placed = 0; placed < MINES;) {
        int x = rand() % N, y = rand() % N;
        if (mine[y][x] || (x >= sx - 1 && x <= sx + 1 && y >= sy - 1 && y <= sy + 1)) continue;
        mine[y][x] = 1; placed++;
    }
    for (int y = 0; y < N; y++) for (int x = 0; x < N; x++)
        for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
            int ax = x + dx, ay = y + dy;
            if (ax >= 0 && ay >= 0 && ax < N && ay < N && mine[ay][ax]) near_[y][x]++;
        }
}

static void reveal(int x, int y) {          /* opens a square; empty areas spread out */
    static int qx[N * N], qy[N * N];
    int h = 0, t = 0;
    qx[t] = x; qy[t] = y; t++;
    while (h < t) {
        int cx = qx[h], cy = qy[h]; h++;
        if (open_[cy][cx] || flag[cy][cx]) continue;
        open_[cy][cx] = 1; opened++;
        draw_cell(cx, cy);
        if (near_[cy][cx] || mine[cy][cx]) continue;
        for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
            int ax = cx + dx, ay = cy + dy;
            if (ax >= 0 && ay >= 0 && ax < N && ay < N && !open_[ay][ax] && t < N * N) { qx[t] = ax; qy[t] = ay; t++; }
        }
    }
}

int main(void) {
    settitle("Minesweeper");
    resize(WIN_W, WIN_H);
    srand(ticks() * 2654435761u + 7);
    new_game();
    struct event e;
    for (;;) {
        if (!pollevent(&e)) {                         /* idle: update the timer */
            if (state == 0 && !first_click) {
                uint32_t s = (ticks() - start_ms) / 1000;
                if (s != shown_secs && s < 1000) { shown_secs = s; draw_header(); }
            }
            sleep(50);
            continue;
        }
        if (e.type == EV_KEY) { if (e.key == KEY_ESC) return 0; if (e.key == 'n' || e.key == 'N') new_game(); continue; }
        if (e.type != EV_MOUSE_DOWN) continue;
        int fx = WIN_W / 2 - 16;
        if (e.y >= 9 && e.y < 41 && e.x >= fx && e.x < fx + 32) { new_game(); continue; }
        if (state) continue;
        int x = (e.x - PAD) / CS, y = (e.y - TOP) / CS;
        if (e.x < PAD || e.y < TOP || x >= N || y >= N) continue;
        if (e.buttons & 2) {                           /* right button: flag */
            if (!open_[y][x]) { flag[y][x] = !flag[y][x]; flags += flag[y][x] ? 1 : -1; draw_cell(x, y); draw_header(); }
            continue;
        }
        if (flag[y][x] || open_[y][x]) continue;
        if (first_click) { lay_mines(x, y); first_click = 0; start_ms = ticks(); }
        if (mine[y][x]) { state = 1; open_[y][x] = 1; draw_all(); continue; }
        reveal(x, y);
        if (opened == N * N - MINES) {
            state = 2;
            for (int yy = 0; yy < N; yy++) for (int xx = 0; xx < N; xx++) if (mine[yy][xx]) flag[yy][xx] = 1;
            flags = MINES;
            draw_all();
        }
        draw_header();
    }
}
