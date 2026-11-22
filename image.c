/* image.c - PNG and BMP decoding for MyOS (see image.h).
 *
 * PNG: all colour types (grey, RGB, palette, grey+alpha, RGBA), bit depths 1-16,
 * transparency (tRNS) and interlaced (Adam7) pictures. The compressed data is
 * unpacked by a small "inflate" (the zlib / DEFLATE format, RFC 1951).
 * BMP: uncompressed 24 and 32 bit, and 8 bit with a palette.
 * GIF: the first frame (animations show their first picture), transparency, interlacing.
 * JPEG: see jpeg.c. */
#include "image.h"

/* ================= inflate (DEFLATE decompression) ================= */
struct bits {
    const uint8_t *in; uint32_t len, pos;
    uint32_t buf; int cnt;
    uint8_t *out; uint32_t outlen, outpos;
    int err;
};
static int getbits(struct bits *s, int need) {
    uint32_t val = s->buf;
    while (s->cnt < need) {
        if (s->pos >= s->len) { s->err = 1; return 0; }
        val |= (uint32_t)s->in[s->pos++] << s->cnt;
        s->cnt += 8;
    }
    s->buf = need < 32 ? val >> need : 0;
    s->cnt -= need;
    return (int)(val & ((1u << need) - 1));
}

struct huff { short count[16]; short symbol[288]; };

/* Build a canonical Huffman decoder from code lengths; 0 = ok. */
static int huff_build(struct huff *h, const uint8_t *length, int n) {
    short offs[16];
    for (int i = 0; i < 16; i++) h->count[i] = 0;
    for (int s = 0; s < n; s++) h->count[length[s]]++;
    if (h->count[0] == n) return 0;                        /* no codes: fine, never used */
    int left = 1;
    for (int len = 1; len < 16; len++) { left <<= 1; left -= h->count[len]; if (left < 0) return -1; }
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = offs[len] + h->count[len];
    for (int s = 0; s < n; s++) if (length[s]) h->symbol[offs[length[s]]++] = (short)s;
    return 0;
}
static int huff_decode(struct bits *s, const struct huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= getbits(s, 1);
        if (s->err) return -1;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count; first += count;
        first <<= 1; code <<= 1;
    }
    return -1;
}

static const short len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const short len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const short dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const short dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int inflate_codes(struct bits *s, const struct huff *lit, const struct huff *dist) {
    for (;;) {
        int sym = huff_decode(s, lit);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (s->outpos >= s->outlen) return -2;
            s->out[s->outpos++] = (uint8_t)sym;
        } else if (sym == 256) return 0;
        else {
            sym -= 257;
            if (sym >= 29) return -1;
            int len = len_base[sym] + getbits(s, len_extra[sym]);
            int ds = huff_decode(s, dist);
            if (ds < 0 || ds >= 30) return -1;
            uint32_t d = (uint32_t)(dist_base[ds] + getbits(s, dist_extra[ds]));
            if (s->err || d > s->outpos) return -1;
            if (s->outpos + (uint32_t)len > s->outlen) return -2;
            uint8_t *o = s->out + s->outpos;
            for (int i = 0; i < len; i++) o[i] = o[(int)i - (int)d];
            s->outpos += (uint32_t)len;
        }
    }
}
static int inflate_fixed(struct bits *s, struct huff *lit, struct huff *dist) {
    uint8_t l[288];
    int i = 0;
    for (; i < 144; i++) l[i] = 8;
    for (; i < 256; i++) l[i] = 9;
    for (; i < 280; i++) l[i] = 7;
    for (; i < 288; i++) l[i] = 8;
    huff_build(lit, l, 288);
    for (i = 0; i < 30; i++) l[i] = 5;
    huff_build(dist, l, 30);
    return inflate_codes(s, lit, dist);
}
static int inflate_dynamic(struct bits *s, struct huff *lit, struct huff *dist) {
    static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    uint8_t l[320];
    int nlen = getbits(s, 5) + 257, ndist = getbits(s, 5) + 1, ncode = getbits(s, 4) + 4;
    if (s->err || nlen > 286 || ndist > 30) return -1;
    for (int i = 0; i < 19; i++) l[order[i]] = i < ncode ? (uint8_t)getbits(s, 3) : 0;
    struct huff lencode;
    if (huff_build(&lencode, l, 19)) return -1;
    for (int i = 0; i < nlen + ndist; ) {
        int sym = huff_decode(s, &lencode);
        if (sym < 0) return -1;
        if (sym < 16) { l[i++] = (uint8_t)sym; continue; }
        int rep, val = 0;
        if (sym == 16) { if (!i) return -1; val = l[i - 1]; rep = 3 + getbits(s, 2); }
        else if (sym == 17) rep = 3 + getbits(s, 3);
        else rep = 11 + getbits(s, 7);
        if (s->err || i + rep > nlen + ndist) return -1;
        while (rep--) l[i++] = (uint8_t)val;
    }
    if (huff_build(lit, l, nlen) || huff_build(dist, l + nlen, ndist)) return -1;
    return inflate_codes(s, lit, dist);
}
/* Unpack a zlib stream into out (whose size is known in advance). 0 = ok. */
static int zlib_inflate(const uint8_t *in, uint32_t len, uint8_t *out, uint32_t outlen, uint32_t *got, void *(*alloc)(uint32_t), void (*release)(void *)) {
    if (len < 2 || (in[0] & 15) != 8 || ((in[0] << 8) | in[1]) % 31 || (in[1] & 32)) return -1;
    struct bits s = { in, len, 2, 0, 0, out, outlen, 0, 0 };
    struct huff *t = alloc(sizeof(struct huff) * 2);      /* two decoders: ~1.2 KB, kept off the small stack */
    if (!t) return -1;
    int last, r = 0;
    do {
        last = getbits(&s, 1);
        int type = getbits(&s, 2);
        if (s.err) { r = -1; break; }
        if (type == 0) {                                   /* stored (not compressed) */
            s.buf = 0; s.cnt = 0;
            if (s.pos + 4 > len) { r = -1; break; }
            uint32_t n = in[s.pos] | in[s.pos + 1] << 8;
            s.pos += 4;
            if (s.pos + n > len || s.outpos + n > outlen) { r = -1; break; }
            for (uint32_t i = 0; i < n; i++) out[s.outpos++] = in[s.pos++];
        } else if (type == 1) r = inflate_fixed(&s, &t[0], &t[1]);
        else if (type == 2) r = inflate_dynamic(&s, &t[0], &t[1]);
        else r = -1;
    } while (!last && !r);
    release(t);
    *got = s.outpos;
    return r == -2 ? 0 : r;                                /* extra data past the picture is harmless */
}

/* ================= PNG ================= */
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int paeth(int a, int b, int c) {
    int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}
/* Undo the per-row filters of one (sub-)image in place. 0 = ok. */
static int unfilter(uint8_t *data, int rows, uint32_t stride, int bpp) {
    uint8_t *prev = 0;
    for (int y = 0; y < rows; y++) {
        uint8_t *row = data + y * (stride + 1), f = row[0], *p = row + 1;
        if (f > 4) return -1;
        for (uint32_t i = 0; i < stride; i++) {
            int a = i >= (uint32_t)bpp ? p[i - bpp] : 0, b = prev ? prev[i] : 0, c = prev && i >= (uint32_t)bpp ? prev[i - bpp] : 0;
            switch (f) {
            case 1: p[i] = (uint8_t)(p[i] + a); break;
            case 2: p[i] = (uint8_t)(p[i] + b); break;
            case 3: p[i] = (uint8_t)(p[i] + ((a + b) >> 1)); break;
            case 4: p[i] = (uint8_t)(p[i] + paeth(a, b, c)); break;
            }
        }
        prev = p;
    }
    return 0;
}

struct png_info {
    int w, h, depth, ctype, channels;
    uint32_t pal[256]; int npal;
    int trns_grey, trns_r, trns_g, trns_b, has_trns_key;
};
static int sample(const uint8_t *row, int index, int depth) {   /* one channel value, scaled to 0..255 */
    switch (depth) {
    case 16: return row[index * 2];
    case 8: return row[index];
    default: {
        int bit = index * depth, v = (row[bit >> 3] >> (8 - depth - (bit & 7))) & ((1 << depth) - 1);
        return v;                                          /* raw value; callers scale or use as palette index */
    }
    }
}
static int sample16(const uint8_t *row, int index) { return row[index * 2] << 8 | row[index * 2 + 1]; }

static uint32_t png_pixel(const struct png_info *p, const uint8_t *row, int x) {
    int d = p->depth, c = p->channels;
    if (p->ctype == 3) {
        int i = sample(row, x, d);
        return i < p->npal ? p->pal[i] : 0xFF000000;
    }
    if (p->ctype == 0 || p->ctype == 4) {
        int g, raw = d == 16 ? sample16(row, x * c) : sample(row, x * c, d);
        g = d == 16 ? raw >> 8 : d == 8 ? raw : raw * 255 / ((1 << d) - 1);
        uint32_t a = 255;
        if (p->ctype == 4) a = (uint32_t)sample(row, x * c + 1, d);
        else if (p->has_trns_key && raw == p->trns_grey) a = 0;
        return a << 24 | (uint32_t)g << 16 | (uint32_t)g << 8 | (uint32_t)g;
    }
    int r = sample(row, x * c, d), g = sample(row, x * c + 1, d), b = sample(row, x * c + 2, d);
    uint32_t a = 255;
    if (p->ctype == 6) a = (uint32_t)sample(row, x * c + 3, d);
    else if (p->has_trns_key) {
        int rr = d == 16 ? sample16(row, x * c) : r, gg = d == 16 ? sample16(row, x * c + 1) : g, bb = d == 16 ? sample16(row, x * c + 2) : b;
        if (rr == p->trns_r && gg == p->trns_g && bb == p->trns_b) a = 0;
    }
    return a << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

static const char *png_decode(const uint8_t *d, uint32_t len, struct image *out, void *(*alloc)(uint32_t), void (*release)(void *)) {
    struct png_info p;
    p.npal = 0; p.has_trns_key = 0; p.w = p.h = p.depth = 0; p.ctype = -1; p.trns_grey = p.trns_r = p.trns_g = p.trns_b = 0;
    uint32_t idat_total = 0, pos = 8;
    int interlace = 0;
    /* first pass: header, palette, transparency, and how much compressed data there is */
    while (pos + 12 <= len) {
        uint32_t n = be32(d + pos);
        const uint8_t *type = d + pos + 4, *c = d + pos + 8;
        if (n > len - pos - 12) return "The PNG file is cut short or damaged.";
        if (type[0] == 'I' && type[1] == 'H' && type[2] == 'D' && type[3] == 'R' && n >= 13) {
            p.w = (int)be32(c); p.h = (int)be32(c + 4); p.depth = c[8]; p.ctype = c[9]; interlace = c[12];
            if (c[10] || c[11] || interlace > 1) return "This PNG uses an unknown compression.";
        } else if (type[0] == 'P' && type[1] == 'L' && type[2] == 'T' && type[3] == 'E') {
            p.npal = (int)(n / 3 > 256 ? 256 : n / 3);
            for (int i = 0; i < p.npal; i++) p.pal[i] = 0xFF000000u | (uint32_t)c[i * 3] << 16 | (uint32_t)c[i * 3 + 1] << 8 | c[i * 3 + 2];
        } else if (type[0] == 't' && type[1] == 'R' && type[2] == 'N' && type[3] == 'S') {
            if (p.ctype == 3) for (uint32_t i = 0; i < n && (int)i < p.npal; i++) p.pal[i] = (p.pal[i] & 0xFFFFFF) | (uint32_t)c[i] << 24;
            else if (p.ctype == 0 && n >= 2) { p.trns_grey = c[0] << 8 | c[1]; p.has_trns_key = 1; }
            else if (p.ctype == 2 && n >= 6) { p.trns_r = c[0] << 8 | c[1]; p.trns_g = c[2] << 8 | c[3]; p.trns_b = c[4] << 8 | c[5]; p.has_trns_key = 1; }
        } else if (type[0] == 'I' && type[1] == 'D' && type[2] == 'A' && type[3] == 'T') idat_total += n;
        else if (type[0] == 'I' && type[1] == 'E' && type[2] == 'N' && type[3] == 'D') break;
        pos += n + 12;
    }
    if (!p.w) return "The PNG file has no header.";
    static const int chans[7] = { 1, 0, 3, 1, 2, 0, 4 };
    if (p.ctype > 6 || !chans[p.ctype]) return "This PNG has an unknown colour type.";
    p.channels = chans[p.ctype];
    int d_ok = p.depth == 8 || (p.depth == 16 && p.ctype != 3) || ((p.depth == 1 || p.depth == 2 || p.depth == 4) && (p.ctype == 0 || p.ctype == 3));
    if (!d_ok) return "This PNG has an unusual bit depth.";
    if (p.w <= 0 || p.h <= 0 || (uint32_t)p.w * (uint32_t)p.h > IMAGE_MAX_PIXELS || p.w > 30000 || p.h > 30000)
        return "The picture is too big (6 megapixels at most).";
    if (p.depth == 16 && p.has_trns_key && p.ctype == 0) {} /* grey key is compared on the raw 16-bit value */
    else if (p.has_trns_key && p.ctype == 2) {               /* 8-bit RGB keys compare against 8-bit samples */
        if (p.depth == 8) { p.trns_r &= 255; p.trns_g &= 255; p.trns_b &= 255; }
    }
    if (!idat_total) return "The PNG file has no picture data.";

    /* gather the compressed data (it may be split over many IDAT chunks) */
    uint8_t *z = alloc(idat_total);
    if (!z) return "Not enough memory.";
    uint32_t zn = 0; pos = 8;
    while (pos + 12 <= len) {
        uint32_t n = be32(d + pos);
        const uint8_t *type = d + pos + 4;
        if (n > len - pos - 12) break;
        if (type[0] == 'I' && type[1] == 'D' && type[2] == 'A' && type[3] == 'T') { for (uint32_t i = 0; i < n; i++) z[zn++] = d[pos + 8 + i]; }
        pos += n + 12;
    }
    /* how many bytes the unpacked data takes (7 passes when interlaced) */
    int bits_pp = p.depth * p.channels, bpp = bits_pp < 8 ? 1 : bits_pp / 8;
    static const int sx[7] = { 0, 4, 0, 2, 0, 1, 0 }, sy[7] = { 0, 0, 4, 0, 2, 0, 1 };
    static const int dx[7] = { 8, 8, 4, 4, 2, 2, 1 }, dy[7] = { 8, 8, 8, 4, 4, 2, 2 };
    int passes = interlace ? 7 : 1;
    uint32_t raw_len = 0;
    for (int k = 0; k < passes; k++) {
        int pw = interlace ? (p.w - sx[k] + dx[k] - 1) / dx[k] : p.w, ph = interlace ? (p.h - sy[k] + dy[k] - 1) / dy[k] : p.h;
        if (pw > 0 && ph > 0) raw_len += (uint32_t)ph * (((uint32_t)pw * bits_pp + 7) / 8 + 1);
    }
    uint8_t *raw = alloc(raw_len);
    uint32_t *px = alloc((uint32_t)p.w * p.h * 4);
    if (!raw || !px) { release(z); if (raw) release(raw); if (px) release(px); return "Not enough memory."; }
    uint32_t got = 0;
    int r = zlib_inflate(z, zn, raw, raw_len, &got, alloc, release);
    release(z);
    if (r || got < raw_len) { release(raw); release(px); return "The PNG picture data is damaged."; }

    uint8_t *q = raw;
    int alpha = 0;
    for (int k = 0; k < passes; k++) {
        int pw = interlace ? (p.w - sx[k] + dx[k] - 1) / dx[k] : p.w, ph = interlace ? (p.h - sy[k] + dy[k] - 1) / dy[k] : p.h;
        if (pw <= 0 || ph <= 0) continue;
        uint32_t stride = ((uint32_t)pw * bits_pp + 7) / 8;
        if (unfilter(q, ph, stride, bpp)) { release(raw); release(px); return "The PNG picture data is damaged."; }
        for (int y = 0; y < ph; y++) {
            const uint8_t *row = q + y * (stride + 1) + 1;
            int oy = interlace ? sy[k] + y * dy[k] : y;
            for (int x = 0; x < pw; x++) {
                int ox = interlace ? sx[k] + x * dx[k] : x;
                uint32_t v = png_pixel(&p, row, x);
                if ((v >> 24) != 255) alpha = 1;
                px[oy * p.w + ox] = v;
            }
        }
        q += (uint32_t)ph * (stride + 1);
    }
    release(raw);
    out->w = p.w; out->h = p.h; out->px = px; out->has_alpha = alpha; out->format = "PNG";
    return 0;
}

/* ================= BMP ================= */
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static const char *bmp_decode(const uint8_t *f, uint32_t size, struct image *out, void *(*alloc)(uint32_t)) {
    if (size < 54) return "The BMP file is damaged.";
    int iw = (int)le32(f + 18), ih = (int)le32(f + 22), bpp = f[28] | f[29] << 8, comp = (int)le32(f + 30), top_down = ih < 0;
    if (top_down) ih = -ih;
    if (comp != 0 && !(comp == 3 && bpp == 32)) return "Only uncompressed BMP pictures.";
    if (bpp != 24 && bpp != 32 && bpp != 8) return "Only 8, 24 and 32-bit BMP pictures.";
    if (iw <= 0 || ih <= 0 || (uint32_t)iw * (uint32_t)ih > IMAGE_MAX_PIXELS) return "The picture is too big (6 megapixels at most).";
    uint32_t off = le32(f + 10), row = ((uint32_t)iw * bpp / 8 + 3) & ~3u, hdr = le32(f + 14);
    if (off + row * ih > size) return "The BMP file is cut short.";
    uint32_t pal[256];
    if (bpp == 8) {
        int n = (int)le32(f + 46); if (!n || n > 256) n = 256;
        for (int i = 0; i < 256; i++) {
            uint32_t at = 14 + hdr + i * 4;
            pal[i] = i < n && at + 4 <= off ? 0xFF000000u | f[at + 2] << 16 | f[at + 1] << 8 | f[at] : 0xFF000000u;
        }
    }
    uint32_t *px = alloc((uint32_t)iw * ih * 4);
    if (!px) return "Not enough memory.";
    for (int y = 0; y < ih; y++) {
        const uint8_t *src = f + off + (top_down ? y : ih - 1 - y) * row;
        for (int x = 0; x < iw; x++) {
            if (bpp == 8) px[y * iw + x] = pal[src[x]];
            else { const uint8_t *s = src + x * (bpp / 8); px[y * iw + x] = 0xFF000000u | s[2] << 16 | s[1] << 8 | s[0]; }
        }
    }
    out->w = iw; out->h = ih; out->px = px; out->has_alpha = 0; out->format = "BMP";
    return 0;
}

/* ================= GIF (first frame) ================= */
static const char *gif_decode(const uint8_t *d, uint32_t len, struct image *out, void *(*alloc)(uint32_t), void (*release)(void *)) {
    if (len < 13) return "The GIF file is cut short.";
    int sw = d[6] | d[7] << 8, sh = d[8] | d[9] << 8, flags = d[10];
    uint32_t pos = 13, gpal[256], lpal[256];
    int gcount = 0, transparent = -1;
    if (flags & 0x80) {                                     /* global colour table */
        gcount = 2 << (flags & 7);
        if (pos + 3u * gcount > len) return "The GIF file is cut short.";
        for (int i = 0; i < gcount; i++) gpal[i] = 0xFF000000u | (uint32_t)d[pos + i * 3] << 16 | (uint32_t)d[pos + i * 3 + 1] << 8 | d[pos + i * 3 + 2];
        pos += 3u * gcount;
    }
    while (pos < len) {
        int b = d[pos++];
        if (b == 0x3B) break;                               /* end of file */
        if (b == 0x21) {                                    /* extension block */
            if (pos >= len) break;
            int label = d[pos++];
            if (label == 0xF9 && pos + 5 < len && d[pos] >= 4 && (d[pos + 1] & 1)) transparent = d[pos + 4];
            while (pos < len && d[pos]) pos += d[pos] + 1u;       /* skip its sub-blocks */
            pos++;
            continue;
        }
        if (b != 0x2C) return "The GIF file is damaged.";
        /* the first picture */
        if (pos + 9 > len) return "The GIF file is cut short.";
        int fx = d[pos] | d[pos + 1] << 8, fy = d[pos + 2] | d[pos + 3] << 8, fw = d[pos + 4] | d[pos + 5] << 8, fh = d[pos + 6] | d[pos + 7] << 8, lf = d[pos + 8];
        pos += 9;
        if (!sw || !sh) { sw = fx + fw; sh = fy + fh; }
        if (!sw || !sh || (uint32_t)sw * (uint32_t)sh > IMAGE_MAX_PIXELS) return "The picture is too big (6 megapixels at most).";
        const uint32_t *pal = gpal; int count = gcount;
        if (lf & 0x80) {
            count = 2 << (lf & 7);
            if (pos + 3u * count > len) return "The GIF file is cut short.";
            for (int i = 0; i < count; i++) lpal[i] = 0xFF000000u | (uint32_t)d[pos + i * 3] << 16 | (uint32_t)d[pos + i * 3 + 1] << 8 | d[pos + i * 3 + 2];
            pos += 3u * count; pal = lpal;
        }
        if (!count) return "The GIF file has no colours.";
        if (pos >= len) return "The GIF file is cut short.";
        int min_code = d[pos++];
        if (min_code < 2 || min_code > 11) return "The GIF file is damaged.";
        uint32_t *px = alloc((uint32_t)sw * sh * 4);
        uint16_t *prefix = alloc(4096 * 2);
        uint8_t *suffix = alloc(4096), *stack = alloc(4097), *idx = alloc((uint32_t)(fw ? fw : 1) * (fh ? fh : 1));
        if (!px || !prefix || !suffix || !stack || !idx) {
            if (px) release(px);
            if (prefix) release(prefix);
            if (suffix) release(suffix);
            if (stack) release(stack);
            if (idx) release(idx);
            return "Not enough memory.";
        }
        for (uint32_t i = 0; i < (uint32_t)sw * sh; i++) px[i] = 0;              /* see-through background */
        /* LZW: codes are read from a stream split into sub-blocks of up to 255 bytes */
        int clear = 1 << min_code, eoi = clear + 1, avail = clear + 2, code_size = min_code + 1, old = -1, first = 0;
        uint32_t bits = 0; int nbits = 0, block_left = 0;
        uint32_t n = 0, total = (uint32_t)fw * fh;
        for (int i = 0; i < clear; i++) { prefix[i] = 0xFFFF; suffix[i] = (uint8_t)i; }
        for (;;) {
            while (nbits < code_size) {                    /* get more bits */
                if (!block_left) { if (pos >= len || !d[pos]) goto done; block_left = d[pos++]; }
                if (pos >= len) goto done;
                bits |= (uint32_t)d[pos++] << nbits; nbits += 8; block_left--;
            }
            int code = (int)(bits & ((1u << code_size) - 1));
            bits >>= code_size; nbits -= code_size;
            if (code == clear) { code_size = min_code + 1; avail = clear + 2; old = -1; continue; }
            if (code == eoi) break;
            if (old < 0) {                                  /* first code after a clear */
                if (code >= clear) break;
                if (n < total) idx[n++] = (uint8_t)code;
                old = first = code; continue;
            }
            int in = code, sp = 0;
            if (code >= avail) { if (code > avail) break; stack[sp++] = (uint8_t)first; code = old; }
            while (code >= clear && sp < 4096) { stack[sp++] = suffix[code]; code = prefix[code]; if (code == 0xFFFF) break; }
            if (code >= clear) break;
            first = code; stack[sp++] = (uint8_t)code;
            if (avail < 4096) {
                prefix[avail] = (uint16_t)old; suffix[avail] = (uint8_t)first; avail++;
                if (avail == (1 << code_size) && code_size < 12) code_size++;
            }
            while (sp > 0 && n < total) idx[n++] = stack[--sp];
            old = in;
        }
    done:
        /* place the pixels (interlaced pictures store rows 0,8,16.. then 4,12.. then 2,6.. then odd rows) */
        for (uint32_t i = 0; i < total; i++) {
            int r = (int)(i / (uint32_t)fw), x = (int)(i % (uint32_t)fw), y = r;
            if (lf & 0x40) {
                int p1 = (fh + 7) / 8, p2 = (fh + 3) / 8, p3 = (fh + 1) / 4;
                if (r < p1) y = r * 8;
                else if (r < p1 + p2) y = (r - p1) * 8 + 4;
                else if (r < p1 + p2 + p3) y = (r - p1 - p2) * 4 + 2;
                else y = (r - p1 - p2 - p3) * 2 + 1;
            }
            int X = fx + x, Y = fy + y;
            if (X >= sw || Y >= sh || i >= n) continue;
            int c = idx[i];
            px[Y * sw + X] = c == transparent || c >= count ? 0 : pal[c];
        }
        release(prefix); release(suffix); release(stack); release(idx);
        int alpha = 0;
        for (uint32_t i = 0; i < (uint32_t)sw * sh && !alpha; i++) if ((px[i] >> 24) != 255) alpha = 1;
        out->w = sw; out->h = sh; out->px = px; out->has_alpha = alpha; out->format = "GIF";
        return 0;
    }
    return "The GIF file has no picture.";
}

/* ================= public ================= */
const char *image_decode(const uint8_t *data, uint32_t len, struct image *out,
                         void *(*alloc)(uint32_t), void (*release)(void *)) {
    out->px = 0; out->w = out->h = 0; out->has_alpha = 0; out->format = "";
    static const uint8_t sig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    if (len >= 8) {
        int png = 1;
        for (int i = 0; i < 8; i++) if (data[i] != sig[i]) png = 0;
        if (png) return png_decode(data, len, out, alloc, release);
    }
    if (len >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) return jpeg_decode(data, len, out, alloc, release);
    if (len >= 6 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F' && data[3] == '8') return gif_decode(data, len, out, alloc, release);
    if (len >= 2 && data[0] == 'B' && data[1] == 'M') return bmp_decode(data, len, out, alloc);
    return "This is not a PNG, JPEG, GIF or BMP picture.";
}

const char *image_thumbnail(const uint8_t *data, uint32_t len, struct image *out, int max_w, int max_h,
                            void *(*alloc)(uint32_t), void (*release)(void *)) {
    struct image full;
    const char *e;
    /* big JPEG photos: the 1/8 size picture is plenty for a preview, and far quicker */
    int big_jpeg = 0;
    if (len >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
        for (uint32_t i = 2; i + 9 < len; ) {                  /* peek at the size in the frame header */
            if (data[i] != 0xFF) { i++; continue; }
            int m = data[i + 1];
            if (m == 0xC0 || m == 0xC1 || m == 0xC2) { int h = data[i + 5] << 8 | data[i + 6], w = data[i + 7] << 8 | data[i + 8]; big_jpeg = w >= max_w * 8 || h >= max_h * 8; break; }
            if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7) || m == 0xFF) { i += m == 0xFF ? 1 : 2; continue; }
            if (m == 0xDA) break;
            i += 2 + (data[i + 2] << 8 | data[i + 3]);
        }
    }
    e = big_jpeg ? jpeg_decode_ex(data, len, &full, alloc, release, 1) : image_decode(data, len, &full, alloc, release);
    if (e) return e;
    int w = full.w, h = full.h;
    if (w > max_w) { h = h * max_w / w; w = max_w; }
    if (h > max_h) { w = w * max_h / h; h = max_h; }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    uint32_t *px = alloc((uint32_t)w * h * 4);
    if (!px) { release(full.px); return "Not enough memory."; }
    image_resample(&full, 0, 0, full.w, full.h, px, w, h);
    out->w = w; out->h = h; out->px = px; out->has_alpha = full.has_alpha; out->format = full.format;
    release(full.px);
    return 0;
}

int image_is_picture_name(const char *n) {
    int k = 0; while (n[k]) k++;
    if (k >= 6 && n[k - 5] == '.' && (n[k - 4] | 32) == 'j' && (n[k - 3] | 32) == 'p' && (n[k - 2] | 32) == 'e' && (n[k - 1] | 32) == 'g') return 1;
    if (k < 5 || n[k - 4] != '.') return 0;
    char a = n[k - 3] | 32, b = n[k - 2] | 32, c = n[k - 1] | 32;
    return (a == 'p' && b == 'n' && c == 'g') || (a == 'b' && b == 'm' && c == 'p') || (a == 'j' && b == 'p' && c == 'g') || (a == 'g' && b == 'i' && c == 'f');
}

/* Resize part of a picture: the box (sx, sy, sw, sh) of src fills dst (dw x dh).
 * Shrinking averages all the pixels that fall into each new pixel (smooth, no
 * jagged edges); enlarging blends the four nearest pixels. */
void image_resample(const struct image *src, int sx, int sy, int sw, int sh, uint32_t *dst, int dw, int dh) {
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    for (int y = 0; y < dh; y++) {
        uint32_t *out = dst + y * dw;
        if (sh >= dh) {                                    /* shrinking (or same) vertically */
            int y0 = sy + y * sh / dh, y1 = sy + (y + 1) * sh / dh;
            if (y1 <= y0) y1 = y0 + 1;
            for (int x = 0; x < dw; x++) {
                int x0, x1;
                if (sw >= dw) { x0 = sx + x * sw / dw; x1 = sx + (x + 1) * sw / dw; if (x1 <= x0) x1 = x0 + 1; }
                else { x0 = sx + x * sw / dw; x1 = x0 + 1; }
                uint32_t a = 0, r = 0, g = 0, b = 0, n = 0;
                for (int yy = y0; yy < y1 && yy < src->h; yy++) {
                    const uint32_t *row = src->px + yy * src->w;
                    for (int xx = x0; xx < x1 && xx < src->w; xx++) {
                        uint32_t p = row[xx];
                        a += p >> 24; r += (p >> 16) & 255; g += (p >> 8) & 255; b += p & 255; n++;
                    }
                }
                if (!n) n = 1;
                out[x] = (a / n) << 24 | (r / n) << 16 | (g / n) << 8 | (b / n);
            }
        } else {                                           /* enlarging: bilinear, 8-bit fractions */
            int fy = (int)(((uint32_t)(y * 2 + 1) * (uint32_t)sh * 128u) / (uint32_t)dh) - 128;
            if (fy < 0) fy = 0;
            int y0 = sy + (fy >> 8), ty = fy & 255, y1 = y0 + 1 < sy + sh ? y0 + 1 : y0;
            if (y1 >= src->h) y1 = src->h - 1;
            for (int x = 0; x < dw; x++) {
                int fx = sw >= dw ? (x * sw / dw) << 8 : (int)(((uint32_t)(x * 2 + 1) * (uint32_t)sw * 128u) / (uint32_t)dw) - 128;
                if (fx < 0) fx = 0;
                int x0 = sx + (fx >> 8), tx = fx & 255, x1 = x0 + 1 < sx + sw ? x0 + 1 : x0;
                if (x1 >= src->w) x1 = src->w - 1;
                uint32_t p00 = src->px[y0 * src->w + x0], p01 = src->px[y0 * src->w + x1];
                uint32_t p10 = src->px[y1 * src->w + x0], p11 = src->px[y1 * src->w + x1], o = 0;
                for (int s = 0; s < 32; s += 8) {
                    int c00 = (p00 >> s) & 255, c01 = (p01 >> s) & 255, c10 = (p10 >> s) & 255, c11 = (p11 >> s) & 255;
                    int top = c00 * (256 - tx) + c01 * tx, bot = c10 * (256 - tx) + c11 * tx;
                    o |= (uint32_t)((top * (256 - ty) + bot * ty) >> 16) << s;
                }
                out[x] = o;
            }
        }
    }
}
