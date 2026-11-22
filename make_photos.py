#!/usr/bin/env python3
"""make_photos.py - renders the sample JPEG photos in media/ (needs numpy + Pillow).
The results are kept in the project, so building MyOS does not need this script.
   media/Valley.jpg               1600x1000 baseline JPEG, 4:2:0
   media/Coast.jpg                1600x1000 progressive JPEG
   media/Tall tree (phone).jpg    stored sideways + EXIF orientation 6, like a phone photo
   media/Rainbow.gif              an animated GIF (MyOS shows the first frame) with see-through parts
"""
import numpy as np
from PIL import Image

rng = np.random.default_rng(2026)

def noise(h, w, octaves=6, base=4, persistence=0.5):
    """fractal value noise in 0..1"""
    out = np.zeros((h, w)); amp = 1.0; total = 0.0
    for o in range(octaves):
        n = base * 2 ** o
        small = rng.random((n + 1, int(n * w / h) + 2))
        layer = np.asarray(Image.fromarray((small * 255).astype(np.uint8)).resize((w, h), Image.BICUBIC), float) / 255
        out += layer * amp; total += amp; amp *= persistence
    return out / total

def lerp(a, b, t): return a + (b - a) * t[..., None]

def ridge_line(w, amp, waves=((420, 1.0), (170, 0.5), (61, 0.22), (23, 0.08))):
    """a mountain outline: a few sine waves with random phases"""
    x = np.arange(w)
    r = sum(a * np.sin(x / l * 2 * np.pi / 6 + rng.random() * 6.28) for l, a in waves)
    return r / sum(a for _, a in waves) * amp

def streaks(h, w, rows=60):
    """horizontal streaky noise, for water"""
    small = rng.random((h // 3, rows))
    return np.asarray(Image.fromarray((small * 255).astype(np.uint8)).resize((w, h), Image.BICUBIC), float) / 255

def sky(h, w, top, bottom, clouds=0.5):
    t = np.linspace(0, 1, h)[:, None] * np.ones((1, w))
    s = lerp(np.array(top, float), np.array(bottom, float), t)
    c = np.clip((noise(h, w, 6, 3) - (1 - clouds)) * 3, 0, 1)
    return lerp(s, np.array([250, 248, 255], float), c * 0.85)

def valley(w=1600, h=1000):
    img = sky(h, w, (70, 125, 205), (190, 215, 240), 0.45)
    y = np.arange(h)[:, None]
    for i, (base, amp, col) in enumerate([(420, 180, (110, 130, 160)), (520, 140, (70, 105, 80)), (640, 110, (60, 120, 50))]):
        ridge = base + ridge_line(w, amp)
        tex = noise(h, w, 7, 8)[..., None] * 60 - 30
        mask = (y >= ridge[None, :])
        shade = np.clip((y - ridge[None, :]) / 300, 0, 1)[..., None]
        layer = np.array(col, float) + tex - shade * 30
        haze = 0.45 - i * 0.18
        layer = layer * (1 - haze) + np.array([190, 210, 235]) * haze
        img = np.where(mask[..., None], layer, img)
    # a winding river
    for yy in range(700, h):
        cx = w * 0.45 + 140 * np.sin(yy / 70) + (yy - 700) * 0.6
        half = 6 + (yy - 700) * 0.12
        x0, x1 = int(cx - half), int(cx + half)
        img[yy, max(0, x0):max(0, min(w, x1))] = img[yy, max(0, x0):max(0, min(w, x1))] * 0.3 + np.array([150, 190, 230]) * 0.7
    return np.clip(img + rng.normal(0, 3, img.shape), 0, 255).astype(np.uint8)

def coast(w=1600, h=1000):
    img = sky(h, w, (40, 40, 110), (255, 160, 110), 0.35)
    y = np.arange(h)[:, None]; x = np.arange(w)[None, :]
    sun = np.exp(-((x - 1050) ** 2 + (y - 520) ** 2) / (2 * 45 ** 2))
    glow = np.exp(-((x - 1050) ** 2 + (y - 520) ** 2) / (2 * 260 ** 2))
    img = img + (sun[..., None] * np.array([255, 240, 200]) + glow[..., None] * np.array([120, 60, 20]))
    sea_top = 560
    waves = streaks(h, w)
    sea = np.array([28, 45, 95], float) + waves[..., None] * 35
    depth = np.clip((y - sea_top) / (h - sea_top), 0, 1)
    width = 30 + depth * 260
    ref = np.exp(-((x - 1050) ** 2) / (2 * width ** 2)) * np.clip(waves * 1.6 - 0.35, 0, 1)
    sea = sea + ref[..., None] * np.array([255, 185, 120])
    img = np.where((y >= sea_top)[..., None], sea, img)
    # a dark headland on the left, sloping down into the sea
    xs = x[0].astype(float)
    top = 330 + ridge_line(w, 45) + np.clip((xs - 380) / 360, 0, 1) ** 1.6 * 900
    cliff = y >= top[None, :]
    rock = np.array([52, 40, 52], float) + noise(h, w, 7, 10)[..., None] * 35 - np.clip((y - top[None, :]) / 400, 0, 1)[..., None] * 25
    img = np.where(cliff[..., None], rock, img)
    return np.clip(img + rng.normal(0, 3, img.shape), 0, 255).astype(np.uint8)

def tree(w=900, h=1200):
    img = sky(h, w, (110, 165, 230), (225, 235, 245), 0.4)
    y = np.arange(h)[:, None]; x = np.arange(w)[None, :]
    ground = 980 + (noise(1, w, 5, 3)[0] - 0.5) * 60
    grass = np.array([70, 140, 60], float) + noise(h, w, 7, 20)[..., None] * 60 - 30
    img = np.where((y >= ground[None, :])[..., None], grass, img)
    trunk = (np.abs(x - 450 - (y - 1000) * 0.03) < 26 + (y - 600) * 0.03) & (y > 600) & (y < 1010)
    img = np.where(trunk[..., None], np.array([95, 65, 40], float) + noise(h, w, 6, 30)[..., None] * 30, img)
    leaves = noise(h, w, 7, 6)
    crown = ((x - 450) ** 2 / 330 ** 2 + (y - 430) ** 2 / 360 ** 2) < 0.55 + leaves * 0.6
    img = np.where(crown[..., None], np.array([40, 110, 45], float) + leaves[..., None] * 90 - 20, img)
    return np.clip(img + rng.normal(0, 3, img.shape), 0, 255).astype(np.uint8)

if __name__ == '__main__':
    Image.fromarray(valley()).save('media/Valley.jpg', quality=88, subsampling=2)
    Image.fromarray(coast()).save('media/Coast.jpg', quality=88, subsampling=2, progressive=True)
    # like a phone held upright: the pixels are stored on their side, the EXIF tag says "turn right"
    t = Image.fromarray(tree()).transpose(Image.Transpose.ROTATE_90)
    ex = Image.Exif(); ex[0x0112] = 6
    t.save('media/Tall tree (phone).jpg', quality=85, exif=ex.tobytes())
    # a rainbow over a see-through background; four frames that slide the colours along
    cols = [(239, 68, 68), (249, 115, 22), (234, 179, 8), (34, 197, 94), (59, 130, 246), (139, 92, 246)]
    frames = []
    for f in range(4):
        im = Image.new('P', (320, 180), 0)
        im.putpalette([255, 255, 255] + [v for c in cols for v in c] + [255, 255, 255] * (256 - 7))
        y, x = np.mgrid[0:180, 0:320]
        r = np.hypot(x - 160, y - 175)
        band = ((150 - r) // 14).astype(int)
        ok = (r < 150) & (r >= 66)
        idx = np.where(ok, (band + f) % 6 + 1, 0).astype(np.uint8)
        im.putdata(idx.flatten().tolist())
        frames.append(im)
    frames[0].save('media/Rainbow.gif', save_all=True, append_images=frames[1:], duration=150, loop=0, transparency=0)
