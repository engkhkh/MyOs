/* shell.c - The command shell and its character grid ("console").
 * The shell doesn't know how it is displayed: the desktop draws the grid inside
 * the Terminal window, and text mode copies it straight to the VGA screen. */
#include <stdint.h>
#include <stddef.h>
#include "kernel.h"

struct cell con_grid[CON_ROWS][CON_COLS];
int con_cx, con_cy;
volatile int con_dirty = 1;
static uint8_t cur_fg = 7, cur_bg = 0;

static char line[CON_COLS];
static int len;
static char history[8][CON_COLS];
static int hist_n, hist_pos;
static char cwd[FS_PATH_MAX] = "/";
static int shell_busy;                    /* a background command (ping...) is running */

/* ---------- string helpers ---------- */
int kstrcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
static int kstartswith(const char *s, const char *p) { while (*p) if (*s++ != *p++) return 0; return 1; }
static void kstrcpy(char *d, const char *s) { while ((*d++ = *s++)) {} }
static int katoi(const char *s) { int neg = 0, v = 0; if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++; while (*s >= '0' && *s <= '9') v = v * 10 + *s++ - '0'; return neg ? -v : v; }

/* ---------- grid output ---------- */
static void scroll(void) {
    for (int y = 1; y < CON_ROWS; y++)
        for (int x = 0; x < CON_COLS; x++) con_grid[y - 1][x] = con_grid[y][x];
    for (int x = 0; x < CON_COLS; x++) con_grid[CON_ROWS - 1][x] = (struct cell){ ' ', 7, 0 };
    con_cy = CON_ROWS - 1;
}
void con_putc(char c) {
    con_dirty = 1;
    if (c == '\n') { con_cx = 0; if (++con_cy >= CON_ROWS) scroll(); return; }
    if (c == '\b') { if (con_cx > 0) { con_cx--; con_grid[con_cy][con_cx] = (struct cell){ ' ', cur_fg, cur_bg }; } return; }
    con_grid[con_cy][con_cx] = (struct cell){ c, cur_fg, cur_bg };
    if (++con_cx >= CON_COLS) { con_cx = 0; if (++con_cy >= CON_ROWS) scroll(); }
}
void con_print(const char *s) { while (*s) con_putc(*s++); }
void con_num(uint32_t n) { char b[11]; int i = 10; b[i] = 0; if (!n) b[--i] = '0'; while (n) { b[--i] = '0' + n % 10; n /= 10; } con_print(&b[i]); }
static void con_color(uint8_t fg) { cur_fg = fg; }
static void two(uint32_t v) { con_putc('0' + v / 10 % 10); con_putc('0' + v % 10); }
static void clear(void) {
    for (int y = 0; y < CON_ROWS; y++) for (int x = 0; x < CON_COLS; x++) con_grid[y][x] = (struct cell){ ' ', 7, 0 };
    con_cx = con_cy = 0; con_dirty = 1;
}
/* ---------- paths ---------- */
/* Turn "docs\\..\\notes.txt" (relative to base) into a clean absolute "/notes.txt". */
void path_resolve(const char *base, const char *in, char *out) {
    char tmp[FS_PATH_MAX * 2];
    int n = 0;
    if ((in[0] == 'C' || in[0] == 'c') && in[1] == ':') in += 2;
    if (in[0] != '/' && in[0] != '\\') { for (const char *p = base; *p && n < FS_PATH_MAX; p++) tmp[n++] = *p; tmp[n++] = '/'; }
    for (const char *p = in; *p && n < (int)sizeof(tmp) - 1; p++) tmp[n++] = *p == '\\' ? '/' : *p;
    tmp[n] = 0;
    int o = 0;
    out[o++] = '/';
    for (int i = 0; tmp[i];) {
        while (tmp[i] == '/') i++;
        int st = i;
        while (tmp[i] && tmp[i] != '/') i++;
        int len = i - st;
        if (len == 0 || (len == 1 && tmp[st] == '.')) continue;
        if (len == 2 && tmp[st] == '.' && tmp[st + 1] == '.') {            /* go up one level */
            if (o > 1) { o--; while (o > 1 && out[o - 1] != '/') o--; if (o > 1) o--; }
            continue;
        }
        if (o > 1) out[o++] = '/';
        for (int k = 0; k < len && o < FS_PATH_MAX - 1; k++) out[o++] = tmp[st + k];
    }
    if (o > 1 && out[o - 1] == '/') o--;
    out[o] = 0;
}
void path_display(const char *path, char *out) {
    int o = 0;
    out[o++] = 'C'; out[o++] = ':';
    if (!path[0] || (path[0] == '/' && !path[1])) { out[o++] = '\\'; out[o] = 0; return; }
    for (; *path && o < FS_PATH_MAX - 1; path++) out[o++] = *path == '/' ? '\\' : *path;
    out[o] = 0;
}

static void prompt(void) { char d[FS_PATH_MAX]; path_display(cwd, d); con_color(10); con_print(d); con_print("> "); con_color(7); }
static void numpad(uint32_t v, int w) { char b[11]; int k = 0; do b[k++] = '0' + v % 10; while (v /= 10); while (w-- > k) con_putc(' '); while (k) con_putc(b[--k]); }
static int need_disk(void) { if (fs_mounted()) return 1; con_color(12); con_print("No disk. Start MyOS with a FAT32 disk attached (see README).\n"); con_color(7); return 0; }
/* split "a b" into first word (copied) and the rest */
static const char *arg2(const char *s, char *first) {
    int k = 0;
    while (*s == ' ') s++;
    if (*s == '"') { s++; while (*s && *s != '"' && k < FS_PATH_MAX - 1) first[k++] = *s++; if (*s == '"') s++; }
    else while (*s && *s != ' ' && k < FS_PATH_MAX - 1) first[k++] = *s++;
    first[k] = 0;
    while (*s == ' ') s++;
    return s;
}
/* A whole-line argument, with optional surrounding quotes removed. */
static const char *unquote(const char *s) {
    static char b[FS_PATH_MAX];
    while (*s == ' ') s++;
    int k = 0;
    for (; *s && k < FS_PATH_MAX - 1; s++) if (*s != '"') b[k++] = *s;
    while (k && b[k - 1] == ' ') k--;
    b[k] = 0;
    return b;
}

/* Find a program: as given, with ".elf" added, then in C:\\Apps. */
static int find_program(const char *name, char *out) {
    char tries[4][FS_PATH_MAX];
    int n = 0; while (name[n]) n++;
    path_resolve(cwd, name, tries[0]);
    int k = 0; while (tries[0][k]) { tries[1][k] = tries[0][k]; k++; }
    tries[1][k] = '.'; tries[1][k + 1] = 'e'; tries[1][k + 2] = 'l'; tries[1][k + 3] = 'f'; tries[1][k + 4] = 0;
    path_resolve("/Apps", name, tries[2]);
    k = 0; while (tries[2][k]) { tries[3][k] = tries[2][k]; k++; }
    tries[3][k] = '.'; tries[3][k + 1] = 'e'; tries[3][k + 2] = 'l'; tries[3][k + 3] = 'f'; tries[3][k + 4] = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t sz;
        if (fs_stat(tries[i], &sz) != 1) continue;
        int j = 0; while (tries[i][j]) j++;
        if (j < 4 || tries[i][j - 4] != '.' || (tries[i][j - 3] | 32) != 'e' || (tries[i][j - 2] | 32) != 'l' || (tries[i][j - 1] | 32) != 'f') continue;
        for (j = 0; tries[i][j]; j++) out[j] = tries[i][j];
        out[j] = 0;
        return 1;
    }
    return 0;
}

static void start_program(const char *path) {
    if (!gfx_available()) { con_print("Programs need graphics mode.\n"); return; }
    int pid = proc_spawn(path);
    if (pid > 0) {
        int k = 0, last = 0; for (; path[k]; k++) if (path[k] == '/') last = k + 1;
        con_print("Started "); con_print(path + last); con_print(" (PID "); con_num(pid); con_print(")\n");
    } else {
        con_color(12);
        con_print(pid == -1 ? "Program not found.\n" : pid == -2 ? "Too many programs running.\n" :
                  pid == -3 ? "Not enough memory.\n" : "That file is not a MyOS program.\n");
        con_color(7);
    }
}

static void cmd_ps(void) {
    con_color(14); con_print(" PID  NAME              STATUS             CPU  MEMORY\n"); con_color(7);
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->state == T_FREE) continue;
        numpad(t->pid, 4); con_print("  ");
        const char *nm = i ? t->name : "System";
        int n = 0; while (nm[n]) n++;
        con_print(nm); while (n++ < 18) con_putc(' ');
        const char *st = i == 0 ? "Running" : t->state == T_READY ? "Running" : t->state == T_WAIT_KEY ? "Waiting for keys" : t->state == T_WAIT_EVENT ? "Waiting for input" :
                         t->state == T_SLEEP ? "Sleeping" : "Ending";
        n = 0; while (st[n]) n++;
        con_print(st); while (n++ < 17) con_putc(' ');
        numpad(t->cpu_pct, 3); con_print("%  ");
        uint32_t kb = i ? t->pages * 4 + (uint32_t)(t->cw * t->ch * 4) / 1024 + 16 : 0;
        if (i) { numpad(kb, 5); con_print(" KB"); } else con_print("    -");
        con_putc('\n');
    }
}

static void cmd_dir(const char *arg) {
    if (!need_disk()) return;
    arg = unquote(arg);
    char p[FS_PATH_MAX], d[FS_PATH_MAX];
    path_resolve(cwd, *arg ? arg : ".", p);
    static struct fs_entry list[128];
    int n = fs_list(p, list, 128);
    if (n < 0) { con_print("Folder not found.\n"); return; }
    path_display(p, d);
    con_print(" Folder of "); con_print(d); con_print("\n\n");
    uint32_t total = 0; int files = 0, dirs = 0;
    for (int i = 0; i < n; i++) {
        struct fs_entry *e = &list[i];
        int day = e->date & 31, mon = (e->date >> 5) & 15, yr = 1980 + (e->date >> 9);
        two(day); con_putc('/'); two(mon); con_putc('/'); con_num(yr); con_print("  ");
        two(e->time >> 11); con_putc(':'); two((e->time >> 5) & 63); con_print("  ");
        if (e->is_dir) { con_color(14); con_print("<DIR>     "); con_color(7); dirs++; }
        else { numpad(e->size, 9); con_putc(' '); total += e->size; files++; }
        con_putc(' ');
        if (e->is_dir) con_color(11);
        con_print(e->name); con_color(7); con_putc('\n');
    }
    con_print("    "); con_num(files); con_print(" file(s), "); con_num(total); con_print(" bytes; ");
    con_num(dirs); con_print(" folder(s), "); con_num(fs_free_kb() / 1024); con_print(" MB free\n");
}

static void cmd_type(const char *arg) {
    if (!need_disk()) return;
    arg = unquote(arg);
    char p[FS_PATH_MAX]; path_resolve(cwd, arg, p);
    uint32_t size;
    if (fs_stat(p, &size) != 1) { con_print("File not found.\n"); return; }
    if (size > 16384) size = 16384;
    char *buf = kmalloc(size + 1);
    if (!buf) return;
    int n = fs_read(p, buf, size);
    for (int i = 0; i < n; i++) if (buf[i] != '\r') con_putc(buf[i] == '\t' ? ' ' : buf[i]);
    if (n > 0 && buf[n - 1] != '\n') con_putc('\n');
    kfree(buf);
}

static void cmd_write(const char *arg, int append) {
    if (!need_disk()) return;
    char name[FS_PATH_MAX], p[FS_PATH_MAX];
    const char *text = arg2(arg, name);
    if (!name[0]) { con_print("Usage: write <file> <text>\n"); return; }
    path_resolve(cwd, name, p);
    uint32_t old = 0;
    int st = fs_stat(p, &old);
    if (st == 2) { con_print("That is a folder.\n"); return; }
    if (!append || st != 1) old = 0;
    int tl = 0; while (text[tl]) tl++;
    char *buf = kmalloc(old + tl + 2);
    if (!buf) return;
    if (old) fs_read(p, buf, old);
    for (int i = 0; i < tl; i++) buf[old + i] = text[i];
    buf[old + tl] = '\n';
    con_print(fs_write(p, buf, old + tl + 1) ? "Saved.\n" : "Could not write the file.\n");
    kfree(buf);
}

static void cmd_copy(const char *arg) {
    if (!need_disk()) return;
    char a[FS_PATH_MAX], b[FS_PATH_MAX], pa[FS_PATH_MAX], pb[FS_PATH_MAX];
    const char *rest = arg2(arg, a); arg2(rest, b);
    if (!a[0] || !b[0]) { con_print("Usage: copy <from> <to>\n"); return; }
    path_resolve(cwd, a, pa); path_resolve(cwd, b, pb);
    uint32_t size;
    if (fs_stat(pa, &size) != 1) { con_print("File not found.\n"); return; }
    if (fs_stat(pb, 0) == 2) {                                   /* copy into a folder */
        int k = 0, last = 0; for (; pa[k]; k++) if (pa[k] == '/') last = k + 1;
        char t[FS_PATH_MAX]; path_resolve(pb, pa + last, t);
        for (k = 0; t[k]; k++) pb[k] = t[k];
        pb[k] = 0;
    }
    char *buf = kmalloc(size + 1);
    if (!buf) { con_print("File too big.\n"); return; }
    int n = fs_read(pa, buf, size);
    con_print(n >= 0 && fs_write(pb, buf, n) ? "1 file copied.\n" : "Copy failed.\n");
    kfree(buf);
}

/* ---------- commands ---------- */
static void shutdown(void) {
    __asm__ volatile("outw %0,%1" :: "a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));  /* QEMU */
    __asm__ volatile("outw %0,%1" :: "a"((uint16_t)0x2000), "Nd"((uint16_t)0xB004)); /* Bochs */
    con_print("It is now safe to turn off your computer.\n");
}

static void run(const char *cmd) {
    while (*cmd == ' ') cmd++;
    if (!*cmd) return;
    if (!kstrcmp(cmd, "help")) {
        con_color(14); con_print("System:  "); con_color(7); con_print("about, mem, memtest, uptime, time, tz <hours>\n");
        con_color(14); con_print("Files:   "); con_color(7); con_print("dir, cd <dir>, type <file>, write <file> <text>,\n");
        con_print("         append <file> <text>, copy <a> <b>, ren <a> <name>,\n");
        con_print("         mkdir <name>, del <name>, disk\n");
        con_color(14); con_print("Screen:  "); con_color(7); con_print("clear, color, echo <text>\n");
        con_color(14); con_print("Apps:    "); con_color(7); con_print("notepad [file], explorer [dir], open <app>\n");
        con_color(14); con_print("Programs:"); con_color(7); con_print(" run <name> (or just type its name), ps, kill <pid>,\n");
        con_print("         taskmgr, settings.  Try: paint, snake, mines, hello, bounce,\n         guess, spin, crash\n");
        con_color(14); con_print("Network: "); con_color(7); con_print("ipconfig, ping <host>, nslookup <host>, web [url]\n");
        con_color(14); con_print("Sound:   "); con_color(7); con_print("beep, tune, volume [0-100], music, viewer\n");
        con_color(14); con_print("Tests:   "); con_color(7); con_print("beep, bsod, pagefault  (crash the kernel on purpose)\n");
        con_color(14); con_print("Power:   "); con_color(7); con_print("reboot, shutdown\n");
    } else if (!kstrcmp(cmd, "about") || !kstrcmp(cmd, "ver")) {
        con_print("MyOS 2.6 - 32-bit x86. Settings, networking, sound, multitasking,\n");
        con_print("FAT32 disk, graphics desktop, mouse, windows. Built from scratch.\n");
    } else if (!kstrcmp(cmd, "mem")) {
        struct mm_stats st; mm_stats(&st);
        con_print("Physical RAM: "); con_num(st.total_kb / 1024); con_print(" MB total, ");
        con_num(st.used_kb / 1024); con_print(" MB used, "); con_num(st.free_kb / 1024); con_print(" MB free\n");
        con_print("Kernel heap:  "); con_num(st.heap_mapped_kb); con_print(" KB mapped, ");
        con_num(st.heap_used / 1024); con_print(" KB used, "); con_num(st.heap_blocks); con_print(" blocks\n");
        if (gfx_available()) { con_print("Screen:       "); con_num(gfx_w); con_putc('x'); con_num(gfx_h); con_print(" x 32-bit color\n"); }
    } else if (!kstrcmp(cmd, "memtest")) {
        enum { N = 64 };
        uint32_t *blk[N]; uint32_t ok = 1, bytes = 0;
        for (int i = 0; i < N; i++) {
            uint32_t sz = 1024 + i * 1024;
            blk[i] = kmalloc(sz);
            if (!blk[i]) { ok = 0; for (int k = 0; k < i; k++) kfree(blk[k]); con_print("Out of memory!\n"); return; }
            for (uint32_t j = 0; j < sz / 4; j++) blk[i][j] = i * 0x01010101u ^ j;
            bytes += sz;
        }
        for (int i = 0; ok && i < N; i++)
            for (uint32_t j = 0; j < (1024u + i * 1024u) / 4; j++)
                if (blk[i][j] != (i * 0x01010101u ^ j)) { ok = 0; break; }
        for (int i = 0; i < N; i++) kfree(blk[i]);
        con_print("Allocated "); con_num(bytes / 1024); con_print(" KB in "); con_num(N); con_print(" blocks. ");
        con_color(ok ? 10 : 12); con_print(ok ? "Data check PASSED" : "Data check FAILED"); con_color(7); con_print(", all freed.\n");
    } else if (!kstrcmp(cmd, "pagefault")) {
        con_print("Writing to unmapped address 0xBAD00000...\n");
        *(volatile uint32_t *)0xBAD00000 = 1;
    } else if (!kstrcmp(cmd, "time")) {
        int h, m, s; rtc_time(&h, &m, &s);
        con_print("Time: "); two(h); con_putc(':'); two(m); con_putc(':'); two(s);
        con_print("  (UTC"); int tz = rtc_tz(); if (tz) { con_putc(tz > 0 ? '+' : '-'); con_num(tz > 0 ? tz : -tz); } con_print(")\n");
    } else if (kstartswith(cmd, "tz")) {
        if (cmd[2] == ' ') { rtc_set_tz(katoi(cmd + 3)); cfg.timezone = rtc_tz(); settings_save(); }
        int tz = rtc_tz();
        con_print("Time zone is UTC"); if (tz) { con_putc(tz > 0 ? '+' : '-'); con_num(tz > 0 ? tz : -tz); } con_putc('\n');
    } else if (kstartswith(cmd, "echo ")) {
        con_print(cmd + 5); con_putc('\n');
    } else if (!kstrcmp(cmd, "clear") || !kstrcmp(cmd, "cls")) {
        clear();
    } else if (!kstrcmp(cmd, "color")) {
        for (int i = 0; i < 16; i++) { cur_bg = i; con_print("  "); }
        cur_bg = 0; con_putc('\n');
    } else if (!kstrcmp(cmd, "uptime")) {
        uint32_t sec = ticks / TIMER_HZ;
        con_print("Up "); con_num(sec / 3600); con_print(" h "); con_num(sec / 60 % 60); con_print(" min ");
        con_num(sec % 60); con_print(" s\n");
    } else if (!kstrcmp(cmd, "beep")) {
        sound_tone(880, 200);
        con_print("Beep!\n");
    } else if (!kstrcmp(cmd, "tune")) {                         /* a little original melody */
        static const uint16_t notes[] = { 392, 523, 659, 784, 659, 523, 587, 698, 880, 784, 0, 784, 1047 };
        static const uint16_t lens[]  = { 150, 150, 150, 300, 150, 150, 150, 150, 150, 300, 80, 150, 450 };
        for (unsigned i = 0; i < sizeof notes / sizeof notes[0]; i++) sound_tone(notes[i], lens[i]);
        con_print("Playing a tune on the PC speaker...\n");
    } else if (kstartswith(cmd, "volume")) {
        if (cmd[6] == ' ') { sound_set_volume(katoi(cmd + 7)); cfg.volume = sound_get_volume(); settings_save(); }
        con_print("Volume: "); con_num(sound_get_volume()); con_print("%");
        con_print(sound_card_present() ? "  (Sound Blaster 16 found)\n" : "  (no sound card: PC speaker only)\n");
    } else if (!kstrcmp(cmd, "bsod")) {                       /* crash the kernel on purpose */
        __asm__ volatile("xor %%ecx,%%ecx; mov $1,%%eax; xor %%edx,%%edx; div %%ecx" ::: "eax","ecx","edx");
    } else if (kstartswith(cmd, "open ")) {
        if (!gfx_available()) con_print("Windows need graphics mode.\n");
        else if (!wm_open(cmd + 5)) { con_print("No app called '"); con_print(cmd + 5); con_print("'.\n"); }
    } else if (!kstrcmp(cmd, "dir") || !kstrcmp(cmd, "ls") || kstartswith(cmd, "dir ") || kstartswith(cmd, "ls ")) {
        cmd_dir(cmd[0] == 'd' ? cmd + 3 + (cmd[3] == ' ') : cmd + 2 + (cmd[2] == ' '));
    } else if (!kstrcmp(cmd, "cd") || kstartswith(cmd, "cd ") || kstartswith(cmd, "cd\\") || kstartswith(cmd, "cd..")) {
        if (!need_disk()) return;
        const char *a = unquote(cmd + 2);
        if (!*a) { char d[FS_PATH_MAX]; path_display(cwd, d); con_print(d); con_putc('\n'); return; }
        char p[FS_PATH_MAX]; path_resolve(cwd, a, p);
        if (fs_stat(p, 0) == 2) fs_canonical(p, cwd);
        else con_print("Folder not found.\n");
    } else if (kstartswith(cmd, "type ") || kstartswith(cmd, "cat ")) {
        cmd_type(cmd + (cmd[0] == 't' ? 5 : 4));
    } else if (kstartswith(cmd, "write ")) {
        cmd_write(cmd + 6, 0);
    } else if (kstartswith(cmd, "append ")) {
        cmd_write(cmd + 7, 1);
    } else if (kstartswith(cmd, "copy ")) {
        cmd_copy(cmd + 5);
    } else if (kstartswith(cmd, "mkdir ") || kstartswith(cmd, "md ")) {
        if (!need_disk()) return;
        char p[FS_PATH_MAX]; path_resolve(cwd, unquote(cmd + (cmd[0] == 'm' && cmd[1] == 'k' ? 6 : 3)), p);
        con_print(fs_mkdir(p) ? "Folder created.\n" : "Could not create folder (exists or bad name?).\n");
    } else if (kstartswith(cmd, "del ") || kstartswith(cmd, "rm ") || kstartswith(cmd, "rmdir ")) {
        if (!need_disk()) return;
        const char *a = cmd + (cmd[0] == 'd' ? 4 : cmd[2] == ' ' ? 3 : 6);
        char p[FS_PATH_MAX]; path_resolve(cwd, unquote(a), p);
        int r = fs_delete(p);
        con_print(r == 1 ? "Deleted.\n" : r == -1 ? "Folder is not empty.\n" : "Not found.\n");
    } else if (kstartswith(cmd, "ren ")) {
        if (!need_disk()) return;
        char a[FS_PATH_MAX], p[FS_PATH_MAX];
        const char *nn = arg2(cmd + 4, a);
        path_resolve(cwd, a, p);
        con_print(*nn && fs_rename(p, unquote(nn)) ? "Renamed.\n" : "Usage: ren <file> <newname>  (target must not exist)\n");
    } else if (!kstrcmp(cmd, "disk")) {
        if (!ata_present()) { con_print("No hard disk found.\n"); return; }
        con_print("Disk:   "); con_print(ata_model()); con_print(", "); con_num(ata_sectors() / 2048); con_print(" MB\n");
        if (!fs_mounted()) { con_print("Format: not FAT32 (cannot open)\n"); return; }
        con_print("Drive C: FAT32"); if (fs_label()[0]) { con_print(", label "); con_print(fs_label()); }
        con_print("\n        "); con_num(fs_free_kb() / 1024); con_print(" MB free of "); con_num(fs_total_kb() / 1024); con_print(" MB\n");
    } else if (!kstrcmp(cmd, "notepad") || kstartswith(cmd, "notepad ")) {
        if (!gfx_available()) { con_print("Notepad needs graphics mode.\n"); return; }
        if (cmd[7]) { char p[FS_PATH_MAX]; path_resolve(cwd, unquote(cmd + 8), p); wm_open_file(p); } else wm_open("notepad");
    } else if (!kstrcmp(cmd, "explorer") || kstartswith(cmd, "explorer ")) {
        if (!gfx_available()) { con_print("Explorer needs graphics mode.\n"); return; }
        char p[FS_PATH_MAX]; path_resolve(cwd, cmd[8] ? unquote(cmd + 9) : ".", p); wm_open_folder(p);
    } else if (!kstrcmp(cmd, "reboot")) {
        reboot();
    } else if (!kstrcmp(cmd, "shutdown")) {
        shutdown();
    } else if (kstartswith(cmd, "run ")) {
        if (!need_disk()) return;
        char p[FS_PATH_MAX];
        if (find_program(unquote(cmd + 4), p)) start_program(p); else con_print("Program not found.\n");
    } else if (!kstrcmp(cmd, "ps") || !kstrcmp(cmd, "tasklist")) {
        cmd_ps();
    } else if (kstartswith(cmd, "kill ")) {
        int pid = katoi(cmd + 5);
        if (pid == 0) con_print("The kernel can't be ended.\n");
        else con_print(proc_kill(pid) ? "Ended.\n" : "No such process.\n");
    } else if (!kstrcmp(cmd, "ipconfig") || !kstrcmp(cmd, "ifconfig")) {
        if (!netcfg.present) { con_print("No network card found (MyOS supports the Intel e1000).\n"); return; }
        char b[20];
        con_color(14); con_print("Ethernet adapter (Intel e1000)\n"); con_color(7);
        con_print("  MAC address . . : ");
        for (int i = 0; i < 6; i++) { const char *hx = "0123456789ABCDEF"; con_putc(hx[netcfg.mac[i] >> 4]); con_putc(hx[netcfg.mac[i] & 15]); if (i < 5) con_putc('-'); }
        con_putc('\n');
        if (!netcfg.ip) { con_print("  Waiting for an address from DHCP...\n"); return; }
        net_ip_text(netcfg.ip, b);      con_print("  IPv4 address  . : "); con_print(b); con_putc('\n');
        net_ip_text(netcfg.mask, b);    con_print("  Subnet mask . . : "); con_print(b); con_putc('\n');
        net_ip_text(netcfg.gateway, b); con_print("  Default gateway : "); con_print(b); con_putc('\n');
        net_ip_text(netcfg.dns, b);     con_print("  DNS server  . . : "); con_print(b); con_putc('\n');
        con_print("  Packets . . . . : "); con_num(netcfg.tx_packets); con_print(" sent, "); con_num(netcfg.rx_packets); con_print(" received\n");
    } else if (kstartswith(cmd, "ping ") || kstartswith(cmd, "nslookup ")) {
        int is_ping = cmd[0] == 'p';
        const char *host = unquote(cmd + (is_ping ? 5 : 9));
        if (!netcfg.ip) { con_print(netcfg.present ? "Not connected yet (no DHCP address).\n" : "No network card found.\n"); return; }
        if (net_job_start(is_ping ? 1 : 2, host)) shell_busy = 1;
    } else if (!kstrcmp(cmd, "gfxbench")) {
        if (!gfx_available()) return;
        uint32_t v = wm_bench(20);
        con_print("Full-screen redraw: "); con_num(v / 100); con_putc('.'); con_putc('0' + v / 10 % 10); con_putc('0' + v % 10);
        con_print(" ms per frame\n");
    } else if (!kstrcmp(cmd, "settings")) {
        if (gfx_available()) wm_open("settings");
    } else if (!kstrcmp(cmd, "taskmgr")) {
        if (gfx_available()) wm_open("taskmgr");
    } else {
        /* typed a program name, maybe followed by a file: "viewer painting.bmp" */
        char p[FS_PATH_MAX], first[FS_PATH_MAX];
        const char *rest = arg2(cmd, first);
        if (fs_mounted() && find_program(first, p)) {
            char argp[FS_PATH_MAX] = "";
            if (*rest) { path_resolve(cwd, unquote(rest), argp); if (fs_stat(argp, 0)) { char c2[FS_PATH_MAX]; if (fs_canonical(argp, c2)) { int k = 0; for (; c2[k]; k++) argp[k] = c2[k]; argp[k] = 0; } } else { int k = 0; const char *u = unquote(rest); for (; u[k] && k < FS_PATH_MAX - 1; k++) argp[k] = u[k]; argp[k] = 0; } }
            if (!gfx_available()) { con_print("Programs need graphics mode.\n"); return; }
            int pid = proc_spawn_arg(p, argp);
            if (pid > 0) { con_print("Started "); con_print(first); con_print(" (PID "); con_num(pid); con_print(")\n"); }
            else con_print("Could not start the program.\n");
            return;
        }
        con_color(12); con_print("'"); con_print(cmd); con_print("' is not a command."); con_color(7); con_print(" Try 'help'.\n");
    }
}

/* ---------- input ---------- */
static void set_line(const char *s) {
    while (len) { len--; con_putc('\b'); }
    for (; *s && len < CON_COLS - 8; s++) { line[len++] = *s; con_putc(*s); }
}

void shell_job_done(void) { shell_busy = 0; prompt(); con_dirty = 1; }

void con_key(int k) {
    con_dirty = 1;
    if (shell_busy) { if (k == KEY_CTRL('c')) net_job_cancel(); return; }   /* Ctrl+C stops ping */
    if (k == '\n') {
        con_putc('\n');
        line[len] = 0;
        if (len) { kstrcpy(history[hist_n % 8], line); hist_n++; }
        hist_pos = hist_n;
        run(line);
        len = 0;
        if (!shell_busy) prompt();
    } else if (k == '\b') {
        if (len) { len--; con_putc('\b'); }
    } else if (k == KEY_UP) {
        if (hist_pos > 0 && hist_pos > hist_n - 8) { hist_pos--; set_line(history[hist_pos % 8]); }
    } else if (k == KEY_DOWN) {
        if (hist_pos < hist_n) { hist_pos++; set_line(hist_pos == hist_n ? "" : history[hist_pos % 8]); }
    } else if (k >= 32 && k < 127 && len < CON_COLS - 8) {
        line[len++] = (char)k; con_putc((char)k);
    }
}

void con_init(void) {
    clear();
    con_color(11); con_print("MyOS [Version 2.6]\n"); con_color(7);
    con_print("Type 'help' for commands. Up/Down arrows recall history.\n");
    if (fs_mounted()) { con_print("Drive C: ready, "); con_num(fs_free_kb() / 1024); con_print(" MB free.\n\n"); }
    else con_print("No FAT32 disk found: file commands are unavailable.\n\n");
    prompt();
}

/* Text-mode fallback: copy the grid onto the 80x25 VGA text screen. */
void con_render_text(void) {
    volatile uint16_t *vga = (uint16_t *)0xB8000;
    for (int y = 0; y < 25; y++)
        for (int x = 0; x < 80; x++) {
            struct cell c = (y < CON_ROWS && x < CON_COLS) ? con_grid[y][x] : (struct cell){ ' ', 7, 0 };
            vga[y * 80 + x] = (uint16_t)(uint8_t)c.ch | (uint16_t)((c.bg << 4 | c.fg) << 8);
        }
    if (con_cy < 25 && con_cx < 80) vga[con_cy * 80 + con_cx] = '_' | 0x0700;
    con_dirty = 0;
}
