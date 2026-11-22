import random, struct, os, sys
from PIL import Image
random.seed(int(sys.argv[1]))
os.makedirs('gcases', exist_ok=True)
for i in range(40):
    w, h = random.randint(1, 160), random.randint(1, 120)
    ncol = random.choice([2, 4, 16, 50, 256])
    im = Image.new('P', (w, h))
    pal = [random.randrange(256) for _ in range(768)]
    im.putpalette(pal)
    px = im.load()
    kind = random.randrange(3)
    for y in range(h):
        for x in range(w):
            px[x, y] = random.randrange(ncol) if kind == 0 else ((x // 3 + y // 5) % ncol if kind == 1 else (x * y // 7) % ncol)
    opts = {'interlace': random.random() < 0.5}
    if random.random() < 0.4: opts['transparency'] = random.randrange(ncol)
    if random.random() < 0.3:      # an animation: only the first frame counts
        im2 = im.copy(); im2.putpixel((0, 0), (px[0, 0] + 1) % ncol)
        opts['save_all'] = True; opts['append_images'] = [im2]
    name = f'gcases/g{i:02d}_{w}x{h}_c{ncol}_i{int(opts["interlace"])}'
    im.save(name + '.gif', **opts)
    r = Image.open(name + '.gif'); r.seek(0)
    d = r.convert('RGBA').tobytes()
    exp = [(d[k+3] << 24 | d[k] << 16 | d[k+1] << 8 | d[k+2]) if d[k+3] else 0 for k in range(0, len(d), 4)]
    open(name + '.exp', 'wb').write(struct.pack(f'<{len(exp)}I', *exp))
