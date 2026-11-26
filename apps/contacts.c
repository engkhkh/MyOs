/* contacts.c - Contacts: an address book, built with the MyOS toolkit (ui.h).
 *
 * The contacts live in one plain text file, C:\Contacts\Contacts.txt, one block per person:
 *     Name: Sara Ahmed
 *     Phone: +966 50 123 4567
 *     Email: sara@example.com
 *     ...                          (a blank line between people)
 * so the file can also be read or fixed in Notepad. The list is sorted A to Z and the
 * search box looks in every field. Tab moves between fields, Enter or Ctrl+S saves,
 * Ctrl+N adds a person. */
#include "ui.h"

#define DIR "/Contacts"
#define FILE_PATH "/Contacts/Contacts.txt"
#define MAXC 200
#define NF 6
#define FLEN 96
#define LIST_W 270
#define TOP (UI_MENU_H + 8)
#define STATUS_H 24
#define ROW_H 46

static const char *field_names[NF] = { "Name", "Phone", "Email", "Address", "Birthday", "Notes" };
struct contact { char f[NF][FLEN]; };
static struct contact *people;
static int np, cur = -1, list_scroll, ask_delete, editing_new;
static struct contact form;                       /* what the fields on the right show */
static char search[64], status[80], error[80];
static uint32_t status_until;
static int W = 760, H = 520;

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static void copy(char *d, const char *s, int max) { int i = 0; for (; s[i] && i < max - 1; i++) d[i] = s[i]; d[i] = 0; }
static void say(const char *m) { copy(status, m, 80); status_until = ticks() + 2500; ui_redraw(); }
static int contains(const char *hay, const char *needle) {
    if (!needle[0]) return 1;
    for (; *hay; hay++) { int i = 0; while (needle[i] && lower(hay[i]) == lower(needle[i])) i++; if (!needle[i]) return 1; }
    return 0;
}
static int before(const char *a, const char *b) {        /* A to Z, ignoring case */
    while (*a && lower(*a) == lower(*b)) { a++; b++; }
    return lower(*a) < lower(*b);
}
static int same(const struct contact *a, const struct contact *b) {
    for (int i = 0; i < NF; i++) { const char *x = a->f[i], *y = b->f[i]; while (*x && *x == *y) { x++; y++; } if (*x != *y) return 0; }
    return 1;
}
static void trim(char *s) {
    int a = 0, n = strlen(s);
    while (s[a] == ' ') a++;
    while (n > a && s[n - 1] == ' ') n--;
    int k = 0; for (int i = a; i < n; i++) s[k++] = s[i];
    s[k] = 0;
}

/* ---------------- the file ---------------- */
static void sort_people(void) {
    for (int i = 1; i < np; i++)
        for (int j = i; j > 0 && before(people[j].f[0], people[j - 1].f[0]); j--) {
            struct contact t = people[j]; people[j] = people[j - 1]; people[j - 1] = t;
            if (cur == j) cur = j - 1; else if (cur == j - 1) cur = j;
        }
}
static void load(void) {
    static char buf[MAXC * NF * (FLEN + 12)];
    int n = readfile(FILE_PATH, buf, sizeof buf - 1);
    if (n < 0) n = 0;
    buf[n] = 0;
    struct contact c; int have = 0;
    for (int i = 0; i < NF; i++) c.f[i][0] = 0;
    for (char *p = buf; ; ) {
        char *e = p; while (*e && *e != '\n') e++;
        char line[200]; int k = 0;
        for (char *q = p; q < e && k < 199; q++) if (*q != '\r') line[k++] = *q;
        line[k] = 0;
        trim(line);
        if (!line[0] || !*e) {                             /* a blank line (or the end) closes a person */
            if (!line[0] && have && np < MAXC) { people[np++] = c; have = 0; for (int i = 0; i < NF; i++) c.f[i][0] = 0; }
        }
        if (line[0]) {
            char *colon = line; while (*colon && *colon != ':') colon++;
            if (*colon) {
                *colon = 0;
                for (int i = 0; i < NF; i++) {
                    const char *a = line, *b = field_names[i];
                    while (*a && lower(*a) == lower(*b)) { a++; b++; }
                    if (!*a && !*b) { char v[FLEN]; copy(v, colon + 1, FLEN); trim(v); copy(c.f[i], v, FLEN); have = 1; }
                }
            }
        }
        if (!*e) { if (have && np < MAXC) people[np++] = c; break; }
        p = e + 1;
    }
    sort_people();
}
static int save_file(void) {
    static char buf[MAXC * NF * (FLEN + 12)];
    int k = 0;
    for (int p = 0; p < np; p++) {
        for (int i = 0; i < NF; i++) {
            if (!people[p].f[i][0] && i) continue;
            const char *a = field_names[i]; while (*a) buf[k++] = *a++;
            buf[k++] = ':'; buf[k++] = ' ';
            const char *v = people[p].f[i]; while (*v) buf[k++] = *v++;
            buf[k++] = '\n';
        }
        buf[k++] = '\n';
    }
    return writefile(FILE_PATH, buf, k) >= 0;
}

/* ---------------- editing ---------------- */
static void show(int i) {
    cur = i; editing_new = 0; error[0] = 0;
    ui_redraw();
    if (i >= 0) form = people[i]; else for (int f = 0; f < NF; f++) form.f[f][0] = 0;
}
static int changed(void) { return editing_new ? form.f[0][0] != 0 : cur >= 0 && !same(&form, &people[cur]); }
static int check_form(void) {
    for (int i = 0; i < NF; i++) trim(form.f[i]);
    error[0] = 0;
    if (!form.f[0][0]) { copy(error, "Please type a name", 80); ui.focus = 10; return 0; }
    const char *e = form.f[2];
    if (e[0]) {
        int at = -1, dot = -1;
        for (int i = 0; e[i]; i++) { if (e[i] == '@') at = i; if (e[i] == '.' && at >= 0) dot = i; if (e[i] == ' ') at = -99; }
        if (at < 1 || dot < at + 2 || !e[dot + 1]) { copy(error, "That email address doesn't look right", 80); ui.focus = 12; return 0; }
    }
    const char *ph = form.f[1];
    for (int i = 0; ph[i]; i++) {
        char c = ph[i];
        if (!((c >= '0' && c <= '9') || c == '+' || c == ' ' || c == '-' || c == '(' || c == ')')) {
            copy(error, "A phone number can only have digits, spaces and + - ( )", 80); ui.focus = 11; return 0;
        }
    }
    return 1;
}
static int save_form(void) {
    if (!editing_new && cur < 0) return 1;
    if (!check_form()) return 0;
    if (editing_new) {
        if (np >= MAXC) { copy(error, "The address book is full (200 people)", 80); return 0; }
        cur = np++; editing_new = 0;
    }
    people[cur] = form;
    sort_people();
    if (!save_file()) { say("Could not save C:\\Contacts\\Contacts.txt"); return 0; }
    say("Saved");
    return 1;
}
static void switch_to(int i) {                            /* keep what was typed, if it's valid */
    if (changed() && !save_form()) return;
    show(i);
}
static void new_contact(void) {
    if (changed() && !save_form()) return;
    show(-1); editing_new = 1; ui.focus = 10; search[0] = 0;
}
static void delete_current(void) {
    if (editing_new) { show(np ? 0 : -1); return; }
    if (cur < 0) return;
    for (int i = cur; i < np - 1; i++) people[i] = people[i + 1];
    np--;
    save_file();
    show(cur < np ? cur : np - 1);
    say("Contact deleted");
}

/* ---------------- drawing ---------------- */
static uint32_t avatar_color(const char *name) {
    static const uint32_t cols[] = { 0x2563EB, 0x059669, 0xD97706, 0xDC2626, 0x7C3AED, 0xDB2777, 0x0891B2, 0x4D7C0F };
    unsigned h = 7; for (; *name; name++) h = h * 31 + lower(*name);
    return cols[h % 8];
}
static void initials(const char *name, char *out) {
    int k = 0, start = 1;
    for (; *name && k < 2; name++) {
        if (*name == ' ') { start = 1; continue; }
        if (start) { char c = *name; out[k++] = c >= 'a' && c <= 'z' ? c - 32 : c; start = 0; }
    }
    if (!k) out[k++] = '?';
    out[k] = 0;
}
static void avatar(int x, int y, int d, const char *name) {
    char in[3]; initials(name, in);
    ui_rfill(x, y, d, d, d / 2, avatar_color(name));
    if (d >= 60) ui_text_big(x + (d - (int)strlen(in) * 16) / 2, y + (d - 32) / 2, in, UI_WHITE, 2);
    else ui_text_center(x, y + (d - 16) / 2, d, in, UI_WHITE, 1);
}
static int visible(int i) {
    for (int f = 0; f < NF; f++) if (contains(people[i].f[f], search)) return 1;
    return 0;
}
static void draw_list(void) {
    int h = H - STATUS_H - TOP;
    ui_fill(0, UI_MENU_H, LIST_W, H - UI_MENU_H - STATUS_H, 0xF9FAFB);
    ui_fill(LIST_W - 1, UI_MENU_H, 1, H - UI_MENU_H - STATUS_H, UI_BORDER);
    ui_textbox(1, 10, TOP, LIST_W - 20, search, sizeof search);
    if (!search[0] && ui.focus != 1) ui_text(18, TOP + 5, "Search contacts", 0x9CA3AF);
    if (ui_button_primary(3, 10, TOP + 34, LIST_W - 20, 28, "+  New contact")) new_contact();
    int ly = TOP + 72, rows = (h - 72) / ROW_H;
    int shown[MAXC], ns = 0;
    for (int i = 0; i < np; i++) if (visible(i)) shown[ns++] = i;
    if (list_scroll > ns - rows) list_scroll = ns - rows;
    if (list_scroll < 0) list_scroll = 0;
    if (!ns) ui_text_center(0, ly + 20, LIST_W, np ? "Nobody matches" : "No contacts yet", UI_MUTED, 0);
    for (int r = 0; r < rows && list_scroll + r < ns; r++) {
        int i = shown[list_scroll + r], cy = ly + r * ROW_H;
        int hot = ui_hit(6, cy, LIST_W - 12, ROW_H - 4);
        if (i == cur && !editing_new) ui_rfill(6, cy, LIST_W - 12, ROW_H - 4, 6, UI_ACCENT_LIGHT);
        else if (hot) ui_rfill(6, cy, LIST_W - 12, ROW_H - 4, 6, 0xF3F4F6);
        avatar(14, cy + 5, 32, people[i].f[0]);
        char nm[28]; copy(nm, people[i].f[0], (LIST_W - 70) / 8);
        char sub[28]; copy(sub, people[i].f[1][0] ? people[i].f[1] : people[i].f[2], (LIST_W - 70) / 8);
        ui_textb(56, cy + 5, nm, UI_TEXT);
        ui_text(56, cy + 22, sub, UI_MUTED);
        if (ui.pressed && hot) switch_to(i);
    }
    if (ns > rows) {
        int bh = rows * ROW_H, th = bh * rows / ns, ty = ly + (bh - th) * list_scroll / (ns - rows);
        ui_rfill(LIST_W - 7, ty, 4, th, 2, 0x9CA3AF);
        if (ui.pressed && ui_hit(LIST_W - 12, ly, 12, bh)) list_scroll += ui.my < ty ? -rows : rows;
    }
}
static void draw_card(void) {
    int x = LIST_W, w = W - LIST_W;
    ui_fill(x, UI_MENU_H, w, H - UI_MENU_H - STATUS_H, UI_WHITE);
    if (cur < 0 && !editing_new) { ui_text_center(x, H / 2 - 20, w, "Pick someone, or add a new contact", UI_MUTED, 0); return; }
    int px = x + 24, y = TOP + 8;
    avatar(px, y, 64, form.f[0][0] ? form.f[0] : "?");
    char big[40]; copy(big, form.f[0][0] ? form.f[0] : "New contact", (w - 130) / 16 < 39 ? (w - 130) / 16 : 39);
    ui_text_big(px + 80, y, big, UI_TEXT, 2);
    ui_text(px + 80, y + 34, editing_new ? "Not saved yet" : changed() ? "Changed - press Save" : "Saved in C:\\Contacts\\Contacts.txt", UI_MUTED);
    y += 86;
    int fw = w - 48 - 96; if (fw > 420) fw = 420;
    for (int i = 0; i < NF; i++) {
        ui_text(px, y + 5, field_names[i], UI_MUTED);
        if (ui_textbox(10 + i, px + 96, y, fw, form.f[i], FLEN)) save_form();   /* Enter saves */
        y += 36;
    }
    if (error[0]) ui_text(px + 96, y + 2, error, 0xDC2626);
    y += 28;
    if (ui_button_primary(30, px + 96, y, 100, 30, "Save")) save_form();
    if (ui_button(31, px + 206, y, 100, 30, editing_new ? "Cancel" : "Delete")) { if (editing_new) delete_current(); else { ask_delete = 1; ui_redraw(); } }
}
static void draw_status(void) {
    ui_fill(0, H - STATUS_H, W, STATUS_H, 0xF3F4F6);
    ui_fill(0, H - STATUS_H, W, 1, UI_BORDER);
    char s[64], num[12]; int k = 0;
    const char *a = utoa(np, num); while (*a) s[k++] = *a++;
    a = np == 1 ? " contact" : " contacts"; while (*a) s[k++] = *a++;
    s[k] = 0;
    ui_text(10, H - STATUS_H + 4, s, UI_MUTED);
    if ((int)(status_until - ticks()) > 0) ui_text(W - 12 - ui_text_w(status), H - STATUS_H + 4, status, 0x059669);
}

int main(void) {
    people = malloc(sizeof(struct contact) * MAXC);
    ui_init("Contacts", W, H);
    ui_resizable(640, 460);
    makedir(DIR);
    load();
    show(np ? 0 : -1);
    static const char *menus[] = { "File|New contact\tCtrl+N|Save\tCtrl+S|Delete contact|-|Exit", "Help|About Contacts" };
    int about = 0;
    for (;;) {
        if (ui.resized) { W = ui.w; H = ui.h; }
        int m = ui_menubar(menus, 2);
        int dialog = ask_delete || about;
        if (m == 0) new_contact();
        if (m == 1) save_form();
        if (m == 2 && (cur >= 0 || editing_new)) { if (editing_new) delete_current(); else { ask_delete = 1; ui_redraw(); } }
        if (m == 3) { if (!changed() || save_form()) exit(0); }
        if (m == 100) about = 1;
        if (!dialog) {
            if (ui.key == KEY_CTRL('n')) { ui.key = 0; new_contact(); }
            if (ui.key == KEY_CTRL('s')) { ui.key = 0; save_form(); }
            if (ui.key == KEY_CTRL('f')) { ui.key = 0; ui.focus = 1; }
            if (ui.key == '\t' && ui.focus >= 10 && ui.focus < 10 + NF) { ui.focus = ui.focus + 1 < 10 + NF ? ui.focus + 1 : 10; ui.key = 0; }
        }
        if (dialog) ui_mute();
        draw_list();
        draw_card();
        draw_status();
        ui_menubar_draw();
        if (dialog) {
            ui_unmute();
            if (ask_delete) {
                char line[64] = "Delete "; int k = 7;
                for (int i = 0; form.f[0][i] && k < 50; i++) line[k++] = form.f[0][i];
                line[k++] = '?'; line[k] = 0;
                int r = ui_dialog("Delete contact", line, "This can't be undone.", "Delete", "Cancel");
                if (r == 0) delete_current();
                if (r >= 0) ask_delete = 0;
            } else if (about) {
                if (ui_dialog("About Contacts", "Contacts for MyOS 3.0", "Saved in C:\\Contacts\\Contacts.txt", "OK", 0) >= 0) about = 0;
            }
        }
        ui_present();
        ui_wait(1000);
    }
}
