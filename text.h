/* text.h - UTF-8, Arabic shaping and right-to-left order (text.c), for the kernel and programs */
#ifndef TEXT_H
#define TEXT_H

#define TEXT_MAX 512                                   /* the longest line laid out at once (characters) */
struct tcell { unsigned short cp, at; };                /* what to draw in a cell, and the byte it came from */

int  utf8_decode(const char *s, int n, int *i);          /* the character at *i (advances *i) */
int  utf8_encode(int cp, char *out);                     /* returns the number of bytes (1..3) */
int  utf8_next(const char *s, int n, int i);
int  utf8_prev(const char *s, int i);
int  utf8_count(const char *s, int a, int b);            /* characters in s[a..b) */
int  utf8_skip(const char *s, int n, int a, int count);  /* byte position count characters after a */
int  text_is_rtl_char(int c);
int  text_has_rtl(const char *s, int n);
int  text_cells(const char *s);                          /* cells a nul-terminated string takes on screen */
/* lay out s[0..n) for display: cells left to right; *rtl = 1 for a right-to-left paragraph */
int  text_layout(const char *s, int n, struct tcell *out, int max, int *rtl);   /* the interface's direction: LTR */
/* the same for text being written or read (Notepad, text boxes): an Arabic paragraph runs right to left */
int  text_layout_para(const char *s, int n, struct tcell *out, int max, int *rtl);
int  text_caret_cell(const struct tcell *cells, int nc, int rtl, int pos, int n);
int  text_pos_at_cell(const struct tcell *cells, int nc, int rtl, int col, int n);
/* the 8x16 bitmap for an Arabic cell (presentation forms, digits, ، ؛ ؟), or 0 */
const unsigned char *text_arabic_glyph(int cp, int bold);
#endif
