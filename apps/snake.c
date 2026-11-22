/* snake.c - the classic Snake game. Arrow keys steer, P pauses, Esc quits.
 * Eat the red food to grow; don't hit the walls or yourself. The best score is
 * saved to C:\Apps\snake.hi so it survives a restart. */
#include "myos.h"

#define CELL 20
#define GW   24                 /* grid size in cells */
#define GH   19
#define TOP  40                 /* score bar height */
#define HIFILE "/Apps/snake.hi"

static int sx[GW * GH], sy[GW * GH], len, dir, food_x, food_y, score, best;

static void cell(int x, int y, uint32_t c) { fill(x * CELL + 1, TOP + y * CELL + 1, CELL - 2, CELL - 2, c); }

static void draw_bar(const char *msg) {
    char b[48];
    fill(0, 0, GW * CELL, TOP, 0x14532D);
    text(12, 12, "Score", 0xBBF7D0); text(60, 12, utoa(score, b), 0xFFFFFF);
    text(150, 12, "Best", 0xBBF7D0); text(190, 12, utoa(best, b), 0xFFFFFF);
    text(GW * CELL - strlen(msg) * 8 - 12, 12, msg, 0xFDE68A);
}

static void place_food(void) {
    for (;;) {
        food_x = rand() % GW; food_y = rand() % GH;
        int ok = 1;
        for (int i = 0; i < len; i++) if (sx[i] == food_x && sy[i] == food_y) ok = 0;
        if (ok) break;
    }
    cell(food_x, food_y, 0xEF4444);
}

static void new_game(void) {
    fill(0, TOP, GW * CELL, GH * CELL, 0x052E16);
    len = 4; dir = 1; score = 0;
    for (int i = 0; i < len; i++) { sx[i] = 8 - i; sy[i] = GH / 2; cell(sx[i], sy[i], i ? 0x22C55E : 0x86EFAC); }
    place_food();
    draw_bar("Arrows to steer");
}

static void save_best(void) { char b[12]; utoa(best, b); writefile(HIFILE, b, strlen(b)); }

int main(void) {
    settitle("Snake");
    resize(GW * CELL, TOP + GH * CELL);
    srand(ticks() * 2654435761u);
    char b[12]; int n = readfile(HIFILE, b, 11);
    if (n > 0) { b[n] = 0; best = atoi(b); }
    new_game();
    int paused = 0, over = 0, next_dir = dir;
    uint32_t last = ticks();
    for (;;) {
        int k;
        while ((k = pollkey()) >= 0) {
            if (k == KEY_ESC) return 0;
            if (k == 'p' || k == 'P') { paused = !paused; draw_bar(paused ? "Paused" : "Arrows to steer"); }
            if (over && (k == ' ' || k == '\n')) { over = 0; new_game(); next_dir = dir; }
            /* 0 = up, 1 = right, 2 = down, 3 = left; no instant reversing */
            if (k == KEY_UP && dir != 2) next_dir = 0;
            if (k == KEY_RIGHT && dir != 3) next_dir = 1;
            if (k == KEY_DOWN && dir != 0) next_dir = 2;
            if (k == KEY_LEFT && dir != 1) next_dir = 3;
        }
        int speed = 140 - score * 3; if (speed < 60) speed = 60;
        if (paused || over || ticks() - last < (uint32_t)speed) { sleep(10); continue; }
        last = ticks();
        dir = next_dir;
        int nx = sx[0] + (dir == 1) - (dir == 3), ny = sy[0] + (dir == 2) - (dir == 0);
        int dead = nx < 0 || ny < 0 || nx >= GW || ny >= GH;
        for (int i = 0; i < len - 1 && !dead; i++) if (sx[i] == nx && sy[i] == ny) dead = 1;
        if (dead) {
            over = 1;
            if (score > best) { best = score; save_best(); draw_bar("New best! Space = again"); }
            else draw_bar("Game over - Space = again");
            for (int i = 0; i < len; i++) cell(sx[i], sy[i], 0x7F1D1D);
            continue;
        }
        int ate = nx == food_x && ny == food_y;
        if (!ate) cell(sx[len - 1], sy[len - 1], 0x052E16);   /* tail moves on */
        else if (len < GW * GH) len++;
        for (int i = len - 1; i > 0; i--) { sx[i] = sx[i - 1]; sy[i] = sy[i - 1]; }
        sx[0] = nx; sy[0] = ny;
        cell(sx[1], sy[1], 0x22C55E);
        cell(nx, ny, 0x86EFAC);
        if (ate) { score++; place_food(); draw_bar("Arrows to steer"); }
    }
}
