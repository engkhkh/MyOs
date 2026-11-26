/* spider.c - Spider Solitaire, built with the MyOS widget toolkit (ui.h) and cards.c.
 *
 * Two decks (104 cards) in 10 columns; only the bottom card of each column starts
 * face up. Build down in any suit, but only a run of ONE suit moves together.
 * A full run King..Ace of one suit leaves the table. Clear all 8 runs to win.
 * Click the stock (bottom right) to deal one more card onto every column -
 * only when no column is empty.
 * Difficulty: 1 suit (spades), 2 suits (spades, hearts) or 4 suits.
 * Keys: F2 new game, Ctrl+Z undo, H hint. Double-click = best move for that card.
 * "spider demo" starts a game that is one move from the end (for testing). */
#include "cards.h"

#define W 820
#define H 640
#define GAP 8
#define TOP (UI_MENU_H + 12)
#define STATUS_H 24
#define BOTTOM_Y (H - STATUS_H - CH - 10)          /* the stock and the finished runs */
#define NCOL 10
#define MAXC 104
#define UP CARD_UP

struct column { int n; uint8_t c[MAXC]; };
struct game {
    struct column col[NCOL];
    uint8_t stock[50]; int nstock;
    uint8_t done[8]; int ndone;                    /* the suit of each finished run */
    int suits, moves, score, number, won;
    uint32_t start, end;
};

static struct game g;
#define UNDO_MAX 200
static struct game *undo_stack;
static int undo_n;
static int suits_choice = 1;

static int dragging, drag_from, drag_index, drag_dx, drag_dy, drag_moved, press_x, press_y;
static int show_help, show_won, celebrate_pending, flash_until, hint_until, hint_from, hint_index, hint_to;
static char flash_msg[64];

static void flash(const char *m) {
    int i = 0; for (; m[i] && i < 63; i++) flash_msg[i] = m[i];
    flash_msg[i] = 0; flash_until = ticks() + 2500;
}

/* ---------------- layout ---------------- */
static int col_x(int i) { return (W - (NCOL * CW + (NCOL - 1) * GAP)) / 2 + i * (CW + GAP); }
static void steps(int p, int *down, int *up) {     /* gaps for face-down and face-up cards, squeezed to fit */
    struct column *q = &g.col[p];
    int nd = 0, nu = 0;
    for (int i = 0; i < q->n - 1; i++) { if (q->c[i] & UP) nu++; else nd++; }
    int room = BOTTOM_Y - 14 - TOP - CH;
    *down = 10; *up = 26;
    while (nd * *down + nu * *up > room && *up > 8) { (*up)--; if (*down > 4 && *up < 18) (*down)--; }
}
static int card_y(int p, int i) {
    int d, u, y = TOP; steps(p, &d, &u);
    for (int k = 0; k < i; k++) y += (g.col[p].c[k] & UP) ? u : d;
    return y;
}
static int stock_x(void) { return col_x(NCOL - 1) - (g.nstock / 10 - 1) * 12; }

/* ---------------- dealing ---------------- */
static uint32_t seed;
static int rnd(void) { seed = seed * 1103515245u + 12345u; return (seed >> 16) & 0x7FFF; }
static void deal(int number, int suits) {
    memset(&g, 0, sizeof g);
    g.number = number; g.suits = suits; g.score = 500;
    uint8_t deck[104];
    for (int i = 0; i < 104; i++) {
        int s = suits == 1 ? 0 : suits == 2 ? (i / 13) % 2 : (i / 13) % 4;   /* spades / + hearts / all four */
        deck[i] = (uint8_t)(s * 13 + i % 13);
    }
    seed = (uint32_t)number * 2654435761u + (uint32_t)suits;
    for (int i = 103; i > 0; i--) { int j = rnd() % (i + 1); uint8_t t = deck[i]; deck[i] = deck[j]; deck[j] = t; }
    int k = 0;
    for (int i = 0; i < 54; i++) { struct column *q = &g.col[i % NCOL]; q->c[q->n++] = deck[k++]; }
    for (int p = 0; p < NCOL; p++) g.col[p].c[g.col[p].n - 1] |= UP;
    while (k < 104) g.stock[g.nstock++] = deck[k++];
    g.start = ticks();
    undo_n = 0; dragging = 0; show_won = 0; hint_until = 0;
}
static void new_game(void) { srand(ticks() * 2654435761u + 11); deal(rand() % 32000 + 1, suits_choice); }

/* ---------------- rules ---------------- */
static int run_ok(int p, int i) {                  /* i..top: face up, one suit, each one lower */
    struct column *q = &g.col[p];
    if (i < 0 || i >= q->n || !(q->c[i] & UP)) return 0;
    for (int k = i + 1; k < q->n; k++)
        if (card_suit(q->c[k]) != card_suit(q->c[k - 1]) || card_rank(q->c[k]) != card_rank(q->c[k - 1]) - 1) return 0;
    return 1;
}
static int legal(int from, int i, int to) {
    if (from == to || !run_ok(from, i)) return 0;
    struct column *b = &g.col[to];
    if (!b->n) return 1;
    int t = b->c[b->n - 1];
    return (t & UP) && card_rank(t) == card_rank(g.col[from].c[i]) + 1;
}
static void push_undo(void) {
    if (undo_n == UNDO_MAX) { for (int i = 1; i < UNDO_MAX; i++) undo_stack[i - 1] = undo_stack[i]; undo_n--; }
    undo_stack[undo_n++] = g;
}
static void undo(void) {
    if (!undo_n) { flash("Nothing to undo"); return; }
    g = undo_stack[--undo_n]; g.score--; dragging = 0;
}
static void flip_top(int p) { struct column *q = &g.col[p]; if (q->n) q->c[q->n - 1] |= UP; }
static void draw_all(void);
static void check_runs(void) {                     /* a full King..Ace run of one suit leaves */
    for (int p = 0; p < NCOL; p++) {
        struct column *q = &g.col[p];
        if (q->n < 13) continue;
        int i = q->n - 13;
        if (card_rank(q->c[i]) == 12 && card_rank(q->c[q->n - 1]) == 0 && run_ok(p, i)) {
            g.done[g.ndone++] = (uint8_t)card_suit(q->c[i]);
            q->n = i; g.score += 100;
            flip_top(p);
            draw_all(); ui_present(); sleep(120);
        }
    }
    if (g.ndone == 8 && !g.won) {
        g.won = 1; g.end = ticks(); show_won = 1; celebrate_pending = 1;
        char m[48] = "You won with a score of ", num[12]; int k = strlen(m);
        utoa(g.score, num); for (int q = 0; num[q]; q++) m[k++] = num[q];
        m[k] = 0;
        notify("Spider Solitaire", m);                        /* shows up in the notification centre too */
    }
}
static int try_move(int from, int i, int to) {
    if (!legal(from, i, to)) return 0;
    push_undo();
    struct column *a = &g.col[from], *b = &g.col[to];
    for (int k = i; k < a->n; k++) b->c[b->n++] = a->c[k];
    a->n = i;
    flip_top(from);
    g.moves++; g.score--;
    dragging = 0; hint_until = 0;
    check_runs();
    return 1;
}
static void deal_row(void) {
    if (!g.nstock) { flash("The stock is empty"); return; }
    for (int p = 0; p < NCOL; p++) if (!g.col[p].n) { flash("Fill every empty column before dealing"); return; }
    push_undo();
    for (int p = 0; p < NCOL; p++) { struct column *q = &g.col[p]; q->c[q->n++] = g.stock[--g.nstock] | UP; }
    g.moves++; g.score--; hint_until = 0;
    check_runs();
}
static int run_start(int p) {                      /* where the movable run at the bottom of column p starts */
    int i = g.col[p].n - 1;
    while (i > 0 && run_ok(p, i - 1)) i--;
    return i;
}
/* best destination for the run i.. of column p: same suit first, then any card, then an empty column */
static int best_to(int p, int i) {
    int c = g.col[p].c[i], any = -1, empty = -1;
    for (int t = 0; t < NCOL; t++) {
        if (!legal(p, i, t)) continue;
        struct column *b = &g.col[t];
        if (!b->n) { if (empty < 0) empty = t; continue; }
        if (card_suit(b->c[b->n - 1]) == card_suit(c)) return t;
        if (any < 0) any = t;
    }
    return any >= 0 ? any : (i > 0 ? empty : -1);  /* a whole column to an empty one is pointless */
}
static void hint(void) {
    /* longest useful run first: one that joins its own suit, or uncovers a card */
    int bp = -1, bi = 0, bt = -1, bscore = -1;
    for (int p = 0; p < NCOL; p++) {
        if (!g.col[p].n) continue;
        for (int i = run_start(p); i < g.col[p].n; i++) {
            int t = best_to(p, i);
            if (t < 0 || !g.col[t].n) continue;
            int same = card_suit(g.col[t].c[g.col[t].n - 1]) == card_suit(g.col[p].c[i]);
            int uncovers = i == 0 || !(g.col[p].c[i - 1] & UP) || card_rank(g.col[p].c[i - 1]) != card_rank(g.col[p].c[i]) + 1;
            int score = same * 4 + uncovers * 2 + (i == run_start(p));
            if (!uncovers && !same) continue;      /* moving from one equal spot to another */
            if (score > bscore) { bscore = score; bp = p; bi = i; bt = t; }
        }
    }
    if (bp < 0) { flash(g.nstock ? "No moves - deal from the stock (bottom right)" : "No more moves - try Undo"); return; }
    hint_from = bp; hint_index = bi; hint_to = bt; hint_until = ticks() + 1500;
}

/* ---------------- mouse ---------------- */
static int hit_card(int *pile, int *index) {
    for (int p = 0; p < NCOL; p++) {
        struct column *q = &g.col[p];
        for (int i = q->n - 1; i >= 0; i--) {
            int x = col_x(p), y = card_y(p, i);
            if (ui.mx >= x && ui.mx < x + CW && ui.my >= y && ui.my < y + CH) { *pile = p; *index = i; return 1; }
        }
    }
    return 0;
}
static int on_stock(void) {
    if (!g.nstock) return 0;
    int x = stock_x();
    return ui.mx >= x && ui.mx < col_x(NCOL - 1) + CW && ui.my >= BOTTOM_Y && ui.my < BOTTOM_Y + CH;
}
static int overlap(int ax, int ay, int bx, int by) {
    int w = ax < bx ? ax + CW - bx : bx + CW - ax, h = ay < by ? ay + CH - by : by + CH - ay;
    return w > 0 && h > 0 ? w * h : 0;
}
static void drop(void) {
    int dx = ui.mx - drag_dx, dy = ui.my - drag_dy, best = -1, best_area = 0, tried = 0;
    for (int p = 0; p < NCOL; p++) {
        if (p == drag_from) continue;
        int n = g.col[p].n, a = overlap(dx, dy, col_x(p), n ? card_y(p, n - 1) : TOP);
        if (a > 0) tried = 1;
        if (a > best_area && legal(drag_from, drag_index, p)) { best = p; best_area = a; }
    }
    if (best >= 0) try_move(drag_from, drag_index, best);
    else if (tried) flash("A card goes on one rank higher, or an empty column");
}

/* ---------------- drawing ---------------- */
static void draw_all(void) {
    ui_vgradient(0, UI_MENU_H, W, H - UI_MENU_H - STATUS_H, 0x1B8048, 0x115E33);
    int hint_on = (int)(hint_until - ticks()) > 0;
    for (int p = 0; p < NCOL; p++) {
        struct column *q = &g.col[p];
        int n = dragging && drag_from == p ? drag_index : q->n;
        if (!n) card_slot(col_x(p), TOP, 0);
        for (int i = 0; i < n; i++) card_draw(col_x(p), card_y(p, i), q->c[i]);
        if (hint_on && p == hint_to) {             /* hint: outline where it goes */
            int y = q->n ? card_y(p, q->n - 1) : TOP;
            for (int k = 0; k < 3; k++) {
                ui_fill(col_x(p) - 3 + k, y - 3 + k, CW + 6 - 2 * k, 1, 0xFDE68A);
                ui_fill(col_x(p) - 3 + k, y + CH + 2 - k, CW + 6 - 2 * k, 1, 0xFDE68A);
                ui_fill(col_x(p) - 3 + k, y - 3 + k, 1, CH + 6 - 2 * k, 0xFDE68A);
                ui_fill(col_x(p) + CW + 2 - k, y - 3 + k, 1, CH + 6 - 2 * k, 0xFDE68A);
            }
        }
    }
    if (hint_on) {                                 /* hint: lift the cards that should move */
        struct column *q = &g.col[hint_from];
        for (int i = hint_index; i < q->n; i++) card_draw(col_x(hint_from) + 4, card_y(hint_from, i) - 6, q->c[i]);
    }
    /* finished runs (bottom left) and the stock (bottom right) */
    for (int i = 0; i < 8; i++) {
        int x = col_x(0) + i * 16;
        if (i < g.ndone) card_draw(x, BOTTOM_Y, (g.done[i] * 13 + 12) | UP);
        else if (i == 0) card_slot(x, BOTTOM_Y, 0);
    }
    for (int k = 0; k < g.nstock / 10; k++) card_draw(stock_x() + k * 12, BOTTOM_Y, 0);
    if (!g.nstock) card_slot(col_x(NCOL - 1), BOTTOM_Y, 0);
    if (dragging) {
        struct column *q = &g.col[drag_from];
        int x = ui.mx - drag_dx, y = ui.my - drag_dy, d, u; steps(drag_from, &d, &u);
        ui_rfill(x + 3, y + 4, CW, CH + (q->n - drag_index - 1) * u, 6, 0x0B3D20);
        for (int i = drag_index; i < q->n; i++) card_draw(x, y + (i - drag_index) * u, q->c[i]);
    }
    ui_fill(0, H - STATUS_H, W, STATUS_H, 0x0B3D20);
    char st[120], num[12]; int k = 0;
    const char *parts[] = { "Score: ", 0, "     Moves: ", 0, "     Runs: ", 0, "/8     ", 0, 0 };
    for (int i = 0; i < 8; i++) {
        const char *s = parts[i];
        if (i == 1) s = g.score < 0 ? "0" : utoa(g.score, num);
        if (i == 3) s = utoa(g.moves, num);
        if (i == 5) s = utoa(g.ndone, num);
        if (i == 7) s = g.suits == 1 ? "1 suit" : g.suits == 2 ? "2 suits" : "4 suits";
        while (s && *s && k < 118) st[k++] = *s++;
    }
    st[k] = 0;
    ui_text(12, H - STATUS_H + 4, st, 0xD1FAE5);
    const char *right = (int)(flash_until - ticks()) > 0 ? flash_msg : 0;
    char deals[40];
    if (!right) {
        int d = g.nstock / 10, q = 0; const char *t = "Deals left: ";
        while (*t) deals[q++] = *t++;
        deals[q++] = '0' + d; deals[q] = 0; right = deals;
    }
    ui_text(W - 10 - ui_text_w(right), H - STATUS_H + 4, right, right == flash_msg ? 0xFDE68A : 0x86EFAC);
}

static void start_demo(void) {                      /* seven runs done; one move finishes the game */
    memset(&g, 0, sizeof g);
    g.suits = 1; g.score = 1200; g.ndone = 7; g.number = 0;
    for (int r = 12; r >= 1; r--) g.col[0].c[g.col[0].n++] = r | UP;     /* King .. 2 of spades */
    g.col[1].c[g.col[1].n++] = 0 | UP;                                   /* the Ace */
    g.start = ticks();
}

int main(void) {
    undo_stack = malloc(sizeof(struct game) * UNDO_MAX);
    ui_init("Spider Solitaire", W, H);
    cards_init();
    char arg[16];
    int a = getarg(arg, sizeof arg);
    if (a > 0 && arg[0] >= '1' && arg[0] <= '4') suits_choice = arg[0] == '1' ? 1 : arg[0] == '2' ? 2 : 4;
    new_game();
    if (a > 0 && arg[0] == 'd') start_demo();
    static const char *menus[] = {
        "Game|New game\tF2|Restart this game|-|Easy: 1 suit|Medium: 2 suits|Hard: 4 suits|-|Undo\tCtrl+Z|Hint\tH|Deal a row\tD|-|Exit",
        "Help|How to play" };
    for (;;) {
        int m = ui_menubar(menus, 2);
        int dialog = show_help || show_won;
        switch (m) {
        case 0: new_game(); break;
        case 1: if (g.number) deal(g.number, g.suits); break;
        case 2: suits_choice = 1; new_game(); break;
        case 3: suits_choice = 2; new_game(); break;
        case 4: suits_choice = 4; new_game(); break;
        case 5: undo(); break;
        case 6: hint(); break;
        case 7: deal_row(); break;
        case 8: exit(0); break;
        case 100: show_help = 1; break;
        }
        if (!dialog) {
            if (ui.key == KEY_F2) new_game();
            if (ui.key == KEY_CTRL('z')) undo();
            if (ui.key == 'h' || ui.key == 'H') hint();
            if (ui.key == 'd' || ui.key == 'D') deal_row();
            int p, i;
            if (ui.pressed && on_stock()) deal_row();
            else if (ui.pressed && hit_card(&p, &i)) {
                if (ui.dbl) {                      /* double-click: the best place for this run */
                    int s = run_ok(p, i) ? i : run_start(p), t = best_to(p, s);
                    if (t < 0 || !try_move(p, s, t)) flash("Those cards can't move anywhere");
                } else if (run_ok(p, i)) {
                    dragging = 1; drag_from = p; drag_index = i; drag_moved = 0;
                    drag_dx = ui.mx - col_x(p); drag_dy = ui.my - card_y(p, i);
                    press_x = ui.mx; press_y = ui.my;
                } else if (g.col[p].c[i] & UP) flash("Only a run of one suit moves together");
            }
            if (dragging && ui.moved) {
                int dx = ui.mx - press_x, dy = ui.my - press_y;
                if (dx * dx + dy * dy > 9) drag_moved = 1;
            }
            if (dragging && ui.released) { if (drag_moved) drop(); dragging = 0; }
        }
        if ((int)(hint_until - ticks()) > 0 || (int)(flash_until - ticks()) > 0) ui_redraw();
        if (celebrate_pending) {
            celebrate_pending = 0;
            draw_all(); ui_menubar_draw(); ui_present();
            for (int r = 0; r < 8; r++)
                if (card_bounce((g.done[r] * 13 + 12) | UP, col_x(0) + r * 16, BOTTOM_Y, H - STATUS_H - CH)) break;
        }
        dialog = show_help || show_won;
        if (dialog) ui_mute();
        draw_all();
        ui_menubar_draw();
        if (dialog) {
            ui_unmute();
            if (show_help) {
                if (ui_dialog("How to play Spider", "Build down from King to Ace. A run of one suit moves",
                              "together; a full run leaves. Click the stock to deal.", "OK", 0) >= 0) show_help = 0;
            } else if (show_won) {
                char line[64] = "You won with a score of ", num[12]; int k = strlen(line);
                utoa(g.score, num); for (int q = 0; num[q]; q++) line[k++] = num[q];
                line[k++] = '!'; line[k] = 0;
                int r = ui_dialog("Congratulations!", line, "Play another game?", "New game", "Close");
                if (r == 0) new_game();
                if (r >= 0) show_won = 0;
            }
        }
        ui_present();
        ui_wait((int)(hint_until - ticks()) > 0 || (int)(flash_until - ticks()) > 0 ? 200 : 1000);
    }
}
