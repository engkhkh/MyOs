/* fat32.c - FAT32 file system (the format used by USB sticks and SD cards).
 *
 * Layout of a FAT32 volume:
 *   [boot sector + reserved] [FAT #1] [FAT #2] [data clusters ...]
 * Files and folders are stored in "clusters". The FAT (File Allocation Table)
 * is a big array saying, for each cluster, which cluster comes next in a file,
 * so a file is a linked chain of clusters. A folder is just a file full of
 * 32-byte entries (name, size, first cluster, dates). Long names such as
 * "My notes.txt" are stored in extra "LFN" entries just before the short one.
 *
 * Paths given to these functions are absolute, like "/DOCS/readme.txt".
 * Upper/lower case is ignored, as on Windows.
 */
#include <stdint.h>
#include <stddef.h>
#include "kernel.h"

#define EOC 0x0FFFFFF8u           /* "end of chain" marker (and above) */
#define ATTR_DIR 0x10
#define ATTR_LFN 0x0F
#define ATTR_VOL 0x08
#define MAX_LFN_SLOTS 20

static int mounted;
static uint32_t part_lba, fat_lba, data_lba, fsinfo_lba;
static uint32_t spc, fat_sz, nfats, root_clus, total_clusters, cluster_bytes;
static uint32_t free_clusters, alloc_hint = 2;
static int fsinfo_invalidated;
static char volume_label[12];
volatile uint32_t fs_generation;     /* bumped on every change, so open windows can refresh */

/* ---------- small helpers ---------- */
static void mcopy(void *d, const void *s, uint32_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; }
static void mzero(void *d, uint32_t n) { uint8_t *a = d; while (n--) *a++ = 0; }
static uint16_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static char upc(char c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
static int name_eq(const char *a, const char *b) { while (*a && upc(*a) == upc(*b)) { a++; b++; } return !*a && !*b; }
static uint32_t clus_lba(uint32_t c) { return data_lba + (c - 2) * spc; }

/* ---------- FAT table ---------- */
static uint8_t fat_cache[512];
static uint32_t fat_cache_lba = 0xFFFFFFFF;

static int fat_load(uint32_t lba) {
    if (lba == fat_cache_lba) return 1;
    if (!ata_read(lba, 1, fat_cache)) return 0;
    fat_cache_lba = lba;
    return 1;
}
static uint32_t fat_get(uint32_t c) {
    if (!fat_load(fat_lba + c * 4 / 512)) return EOC;
    return rd32(&fat_cache[c * 4 % 512]) & 0x0FFFFFFF;
}
static void invalidate_fsinfo(void) {
    if (fsinfo_invalidated || !fsinfo_lba) return;
    uint8_t s[512];
    if (ata_read(fsinfo_lba, 1, s) && rd32(s) == 0x41615252) {
        wr32(s + 488, 0xFFFFFFFF); wr32(s + 492, 0xFFFFFFFF);  /* "unknown": other OSes will recount */
        ata_write(fsinfo_lba, 1, s);
    }
    fsinfo_invalidated = 1;
}
static int fat_set(uint32_t c, uint32_t v) {
    invalidate_fsinfo();
    uint32_t lba = fat_lba + c * 4 / 512;
    if (!fat_load(lba)) return 0;
    uint8_t *p = &fat_cache[c * 4 % 512];
    uint32_t old = rd32(p) & 0x0FFFFFFF;
    wr32(p, (rd32(p) & 0xF0000000) | (v & 0x0FFFFFFF));
    for (uint32_t i = 0; i < nfats; i++)                 /* keep every FAT copy in sync */
        if (!ata_write(lba + i * fat_sz, 1, fat_cache)) return 0;
    if (old == 0 && v != 0) free_clusters--;
    if (old != 0 && v == 0) free_clusters++;
    return 1;
}

static uint32_t alloc_cluster(uint32_t prev, int zero) {
    for (uint32_t n = 0; n < total_clusters; n++) {
        uint32_t c = 2 + (alloc_hint - 2 + n) % total_clusters;
        if (fat_get(c) == 0) {
            if (!fat_set(c, 0x0FFFFFFF)) return 0;
            if (prev && !fat_set(prev, c)) return 0;
            alloc_hint = c + 1;
            if (zero) {
                uint8_t *z = kmalloc(cluster_bytes);
                if (!z) return 0;
                mzero(z, cluster_bytes);
                ata_write(clus_lba(c), spc, z);
                kfree(z);
            }
            return c;
        }
    }
    return 0;   /* disk full */
}

static void free_chain(uint32_t c) {
    while (c >= 2 && c < EOC) { uint32_t n = fat_get(c); fat_set(c, 0); c = n; }
}

/* ---------- directory entries ---------- */
struct dinfo {
    char name[FS_NAME_MAX];
    uint8_t raw[32];
    uint32_t lba, off;                       /* where the short entry lives */
    uint32_t lfn_lba[MAX_LFN_SLOTS], lfn_off[MAX_LFN_SLOTS];
    int nlfn;
};
static uint32_t ent_cluster(const uint8_t *e) { return (uint32_t)rd16(e + 20) << 16 | rd16(e + 26); }

static void short_to_name(const uint8_t *e, char *out) {
    int k = 0;
    for (int i = 0; i < 8 && e[i] != ' '; i++) out[k++] = e[i];
    if (e[8] != ' ') { out[k++] = '.'; for (int i = 8; i < 11 && e[i] != ' '; i++) out[k++] = e[i]; }
    out[k] = 0;
}

/* Walk every entry of a folder. The callback returns 1 to stop. */
typedef int (*dir_cb)(struct dinfo *d, void *ctx);
static int dir_walk(uint32_t dclus, dir_cb cb, void *ctx) {
    uint8_t *buf = kmalloc(cluster_bytes);
    if (!buf) return 0;
    struct dinfo d;
    char lfn[MAX_LFN_SLOTS * 13 + 1];
    int have_lfn = 0, result = 0;
    d.nlfn = 0;
    for (uint32_t c = dclus; c >= 2 && c < EOC; c = fat_get(c)) {
        if (!ata_read(clus_lba(c), spc, buf)) break;
        for (uint32_t i = 0; i < cluster_bytes / 32; i++) {
            uint8_t *e = buf + i * 32;
            uint32_t lba = clus_lba(c) + i * 32 / 512, off = i * 32 % 512;
            if (e[0] == 0x00) goto done;                          /* end of folder */
            if (e[0] == 0xE5) { have_lfn = 0; d.nlfn = 0; continue; }   /* deleted */
            if (e[11] == ATTR_LFN) {
                int ord = e[0] & 0x3F;
                if (e[0] & 0x40) { have_lfn = 1; d.nlfn = 0; mzero(lfn, sizeof lfn); }
                if (ord < 1 || ord > MAX_LFN_SLOTS || d.nlfn >= MAX_LFN_SLOTS) { have_lfn = 0; continue; }
                static const int pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
                for (int k = 0; k < 13; k++) {
                    uint16_t ch = rd16(e + pos[k]);
                    lfn[(ord - 1) * 13 + k] = (ch == 0 || ch == 0xFFFF) ? 0 : (ch < 128 ? (char)ch : '?');
                }
                d.lfn_lba[d.nlfn] = lba; d.lfn_off[d.nlfn] = off; d.nlfn++;
                continue;
            }
            if (e[11] & ATTR_VOL) { have_lfn = 0; d.nlfn = 0; continue; }
            mcopy(d.raw, e, 32);
            d.lba = lba; d.off = off;
            if (have_lfn && lfn[0]) { int k = 0; for (; k < FS_NAME_MAX - 1 && lfn[k]; k++) d.name[k] = lfn[k]; d.name[k] = 0; }
            else { short_to_name(e, d.name); d.nlfn = 0; }
            if (cb(&d, ctx)) { result = 1; goto done; }
            have_lfn = 0; d.nlfn = 0;
        }
    }
done:
    kfree(buf);
    return result;
}

static int write_raw(uint32_t lba, uint32_t off, const uint8_t *raw) {
    uint8_t s[512];
    if (!ata_read(lba, 1, s)) return 0;
    mcopy(s + off, raw, 32);
    return ata_write(lba, 1, s);
}

struct find_ctx { const char *name; struct dinfo *out; };
static int find_cb(struct dinfo *d, void *v) {
    struct find_ctx *f = v;
    char sn[13]; short_to_name(d->raw, sn);
    if (name_eq(f->name, d->name) || name_eq(f->name, sn)) { *f->out = *d; return 1; }
    return 0;
}
static int dir_find(uint32_t dclus, const char *name, struct dinfo *out) {
    struct find_ctx f = { name, out };
    return dir_walk(dclus, find_cb, &f);
}

/* ---------- paths ---------- */
/* Resolve a folder path to its first cluster. Returns 0 if not found. */
static uint32_t resolve_dir(const char *path) {
    uint32_t c = root_clus;
    char part[FS_NAME_MAX];
    while (*path) {
        while (*path == '/' || *path == '\\') path++;
        if (!*path) break;
        int k = 0;
        while (*path && *path != '/' && *path != '\\') { if (k < FS_NAME_MAX - 1) part[k++] = *path; path++; }
        part[k] = 0;
        struct dinfo d;
        if (!dir_find(c, part, &d) || !(d.raw[11] & ATTR_DIR)) return 0;
        c = ent_cluster(d.raw);
        if (c == 0) c = root_clus;                /* ".." pointing at the root */
    }
    return c;
}

/* Rewrite a path with each name spelled as stored on disk ("/docs" -> "/Docs"). */
int fs_canonical(const char *path, char *out) {
    if (!mounted) return 0;
    uint32_t c = root_clus;
    int o = 0;
    char part[FS_NAME_MAX];
    while (*path) {
        while (*path == '/' || *path == '\\') path++;
        if (!*path) break;
        int k = 0;
        while (*path && *path != '/' && *path != '\\') { if (k < FS_NAME_MAX - 1) part[k++] = *path; path++; }
        part[k] = 0;
        struct dinfo d;
        if (!dir_find(c, part, &d)) return 0;
        out[o++] = '/';
        for (int i = 0; d.name[i] && o < FS_PATH_MAX - 1; i++) out[o++] = d.name[i];
        c = ent_cluster(d.raw);
        if (c == 0) c = root_clus;
    }
    if (!o) out[o++] = '/';
    out[o] = 0;
    return 1;
}

/* Split "/a/b/name" into the parent folder's cluster and "name". */
static uint32_t split(const char *path, char *leaf) {
    int last = -1, n = 0;
    for (; path[n]; n++) if (path[n] == '/' || path[n] == '\\') last = n;
    while (n > 0 && (path[n - 1] == '/' || path[n - 1] == '\\')) n--;
    int k = 0;
    for (int i = last + 1; i < n && k < FS_NAME_MAX - 1; i++) leaf[k++] = path[i];
    leaf[k] = 0;
    char parent[FS_PATH_MAX];
    int p = last > 0 ? last : 0;
    if (p >= FS_PATH_MAX) return 0;
    for (int i = 0; i < p; i++) parent[i] = path[i];
    parent[p] = 0;
    return resolve_dir(parent);
}

/* ---------- creating entries ---------- */
static void timestamp(uint16_t *date, uint16_t *tim) {
    int h, m, s, d, mo, y;
    rtc_time(&h, &m, &s); rtc_date(&d, &mo, &y);
    *date = (uint16_t)(((y - 1980) << 9) | (mo << 5) | d);
    *tim = (uint16_t)((h << 11) | (m << 5) | (s / 2));
}

static int valid_83_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '~' || c == '!' || c == '#' || c == '$' || c == '%' || c == '&';
}

/* Is the name already a plain upper-case 8.3 name like "README.TXT"? */
static int is_plain_83(const char *n, uint8_t *out) {
    int i = 0, k = 0;
    for (int j = 0; j < 11; j++) out[j] = ' ';
    for (; n[i] && n[i] != '.'; i++) { if (k >= 8 || !valid_83_char(n[i])) return 0; out[k++] = n[i]; }
    if (k == 0) return 0;
    if (n[i] == '.') {
        i++; k = 8;
        for (; n[i]; i++) { if (k >= 11 || !valid_83_char(n[i])) return 0; out[k++] = n[i]; }
        if (k == 8) return 0;
    }
    return 1;
}

struct short_ctx { const uint8_t *sn; int clash; };
static int short_cb(struct dinfo *d, void *v) {
    struct short_ctx *s = v;
    for (int i = 0; i < 11; i++) if (d->raw[i] != s->sn[i]) return 0;
    s->clash = 1; return 1;
}

/* Make a unique short name such as "MYNOTE~1TXT" for a long name. */
static void make_short(uint32_t dclus, const char *name, uint8_t *sn) {
    int last_dot = -1, n = 0;
    for (; name[n]; n++) if (name[n] == '.') last_dot = n;
    char base[9] = { 0 }, ext[4] = { 0 };
    int k = 0;
    for (int i = 0; i < (last_dot > 0 ? last_dot : n) && k < 6; i++) {
        char c = upc(name[i]);
        if (c == ' ' || c == '.') continue;
        base[k++] = valid_83_char(c) ? c : '_';
    }
    if (!k) base[k++] = 'F';
    int e = 0;
    if (last_dot > 0) for (int i = last_dot + 1; name[i] && e < 3; i++) { char c = upc(name[i]); if (c != ' ') ext[e++] = valid_83_char(c) ? c : '_'; }
    for (int num = 1; num < 100; num++) {
        for (int j = 0; j < 11; j++) sn[j] = ' ';
        int p = 0;
        for (; p < k; p++) sn[p] = base[p];
        sn[p++] = '~';
        if (num >= 10) sn[p++] = '0' + num / 10;
        sn[p] = '0' + num % 10;
        for (int j = 0; j < e; j++) sn[8 + j] = ext[j];
        struct short_ctx s = { sn, 0 };
        dir_walk(dclus, short_cb, &s);
        if (!s.clash) return;
    }
}

static uint8_t lfn_checksum(const uint8_t *sn) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + sn[i]);
    return sum;
}

/* Find `need` consecutive free entry slots in a folder, growing it if needed. */
static int find_slots(uint32_t dclus, int need, uint32_t *lbas, uint32_t *offs) {
    uint8_t *buf = kmalloc(cluster_bytes);
    if (!buf) return 0;
    int run = 0;
    uint32_t c = dclus, last = dclus;
    for (;;) {
        for (; c >= 2 && c < EOC; c = fat_get(c)) {
            last = c;
            if (!ata_read(clus_lba(c), spc, buf)) { kfree(buf); return 0; }
            for (uint32_t i = 0; i < cluster_bytes / 32; i++) {
                uint8_t f = buf[i * 32];
                if (f == 0x00 || f == 0xE5) {
                    lbas[run] = clus_lba(c) + i * 32 / 512; offs[run] = i * 32 % 512;
                    if (++run == need) { kfree(buf); return 1; }
                } else run = 0;
            }
        }
        c = alloc_cluster(last, 1);               /* folder is full: add a cluster */
        if (!c) { kfree(buf); return 0; }
    }
}

static int create_entry(uint32_t dclus, const char *name, uint8_t attr, uint32_t first, uint32_t size, struct dinfo *out) {
    uint8_t sn[11];
    int lfn = !is_plain_83(name, sn);
    if (lfn) make_short(dclus, name, sn);
    int len = 0; while (name[len]) len++;
    int nl = lfn ? (len + 12) / 13 : 0;
    if (nl + 1 > MAX_LFN_SLOTS) return 0;
    uint32_t lbas[MAX_LFN_SLOTS + 1], offs[MAX_LFN_SLOTS + 1];
    if (!find_slots(dclus, nl + 1, lbas, offs)) return 0;

    uint8_t e[32];
    uint8_t sum = lfn_checksum(sn);
    static const int pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    for (int s = 0; s < nl; s++) {                /* long-name pieces, last piece first */
        int ord = nl - s;
        mzero(e, 32);
        e[0] = (uint8_t)(ord | (s == 0 ? 0x40 : 0));
        e[11] = ATTR_LFN; e[13] = sum;
        for (int k = 0; k < 13; k++) {
            int ci = (ord - 1) * 13 + k;
            uint16_t ch = ci < len ? (uint8_t)name[ci] : (ci == len ? 0 : 0xFFFF);
            wr16(e + pos[k], ch);
        }
        if (!write_raw(lbas[s], offs[s], e)) return 0;
    }
    mzero(e, 32);
    mcopy(e, sn, 11);
    e[11] = attr;
    uint16_t dt, tm; timestamp(&dt, &tm);
    wr16(e + 14, tm); wr16(e + 16, dt); wr16(e + 18, dt); wr16(e + 22, tm); wr16(e + 24, dt);
    wr16(e + 20, first >> 16); wr16(e + 26, first & 0xFFFF);
    wr32(e + 28, size);
    if (!write_raw(lbas[nl], offs[nl], e)) return 0;
    if (out) { mcopy(out->raw, e, 32); out->lba = lbas[nl]; out->off = offs[nl]; }
    return 1;
}

static int valid_name(const char *n) {
    if (!*n || (n[0] == '.' && (!n[1] || (n[1] == '.' && !n[2])))) return 0;
    for (; *n; n++) if (*n == '/' || *n == '\\' || *n == ':' || *n == '*' || *n == '?' || *n == '"' || *n == '<' || *n == '>' || *n == '|') return 0;
    return 1;
}

/* ---------- public API ---------- */
int fs_init(void) {
    uint8_t s[512];
    mounted = 0;
    if (!ata_present() || !ata_read(0, 1, s) || rd16(s + 510) != 0xAA55) return 0;
    part_lba = 0;
    if (!(s[82] == 'F' && s[83] == 'A' && s[84] == 'T' && s[85] == '3' && s[86] == '2')) {
        for (int i = 0; i < 4; i++) {                       /* look in the partition table */
            uint8_t *p = s + 446 + i * 16;
            if (p[4] == 0x0B || p[4] == 0x0C) { part_lba = rd32(p + 8); break; }
        }
        if (!part_lba || !ata_read(part_lba, 1, s)) return 0;
    }
    if (rd16(s + 11) != 512 || !s[13] || !rd32(s + 36)) return 0;   /* only 512-byte sectors */
    spc = s[13];
    nfats = s[16];
    fat_sz = rd32(s + 36);
    root_clus = rd32(s + 44);
    fat_lba = part_lba + rd16(s + 14);
    data_lba = fat_lba + nfats * fat_sz;
    fsinfo_lba = rd16(s + 48) ? part_lba + rd16(s + 48) : 0;
    uint32_t total = rd16(s + 19) ? rd16(s + 19) : rd32(s + 32);
    total_clusters = (total - (data_lba - part_lba)) / spc;
    cluster_bytes = spc * 512;
    for (int i = 0; i < 11; i++) volume_label[i] = s[71 + i];
    for (int i = 10; i >= 0 && volume_label[i] == ' '; i--) volume_label[i] = 0;
    volume_label[11] = 0;
    free_clusters = 0;
    for (uint32_t c = 2; c < total_clusters + 2; c++) if (fat_get(c) == 0) free_clusters++;
    mounted = 1;
    return 1;
}

int fs_mounted(void) { return mounted; }
uint32_t fs_total_kb(void) { return mounted ? total_clusters * (cluster_bytes / 512) / 2 : 0; }
uint32_t fs_free_kb(void) { return mounted ? free_clusters * (cluster_bytes / 512) / 2 : 0; }
const char *fs_label(void) { return volume_label; }

struct list_ctx { struct fs_entry *arr; int max, n; };
static int list_cb(struct dinfo *d, void *v) {
    struct list_ctx *l = v;
    if (d->name[0] == '.' && (!d->name[1] || (d->name[1] == '.' && !d->name[2]))) return 0;
    if (l->n >= l->max) return 1;
    struct fs_entry *e = &l->arr[l->n++];
    int k = 0; for (; d->name[k] && k < FS_NAME_MAX - 1; k++) e->name[k] = d->name[k]; e->name[k] = 0;
    e->is_dir = (d->raw[11] & ATTR_DIR) != 0;
    e->size = rd32(d->raw + 28);
    e->date = rd16(d->raw + 24); e->time = rd16(d->raw + 22);
    return 0;
}

/* List a folder: folders first, then files, each sorted by name. */
int fs_list(const char *path, struct fs_entry *arr, int max) {
    if (!mounted) return -1;
    uint32_t c = resolve_dir(path);
    if (!c) return -1;
    struct list_ctx l = { arr, max, 0 };
    dir_walk(c, list_cb, &l);
    for (int i = 1; i < l.n; i++)                       /* insertion sort */
        for (int j = i; j > 0; j--) {
            struct fs_entry *a = &arr[j - 1], *b = &arr[j];
            int swap = 0;
            if (a->is_dir != b->is_dir) swap = b->is_dir;
            else {
                const char *x = a->name, *y = b->name;
                while (*x && upc(*x) == upc(*y)) { x++; y++; }
                swap = upc(*x) > upc(*y);
            }
            if (!swap) break;
            struct fs_entry t = *a; *a = *b; *b = t;
        }
    return l.n;
}

/* 0 = missing, 1 = file, 2 = folder */
int fs_stat(const char *path, uint32_t *size) {
    if (!mounted) return 0;
    char leaf[FS_NAME_MAX];
    uint32_t dc = split(path, leaf);
    if (!leaf[0]) return resolve_dir(path) ? 2 : 0;
    if (!dc) return 0;
    struct dinfo d;
    if (!dir_find(dc, leaf, &d)) return 0;
    if (size) *size = rd32(d.raw + 28);
    return (d.raw[11] & ATTR_DIR) ? 2 : 1;
}

int fs_read(const char *path, void *buf, uint32_t max) {
    if (!mounted) return -1;
    char leaf[FS_NAME_MAX];
    uint32_t dc = split(path, leaf);
    struct dinfo d;
    if (!dc || !leaf[0] || !dir_find(dc, leaf, &d) || (d.raw[11] & ATTR_DIR)) return -1;
    uint32_t size = rd32(d.raw + 28), done = 0;
    if (size > max) size = max;
    uint8_t *cb = kmalloc(cluster_bytes);
    if (!cb) return -1;
    for (uint32_t c = ent_cluster(d.raw); c >= 2 && c < EOC && done < size; c = fat_get(c)) {
        if (!ata_read(clus_lba(c), spc, cb)) break;
        uint32_t n = size - done < cluster_bytes ? size - done : cluster_bytes;
        mcopy((uint8_t *)buf + done, cb, n);
        done += n;
    }
    kfree(cb);
    return (int)done;
}

/* Create or replace a file with the given contents. */
int fs_write(const char *path, const void *data, uint32_t len) {
    fs_generation++;
    if (!mounted) return 0;
    char leaf[FS_NAME_MAX];
    uint32_t dc = split(path, leaf);
    if (!dc || !valid_name(leaf)) return 0;
    struct dinfo d;
    int exists = dir_find(dc, leaf, &d);
    if (exists && (d.raw[11] & ATTR_DIR)) return 0;
    if (exists) { free_chain(ent_cluster(d.raw)); }

    /* write the data cluster by cluster */
    uint32_t first = 0, prev = 0, done = 0;
    uint8_t *cb = kmalloc(cluster_bytes);
    if (!cb) return 0;
    while (done < len) {
        uint32_t c = alloc_cluster(prev, 0);
        if (!c) { kfree(cb); return 0; }
        if (!first) first = c;
        uint32_t n = len - done < cluster_bytes ? len - done : cluster_bytes;
        mzero(cb, cluster_bytes);
        mcopy(cb, (const uint8_t *)data + done, n);
        if (!ata_write(clus_lba(c), spc, cb)) { kfree(cb); return 0; }
        done += n; prev = c;
    }
    kfree(cb);

    if (exists) {
        uint16_t dt, tm; timestamp(&dt, &tm);
        wr16(d.raw + 20, first >> 16); wr16(d.raw + 26, first & 0xFFFF);
        wr32(d.raw + 28, len);
        wr16(d.raw + 22, tm); wr16(d.raw + 24, dt); wr16(d.raw + 18, dt);
        return write_raw(d.lba, d.off, d.raw);
    }
    return create_entry(dc, leaf, 0x20, first, len, 0);
}

int fs_mkdir(const char *path) {
    fs_generation++;
    if (!mounted) return 0;
    char leaf[FS_NAME_MAX];
    uint32_t dc = split(path, leaf);
    struct dinfo d;
    if (!dc || !valid_name(leaf) || dir_find(dc, leaf, &d)) return 0;
    uint32_t c = alloc_cluster(0, 1);
    if (!c) return 0;
    /* every folder starts with "." (itself) and ".." (its parent) */
    uint8_t *buf = kmalloc(cluster_bytes);
    if (!buf) return 0;
    mzero(buf, cluster_bytes);
    uint16_t dt, tm; timestamp(&dt, &tm);
    for (int i = 0; i < 2; i++) {
        uint8_t *e = buf + i * 32;
        for (int j = 0; j < 11; j++) e[j] = ' ';
        e[0] = '.'; if (i) e[1] = '.';
        e[11] = ATTR_DIR;
        uint32_t cl = i ? (dc == root_clus ? 0 : dc) : c;
        wr16(e + 20, cl >> 16); wr16(e + 26, cl & 0xFFFF);
        wr16(e + 22, tm); wr16(e + 24, dt); wr16(e + 14, tm); wr16(e + 16, dt);
    }
    int ok = ata_write(clus_lba(c), spc, buf);
    kfree(buf);
    return ok && create_entry(dc, leaf, ATTR_DIR, c, 0, 0);
}

static int nonempty_cb(struct dinfo *d, void *v) {
    (void)v;
    return !(d->name[0] == '.' && (!d->name[1] || (d->name[1] == '.' && !d->name[2])));
}

/* Delete a file, or an empty folder. */
int fs_delete(const char *path) {
    fs_generation++;
    if (!mounted) return 0;
    char leaf[FS_NAME_MAX];
    uint32_t dc = split(path, leaf);
    struct dinfo d;
    if (!dc || !leaf[0] || !dir_find(dc, leaf, &d)) return 0;
    uint32_t c = ent_cluster(d.raw);
    if ((d.raw[11] & ATTR_DIR) && dir_walk(c, nonempty_cb, 0)) return -1;   /* not empty */
    uint8_t e[32];
    for (int i = 0; i < d.nlfn; i++) {
        uint8_t s[512];
        if (!ata_read(d.lfn_lba[i], 1, s)) return 0;
        s[d.lfn_off[i]] = 0xE5;
        if (!ata_write(d.lfn_lba[i], 1, s)) return 0;
    }
    mcopy(e, d.raw, 32);
    e[0] = 0xE5;
    if (!write_raw(d.lba, d.off, e)) return 0;
    free_chain(c);
    return 1;
}

/* Rename within the same folder (keeps the contents). */
int fs_rename(const char *path, const char *newname) {
    fs_generation++;
    if (!mounted || !valid_name(newname)) return 0;
    char leaf[FS_NAME_MAX];
    uint32_t dc = split(path, leaf);
    struct dinfo d, x;
    if (!dc || !dir_find(dc, leaf, &d) || dir_find(dc, newname, &x)) return 0;
    uint32_t first = ent_cluster(d.raw), size = rd32(d.raw + 28);
    uint8_t attr = d.raw[11];
    if (!create_entry(dc, newname, attr, first, size, 0)) return 0;
    /* remove the old entry without freeing its clusters */
    for (int i = 0; i < d.nlfn; i++) {
        uint8_t s[512];
        if (ata_read(d.lfn_lba[i], 1, s)) { s[d.lfn_off[i]] = 0xE5; ata_write(d.lfn_lba[i], 1, s); }
    }
    d.raw[0] = 0xE5;
    return write_raw(d.lba, d.off, d.raw);
}
