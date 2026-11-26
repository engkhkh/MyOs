/* cards.c - playing-card pictures shared by Solitaire and FreeCell (see cards.h). */
#include "cards.h"
#define UP CARD_UP
static int suit(int c) { return card_suit(c); }
static int rank(int c) { return card_rank(c); }
static int red(int c) { return card_red(c); }

/* ---------------- drawing cards ---------------- */
/* Suit shapes are drawn from simple geometry, so they look smooth at any size. */
static int in_circle(int x, int y, int cx, int cy, int r) { return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r; }
static int suit_pixel(int s, int x, int y) {                 /* x, y in 0..100 */
    int ax = x > 50 ? x - 50 : 50 - x;
    switch (s) {
    case 2: return ax * 48 + (y > 50 ? y - 50 : 50 - y) * 36 <= 36 * 48;                       /* diamond */
    case 1: return in_circle(x, y, 29, 34, 22) || in_circle(x, y, 71, 34, 22) ||                 /* heart */
                   (y >= 34 && y <= 94 && ax * 60 <= (94 - y) * 47);
    case 0: return in_circle(x, y, 29, 58, 21) || in_circle(x, y, 71, 58, 21) ||                 /* spade */
                   (y >= 4 && y <= 58 && ax * 54 <= (y - 4) * 49) ||
                   (y >= 62 && y <= 98 && ax * 36 <= 5 * 36 + (y - 62) * 14);
    default: return in_circle(x, y, 50, 27, 21) || in_circle(x, y, 27, 60, 21) ||                /* club */
                    in_circle(x, y, 73, 60, 21) || in_circle(x, y, 50, 52, 12) ||
                    (y >= 62 && y <= 98 && ax * 36 <= 5 * 36 + (y - 62) * 14);
    }
}
static void draw_suit(int x, int y, int size, int s, uint32_t c) {
    for (int j = 0; j < size; j++)
        for (int i = 0; i < size; i++) {
            /* 2x2 samples per pixel give soft edges */
            int hits = 0;
            for (int sy = 0; sy < 2; sy++) for (int sx = 0; sx < 2; sx++)
                hits += suit_pixel(s, (i * 4 + sx * 2 + 1) * 100 / (size * 4), (j * 4 + sy * 2 + 1) * 100 / (size * 4));
            if (hits == 4) ui_pixel(x + i, y + j, c);
            else if (hits) {
                int px = x + i, py = y + j;
                if (px >= 0 && py >= 0 && px < ui.w && py < ui.h) ui.px[py * ui.w + px] = ui_mix(ui.px[py * ui.w + px], c, hits * 64);
            }
        }
}
static const char *rank_name[13] = { "A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K" };

static void draw_back(int x, int y) {
    ui_rfill(x, y, CW, CH, 6, 0x1E3A8A);
    ui_rfill(x + 4, y + 4, CW - 8, CH - 8, 4, 0x2563EB);
    ui_clip(x + 6, y + 6, CW - 12, CH - 12);
    for (int j = 0; j < CH - 12; j++)
        for (int i = 0; i < CW - 12; i++)
            if (((i + j) % 10) == 0 || ((i - j + 200) % 10) == 0) ui_pixel(x + 6 + i, y + 6 + j, 0x3B82F6);
    ui_noclip();
    ui_rframe(x + 4, y + 4, CW - 8, CH - 8, 4, 0x93C5FD);
}
static void draw_face(int x, int y, int c) {
    uint32_t ink = red(c) ? 0xDC2626 : 0x111827;
    int r = rank(c), s = suit(c);
    ui_rfill(x, y, CW, CH, 6, 0x9CA3AF);
    ui_rfill(x + 1, y + 1, CW - 2, CH - 2, 5, UI_WHITE);
    ui_textb(x + 5, y + 4, rank_name[r], ink);
    draw_suit(x + 5, y + 22, 11, s, ink);
    if (r >= 10) {                                           /* J Q K: a framed picture panel */
        ui_rfill(x + 18, y + 14, CW - 36 + 12, CH - 28, 4, red(c) ? 0xFECACA : 0xD1D5DB);
        ui_rfill(x + 19, y + 15, CW - 38 + 12, CH - 30, 3, red(c) ? 0xFEF2F2 : 0xF3F4F6);
        ui_text_big(x + 26, y + 18, rank_name[r], ink, 3);
        draw_suit(x + 31, y + 72, 14, s, ink);
    } else if (r == 0) draw_suit(x + 18, y + 32, 36, s, ink);   /* Ace: one big suit */
    else draw_suit(x + 24, y + 36, 26, s, ink);
    int w = ui_text_w(rank_name[r]);                           /* bottom-right corner */
    ui_textb(x + CW - 5 - w, y + CH - 21, rank_name[r], ink);
    draw_suit(x + CW - 16, y + CH - 34, 11, s, ink);
}
/* Every card is drawn once at start-up into a small picture, then just copied. */
#define KEYCOL 0xFF00FF                                     /* "transparent" (the rounded corners) */
static uint32_t *sprite[53];
void cards_init(void) {
    ui_no_theme();                                    /* cards and the green table keep their colours */
    uint32_t *keep = ui.px; int kw = ui.w, kh = ui.h;
    for (int i = 0; i < 53; i++) {
        sprite[i] = malloc(CW * CH * 4);
        ui.px = sprite[i]; ui.w = CW; ui.h = CH; ui_noclip();
        ui_fill(0, 0, CW, CH, KEYCOL);
        if (i < 52) draw_face(0, 0, i); else draw_back(0, 0);
    }
    ui.px = keep; ui.w = kw; ui.h = kh; ui_noclip();
}
void card_draw(int x, int y, int c) {
    const uint32_t *src = sprite[(c & UP) ? (c & 0x7F) : 52];
    for (int j = 0; j < CH; j++) {
        int py = y + j;
        if (py < UI_MENU_H || py >= ui.h) continue;
        uint32_t *d = ui.px + py * ui.w;
        for (int i = 0; i < CW; i++) {
            int px = x + i; uint32_t v = src[j * CW + i];
            if (px >= 0 && px < ui.w && v != KEYCOL) d[px] = v;
        }
    }
}
void card_slot(int x, int y, const char *label) {
    ui_rframe(x, y, CW, CH, 6, 0x4ADE80);
    ui_rfill(x + 1, y + 1, CW - 2, CH - 2, 5, FELT_DARK);
    if (label) ui_text_big(x + (CW - 16 * strlen(label)) / 2, y + 34, label, 0x2F8F57, 2);
}


/* The classic win animation: the card jumps off and bounces along the bottom,
 * leaving a trail. Returns 1 if the player pressed a key or clicked to skip. */
int card_bounce(int c, int x0, int y0, int floor_y) {
    int x = x0 * 16, y = y0 * 16;
    int vx = (int)(rand() % 6 + 2) * 16 * ((rand() & 1) ? 1 : -1), vy = -(int)(rand() % 8) * 16;
    int floor = floor_y * 16;
    for (;;) {
        x += vx; y += vy; vy += 10;
        if (y > floor) { y = floor; vy = -vy * 3 / 4; }
        if (x < -CW * 16 || x > ui.w * 16) return 0;
        card_draw(x / 16, y / 16, c);
        ui_present_rect(x / 16 - 8, y / 16 - 8, CW + 16, CH + 16);
        sleep(12);
        struct event e;
        if (pollevent(&e) && (e.type == EV_KEY || e.type == EV_MOUSE_DOWN)) return 1;
    }
}
