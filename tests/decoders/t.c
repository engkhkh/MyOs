#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "../myos/image.h"
static void *al(uint32_t n) { return malloc(n); }
static void rl(void *p) { free(p); }
int main(int argc, char **argv) {
    int bad = 0;
    for (int a = 1; a < argc; a++) {
        char exp[512]; snprintf(exp, sizeof exp, "%.*s.exp", (int)(strlen(argv[a]) - 4), argv[a]);
        FILE *f = fopen(argv[a], "rb"); fseek(f, 0, 2); long n = ftell(f); rewind(f);
        unsigned char *d = malloc(n); fread(d, 1, n, f); fclose(f);
        struct image im; const char *e = image_decode(d, n, &im, al, rl);
        if (e) { printf("FAIL %s: %s\n", argv[a], e); bad++; continue; }
        FILE *g = fopen(exp, "rb"); uint32_t *x = malloc(im.w * im.h * 4); fread(x, 4, im.w * im.h, g); fclose(g);
        int diff = 0; for (int i = 0; i < im.w * im.h; i++) if (x[i] != im.px[i]) { if (!diff) printf("MISMATCH %s at %d: got %08x want %08x\n", argv[a], i, im.px[i], x[i]); diff++; }
        if (diff) bad++;
        free(im.px); free(d); free(x);
    }
    printf("%d of %d failed\n", bad, argc - 1);
    return bad != 0;
}
