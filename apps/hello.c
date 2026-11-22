/* hello.c - a first user program: prints, reads a file, waits for a key. */
#include "myos.h"

static void two(int v) { char b[3] = { '0' + v / 10 % 10, '0' + v % 10, 0 }; print(b); }
static char buf[1024];

int main(void) {
    settitle("Hello");
    setcolor(0x4ADE80, 0x111827);
    print("Hello from a user program!\n\n");
    setcolor(0xD4D4D4, 0x111827);
    print("I am process #"); printnum(getpid());
    print(", running in user mode (ring 3)\nin my own private memory at 0x40000000.\n\n");
    int t[6]; gettime(t);
    print("The time is "); two(t[0]); print(":"); two(t[1]);
    print(" on "); two(t[3]); print("/"); two(t[4]); print("/"); printnum(t[5]); print(".\n\n");

    print("Reading C:\\Welcome.txt with a system call:\n");
    setcolor(0x93C5FD, 0x111827);
    int n = readfile("/Welcome.txt", buf, sizeof buf - 1);
    if (n < 0) print("  (file not found)\n");
    else {
        buf[n] = 0;
        int lines = 0;
        for (int i = 0; i < n && lines < 5; i++) {
            char s[2] = { buf[i], 0 };
            if (i == 0 || buf[i - 1] == '\n') print("  | ");
            print(s);
            if (buf[i] == '\n') lines++;
        }
    }
    setcolor(0xFACC15, 0x111827);
    print("\nPress any key to close this program.");
    getkey();
    return 0;
}
