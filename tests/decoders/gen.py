import zlib, struct, random, sys, os
from PIL import Image
random.seed(int(sys.argv[1]) if len(sys.argv) > 1 else 1)
CH = {0:1, 2:3, 3:1, 4:2, 6:4}
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
def pack_row(samples, depth):
    if depth == 8: return bytes(samples)
    if depth == 16: return b''.join(struct.pack('>H', s) for s in samples)
    out, acc, n = bytearray(), 0, 0
    for s in samples:
        acc = (acc << depth) | s; n += depth
        if n == 8: out.append(acc); acc = n = 0
    if n: out.append(acc << (8 - n))
    return bytes(out)
def paeth(a, b, c):
    p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else (b if pb <= pc else c)
def filt(rows, bpp):
    out, prev = bytearray(), bytes(len(rows[0])) if rows else b''
    for r in rows:
        f = random.randrange(5); o = bytearray([f])
        for i in range(len(r)):
            a = r[i - bpp] if i >= bpp else 0; b = prev[i]; c = prev[i - bpp] if i >= bpp else 0
            pred = [0, a, b, (a + b) // 2, paeth(a, b, c)][f]
            o.append((r[i] - pred) & 255)
        out += o; prev = r
    return bytes(out)
def make(ctype, depth, w, h, interlace, trns):
    ch = CH[ctype]; maxv = (1 << depth) - 1
    npal = random.randint(1, min(256, 1 << depth)) if ctype == 3 else 0
    pal = [(random.randrange(256), random.randrange(256), random.randrange(256)) for _ in range(npal)]
    palalpha = [random.randrange(256) for _ in range(npal)] if (ctype == 3 and trns) else None
    pix = [[[random.randint(0, maxv if ctype != 3 else npal - 1) for _ in range(ch)] for x in range(w)] for y in range(h)]
    key = None
    if trns and ctype in (0, 2):
        key = list(pix[random.randrange(h)][random.randrange(w)])   # a colour that occurs
    bits_pp = depth * ch; bpp = max(1, bits_pp // 8)
    data = b''
    passes = [(0,0,8,8),(4,0,8,8),(0,4,4,8),(2,0,4,4),(0,2,2,4),(1,0,2,2),(0,1,1,2)] if interlace else [(0,0,1,1)]
    for sx, sy, dx, dy in passes:
        xs = list(range(sx, w, dx)); ys = list(range(sy, h, dy))
        if not xs or not ys: continue
        rows = [pack_row([s for x in xs for s in pix[y][x]], depth) for y in ys]
        data += filt(rows, bpp)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, depth, ctype, 0, 0, interlace))
    if ctype == 3:
        png += chunk(b'PLTE', b''.join(bytes(p) for p in pal))
        if palalpha: png += chunk(b'tRNS', bytes(palalpha[:random.randint(1, npal)]))
    if key is not None:
        png += chunk(b'tRNS', b''.join(struct.pack('>H', v) for v in key))
    z = zlib.compress(data, random.choice([0, 1, 6, 9]))
    k = random.randint(1, 3)                                         # split into several IDATs
    cuts = sorted(random.sample(range(1, len(z)), min(k - 1, len(z) - 1))) if len(z) > 2 else []
    parts = [z[i:j] for i, j in zip([0] + cuts, cuts + [len(z)])]
    for p in parts: png += chunk(b'IDAT', p)
    png += chunk(b'IEND', b'')
    # expected ARGB
    exp = []
    tr = (palalpha[:0] if False else None)
    pa_used = None
    if ctype == 3 and palalpha:
        # alpha only for entries listed in tRNS
        t = png.find(b'tRNS'); n = struct.unpack('>I', png[t-4:t])[0]; pa_used = list(png[t+4:t+4+n])
    for y in range(h):
        for x in range(w):
            s = pix[y][x]
            if ctype == 3:
                r, g, b = pal[s[0]]; a = pa_used[s[0]] if pa_used and s[0] < len(pa_used) else 255
            elif ctype in (0, 4):
                v = s[0]; g8 = v >> 8 if depth == 16 else (v if depth == 8 else v * 255 // maxv)
                r = g = b = g8
                a = (s[1] >> 8 if depth == 16 else s[1]) if ctype == 4 else (0 if key is not None and s == key else 255)
            else:
                r, g, b = [(v >> 8 if depth == 16 else v) for v in s[:3]]
                a = (s[3] >> 8 if depth == 16 else s[3]) if ctype == 6 else (0 if key is not None and s[:3] == key else 255)
            exp.append(a << 24 | r << 16 | g << 8 | b)
    return png, exp
cases = []
for ctype, depths in [(0, [1, 2, 4, 8, 16]), (2, [8, 16]), (3, [1, 2, 4, 8]), (4, [8, 16]), (6, [8, 16])]:
    for depth in depths:
        for interlace in (0, 1):
            for trns in (0, 1):
                if trns and ctype in (4, 6): continue
                w, h = random.randint(1, 37), random.randint(1, 29)
                cases.append((ctype, depth, w, h, interlace, trns))
os.makedirs('cases', exist_ok=True)
for i, (ctype, depth, w, h, il, trns) in enumerate(cases):
    png, exp = make(ctype, depth, w, h, il, trns)
    name = f'cases/c{i:02d}_t{ctype}_d{depth}_i{il}_k{trns}_{w}x{h}'
    open(name + '.png', 'wb').write(png)
    open(name + '.exp', 'wb').write(struct.pack(f'<{len(exp)}I', *exp))
    im = Image.open(name + '.png'); im.load()                         # PIL must accept the file
    if depth <= 8 and not trns:                                       # PIL cross-check of the expected colours
        rgba = list(im.convert('RGBA').getdata())
        for (r, g, b, a), e in zip(rgba, exp):
            if (a << 24 | r << 16 | g << 8 | b) != e and ctype != 0:
                print('PIL disagrees', name); break
print(len(cases), 'cases')
