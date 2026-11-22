/* guess.c - guess the number game: shows keyboard input in a program. */
#include "myos.h"

int main(void) {
    settitle("Guess the number");
    srand(ticks() * 2654435761u);
    for (;;) {
        clear(0x172554);
        setcolor(0xFDE047, 0x172554);
        print("=== Guess the number ===\n\n");
        setcolor(0xE0E7FF, 0x172554);
        print("I'm thinking of a number from 1 to 100.\n\n");
        int secret = rand() % 100 + 1, tries = 0;
        char line[16];
        for (;;) {
            print("Your guess: ");
            readline(line, sizeof line);
            int g = atoi(line);
            if (g < 1 || g > 100) { print("  Please type a number from 1 to 100.\n"); continue; }
            tries++;
            if (g < secret) print("  Higher!\n");
            else if (g > secret) print("  Lower!\n");
            else break;
        }
        setcolor(0x86EFAC, 0x172554);
        print("\n  Correct! You got it in "); printnum(tries); print(tries == 1 ? " try.\n" : " tries.\n\n");
        setcolor(0xE0E7FF, 0x172554);
        print("Play again? (y/n) ");
        int k = getkey();
        if (k != 'y' && k != 'Y') return 0;
    }
}
