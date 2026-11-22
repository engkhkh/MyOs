import random, struct, os, sys, io
from PIL import Image, ImageOps
random.seed(int(sys.argv[1]))
os.makedirs('jcases', exist_ok=True)
def content(w, h):
    im = Image.new('RGB', (w, h)); px = im.load()
    kind = random.randrange(3)
    for y in range(h):
        for x in range(w):
            if kind == 0: px[x, y] = (random.randrange(256), random.randrange(256), random.randrange(256))
            elif kind == 1: px[x, y] = ((x * 7 + y) % 256, (y * 5) % 256, (x * y) % 256)
            else: px[x, y] = (128 + int(100 * ((x // 9 + y // 7) % 2)), (x * 255 // max(1, w - 1)), 200 if (x - w // 2) ** 2 + (y - h // 2) ** 2 < (min(w, h) // 3) ** 2 else 40)
    return im
n = 0
def save(im, opts, tag, ref=None):
    global n
    name = f'jcases/j{n:03d}_{tag}'
    n += 1
    im.save(name + '.jpg', 'JPEG', **opts)
    dec = Image.open(name + '.jpg')
    if ref == 'exif': dec = ImageOps.exif_transpose(dec)
    d = dec.convert('RGBA').tobytes()
    exp = [d[i+3] << 24 | d[i] << 16 | d[i+1] << 8 | d[i+2] for i in range(0, len(d), 4)]
    open(name + '.exp', 'wb').write(struct.pack(f'<{len(exp)}I', *exp))
for i in range(40):
    w, h = random.randint(1, 140), random.randint(1, 140)
    im = content(w, h)
    sub = random.choice([0, 1, 2])
    opts = dict(quality=random.choice([5, 30, 75, 90, 100]), subsampling=sub, progressive=random.random() < 0.5, optimize=random.random() < 0.5)
    if random.random() < 0.3:
        opts['restart_marker_blocks'] = random.randint(1, 5)
    tag = f's{sub}_p{int(opts["progressive"])}_{w}x{h}'
    if random.random() < 0.25: im = im.convert('L'); tag = 'grey_' + tag; opts.pop('subsampling')
    try: save(im, opts, tag)
    except TypeError:
        opts.pop('restart_marker_blocks', None); save(im, opts, tag)
# EXIF orientations
for o in range(1, 9):
    im = content(random.randint(20, 60), random.randint(20, 60))
    ex = Image.Exif(); ex[0x0112] = o
    save(im, dict(quality=90, exif=ex.tobytes()), f'exif{o}', ref='exif')
print(n, 'jpeg cases')
