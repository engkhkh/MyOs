/* calc.c - Calculator, in the style of the Windows "Standard" calculator.
 * Mouse or keyboard: digits, . + - * / = (or Enter), Backspace, Esc = C,
 * Del = CE, % . Uses real floating-point numbers (double), so the kernel saves
 * each program's FPU registers when it switches tasks. */
#include "myos.h"

static void strcpy_(char *d, const char *s);
static int strcmp_(const char *a, const char *b);

#define W 360
#define H 470
#define DISP_H 110
#define BW 81
#define BH 52
#define GAP 6

static char entry[32] = "0";          /* what is being typed */
static char hist[64];                 /* small line above: "12 + 7 =" */
static char shown[40];                /* what the display shows */
static double acc, last_operand;
static char op, last_op;              /* pending operator */
static int new_entry = 1, error, hover = -1;

static const char *labels[24] = {
    "%", "CE", "C", "<-",
    "1/x", "x^2", "sqrt", "/",
    "7", "8", "9", "*",
    "4", "5", "6", "-",
    "1", "2", "3", "+",
    "+/-", "0", ".", "=" };

/* ---------- numbers <-> text ---------- */
static double fsqrt(double x) { __asm__("fsqrt" : "+t"(x)); return x; }

static double parse(const char *s) {
    double v = 0, scale = 1; int neg = 0, frac = 0;
    if (*s == '-') { neg = 1; s++; }
    for (; *s; s++) {
        if (*s == '.') { frac = 1; continue; }
        if (*s < '0' || *s > '9') break;
        if (frac) { scale /= 10; v += (*s - '0') * scale; } else v = v * 10 + (*s - '0');
    }
    return neg ? -v : v;
}

/* Up to 15 significant digits; scientific notation for huge or tiny numbers. */
static void format(double x, char *out) {
    if (x != x) { strcpy_(out, "Invalid input"); return; }
    if (x - x != 0) { strcpy_(out, "Overflow"); return; }  /* infinity */
    char *o = out;
    if (x < 0) { *o++ = '-'; x = -x; }
    if (x == 0) { *o++ = '0'; *o = 0; return; }
    int e = 0;
    while (x >= 10) { x /= 10; e++; }
    while (x < 1) { x *= 10; e--; }
    char d[17];
    for (int i = 0; i < 16; i++) { int k = (int)x; if (k > 9) k = 9; d[i] = (char)('0' + k); x = (x - k) * 10; }
    if (d[15] >= '5') {                                    /* round to 15 digits */
        int i = 14;
        while (i >= 0 && d[i] == '9') d[i--] = '0';
        if (i >= 0) d[i]++; else { for (int k = 14; k > 0; k--) d[k] = d[k - 1]; d[0] = '1'; e++; }
    }
    int n = 15; while (n > 1 && d[n - 1] == '0') n--;      /* drop trailing zeros */
    if (e >= 15 || e < -7) {                               /* 1.2345e+20 */
        *o++ = d[0];
        if (n > 1) { *o++ = '.'; for (int i = 1; i < n && i < 10; i++) *o++ = d[i]; }
        *o++ = 'e'; *o++ = e < 0 ? '-' : '+';
        char t[8]; utoa(e < 0 ? -e : e, t); for (char *q = t; *q; q++) *o++ = *q;
    } else if (e >= 0) {                                   /* 1234.5678 */
        for (int i = 0; i <= e; i++) *o++ = i < n ? d[i] : '0';
        if (n > e + 1) { *o++ = '.'; for (int i = e + 1; i < n; i++) *o++ = d[i]; }
    } else {                                               /* 0.000123 */
        *o++ = '0'; *o++ = '.';
        for (int i = 0; i < -e - 1; i++) *o++ = '0';
        for (int i = 0; i < n && (o - out) < 30; i++) *o++ = d[i];
    }
    *o = 0;
}
static void strcpy_(char *d, const char *s) { while ((*d++ = *s++)) {} }

/* ---------- drawing ---------- */
static void button_rect(int i, int *x, int *y) { *x = 8 + (i % 4) * (BW + GAP); *y = DISP_H + 8 + (i / 4) * (BH + GAP); }

static void draw_button(int i) {
    int x, y; button_rect(i, &x, &y);
    int is_num = (labels[i][0] >= '0' && labels[i][0] <= '9' && !labels[i][1]) || i == 20 || i == 22;
    uint32_t bg = i == 23 ? (hover == i ? 0x1D4ED8 : 0x2563EB) : is_num ? (hover == i ? 0xF3F4F6 : 0xFFFFFF) : (hover == i ? 0xE5E7EB : 0xF3F4F6);
    fill(x, y, BW, BH, 0xE5E7EB);
    fill(x + 1, y + 1, BW - 2, BH - 2, bg);
    int len = strlen(labels[i]);
    uint32_t fg = i == 23 ? 0xFFFFFF : 0x111827;
    if (is_num || i == 23 || (i % 4 == 3 && i >= 7) || i == 3) bigtext(x + (BW - len * 16) / 2, y + (BH - 32) / 2, labels[i], fg, 2);
    else text(x + (BW - len * 8) / 2, y + (BH - 16) / 2, labels[i], fg);
}

static void draw_display(void) {
    fill(0, 0, W, DISP_H, 0xF9FAFB);
    text(W - 16 - strlen(hist) * 8, 14, hist, 0x6B7280);
    const char *s = error ? shown : (new_entry ? shown : entry);
    int len = strlen(s), scale = len <= 13 ? 3 : 2;
    if (len * 8 * scale > W - 24) scale = 1;
    bigtext(W - 16 - len * 8 * scale, DISP_H - 18 - 16 * scale, s, 0x111827, scale);
}

static void draw_all(void) {
    fill(0, 0, W, H, 0xE5E7EB);
    draw_display();
    for (int i = 0; i < 24; i++) draw_button(i);
}

/* ---------- logic ---------- */
static double current(void) { return new_entry ? parse(shown) : parse(entry); }

static double apply(double a, char o, double b) {
    switch (o) {
    case '+': return a + b;
    case '-': return a - b;
    case '*': return a * b;
    case '/': if (b == 0) { error = 1; strcpy_(shown, "Cannot divide by zero"); return 0; } return a / b;
    }
    return b;
}

static void show_value(double v) { format(v, shown); new_entry = 1; }
static void reset(void) { strcpy_(entry, "0"); strcpy_(shown, "0"); hist[0] = 0; acc = 0; op = 0; last_op = 0; new_entry = 1; error = 0; }

static void hist_set(double a, char o, const char *tail) {
    char n[40]; format(a, n);
    strcpy_(hist, n);
    int k = strlen(hist); hist[k++] = ' '; hist[k++] = o; hist[k] = 0;
    if (tail) { hist[k++] = ' '; strcpy_(hist + k, tail); }
}

static void press(const char *l) {
    if (error && strcmp_(l, "C") && strcmp_(l, "CE") && !(l[0] >= '0' && l[0] <= '9')) return;
    if (error) reset();
    char c = l[0];
    if (!l[1] && c >= '0' && c <= '9') {
        if (new_entry) { entry[0] = c; entry[1] = 0; new_entry = 0; }
        else if (strlen(entry) < 16) {
            if (!strcmp_(entry, "0")) entry[0] = c;
            else if (!strcmp_(entry, "-0")) entry[1] = c;
            else { int k = strlen(entry); entry[k] = c; entry[k + 1] = 0; }
        }
    } else if (!strcmp_(l, ".")) {
        if (new_entry) { strcpy_(entry, "0."); new_entry = 0; }
        else { int dot = 0; for (char *q = entry; *q; q++) if (*q == '.') dot = 1; if (!dot) { int k = strlen(entry); entry[k] = '.'; entry[k + 1] = 0; } }
    } else if (!l[1] && (c == '+' || c == '-' || c == '*' || c == '/')) {
        double v = current();
        if (op && !new_entry) { acc = apply(acc, op, v); if (error) return; }
        else if (!op) acc = v;
        op = c;
        show_value(acc);
        hist_set(acc, op, 0);
    } else if (!strcmp_(l, "=")) {
        if (op) {
            double v = current();
            last_op = op; last_operand = v;
            char b[40]; format(v, b);
            int k = strlen(b); b[k] = ' '; b[k + 1] = '='; b[k + 2] = 0;
            hist_set(acc, op, b);
            acc = apply(acc, op, v);
            op = 0;
            if (!error) show_value(acc);
        } else if (last_op) {                               /* "=" again repeats the last step */
            double v = current();
            char b[40]; format(last_operand, b);
            int k = strlen(b); b[k] = ' '; b[k + 1] = '='; b[k + 2] = 0;
            hist_set(v, last_op, b);
            acc = apply(v, last_op, last_operand);
            if (!error) show_value(acc);
        }
    } else if (!strcmp_(l, "C")) reset();
    else if (!strcmp_(l, "CE")) { strcpy_(entry, "0"); new_entry = 0; }
    else if (!strcmp_(l, "<-")) {
        if (!new_entry) { int k = strlen(entry); if (k > 1 && !(k == 2 && entry[0] == '-')) entry[k - 1] = 0; else strcpy_(entry, "0"); }
    } else if (!strcmp_(l, "+/-")) {
        if (new_entry) { double v = -parse(shown); format(v, entry); new_entry = 0; }
        else if (entry[0] == '-') strcpy_(entry, entry + 1);
        else if (strcmp_(entry, "0")) { for (int k = strlen(entry); k >= 0; k--) entry[k + 1] = entry[k]; entry[0] = '-'; }
    } else {                                                 /* one-number functions */
        double v = current(), r = v;
        if (!strcmp_(l, "%")) r = op ? acc * v / 100 : 0;
        else if (!strcmp_(l, "1/x")) { if (v == 0) { error = 1; strcpy_(shown, "Cannot divide by zero"); return; } r = 1 / v; }
        else if (!strcmp_(l, "x^2")) r = v * v;
        else if (!strcmp_(l, "sqrt")) { if (v < 0) { error = 1; strcpy_(shown, "Invalid input"); return; } r = fsqrt(v); }
        format(r, entry); new_entry = 0;
        if (!strcmp_(l, "x^2") || !strcmp_(l, "sqrt") || !strcmp_(l, "1/x")) { show_value(r); }
    }
}
static int strcmp_(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }

static int button_at(int mx, int my) {
    for (int i = 0; i < 24; i++) { int x, y; button_rect(i, &x, &y); if (mx >= x && mx < x + BW && my >= y && my < y + BH) return i; }
    return -1;
}

int main(void) {
    settitle("Calculator");
    resize(W, H);
    reset();
    draw_all();
    struct event e;
    for (;;) {
        getevent(&e);
        const char *l = 0;
        if (e.type == EV_KEY) {
            static char one[2];
            int k = e.key;
            if ((k >= '0' && k <= '9') || k == '.' || k == '+' || k == '-' || k == '*' || k == '/') { one[0] = (char)k; one[1] = 0; l = one; }
            else if (k == ',') l = ".";
            else if (k == '\n' || k == '=') l = "=";
            else if (k == '\b') l = "<-";
            else if (k == KEY_ESC) l = "C";
            else if (k == KEY_DEL) l = "CE";
            else if (k == '%') l = "%";
            else if (k == 'r' || k == 'R') l = "1/x";
            else if (k == 'q' || k == 'Q') l = "x^2";
            else if (k == '@') l = "sqrt";
            else if (k == KEY_CTRL('c')) {                   /* copy the number on the display */
                const char *t = error ? shown : (new_entry ? shown : entry);
                clipset(t, strlen(t));
            } else if (k == KEY_CTRL('v')) {                 /* paste a number from the clipboard */
                char buf[40]; int n = clipget(buf, 39), ok = n > 0 && n < 39, dot = 0, digits = 0;
                if (ok) { buf[n] = 0; while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' ' || buf[n - 1] == '\r')) buf[--n] = 0; }
                for (int i = 0; ok && buf[i]; i++) {
                    if (buf[i] == '.' && !dot) dot = 1;
                    else if (buf[i] == '-' && i == 0) {}
                    else if (buf[i] >= '0' && buf[i] <= '9') digits++;
                    else ok = 0;
                }
                if (ok && digits && digits <= 16) {
                    if (error) reset();
                    strcpy_(entry, buf); new_entry = 0;
                    draw_display();
                }
            }
        } else if (e.type == EV_MOUSE_MOVE) {
            int h = button_at(e.x, e.y);
            if (h != hover) { int old = hover; hover = h; if (old >= 0) draw_button(old); if (h >= 0) draw_button(h); }
        } else if (e.type == EV_MOUSE_DOWN && (e.buttons & 1)) {
            int b = button_at(e.x, e.y);
            if (b >= 0) l = labels[b];
        }
        if (l) { press(l); draw_display(); }
    }
}
