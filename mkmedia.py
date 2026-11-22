#!/usr/bin/env python3
"""mkmedia.py OUTDIR - makes MyOS's sample media (all original, generated here):
   Music/Sunrise.wav          16-bit mono 22 kHz: soft arpeggios over C-Am-F-G
   Music/Chiptune.wav         8-bit mono 11 kHz: square-wave melody + bass
   Music/Test tone 440 Hz.wav 16-bit stereo 44.1 kHz: pure A4, for testing
   Pictures/Sunset.bmp        800x500 drawn landscape
   Pictures/Aurora.bmp        1280x800 (bigger than the screen: tests shrink-to-fit)
   Pictures/Mountains.png     1280x800 RGB PNG landscape (a good wallpaper)
   Pictures/MyOS logo.png     512x512 PNG with see-through (alpha) edges
   Pictures/Pixel art.png     palette PNG with a see-through colour
"""
import math, os, struct, sys, random, zlib

def wav(path, rate, bits, chans, frames):
    data = bytearray()
    for f in frames:
        for v in (f if chans == 2 else (f,)):
            v = max(-1.0, min(1.0, v))
            data += struct.pack('<h', int(v * 32000)) if bits == 16 else bytes([int(128 + v * 120)])
    hdr = b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVE'
    hdr += b'fmt ' + struct.pack('<IHHIIHH', 16, 1, chans, rate, rate * chans * bits // 8, chans * bits // 8, bits)
    hdr += b'data' + struct.pack('<I', len(data))
    open(path, 'wb').write(hdr + data)

def note(n): return 440.0 * 2 ** ((n - 69) / 12)   # MIDI note number -> Hz

def sunrise(rate=22050):
    chords = [[60, 64, 67, 72], [57, 60, 64, 69], [53, 57, 60, 65], [55, 59, 62, 67]]
    step = 0.18; out = []
    for rep in range(2):
        for ch in chords:
            pattern = ch + ch[2:0:-1] + ch + [ch[3] + 12, ch[2]]
            for n in pattern:
                f = note(n); N = int(step * rate)
                for i in range(N):
                    t = i / rate; env = math.exp(-t * 6)
                    s = 0.45 * math.sin(2 * math.pi * f * t) + 0.15 * math.sin(4 * math.pi * f * t)
                    s += 0.12 * math.sin(2 * math.pi * note(ch[0] - 12) * (len(out) / rate))   # held bass
                    out.append(s * env + 0.12 * math.sin(2 * math.pi * note(ch[0] - 12) * (len(out) / rate)))
    tail = int(rate * 0.8)
    out += [0.3 * math.sin(2 * math.pi * note(48) * i / rate) * math.exp(-i / rate * 4) for i in range(tail)]
    return out

def chiptune(rate=11025):
    melody = [72, 74, 76, 79, 76, 74, 72, 67, 69, 72, 74, 72, 0, 76, 79, 81,
              79, 76, 74, 72, 74, 76, 74, 72, 67, 69, 72, 0, 72, 79, 84, 0]
    bass = [48, 48, 43, 43, 45, 45, 41, 43]
    step = 0.16; out = []
    for i, n in enumerate(melody * 2):
        N = int(step * rate); b = note(bass[(i // 4) % len(bass)])
        for k in range(N):
            t = k / rate; tt = len(out) / rate
            s = 0.0
            if n: s += 0.35 * (1 if math.sin(2 * math.pi * note(n) * t) > 0 else -1) * (1 - k / N * 0.6)
            s += 0.25 * (1 if math.sin(2 * math.pi * b * tt) > 0.3 else -1)
            out.append(s * 0.8)
    return out

def bmp(path, w, h, pixel):
    row = (w * 3 + 3) & ~3
    data = bytearray()
    for y in range(h - 1, -1, -1):
        r = bytearray()
        for x in range(w):
            R, G, B = pixel(x, y)
            r += bytes([B, G, R])
        r += b'\0' * (row - w * 3)
        data += r
    hdr = b'BM' + struct.pack('<IHHI', 54 + len(data), 0, 0, 54)
    hdr += struct.pack('<IiiHHIIiiII', 40, w, h, 1, 24, 0, len(data), 2835, 2835, 0, 0)
    open(path, 'wb').write(hdr + data)

def png(path, w, h, ctype, rows, palette=None, trns=None):
    """ctype 2 = RGB, 6 = RGBA, 3 = palette; rows yields one bytes object per row."""
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    raw = b''.join(b'\0' + r for r in rows)
    data = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, ctype, 0, 0, 0))
    if palette: data += chunk(b'PLTE', b''.join(bytes(c) for c in palette))
    if trns: data += chunk(b'tRNS', bytes(trns))
    data += chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')
    open(path, 'wb').write(data)

def mix(a, b, t): return tuple(int(x + (y - x) * t) for x, y in zip(a, b))

def mountains(w=1280, h=800):
    horizon = 560
    sky_top, sky_mid, sky_low = (22, 34, 90), (124, 77, 160), (255, 150, 90)
    sun_x, sun_y = 820, 470
    ridges = [  # (base height, colour, wave sizes)
        (420, (88, 70, 140), [(230, 70), (97, 30), (41, 12)]),
        (470, (58, 46, 104), [(170, 55), (71, 22), (29, 9)]),
        (515, (32, 28, 66), [(300, 40), (53, 18), (19, 6)]),
    ]
    def sky(y):
        t = y / horizon
        return mix(sky_top, sky_mid, t / 0.6) if t < 0.6 else mix(sky_mid, sky_low, (t - 0.6) / 0.4)
    def scene(x, y):
        c = sky(min(y, horizon))
        d = math.hypot(x - sun_x, y - sun_y)
        if d < 46: c = (255, 236, 180)
        elif d < 220: c = mix(c, (255, 190, 120), ((220 - d) / 174) ** 2 * 0.65)
        for base, col, waves in ridges:
            top = base + sum(a * math.sin(x / l + l) for l, a in waves) * 0.6
            if y >= top: c = mix(col, sky(base - 60), 0.15 * (1 - (y - top) / 200) if y - top < 200 else 0)
        return c
    rows = []
    for y in range(h):
        r = bytearray()
        if y < horizon:
            for x in range(w): r += bytes(scene(x, y))
        else:                                     # the lake: a darker, rippled mirror image
            dy = y - horizon
            for x in range(w):
                ry = horizon - 1 - dy - int(3 * math.sin(x / 9 + dy / 3) * (dy / 240))
                c = scene(x, max(0, ry))
                r += bytes(mix(c, (12, 18, 50), 0.35 + dy / 600))
        rows.append(bytes(r))
    return rows

def logo(n=512):
    rows = []
    for y in range(n):
        r = bytearray()
        for x in range(n):
            # rounded square from 56 to 456 with radius 96; soft shadow below it
            def sd(px, py, lo=56, hi=456, rad=96):
                qx = max(lo + rad - px, 0, px - (hi - rad)); qy = max(lo + rad - py, 0, py - (hi - rad))
                return math.hypot(qx, qy) - rad
            d = sd(x + 0.5, y + 0.5)
            shadow = max(0.0, min(1.0, (24 - sd(x + 0.5, y - 14.5)) / 24)) * 0.35
            if d < 1:
                cover = min(1.0, 1 - d) if d > 0 else 1.0
                t = (x + y) / (2 * n)
                col = mix((59, 130, 246), (168, 85, 247), t)
                # two linked rings, like the MyOS Start button
                for cx in (206, 306):
                    ring = abs(math.hypot(x + 0.5 - cx, y + 0.5 - 256) - 86) - 15
                    if ring < 1: col = mix(col, (255, 255, 255), 0.95 * min(1.0, 1 - ring))
                a = cover + shadow * (1 - cover)
                rgb = mix((0, 0, 0), col, cover / a if a else 0)
                r += bytes(rgb) + bytes([int(a * 255)])
            else:
                r += bytes([0, 0, 0, int(shadow * 255)])
        rows.append(bytes(r))
    return rows

def pixel_art(scale=8):
    art = ["................",
           "...RR.....RR....",
           "..RLLR...RLLR...",
           ".RLLRRR.RRRRRR..",
           ".RLRRRRRRRRRRR..",
           ".RRRRRRRRRRRRR..",
           ".RRRRRRRRRRRRR..",
           "..RRRRRRRRRRR...",
           "...RRRRRRRRR....",
           "....RRRRRRR.....",
           ".....RRRRR......",
           "......RRR.......",
           ".......R........",
           "................",
           "..Y..Y....Y..Y..",
           "................"]
    pal = [(0, 0, 0), (220, 38, 38), (252, 165, 165), (250, 204, 21)]
    idx = {'.': 0, 'R': 1, 'L': 2, 'Y': 3}
    rows = []
    for line in art:
        r = bytes(idx[c] for c in line for _ in range(scale))
        rows += [r] * scale
    return rows, pal

def sunset(x, y, w=800, h=500):
    t = y / h
    sky = (int(255 - 60 * t), int(120 + 60 * (1 - t) - 40 * t), int(80 + 120 * t))
    d = math.hypot(x - 560, y - 300)
    if d < 70: sky = (255, 220, 120)
    elif d < 140: a = (140 - d) / 70 * 0.5; sky = tuple(int(c * (1 - a) + s * a) for c, s in zip(sky, (255, 200, 120)))
    m1 = 330 + 50 * math.sin(x / 90) + 20 * math.sin(x / 23)
    m2 = 390 + 30 * math.sin(x / 60 + 1) + 12 * math.sin(x / 17)
    if y > m2: return (30, 25, 60)
    if y > m1: return (70, 50, 100)
    if y > 440: return (40, 30, 70)
    return sky

def aurora(x, y, w=1280, h=800):
    random.seed(x * 7 + y * 131)
    base = (8, 14, 40 + y * 30 // h)
    g = 0
    for k, c in ((1, (60, 255, 170)), (2, (120, 120, 255))):
        cy = 250 + 90 * math.sin(x / (160 * k) + k) + 40 * math.sin(x / 47)
        g = max(0, 1 - abs(y - cy) / (70 + 30 * k))
        base = tuple(min(255, int(b + cc * g * 0.8)) for b, cc in zip(base, c))
    if y > 650 + 20 * math.sin(x / 70): return (5, 8, 15)
    if random.random() < 0.002 and y < 500: return (255, 255, 255)
    return base

if __name__ == '__main__':
    out = sys.argv[1]
    os.makedirs(out + '/Music', exist_ok=True); os.makedirs(out + '/Pictures', exist_ok=True)
    wav(out + '/Music/Sunrise.wav', 22050, 16, 1, sunrise())
    wav(out + '/Music/Chiptune.wav', 11025, 8, 1, chiptune())
    wav(out + '/Music/Test tone 440 Hz.wav', 44100, 16, 2,
        [(0.5 * math.sin(2 * math.pi * 440 * i / 44100),) * 2 for i in range(44100 * 3)])
    bmp(out + '/Pictures/Sunset.bmp', 800, 500, sunset)
    bmp(out + '/Pictures/Aurora.bmp', 1280, 800, aurora)
    png(out + '/Pictures/Mountains.png', 1280, 800, 2, mountains())
    png(out + '/Pictures/MyOS logo.png', 512, 512, 6, logo())
    rows, pal = pixel_art()
    png(out + '/Pictures/Pixel art.png', 128, 128, 3, rows, pal, trns=[0])
