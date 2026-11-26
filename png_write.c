/* png_write.c - turn the screen into a PNG file (for screenshots).
 *
 * PNG = an 8-byte signature + chunks (IHDR size/colour type, IDAT zlib data, IEND).
 * Each row of RGB pixels is stored with the "Sub" filter (every byte minus the byte
 * one pixel to the left), so the flat areas of a desktop turn into long runs of
 * zeros. The zlib stream uses DEFLATE with the fixed Huffman codes and only one kind
 * of back-reference - "repeat the last 3 bytes" or "repeat the last byte" - which is
 * enough to squeeze those runs down. Not as small as a real compressor, but simple,
 * and any picture program (and MyOS's own decoder) can open the result. */
#include "kernel.h"

static uint32_t crc_table[256];
static void crc_init(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}
static uint32_t crc(const uint8_t *p, int n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ---- bit writer (DEFLATE sends bits least-significant first) ---- */
static uint8_t *out; static int pos, cap; static uint32_t bitbuf; static int bitcnt;
static void put_bits(uint32_t v, int n) {
    bitbuf |= v << bitcnt; bitcnt += n;
    while (bitcnt >= 8) { if (pos < cap) out[pos] = (uint8_t)bitbuf; pos++; bitbuf >>= 8; bitcnt -= 8; }
}
static void put_rev(uint32_t code, int n) {              /* Huffman codes go most-significant first */
    uint32_t r = 0; for (int i = 0; i < n; i++) r |= ((code >> i) & 1) << (n - 1 - i);
    put_bits(r, n);
}
static void literal(int v) {                              /* fixed Huffman literal/length codes */
    if (v < 144) put_rev(0x30 + v, 8);
    else if (v < 256) put_rev(0x190 + v - 144, 9);
    else if (v < 280) put_rev(v - 256, 7);
    else put_rev(0xC0 + v - 280, 8);
}
static const int len_base[] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const int len_extra[] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static void match(int len, int dist) {                    /* dist is 1 or 3 here */
    int i = 28; while (len_base[i] > len) i--;
    literal(257 + i);
    if (len_extra[i]) put_bits(len - len_base[i], len_extra[i]);
    put_rev(dist == 1 ? 0 : 2, 5);                       /* distance codes 0 (=1) and 2 (=3), no extra bits */
}

uint8_t *png_encode(const uint32_t *px, int w, int h, int *len) {
    crc_init();
    int raw = (w * 3 + 1) * h;
    uint8_t *data = kmalloc(raw);
    if (!data) return 0;
    for (int y = 0; y < h; y++) {                         /* filter "Sub" (1) on every row */
        uint8_t *r = data + y * (w * 3 + 1);
        r[0] = 1;
        const uint32_t *p = px + y * w;
        for (int x = 0; x < w; x++) {
            uint32_t c = p[x], l = x ? p[x - 1] : 0;
            r[1 + x * 3 + 0] = (uint8_t)((c >> 16) - (l >> 16));
            r[1 + x * 3 + 1] = (uint8_t)((c >> 8) - (l >> 8));
            r[1 + x * 3 + 2] = (uint8_t)(c - l);
        }
    }
    cap = raw + raw / 8 + 1024;                           /* worst case: 9 bits per byte */
    out = kmalloc(cap + 64);
    if (!out) { kfree(data); return 0; }
    static const uint8_t sig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    for (int i = 0; i < 8; i++) out[i] = sig[i];
    pos = 8;
    /* IHDR */
    uint8_t ihdr[25] = { 0, 0, 0, 13, 'I', 'H', 'D', 'R',
                         (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                         (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h,
                         8, 2, 0, 0, 0 };                     /* 8 bits, RGB, deflate, no interlace */
    uint32_t c = crc(ihdr + 4, 17);
    ihdr[21] = c >> 24; ihdr[22] = c >> 16; ihdr[23] = c >> 8; ihdr[24] = c;
    for (int i = 0; i < 25; i++) out[pos++] = ihdr[i];
    /* IDAT: zlib header, one fixed-Huffman block, Adler-32 */
    int idat = pos; pos += 8;                             /* length + "IDAT" filled in later */
    out[pos++] = 0x78; out[pos++] = 0x01;
    bitbuf = 0; bitcnt = 0;
    put_bits(1, 1); put_bits(1, 2);                       /* final block, fixed Huffman */
    for (int i = 0; i < raw; ) {
        int run1 = 0, run3 = 0;                           /* how far "the same as 1 / 3 bytes back" goes */
        if (i >= 1) while (i + run1 < raw && run1 < 258 && data[i + run1] == data[i + run1 - 1]) run1++;
        if (i >= 3) while (i + run3 < raw && run3 < 258 && data[i + run3] == data[i + run3 - 3]) run3++;
        if (run3 >= 3 && run3 >= run1) { match(run3, 3); i += run3; }
        else if (run1 >= 3) { match(run1, 1); i += run1; }
        else literal(data[i++]);
    }
    literal(256);                                          /* end of block */
    if (bitcnt) put_bits(0, 8 - bitcnt);
    uint32_t a = 1, b = 0;
    for (int i = 0; i < raw; i++) { a = (a + data[i]) % 65521; b = (b + a) % 65521; }
    uint32_t ad = (b << 16) | a;
    out[pos++] = ad >> 24; out[pos++] = ad >> 16; out[pos++] = ad >> 8; out[pos++] = ad;
    kfree(data);
    if (pos > cap) { kfree(out); return 0; }
    int n = pos - idat - 8;
    out[idat] = n >> 24; out[idat + 1] = n >> 16; out[idat + 2] = n >> 8; out[idat + 3] = n;
    out[idat + 4] = 'I'; out[idat + 5] = 'D'; out[idat + 6] = 'A'; out[idat + 7] = 'T';
    c = crc(out + idat + 4, n + 4);
    out[pos++] = c >> 24; out[pos++] = c >> 16; out[pos++] = c >> 8; out[pos++] = c;
    static const uint8_t iend[12] = { 0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82 };
    for (int i = 0; i < 12; i++) out[pos++] = iend[i];
    *len = pos;
    return out;
}
