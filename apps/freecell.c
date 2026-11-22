/* freecell.c - FreeCell, built with the MyOS widget toolkit (ui.h) and cards.c.
 *
 * All cards are dealt face up into 8 columns. Use the 4 free cells (top left)
 * to park single cards, and build the 4 foundations (top right) from Ace to
 * King by suit. Columns are built down in alternating colours.
 * The deals use the same numbers as Windows FreeCell (Game #1 ... #32000).
 * Keys: F2 new game, Ctrl+Z undo. Double-click = to a foundation or free cell. */
#include "cards.h"

#define W 680
#define H 620
#define GAP 10
#define TOP (UI_MENU_H + 12)
#define TAB_Y (TOP + CH + 22)
#define STATUS_H 24

/* piles: 0-3 free cells, 4-7 foundations, 8-15 columns */
#define CELL 0
#define FOUND 4
#define COL 8
#define NPILES 16
#define UP CARD_UP

struct pile { int n; uint8_t c[24]; };
struct game { struct pile p[NPILES]; int moves, won, number; uint32_t start, end; };

static struct game g;
#define UNDO_MAX 300
static struct game *undo_stack;
static int undo_n;

static int dragging, drag_from, drag_index, drag_dx, drag_dy, drag_moved, press_x, press_y;
static int show_help, show_won, show_pick, celebrate_pending, flash_msg_until;
static char pick_text[8], flash_msg[64];

static int top(int p) { return g.p[p].n ? g.p[p].c[g.p[p].n - 1] : -1; }
static int col_x(int i) { return (W - (8 * CW + 7 * GAP)) / 2 + i * (CW + GAP); }
static int pile_x(int p) { return p < COL ? col_x(p) : col_x(p - COL); }
static int step(int p) {                                     /* squeeze long columns to fit */
    int n = g.p[p].n, room = H - STATUS_H - 8 - TAB_Y - CH, s = 32;
    if (n > 1 && s * (n - 1) > room) s = room / (n - 1);
    return s;
}
static int card_y(int p, int i) { return p < COL ? TOP : TAB_Y + i * step(p); }

static void flash(const char *m) {
    int i = 0; for (; m[i] && i < 63; i++) flash_msg[i] = m[i];
    flash_msg[i] = 0; flash_msg_until = ticks() + 2500;
}

/* ---------------- dealing: the Microsoft FreeCell numbering ---------------- */
static uint32_t ms_seed;
static int ms_rand(void) { ms_seed = ms_seed * 214013u + 2531011u; return (ms_seed >> 16) & 0x7FFF; }
static void deal(int number) {
    memset(&g, 0, sizeof g);
    g.number = number;
    /* the MS deck order: card index = rank * 4 + suit (clubs, diamonds, hearts, spades) */
    static const int suit_map[4] = { 3, 2, 1, 0 };          /* -> our suits (spades 0, hearts 1, diamonds 2, clubs 3) */
    int deck[52];
    for (int i = 0; i < 52; i++) deck[i] = suit_map[i % 4] * 13 + i / 4;
    ms_seed = (uint32_t)number;
    for (int i = 0, left = 52; left > 0; i++, left--) {
        int j = ms_rand() % left;
        struct pile *q = &g.p[COL + i % 8];
        q->c[q->n++] = (uint8_t)(deck[j] | UP);
        deck[j] = deck[left - 1];
    }
    g.start = ticks();
    undo_n = 0; dragging = 0; show_won = 0;
}
static void new_game(void) { srand(ticks() * 2654435761u + 7); deal(rand() % 32000 + 1); }

/* ---------------- rules ---------------- */
static int free_cells(void) { int n = 0; for (int i = CELL; i < FOUND; i++) if (!g.p[i].n) n++; return n; }
static int empty_cols(void) { int n = 0; for (int i = COL; i < NPILES; i++) if (!g.p[i].n) n++; return n; }
static int max_move(int to_empty) {                          /* the "supermove" limit */
    int e = empty_cols() - (to_empty ? 1 : 0), m = free_cells() + 1;
    while (e-- > 0) m *= 2;
    return m;
}
static int run_ok(int p, int i) {                            /* cards i..top form a proper sequence */
    struct pile *q = &g.p[p];
    for (int k = i + 1; k < q->n; k++)
        if (card_red(q->c[k]) == card_red(q->c[k - 1]) || card_rank(q->c[k]) != card_rank(q->c[k - 1]) - 1) return 0;
    return 1;
}
static int can_found(int c, int f) {
    int t = top(f);
    if (t < 0) return card_rank(c) == 0;
    return card_suit(t) == card_suit(c) && card_rank(c) == card_rank(t) + 1;
}
static int can_col(int c, int p) {
    int t = top(p);
    return t < 0 || (card_red(t) != card_red(c) && card_rank(c) == card_rank(t) - 1);
}
static void push_undo(void) {
    if (undo_n == UNDO_MAX) { for (int i = 1; i < UNDO_MAX; i++) undo_stack[i - 1] = undo_stack[i]; undo_n--; }
    undo_stack[undo_n++] = g;
}
static void undo(void) { if (undo_n) { g = undo_stack[--undo_n]; dragging = 0; } }
static int found_total(void) { int n = 0; for (int f = FOUND; f < COL; f++) n += g.p[f].n; return n; }

static int legal(int from, int i, int to) {
    struct pile *a = &g.p[from];
    int c = a->c[i], count = a->n - i;
    if (from == to || count < 1) return 0;
    if (to < FOUND) return count == 1 && !g.p[to].n;
    if (to < COL) return count == 1 && can_found(c, to);
    if (!run_ok(from, i) || !can_col(c, to)) return 0;
    if (count > max_move(!g.p[to].n)) return 0;
    return 1;
}
static void do_move(int from, int i, int to) {
    struct pile *a = &g.p[from], *b = &g.p[to];
    for (int k = i; k < a->n; k++) b->c[b->n++] = a->c[k];
    a->n = i;
}

/* Play cards to the foundations when it can't hurt (like Windows does). */
static int safe_to_found(int c) {
    int r = card_rank(c), have[4] = { -1, -1, -1, -1 };      /* highest rank on each suit's foundation */
    if (r <= 1) return 1;                                    /* Aces and twos always */
    for (int f = FOUND; f < COL; f++) { int t = top(f); if (t >= 0) have[card_suit(t)] = card_rank(t); }
    /* safe when both suits of the other colour already have the card one lower */
    return card_red(c) ? (have[0] >= r - 1 && have[3] >= r - 1) : (have[1] >= r - 1 && have[2] >= r - 1);
}
static void draw_all(void);
static void auto_play(void) {
    for (;;) {
        int moved = 0;
        for (int p = 0; p < NPILES && !moved; p++) {
            if (p >= FOUND && p < COL) continue;
            int c = top(p);
            if (c < 0 || !safe_to_found(c)) continue;
            for (int f = FOUND; f < COL; f++)
                if (can_found(c, f)) { do_move(p, g.p[p].n - 1, f); moved = 1; break; }
        }
        if (!moved) break;
        draw_all(); ui_present(); sleep(60);
    }
    if (found_total() == 52 && !g.won) { g.won = 1; g.end = ticks(); show_won = 1; celebrate_pending = 1; }
}
static int try_move(int from, int i, int to) {
    if (!legal(from, i, to)) return 0;
    push_undo();
    do_move(from, i, to);
    g.moves++;
    dragging = 0;
    auto_play();
    return 1;
}
static void quick_move(int p) {                              /* double-click: foundation, else a free cell */
    int n = g.p[p].n;
    if (!n) return;
    for (int f = FOUND; f < COL; f++) if (try_move(p, n - 1, f)) return;
    if (p >= COL) for (int c = CELL; c < FOUND; c++) if (try_move(p, n - 1, c)) return;
    flash("That card can't go anywhere by itself");
}

/* ---------------- mouse ---------------- */
static int hit_card(int *pile, int *index) {
    for (int p = 0; p < NPILES; p++) {
        struct pile *q = &g.p[p];
        for (int i = q->n - 1; i >= 0; i--) {
            if (p < COL && i != q->n - 1) break;
            int x = pile_x(p), y = card_y(p, i);
            if (ui.mx >= x && ui.mx < x + CW && ui.my >= y && ui.my < y + CH) { *pile = p; *index = i; return 1; }
        }
    }
    return 0;
}
static int overlap(int ax, int ay, int bx, int by) {
    int w = ax < bx ? ax + CW - bx : bx + CW - ax, h = ay < by ? ay + CH - by : by + CH - ay;
    return w > 0 && h > 0 ? w * h : 0;
}
static void drop(void) {
    int dx = ui.mx - drag_dx, dy = ui.my - drag_dy, best = -1, best_area = 0, tried = 0;
    for (int p = 0; p < NPILES; p++) {
        if (p == drag_from) continue;
        int n = g.p[p].n, a = overlap(dx, dy, pile_x(p), p < COL ? TOP : card_y(p, n ? n - 1 : 0));
        if (a > 0) tried = 1;
        if (a > best_area && legal(drag_from, drag_index, p)) { best = p; best_area = a; }
    }
    if (best >= 0) try_move(drag_from, drag_index, best);
    else if (tried && drag_from >= COL && g.p[drag_from].n - drag_index > 1) {
        /* tell the player why a long run didn't move */
        char m[64] = "You can move only ", num[8];
        int k = strlen(m); utoa(max_move(0), num);
        for (int q = 0; num[q]; q++) m[k++] = num[q];
        const char *t = " cards at once here"; while (*t) m[k++] = *t++;
        m[k] = 0;
        if (g.p[drag_from].n - drag_index > max_move(0) && run_ok(drag_from, drag_index)) flash(m);
    }
}

/* ---------------- drawing ---------------- */
static void draw_all(void) {
    ui_vgradient(0, UI_MENU_H, W, H - UI_MENU_H - STATUS_H, 0x1B8048, 0x115E33);
    for (int p = 0; p < COL; p++) {
        int n = g.p[p].n - (dragging && drag_from == p ? 1 : 0);
        if (n > 0) card_draw(pile_x(p), TOP, g.p[p].c[n - 1]);
        else card_slot(pile_x(p), TOP, p >= FOUND ? "A" : 0);
    }
    ui_fill(col_x(4) - GAP / 2 - 1, TOP + 6, 2, CH - 12, 0x2F8F57);    /* between cells and foundations */
    for (int p = COL; p < NPILES; p++) {
        struct pile *q = &g.p[p];
        int n = dragging && drag_from == p ? drag_index : q->n;
        if (!n) card_slot(pile_x(p), TAB_Y, 0);
        for (int i = 0; i < n; i++) card_draw(pile_x(p), card_y(p, i), q->c[i]);
    }
    if (dragging) {
        struct pile *q = &g.p[drag_from];
        int x = ui.mx - drag_dx, y = ui.my - drag_dy, s = drag_from >= COL ? step(drag_from) : 32;
        ui_rfill(x + 3, y + 4, CW, CH + (q->n - drag_index - 1) * s, 6, 0x0B3D20);
        for (int i = drag_index; i < q->n; i++) card_draw(x, y + (i - drag_index) * s, q->c[i]);
    }
    ui_fill(0, H - STATUS_H, W, STATUS_H, 0x0B3D20);
    char st[100], num[12]; int k = 0;
    uint32_t secs = ((g.won ? g.end : ticks()) - g.start) / 1000;
    const char *parts[] = { "Game #", 0, "     Moves: ", 0, "     Time: ", 0 };
    char tm[12]; utoa(secs / 60, tm); int tl = strlen(tm); tm[tl++] = ':'; tm[tl++] = '0' + secs % 60 / 10; tm[tl++] = '0' + secs % 10; tm[tl] = 0;
    for (int i = 0; i < 6; i++) {
        const char *s = parts[i];
        if (i == 1) s = utoa(g.number, num);
        if (i == 3) s = utoa(g.moves, num);
        if (i == 5) s = tm;
        while (*s && k < 98) st[k++] = *s++;
    }
    st[k] = 0;
    ui_text(12, H - STATUS_H + 4, st, 0xD1FAE5);
    if ((int)(flash_msg_until - ticks()) > 0) ui_text(W - 10 - ui_text_w(flash_msg), H - STATUS_H + 4, flash_msg, 0xFDE68A);
    else {
        char fc[40] = "Free cells: "; int q = strlen(fc); fc[q++] = '0' + free_cells(); fc[q] = 0;
        ui_text(W - 10 - ui_text_w(fc), H - STATUS_H + 4, fc, 0x86EFAC);
    }
}

/* "Select game" box: a dialog with a text box */
static int pick_dialog(void) {
    int w = 340, h = 150, x = (W - w) / 2, y = (H - h) / 2, r = -1;
    for (int i = 0; i < ui.w * ui.h; i++) ui.px[i] = (ui.px[i] >> 1) & 0x7F7F7F;
    ui_rfill(x + 3, y + 4, w, h, 10, 0x202020);
    ui_rfill(x, y, w, h, 10, UI_WHITE);
    ui_textb(x + 24, y + 18, "Select a game", UI_TEXT);
    ui_text(x + 24, y + 44, "Game number (1 to 32000):", 0x374151);
    ui.focus = 77;
    if (ui_textbox(77, x + 24, y + 66, 120, pick_text, 6)) r = 0;
    if (ui_button_primary(78, x + w - 226, y + h - 44, 100, 30, "Play")) r = 0;
    if (ui_button(79, x + w - 116, y + h - 44, 100, 30, "Cancel") || ui.key == KEY_ESC) r = 1;
    if (r >= 0) ui_redraw();
    return r;
}

int main(void) {
    undo_stack = malloc(sizeof(struct game) * UNDO_MAX);
    ui_init("FreeCell", W, H);
    cards_init();
    new_game();
    char arg[16];
    if (getarg(arg, sizeof arg) > 0) {
        if (arg[0] == 'd') {                                  /* "freecell demo": almost solved */
            memset(&g, 0, sizeof g); g.number = 0;
            for (int f = 0; f < 4; f++) for (int r = 0; r < 11; r++) g.p[FOUND + f].c[g.p[FOUND + f].n++] = (f * 13 + r) | UP;
            static const uint8_t c[8] = { 12, 13 + 11, 26 + 12, 39 + 11, 13 + 12, 11, 39 + 12, 26 + 11 };  /* K Q K Q ... */
            for (int i = 0; i < 8; i++) g.p[COL + i].c[g.p[COL + i].n++] = c[i] | UP;   /* kings and queens */
            g.start = ticks();
        } else {                                              /* "freecell 617": a numbered game */
            int n = 0; for (int i = 0; arg[i] >= '0' && arg[i] <= '9'; i++) n = n * 10 + arg[i] - '0';
            if (n >= 1 && n <= 32000) deal(n);
        }
    }
    static const char *menus[] = {
        "Game|New game\tF2|Select game...\tF3|Restart this game|Undo\tCtrl+Z|-|Exit",
        "Help|How to play" };
    for (;;) {
        int m = ui_menubar(menus, 2);
        int dialog = show_help || show_won || show_pick;
        switch (m) {
        case 0: new_game(); break;
        case 1: show_pick = 1; pick_text[0] = 0; break;
        case 2: { int n = g.number; if (n) deal(n); break; }
        case 3: undo(); break;
        case 4: exit(0); break;
        case 100: show_help = 1; break;
        }
        if (!dialog) {
            if (ui.key == KEY_F2) new_game();
            if (ui.key == KEY_F3) { show_pick = 1; pick_text[0] = 0; }
            if (ui.key == KEY_CTRL('z')) undo();
            int p, i;
            if (ui.pressed && hit_card(&p, &i)) {
                if (ui.dbl && i == g.p[p].n - 1 && (p < FOUND || p >= COL)) quick_move(p);
                else if (p < FOUND || p >= COL) {
                    dragging = 1; drag_from = p; drag_index = i; drag_moved = 0;
                    drag_dx = ui.mx - pile_x(p); drag_dy = ui.my - card_y(p, i);
                    press_x = ui.mx; press_y = ui.my;
                }
            }
            if (dragging && ui.moved) {
                int dx = ui.mx - press_x, dy = ui.my - press_y;
                if (dx * dx + dy * dy > 9) drag_moved = 1;
            }
            if (dragging && ui.released) { if (drag_moved) drop(); dragging = 0; }
        }
        if (celebrate_pending) {
            celebrate_pending = 0;
            draw_all(); ui_menubar_draw(); ui_present();
            for (int r = 12; r >= 0; r--)
                for (int f = FOUND; f < COL; f++)
                    if (card_bounce(g.p[f].c[r] | UP, pile_x(f), TOP, H - STATUS_H - CH)) { r = -1; break; }
        }
        dialog = show_help || show_won || show_pick;
        if (dialog) ui_mute();
        draw_all();
        ui_menubar_draw();
        if (dialog) {
            ui_unmute();
            if (show_help) {
                if (ui_dialog("How to play FreeCell", "Build Ace to King by suit (top right). Columns go down",
                              "in alternating colours; park cards in the 4 free cells.", "OK", 0) >= 0) show_help = 0;
            } else if (show_pick) {
                int r = pick_dialog();
                if (r == 0) {
                    int n = 0; for (int i = 0; pick_text[i]; i++) if (pick_text[i] >= '0' && pick_text[i] <= '9') n = n * 10 + pick_text[i] - '0';
                    if (n >= 1 && n <= 32000) { deal(n); show_pick = 0; } else flash("Pick a number from 1 to 32000");
                }
                if (r == 1) show_pick = 0;
                if (!show_pick) ui.focus = 0;
            } else if (show_won) {
                char line[64] = "You solved game #", num[12]; int k = strlen(line);
                utoa(g.number, num); for (int q = 0; num[q]; q++) line[k++] = num[q];
                const char *t = " in "; while (*t) line[k++] = *t++;
                utoa(g.moves, num); for (int q = 0; num[q]; q++) line[k++] = num[q];
                t = " moves!"; while (*t) line[k++] = *t++;
                line[k] = 0;
                int r = ui_dialog("Congratulations!", line, "Play another game?", "New game", "Close");
                if (r == 0) new_game();
                if (r >= 0) show_won = 0;
            }
        }
        ui_present();
        ui_wait(1000);
    }
}
