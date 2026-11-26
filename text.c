/* text.c - UTF-8, Arabic shaping and right-to-left order, shared by the kernel (gfx.c,
 * the window manager) and programs (apps/ui.c), like image.c.
 *
 * MyOS text is UTF-8. Plain ASCII is drawn as before; a string with Arabic in it goes
 * through text_layout(), which
 *   1. decodes UTF-8 into characters, remembering where each one starts;
 *   2. shapes Arabic: every letter takes its isolated, initial, middle or final form
 *      depending on whether its neighbours join to it (lam + alef become one ligature;
 *      the short-vowel marks are skipped);
 *   3. orders the line for display - a simplified version of the Unicode bidi rules:
 *      the paragraph is right-to-left when its first letter is Arabic; runs of Arabic go
 *      right to left, runs of Latin letters and numbers stay left to right, spaces and
 *      punctuation take the direction around them; brackets are mirrored in Arabic runs.
 * Every character is one 8x16 cell, so screen columns still work as before. */
#include "text.h"
#include "arabic_font.h"

int utf8_decode(const char *s, int n, int *i) {
    const unsigned char *u = (const unsigned char *)s;
    int c = u[*i];
    if (c < 0x80) { (*i)++; return c; }
    int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    int cp = len == 4 ? c & 7 : len == 3 ? c & 15 : len == 2 ? c & 31 : '?';
    if (len == 1) { (*i)++; return '?'; }                      /* a stray continuation byte */
    for (int k = 1; k < len; k++) {
        if (*i + k >= n || (u[*i + k] & 0xC0) != 0x80) { (*i)++; return '?'; }
        cp = cp << 6 | (u[*i + k] & 63);
    }
    *i += len;
    return cp;
}
int utf8_encode(int cp, char *out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | cp >> 6); out[1] = (char)(0x80 | (cp & 63)); return 2; }
    out[0] = (char)(0xE0 | cp >> 12); out[1] = (char)(0x80 | (cp >> 6 & 63)); out[2] = (char)(0x80 | (cp & 63));
    return 3;
}
int utf8_next(const char *s, int n, int i) {                     /* where the character after i starts */
    if (i >= n) return n;
    i++;
    while (i < n && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
    return i;
}
int utf8_prev(const char *s, int i) {                            /* where the character before i starts */
    if (i <= 0) return 0;
    i--;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return i;
}
int utf8_count(const char *s, int a, int b) {                    /* characters in s[a..b) */
    int n = 0;
    for (int i = a; i < b; i++) if (((unsigned char)s[i] & 0xC0) != 0x80) n++;
    return n;
}
int utf8_skip(const char *s, int n, int a, int count) {          /* the byte count characters after a */
    while (count-- > 0 && a < n) a = utf8_next(s, n, a);
    return a;
}

/* ---- Arabic letters: their isolated presentation form and how they join ---- */
/* joining: 0 none, 1 joins on the right only (to the letter before), 2 both sides */
static const unsigned short ar_iso[] = {          /* U+0621 .. U+064A */
    0xFE80, 0xFE81, 0xFE83, 0xFE85, 0xFE87, 0xFE89, 0xFE8D, 0xFE8F, 0xFE93, 0xFE95, 0xFE99, 0xFE9D, 0xFEA1, 0xFEA5,
    0xFEA9, 0xFEAB, 0xFEAD, 0xFEAF, 0xFEB1, 0xFEB5, 0xFEB9, 0xFEBD, 0xFEC1, 0xFEC5, 0xFEC9, 0xFECD,
    0, 0, 0, 0, 0, 0x0640,                         /* U+063B..063F unused, tatweel */
    0xFED1, 0xFED5, 0xFED9, 0xFEDD, 0xFEE1, 0xFEE5, 0xFEE9, 0xFEED, 0xFEEF, 0xFEF1 };
static const unsigned char ar_join[] = {
    0, 1, 1, 1, 1, 2, 1, 2, 1, 2, 2, 2, 2, 2, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2,
    0, 0, 0, 0, 0, 2,
    2, 2, 2, 2, 2, 2, 2, 1, 1, 2 };
static int is_letter_ar(int c) { return c >= 0x0621 && c <= 0x064A && (c > 0x063A ? c >= 0x0640 : 1) && ar_iso[c - 0x0621]; }
static int is_mark(int c) { return (c >= 0x064B && c <= 0x065F) || c == 0x0670; }   /* short vowels: not drawn */
static int joining(int c) { return is_letter_ar(c) ? ar_join[c - 0x0621] : 0; }
int text_is_rtl_char(int c) { return (c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF); }
static int is_ltr_char(int c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= 0xC0 && c < 0x590); }
static int is_digit(int c) { return (c >= '0' && c <= '9') || (c >= 0x0660 && c <= 0x0669); }

int text_has_rtl(const char *s, int n) {
    for (int i = 0; i < n; ) { int c = utf8_decode(s, n, &i); if (text_is_rtl_char(c)) return 1; }
    return 0;
}
int text_cells(const char *s) {                                   /* screen cells a nul-terminated string takes */
    int n = 0; while (s[n]) n++;
    static struct tcell c[TEXT_MAX];
    int rtl;
    for (int i = 0; i < n; i++) if ((unsigned char)s[i] >= 0x80) return text_layout(s, n, c, TEXT_MAX, &rtl);
    return n;
}

static int auto_dir;                                               /* 1: the paragraph's own direction (text being edited) */
int text_layout_para(const char *s, int n, struct tcell *out, int max, int *rtl) {
    auto_dir = 1;
    int m = text_layout(s, n, out, max, rtl);
    auto_dir = 0;
    return m;
}
int text_layout(const char *s, int n, struct tcell *out, int max, int *rtl) {
    static struct { int cp, orig, at, dir; } ch[TEXT_MAX];        /* logical order; dir 1 = RTL, 0 = LTR, -1 neutral (static: big for a stack) */
    int m = 0;
    for (int i = 0; i < n && m < TEXT_MAX; ) {
        int at = i, c = utf8_decode(s, n, &i);
        if (is_mark(c)) continue;                                  /* vowel marks are skipped */
        if (c == '\t') c = ' ';
        ch[m].cp = ch[m].orig = c; ch[m].at = at; m++;
    }
    /* shaping: the form of each Arabic letter depends on its neighbours */
    for (int k = 0; k < m; k++) {
        int c = ch[k].orig;
        if (!is_letter_ar(c)) continue;
        int prev = k > 0 ? ch[k - 1].orig : 0, next = k + 1 < m ? ch[k + 1].orig : 0;
        int jp = joining(prev) == 2;                               /* the letter before joins forward to this one */
        int jn = joining(c) == 2 && joining(next) > 0;             /* this one joins to the next */
        if (c == 0x0644 && (next == 0x0622 || next == 0x0623 || next == 0x0625 || next == 0x0627)) {   /* lam + alef */
            int base = next == 0x0622 ? 0xFEF5 : next == 0x0623 ? 0xFEF7 : next == 0x0625 ? 0xFEF9 : 0xFEFB;
            ch[k].cp = base + jp;
            ch[k].orig = next;                                     /* joins like the alef: not to the next letter */
            for (int q = k + 1; q < m - 1; q++) ch[q] = ch[q + 1];  /* the alef is part of the ligature now */
            m--;
            continue;
        }
        if (c == 0x0640) continue;                                 /* tatweel keeps its own shape */
        int iso = ar_iso[c - 0x0621];
        if (joining(c) == 1) ch[k].cp = iso + (jp ? 1 : 0);
        else if (joining(c) == 2) ch[k].cp = jp && jn ? iso + 3 : jp ? iso + 1 : jn ? iso + 2 : iso;
        else ch[k].cp = iso;
    }
    /* directions, and the paragraph direction from the first strong character */
    int para = -1;
    for (int k = 0; k < m; k++) {
        int c = ch[k].cp;
        ch[k].dir = text_is_rtl_char(c) && !is_digit(c) ? 1 : (is_ltr_char(c) || is_digit(c)) ? 0 : -1;
        if (para < 0 && ch[k].dir >= 0 && !is_digit(c)) para = ch[k].dir;
    }
    if (para < 0 || !auto_dir) para = 0;                           /* labels and names: an English (LTR) interface */
    *rtl = para;
    /* neutrals (spaces, punctuation) between two runs of the same direction take it; otherwise the paragraph's */
    for (int k = 0; k < m; k++) {
        if (ch[k].dir >= 0) continue;
        int a = k - 1; while (a >= 0 && ch[a].dir < 0) a--;
        int b = k + 1; while (b < m && ch[b].dir < 0) b++;
        int da = a >= 0 ? ch[a].dir : para, db = b < m ? ch[b].dir : para;
        ch[k].dir = da == db ? da : para;
    }
    /* order: in an RTL paragraph the runs go right to left; an RTL run's characters go right to left */
    int o = 0;
    int start = para ? m - 1 : 0, step = para ? -1 : 1;
    for (int k = start; k >= 0 && k < m && o < max; ) {
        int d = ch[k].dir, e = k;
        while (e + step >= 0 && e + step < m && ch[e + step].dir == d) e += step;
        int lo = k < e ? k : e, hi = k < e ? e : k;               /* the run is ch[lo..hi] */
        if (d == 1) for (int q = hi; q >= lo && o < max; q--) {    /* right to left */
            int c = ch[q].cp;
            if (c == '(') c = ')'; else if (c == ')') c = '('; else if (c == '[') c = ']'; else if (c == ']') c = '[';
            out[o].cp = (unsigned short)c; out[o].at = (unsigned short)ch[q].at; o++;
        } else for (int q = lo; q <= hi && o < max; q++) { out[o].cp = (unsigned short)ch[q].cp; out[o].at = (unsigned short)ch[q].at; o++; }
        k = e + step;
    }
    return o;
}

/* Where to draw the caret for byte position pos (0..n) of a laid-out line, in cells
 * from the left. cells: the line's layout (count = nc). */
int text_caret_cell(const struct tcell *cells, int nc, int rtl, int pos, int n) {
    if (pos >= n) return rtl ? 0 : nc;                            /* after the last character */
    for (int k = 0; k < nc; k++)
        if (cells[k].at == pos) {
            int rtl_char = text_is_rtl_char(cells[k].cp) && !is_digit(cells[k].cp);
            return rtl_char ? k + 1 : k;                          /* before an RTL character = on its right side */
        }
    int best = -1;                                                /* inside a ligature or after a skipped mark: */
    for (int k = 0; k < nc; k++) if (cells[k].at < pos && (best < 0 || cells[k].at > cells[best].at)) best = k;
    if (best >= 0) return text_is_rtl_char(cells[best].cp) ? best : best + 1;   /* next to the cell it belongs to */
    return rtl ? 0 : nc;
}
/* The byte position for a click at cell col of a laid-out line. */
int text_pos_at_cell(const struct tcell *cells, int nc, int rtl, int col, int n) {
    if (col < 0) col = 0;
    if (col >= nc) return rtl ? 0 : n;
    int c = cells[col].cp;
    if (text_is_rtl_char(c) && !is_digit(c)) return cells[col].at;   /* left half of an RTL letter = after it */
    return cells[col].at;
}

const unsigned char *text_arabic_glyph(int cp, int bold) {
    int i = -1;
    if (cp >= 0xFE70 && cp <= 0xFEFF) i = cp - 0xFE70;
    else if (cp == 0x060C) i = 144;
    else if (cp == 0x061B) i = 145;
    else if (cp == 0x061F) i = 146;
    else if (cp == 0x0640) i = 147;
    else if (cp >= 0x0660 && cp <= 0x0669) i = 148 + cp - 0x0660;
    if (i < 0) return 0;
    return bold ? arabic_bold[i] : arabic_regular[i];
}
