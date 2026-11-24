/* calendar.c - Calendar: a month view with events, built with the MyOS toolkit (ui.h).
 *
 * Events are kept in C:\Calendar\Events.txt, one per line, so Notepad can read them:
 *     2026-11-24 14:30 Dentist
 *     2026-12-25 --:-- Holiday              (--:-- = all day)
 * Click a day (or move with the arrow keys) to see its events on the right; type a time
 * (optional) and a title and press Enter to add one. PgUp/PgDn change the month,
 * T (or Home) jumps to today. Also opens from the clock on the taskbar. */
#include "ui.h"

#define DIR "/Calendar"
#define FILE_PATH "/Calendar/Events.txt"
#define MAXE 500
#define TITLE_MAX 48
#define GX 12
#define GY (UI_MENU_H + 74)
#define CELL_W 82
#define CELL_H 70
#define PANEL_X (GX + 7 * CELL_W + 16)
#define STATUS_H 24

struct cal_event { int y, m, d, mins; char title[TITLE_MAX]; };    /* mins = -1: all day */
static struct cal_event *ev;
static int ne;
static int W = 880, H = 560;
static int ty, tm, td;                                         /* today */
static int vy, vm;                                             /* the month on screen */
static int sy, sm, sd;                                         /* the selected day */
static char in_time[8], in_title[TITLE_MAX], error[64], status[64];
static uint32_t status_until;

static const char *month_names[] = { "January", "February", "March", "April", "May", "June", "July",
                                     "August", "September", "October", "November", "December" };
static const char *day_names[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };

static void copy(char *d, const char *s, int max) { int i = 0; for (; s[i] && i < max - 1; i++) d[i] = s[i]; d[i] = 0; }
static void say(const char *m) { copy(status, m, 64); status_until = ticks() + 2500; ui_redraw(); }
static int leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static int days_in(int y, int m) { static const int d[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }; return d[m - 1] + (m == 2 && leap(y)); }
static int weekday(int y, int m, int d) {                      /* 0 = Sunday (Sakamoto's method) */
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}
static void add_days(int *y, int *m, int *d, int n) {
    *d += n;
    while (*d < 1) { if (--*m < 1) { *m = 12; --*y; } *d += days_in(*y, *m); }
    while (*d > days_in(*y, *m)) { *d -= days_in(*y, *m); if (++*m > 12) { *m = 1; ++*y; } }
}
static int cmp_event(const struct cal_event *a, const struct cal_event *b) {
    if (a->y != b->y) return a->y - b->y;
    if (a->m != b->m) return a->m - b->m;
    if (a->d != b->d) return a->d - b->d;
    return a->mins - b->mins;
}
static void sort_events(void) {
    for (int i = 1; i < ne; i++)
        for (int j = i; j > 0 && cmp_event(&ev[j], &ev[j - 1]) < 0; j--) { struct cal_event t = ev[j]; ev[j] = ev[j - 1]; ev[j - 1] = t; }
}
static int num(const char *s, int n) { int v = 0; for (int i = 0; i < n; i++) { if (s[i] < '0' || s[i] > '9') return -1; v = v * 10 + s[i] - '0'; } return v; }

/* ---------------- the file ---------------- */
static void load(void) {
    static char buf[MAXE * 72];
    int n = readfile(FILE_PATH, buf, sizeof buf - 1);
    if (n < 0) n = 0;
    buf[n] = 0;
    for (char *p = buf; *p && ne < MAXE; ) {
        char *e = p; while (*e && *e != '\n') e++;
        char line[100]; int k = 0;
        for (char *q = p; q < e && k < 99; q++) if (*q != '\r') line[k++] = *q;
        line[k] = 0;
        /* YYYY-MM-DD HH:MM Title */
        if (k >= 18 && line[4] == '-' && line[7] == '-' && line[10] == ' ' && line[16] == ' ') {
            struct cal_event *v = &ev[ne];
            v->y = num(line, 4); v->m = num(line + 5, 2); v->d = num(line + 8, 2);
            int hh = num(line + 11, 2), mm = num(line + 14, 2);
            v->mins = hh >= 0 && mm >= 0 ? hh * 60 + mm : -1;
            copy(v->title, line + 17, TITLE_MAX);
            if (v->y > 0 && v->m >= 1 && v->m <= 12 && v->d >= 1 && v->d <= 31 && v->title[0]) ne++;
        }
        p = *e ? e + 1 : e;
    }
    sort_events();
}
static void two(char *o, int v) { o[0] = '0' + v / 10 % 10; o[1] = '0' + v % 10; }
static int save(void) {
    static char buf[MAXE * 72];
    int k = 0;
    for (int i = 0; i < ne; i++) {
        struct cal_event *v = &ev[i];
        buf[k++] = '0' + v->y / 1000 % 10; buf[k++] = '0' + v->y / 100 % 10; two(buf + k, v->y); k += 2;
        buf[k++] = '-'; two(buf + k, v->m); k += 2; buf[k++] = '-'; two(buf + k, v->d); k += 2; buf[k++] = ' ';
        if (v->mins < 0) { const char *a = "--:--"; while (*a) buf[k++] = *a++; }
        else { two(buf + k, v->mins / 60); k += 2; buf[k++] = ':'; two(buf + k, v->mins % 60); k += 2; }
        buf[k++] = ' ';
        for (const char *t = v->title; *t; t++) buf[k++] = *t;
        buf[k++] = '\n';
    }
    return writefile(FILE_PATH, buf, k) >= 0;
}

/* ---------------- editing ---------------- */
static void select_day(int y, int m, int d) { sy = y; sm = m; sd = d; vy = y; vm = m; error[0] = 0; ui_redraw(); }
static void go_month(int delta) {
    vm += delta;
    while (vm < 1) { vm += 12; vy--; }
    while (vm > 12) { vm -= 12; vy++; }
    sy = vy; sm = vm; if (sd > days_in(vy, vm)) sd = days_in(vy, vm);
    ui_redraw();
}
static int parse_time(const char *s) {                         /* "" = all day; "9", "930", "9:30", "14:05" */
    int h = 0, m = 0, i = 0, digits = 0;
    while (s[i] == ' ') i++;
    if (!s[i]) return -1;
    char d[5]; int nd = 0;
    for (; s[i]; i++) {
        if (s[i] >= '0' && s[i] <= '9') { if (nd < 4) d[nd++] = s[i]; digits++; }
        else if (s[i] != ':' && s[i] != '.' && s[i] != ' ') return -2;
    }
    if (!digits || digits > 4) return -2;
    if (nd <= 2) { h = num(d, nd); m = 0; }
    else { h = num(d, nd - 2); m = num(d + nd - 2, 2); }
    if (h > 23 || m > 59) return -2;
    return h * 60 + m;
}
static void add_event(void) {
    char t[TITLE_MAX]; copy(t, in_title, TITLE_MAX);
    int a = 0; while (t[a] == ' ') a++;
    if (!t[a]) { copy(error, "Type what the event is", 64); ui.focus = 2; return; }
    int mins = parse_time(in_time);
    if (mins == -2) { copy(error, "Time: like 9:30 or 14:00 (empty = all day)", 64); ui.focus = 1; return; }
    if (ne >= MAXE) { copy(error, "The calendar is full (500 events)", 64); return; }
    struct cal_event *v = &ev[ne++];
    v->y = sy; v->m = sm; v->d = sd; v->mins = mins; copy(v->title, t + a, TITLE_MAX);
    sort_events();
    if (!save()) { say("Could not save C:\\Calendar\\Events.txt"); return; }
    in_title[0] = in_time[0] = 0; error[0] = 0;
    ui.focus = 0;                                              /* the next key starts a new event */
    say("Event added");
}
static void delete_event(int i) {
    for (int k = i; k < ne - 1; k++) ev[k] = ev[k + 1];
    ne--;
    save();
    say("Event deleted");
}

/* ---------------- drawing ---------------- */
static uint32_t event_color(const char *t) {
    static const uint32_t c[] = { 0x2563EB, 0x059669, 0xD97706, 0xDB2777, 0x7C3AED, 0x0891B2 };
    unsigned h = 5; for (; *t; t++) h = h * 33 + (unsigned char)*t;
    return c[h % 6];
}
static void time_text(int mins, char *o) {
    if (mins < 0) { copy(o, "All day", 8); return; }
    two(o, mins / 60); o[2] = ':'; two(o + 3, mins % 60); o[5] = 0;
}
static void draw_month(void) {
    ui_fill(0, UI_MENU_H, PANEL_X - 8, H - UI_MENU_H - STATUS_H, UI_WHITE);
    char title[24]; int k = 0;
    for (const char *a = month_names[vm - 1]; *a; a++) title[k++] = *a;
    title[k++] = ' '; utoa(vy, title + k);
    ui_text_big(GX + 4, UI_MENU_H + 12, title, UI_TEXT, 2);
    int bx = GX + 7 * CELL_W - 214;
    if (ui_button(10, bx, UI_MENU_H + 14, 36, 28, "<")) go_month(-1);
    if (ui_button(11, bx + 42, UI_MENU_H + 14, 36, 28, ">")) go_month(1);
    if (ui_button(12, bx + 84, UI_MENU_H + 14, 130, 28, "Today")) select_day(ty, tm, td);
    for (int c = 0; c < 7; c++) {
        char dn[4]; copy(dn, day_names[c], 4);
        ui_text(GX + c * CELL_W + 8, GY - 19, dn, c == 5 || c == 6 ? 0x9CA3AF : UI_MUTED);
    }
    int first = weekday(vy, vm, 1);
    int y = vy, m = vm, d = 1; add_days(&y, &m, &d, -first);  /* the Sunday on or before the 1st */
    int ei = 0;
    for (int r = 0; r < 6; r++)
        for (int c = 0; c < 7; c++) {
            int x = GX + c * CELL_W, cy = GY + r * CELL_H;
            int in_month = m == vm, is_today = y == ty && m == tm && d == td, is_sel = y == sy && m == sm && d == sd;
            int hot = ui_hit(x, cy, CELL_W, CELL_H);
            ui_fill(x, cy, CELL_W, CELL_H, is_sel ? UI_ACCENT_LIGHT : hot ? 0xF3F4F6 : in_month ? UI_WHITE : 0xF9FAFB);
            ui_fill(x, cy, CELL_W, 1, UI_BORDER); ui_fill(x, cy, 1, CELL_H, UI_BORDER);
            char dn[4]; utoa(d, dn);
            if (is_today) { ui_rfill(x + 4, cy + 3, 26, 22, 11, UI_ACCENT); ui_text(x + 17 - (int)strlen(dn) * 4, cy + 6, dn, UI_WHITE); }
            else ui_text(x + 8, cy + 6, dn, in_month ? UI_TEXT : 0xB0B5BD);
            /* this day's events: up to two labels, then "+N more" */
            while (ei < ne && (ev[ei].y < y || (ev[ei].y == y && (ev[ei].m < m || (ev[ei].m == m && ev[ei].d < d))))) ei++;
            int shown = 0, more = 0;
            for (int j = ei; j < ne && ev[j].y == y && ev[j].m == m && ev[j].d == d; j++) {
                if (shown < 2) {
                    char lab[11]; copy(lab, ev[j].title, 10);
                    ui_rfill(x + 4, cy + 28 + shown * 18, CELL_W - 8, 16, 4, in_month ? event_color(ev[j].title) : 0xCBD5E1);
                    ui_text(x + 8, cy + 28 + shown * 18, lab, UI_WHITE);
                    shown++;
                } else more++;
            }
            if (more) { char mm[12] = "+"; utoa(more, mm + 1); int l = strlen(mm); copy(mm + l, " more", 6); ui_text(x + CELL_W - 8 - (int)strlen(mm) * 8, cy + 6, mm, UI_MUTED); }
            if (ui.pressed && hot) { sy = y; sm = m; sd = d; error[0] = 0; if (!in_month) { vy = y; vm = m; } ui.focus = 2; ui_redraw(); }
            add_days(&y, &m, &d, 1);
        }
    ui_fill(GX, GY + 6 * CELL_H, 7 * CELL_W + 1, 1, UI_BORDER);
    ui_fill(GX + 7 * CELL_W, GY, 1, 6 * CELL_H, UI_BORDER);
}
static void draw_panel(void) {
    int x = PANEL_X, w = W - PANEL_X - 12;
    ui_fill(x - 8, UI_MENU_H, W - x + 8, H - UI_MENU_H - STATUS_H, 0xF9FAFB);
    ui_fill(x - 8, UI_MENU_H, 1, H - UI_MENU_H - STATUS_H, UI_BORDER);
    char line[40]; int k = 0;
    for (const char *a = day_names[weekday(sy, sm, sd)]; *a; a++) line[k++] = *a;
    line[k] = 0;
    ui_textb(x, UI_MENU_H + 14, line, UI_ACCENT);
    k = 0; utoa(sd, line); k = strlen(line); line[k++] = ' ';
    for (const char *a = month_names[sm - 1]; *a; a++) line[k++] = *a;
    line[k++] = ' '; utoa(sy, line + k);
    ui_text_big(x, UI_MENU_H + 32, line, UI_TEXT, 1);
    int y = UI_MENU_H + 66, any = 0, rows_end = H - STATUS_H - 150;
    for (int i = 0; i < ne; i++) {
        if (ev[i].y != sy || ev[i].m != sm || ev[i].d != sd) continue;
        if (y > rows_end) { ui_text(x, y, "...", UI_MUTED); break; }
        any = 1;
        char tt[8]; time_text(ev[i].mins, tt);
        ui_rfill(x, y, 4, 34, 2, event_color(ev[i].title));
        ui_text(x + 10, y, tt, UI_MUTED);
        char tl[24]; copy(tl, ev[i].title, (w - 44) / 8 < 23 ? (w - 44) / 8 : 23);
        ui_textb(x + 10, y + 17, tl, UI_TEXT);
        if (ui_button(100 + i, x + w - 26, y + 6, 24, 22, "x")) { delete_event(i); return; }
        y += 44;
    }
    if (!any) ui_text(x, y, "No events", UI_MUTED);
    /* add an event */
    int fy = H - STATUS_H - 138;
    ui_fill(x, fy - 10, w, 1, UI_BORDER);
    ui_textb(x, fy, "Add an event", UI_TEXT);
    ui_text(x, fy + 26, "Time", UI_MUTED);
    if (ui_textbox(1, x + 48, fy + 22, 80, in_time, sizeof in_time)) {   /* Enter: on to the title, or add */
        if (in_title[0]) add_event(); else ui.focus = 2;
    }
    if (!in_time[0] && ui.focus != 1) ui_text(x + 56, fy + 27, "all day", 0x9CA3AF);
    ui_text(x, fy + 60, "What", UI_MUTED);
    if (ui_textbox(2, x + 48, fy + 56, w - 48, in_title, TITLE_MAX)) add_event();   /* Enter adds */
    if (ui_button_primary(3, x + 48, fy + 90, 110, 28, "Add")) add_event();
    if (error[0]) { char e[40]; copy(e, error, (w + 8) / 8 < 39 ? (w + 8) / 8 : 39); ui_text(x, fy - 30, e, 0xDC2626); }
}
static void draw_status(void) {
    ui_fill(0, H - STATUS_H, W, STATUS_H, 0xF3F4F6);
    ui_fill(0, H - STATUS_H, W, 1, UI_BORDER);
    char s[64]; int k; utoa(ne, s); k = strlen(s);
    copy(s + k, ne == 1 ? " event in C:\\Calendar\\Events.txt" : " events in C:\\Calendar\\Events.txt", 60 - k);
    ui_text(10, H - STATUS_H + 4, s, UI_MUTED);
    if ((int)(status_until - ticks()) > 0) ui_text(W - 12 - ui_text_w(status), H - STATUS_H + 4, status, 0x059669);
}

int main(void) {
    ev = malloc(sizeof(struct cal_event) * MAXE);
    ui_init("Calendar", W, H);
    makedir(DIR);
    load();
    int t[6]; gettime(t);
    td = t[3]; tm = t[4]; ty = t[5];
    select_day(ty, tm, td);
    static const char *menus[] = { "View|Today\tT|Previous month\tPgUp|Next month\tPgDn|-|Exit", "Help|How to use Calendar" };
    int help = 0, last_day = td;
    for (;;) {
        gettime(t);                                            /* past midnight: today moves on */
        if (t[3] != last_day) { last_day = t[3]; td = t[3]; tm = t[4]; ty = t[5]; ui_redraw(); }
        int m = ui_menubar(menus, 2);
        if (m == 0) select_day(ty, tm, td);
        if (m == 1) go_month(-1);
        if (m == 2) go_month(1);
        if (m == 3) exit(0);
        if (m == 100) help = 1;
        if (!help && ui.focus != 1 && ui.focus != 2) {         /* keys move around when no box has the caret */
            int k = ui.key;
            if (k == KEY_LEFT) { add_days(&sy, &sm, &sd, -1); vy = sy; vm = sm; }
            if (k == KEY_RIGHT) { add_days(&sy, &sm, &sd, 1); vy = sy; vm = sm; }
            if (k == KEY_UP) { add_days(&sy, &sm, &sd, -7); vy = sy; vm = sm; }
            if (k == KEY_DOWN) { add_days(&sy, &sm, &sd, 7); vy = sy; vm = sm; }
            if (k == KEY_PGUP) go_month(-1);
            if (k == KEY_PGDN) go_month(1);
            if (k == 't' || k == 'T' || k == KEY_HOME) select_day(ty, tm, td);
            if (k == '\n') ui.focus = 2;
            if (k >= '0' && k <= '9') ui.focus = 1;              /* just start typing: digits are a time, */
            else if (k > ' ' && k < 127 && k != 't' && k != 'T') ui.focus = 2;   /* anything else the title */
        }
        if (ui.key == KEY_ESC) { ui.focus = 0; ui.key = 0; }
        if (ui.key == '\t' && (ui.focus == 1 || ui.focus == 2)) { ui.focus = ui.focus == 1 ? 2 : 1; ui.key = 0; }
        if (help) ui_mute();
        draw_month();
        draw_panel();
        draw_status();
        ui_menubar_draw();
        if (help) {
            ui_unmute();
            if (ui_dialog("How to use Calendar", "Click a day, type a time (or leave it for all day)",
                          "and what it is, then press Enter. T = today.", "OK", 0) >= 0) help = 0;
        }
        ui_present();
        ui_wait(1000);
    }
}
