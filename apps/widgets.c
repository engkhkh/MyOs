/* widgets.c - Widget Gallery: shows every control in the MyOS toolkit (ui.h).
 * A good starting point for your own programs. */
#include "ui.h"

#define W 540
#define H 400

static const char *const menus[] = {
    "File|Reset\tCtrl+R|-|Exit",
    "Help|About Widget Gallery",
};
static const char *const fruits[] = {
    "Apple", "Banana", "Cherry", "Date", "Elderberry", "Fig", "Grape",
    "Honeydew", "Kiwi", "Lemon", "Mango", "Orange", "Papaya", "Pear",
};

static char name[64];
static int bold_on = 1, sound_on, size = 1, sel = -1, scroll, level = 40, about;
static char message[96] = "Type your name and press the button.";

static void reset(void) {
    name[0] = 0; bold_on = 1; sound_on = 0; size = 1; sel = -1; scroll = 0; level = 40;
    const char *m = "Everything was reset.";
    int i = 0; for (; m[i]; i++) message[i] = m[i]; message[i] = 0;
}
static void say(const char *a, const char *b) {
    int n = 0;
    for (int i = 0; a[i] && n < 90; i++) message[n++] = a[i];
    for (int i = 0; b && b[i] && n < 94; i++) message[n++] = b[i];
    message[n] = 0;
}

int main(void) {
    ui_init("Widget Gallery", W, H);
    for (;;) {
        int choice = ui_menubar(menus, 2);
        if (choice == 0) reset();
        if (choice == 1) exit(0);
        if (choice == 100) about = 1;
        if (ui.key == KEY_CTRL('r')) reset();

        if (about) ui_mute();
        ui_fill(0, UI_MENU_H, W, H - UI_MENU_H, UI_BG);

        /* left column: text box, button, check boxes, radio buttons */
        ui_textb(20, 40, "Your name", UI_TEXT);
        int enter = ui_textbox(1, 20, 60, 230, name, sizeof name);
        if (ui_button_primary(2, 20, 96, 110, 30, "Say hello") || enter) {
            if (name[0]) say("Hello, ", name); else say("Please type a name first.", 0);
            if (name[0] && sound_on) tone(880, 80);
        }
        if (ui_button(3, 140, 96, 110, 30, "Clear")) { name[0] = 0; say("Cleared.", 0); }

        ui_textb(20, 146, "Options", UI_TEXT);
        ui_checkbox(4, 20, 168, "Bold greeting", &bold_on);
        if (ui_checkbox(5, 20, 194, "Beep when greeting", &sound_on) && sound_on) tone(660, 60);

        ui_textb(20, 232, "Text size", UI_TEXT);
        ui_radio(6, 20, 254, "Small", &size, 1);
        ui_radio(7, 100, 254, "Large", &size, 2);
        ui_radio(8, 180, 254, "Huge", &size, 3);

        /* right column: list */
        ui_textb(290, 40, "Pick a fruit", UI_TEXT);
        int r = ui_list(9, 290, 60, 230, 188, fruits, 14, &sel, &scroll);
        if (r == 1) say("You picked ", fruits[sel]);
        if (r == 2) say("Yum! A double-click on ", fruits[sel]);

        /* progress bar with - / + buttons */
        ui_textb(290, 262, "Level", UI_TEXT);
        if (ui_button(10, 350, 256, 30, 26, "-") && level > 0) level -= 10;
        if (ui_button(11, 384, 256, 30, 26, "+") && level < 100) level += 10;
        ui_progress(424, 265, 96, 10, level, 100);

        /* the message line */
        ui_fill(0, 296, W, 1, 0xE5E7EB);
        ui_rfill(20, 308, W - 40, 72, 8, UI_WHITE);
        if (size == 1) { if (bold_on) ui_textb(36, 336, message, UI_TEXT); else ui_text(36, 336, message, UI_TEXT); }
        else {
            char t[40]; int i = 0; for (; message[i] && i < (size == 2 ? 30 : 19); i++) t[i] = message[i]; t[i] = 0;
            ui_text_big(36, size == 2 ? 328 : 320, t, bold_on ? UI_ACCENT : UI_TEXT, size);
        }
        ui_menubar_draw();
        if (about) {
            ui_unmute();
            if (ui_dialog("About Widget Gallery", "Every control here comes from apps/ui.c,", "the MyOS widget toolkit.", "OK", 0) >= 0) about = 0;
        }
        ui_present();
        ui_next();
    }
}
