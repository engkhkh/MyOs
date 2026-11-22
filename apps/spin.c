/* spin.c - a "badly behaved" program that never pauses or yields.
 * On a system without preemptive multitasking this would freeze everything.
 * On MyOS the timer interrupt still switches to other programs 100 times a
 * second, so the desktop keeps working. Press Q to quit (checked rarely). */
#include "myos.h"

int main(void) {
    settitle("Busy loop (CPU hog)");
    clear(0x1C1917);
    setcolor(0xFDBA74, 0x1C1917);
    print("This program counts as fast as it can, forever,\n");
    print("and never gives up the processor voluntarily.\n\n");
    setcolor(0xD6D3D1, 0x1C1917);
    print("Yet you can still move windows, type and run\n");
    print("other programs: the scheduler takes the CPU away\n");
    print("every 10 ms (preemptive multitasking).\n\n");
    print("Open Task Manager to see it using the CPU.\n");
    const char spinner[4] = { '|', '/', '-', '\\' };
    uint32_t n = 0, round = 0;
    char buf[48];
    for (;;) {
        n++;
        if ((n & 0x3FFFFF) == 0) {                          /* every ~4 million counts */
            round++;
            fill(0, 230, 480, 60, 0x292524);
            char *p = buf; const char *a = "Count: "; while (*a) *p++ = *a++;
            p = utoa(n, p); while (*p) p++;
            *p++ = ' '; *p++ = ' '; *p++ = spinner[round & 3]; *p = 0;
            text(16, 240, buf, 0xFDE68A);
            text(16, 262, "Press Q to stop", 0xA8A29E);
            int k = pollkey();
            if (k == 'q' || k == 'Q') return 0;
        }
    }
}
