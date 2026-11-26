/* proc.c - Processes, scheduler, program loader and system calls.
 *
 * Task 0 is the kernel itself ("System"): it runs the desktop, shell and disk.
 * Tasks 1..MAX_TASKS-1 are user programs loaded from disk. Each program has:
 *   - its own address space (page directory) with code at 0x40000000,
 *   - its own kernel stack (used while it is interrupted or in a system call),
 *   - a window canvas it draws into through system calls.
 *
 * Scheduling is preemptive round-robin: the 100 Hz timer interrupt switches to
 * the next ready task, so a program stuck in an endless loop can't freeze the PC.
 * To keep the kernel simple and safe, kernel code is never interrupted midway:
 * task 0 is only switched away from while it is idle (or when it yields), and
 * system calls run with interrupts off.
 *
 * Programs talk to the kernel with "int 0x80": EAX = call number, EBX, ECX, EDX,
 * ESI, EDI = arguments, result back in EAX. Every pointer a program passes is
 * checked against its page tables, so a bad pointer can only hurt the program.
 */
#include <stdint.h>
#include <stddef.h>
#include "kernel.h"
#include "font.h"

#define KSTACK_SIZE 16384
#define USTACK_SIZE (64 * 1024)

struct task tasks[MAX_TASKS];
static int cur;
static int next_pid = 1;
volatile int kernel_idle;

static void kcopy(void *d, const void *s, uint32_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; }
static void kzero(void *d, uint32_t n) { uint8_t *a = d; while (n--) *a++ = 0; }
static void kstrncpy(char *d, const char *s, int n) { int i = 0; for (; s[i] && i < n - 1; i++) d[i] = s[i]; d[i] = 0; }

/* The floating-point unit (FPU): programs may use it (e.g. the Calculator), so
 * each task keeps its own copy of the FPU registers, swapped on every switch. */
static void fpu_init(void) {
    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 = (cr0 & ~(1u << 2)) | (1u << 1) | (1u << 5);     /* FPU present (EM=0), MP=1, native errors (NE=1) */
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));
    __asm__ volatile("fninit");
}
static void fpu_fresh_state(uint8_t *area) {             /* a clean FPU state for a new task */
    __asm__ volatile("fninit; fnsave (%0)" :: "r"(area) : "memory");
}

void proc_init(void) {
    kzero(tasks, sizeof tasks);
    fpu_init();
    fpu_fresh_state(tasks[0].fpu);
    tasks[0].state = T_READY;
    tasks[0].pd = vmm_kernel_dir();
    kstrncpy(tasks[0].name, "System", sizeof tasks[0].name);
    cur = 0;
}

int proc_current(void) { return cur; }

/* ---------- scheduling ---------- */
void proc_save(struct regs *r) { tasks[cur].esp = (uint32_t)r; }

static void switch_to(int n) {
    if (n != cur) {
        __asm__ volatile("fnsave (%0)" :: "r"(tasks[cur].fpu) : "memory");   /* park this task's FPU registers */
        __asm__ volatile("frstor (%0)" :: "r"(tasks[n].fpu) : "memory");    /* and bring back the next one's */
    }
    cur = n;
    if (tasks[n].is_user) tss_set_kernel_stack(tasks[n].kstack + KSTACK_SIZE);
    vmm_switch(tasks[n].pd);
}

/* Pick the next ready task after the current one (task 0 is always ready). */
static uint32_t schedule(void) {
    int n = 0;
    for (int i = 1; i <= MAX_TASKS; i++) {
        int c = (cur + i) % MAX_TASKS;
        if (tasks[c].state == T_READY) { n = c; break; }
    }
    switch_to(n);
    return tasks[cur].esp;
}

uint32_t proc_tick(struct regs *r) {
    if (cur != 0 || !kernel_idle) tasks[cur].cpu_ticks++;
    for (int i = 1; i < MAX_TASKS; i++)
        if (tasks[i].state == T_SLEEP && (int32_t)(ticks - tasks[i].wake) >= 0) tasks[i].state = T_READY;
    if (cur == 0 && !kernel_idle) return (uint32_t)r;    /* never interrupt kernel work */
    if (cur != 0 && (r->cs & 3) != 3) return (uint32_t)r;
    return schedule();
}

int proc_any_ready(void) {
    for (int i = 1; i < MAX_TASKS; i++) if (tasks[i].state == T_READY) return 1;
    return 0;
}

/* Called by task 0 to give the CPU to programs for a while. */
void proc_yield(void) { __asm__ volatile("int $0x80" :: "a"(SYS_YIELD) : "memory"); }

/* ---------- canvas drawing (the program's window contents) ---------- */
static void cv_fill(struct task *t, int x, int y, int w, int h, uint32_t c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > t->cw) w = t->cw - x;
    if (y + h > t->ch) h = t->ch - y;
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) t->canvas[(y + j) * t->cw + x + i] = c;
}
static void cv_char(struct task *t, int x, int y, char ch, uint32_t fg, int bg_on, uint32_t bg) {
    const uint8_t *g = font_regular[(uint8_t)ch & 127];
    for (int j = 0; j < 16; j++)
        for (int i = 0; i < 8; i++) {
            int px = x + i, py = y + j;
            if (px < 0 || py < 0 || px >= t->cw || py >= t->ch) continue;
            if (g[j] & (0x80 >> i)) t->canvas[py * t->cw + px] = fg;
            else if (bg_on) t->canvas[py * t->cw + px] = bg;
        }
}
/* Big text: each font pixel becomes a scale x scale block (bold font when scaled). */
static void cv_char_big(struct task *t, int x, int y, char ch, uint32_t fg, int scale) {
    const uint8_t *g = (scale > 1 ? font_bold : font_regular)[(uint8_t)ch & 127];
    for (int j = 0; j < 16; j++)
        for (int i = 0; i < 8; i++)
            if (g[j] & (0x80 >> i)) cv_fill(t, x + i * scale, y + j * scale, scale, scale, fg);
}
static void cv_scroll(struct task *t) {
    int rowpix = 16 * t->cw;
    for (int i = 0; i < t->cw * t->ch - rowpix; i++) t->canvas[i] = t->canvas[i + rowpix];
    cv_fill(t, 0, t->ch - 16, t->cw, 16, t->bg);
}
static void cv_putc(struct task *t, char c) {
    int cols = (t->cw - 8) / 8, rows = (t->ch - 8) / 16;
    if (c == '\r') return;
    if (c == '\n') { t->tx = 0; t->ty++; }
    else if (c == '\b') { if (t->tx > 0) { t->tx--; cv_fill(t, 4 + t->tx * 8, 4 + t->ty * 16, 8, 16, t->bg); } return; }
    else {
        if (c == '\t') c = ' ';
        cv_char(t, 4 + t->tx * 8, 4 + t->ty * 16, c, t->fg, 1, t->bg);
        if (++t->tx >= cols) { t->tx = 0; t->ty++; }
    }
    if (t->ty >= rows) { cv_scroll(t); t->ty = rows - 1; }
}

/* ---------- creating and destroying programs ---------- */
struct elf_hdr { uint8_t ident[16]; uint16_t type, machine; uint32_t version, entry, phoff, shoff, flags;
                 uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } __attribute__((packed));
struct elf_ph  { uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align; } __attribute__((packed));

static int map_range(struct task *t, uint32_t start, uint32_t end, int zero) {
    for (uint32_t p = start & ~0xFFFu; p < end; p += 4096) {
        if (vmm_user_ok(t->pd, p, 1)) continue;
        uint32_t f = pmm_alloc();
        if (!f || !vmm_map_user(t->pd, p, f)) return 0;
        t->pages++;
        if (zero) kzero((void *)p, 4096);       /* never hand out another program's old data */
    }
    return 1;
}

static void free_task(int i) {
    struct task *t = &tasks[i];
    sound_stop_owner(t->pid);
    net_close_owner(t->pid);
    if (t->pd) vmm_free_space(t->pd);
    if (t->kstack) kfree((void *)t->kstack);
    if (t->canvas) kfree(t->canvas);
    t->pd = 0; t->kstack = 0; t->canvas = 0;
    t->state = T_FREE;
    wm_proc_closed(i);
}

/* Load an ELF program from disk and start it. Returns its PID, or a negative error. */
int proc_spawn(const char *path) { return proc_spawn_arg(path, ""); }

int proc_spawn_arg(const char *path, const char *arg) {
    uint32_t size;
    if (fs_stat(path, &size) != 1) return -1;                      /* not found */
    int slot = -1;
    for (int i = 1; i < MAX_TASKS; i++) if (tasks[i].state == T_FREE) { slot = i; break; }
    if (slot < 0) return -2;                                        /* too many programs */
    uint8_t *file = kmalloc(size + 1);
    if (!file) return -3;
    if (fs_read(path, file, size) != (int)size) { kfree(file); return -3; }

    struct elf_hdr *eh = (struct elf_hdr *)file;
    if (size < sizeof *eh || eh->ident[0] != 0x7F || eh->ident[1] != 'E' || eh->ident[2] != 'L' || eh->ident[3] != 'F'
        || eh->ident[4] != 1 || eh->machine != 3 || eh->type != 2 || eh->phoff + eh->phnum * sizeof(struct elf_ph) > size) {
        kfree(file); return -4;                                     /* not a 32-bit x86 program */
    }

    struct task *t = &tasks[slot];
    kzero(t, sizeof *t);
    t->pd = vmm_new_space();
    t->kstack = (uint32_t)kmalloc(KSTACK_SIZE);
    t->cw = PROC_CANVAS_W; t->ch = PROC_CANVAS_H;
    t->canvas = kmalloc(t->cw * t->ch * 4);
    if (!t->pd || !t->kstack || !t->canvas) { free_task(slot); kfree(file); return -3; }

    /* Map and fill each loadable segment. We briefly switch to the program's
     * address space (interrupts off) so we can copy straight into its pages. */
    uint32_t flags;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags));
    int ok = 1;
    struct elf_ph *ph = (struct elf_ph *)(file + eh->phoff);
    for (int i = 0; i < eh->phnum && ok; i++) {
        if (ph[i].type != 1 || !ph[i].memsz) continue;              /* PT_LOAD only */
        uint32_t va = ph[i].vaddr, end = va + ph[i].memsz;
        if (va < USER_BASE || end > USER_TOP - USTACK_SIZE || end < va || ph[i].filesz > ph[i].memsz
            || ph[i].offset + ph[i].filesz > size) { ok = 0; break; }
        if (!map_range(t, va, end, 0)) { ok = 0; break; }
        if (end > t->brk_base) t->brk_base = end;
        vmm_switch(t->pd);
        kzero((void *)va, ph[i].memsz);
        kcopy((void *)va, file + ph[i].offset, ph[i].filesz);
        vmm_switch(vmm_kernel_dir());
    }
    if (ok) { vmm_switch(t->pd); ok = map_range(t, USER_TOP - USTACK_SIZE, USER_TOP, 1); vmm_switch(vmm_kernel_dir()); }  /* stack */
    t->brk_base = t->brk = (t->brk_base + 0xFFF) & ~0xFFFu;           /* heap starts after the program */
    if (eh->entry < USER_BASE || eh->entry >= USER_TOP) ok = 0;
    uint32_t entry = eh->entry;
    __asm__ volatile("push %0; popf" :: "r"(flags));
    kfree(file);
    if (!ok) { free_task(slot); return -4; }

    /* Build the stack frame that isr_common will "return" into: ring 3, at the entry point. */
    struct regs *r = (struct regs *)(t->kstack + KSTACK_SIZE - sizeof(struct regs));
    kzero(r, sizeof *r);
    r->gs = r->fs = r->es = r->ds = 0x23;
    r->eip = entry; r->cs = 0x1B; r->eflags = 0x202;              /* interrupts on */
    r->useresp = USER_TOP - 16; r->ss = 0x23;
    t->esp = (uint32_t)r;
    t->is_user = 1;
    t->pid = next_pid++;
    t->resizable = 0; t->min_w = 160; t->min_h = 100;
    t->start_tick = ticks;
    t->fg = 0xD4D4D4; t->bg = 0x111827;
    for (int i = 0; i < t->cw * t->ch; i++) t->canvas[i] = t->bg;
    /* name = file name without folder or ".elf" */
    const char *b = path; for (const char *p = path; *p; p++) if (*p == '/') b = p + 1;
    kstrncpy(t->name, b, sizeof t->name);
    int n = 0; while (t->name[n]) n++;
    if (n > 4 && t->name[n - 4] == '.') t->name[n - 4] = 0;
    kstrncpy(t->title, t->name, sizeof t->title);
    kstrncpy(t->arg, arg ? arg : "", sizeof t->arg);
    fpu_fresh_state(t->fpu);
    __asm__ volatile("frstor (%0)" :: "r"(tasks[cur].fpu) : "memory");     /* keep the running task's FPU intact */
    t->state = T_READY;
    wm_proc_opened(slot);
    return t->pid;
}

/* End a program. If it is the one running right now we can't free its stack
 * yet, so it becomes a "zombie" and task 0 cleans it up shortly after. */
static void proc_end(int i, const char *why) {
    struct task *t = &tasks[i];
    if (t->state == T_FREE || t->state == T_ZOMBIE) return;
    if (why) wm_proc_crashed(t->name, why);
    if (i == cur) t->state = T_ZOMBIE;
    else free_task(i);
}

int proc_kill(int pid) {
    for (int i = 1; i < MAX_TASKS; i++)
        if (tasks[i].state != T_FREE && tasks[i].pid == pid) { proc_end(i, 0); return 1; }
    return 0;
}

void proc_reap(void) {
    for (int i = 1; i < MAX_TASKS; i++) if (tasks[i].state == T_ZOMBIE && i != cur) free_task(i);
}

/* A program caused a CPU exception: report it, end it, run something else. */
uint32_t proc_fault(const char *name, struct regs *r) {
    char msg[96], *p = msg;
    const char *a = "The program caused a "; while (*a) *p++ = *a++;
    while (*name && p < msg + 60) *p++ = *name++;
    if (r->int_no == 14) {
        uint32_t cr2; __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        const char *b = " at 0x"; while (*b) *p++ = *b++;
        for (int i = 7; i >= 0; i--) *p++ = "0123456789ABCDEF"[(cr2 >> (i * 4)) & 15];
    }
    *p = 0;
    proc_end(cur, msg);
    return schedule();
}

/* ---------- events (keyboard + mouse) ---------- */
#define EVQ 32
static void push_event(int i, struct event ev) {
    struct task *t = &tasks[i];
    if (t->state == T_FREE || t->state == T_ZOMBIE) return;
    if (t->state == T_WAIT_KEY && ev.type == EV_KEY) {  /* waiting in getkey(): hand it over */
        ((struct regs *)t->esp)->eax = (uint32_t)ev.key;
        t->state = T_READY;
        return;
    }
    /* mouse moves pile up fast: replace a move that hasn't been read yet */
    if (ev.type == EV_MOUSE_MOVE && t->eh != t->et) {
        struct event *last = &t->evq[(t->eh + EVQ - 1) % EVQ];
        if (last->type == EV_MOUSE_MOVE) { *last = ev; goto wake; }
    }
    int n = (t->eh + 1) % EVQ;
    if (n == t->et) return;                             /* queue full: drop */
    t->evq[t->eh] = ev; t->eh = n;
wake:
    if (t->state == T_WAIT_EVENT) {                     /* waiting in getevent(): ask it to look again */
        ((struct regs *)t->esp)->eax = (uint32_t)-2;
        t->state = T_READY;
    }
}
void proc_key(int i, int key) { struct event e = { EV_KEY, key, 0, 0, 0 }; push_event(i, e); }
/* The user dragged a program window to a new size: a new canvas, then tell the program. */
int proc_set_size(int slot, int w, int h) {
    struct task *t = &tasks[slot];
    int mw, mh;
    wm_max_client(&mw, &mh);
    if (w < t->min_w) w = t->min_w;
    if (h < t->min_h) h = t->min_h;
    if (w > mw) w = mw;
    if (h > mh) h = mh;
    if (w == t->cw && h == t->ch) return 0;
    uint32_t *nc = kmalloc(w * h * 4);
    if (!nc) return -1;
    for (int i = 0; i < w * h; i++) nc[i] = t->bg;
    kfree(t->canvas);
    t->canvas = nc; t->cw = w; t->ch = h; t->tx = t->ty = 0;
    struct event e = { EV_RESIZE, 0, w, h, 0 };
    push_event(slot, e);
    return 0;
}
void proc_mouse(int i, int type, int x, int y, int buttons) { struct event e = { type, 0, x, y, buttons }; push_event(i, e); }

static int pop_event(struct task *t, struct event *out) {
    if (t->eh == t->et) return 0;
    *out = t->evq[t->et]; t->et = (t->et + 1) % EVQ;
    return 1;
}
static int pop_key(struct task *t) {                    /* getkey() ignores mouse events */
    struct event e;
    while (pop_event(t, &e)) if (e.type == EV_KEY) return e.key;
    return -1;
}

/* ---------- system calls ---------- */
/* Copy a string from the program, checking every page it touches. */
static int user_str(struct task *t, uint32_t p, char *out, int max) {
    for (int i = 0; i < max - 1; i++) {
        if (i == 0 || ((p + i) & 0xFFF) == 0) if (!vmm_user_ok(t->pd, p + i, 1)) return 0;
        out[i] = *(char *)(p + i);
        if (!out[i]) return 1;
    }
    out[max - 1] = 0;
    return 1;
}

uint32_t proc_syscall(struct regs *r) {
    struct task *t = &tasks[cur];
    if (cur == 0) {                                     /* the kernel only ever yields */
        if (r->eax == SYS_YIELD) return schedule();
        return (uint32_t)r;
    }
    uint32_t a = r->ebx, b = r->ecx, c = r->edx, d = r->esi, e = r->edi;
    char s[256];
    int32_t ret = 0;
    switch (r->eax) {
    case SYS_EXIT:
        t->exit_code = (int)a;
        proc_end(cur, 0);
        return schedule();
    case SYS_PRINT:
        if (!user_str(t, a, s, sizeof s)) { ret = -1; break; }
        for (char *q = s; *q; q++) cv_putc(t, *q);
        wm_canvas_dirty(cur);
        break;
    case SYS_GETKEY:
        if ((ret = pop_key(t)) >= 0) break;
        t->state = T_WAIT_KEY;                          /* sleep until proc_key() */
        return schedule();
    case SYS_POLLKEY:
        ret = pop_key(t);
        break;
    case SYS_GETEVENT:
    case SYS_POLLEVENT: {
        if (!vmm_user_ok(t->pd, a, sizeof(struct event))) { ret = -1; break; }
        struct event ev;
        if (pop_event(t, &ev)) { *(struct event *)a = ev; ret = 1; break; }
        if (r->eax == SYS_POLLEVENT) { ret = 0; break; }
        t->state = T_WAIT_EVENT;                        /* woken with -2: the library asks again */
        return schedule(); }
    case SYS_SBRK: {
        int32_t inc = (int32_t)a;
        uint32_t old = t->brk, nb = old + inc;
        if ((inc > 0 && nb < old) || nb < t->brk_base || nb > USER_TOP - USTACK_SIZE - 0x10000) { ret = -1; break; }
        if (inc > 0 && !map_range(t, old, nb, 1)) { ret = -1; break; }
        t->brk = nb;
        ret = (int32_t)old;
        break; }
    case SYS_RESIZE: {
        int w = (int)a, h = (int)b, mw, mh;
        wm_max_client(&mw, &mh);
        if (w < 160) w = 160;
        if (w > mw) w = mw;
        if (h < 100) h = 100;
        if (h > mh) h = mh;
        uint32_t *nc = kmalloc(w * h * 4);
        if (!nc) { ret = -1; break; }
        kfree(t->canvas);
        t->canvas = nc; t->cw = w; t->ch = h; t->tx = t->ty = 0;
        for (int i = 0; i < w * h; i++) nc[i] = t->bg;
        wm_proc_resized(cur);
        break; }
    case SYS_RESIZABLE:
        t->resizable = 1;
        t->min_w = (int)a < 160 ? 160 : (int)a; t->min_h = (int)b < 100 ? 100 : (int)b;
        ret = 0;
        break;
    case SYS_BLIT: {
        int x = (int16_t)(a & 0xFFFF), y = (int16_t)(a >> 16), w = b & 0xFFFF, h = b >> 16, stride = (int)d;
        if (w <= 0 || h <= 0 || stride < w) { ret = -1; break; }
        if (!vmm_user_ok(t->pd, c, ((uint32_t)(h - 1) * stride + w) * 4)) { ret = -1; break; }
        const uint32_t *src = (const uint32_t *)c;
        for (int j = 0; j < h; j++) {
            int py = y + j;
            if (py < 0 || py >= t->ch) continue;
            for (int i = 0; i < w; i++) {
                int px = x + i;
                if (px >= 0 && px < t->cw) t->canvas[py * t->cw + px] = src[j * stride + i];
            }
        }
        wm_canvas_dirty(cur);
        break; }
    case SYS_SLEEP: {
        uint32_t tk = a * TIMER_HZ / 1000;
        t->wake = ticks + (tk ? tk : 1);
        t->state = T_SLEEP;
        r->eax = 0;
        return schedule(); }
    case SYS_TICKS:
        ret = (int32_t)(ticks * (1000 / TIMER_HZ));
        break;
    case SYS_FILL:
        cv_fill(t, (int)a, (int)b, (int)c, (int)d, e);
        wm_canvas_dirty(cur);
        break;
    case SYS_TEXT:
        if (!user_str(t, c, s, sizeof s)) { ret = -1; break; }
        if (e >= 2 && e <= 4) for (int i = 0; s[i]; i++) cv_char_big(t, (int)a + i * 8 * (int)e, (int)b, s[i], d, (int)e);
        else for (int i = 0; s[i]; i++) cv_char(t, (int)a + i * 8, (int)b, s[i], d, 0, 0);
        wm_canvas_dirty(cur);
        break;
    case SYS_CLEAR:
        t->bg = a;
        cv_fill(t, 0, 0, t->cw, t->ch, a);
        t->tx = t->ty = 0;
        wm_canvas_dirty(cur);
        break;
    case SYS_SETCOLOR:
        t->fg = a; t->bg = b;
        break;
    case SYS_GETPID:
        ret = t->pid;
        break;
    case SYS_WALLPAPER: {
        char path[FS_PATH_MAX], abs[FS_PATH_MAX];
        if (!user_str(t, a, path, sizeof path)) { ret = -1; break; }
        path_resolve("/", path, abs);
        ret = wm_set_wallpaper(abs);
        break; }
    case SYS_READFILE: {
        char path[FS_PATH_MAX], abs[FS_PATH_MAX];
        if (!user_str(t, a, path, sizeof path) || !vmm_user_ok(t->pd, b, c)) { ret = -1; break; }
        path_resolve("/", path, abs);
        ret = fs_read(abs, (void *)b, c);
        break; }
    case SYS_WRITEFILE: {
        char path[FS_PATH_MAX], abs[FS_PATH_MAX];
        if (!user_str(t, a, path, sizeof path) || !vmm_user_ok(t->pd, b, c)) { ret = -1; break; }
        path_resolve("/", path, abs);
        ret = fs_write(abs, (void *)b, c) ? (int32_t)c : -1;
        break; }
    case SYS_NOTIFY: {
        char title[40], text[72];
        if (!user_str(t, a, title, sizeof title)) { ret = -1; break; }
        if (!b || !user_str(t, b, text, sizeof text)) text[0] = 0;
        wm_notify(title, text, 0);
        break; }
    case SYS_DELETE: case SYS_MKDIR: {
        char path[FS_PATH_MAX], abs[FS_PATH_MAX];
        if (!user_str(t, a, path, sizeof path)) { ret = -1; break; }
        path_resolve("/", path, abs);
        if (r->eax == SYS_MKDIR) ret = fs_stat(abs, 0) == 2 || fs_mkdir(abs) ? 0 : -1;
        else ret = fs_stat(abs, 0) == 1 && fs_delete(abs) ? 0 : -1;
        break; }
    case SYS_TIME: {
        if (!vmm_user_ok(t->pd, a, 6 * 4)) { ret = -1; break; }
        int *o = (int *)a;
        rtc_time(&o[0], &o[1], &o[2]); rtc_date(&o[3], &o[4], &o[5]);
        break; }
    case SYS_YIELD:
        return schedule();
    case SYS_CANVAS:
        ret = (t->cw << 16) | t->ch;
        break;
    case SYS_SETTITLE:
        if (user_str(t, a, s, sizeof t->title)) { kstrncpy(t->title, s, sizeof t->title); wm_redraw(); }
        break;
    case SYS_TONE:
        sound_tone(a, b);
        break;
    case SYS_PLAY: {
        if (!vmm_user_ok(t->pd, a, b)) { ret = -1; break; }
        ret = sound_play((void *)a, b, c, d & 0xFF, (d >> 8) & 0xFF, t->pid);
        break; }
    case SYS_STOPSOUND:
        sound_stop();
        break;
    case SYS_SOUNDPOS:
        ret = sound_position();
        break;
    case SYS_VOLUME:
        if ((int)a >= 0) sound_set_volume((int)a);
        ret = sound_get_volume();
        break;
    case SYS_GETARG: {
        if (!b || !vmm_user_ok(t->pd, a, b)) { ret = -1; break; }
        uint32_t k = 0;
        for (; t->arg[k] && k < b - 1; k++) ((char *)a)[k] = t->arg[k];
        ((char *)a)[k] = 0;
        ret = (int32_t)k;
        break; }
    case SYS_LISTDIR: {
        char path[FS_PATH_MAX], abs[FS_PATH_MAX];
        if (!user_str(t, a, path, sizeof path) || c > 512 || !vmm_user_ok(t->pd, b, c * sizeof(struct dirent))) { ret = -1; break; }
        path_resolve("/", path, abs);
        struct fs_entry *tmp = kmalloc((c ? c : 1) * sizeof(struct fs_entry));
        if (!tmp) { ret = -1; break; }
        int n = fs_list(abs, tmp, c);
        struct dirent *out = (struct dirent *)b;
        for (int i = 0; i < n; i++) {
            kstrncpy(out[i].name, tmp[i].name, sizeof out[i].name);
            out[i].is_dir = tmp[i].is_dir;
            out[i].size = tmp[i].size;
        }
        kfree(tmp);
        ret = n;
        break; }
    case SYS_RESOLVE: {
        if (!user_str(t, a, s, 64) || !vmm_user_ok(t->pd, b, 4)) { ret = -1; break; }
        uint32_t ip;
        ret = net_resolve(s, &ip);
        if (ret == 1) *(uint32_t *)b = ip;
        break; }
    case SYS_TCPOPEN:
        ret = tcp_open(a, (uint16_t)b, t->pid);
        break;
    case SYS_TCPSTATUS:
        ret = tcp_status((int)a, t->pid);
        break;
    case SYS_TCPSEND:
        if (!vmm_user_ok(t->pd, b, c)) { ret = -1; break; }
        ret = tcp_send((int)a, t->pid, (void *)b, c);
        break;
    case SYS_TCPRECV:
        if (!vmm_user_ok(t->pd, b, c)) { ret = -1; break; }
        ret = tcp_recv((int)a, t->pid, (void *)b, c);
        break;
    case SYS_TCPCLOSE:
        tcp_close((int)a, t->pid);
        break;
    case SYS_CLIPSET:                                       /* text onto the shared clipboard */
        if ((int)b < 0 || (int)b > CLIP_MAX || !vmm_user_ok(t->pd, a, b)) { ret = -1; break; }
        clip_set((const char *)a, (int)b); ret = 0;
        break;
    case SYS_CLIPGET: {
        int n = clip_len();
        if ((int)b > 0) {
            if (!vmm_user_ok(t->pd, a, b)) { ret = -1; break; }
            int m = n < (int)b ? n : (int)b;
            const char *c = clip_text();
            for (int i = 0; i < m; i++) ((char *)a)[i] = c[i];
        }
        ret = n;
        break;
    }
    case SYS_FONT:                                          /* lets programs draw text themselves */
        if (!vmm_user_ok(t->pd, a, 128 * 16)) { ret = -1; break; }
        for (int i = 0; i < 128 * 16; i++) ((uint8_t *)a)[i] = (b ? &font_bold[0][0] : &font_regular[0][0])[i];
        ret = 0;
        break;
    case SYS_NETINFO:
        if (!vmm_user_ok(t->pd, a, 16)) { ret = -1; break; }
        ((uint32_t *)a)[0] = netcfg.ip; ((uint32_t *)a)[1] = netcfg.mask;
        ((uint32_t *)a)[2] = netcfg.gateway; ((uint32_t *)a)[3] = netcfg.dns;
        ret = netcfg.ip != 0;
        break;
    default:
        ret = -1;
    }
    r->eax = (uint32_t)ret;
    return (uint32_t)r;
}
