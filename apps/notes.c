/* notes.c - Notes: quick notes that save themselves, built with the MyOS toolkit (ui.h).
 *
 * Every note is a text file in C:\Notes named after its first line, so notes can also be
 * opened in Notepad or File Explorer. The list on the left shows each note's title and
 * the start of its text; the search box filters it. Notes save themselves a moment after
 * you stop typing (and when you switch notes). Ctrl+N new note, Ctrl+F search,
 * Ctrl+S save now. */
#include "ui.h"
#include "../text.h"

#define DIR "/Notes"
#define MAXN 100
#define NOTE_MAX 8000
#define LIST_W 250
#define TOP (UI_MENU_H + 8)
#define STATUS_H 24
#define CARD_H 54

struct note { char file[64]; char *text; int dirty; uint32_t edited; };
static struct note notes[MAXN];
static int nn, cur = -1, list_scroll, ask_delete, show_about;
static char search[64];
static struct ui_textarea ta;
static int W = 780, H = 520;
static char status[80];
static uint32_t status_until;

static void say(const char *m) { int i = 0; for (; m[i] && i < 79; i++) status[i] = m[i]; status[i] = 0; status_until = ticks() + 2500; ui_redraw(); }
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int contains(const char *hay, const char *needle) {     /* case-insensitive */
    if (!needle[0]) return 1;
    for (; *hay; hay++) {
        int i = 0;
        while (needle[i] && lower(hay[i]) == lower(needle[i])) i++;
        if (!needle[i]) return 1;
    }
    return 0;
}
static int whole_chars(const char *s, int k) {               /* drop a UTF-8 character cut in half at the end */
    if (!k) return 0;
    int p = utf8_prev(s, k), c = (unsigned char)s[p];
    int len = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
    return p + len > k ? p : k;
}
static void title_of(const char *text, char *out, int max) {  /* the first non-empty line */
    while (*text == '\n' || *text == ' ') text++;
    int k = 0;
    while (text[k] && text[k] != '\n' && k < max - 1) { out[k] = text[k]; k++; }
    k = whole_chars(out, k);
    while (k > 0 && out[k - 1] == ' ') k--;
    out[k] = 0;
    if (!k) { const char *u = "Untitled"; while (*u && k < max - 1) out[k++] = *u++; out[k] = 0; }
}
static void preview_of(const char *text, char *out, int max) { /* the text after the title, on one line */
    while (*text == '\n' || *text == ' ') text++;
    while (*text && *text != '\n') text++;
    int k = 0, sp = 0;
    for (; *text && k < max - 1; text++) {
        char c = *text == '\n' ? ' ' : *text;
        if (c == ' ') { if (k && !sp) out[k++] = ' '; sp = 1; } else { out[k++] = c; sp = 0; }
    }
    k = whole_chars(out, k);
    out[k] = 0;
}
static void path_of(const char *file, char *out) {
    const char *d = DIR "/"; int k = 0;
    while (*d) out[k++] = *d++;
    while (*file) out[k++] = *file++;
    out[k] = 0;
}
static int same_ci(const char *a, const char *b) { while (*a && lower(*a) == lower(*b)) { a++; b++; } return !*a && !*b; }
/* a file name for this note: its title (without characters FAT can't take) + ".txt", unique */
static void file_for(int i, char *out) {
    char t[40]; title_of(notes[i].text, t, 36);
    int k = 0;
    for (int j = 0; t[j]; j++) {
        char c = t[j];
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '-';
        out[k++] = c;
    }
    while (k && (out[k - 1] == '.' || out[k - 1] == ' ')) k--;
    if (!k) { const char *u = "Untitled"; while (*u) out[k++] = *u++; }
    out[k] = 0;
    char base[48]; for (int j = 0; j <= k; j++) base[j] = out[j];
    for (int n = 1; n < 100; n++) {
        int q = 0; while (base[q]) { out[q] = base[q]; q++; }
        if (n > 1) { out[q++] = ' '; out[q++] = '('; char num[8]; utoa(n, num); for (int z = 0; num[z]; z++) out[q++] = num[z]; out[q++] = ')'; }
        const char *e = ".txt"; while (*e) out[q++] = *e++;
        out[q] = 0;
        int taken = 0;
        for (int o = 0; o < nn; o++) if (o != i && same_ci(notes[o].file, out)) taken = 1;
        if (!taken) return;
    }
}
static void save(int i) {
    if (i < 0 || i >= nn || !notes[i].dirty) return;
    char name[64], p[96]; file_for(i, name);
    path_of(name, p);
    if (writefile(p, notes[i].text, strlen(notes[i].text)) < 0) { say("Could not save - is the disk full?"); return; }
    if (notes[i].file[0] && !same_ci(notes[i].file, name)) {   /* the title changed: drop the old file */
        char old[96]; path_of(notes[i].file, old); deletefile(old);
    }
    int k = 0; while (name[k]) { notes[i].file[k] = name[k]; k++; }
    notes[i].file[k] = 0;
    notes[i].dirty = 0;
}
static void save_all(void) { for (int i = 0; i < nn; i++) save(i); }
static void open_note(int i) {
    if (i == cur) return;
    save(cur);
    cur = i;
    ui_textarea_reset(&ta);
    if (i >= 0) { ta.caret = ta.anchor = strlen(notes[i].text); ui.focus = 2; }
}
static int new_note(const char *text) {
    if (nn >= MAXN) { say("That's the most notes this version can keep (100)"); return -1; }
    for (int i = nn; i > 0; i--) notes[i] = notes[i - 1];   /* new notes go to the top */
    nn++;
    if (cur >= 0) cur++;
    struct note *n = &notes[0];
    n->file[0] = 0; n->text = malloc(NOTE_MAX); n->dirty = 1; n->edited = 0;
    int k = 0; for (; text[k] && k < NOTE_MAX - 1; k++) n->text[k] = text[k];
    n->text[k] = 0;
    search[0] = 0; list_scroll = 0;
    if (text[0]) save(0);
    open_note(0);
    return 0;
}
static void delete_current(void) {
    if (cur < 0) return;
    if (notes[cur].file[0]) { char p[96]; path_of(notes[cur].file, p); deletefile(p); }
    for (int i = cur; i < nn - 1; i++) notes[i] = notes[i + 1];
    nn--;
    int next = cur < nn ? cur : nn - 1;
    cur = -1;
    open_note(next);
    say("Note deleted");
}
static void load(void) {
    makedir(DIR);
    static struct dirent d[MAXN];
    int n = listdir(DIR, d, MAXN);
    for (int i = 0; i < n && nn < MAXN; i++) {
        int l = strlen(d[i].name);
        if (d[i].is_dir || l < 5 || !same_ci(d[i].name + l - 4, ".txt")) continue;
        struct note *t = &notes[nn];
        int k = 0; while (d[i].name[k] && k < 63) { t->file[k] = d[i].name[k]; k++; }
        t->file[k] = 0;
        t->text = malloc(NOTE_MAX);
        char p[96]; path_of(t->file, p);
        int got = readfile(p, t->text, NOTE_MAX - 1);
        if (got < 0) got = 0;
        int q = 0; for (int j = 0; j < got; j++) if (t->text[j] != '\r') t->text[q++] = t->text[j];
        t->text[q] = 0;
        t->dirty = 0;
        nn++;
    }
    for (int i = 1; i < nn; i++)                              /* A to Z by title */
        for (int j = i; j > 0; j--) {
            char a[40], b[40]; title_of(notes[j - 1].text, a, 40); title_of(notes[j].text, b, 40);
            int k = 0; while (a[k] && lower(a[k]) == lower(b[k])) k++;
            if (lower(a[k]) <= lower(b[k])) break;
            struct note t = notes[j]; notes[j] = notes[j - 1]; notes[j - 1] = t;
        }
}

/* ---------------- drawing ---------------- */
static int visible(int i) { return contains(notes[i].text, search); }
static void draw_list(void) {
    int x = 0, y = TOP, h = H - STATUS_H - TOP;
    ui_fill(x, UI_MENU_H, LIST_W, H - UI_MENU_H - STATUS_H, 0xF9FAFB);
    ui_fill(LIST_W - 1, UI_MENU_H, 1, H - UI_MENU_H - STATUS_H, UI_BORDER);
    ui_textbox(1, 10, y, LIST_W - 20, search, sizeof search);
    if (!search[0] && ui.focus != 1) ui_text(18, y + 5, "Search notes", 0x9CA3AF);
    if (ui_button_primary(3, 10, y + 34, LIST_W - 20, 28, "+  New note")) new_note("");
    int ly = y + 72, rows = (h - 72) / CARD_H;
    int shown[MAXN], ns = 0;
    for (int i = 0; i < nn; i++) if (visible(i)) shown[ns++] = i;
    if (list_scroll > ns - rows) list_scroll = ns - rows;
    if (list_scroll < 0) list_scroll = 0;
    if (!ns) ui_text_center(0, ly + 20, LIST_W, nn ? "No notes match" : "No notes yet", UI_MUTED, 0);
    for (int r = 0; r < rows && list_scroll + r < ns; r++) {
        int i = shown[list_scroll + r], cy = ly + r * CARD_H;
        int hot = ui_hit(6, cy, LIST_W - 12, CARD_H - 4);
        if (i == cur) ui_rfill(6, cy, LIST_W - 12, CARD_H - 4, 6, UI_ACCENT_LIGHT);
        else if (hot) ui_rfill(6, cy, LIST_W - 12, CARD_H - 4, 6, 0xF3F4F6);
        char t[28], p[28];
        title_of(notes[i].text, t, (LIST_W - 40) / 8);
        preview_of(notes[i].text, p, (LIST_W - 40) / 8);
        ui_textb(16, cy + 7, t, UI_TEXT);
        ui_text(16, cy + 26, p[0] ? p : "No more text", UI_MUTED);
        if (notes[i].dirty) ui_rfill(LIST_W - 22, cy + 10, 6, 6, 3, UI_ACCENT);
        if (ui.pressed && hot) open_note(i);
    }
    if (ns > rows) {                                          /* more notes: a scroll bar + PgUp/PgDn on it */
        int bh = rows * CARD_H, th = bh * rows / ns, ty = ly + (bh - th) * list_scroll / (ns - rows);
        ui_rfill(LIST_W - 7, ty, 4, th, 2, 0x9CA3AF);
        if (ui.pressed && ui_hit(LIST_W - 12, ly, 12, bh)) list_scroll += ui.my < ty ? -rows : rows;
    }
}
static int words(const char *s) { int n = 0, in = 0; for (; *s; s++) { int w = (unsigned char)*s > ' '; if (w && !in) n++; in = w; } return n; }
static void draw_editor(void) {
    int x = LIST_W, w = W - LIST_W;
    ui_fill(x, UI_MENU_H, w, H - UI_MENU_H - STATUS_H, UI_WHITE);
    if (cur < 0) {
        ui_text_center(x, H / 2 - 20, w, "Pick a note, or make a new one", UI_MUTED, 0);
        return;
    }
    if (ui_button(4, W - 98, TOP, 86, 26, "Delete")) { ask_delete = 1; ui_redraw(); }
    char t[48]; title_of(notes[cur].text, t, (w - 130) / 8);
    ui_textb(x + 16, TOP + 5, t, UI_TEXT);
    if (ui_textarea(2, x + 10, TOP + 34, w - 20, H - STATUS_H - TOP - 44, notes[cur].text, NOTE_MAX, &ta)) {
        notes[cur].dirty = 1; notes[cur].edited = ticks();
    }
}
static void draw_status(void) {
    ui_fill(0, H - STATUS_H, W, STATUS_H, 0xF3F4F6);
    ui_fill(0, H - STATUS_H, W, 1, UI_BORDER);
    char s[96], num[12]; int k = 0;
    const char *a = utoa(nn, num); while (*a) s[k++] = *a++;
    const char *b = nn == 1 ? " note in C:\\Notes" : " notes in C:\\Notes"; while (*b) s[k++] = *b++;
    s[k] = 0;
    ui_text(10, H - STATUS_H + 4, s, UI_MUTED);
    k = 0;
    if ((int)(status_until - ticks()) > 0) { const char *m = status; while (*m) s[k++] = *m++; }
    else if (cur >= 0) {
        const char *m = notes[cur].dirty ? "Editing...   " : "Saved   "; while (*m) s[k++] = *m++;
        m = utoa(words(notes[cur].text), num); while (*m) s[k++] = *m++;
        m = " words, "; while (*m) s[k++] = *m++;
        m = utoa(utf8_count(notes[cur].text, 0, strlen(notes[cur].text)), num); while (*m) s[k++] = *m++;
        m = " characters"; while (*m) s[k++] = *m++;
    }
    s[k] = 0;
    ui_text(W - 12 - ui_text_w(s), H - STATUS_H + 4, s, UI_MUTED);
}

int main(void) {
    ui_init("Notes", W, H);
    ui_resizable(600, 360);
    load();
    if (!nn) new_note("Welcome to Notes\n\nNotes save themselves a moment after you stop typing.\n"
                      "Each note is a text file in C:\\Notes, named after its first line.\n\n"
                      "Ctrl+N  new note\nCtrl+F  search\nCtrl+S  save now\n");
    else open_note(0);
    static const char *menus[] = { "File|New note\tCtrl+N|Save now\tCtrl+S|Delete note|-|Exit", "Help|About Notes" };
    for (;;) {
        if (ui.resized) { W = ui.w; H = ui.h; }
        int m = ui_menubar(menus, 2);
        int dialog = ask_delete || show_about;
        if (m == 0) new_note("");
        if (m == 1) { save_all(); say("Saved"); }
        if (m == 2 && cur >= 0) ask_delete = 1;
        if (m == 3) { save_all(); exit(0); }
        if (m == 100) show_about = 1;
        if (!dialog) {
            if (ui.key == KEY_CTRL('n')) { ui.key = 0; new_note(""); }
            if (ui.key == KEY_CTRL('s')) { ui.key = 0; save_all(); say("Saved"); }
            if (ui.key == KEY_CTRL('f')) { ui.key = 0; ui.focus = 1; }
        }
        if (dialog) ui_mute();
        draw_list();
        draw_editor();
        draw_status();
        ui_menubar_draw();
        if (dialog) {
            ui_unmute();
            if (ask_delete) {
                char t[40]; title_of(notes[cur].text, t, 30);
                int r = ui_dialog("Delete this note?", t, "It can't be brought back.", "Delete", "Cancel");
                if (r == 0) delete_current();
                if (r >= 0) ask_delete = 0;
            } else if (show_about) {
                if (ui_dialog("About Notes", "Notes for MyOS 3.0", "Each note is a .txt file in C:\\Notes.", "OK", 0) >= 0) show_about = 0;
            }
        }
        /* save a moment after the typing stops */
        if (cur >= 0 && notes[cur].dirty && (int)(ticks() - notes[cur].edited) > 800) save(cur);
        ui_present();
        ui_wait(cur >= 0 && notes[cur].dirty ? 300 : 1000);
    }
}
