/* solitaire.c - Klondike Solitaire, built with the MyOS widget toolkit (ui.h).
 *
 * Drag cards with the mouse. Click the deck to turn cards over. Double-click a
 * card to send it to the foundations (top right). Build the four foundations
 * up by suit from Ace to King to win.
 * Keys: F2 new game, Ctrl+Z undo, Space = turn a card. */
#include "cards.h"

#define W 620
#define H 620
#define GAP 14
#define TOP (UI_MENU_H + 12)
#define TAB_Y (TOP + CH + 18)
#define STATUS_H 24

/* piles: 0 stock, 1 waste, 2-5 foundations, 6-12 tableau */
#define STOCK 0
#define WASTE 1
#define FOUND 2
#define TAB 6
#define NPILES 13
#define UP CARD_UP

struct pile { int n; uint8_t c[52]; };
struct game { struct pile p[NPILES]; int score, moves, draw3, won; uint32_t start; };

static struct game g;
#define UNDO_MAX 200
static struct game *undo_stack;
static int undo_n;

static int dragging, drag_from, drag_index, drag_dx, drag_dy, drag_moved, press_x, press_y;
static int show_help, show_won, show_newgame;
static uint32_t won_time;
static int celebrate_pending;

static int suit(int c) { return (c & 0x7F) / 13; }
static int rank(int c) { return (c & 0x7F) % 13; }          /* 0 = Ace ... 12 = King */
static int red(int c) { int s = suit(c); return s == 1 || s == 2; }
static int top(int p) { return g.p[p].n ? g.p[p].c[g.p[p].n - 1] : -1; }

/* ---------------- layout ---------------- */
static int col_x(int i) { return 16 + i * (CW + GAP); }
static int pile_x(int p) {
    if (p == STOCK) return col_x(0);
    if (p == WASTE) return col_x(1);
    if (p < TAB) return col_x(p - FOUND + 3);
    return col_x(p - TAB);
}
static int down_step(void) { return 7; }
static int up_step(int p) {                                  /* squeeze tall columns to fit */
    struct pile *q = &g.p[p];
    int downs = 0, ups = 0;
    for (int i = 0; i < q->n; i++) { if (q->c[i] & UP) ups++; else downs++; }
    int room = H - STATUS_H - 8 - TAB_Y - CH - downs * down_step();
    int step = 24;
    if (ups > 1 && step * (ups - 1) > room) step = room / (ups - 1);
    return step < 10 ? 10 : step;
}
static int card_y(int p, int i) {                            /* y of card i in pile p */
    if (p < TAB) return TOP;
    int y = TAB_Y, us = up_step(p);
    for (int k = 0; k < i; k++) y += (g.p[p].c[k] & UP) ? us : down_step();
    return y;
}
static int waste_shown(void) { int n = g.p[WASTE].n; return g.draw3 ? (n < 3 ? n : 3) : (n ? 1 : 0); }
static int card_x(int p, int i) {
    if (p == WASTE && g.draw3) { int first = g.p[WASTE].n - waste_shown(); return pile_x(p) + (i > first ? (i - first) * 16 : 0); }
    return pile_x(p);
}

/* ---------------- rules ---------------- */
static int can_found(int c, int f) {
    int t = top(f);
    if (t < 0) return rank(c) == 0;
    return suit(t) == suit(c) && rank(c) == rank(t) + 1;
}
static int can_tab(int c, int p) {
    int t = top(p);
    if (t < 0) return rank(c) == 12;
    return (t & UP) && red(t) != red(c) && rank(c) == rank(t) - 1;
}
static void push_undo(void) {
    if (undo_n == UNDO_MAX) { for (int i = 1; i < UNDO_MAX; i++) undo_stack[i - 1] = undo_stack[i]; undo_n--; }
    undo_stack[undo_n++] = g;
}
static void undo(void) { if (undo_n) { g = undo_stack[--undo_n]; dragging = 0; } }

static void flip_tops(void) {                                /* turn over uncovered cards */
    for (int p = TAB; p < NPILES; p++) {
        struct pile *q = &g.p[p];
        if (q->n && !(q->c[q->n - 1] & UP)) { q->c[q->n - 1] |= UP; g.score += 5; }
    }
}
static int found_total(void) { int n = 0; for (int f = FOUND; f < TAB; f++) n += g.p[f].n; return n; }
static void check_won(void) {
    if (found_total() == 52 && !g.won) { g.won = 1; show_won = 1; won_time = ticks(); celebrate_pending = 1; }
}
/* move cards from pile `from` (index i to the top) onto pile `to`, if the rules allow */
static int try_move(int from, int i, int to) {
    struct pile *a = &g.p[from], *b = &g.p[to];
    int c = a->c[i], count = a->n - i;
    if (from == to || !(c & UP)) return 0;
    if (to >= FOUND && to < TAB) { if (count != 1 || !can_found(c, to)) return 0; }
    else if (to >= TAB) { if (!can_tab(c, to)) return 0; }
    else return 0;
    push_undo();
    for (int k = 0; k < count; k++) b->c[b->n++] = a->c[i + k];
    a->n = i;
    if (to < TAB) g.score += 10;                              /* to a foundation */
    else if (from == WASTE) g.score += 5;
    else if (from >= FOUND && from < TAB) g.score -= 15;
    if (g.score < 0) g.score = 0;
    g.moves++;
    flip_tops();
    check_won();
    return 1;
}
static void deal_from_stock(void) {
    push_undo();
    struct pile *s = &g.p[STOCK], *w = &g.p[WASTE];
    if (!s->n) {                                             /* turn the waste back over */
        if (!w->n) { undo_n--; return; }
        while (w->n) s->c[s->n++] = w->c[--w->n] & 0x7F;
        if (!g.draw3) { g.score -= 100; if (g.score < 0) g.score = 0; }
    } else {
        int k = g.draw3 ? 3 : 1;
        while (k-- && s->n) w->c[w->n++] = s->c[--s->n] | UP;
    }
    g.moves++;
}
static int auto_found(int p) {                               /* send the top card of p to a foundation */
    if (!g.p[p].n) return 0;
    int i = g.p[p].n - 1;
    for (int f = FOUND; f < TAB; f++) if (try_move(p, i, f)) return 1;
    return 0;
}
static void new_game(void) {
    int draw3 = g.draw3;
    memset(&g, 0, sizeof g);
    g.draw3 = draw3;
    uint8_t deck[52];
    for (int i = 0; i < 52; i++) deck[i] = (uint8_t)i;
    srand(ticks() * 2654435761u + 1);
    for (int i = 51; i > 0; i--) { int j = rand() % (i + 1); uint8_t t = deck[i]; deck[i] = deck[j]; deck[j] = t; }
    int k = 0;
    for (int col = 0; col < 7; col++)
        for (int r = 0; r <= col; r++) g.p[TAB + col].c[g.p[TAB + col].n++] = deck[k++] | (r == col ? UP : 0);
    while (k < 52) g.p[STOCK].c[g.p[STOCK].n++] = deck[k++];
    g.start = ticks();
    undo_n = 0; dragging = 0; show_won = 0;
}

/* ---------------- hit testing ---------------- */
static int hit_card(int *pile, int *index) {                 /* which face-up card is under the mouse? */
    for (int p = 1; p < NPILES; p++) {
        struct pile *q = &g.p[p];
        for (int i = q->n - 1; i >= 0; i--) {
            if (p < TAB && i != q->n - 1) break;                 /* only the top of waste / foundations */
            int x = card_x(p, i), y = card_y(p, i);
            if (ui.mx >= x && ui.mx < x + CW && ui.my >= y && ui.my < y + CH) {
                if (!(q->c[i] & UP)) return 0;
                *pile = p; *index = i; return 1;
            }
        }
    }
    return 0;
}
static int overlap(int ax, int ay, int bx, int by) {
    int w = (ax < bx ? ax + CW - bx : bx + CW - ax), h = (ay < by ? ay + CH - by : by + CH - ay);
    return w > 0 && h > 0 ? w * h : 0;
}
static void drop(void) {                                     /* release a dragged pile on the best target */
    int dx = ui.mx - drag_dx, dy = ui.my - drag_dy, best = -1, best_area = 0;
    int c = g.p[drag_from].c[drag_index];
    for (int p = FOUND; p < NPILES; p++) {
        if (p == drag_from) continue;
        int n = g.p[p].n, tx = pile_x(p), ty = p < TAB ? TOP : card_y(p, n ? n - 1 : 0);
        int ok = p < TAB ? (g.p[drag_from].n - drag_index == 1 && can_found(c, p)) : can_tab(c, p);
        int a = overlap(dx, dy, tx, ty);
        if (ok && a > best_area) { best = p; best_area = a; }
    }
    if (best >= 0) try_move(drag_from, drag_index, best);
}

/* ---------------- the whole window ---------------- */
static void draw_board(void) {
    ui_vgradient(0, UI_MENU_H, W, H - UI_MENU_H - STATUS_H, 0x1B8048, 0x115E33);
    /* stock */
    if (g.p[STOCK].n) {
        int n = g.p[STOCK].n;
        for (int i = 0; i < (n > 2 ? 2 : n - 1); i++) card_draw(pile_x(STOCK) + 4 - 2 * i, TOP + 4 - 2 * i, 0);  /* a little depth */
        card_draw(pile_x(STOCK), TOP, 0);
    } else card_slot(pile_x(STOCK), TOP, g.p[WASTE].n ? "O" : 0);
    /* waste */
    struct pile *w = &g.p[WASTE];
    int shown = waste_shown();
    if (!w->n) card_slot(pile_x(WASTE), TOP, 0);
    for (int i = w->n - shown; i < w->n; i++) {
        if (dragging && drag_from == WASTE && i >= drag_index) continue;
        card_draw(card_x(WASTE, i), TOP, w->c[i]);
    }
    /* foundations */
    for (int f = FOUND; f < TAB; f++) {
        struct pile *q = &g.p[f];
        int n = q->n - (dragging && drag_from == f ? 1 : 0);
        if (n > 0) card_draw(pile_x(f), TOP, q->c[n - 1]);
        else card_slot(pile_x(f), TOP, "A");
    }
    /* tableau */
    for (int p = TAB; p < NPILES; p++) {
        struct pile *q = &g.p[p];
        int n = dragging && drag_from == p ? drag_index : q->n;
        if (!q->n || !n) { if (!(dragging && drag_from == p && drag_index > 0)) card_slot(pile_x(p), TAB_Y, 0); }
        for (int i = 0; i < n; i++) card_draw(pile_x(p), card_y(p, i), q->c[i]);
    }
    /* the cards being dragged follow the mouse */
    if (dragging) {
        struct pile *q = &g.p[drag_from];
        int x = ui.mx - drag_dx, y = ui.my - drag_dy, us = drag_from >= TAB ? up_step(drag_from) : 24;
        for (int i = drag_index; i < q->n; i++) {
            if (i == drag_index) ui_rfill(x + 3, y + 4, CW, CH, 6, 0x0B3D20);        /* shadow */
            card_draw(x, y + (i - drag_index) * us, q->c[i]);
        }
    }
    /* status bar */
    ui_fill(0, H - STATUS_H, W, STATUS_H, 0x0B3D20);
    char st[120], num[12]; int k = 0;
    const char *parts[] = { "Score: ", 0, "     Moves: ", 0, "     Time: ", 0 };
    uint32_t secs = ((g.won ? won_time : ticks()) - g.start) / 1000;
    char tm[12]; utoa(secs / 60, tm); int tl = strlen(tm); tm[tl++] = ':'; tm[tl++] = '0' + secs % 60 / 10; tm[tl++] = '0' + secs % 10; tm[tl] = 0;
    for (int i = 0; i < 6; i++) {
        const char *s = parts[i];
        if (i == 1) s = utoa(g.score, num);
        if (i == 3) s = utoa(g.moves, num);
        if (i == 5) s = tm;
        while (*s && k < 118) st[k++] = *s++;
    }
    st[k] = 0;
    ui_text(12, H - STATUS_H + 4, st, 0xD1FAE5);
    ui_text(W - 8 - ui_text_w(g.draw3 ? "Draw 3" : "Draw 1"), H - STATUS_H + 4, g.draw3 ? "Draw 3" : "Draw 1", 0x86EFAC);
}

/* the famous ending: cards bounce off the foundations, leaving trails */
static void celebrate(void) {
    ui_present();
    for (int round = 12; round >= 0; round--)
        for (int f = FOUND; f < TAB; f++)
            if (card_bounce(g.p[f].c[round] | UP, pile_x(f), TOP, H - STATUS_H - CH)) return;
}
static void auto_finish(void) {                              /* play every card it can to the foundations */
    for (;;) {
        int moved = 0;
        for (int p = WASTE; p < NPILES && !moved; p++) if (p < FOUND || p >= TAB) moved = auto_found(p);
        if (!moved) break;
        draw_board(); ui_present(); sleep(40);
    }
}
static int can_auto_finish(void) {
    if (g.p[STOCK].n || g.p[WASTE].n) return 0;
    for (int p = TAB; p < NPILES; p++) for (int i = 0; i < g.p[p].n; i++) if (!(g.p[p].c[i] & UP)) return 0;
    return found_total() < 52;
}

int main(void) {
    undo_stack = malloc(sizeof(struct game) * UNDO_MAX);
    ui_init("Solitaire", W, H);
    cards_init();
    new_game();
    char arg[16];
    if (getarg(arg, sizeof arg) > 0 && arg[0] == 'd') {     /* "solitaire demo": an almost finished game */
        memset(&g, 0, sizeof g);
        for (int f = 0; f < 4; f++) for (int r = 0; r < 10; r++) g.p[FOUND + f].c[g.p[FOUND + f].n++] = (f * 13 + r) | UP;
        /* suits: 0 spades, 1 hearts, 2 diamonds, 3 clubs; ranks: 10 = J, 11 = Q, 12 = K */
        static const uint8_t cols[4][3] = { { 12, 13 + 11, 10 }, { 13 + 12, 11, 13 + 10 }, { 39 + 12, 26 + 11, 39 + 10 }, { 26 + 12, 39 + 11, 0 } };
        for (int c = 0; c < 4; c++) for (int k = 0; k < (c == 3 ? 2 : 3); k++) g.p[TAB + c].c[g.p[TAB + c].n++] = cols[c][k] | UP;
        g.p[WASTE].c[g.p[WASTE].n++] = (26 + 10) | UP;              /* jack of diamonds */
        g.start = ticks();
    }
    static const char *menus[2];
    for (;;) {
        menus[0] = g.draw3 ? "Game|New game\tF2|Undo\tCtrl+Z|Finish (play all to Aces)|-|Draw 1 card|*Draw 3 cards|-|Exit"
                           : "Game|New game\tF2|Undo\tCtrl+Z|Finish (play all to Aces)|-|*Draw 1 card|Draw 3 cards|-|Exit";
        menus[1] = "Help|How to play";
        int m = ui_menubar(menus, 2);
        int dialog = show_help || show_won || show_newgame;
        switch (m) {
        case 0: new_game(); break;
        case 1: undo(); break;
        case 2: auto_finish(); break;
        case 3: case 4: if (g.draw3 != (m == 4)) { g.draw3 = m == 4; show_newgame = 1; } break;
        case 5: exit(0); break;
        case 100: show_help = 1; break;
        }
        if (!dialog) {
            if (ui.key == KEY_F2) new_game();
            if (ui.key == KEY_CTRL('z')) undo();
            if (ui.key == ' ') deal_from_stock();
            /* mouse */
            int p, i;
            if (ui.pressed) {
                if (ui_hit(pile_x(STOCK), TOP, CW, CH)) deal_from_stock();
                else if (hit_card(&p, &i)) {
                    if (ui.dbl && i == g.p[p].n - 1 && (p == WASTE || p >= TAB)) { auto_found(p); }
                    else {
                        dragging = 1; drag_from = p; drag_index = i; drag_moved = 0;
                        drag_dx = ui.mx - card_x(p, i); drag_dy = ui.my - card_y(p, i);
                        press_x = ui.mx; press_y = ui.my;
                    }
                }
            }
            if (dragging && ui.moved) {
                int ddx = ui.mx - press_x, ddy = ui.my - press_y;
                if (ddx * ddx + ddy * ddy > 9) drag_moved = 1;
            }
            if (dragging && ui.released) { dragging = 0; if (drag_moved) drop(); }
            if (!dragging && can_auto_finish() && ui.released) auto_finish();
        }
        if (celebrate_pending) {                             /* just won: the bouncing cards first */
            celebrate_pending = 0; dragging = 0;
            draw_board(); ui_menubar_draw(); celebrate();
        }
        dialog = show_help || show_won || show_newgame;
        if (dialog) ui_mute();
        draw_board();
        ui_menubar_draw();
        if (dialog) {
            ui_unmute();
            if (show_help) {
                if (ui_dialog("How to play", "Build Ace to King by suit on the top right. Columns go",
                              "down in alternating colours. Drag cards; double-click = to Ace.", "OK", 0) >= 0) show_help = 0;
            } else if (show_newgame) {
                int r = ui_dialog("Start a new game?", g.draw3 ? "Draw 3 cards at a time from now on."
                                                             : "Draw 1 card at a time from now on.", 0, "New game", "Later");
                if (r == 0) new_game();
                if (r >= 0) show_newgame = 0;
            } else if (show_won) {
                char line[64], num[12]; int k = 0;
                const char *a = "You won with a score of "; while (*a) line[k++] = *a++;
                utoa(g.score, num); for (int q = 0; num[q]; q++) line[k++] = num[q];
                line[k++] = '!'; line[k] = 0;
                int r = ui_dialog("Congratulations!", line, "Play another game?", "New game", "Close");
                if (r == 0) new_game();
                if (r >= 0) show_won = 0;
            }
        }
        ui_present();
        ui_wait(1000);                                       /* wake every second for the clock */
    }
}
