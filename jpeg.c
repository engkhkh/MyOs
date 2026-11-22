/* jpeg.c - JPEG decoding for MyOS (used by image.c).
 *
 * Supports baseline and progressive JPEGs (Huffman coded, 8-bit), grey and
 * colour pictures with any common chroma subsampling (4:4:4, 4:2:2, 4:2:0,
 * ...), restart markers, Adobe RGB/CMYK files, and the camera's EXIF
 * "orientation" tag (so portrait photos come out the right way up).
 *
 * The maths follows the IJG / libjpeg-turbo reference ("islow" integer IDCT,
 * "fancy" upsampling, the same colour conversion), so pictures look exactly
 * as they do elsewhere. Integers only - no floating point (the kernel uses this). */
#include "image.h"

#define MAXC 4

struct huff_t {
    uint8_t lut_len[512]; uint8_t lut_sym[512];    /* fast path: codes of up to 9 bits */
    int maxcode[18]; int valptr[17]; uint16_t mincode[17];
    uint8_t vals[256]; int present;
};
struct comp {
    int id, h, v, tq, td, ta;
    int bw, bh;                  /* blocks stored (whole MCUs) */
    int stride;                  /* bytes per row of plane */
    int cw, ch;                  /* real size of this component in pixels */
    int16_t *coef;               /* bw * bh * 64 */
    uint8_t *plane;              /* bw*8 x bh*8 after the IDCT */
    int dc_pred;
};
struct jdec {
    const uint8_t *d; uint32_t len, pos;
    uint32_t bitbuf; int bitcnt; int marker_hit;
    uint16_t qt[4][64];
    struct huff_t dc[4], ac[4];
    struct comp c[MAXC]; int nc;
    int w, h, progressive, hmax, vmax, mcux, mcuy;
    int restart, eobrun;
    int adobe, adobe_transform, orientation;
    int small;                   /* 1 = make a 1/8 size picture from the blocks' average colours (previews) */
    void *(*alloc)(uint32_t); void (*release)(void *);
};

static const uint8_t zigzag[64 + 16] = {   /* natural order of the n-th coefficient (+ padding for bad data) */
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21,
    28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61,
    54, 47, 55, 62, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63 };

/* ---------------- bits ---------------- */
static void fill_bits(struct jdec *j) {
    while (j->bitcnt <= 24) {
        uint32_t byte = 0;
        if (!j->marker_hit && j->pos < j->len) {
            byte = j->d[j->pos];
            if (byte == 0xFF) {
                uint8_t nx = j->pos + 1 < j->len ? j->d[j->pos + 1] : 0;
                if (nx == 0x00) j->pos += 2;
                else { j->marker_hit = 1; byte = 0; }      /* a marker: give zeros from now on */
            } else j->pos++;
        }
        j->bitbuf |= byte << (24 - j->bitcnt);
        j->bitcnt += 8;
    }
}
static int get_bits(struct jdec *j, int n) {
    if (n <= 0 || n > 16) return 0;                    /* only a damaged file asks for more */
    if (j->bitcnt < n) fill_bits(j);
    int v = (int)(j->bitbuf >> (32 - n));
    j->bitbuf <<= n; j->bitcnt -= n;
    return v;
}
static int get_bit(struct jdec *j) { return get_bits(j, 1); }
static int extend(int v, int n) { return n > 0 && n <= 16 && v < (1 << (n - 1)) ? v - (1 << n) + 1 : v; }

static int huff_setup(struct huff_t *t, const uint8_t *counts, const uint8_t *vals, int nvals) {
    int k = 0, code = 0;
    for (int i = 0; i < 512; i++) t->lut_len[i] = 0;
    for (int i = 0; i < nvals && i < 256; i++) t->vals[i] = vals[i];
    for (int l = 1; l <= 16; l++) {
        t->valptr[l] = k; t->mincode[l] = (uint16_t)code;
        for (int i = 0; i < counts[l - 1]; i++, k++, code++) {
            if (k >= nvals || code >= (1 << l)) return -1;  /* more codes than fit: a damaged table */
            if (l <= 9) {
                int shift = 9 - l;
                for (int f = 0; f < (1 << shift); f++) { t->lut_len[(code << shift) | f] = (uint8_t)l; t->lut_sym[(code << shift) | f] = vals[k]; }
            }
        }
        t->maxcode[l] = counts[l - 1] ? code - 1 : -1;
        if (code > (1 << l)) return -1;
        code <<= 1;
    }
    t->maxcode[17] = 0x7FFFFFFF;
    t->present = 1;
    return 0;
}
static int huff_get(struct jdec *j, const struct huff_t *t) {
    if (j->bitcnt < 16) fill_bits(j);
    int peek = (int)(j->bitbuf >> 23);
    int l = t->lut_len[peek];
    if (l) { j->bitbuf <<= l; j->bitcnt -= l; return t->lut_sym[peek]; }
    for (l = 10; l <= 16; l++) {
        int code = (int)(j->bitbuf >> (32 - l));
        if (t->maxcode[l] >= 0 && code <= t->maxcode[l] && code >= t->mincode[l]) {
            j->bitbuf <<= l; j->bitcnt -= l;
            int idx = t->valptr[l] + code - t->mincode[l];
            return idx < 256 ? t->vals[idx] : 0;
        }
    }
    j->bitbuf <<= 16; j->bitcnt -= 16;                 /* bad code: skip it */
    return 0;
}

/* ---------------- one block of one scan ---------------- */
static void block_baseline(struct jdec *j, struct comp *c, int16_t *b) {
    int s = huff_get(j, &j->dc[c->td]);
    int diff = s ? extend(get_bits(j, s), s) : 0;
    c->dc_pred += diff;
    b[0] = (int16_t)c->dc_pred;
    for (int k = 1; k < 64; k++) {
        int rs = huff_get(j, &j->ac[c->ta]), r = rs >> 4;
        s = rs & 15;
        if (s) { k += r; if (k > 63) break; b[zigzag[k]] = (int16_t)extend(get_bits(j, s), s); }
        else { if (r != 15) break; k += 15; }
    }
}
static void block_dc_first(struct jdec *j, struct comp *c, int16_t *b, int al) {
    int s = huff_get(j, &j->dc[c->td]);
    int diff = s ? extend(get_bits(j, s), s) : 0;
    c->dc_pred += diff;
    b[0] = (int16_t)(c->dc_pred * (1 << al));
}
static void block_dc_refine(struct jdec *j, int16_t *b, int al) { if (get_bit(j)) b[0] |= (int16_t)(1 << al); }
static void block_ac_first(struct jdec *j, struct comp *c, int16_t *b, int ss, int se, int al) {
    if (j->eobrun > 0) { j->eobrun--; return; }
    for (int k = ss; k <= se; k++) {
        int rs = huff_get(j, &j->ac[c->ta]), r = rs >> 4, s = rs & 15;
        if (s) { k += r; if (k > 63) break; b[zigzag[k]] = (int16_t)(extend(get_bits(j, s), s) * (1 << al)); }
        else {
            if (r == 15) { k += 15; continue; }
            j->eobrun = 1 << r;
            if (r) j->eobrun += get_bits(j, r);
            j->eobrun--;
            break;
        }
    }
}
static void refine_coef(struct jdec *j, int16_t *coef, int p1, int m1) {
    if (get_bit(j) && (*coef & p1) == 0) *coef = (int16_t)(*coef >= 0 ? *coef + p1 : *coef + m1);
}
static void block_ac_refine(struct jdec *j, struct comp *c, int16_t *b, int ss, int se, int al) {
    int p1 = 1 << al, m1 = -1 * (1 << al), k = ss;
    if (j->eobrun <= 0) {
        for (; k <= se; k++) {
            int rs = huff_get(j, &j->ac[c->ta]), r = rs >> 4, s = rs & 15, val = 0;
            if (s) val = get_bit(j) ? p1 : m1;
            else if (r != 15) {
                j->eobrun = 1 << r;
                if (r) j->eobrun += get_bits(j, r);
                break;
            }
            for (; k <= se; k++) {                      /* skip r zero coefficients, refining non-zero ones */
                int16_t *coef = &b[zigzag[k]];
                if (*coef) refine_coef(j, coef, p1, m1);
                else { if (r == 0) break; r--; }
            }
            if (val && k <= se) b[zigzag[k]] = (int16_t)val;
        }
    }
    if (j->eobrun > 0) {
        for (; k <= se; k++) { int16_t *coef = &b[zigzag[k]]; if (*coef) refine_coef(j, coef, p1, m1); }
        j->eobrun--;
    }
}

/* ---------------- a scan (SOS) ---------------- */
static void reset_scan_state(struct jdec *j) {
    j->bitbuf = 0; j->bitcnt = 0; j->marker_hit = 0; j->eobrun = 0;
    for (int i = 0; i < j->nc; i++) j->c[i].dc_pred = 0;
}
static int do_restart(struct jdec *j) {
    /* skip to the RSTn marker that follows (it may be preceded by fill bytes) */
    j->bitbuf = 0; j->bitcnt = 0; j->marker_hit = 0;
    while (j->pos + 1 < j->len) {
        if (j->d[j->pos] == 0xFF && j->d[j->pos + 1] >= 0xD0 && j->d[j->pos + 1] <= 0xD7) { j->pos += 2; break; }
        if (j->d[j->pos] == 0xFF && j->d[j->pos + 1] != 0 && j->d[j->pos + 1] != 0xFF) break;   /* some other marker */
        j->pos++;
    }
    j->eobrun = 0;
    for (int i = 0; i < j->nc; i++) j->c[i].dc_pred = 0;
    return 0;
}
static void decode_scan(struct jdec *j, struct comp **sc, int ns, int ss, int se, int ah, int al) {
    reset_scan_state(j);
    int count = 0;
    #define BLOCK(cp, bx, by) do { \
        int16_t *blk_ = (cp)->coef + ((by) * (cp)->bw + (bx)) * 64; \
        if (!j->progressive) block_baseline(j, (cp), blk_); \
        else if (ss == 0) { if (!ah) block_dc_first(j, (cp), blk_, al); else block_dc_refine(j, blk_, al); } \
        else { if (!ah) block_ac_first(j, (cp), blk_, ss, se, al); else block_ac_refine(j, (cp), blk_, ss, se, al); } \
    } while (0)
    if (ns == 1) {                                     /* one component: its own block grid */
        struct comp *c = sc[0];
        int bx_n = (c->cw + 7) / 8, by_n = (c->ch + 7) / 8;
        for (int by = 0; by < by_n; by++)
            for (int bx = 0; bx < bx_n; bx++) {
                if (j->restart && count && count % j->restart == 0) do_restart(j);
                BLOCK(c, bx, by);
                count++;
            }
    } else {                                           /* interleaved: MCU by MCU */
        for (int my = 0; my < j->mcuy; my++)
            for (int mx = 0; mx < j->mcux; mx++) {
                if (j->restart && count && count % j->restart == 0) do_restart(j);
                for (int i = 0; i < ns; i++) {
                    struct comp *c = sc[i];
                    for (int y = 0; y < c->v; y++)
                        for (int x = 0; x < c->h; x++) BLOCK(c, mx * c->h + x, my * c->v + y);
                }
                count++;
            }
    }
    #undef BLOCK
}

/* ---------------- IDCT (libjpeg "islow") ---------------- */
#define CONST_BITS 13
#define PASS1_BITS 2
#define DESCALE(x, n) (((x) + ((int64_t)1 << ((n) - 1))) >> (n))
static uint8_t clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }
static uint8_t clamp64(int64_t v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }
static void idct_block(const int16_t *in, const uint16_t *q, uint8_t *out, int stride) {
    int64_t ws[64];   /* 64-bit, so even a damaged file cannot overflow (libjpeg-turbo does the same on 64-bit PCs) */
    for (int c = 0; c < 8; c++) {
        const int16_t *ip = in + c; int64_t *w = ws + c;
        if (!ip[8] && !ip[16] && !ip[24] && !ip[32] && !ip[40] && !ip[48] && !ip[56]) {
            int64_t dc = (int64_t)(ip[0] * q[c]) * (1 << PASS1_BITS);
            for (int r = 0; r < 8; r++) w[r * 8] = dc;
            continue;
        }
        int64_t z2 = (int64_t)ip[16] * q[16 + c], z3 = (int64_t)ip[48] * q[48 + c];
        int64_t z1 = (z2 + z3) * 4433;
        int64_t tmp2 = z1 + z3 * -15137, tmp3 = z1 + z2 * 6270;
        z2 = (int64_t)ip[0] * q[c]; z3 = (int64_t)ip[32] * q[32 + c];
        int64_t tmp0 = (z2 + z3) * (1 << CONST_BITS), tmp1 = (z2 - z3) * (1 << CONST_BITS);
        int64_t tmp10 = tmp0 + tmp3, tmp13 = tmp0 - tmp3, tmp11 = tmp1 + tmp2, tmp12 = tmp1 - tmp2;
        tmp0 = (int64_t)ip[56] * q[56 + c]; tmp1 = (int64_t)ip[40] * q[40 + c]; tmp2 = (int64_t)ip[24] * q[24 + c]; tmp3 = (int64_t)ip[8] * q[8 + c];
        z1 = tmp0 + tmp3; z2 = tmp1 + tmp2; z3 = tmp0 + tmp2; int64_t z4 = tmp1 + tmp3;
        int64_t z5 = (z3 + z4) * 9633;
        tmp0 *= 2446; tmp1 *= 16819; tmp2 *= 25172; tmp3 *= 12299;
        z1 *= -7373; z2 *= -20995; z3 *= -16069; z4 *= -3196;
        z3 += z5; z4 += z5;
        tmp0 += z1 + z3; tmp1 += z2 + z4; tmp2 += z2 + z3; tmp3 += z1 + z4;
        w[0] = DESCALE(tmp10 + tmp3, CONST_BITS - PASS1_BITS); w[56] = DESCALE(tmp10 - tmp3, CONST_BITS - PASS1_BITS);
        w[8] = DESCALE(tmp11 + tmp2, CONST_BITS - PASS1_BITS); w[48] = DESCALE(tmp11 - tmp2, CONST_BITS - PASS1_BITS);
        w[16] = DESCALE(tmp12 + tmp1, CONST_BITS - PASS1_BITS); w[40] = DESCALE(tmp12 - tmp1, CONST_BITS - PASS1_BITS);
        w[24] = DESCALE(tmp13 + tmp0, CONST_BITS - PASS1_BITS); w[32] = DESCALE(tmp13 - tmp0, CONST_BITS - PASS1_BITS);
    }
    for (int r = 0; r < 8; r++) {
        int64_t *w = ws + r * 8; uint8_t *o = out + r * stride;
        if (!w[1] && !w[2] && !w[3] && !w[4] && !w[5] && !w[6] && !w[7]) {
            uint8_t v = clamp64(DESCALE(w[0], PASS1_BITS + 3) + 128);
            for (int i = 0; i < 8; i++) o[i] = v;
            continue;
        }
        int64_t z2 = w[2], z3 = w[6];
        int64_t z1 = (z2 + z3) * 4433;
        int64_t tmp2 = z1 + z3 * -15137, tmp3 = z1 + z2 * 6270;
        int64_t tmp0 = (w[0] + w[4]) * (1 << CONST_BITS), tmp1 = (w[0] - w[4]) * (1 << CONST_BITS);
        int64_t tmp10 = tmp0 + tmp3, tmp13 = tmp0 - tmp3, tmp11 = tmp1 + tmp2, tmp12 = tmp1 - tmp2;
        tmp0 = w[7]; tmp1 = w[5]; tmp2 = w[3]; tmp3 = w[1];
        z1 = tmp0 + tmp3; z2 = tmp1 + tmp2; z3 = tmp0 + tmp2; int64_t z4 = tmp1 + tmp3;
        int64_t z5 = (z3 + z4) * 9633;
        tmp0 *= 2446; tmp1 *= 16819; tmp2 *= 25172; tmp3 *= 12299;
        z1 *= -7373; z2 *= -20995; z3 *= -16069; z4 *= -3196;
        z3 += z5; z4 += z5;
        tmp0 += z1 + z3; tmp1 += z2 + z4; tmp2 += z2 + z3; tmp3 += z1 + z4;
        const int n = CONST_BITS + PASS1_BITS + 3;
        o[0] = clamp64(DESCALE(tmp10 + tmp3, n) + 128); o[7] = clamp64(DESCALE(tmp10 - tmp3, n) + 128);
        o[1] = clamp64(DESCALE(tmp11 + tmp2, n) + 128); o[6] = clamp64(DESCALE(tmp11 - tmp2, n) + 128);
        o[2] = clamp64(DESCALE(tmp12 + tmp1, n) + 128); o[5] = clamp64(DESCALE(tmp12 - tmp1, n) + 128);
        o[3] = clamp64(DESCALE(tmp13 + tmp0, n) + 128); o[4] = clamp64(DESCALE(tmp13 - tmp0, n) + 128);
    }
}

/* ---------------- upsampling: one output row of one component ---------------- */
static void upsample_row(const struct jdec *j, const struct comp *c, int y, uint8_t *out) {
    int fx = j->hmax / c->h, fy = j->vmax / c->v, stride = c->stride, W = j->w;
    if (fx == 1 && fy == 1) { const uint8_t *p = c->plane + y * stride; for (int x = 0; x < W; x++) out[x] = p[x]; return; }
    int cw = c->cw;
    /* (like libjpeg, components only 1-2 samples wide just repeat their pixels) */
    if (cw > 2 && fx == 2 && fy == 1) {                          /* h2v1 "fancy": triangle filter across */
        const uint8_t *p = c->plane + y * stride;
        if (cw == 1) { for (int x = 0; x < W; x++) out[x] = p[0]; return; }
        uint8_t tmp[2]; (void)tmp;
        for (int i = 0; i < cw && 2 * i < W + 1; i++) {
            int v = p[i] * 3, a, b;
            if (i == 0) { a = p[0]; b = (p[0] * 3 + p[1] + 2) >> 2; }
            else if (i == cw - 1) { a = (v + p[i - 1] + 1) >> 2; b = p[i]; }
            else { a = (v + p[i - 1] + 1) >> 2; b = (v + p[i + 1] + 2) >> 2; }
            if (2 * i < W) out[2 * i] = (uint8_t)a;
            if (2 * i + 1 < W) out[2 * i + 1] = (uint8_t)b;
        }
        return;
    }
    if (cw > 2 && fx == 2 && fy == 2) {                /* h2v2 "fancy": triangle filter both ways */
        int iy = y >> 1, ny = (y & 1) ? iy + 1 : iy - 1;
        if (ny < 0) ny = 0;
        if (ny > c->ch - 1) ny = c->ch - 1;
        const uint8_t *p0 = c->plane + iy * stride, *p1 = c->plane + ny * stride;
        if (cw == 1) { int v = (p0[0] * 3 + p1[0]) * 4; for (int x = 0; x < W; x++) out[x] = (uint8_t)((v + 8) >> 4); return; }
        int last = 0, cur = p0[0] * 3 + p1[0], next;
        for (int i = 0; i < cw; i++) {
            next = i + 1 < cw ? p0[i + 1] * 3 + p1[i + 1] : 0;
            int a = i == 0 ? (cur * 4 + 8) >> 4 : (cur * 3 + last + 8) >> 4;
            int b = i == cw - 1 ? (cur * 4 + 7) >> 4 : (cur * 3 + next + 7) >> 4;
            if (2 * i < W) out[2 * i] = (uint8_t)a;
            if (2 * i + 1 < W) out[2 * i + 1] = (uint8_t)b;
            last = cur; cur = next;
        }
        return;
    }
    if (cw > 2 && fx == 1 && fy == 2) {                /* h1v2 "fancy" (4:4:0): triangle filter down */
        int iy = y >> 1, ny = (y & 1) ? iy + 1 : iy - 1, bias = (y & 1) ? 2 : 1;
        if (ny < 0) ny = 0;
        if (ny > c->ch - 1) ny = c->ch - 1;
        const uint8_t *p0 = c->plane + iy * stride, *p1 = c->plane + ny * stride;
        for (int x = 0; x < W; x++) out[x] = (uint8_t)((p0[x] * 3 + p1[x] + bias) >> 2);
        return;
    }
    const uint8_t *p = c->plane + (y / fy) * stride;   /* other ratios: plain repeat */
    for (int x = 0; x < W; x++) out[x] = p[x / fx];
}

/* ---------------- EXIF orientation ---------------- */
static void read_exif(struct jdec *j, const uint8_t *p, uint32_t n) {
    if (n < 14 || p[0] != 'E' || p[1] != 'x' || p[2] != 'i' || p[3] != 'f' || p[4] || p[5]) return;
    const uint8_t *t = p + 6; uint32_t tn = n - 6;
    int le = t[0] == 'I';
    #define U16(o) (le ? (t[o] | t[(o) + 1] << 8) : (t[o] << 8 | t[(o) + 1]))
    #define U32(o) (le ? (uint32_t)(t[o] | t[(o) + 1] << 8 | t[(o) + 2] << 16 | (uint32_t)t[(o) + 3] << 24) : (uint32_t)((uint32_t)t[o] << 24 | t[(o) + 1] << 16 | t[(o) + 2] << 8 | t[(o) + 3]))
    if (tn < 8 || (t[0] != 'I' && t[0] != 'M')) return;
    uint32_t ifd = U32(4);
    if (ifd + 2 > tn) return;
    int count = U16(ifd);
    for (int i = 0; i < count; i++) {
        uint32_t e = ifd + 2 + i * 12;
        if (e + 12 > tn) break;
        if (U16(e) == 0x0112) { int o = U16(e + 8); if (o >= 1 && o <= 8) j->orientation = o; }
    }
    #undef U16
    #undef U32
}

/* ---------------- the whole file ---------------- */
static const char *jpeg_body(struct jdec *j, struct image *out) {
    int frame = 0;
    j->pos = 2;
    for (;;) {
        while (j->pos < j->len && j->d[j->pos] != 0xFF) j->pos++;          /* find the next marker */
        while (j->pos < j->len && j->d[j->pos] == 0xFF) j->pos++;
        if (j->pos >= j->len) break;
        int m = j->d[j->pos++];
        if (m == 0xD9) break;                                             /* end of picture */
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) continue;
        if (j->pos + 2 > j->len) return "The JPEG file is cut short.";
        uint32_t n = (uint32_t)(j->d[j->pos] << 8 | j->d[j->pos + 1]);
        if (n < 2 || j->pos + n > j->len) return "The JPEG file is cut short or damaged.";
        const uint8_t *p = j->d + j->pos + 2; uint32_t pn = n - 2;
        j->pos += n;
        switch (m) {
        case 0xDB:                                                         /* quantisation tables */
            for (uint32_t i = 0; i < pn; ) {
                int pq = p[i] >> 4, tq = p[i] & 3; i++;
                if (i + (pq ? 128u : 64u) > pn) return "The JPEG file is damaged.";
                for (int k = 0; k < 64; k++) { j->qt[tq][zigzag[k]] = pq ? (uint16_t)(p[i] << 8 | p[i + 1]) : p[i]; i += pq ? 2 : 1; }
            }
            break;
        case 0xC4:                                                         /* Huffman tables */
            for (uint32_t i = 0; i + 17 <= pn; ) {
                int tc = p[i] >> 4, th = p[i] & 3, total = 0;
                for (int k = 0; k < 16; k++) total += p[i + 1 + k];
                if (total > 256 || i + 17 + total > pn) return "The JPEG file is damaged.";
                if (huff_setup(tc ? &j->ac[th] : &j->dc[th], p + i + 1, p + i + 17, total)) return "The JPEG file is damaged.";
                i += 17 + total;
            }
            break;
        case 0xDD: if (pn >= 2) j->restart = p[0] << 8 | p[1]; break;
        case 0xE1: read_exif(j, p, pn); break;
        case 0xEE: if (pn >= 12 && p[0] == 'A' && p[1] == 'd' && p[2] == 'o' && p[3] == 'b' && p[4] == 'e') { j->adobe = 1; j->adobe_transform = p[11]; } break;
        case 0xC0: case 0xC1: case 0xC2: {                                 /* start of frame */
            if (frame) return "The JPEG file is damaged.";
            frame = 1;
            j->progressive = m == 0xC2;
            if (pn < 6 || p[0] != 8) return "Only 8-bit JPEG pictures.";
            j->h = p[1] << 8 | p[2]; j->w = p[3] << 8 | p[4]; j->nc = p[5];
            if (!j->w || !j->h) return "This JPEG has no size.";
            if ((uint32_t)j->w * (uint32_t)j->h > IMAGE_MAX_PIXELS) return "The picture is too big (6 megapixels at most).";
            if (j->nc != 1 && j->nc != 3 && j->nc != 4) return "This JPEG has an unusual number of colours.";
            if (pn < 6u + 3u * j->nc) return "The JPEG file is damaged.";
            j->hmax = j->vmax = 1;
            for (int i = 0; i < j->nc; i++) {
                struct comp *c = &j->c[i];
                c->id = p[6 + i * 3]; c->h = p[7 + i * 3] >> 4; c->v = p[7 + i * 3] & 15; c->tq = p[8 + i * 3] & 3;
                if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4) return "The JPEG file is damaged.";
                if (c->h > j->hmax) j->hmax = c->h;
                if (c->v > j->vmax) j->vmax = c->v;
            }
            j->mcux = (j->w + 8 * j->hmax - 1) / (8 * j->hmax);
            j->mcuy = (j->h + 8 * j->vmax - 1) / (8 * j->vmax);
            for (int i = 0; i < j->nc; i++) {
                struct comp *c = &j->c[i];
                if (j->hmax % c->h || j->vmax % c->v) return "This JPEG uses unusual subsampling.";
                c->bw = j->mcux * c->h; c->bh = j->mcuy * c->v;
                c->cw = (j->w * c->h + j->hmax - 1) / j->hmax; c->ch = (j->h * c->v + j->vmax - 1) / j->vmax;
                uint32_t nb = (uint32_t)c->bw * c->bh;
                c->coef = j->alloc(nb * 64 * 2);
                if (!c->coef) return "Not enough memory.";
                for (uint32_t k = 0; k < nb * 64; k++) c->coef[k] = 0;
            }
            break;
        }
        case 0xC3: case 0xC5: case 0xC6: case 0xC7: case 0xC9: case 0xCA: case 0xCB: case 0xCD: case 0xCE: case 0xCF:
            return "This kind of JPEG (lossless / arithmetic) isn't supported.";
        case 0xDA: {                                                       /* start of scan */
            if (!frame) return "The JPEG file is damaged.";
            int ns = p[0];
            if (ns < 1 || ns > 4 || pn < 1u + 2u * ns + 3u) return "The JPEG file is damaged.";
            struct comp *sc[4];
            for (int i = 0; i < ns; i++) {
                int id = p[1 + i * 2], k;
                for (k = 0; k < j->nc && j->c[k].id != id; k++) {}
                if (k == j->nc) return "The JPEG file is damaged.";
                sc[i] = &j->c[k];
                sc[i]->td = p[2 + i * 2] >> 4 & 3; sc[i]->ta = p[2 + i * 2] & 3;
            }
            int ss = p[1 + ns * 2], se = p[2 + ns * 2], ah = p[3 + ns * 2] >> 4, al = p[3 + ns * 2] & 15;
            if (!j->progressive) { ss = 0; se = 63; ah = al = 0; }
            if (ss > 63 || se > 63 || ss > se || al > 13) return "The JPEG file is damaged.";
            for (int i = 0; i < ns; i++) {                                 /* the needed tables must exist */
                if ((ss == 0 && !ah && !j->dc[sc[i]->td].present) || (se > 0 && (ss > 0 || !j->progressive) && !j->ac[sc[i]->ta].present))
                    return "The JPEG file is missing a table.";
            }
            if (j->progressive && ss > 0 && ns != 1) return "The JPEG file is damaged.";
            if (!(j->small && j->progressive && ss > 0)) decode_scan(j, sc, ns, ss, se, ah, al);   /* previews need no detail */
            /* skip over the scan's data to the next real marker */
            while (j->pos + 1 < j->len && !(j->d[j->pos] == 0xFF && j->d[j->pos + 1] != 0 && !(j->d[j->pos + 1] >= 0xD0 && j->d[j->pos + 1] <= 0xD7) && j->d[j->pos + 1] != 0xFF)) j->pos++;
            break;
        }
        default: break;                                                    /* APPn, COM, ...: ignore */
        }
    }
    if (!frame) return "This is not a JPEG picture.";

    if (j->small) {                                    /* each block's average colour = one pixel */
        j->w = (j->w + 7) / 8; j->h = (j->h + 7) / 8;
        for (int i = 0; i < j->nc; i++) {
            struct comp *c = &j->c[i];
            c->stride = c->bw;
            c->cw = (c->cw + 7) / 8; c->ch = (c->ch + 7) / 8;
            c->plane = j->alloc((uint32_t)c->bw * c->bh);
            if (!c->plane) return "Not enough memory.";
            for (int b = 0; b < c->bw * c->bh; b++) {
                int64_t v = (((int64_t)c->coef[b * 64] * j->qt[c->tq][0]) + 4) >> 3;   /* = the IDCT of a flat block */
                c->plane[b] = clamp64(v + 128);
            }
            j->release(c->coef); c->coef = 0;
        }
    } else
    /* dequantise + IDCT every block into a plane per component */
    for (int i = 0; i < j->nc; i++) {
        struct comp *c = &j->c[i];
        c->stride = c->bw * 8;
        c->plane = j->alloc((uint32_t)c->bw * 8 * c->bh * 8);
        if (!c->plane) return "Not enough memory.";
        for (int by = 0; by < c->bh; by++)
            for (int bx = 0; bx < c->bw; bx++)
                idct_block(c->coef + (by * c->bw + bx) * 64, j->qt[c->tq], c->plane + by * 8 * c->bw * 8 + bx * 8, c->bw * 8);
        j->release(c->coef); c->coef = 0;
    }
    /* upsample + colour convert */
    uint32_t *px = j->alloc((uint32_t)j->w * j->h * 4);
    uint8_t *rows = j->alloc((uint32_t)j->w * 4);
    if (!px || !rows) { if (px) j->release(px); if (rows) j->release(rows); return "Not enough memory."; }
    int rgb = j->nc == 3 && ((j->adobe && j->adobe_transform == 0) || (j->c[0].id == 'R' && j->c[1].id == 'G' && j->c[2].id == 'B'));
    int ycck = j->nc == 4 && j->adobe && j->adobe_transform == 2;
    for (int y = 0; y < j->h; y++) {
        for (int i = 0; i < j->nc; i++) upsample_row(j, &j->c[i], y, rows + i * j->w);
        uint32_t *o = px + y * j->w;
        for (int x = 0; x < j->w; x++) {
            int Y = rows[x], r, g, b;
            if (j->nc == 1) { r = g = b = Y; }
            else {
                int cb = rows[j->w + x] - 128, cr = rows[2 * j->w + x] - 128;
                if (rgb) { r = Y; g = cb + 128; b = cr + 128; }
                else {
                    r = Y + ((91881 * cr + 32768) >> 16);
                    g = Y + ((-22554 * cb - 46802 * cr + 32768) >> 16);
                    b = Y + ((116130 * cb + 32768) >> 16);
                }
                if (j->nc == 4) {                                          /* CMYK (Adobe stores it inverted) */
                    int k = rows[3 * j->w + x];
                    if (ycck) { r = 255 - clamp8(r); g = 255 - clamp8(g); b = 255 - clamp8(b); }
                    else { r = Y; g = cb + 128; b = cr + 128; }
                    r = clamp8(r) * k / 255; g = clamp8(g) * k / 255; b = clamp8(b) * k / 255;
                    if (!j->adobe) { r = 255 - r; g = 255 - g; b = 255 - b; }
                }
            }
            o[x] = 0xFF000000u | (uint32_t)clamp8(r) << 16 | (uint32_t)clamp8(g) << 8 | clamp8(b);
        }
    }
    j->release(rows);
    /* turn the picture the way the camera says */
    int o = j->orientation, W = j->w, H = j->h;
    if (o >= 2 && o <= 8) {
        int nw = o >= 5 ? H : W, nh = o >= 5 ? W : H;
        uint32_t *t = j->alloc((uint32_t)W * H * 4);
        if (t) {
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++) {
                    int dx, dy;
                    switch (o) {
                    case 2: dx = W - 1 - x; dy = y; break;              /* mirrored */
                    case 3: dx = W - 1 - x; dy = H - 1 - y; break;      /* upside down */
                    case 4: dx = x; dy = H - 1 - y; break;
                    case 5: dx = y; dy = x; break;
                    case 6: dx = H - 1 - y; dy = x; break;              /* turned right (most portrait photos) */
                    case 7: dx = H - 1 - y; dy = W - 1 - x; break;
                    default: dx = y; dy = W - 1 - x; break;             /* 8: turned left */
                    }
                    t[dy * nw + dx] = px[y * W + x];
                }
            j->release(px); px = t; W = nw; H = nh;
        }
    }
    out->w = W; out->h = H; out->px = px; out->has_alpha = 0; out->format = "JPEG";
    return 0;
}

const char *jpeg_decode(const uint8_t *data, uint32_t len, struct image *out, void *(*alloc)(uint32_t), void (*release)(void *)) {
    return jpeg_decode_ex(data, len, out, alloc, release, 0);
}
const char *jpeg_decode_ex(const uint8_t *data, uint32_t len, struct image *out, void *(*alloc)(uint32_t), void (*release)(void *), int small) {
    struct jdec *j = alloc(sizeof(struct jdec));        /* ~6 KB: too big for a small stack */
    if (!j) return "Not enough memory.";
    uint8_t *z = (uint8_t *)j;
    for (uint32_t i = 0; i < sizeof(struct jdec); i++) z[i] = 0;
    j->d = data; j->len = len; j->alloc = alloc; j->release = release; j->small = small;
    const char *e = jpeg_body(j, out);
    for (int i = 0; i < MAXC; i++) { if (j->c[i].coef) release(j->c[i].coef); if (j->c[i].plane) release(j->c[i].plane); }
    release(j);
    return e;
}
