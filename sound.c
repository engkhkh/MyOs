/* sound.c - Sound for MyOS.
 *
 * 1. PC speaker: the simple beeper every PC has. It plays one square-wave tone
 *    at a time. Tones are queued and the timer interrupt moves through the
 *    queue, so melodies play in the background without freezing anything.
 *
 * 2. Sound Blaster 16: a real sound card (emulated by QEMU, VirtualBox, Bochs)
 *    that plays recorded audio such as .wav files. Audio flows like this:
 *       program's samples -> kernel copy -> 64 KB DMA buffer -> sound card
 *    The DMA controller streams the buffer to the card by itself, over and over
 *    ("auto-init"). The card interrupts us each time it finishes one half of the
 *    buffer, and we refill that half with the next part of the song while the
 *    other half plays (double buffering).
 */
#include <stdint.h>
#include "kernel.h"

/* ================= PC speaker ================= */
struct tone { uint16_t freq, ticks; };
static struct tone tq[64];
static volatile int tq_head, tq_tail;
static int tone_left;                         /* ticks left on the current tone */

static void speaker_set(uint32_t freq) {
    if (!freq) { outb(0x61, inb(0x61) & 0xFC); return; }
    uint32_t div = 1193182 / freq;
    outb(0x43, 0xB6);                         /* PIT channel 2, square wave */
    outb(0x42, div & 0xFF);
    outb(0x42, div >> 8);
    outb(0x61, inb(0x61) | 3);                /* connect the speaker */
}

/* Queue a tone (freq 0 = silence) lasting ms milliseconds. */
void sound_tone(uint32_t freq, uint32_t ms) {
    int n = (tq_head + 1) % 64;
    if (n == tq_tail) return;
    uint32_t t = ms * TIMER_HZ / 1000;
    tq[tq_head].freq = freq > 20000 ? 0 : freq;
    tq[tq_head].ticks = t ? t : 1;
    tq_head = n;
}

/* Called by the timer interrupt 100 times a second. */
void sound_tick(void) {
    if (tone_left > 0 && --tone_left > 0) return;
    if (tq_tail == tq_head) { if (tone_left == 0) { speaker_set(0); tone_left = -1; } return; }
    speaker_set(tq[tq_tail].freq);
    tone_left = tq[tq_tail].ticks;
    tq_tail = (tq_tail + 1) % 64;
}

/* ================= Sound Blaster 16 ================= */
#define SB      0x220
#define HALF    8192                          /* stereo frames per half buffer */

static int16_t dmabuf[HALF * 2 * 2] __attribute__((aligned(65536)));   /* 64 KB, never crosses a DMA boundary */
static int sb_ok;
static volatile int playing;
static uint8_t *src;                          /* the song (kernel copy) */
static uint32_t src_frames, pos, rate;
static int bits, channels, half_next, silent_halves;
static int owner_pid;
static int volume = 100;

static int dsp_read(void) {
    for (int i = 0; i < 100000; i++) if (inb(SB + 0xE) & 0x80) return inb(SB + 0xA);
    return -1;
}
static void dsp_write(uint8_t v) {
    for (int i = 0; i < 100000 && (inb(SB + 0xC) & 0x80); i++) {}
    outb(SB + 0xC, v);
}
static void mixer(uint8_t reg, uint8_t v) { outb(SB + 4, reg); outb(SB + 5, v); }

int sound_init(void) {
    outb(SB + 6, 1);                          /* reset the DSP */
    for (int i = 0; i < 100; i++) inb(SB + 6);
    outb(SB + 6, 0);
    if (dsp_read() != 0xAA) return 0;         /* no card */
    dsp_write(0xE1);                          /* DSP version: SB16 is 4.xx */
    int major = dsp_read(); dsp_read();
    if (major < 4) return 0;
    mixer(0x80, 0x02);                        /* use IRQ 5 */
    mixer(0x81, 0x22);                        /* 8-bit DMA 1, 16-bit DMA 5 */
    mixer(0x22, 0xFF);                        /* master volume: max (we scale samples ourselves) */
    dsp_write(0xD1);                          /* speaker output on */
    sb_ok = 1;
    return 1;
}
int sound_card_present(void) { return sb_ok; }

/* Fill one half of the DMA buffer with the next frames of the song. */
static void refill(int half) {
    int16_t *out = dmabuf + half * HALF * 2;
    int bps = bits / 8 * channels;            /* bytes per source frame */
    for (int i = 0; i < HALF; i++) {
        int32_t l = 0, r = 0;
        if (pos < src_frames) {
            uint8_t *p = src + pos * bps;
            if (bits == 16) { l = (int16_t)(p[0] | p[1] << 8); r = channels == 2 ? (int16_t)(p[2] | p[3] << 8) : l; }
            else { l = (p[0] - 128) << 8; r = channels == 2 ? (p[1] - 128) << 8 : l; }
            pos++;
        }
        out[i * 2] = (int16_t)(l * volume / 100);
        out[i * 2 + 1] = (int16_t)(r * volume / 100);
    }
    if (pos >= src_frames) silent_halves++;
}

static void stop_now(void) {
    if (playing) {
        dsp_write(0xD5);                      /* pause 16-bit output */
        dsp_write(0xD9);                      /* leave auto-init mode */
        outb(0xD4, 0x05);                     /* mask DMA channel 5 */
    }
    playing = 0;
    if (src) { kfree(src); src = 0; }
}

/* Interrupt 5: the card finished one half of the buffer. */
void sound_irq(void) {
    inb(SB + 0xF);                            /* acknowledge the 16-bit interrupt */
    if (!playing) return;
    if (silent_halves >= 2) { stop_now(); return; }
    refill(half_next);
    half_next ^= 1;
}

/* Start playing PCM audio (copied, so the program may free its buffer). */
int sound_play(const void *data, uint32_t bytes, uint32_t hz, int b, int ch, int pid) {
    if (!sb_ok || (b != 8 && b != 16) || (ch != 1 && ch != 2) || hz < 4000 || hz > 48000) return -1;
    uint32_t flags; __asm__ volatile("pushf; pop %0; cli" : "=r"(flags));
    stop_now();
    __asm__ volatile("push %0; popf" :: "r"(flags));
    uint8_t *copy = kmalloc(bytes ? bytes : 1);
    if (!copy) return -1;
    const uint8_t *s = data;
    for (uint32_t i = 0; i < bytes; i++) copy[i] = s[i];

    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags));
    src = copy; bits = b; channels = ch; rate = hz; owner_pid = pid;
    src_frames = bytes / (b / 8 * ch); pos = 0; silent_halves = 0;
    refill(0); refill(1);
    half_next = 0;

    uint32_t phys = (uint32_t)dmabuf;         /* identity-mapped, below 16 MB */
    uint32_t words = sizeof(dmabuf) / 2;
    outb(0xD4, 0x05);                         /* mask channel 5 */
    outb(0xD8, 0x00);                         /* reset the byte flip-flop */
    outb(0xD6, 0x59);                         /* single, auto-init, memory -> card, channel 5 */
    outb(0xC4, (phys >> 1) & 0xFF);           /* 16-bit channels count in words */
    outb(0xC4, (phys >> 9) & 0xFF);
    outb(0x8B, (phys >> 16) & 0xFF);          /* page */
    outb(0xC6, (words - 1) & 0xFF);
    outb(0xC6, (words - 1) >> 8);
    outb(0xD4, 0x01);                         /* unmask channel 5 */

    dsp_write(0x41);                          /* output sample rate */
    dsp_write(hz >> 8); dsp_write(hz & 0xFF);
    dsp_write(0xB6);                          /* 16-bit, output, auto-init, FIFO */
    dsp_write(0x30);                          /* stereo, signed */
    uint32_t n = HALF * 2 - 1;                /* samples per interrupt - 1 */
    dsp_write(n & 0xFF); dsp_write(n >> 8);
    playing = 1;
    __asm__ volatile("push %0; popf" :: "r"(flags));
    return 0;
}

void sound_stop(void) {
    uint32_t flags; __asm__ volatile("pushf; pop %0; cli" : "=r"(flags));
    stop_now();
    __asm__ volatile("push %0; popf" :: "r"(flags));
}
/* A program ended: stop its music. */
void sound_stop_owner(int pid) { if (playing && owner_pid == pid) sound_stop(); }

/* Milliseconds played so far, or -1 when nothing is playing. */
int sound_position(void) {
    if (!playing) return -1;
    uint32_t p = pos > 2 * HALF ? pos - 2 * HALF : 0;     /* minus what is still buffered */
    return (int)(p / rate * 1000 + p % rate * 1000 / rate);
}
void sound_set_volume(int v) { volume = v < 0 ? 0 : v > 100 ? 100 : v; }
int sound_get_volume(void) { return volume; }
