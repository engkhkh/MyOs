/* web.c - a simple Web browser for MyOS.
 *
 * Type an address (Ctrl+L or click the address bar), press Enter. The browser
 *   1. looks the host name up with DNS,
 *   2. opens a TCP connection to port 80,
 *   3. sends an HTTP "GET" request and reads the reply,
 *   4. turns the HTML into readable text: headings, paragraphs, lists, and
 *      blue links you can click,
 *   5. then downloads the page's pictures (PNG, JPEG, GIF, BMP) one by one and
 *      puts them into the page - small ones inside the text, big ones as blocks.
 * Back button / Backspace goes back. Arrows, PgUp/PgDn, Space, Home/End scroll.
 * Plain http:// only: https:// needs encryption, which MyOS doesn't have yet.
 */
#include "myos.h"
#include "../image.h"

static int W = 820, H = 580;           /* the window: the user can resize / maximise it */
#define BAR 40                         /* toolbar */
#define STAT 20                        /* status bar */
#define LINE_H 18
#define COLS ((W - 36) / 8)
#define ROWS ((H - BAR - STAT - 8) / LINE_H)
#define MAXCOLS ((1024 - 36) / 8)            /* the widest a page line can be (window maximised) */
#define MAXLINES 16000
#define MAXLINKS 1500
#define MAXDOWN (1024 * 1024)
#define MAXIMGDOWN (4 * 1024 * 1024)   /* biggest picture file */
#define MAXIMG 80                      /* pictures per page */
#define IMG_MEM (40 * 1024 * 1024)     /* memory for the page's pictures */

/* text styles */
enum { ST_TEXT, ST_HEAD, ST_LINK, ST_PRE, ST_BOLD, ST_DIM };
static const uint32_t style_col[] = { 0x1F2937, 0x1E3A8A, 0x1D4ED8, 0x065F46, 0x111827, 0x6B7280 };

struct cell { char c; uint8_t st; int16_t link; };
static struct cell *page;              /* MAXLINES x COLS */
static int nlines, top_line;
static char *links[MAXLINKS];
static int nlinks;

/* pictures: one entry per different address on the page */
struct webimg { char *src; int state; int w, h; uint32_t *px; int aw, ah; };   /* state: 0 waiting, 1 ready, 2 failed */
static struct webimg imgs[MAXIMG];
static int nimgs;
static uint32_t img_mem;
/* where pictures sit on the laid-out page */
struct place { int img, line, x, yoff; };
static struct place places[MAXIMG * 4];
static int nplaces;
static char *html; static int html_plain, html_owned;  /* the page source, kept for laying out again */
static struct event stash; static int stashed;          /* a click that stopped picture loading */

static char url[512], addr[512], title[96], status[128];
static int addr_focus = 1;
static char history[16][512];
static int nhist;

/* ---------- small string helpers ---------- */
static int slen(const char *s) { return strlen(s); }
static void scpy(char *d, const char *s, int max) { int i = 0; for (; s[i] && i < max - 1; i++) d[i] = s[i]; d[i] = 0; }
static void scat(char *d, const char *s, int max) { int n = slen(d); scpy(d + n, s, max - n); }
static char lc(char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int starts_ci(const char *s, const char *p) { while (*p) if (lc(*s++) != *p++) return 0; return 1; }
static char *find_ci(char *s, const char *p) { for (; *s; s++) if (starts_ci(s, p)) return s; return 0; }
static char *sdup(const char *s) { char *d = malloc(slen(s) + 1); if (d) scpy(d, s, slen(s) + 1); return d; }

/* ---------- drawing ---------- */
static void set_status(const char *s) {
    scpy(status, s, sizeof status);
    fill(0, H - STAT, W, STAT, 0xF3F4F6);
    text(8, H - STAT + 2, status, 0x4B5563);
}

static void draw_toolbar(void) {
    fill(0, 0, W, BAR, 0xE5E7EB);
    fill(8, 7, 36, 26, nhist > 1 ? 0xFFFFFF : 0xF3F4F6); text(18, 12, "<", nhist > 1 ? 0x111827 : 0x9CA3AF);
    fill(48, 7, 36, 26, 0xFFFFFF); text(56, 12, "(R)", 0x111827);
    fill(92, 7, W - 100, 26, addr_focus ? 0x2563EB : 0xD1D5DB);
    fill(93, 8, W - 102, 24, 0xFFFFFF);
    int n = slen(addr), maxc = (W - 120) / 8, st = n > maxc ? n - maxc : 0;
    text(100, 12, addr + st, 0x111827);
    if (addr_focus) fill(100 + (n - st) * 8, 11, 2, 18, 0x111827);
}

static void draw_page(void) {
    int y0 = BAR, h = H - BAR - STAT;
    fill(0, y0, W, h, 0xFFFFFF);
    char run[COLS + 1];
    for (int r = 0; r < ROWS && top_line + r < nlines; r++) {
        struct cell *ln = page + (top_line + r) * COLS;
        int y = y0 + 6 + r * LINE_H;
        for (int c = 0; c < COLS;) {
            if (!ln[c].c) break;
            int st = ln[c].st, k = 0, start = c;
            while (c < COLS && ln[c].c && ln[c].st == st) run[k++] = ln[c++].c;
            run[k] = 0;
            text(14 + start * 8, y, run, style_col[st]);
            if (st == ST_HEAD || st == ST_BOLD) text(15 + start * 8, y, run, style_col[st]);   /* fake bold */
            if (st == ST_LINK) fill(14 + start * 8, y + 15, k * 8, 1, style_col[st]);
        }
    }
    for (int i = 0; i < nplaces; i++) {                       /* pictures */
        struct place *pl = &places[i];
        struct webimg *im = &imgs[pl->img];
        int dh = im->state == 1 ? im->h : im->ah, dw = im->state == 1 ? im->w : im->aw;
        int y = y0 + 6 + (pl->line - top_line) * LINE_H + pl->yoff;
        if (y >= H - STAT || y + dh <= y0) continue;
        int skip = y < y0 ? y0 - y : 0, hh = dh - skip;
        if (y + skip + hh > H - STAT) hh = H - STAT - y - skip;
        if (hh <= 0) continue;
        if (im->state == 1) blit(pl->x, y + skip, dw, hh, im->px + skip * dw, dw);
        else {                                                 /* still loading: a grey box of the right size */
            fill(pl->x, y + skip, dw, hh, 0xE5E7EB);
            if (!skip && dh > 20 && dw > 70) text(pl->x + 6, y + 4, "picture...", 0x9CA3AF);
        }
    }
    if (nlines > ROWS) {                                       /* scroll bar */
        fill(W - 12, y0 + 2, 8, h - 4, 0xF3F4F6);
        int bh = (h - 4) * ROWS / nlines; if (bh < 20) bh = 20;
        int by = (h - 4 - bh) * top_line / (nlines - ROWS);
        fill(W - 12, y0 + 2 + by, 8, bh, 0x9CA3AF);
    }
}

static void redraw(void) { draw_toolbar(); draw_page(); set_status(status); }

/* ---------- page building: words go onto lines, wrapping at COLS ---------- */
static int cx, cur_link = -1, cur_style, in_pre, pending_space, blank_lines;

static int truncated, hang;
static void newline(void) {
    if (nlines >= MAXLINES - 1) {                 /* page too long: stop here, say so */
        if (!truncated) {
            truncated = 1;
            struct cell *l = page + (nlines - 1) * COLS;
            const char *m = "[ The rest of this page is too long to show. ]";
            for (int i = 0; i < COLS; i++) { l[i].c = 0; l[i].link = -1; }
            for (int i = 0; m[i] && i < COLS; i++) { l[i].c = m[i]; l[i].st = ST_DIM; }
        }
        cx = COLS; return;
    }
    nlines++;
    memset(page + (nlines - 1) * COLS, 0, COLS * sizeof(struct cell));   /* clear lines only as they are used */
    cx = 0; pending_space = 0;
    struct cell *l = page + (nlines - 1) * COLS;
    for (; cx < hang && cx < COLS / 2; cx++) { l[cx].c = ' '; l[cx].link = -1; }   /* line up wrapped list items */
}
static void put_cell(char c, int st, int link) {
    if (truncated) return;
    if (cx >= COLS) newline();
    if (truncated) return;
    struct cell *p = page + (nlines - 1) * COLS + cx++;
    p->c = c; p->st = (uint8_t)st; p->link = (int16_t)link;
    blank_lines = 0;
}
static void put_char(char c, int st) { put_cell(c, st, cur_link); }
static void para_break(int lines) {                           /* end the line, then leave blank lines */
    hang = 0;
    if (cx > 0) newline();
    while (blank_lines < lines) { newline(); blank_lines++; }
}
static int style_now(void) { return cur_link >= 0 ? ST_LINK : cur_style; }

static char word[256]; static int wlen;
static void flush_word(void) {
    if (!wlen) return;
    if (pending_space && cx > 0) {
        if (cx + 1 + wlen > COLS) newline();
        else {                                    /* a space inside one link belongs to the link */
            int prev = page[(nlines - 1) * COLS + cx - 1].link;
            if (cur_link >= 0 && prev == cur_link) put_cell(' ', ST_LINK, cur_link); else put_cell(' ', ST_TEXT, -1);
        }
    }
    else if (cx + wlen > COLS && cx > 0) newline();
    for (int i = 0; i < wlen; i++) put_char(word[i], style_now());
    wlen = 0; pending_space = 0;
}
static void add_text_char(char c) {
    if (in_pre) {
        if (c == '\n') { newline(); return; }
        if (c == '\t') { do put_char(' ', ST_PRE); while (cx % 4); return; }
        put_char(c, cur_link >= 0 ? ST_LINK : ST_PRE);
        return;
    }
    if (c == ' ' || c == '\n' || c == '\t' || c == '\r') { flush_word(); pending_space = 1; return; }
    if (wlen < 255) word[wlen++] = c;
    if (wlen >= COLS) flush_word();
}

/* The font has plain ASCII only, so accented letters become their base letter. */
static const char *plain_of(int cp) {
    static const char *lat = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";   /* U+00C0..U+00FF */
    static char one[2];
    if (cp >= 32 && cp < 127) { one[0] = (char)cp; one[1] = 0; return one; }
    if (cp >= 0xC0 && cp <= 0xFF) { one[0] = lat[cp - 0xC0]; one[1] = 0; return one; }
    switch (cp) {
    case 0xA0: return " ";  case 0xA9: return "(c)"; case 0xAE: return "(R)"; case 0xB0: return "o";
    case 0xAB: return "<<"; case 0xBB: return ">>"; case 0xB7: return "."; case 0x2122: return "TM";
    case 0x2018: case 0x2019: return "'"; case 0x201C: case 0x201D: return "\"";
    case 0x2013: return "-"; case 0x2014: return "--"; case 0x2026: return "..."; case 0x2022: return "*";
    case 0x20AC: return "EUR"; case 0xA3: return "GBP";
    }
    return "?";
}

/* &amp; &lt; &#233; ... -> plain characters (accents become the base letter or ?) */
static const char *ent_extra = "";                            /* more characters from the last entity */
static const char *entity(const char *s, char *outc) {
    ent_extra = "";
    static const struct { const char *n; char c; } tab[] = {
        { "amp;", '&' }, { "lt;", '<' }, { "gt;", '>' }, { "quot;", '"' }, { "apos;", '\'' }, { "nbsp;", ' ' },
        { "rsquo;", '\'' }, { "lsquo;", '\'' }, { "rdquo;", '"' }, { "ldquo;", '"' }, { "ndash;", '-' },
        { "mdash;", '-' }, { "hellip;", '.' }, { "bull;", '*' }, { "middot;", '.' } };
    static const struct { const char *n; int cp; } tab2[] = {
        { "copy;", 0xA9 }, { "reg;", 0xAE }, { "trade;", 0x2122 }, { "eacute;", 0xE9 }, { "egrave;", 0xE8 },
        { "aacute;", 0xE1 }, { "agrave;", 0xE0 }, { "ouml;", 0xF6 }, { "uuml;", 0xFC }, { "auml;", 0xE4 },
        { "ccedil;", 0xE7 }, { "ntilde;", 0xF1 }, { "euro;", 0x20AC }, { "pound;", 0xA3 }, { "deg;", 0xB0 },
        { "laquo;", 0xAB }, { "raquo;", 0xBB } };
    for (unsigned i = 0; i < sizeof tab2 / sizeof tab2[0]; i++)
        if (starts_ci(s, tab2[i].n)) { const char *pl = plain_of(tab2[i].cp); *outc = pl[0]; ent_extra = pl + 1; return s + slen(tab2[i].n); }
    for (unsigned i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (starts_ci(s, tab[i].n)) { *outc = tab[i].c; return s + slen(tab[i].n); }
    if (*s == '#') {
        int v = 0; const char *p = s + 1;
        if (*p == 'x' || *p == 'X') { p++; while ((*p >= '0' && *p <= '9') || (lc(*p) >= 'a' && lc(*p) <= 'f')) { v = v * 16 + (*p <= '9' ? *p - '0' : lc(*p) - 'a' + 10); p++; } }
        else while (*p >= '0' && *p <= '9') v = v * 10 + *p++ - '0';
        if (*p == ';') p++;
        const char *pl = plain_of(v);
        *outc = pl[0];
        ent_extra = pl + 1;                                    /* e.g. "(c)": the rest comes after */
        return p;
    }
    *outc = '&';
    return s;
}

static int kstrcmp_ci(const char *a, const char *b) { while (*a && lc(*a) == lc(*b)) { a++; b++; } return lc(*a) - lc(*b); }
static int is_tag(const char *name, const char *t) { return !kstrcmp_ci(name, t); }

/* Find attr="value" inside a tag's text. */
static int get_attr(const char *tag, const char *attr, char *out, int max) {
    int al = slen(attr);
    for (const char *p = tag; *p; p++) {
        if ((p == tag || p[-1] == ' ' || p[-1] == '\n' || p[-1] == '\t') && starts_ci(p, attr) && (p[al] == '=' || p[al] == ' ')) {
            const char *v = p + al; while (*v == ' ') v++;
            if (*v != '=') continue;
            v++; while (*v == ' ') v++;
            char q = (*v == '"' || *v == '\'') ? *v++ : 0;
            int k = 0;
            while (*v && k < max - 1 && (q ? *v != q : (*v != ' ' && *v != '>'))) {
                if (*v == '&') { char c; const char *nv = entity(v + 1, &c); if (nv != v + 1) { out[k++] = c; v = nv; continue; } }
                out[k++] = *v++;
            }
            out[k] = 0;
            return 1;
        }
    }
    return 0;
}

/* ---------- pictures in the page ---------- */
static void resolve_url(const char *base, const char *ref, char *out, int max);
static int size_attr(const char *tag, const char *name) {     /* width="120" -> 120 (not "50%") */
    char v[24];
    if (!get_attr(tag, name, v, sizeof v)) return 0;
    int n = 0, i = 0;
    for (; v[i] >= '0' && v[i] <= '9'; i++) n = n * 10 + v[i] - '0';
    return v[i] == '%' ? 0 : n;
}
static int same_str(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void fit_size(int *w, int *h) {                         /* keep pictures inside the page width */
    int maxw = COLS * 8 - 8;
    if (*w > maxw) { *h = *h * maxw / *w; *w = maxw; }
    if (*h > 1600) { *w = *w * 1600 / *h; *h = 1600; }
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
}
static void put_alt(const char *tag) {
    char alt[80];
    if (get_attr(tag, "alt", alt, sizeof alt) && alt[0]) {
        for (const char *q = "[image: "; *q; q++) add_text_char(*q);
        for (char *q = alt; *q; q++) add_text_char(*q);
        add_text_char(']'); flush_word();
    }
}
static void put_image(const char *tag) {
    char src[512], full[512];
    if (!get_attr(tag, "src", src, sizeof src) || !src[0] || starts_ci(src, "data:") || !starts_ci(url, "http")) { put_alt(tag); return; }
    resolve_url(url, src, full, sizeof full);
    int i = 0;
    while (i < nimgs && !same_str(imgs[i].src, full)) i++;
    if (i == nimgs) {                                          /* a new picture address */
        if (nimgs == MAXIMG) { put_alt(tag); return; }
        imgs[i].src = sdup(full);
        if (!imgs[i].src) { put_alt(tag); return; }
        imgs[i].state = 0; imgs[i].px = 0;
        imgs[i].aw = size_attr(tag, "width"); imgs[i].ah = size_attr(tag, "height");
        if (imgs[i].aw && !imgs[i].ah) imgs[i].aw = 0;
        if (imgs[i].ah && !imgs[i].aw) imgs[i].ah = 0;
        if (imgs[i].aw) fit_size(&imgs[i].aw, &imgs[i].ah);
        nimgs++;
    }
    struct webimg *im = &imgs[i];
    if (im->state == 2 || (im->state == 0 && !im->aw)) { put_alt(tag); return; }   /* failed, or size not known yet */
    if (nplaces == MAXIMG * 4 || truncated) return;
    int w = im->state == 1 ? im->w : im->aw, h = im->state == 1 ? im->h : im->ah, cells = (w + 7) / 8;
    if (h <= 20 && cells <= COLS / 3) {                        /* small: sits in the line, like a letter */
        if (pending_space && cx > 0) { put_cell(' ', ST_TEXT, cur_link); pending_space = 0; }
        if (cx + cells > COLS) newline();
        places[nplaces++] = (struct place){ i, nlines - 1, 14 + cx * 8, (16 - h) / 2 };
        for (int k = 0; k < cells; k++) put_cell(' ', ST_TEXT, cur_link);
        return;
    }
    /* big: a block of whole lines */
    if (cx > hang) newline();
    int lines = (h + 4 + LINE_H - 1) / LINE_H;
    int x = 14 + cx * 8;
    if (x + w > W - 16) x = 14;
    places[nplaces++] = (struct place){ i, nlines - 1, x, 0 };
    for (int l = 0; l < lines && !truncated; l++) {
        for (int k = 0; k < cells && cx < COLS; k++) put_cell(' ', ST_TEXT, cur_link);
        newline();
    }
    blank_lines = 0;
}

static void layout_html(char *h, int plain) {
    nlines = 1; nplaces = 0; cx = 0; cur_link = -1; cur_style = ST_TEXT; in_pre = plain; pending_space = 0; blank_lines = 1; wlen = 0;
    truncated = 0; hang = 0;
    memset(page, 0, COLS * sizeof(struct cell));
    for (int i = 0; i < nlinks; i++) free(links[i]);
    nlinks = 0; title[0] = 0;
    int skip = 0, in_title = 0, list_depth = 0;
    char tbuf[1024];
    for (char *p = h; *p;) {
        if (!plain && *p == '<') {
            if (p[1] == '!' && p[2] == '-' && p[3] == '-') { char *e = find_ci(p + 4, "-->"); p = e ? e + 3 : p + slen(p); continue; }
            char *e = p + 1; int k = 0;
            while (*e && *e != '>') { if (k < 1023) tbuf[k++] = *e; e++; }
            tbuf[k] = 0;
            p = *e ? e + 1 : e;
            int closing = tbuf[0] == '/';
            char name[16]; int n = 0;
            for (char *q = tbuf + closing; *q && *q != ' ' && *q != '\n' && *q != '\t' && *q != '/' && n < 15; q++) name[n++] = lc(*q);
            name[n] = 0;
            if (is_tag(name, "script") || is_tag(name, "style") || is_tag(name, "head") || is_tag(name, "svg") || is_tag(name, "noscript")) {
                if (!closing) { skip++; if (is_tag(name, "head")) skip--; }
                else if (!is_tag(name, "head")) skip = skip > 0 ? skip - 1 : 0;
                if (!closing && !is_tag(name, "head")) {           /* jump to the closing tag */
                    char close[20] = "</"; scat(close, name, 20);
                    char *c = find_ci(p, close);
                    if (c) { p = c; skip--; }
                }
                continue;
            }
            if (is_tag(name, "title")) { in_title = !closing; continue; }
            flush_word();
            if (is_tag(name, "br")) { newline(); blank_lines = 0; }
            else if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) {
                para_break(1);
                cur_style = closing ? ST_TEXT : ST_HEAD;
            } else if (is_tag(name, "p") || is_tag(name, "div") || is_tag(name, "section") || is_tag(name, "article") ||
                       is_tag(name, "header") || is_tag(name, "footer") || is_tag(name, "nav") || is_tag(name, "table") ||
                       is_tag(name, "form") || is_tag(name, "blockquote") || is_tag(name, "dl") || is_tag(name, "center") || is_tag(name, "main")) {
                para_break(is_tag(name, "p") || is_tag(name, "blockquote") ? 1 : 0);
            } else if (is_tag(name, "ul") || is_tag(name, "ol")) {
                list_depth += closing ? -1 : 1; if (list_depth < 0) list_depth = 0;
                para_break(list_depth ? 0 : 1);
            } else if (is_tag(name, "li") && !closing) {
                hang = 0;
                if (cx > 0) newline();
                for (int d = 0; d < list_depth; d++) { put_char(' ', ST_TEXT); put_char(' ', ST_TEXT); }
                put_char('*', ST_DIM); put_char(' ', ST_TEXT);
                hang = cx;
            } else if ((is_tag(name, "tr") || is_tag(name, "dt") || is_tag(name, "dd")) && !closing) {
                if (cx > 0) newline();
            } else if ((is_tag(name, "td") || is_tag(name, "th")) && !closing) {
                if (cx > 0) { put_char(' ', ST_TEXT); put_char(' ', ST_TEXT); }
            } else if (is_tag(name, "hr")) {
                para_break(0);
                for (int c = 0; c < COLS; c++) put_char('-', ST_DIM);
                newline();
            } else if (is_tag(name, "pre")) {
                para_break(1); in_pre = !closing;
            } else if (is_tag(name, "b") || is_tag(name, "strong")) {
                if (cur_style != ST_HEAD) cur_style = closing ? ST_TEXT : ST_BOLD;
            } else if (is_tag(name, "a")) {
                if (closing) cur_link = -1;
                else {
                    char href[400];
                    if (get_attr(tbuf, "href", href, sizeof href) && href[0] != '#' && !starts_ci(href, "javascript:") && nlinks < MAXLINKS) {
                        links[nlinks] = sdup(href);
                        if (links[nlinks]) cur_link = nlinks++;
                    }
                }
            } else if (is_tag(name, "img") && !closing) {
                put_image(tbuf);
            }
            continue;
        }
        char c = *p++;
        const char *extra = "";
        if (c == '&' && !plain) { p = (char *)entity(p, &c); extra = ent_extra; }
        else if ((unsigned char)c >= 0x80) {                   /* UTF-8: decode, then simplify */
            unsigned char u = (unsigned char)c; int cp = 0, more = 0;
            if ((u & 0xE0) == 0xC0) { cp = u & 0x1F; more = 1; }
            else if ((u & 0xF0) == 0xE0) { cp = u & 0x0F; more = 2; }
            else if ((u & 0xF8) == 0xF0) { cp = u & 0x07; more = 3; }
            else continue;
            for (; more && ((unsigned char)*p & 0xC0) == 0x80; more--) cp = cp << 6 | (*p++ & 0x3F);
            const char *pl = plain_of(cp);
            c = pl[0]; extra = pl + 1;
        }
        if (in_title) { int n = slen(title); if (n < 90 && (c != ' ' || (n && title[n - 1] != ' '))) { title[n] = c == '\n' ? ' ' : c; title[n + 1] = 0; } continue; }
        if (skip) continue;
        add_text_char(c);
        while (*extra) add_text_char(*extra++);
    }
    flush_word();
    while (nlines > 1 && !truncated) {                         /* drop trailing empty lines */
        struct cell *l = page + (nlines - 1) * COLS; if (l[0].c) break; nlines--;
    }
}

/* ---------- URLs ---------- */
/* Turn a link into a full http:// address, relative to the current page. */
static void resolve_url(const char *base, const char *ref, char *out, int max) {
    while (*ref == ' ') ref++;
    if (starts_ci(ref, "http://") || starts_ci(ref, "https://")) { scpy(out, ref, max); return; }
    if (ref[0] == '/' && ref[1] == '/') { scpy(out, "http:", max); scat(out, ref, max); return; }
    const char *hs = base + (starts_ci(base, "https://") ? 8 : 7);
    const char *pe = hs; while (*pe && *pe != '/') pe++;           /* end of host */
    int hostlen = pe - base;
    if (hostlen >= max) hostlen = max - 1;
    for (int i = 0; i < hostlen; i++) out[i] = base[i];
    out[hostlen] = 0;
    if (ref[0] == '/') { scat(out, ref, max); return; }
    char dir[512]; scpy(dir, *pe ? pe : "/", sizeof dir);         /* folder of the current page */
    int q = 0; while (dir[q] && dir[q] != '?' && dir[q] != '#') q++; dir[q] = 0;
    int last = 0; for (int i = 0; dir[i]; i++) if (dir[i] == '/') last = i;
    dir[last + 1] = 0;
    scat(dir, ref, sizeof dir);
    /* clean up "./" and "../" */
    char clean[512]; int o = 0;
    for (int i = 0; dir[i] && o < 510;) {
        if (dir[i] == '/' && dir[i + 1] == '.' && dir[i + 2] == '/') { i += 2; continue; }
        if (dir[i] == '/' && dir[i + 1] == '.' && dir[i + 2] == '.' && (dir[i + 3] == '/' || !dir[i + 3])) {
            while (o > 0 && clean[o - 1] != '/') o--;
            if (o > 0) o--;
            i += 3; continue;
        }
        clean[o++] = dir[i++];
    }
    clean[o] = 0;
    if (!clean[0]) scpy(clean, "/", sizeof clean);
    scat(out, clean, max);
}

/* ---------- HTTP ---------- */
static void scroll(int d);
static int cancelled(void) {
    struct event e;
    while (pollevent(&e)) {
        if (e.type == EV_KEY) {
            int k = e.key;
            if (k == KEY_ESC) return 1;
            if (k == KEY_DOWN) scroll(3);
            else if (k == KEY_UP) scroll(-3);
            else if (k == KEY_PGDN || k == ' ') scroll(ROWS - 2);
            else if (k == KEY_PGUP) scroll(-(ROWS - 2));
            else if (k == KEY_HOME) scroll(-1000000);
            else if (k == KEY_END) scroll(1000000);
            else { stash = e; stashed = 1; return 1; }      /* anything else: stop, then do it */
        } else if (e.type == EV_MOUSE_DOWN) { stash = e; stashed = 1; return 1; }
    }
    return 0;
}

static char *body; static int body_len;
static char ctype[64], location[512];
static int http_code;

/* Download url into body. Returns 1 on success (any HTTP status), 0 on failure (status explains). */
static int http_get(const char *u, int maxdown, int quiet) {
    char host[256], path[512];
    const char *p = u + 7;                                     /* after "http://" */
    int k = 0; while (*p && *p != '/' && *p != ':' && k < 255) host[k++] = *p++;
    host[k] = 0;
    int port = 80;
    if (*p == ':') { port = 0; p++; while (*p >= '0' && *p <= '9') port = port * 10 + *p++ - '0'; }
    scpy(path, *p ? p : "/", sizeof path);
    for (char *f = path; *f; f++) if (*f == '#') { *f = 0; break; }

    char m[160]; scpy(m, "Looking up ", 160); scat(m, host, 160); scat(m, "...", 160); if (!quiet) set_status(m);
    unsigned ip = 0; uint32_t t0 = ticks(); int r;
    while ((r = resolve(host, &ip)) == 0) { if (ticks() - t0 > 8000 || cancelled()) { r = -1; break; } sleep(30); }
    if (r < 0) { set_status("Could not find that server (DNS lookup failed)."); return 0; }

    scpy(m, "Connecting to ", 160); scat(m, host, 160); scat(m, "...", 160); if (!quiet) set_status(m);
    int c = tcpopen(ip, port);
    if (c < 0) { set_status("No network connection."); return 0; }
    t0 = ticks();
    while ((r = tcpstatus(c)) == 0) { if (ticks() - t0 > 10000 || cancelled()) { r = -1; break; } sleep(20); }
    if (r < 0) { tcpclose(c); set_status("The server did not accept the connection."); return 0; }

    char req[1024];
    scpy(req, "GET ", sizeof req); scat(req, path, sizeof req); scat(req, " HTTP/1.1\r\nHost: ", sizeof req);
    scat(req, host, sizeof req);
    scat(req, "\r\nUser-Agent: MyOS-Web/1.0\r\nAccept: text/html, text/plain, image/png, image/jpeg, image/gif, */*;q=0.5\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n", sizeof req);
    tcpsend(c, req, slen(req));

    int cap = 65536; body_len = 0;
    if (body) free(body);
    body = malloc(cap + 1);
    if (!body) { tcpclose(c); set_status("Out of memory."); return 0; }
    t0 = ticks();
    uint32_t last_ui = 0;
    for (;;) {
        if (body_len == cap) {
            if (cap >= maxdown) break;
            char *nb = malloc(cap * 2 + 1);
            if (!nb) break;
            memcpy(nb, body, body_len); free(body); body = nb; cap *= 2;
        }
        int n = tcprecv(c, body + body_len, cap - body_len);
        if (n < 0) break;                                      /* server finished */
        if (n == 0) {
            if (ticks() - t0 > 15000 || cancelled()) break;
            sleep(15); continue;
        }
        body_len += n; t0 = ticks();
        if (!quiet && ticks() - last_ui > 200) {
            last_ui = ticks();
            char kb[16]; scpy(m, "Downloading... ", 160); scat(m, utoa(body_len / 1024, kb), 160); scat(m, " KB", 160); set_status(m);
        }
    }
    tcpclose(c);
    body[body_len] = 0;

    /* status line and headers */
    char *hend = find_ci(body, "\r\n\r\n");
    if (body_len < 12 || !starts_ci(body, "http/") || !hend) { set_status("The server's reply was not understood."); return 0; }
    http_code = atoi(body + 9);
    ctype[0] = 0; location[0] = 0;
    int chunked = 0;
    for (char *l = body; l < hend; ) {
        char *e = l; while (e < hend && *e != '\r') e++;
        if (starts_ci(l, "content-type:")) { char *v = l + 13; while (*v == ' ') v++; int i = 0; while (v < e && i < 63) ctype[i++] = lc(*v++); ctype[i] = 0; }
        if (starts_ci(l, "location:")) { char *v = l + 9; while (*v == ' ') v++; int i = 0; while (v < e && i < 511) location[i++] = *v++; location[i] = 0; }
        if (starts_ci(l, "transfer-encoding:") && find_ci(l, "chunked") && find_ci(l, "chunked") < e) chunked = 1;
        l = e + 2;
    }
    char *b = hend + 4;
    int blen = body_len - (b - body);
    if (chunked) {                                             /* undo "chunked" encoding in place */
        char *in = b, *o = b, *end = b + blen;
        while (in < end) {
            int sz = 0;
            while (in < end && ((*in >= '0' && *in <= '9') || (lc(*in) >= 'a' && lc(*in) <= 'f'))) { sz = sz * 16 + (*in <= '9' ? *in - '0' : lc(*in) - 'a' + 10); in++; }
            while (in < end && *in != '\n') in++;
            in++;
            if (sz <= 0 || in + sz > end) { if (sz > 0) { int n = end - in; memcpy(o, in, n); o += n; } break; }
            memcpy(o, in, sz); o += sz; in += sz + 2;
        }
        blen = o - b;
    }
    memcpy(body, b, blen);
    body_len = blen; body[blen] = 0;
    return 1;
}

/* ---------- pages ---------- */
static const char *home_page =
    "<title>Start page</title><h1>Welcome to the MyOS Web browser</h1>"
    "<p>MyOS can now talk to the internet: it gets an address with DHCP, looks names up with DNS, "
    "connects with TCP and downloads pages with HTTP - all written from scratch.</p>"
    "<p>Type an address in the bar above and press Enter. Try one of these:</p><ul>"
    "<li><a href=\"http://info.cern.ch/\">info.cern.ch</a> - the very first website (1991)</li>"
    "<li><a href=\"http://example.com/\">example.com</a> - a small test page</li>"
    "<li><a href=\"http://neverssl.com/\">neverssl.com</a> - a site that never uses encryption</li>"
    "<li><a href=\"http://textfiles.com/\">textfiles.com</a> - old computer text files</li></ul>"
    "<h2>Good to know</h2><ul><li>Only <b>http://</b> sites work. Most sites use https://, which needs "
    "encryption that MyOS does not have yet.</li><li>Pictures (PNG, JPEG, GIF) load after the text; colours and scripts are not shown.</li>"
    "<li>Keys: Ctrl+L address bar, Backspace back, arrows / PgUp / PgDn / Space scroll, Esc stops loading.</li></ul>";

/* ---------- downloading pictures ---------- */
static void *ualloc(uint32_t n) { return malloc(n); }
static void ufree(void *p) { free(p); }
static void clear_images(void) {
    for (int i = 0; i < nimgs; i++) { free(imgs[i].src); if (imgs[i].px) free(imgs[i].px); }
    nimgs = 0; nplaces = 0; img_mem = 0;
}
/* Decode a downloaded picture file into imgs[i] at the size it is shown. 1 = ok. */
static int make_picture(int i, const uint8_t *data, int len) {
    struct image im;
    if (image_decode(data, len, &im, ualloc, ufree)) return 0;
    int w = imgs[i].aw ? imgs[i].aw : im.w, h = imgs[i].aw ? imgs[i].ah : im.h;
    fit_size(&w, &h);
    uint32_t bytes = (uint32_t)w * h * 4;
    uint32_t *px = img_mem + bytes <= IMG_MEM ? malloc(bytes) : 0;
    if (!px) { free(im.px); return 0; }
    image_resample(&im, 0, 0, im.w, im.h, px, w, h);
    free(im.px);
    for (int k = 0; k < w * h; k++) {                          /* see-through parts: the white page */
        uint32_t p = px[k], a = p >> 24;
        if (a == 255) continue;
        uint32_t o = 0;
        for (int sh = 0; sh < 24; sh += 8) o |= ((((p >> sh) & 255) * a + 255 * (255 - a)) / 255) << sh;
        px[k] = o;
    }
    imgs[i].px = px; imgs[i].w = w; imgs[i].h = h; imgs[i].state = 1; img_mem += bytes;
    return 1;
}
static int fetch_picture(int i) {
    char u[512]; scpy(u, imgs[i].src, sizeof u);
    for (int hops = 0; hops < 5; hops++) {
        if (starts_ci(u, "https://") || !http_get(u, MAXIMGDOWN, 1)) return 0;
        if (http_code >= 300 && http_code < 400 && location[0]) { char n[512]; resolve_url(u, location, n, sizeof n); scpy(u, n, sizeof u); continue; }
        return http_code == 200 && make_picture(i, (const uint8_t *)body, body_len);
    }
    return 0;
}
static void load_pictures(void) {
    int todo = 0, done = 0, shown = 0;
    for (int i = 0; i < nimgs; i++) if (!imgs[i].state) todo++;
    if (!todo || !html) return;
    for (int i = 0; i < nimgs; i++) {
        if (imgs[i].state) continue;
        char m[128], nb[12];
        scpy(m, "Loading pictures: ", 128); scat(m, utoa(done + 1, nb), 128); scat(m, " of ", 128); scat(m, utoa(todo, nb), 128);
        scat(m, "   (Esc stops)", 128); set_status(m);
        if (cancelled()) break;
        int had_size = imgs[i].aw != 0;
        if (fetch_picture(i)) shown++; else imgs[i].state = 2;
        done++;
        /* lay the page out again when the picture changed the page; then show it */
        if (!had_size || imgs[i].state == 2) layout_html(html, html_plain);
        int max = nlines - ROWS; if (top_line > max) top_line = max > 0 ? max : 0;
        draw_page();
    }
    for (int i = 0; i < nimgs; i++) if (!imgs[i].state) imgs[i].state = 2;    /* stopped: show their text */
    layout_html(html, html_plain);
    int max = nlines - ROWS; if (top_line > max) top_line = max > 0 ? max : 0;
    draw_page();
    char m[160], nb[16];
    scpy(m, "Done: ", 160); scat(m, utoa(nlinks, nb), 160); scat(m, " links, ", 160);
    scat(m, utoa(shown, nb), 160); scat(m, shown == 1 ? " picture" : " pictures", 160);
    if (shown < todo) { scat(m, " (", 160); scat(m, utoa(todo - shown, nb), 160); scat(m, " could not be shown)", 160); }
    set_status(m);
}

static void set_html(char *h, int plain, int owned) {
    if (html && html_owned) free(html);
    html = h; html_plain = plain; html_owned = owned;
}

static void show_message(const char *t, const char *msg) {
    static char h[700];                                        /* kept, so a resized window can lay it out again */
    scpy(h, "<title>", 700); scat(h, t, 700); scat(h, "</title><h1>", 700); scat(h, t, 700);
    scat(h, "</h1><p>", 700); scat(h, msg, 700); scat(h, "</p><p><a href=\"about:home\">Back to the start page</a></p>", 700);
    set_html(h, 0, 0);
    layout_html(h, 0);
}

static void go(const char *u, int add_history) {
    char full[512];
    while (*u == ' ') u++;
    clear_images();
    top_line = 0;
    if (!*u || starts_ci(u, "about:home")) {
        scpy(url, "about:home", sizeof url);
        set_html((char *)home_page, 0, 0);
        layout_html((char *)home_page, 0);
        set_status("Ready.");
    } else {
        if (!starts_ci(u, "http://") && !starts_ci(u, "https://")) { scpy(full, "http://", 512); scat(full, u, 512); }
        else scpy(full, u, 512);
        scpy(url, full, sizeof url);
        scpy(addr, url, sizeof addr); addr_focus = 0;
        draw_toolbar();
        if (starts_ci(full, "https://")) {
            show_message("Secure site", "This address uses https://, which needs encryption (TLS). MyOS can only open plain http:// pages for now. "
                         "Many sites also answer on http:// - try changing the address.");
            set_status("https is not supported yet.");
        } else {
            int ok = 0;
            for (int hops = 0; hops < 6; hops++) {
                if (!http_get(url, MAXDOWN, 0)) break;
                if (http_code >= 300 && http_code < 400 && location[0]) {      /* redirect */
                    char next[512]; resolve_url(url, location, next, sizeof next);
                    scpy(url, next, sizeof url); scpy(addr, url, sizeof addr); draw_toolbar();
                    if (starts_ci(url, "https://")) {
                        show_message("Secure site", "This site sent us to its https:// address, which needs encryption (TLS). MyOS can only open plain http:// pages for now.");
                        set_status("Redirected to https - not supported yet.");
                        ok = 2; break;
                    }
                    continue;
                }
                ok = 1; break;
            }
            if (ok == 1) {
                int is_html = !ctype[0] || find_ci(ctype, "html") != 0;
                int is_text = find_ci(ctype, "text/") != 0;
                int is_image = find_ci(ctype, "image/") != 0;
                if (is_image) {                                /* a picture on its own: show just the picture */
                    imgs[0].src = sdup(url); imgs[0].state = 0; imgs[0].aw = imgs[0].ah = 0; imgs[0].px = 0; nimgs = 1;
                    int hn = slen(url) * 2 + 64;
                    char *h = malloc(hn);
                    if (imgs[0].src && h && make_picture(0, (const uint8_t *)body, body_len)) {
                        const char *name = url; for (const char *q = url; *q; q++) if (*q == '/' && q[1]) name = q + 1;
                        scpy(h, "<title>", hn); scat(h, name, hn); scat(h, "</title><img src=\"", hn); scat(h, url, hn); scat(h, "\">", hn);
                        set_html(h, 0, 1);
                        layout_html(html, 0);
                    } else {
                        if (h) free(h);
                        show_message("Can't show this picture", "MyOS can show PNG, JPEG, GIF and BMP pictures. This one is in another format, or damaged.");
                    }
                } else if (!is_html && !is_text) {
                    char msg[200]; scpy(msg, "This file is not a web page (it is ", 200); scat(msg, ctype, 200); scat(msg, "), so it can't be shown.", 200);
                    show_message("Can't show this file", msg);
                } else { set_html(body, !is_html, 1); body = 0; layout_html(html, html_plain); }
                char m[160], nb[16];
                scpy(m, "Done: ", 160);
                if (http_code != 200) { scat(m, "HTTP ", 160); scat(m, utoa(http_code, nb), 160); scat(m, ", ", 160); }
                scat(m, utoa(body_len / 1024 + (body_len % 1024 != 0), nb), 160); scat(m, " KB, ", 160);
                scat(m, utoa(nlinks, nb), 160); scat(m, " links", 160);
                set_status(m);
            } else if (!ok) {
                char st[128]; scpy(st, status, 128);
                show_message("Can't reach this page", st);
                set_status(st);
            }
        }
    }
    scpy(addr, url, sizeof addr);
    if (add_history) {
        if (nhist == 16) { for (int i = 1; i < 16; i++) scpy(history[i - 1], history[i], 512); nhist--; }
        scpy(history[nhist++], url, 512);
    }
    char t[128]; scpy(t, title[0] ? title : url, 100); scat(t, " - Web", 128); settitle(t);
    top_line = 0;
    redraw();
    load_pictures();                                           /* the text is up: now fetch the pictures */
}

static void back(void) {
    if (nhist < 2) return;
    nhist--;
    go(history[nhist - 1], 0);
}

static void scroll(int d) {
    int max = nlines - ROWS; if (max < 0) max = 0;
    top_line += d;
    if (top_line > max) top_line = max;
    if (top_line < 0) top_line = 0;
    draw_page();
}

int main(void) {
    settitle("Web");
    resize(W, H);
    page = malloc(MAXLINES * MAXCOLS * sizeof(struct cell));
    resizable(560, 300);
    if (!page) { print("Not enough memory."); getkey(); return 1; }
    unsigned ni[4];
    if (!netinfo(ni)) scpy(status, "Not connected: no network address yet (is the network card enabled?).", sizeof status);
    char arg[256];
    if (getarg(arg, sizeof arg) > 0) go(arg, 1);
    else { go("about:home", 1); addr[0] = 0; addr_focus = 1; draw_toolbar(); if (!netinfo(ni)) set_status(status); }

    struct event e;
    int drag_bar = 0;
    for (;;) {
        if (stashed) { e = stash; stashed = 0; }               /* a click or key that stopped picture loading */
        else getevent(&e);
        if (e.type == EV_RESIZE) {                             /* the user resized the window: lay the page out again */
            W = e.x; H = e.y;
            if (html) layout_html(html, html_plain);
            int max = nlines - ROWS; if (top_line > max) top_line = max > 0 ? max : 0;
            redraw();
            continue;
        }
        if (e.type == EV_KEY) {
            int k = e.key;
            if (k == KEY_CTRL('l')) { addr_focus = 1; addr[0] = 0; draw_toolbar(); continue; }
            if (addr_focus) {
                int n = slen(addr);
                if (k == '\n') { addr_focus = 0; go(addr, 1); }
                else if (k == KEY_ESC) { addr_focus = 0; scpy(addr, url, sizeof addr); draw_toolbar(); }
                else if (k == '\b') { if (n) addr[n - 1] = 0; draw_toolbar(); }
                else if (k >= 32 && k < 127 && n < 500) { addr[n] = (char)k; addr[n + 1] = 0; draw_toolbar(); }
                continue;
            }
            if (k == '\b') back();
            else if (k == KEY_DOWN) scroll(3);
            else if (k == KEY_UP) scroll(-3);
            else if (k == KEY_PGDN || k == ' ') scroll(ROWS - 2);
            else if (k == KEY_PGUP) scroll(-(ROWS - 2));
            else if (k == KEY_HOME) scroll(-1000000);
            else if (k == KEY_END) scroll(1000000);
            else if (k == KEY_ESC) return 0;
        } else if (e.type == EV_MOUSE_DOWN) {
            if (e.y < BAR) {
                if (e.x >= 8 && e.x < 44) back();
                else if (e.x >= 48 && e.x < 84) go(url, 0);
                else if (e.x >= 92) { addr_focus = 1; addr[0] = 0; draw_toolbar(); }
                continue;
            }
            if (addr_focus) { addr_focus = 0; scpy(addr, url, sizeof addr); draw_toolbar(); }
            if (e.x >= W - 16 && nlines > ROWS) { drag_bar = 1; }
            else if (e.y < H - STAT) {
                int row = (e.y - BAR - 6) / LINE_H, col = (e.x - 14) / 8;
                if (row >= 0 && col >= 0 && col < COLS && top_line + row < nlines) {
                    struct cell *cl = page + (top_line + row) * COLS + col;
                    if (cl->c && cl->link >= 0 && cl->link < nlinks) {
                        char next[512]; resolve_url(starts_ci(url, "http") ? url : "http://localhost/", links[cl->link], next, sizeof next);
                        if (starts_ci(links[cl->link], "about:")) scpy(next, links[cl->link], sizeof next);
                        if (starts_ci(next, "mailto:")) set_status("E-mail links are not supported.");
                        else go(next, 1);
                    }
                }
            }
        } else if (e.type == EV_MOUSE_UP) drag_bar = 0;
        if (drag_bar && (e.type == EV_MOUSE_MOVE || e.type == EV_MOUSE_DOWN)) {
            int h = H - BAR - STAT - 4, max = nlines - ROWS;
            int t = (e.y - BAR) * max / (h > 0 ? h : 1);
            top_line = t < 0 ? 0 : t > max ? max : t;
            draw_page();
        }
    }
}
