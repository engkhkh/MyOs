/* crash.c - deliberately misbehaves, to show that MyOS protects itself.
 * Each choice makes the CPU raise an exception. Because this program runs in
 * user mode with its own memory, MyOS just closes it and keeps running. */
#include "myos.h"

int main(void) {
    settitle("Crash test");
    clear(0x450A0A);
    setcolor(0xFECACA, 0x450A0A);
    print("This program will break the rules on purpose.\n");
    print("MyOS should close only this program, nothing else.\n\n");
    setcolor(0xFFFFFF, 0x450A0A);
    print("  1  Write to address 0 (a null pointer)\n");
    print("  2  Overwrite the kernel's memory\n");
    print("  3  Divide by zero\n");
    print("  4  Run a privileged instruction (cli)\n");
    print("  5  Read memory outside my space\n");
    print("  Q  Quit politely\n\n");
    setcolor(0xFECACA, 0x450A0A);
    print("Choose: ");
    for (;;) {
        int k = getkey();
        volatile int zero = 0;
        switch (k) {
        case '1': print("1\nWriting to 0x00000000...\n"); *(volatile int *)0 = 42; break;
        case '2': print("2\nWriting to the kernel at 0x00100000...\n"); *(volatile int *)0x100000 = 42; break;
        case '3': print("3\nDividing by zero...\n"); printnum(100 / zero); break;
        case '4': print("4\nDisabling interrupts...\n"); __asm__ volatile("cli"); break;
        case '5': print("5\nReading 0x90000000...\n"); printnum(*(volatile int *)0x90000000); break;
        case 'q': case 'Q': return 0;
        }
    }
}
