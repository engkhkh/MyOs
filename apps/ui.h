/* ui.h - the MyOS widget toolkit for programs.
 *
 * Programs draw each frame into their own pixel buffer and show it with one
 * ui_present() call, so nothing flickers. Widgets are "immediate mode": call
 * them every frame while drawing, and they return 1 when they were used.
 *
 *     ui_init("My app", 400, 300);
 *     for (;;) {
 *         ui_next();                                  // wait for mouse / keyboard
 *         int m = ui_menubar(menus, 2);               // menus first (they take clicks)
 *         ui_fill(0, UI_MENU_H, 400, 276, UI_BG);
 *         if (ui_button(1, 20, 40, 90, 28, "Hello")) ...;
 *         ui_menubar_draw();                          // menus last (drawn on top)
 *         ui_present();
 *     }
 */
#ifndef UI_H
#define UI_H
#include "myos.h"

#define UI_BG      0xF3F4F6
#define UI_WHITE   0xFFFFFF
#define UI_TEXT    0x111827
#define UI_MUTED   0x6B7280
#define UI_BORDER  0xD1D5DB
#define UI_ACCENT  0x2563EB
#define UI_ACCENT_LIGHT 0xDBEAFE
#define UI_MENU_H  24

struct ui_state {
    uint32_t *px; int w, h;          /* the frame being drawn */
    int mx, my, down;                /* mouse position, left button held */
    int pressed, released;           /* left button went down / up this frame */
    int rpressed;                    /* right button went down this frame */
    int dbl;                         /* this press was a double-click */
    int moved;                       /* the mouse moved this frame */
    int key;                         /* key pressed this frame (0 = none); widgets with focus take it */
    int active, focus;               /* widget being pressed / widget with keyboard focus */
    int timeout;                     /* ui_wait() returned because time ran out */
    int resized;                     /* the user resized the window this frame: lay out again for ui.w x ui.h */
};
extern struct ui_state ui;

/* setup and frames */
void ui_init(const char *title, int w, int h);
void ui_resize(int w, int h);
void ui_resizable(int min_w, int min_h);   /* let the user resize / maximise the window (see ui.resized) */
void ui_next(void);                  /* wait for input */
void ui_wait(int ms);                /* wait for input, at most ms milliseconds (ui.timeout = 1 if none) */
void ui_present(void);               /* show the whole frame */
void ui_redraw(void);                /* make the next ui_wait() return at once (e.g. after a change) */
void ui_present_rect(int x, int y, int w, int h);

/* drawing */
void ui_clip(int x, int y, int w, int h);
void ui_noclip(void);
void ui_pixel(int x, int y, uint32_t c);
void ui_fill(int x, int y, int w, int h, uint32_t c);
void ui_rfill(int x, int y, int w, int h, int r, uint32_t c);      /* rounded corners */
void ui_frame(int x, int y, int w, int h, uint32_t c);             /* 1-pixel outline */
void ui_rframe(int x, int y, int w, int h, int r, uint32_t c);     /* rounded outline */
void ui_line(int x0, int y0, int x1, int y1, int thick, uint32_t c);
void ui_vgradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom);
int  ui_text(int x, int y, const char *s, uint32_t c);             /* returns width in pixels */
int  ui_textb(int x, int y, const char *s, uint32_t c);            /* bold */
void ui_text_big(int x, int y, const char *s, uint32_t c, int scale);
void ui_text_center(int x, int y, int w, const char *s, uint32_t c, int bold);
int  ui_text_w(const char *s);
uint32_t ui_mix(uint32_t a, uint32_t b, int t);                    /* blend a->b, t = 0..256 */

/* input helpers */
int ui_hit(int x, int y, int w, int h);                            /* is the mouse inside? */
int ui_clicked(int x, int y, int w, int h);                        /* pressed inside this frame */

/* widgets (id = any unique number > 0) */
void ui_label(int x, int y, const char *s);
int  ui_button(int id, int x, int y, int w, int h, const char *label);
int  ui_button_primary(int id, int x, int y, int w, int h, const char *label);
int  ui_checkbox(int id, int x, int y, const char *label, int *value);
int  ui_radio(int id, int x, int y, const char *label, int *value, int mine);
int  ui_textbox(int id, int x, int y, int w, char *buf, int max);   /* 1 when Enter is pressed */
/* Multi-line text: word wrap, caret, Shift-less selection by mouse drag, Ctrl+A/C/X/V,
 * arrows, Home/End, PgUp/PgDn, scrolling. Keep one struct ui_textarea per text area.
 * Returns 1 when the text changed this frame. */
struct ui_textarea { int caret, anchor, scroll, dragging; };
int  ui_textarea(int id, int x, int y, int w, int h, char *buf, int max, struct ui_textarea *st);
void ui_textarea_reset(struct ui_textarea *st);              /* caret to the start, nothing selected */
int  ui_list(int id, int x, int y, int w, int h, const char *const *items, int n, int *sel, int *scroll);
                                                                    /* 1 = selection changed, 2 = double-click / Enter */
void ui_progress(int x, int y, int w, int h, int value, int max);

/* menu bar: each menu is "Title|Item|Item|-|Item"; "-" is a separator,
 * a leading '*' shows a tick, a leading '!' greys the item out.
 * ui_menubar() returns menu * 100 + item (both from 0; separators are not counted)
 * when an item is chosen, else -1. Text after a tab is shown as the shortcut. */
int  ui_menubar(const char *const *menus, int n);
void ui_menubar_draw(void);
int  ui_menu_open(void);

/* While a dialog is showing, draw the rest of the window between ui_mute() and
 * ui_unmute() so it ignores the mouse and keyboard, then call ui_dialog(). */
void ui_mute(void);
void ui_unmute(void);
/* a centred message box drawn over the frame; returns the button clicked (0, 1..) or -1 */
int  ui_dialog(const char *title, const char *line1, const char *line2, const char *btn1, const char *btn2);
#endif
