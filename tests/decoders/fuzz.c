#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../myos/image.h"
static void *al(uint32_t n) { return malloc(n); }
static void rl(void *p) { free(p); }
int main(int argc, char **argv) {
    srand(42); int ok = 0, err = 0;
    for (int a = 1; a < argc; a++) {
        FILE *f = fopen(argv[a], "rb"); fseek(f, 0, 2); long n = ftell(f); rewind(f);
        unsigned char *d = malloc(n); if (fread(d, 1, n, f) != (size_t)n) return 1; fclose(f);
        for (int t = 0; t < 300; t++) {
            long m = t % 3 == 0 ? rand() % (n + 1) : n;
            unsigned char *c = malloc(m ? m : 1); memcpy(c, d, m);
            if (t % 3) for (int k = 0; k < 1 + rand() % 6; k++) c[8 + rand() % (m - 8)] ^= 1 << (rand() % 8);
            struct image im; const char *e = image_decode(c, m, &im, al, rl);
            if (e) err++; else { ok++; free(im.px); }
            free(c);
        }
        free(d);
    }
    printf("fuzz done: %d decoded, %d rejected, no crash\n", ok, err);
    return 0;
}
