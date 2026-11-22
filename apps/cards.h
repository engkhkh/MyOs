/* cards.h - playing cards for MyOS card games (drawn with the ui.h toolkit).
 * A card is a byte: suit * 13 + rank (rank 0 = Ace ... 12 = King,
 * suits 0 spades, 1 hearts, 2 diamonds, 3 clubs), plus CARD_UP when face up. */
#ifndef CARDS_H
#define CARDS_H
#include "ui.h"
#define CW 72                       /* card size in pixels */
#define CH 100
#define CARD_UP 0x80
#define FELT 0x17703C
#define FELT_DARK 0x0F5A2F
static inline int card_suit(int c) { return (c & 0x7F) / 13; }
static inline int card_rank(int c) { return (c & 0x7F) % 13; }
static inline int card_red(int c) { int s = card_suit(c); return s == 1 || s == 2; }
void cards_init(void);                              /* draw all 52 faces + the back once */
void card_draw(int x, int y, int c);                /* face or back, by CARD_UP */
void card_slot(int x, int y, const char *label);    /* an empty place on the table */
int  card_bounce(int c, int x, int y, int floor_y);    /* win animation for one card; 1 = skipped */
#endif
