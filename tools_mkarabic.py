#!/usr/bin/env python3
"""Makes arabic_font.h: 8x16 bitmaps of the Arabic presentation forms (U+FE70..U+FEFF,
the shapes a letter takes at the start, middle or end of a word, and the lam-alef
ligatures) plus the Arabic digits and punctuation, regular and bold, from DejaVu Sans.
Each glyph is drawn at its own width and squeezed into an 8-pixel cell, so the joining
strokes meet the cell edges and letters connect. The baseline is on row 12, like the
Latin font. Run once; the generated header is part of the source."""
import sys
from PIL import Image, ImageDraw, ImageFont

REG = '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'
BOLD = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
EXTRA = [0x060C, 0x061B, 0x061F, 0x0640] + list(range(0x0660, 0x066A))   # ، ؛ ؟ tatweel, digits

def cell(font, cp, thr):
    adv = font.getlength(chr(cp))
    if adv < 1:
        return [0] * 16
    im = Image.new('L', (int(adv) + 8, 32), 0)
    ImageDraw.Draw(im).text((4, 20), chr(cp), font=font, fill=255, anchor='ls')
    box = im.crop((4, 0, 4 + max(1, round(adv)), 32)).resize((8, 32), Image.BILINEAR).crop((0, 8, 8, 24))
    rows = []
    for y in range(16):
        b = 0
        for x in range(8):
            if box.getpixel((x, y)) > thr:
                b |= 0x80 >> x
        rows.append(b)
    return rows

def table(path, thr):
    f = ImageFont.truetype(path, 14)
    return [cell(f, cp, thr) for cp in list(range(0xFE70, 0xFF00)) + EXTRA]

out = ['/* arabic_font.h - made by tools_mkarabic.py from DejaVu Sans (do not edit by hand).',
       ' * Glyph i is U+FE70 + i for i < 144, then: ' + ' '.join('U+%04X' % c for c in EXTRA) + ' */',
       '#define ARABIC_GLYPHS %d' % (144 + len(EXTRA))]
for name, path, thr in [('arabic_regular', REG, 110), ('arabic_bold', BOLD, 120)]:
    out.append('static const unsigned char %s[ARABIC_GLYPHS][16] = {' % name)
    for g in table(path, thr):
        out.append('    {' + ','.join('0x%02X' % b for b in g) + '},')
    out.append('};')
open(sys.argv[1] if len(sys.argv) > 1 else 'arabic_font.h', 'w').write('\n'.join(out) + '\n')
