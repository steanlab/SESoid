#!/usr/bin/env python3
# SES V300 (not badge154): builds the boot splash bitmaps for the 720 x 256 landscape picture (black background, white text)
# and writes ble_splash_data.h. Run once on a PC with Pillow installed; the generated header is committed, the firmware
# needs neither Pillow nor a font. Repository: steanlab/SESoid, tools/make_splash.py -> ble_splash_data.h next to v300_ble_probe.slcp.
#
#   python tools/make_splash.py [--preview splash_preview.png]
#
# Text is rendered with DejaVu Sans (Bitstream Vera licence: free to embed rendered glyph bitmaps).
# Title letters have their own colour (per column code 1 white / 2 yellow / 3 red in spl_title_col).
# Storage: column-major, 1 bit per pixel, for every picture column u (0..W-1) ceil(H/8) bytes, bit k = row k of the block (1 = white).
import argparse, os, re, sys
import math
from PIL import Image, ImageDraw, ImageFont, ImageFilter, ImageOps

W_PANEL, H_PANEL = 720, 256
TITLE = "GECKO SLIDE"
OUTLINE = 3                      # width of the white contour of the title letters, pixels
FONT_DIR = "/usr/share/fonts/truetype/dejavu/"
TITLE_FONT = FONT_DIR + "DejaVuSans-Bold.ttf"
SUB_FONT = FONT_DIR + "DejaVuSansCondensed-Bold.ttf"


def render(text, path, size, spacing=0):
    f = ImageFont.truetype(path, size)
    # draw glyph by glyph so that letter spacing can be added; 4x supersampling and a threshold give clean 1-bit shapes
    S = 4
    fb = ImageFont.truetype(path, size * S)
    adv = [fb.getlength(c) for c in text]
    total = int(sum(adv) / S + spacing * (len(text) - 1)) + 8
    asc, desc = fb.getmetrics()
    big = Image.new("L", (total * S, (asc + desc) + 8 * S), 0)
    d = ImageDraw.Draw(big)
    x = 4.0 * S
    starts = []
    for c, a in zip(text, adv):
        starts.append(x / S)
        d.text((x, 4 * S), c, font=fb, fill=255)
        x += a + spacing * S
    starts.append(x / S - spacing)
    small = big.resize((big.width // S, big.height // S), Image.LANCZOS).point(lambda v: 255 if v >= 128 else 0)
    bbox = small.getbbox()
    render.spans = [(starts[i] - bbox[0], starts[i + 1] - bbox[0]) for i in range(len(text))]   # advance span of every glyph in cropped coordinates
    return small.crop(bbox)


def fit(text, path, max_w, size0, spacing=0):
    size = size0
    while size > 6:
        im = render(text, path, size, spacing)
        if im.width <= max_w:
            fit.spans = render.spans
            return im, size
        size -= 1
    raise SystemExit("cannot fit " + text)


def pack(im):
    w, h = im.size
    hb = (h + 7) // 8
    px = im.load()
    out = []
    for u in range(w):
        for b in range(hb):
            v = 0
            for k in range(8):
                y = b * 8 + k
                if y < h and px[u, y]:
                    v |= 1 << k
            out.append(v)
    return w, h, hb, out


def emit_array(f, name, data):
    f.write("static const uint8_t %s[%d] = {\n" % (name, len(data)))
    for i in range(0, len(data), 24):
        f.write("  " + ",".join("0x%02X" % b for b in data[i:i + 24]) + ",\n")
    f.write("};\n")


# ---- angular "cyberpunk" capitals drawn by hand (no font needed): grid 10 x 14 units, stroke 3, chamfered corners
GLYPHS = {
  "G": ([[(0,3),(3,0),(10,0),(10,3),(3,3),(3,11),(7,11),(7,9),(5,9),(5,6),(10,6),(10,14),(3,14),(0,11)]], 10),
  "E": ([[(0,3),(3,0),(10,0),(10,3),(3,3),(3,5.5),(8,5.5),(8,8.5),(3,8.5),(3,11),(10,11),(10,14),(3,14),(0,11)]], 10),
  "C": ([[(0,3),(3,0),(10,0),(10,3),(3,3),(3,11),(10,11),(10,14),(3,14),(0,11)]], 10),
  "K": ([[(0,3),(3,0),(3,5),(7,0),(10.5,0),(5.5,7),(10.5,14),(7,14),(3,9),(3,11),(3,14),(0,11)]], 10.5),
  "O": ([[(3,0),(7,0),(10,3),(10,11),(7,14),(3,14),(0,11),(0,3)], [(4,3),(6,3),(7,4),(7,10),(6,11),(4,11),(3,10),(3,4)]], 10),
  "S": ([[(0,3),(3,0),(10,0),(10,3),(3,3),(3,5.5),(10,5.5),(10,11),(7,14),(0,14),(0,11),(7,11),(7,8.5),(0,8.5)]], 10),
  "L": ([[(0,0),(3,0),(3,11),(10,11),(10,14),(3,14),(0,11)]], 10),
  "I": ([[(0,0),(3,0),(3,14),(0,14)]], 3),
  "D": ([[(0,0),(7,0),(10,3),(10,11),(7,14),(0,14)], [(3,3),(6,3),(7,4),(7,10),(6,11),(3,11)]], 10),
}


def cyber_title(text, unit, gap, space):
    """text in GLYPHS letters and spaces -> 1-bit image of the filled letters, `unit` pixels per grid unit"""
    S = 4
    items, x = [], 0.0
    for ch in text:
        if ch == " ":
            x += space
            continue
        polys, w = GLYPHS[ch]
        items.append((x, polys))
        x += w + gap
    total = x - gap
    im = Image.new("L", (int(total * unit * S) + 1, 14 * unit * S + 1), 0)
    d = ImageDraw.Draw(im)
    for x0, polys in items:
        for k, poly in enumerate(polys):
            pts = [((x0 + px) * unit * S, py * unit * S) for px, py in poly]
            d.polygon(pts, fill=255 if k == 0 else 0)
    return im.resize((im.width // S, im.height // S), Image.LANCZOS).point(lambda v: 255 if v >= 128 else 0)


ap = argparse.ArgumentParser()
ap.add_argument("--preview", default=None)
ap.add_argument("--style", default="split", choices=["chamfer", "slit", "round", "tron", "wide", "pixel", "glitch", "slant", "split"])
ap.add_argument("--gecko", default="none", choices=["none", "line", "inside", "both"])
ap.add_argument("--etch", action="store_true", help="solid letters with the gecko etched out of them")
ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "ble_splash_data.h"))
a = ap.parse_args()

MARGIN = 20
unit = 5
title = cyber_title(TITLE, unit, 3, 8)
while title.width > W_PANEL - 2 * MARGIN - 2 * OUTLINE and unit > 1:
    unit -= 1
    title = cyber_title(TITLE, unit, 3, 8)
tsize = unit
# contour letters: white outline OUTLINE pixels wide, the inside stays black like the background
def erode(im, r):
    return im.filter(ImageFilter.MinFilter(2 * r + 1)).point(lambda v: 255 if v else 0)

def sub_(x, y):
    return Image.composite(Image.new("L", x.size, 0), x, y)           # x without the pixels of y

PIX = {  # 5 x 7 pixel capitals
  "G": ["01111","10000","10000","10011","10001","10001","01111"], "E": ["11111","10000","10000","11110","10000","10000","11111"],
  "C": ["01111","10000","10000","10000","10000","10000","01111"], "K": ["10001","10010","10100","11000","10100","10010","10001"],
  "O": ["01110","10001","10001","10001","10001","10001","01110"], "S": ["01111","10000","10000","01110","00001","00001","11110"],
  "L": ["10000","10000","10000","10000","10000","10000","11111"], "I": ["11111","00100","00100","00100","00100","00100","11111"],
  "D": ["11110","10001","10001","10001","10001","10001","11110"]}
if a.style == "pixel":
    cell = 10; xs = 0; im = Image.new("L", (63 * cell, 7 * cell), 0); d = ImageDraw.Draw(im)
    for ch in TITLE:
        if ch == " ":
            xs += 4; continue
        for r, row in enumerate(PIX[ch]):
            for c, v in enumerate(row):
                if v == "1": d.rectangle(((xs + c) * cell, r * cell, (xs + c + 1) * cell - 1, (r + 1) * cell - 1), fill=255)
        xs += 6
    title = im.crop(im.getbbox())
if a.style == "wide":                       # a bold sans stretched into an extended techno face
    im, _ = fit(TITLE, TITLE_FONT, 1000, 80, spacing=2)
    wmax = W_PANEL - 2 * MARGIN - 2 * OUTLINE
    title = im.resize((wmax, int(im.height * 0.95)), Image.LANCZOS).point(lambda v: 255 if v >= 128 else 0)

INTER_BLACK = "/usr/share/fonts/opentype/inter/Inter-Black.otf"
def split_title():
    """solid Inter Black capitals, tight spacing, upper half and lower half shifted sideways (style "split")"""
    S = 3; size = 84; sp = -3; space = -8
    fb = ImageFont.truetype(INTER_BLACK, size * S)
    adv = [fb.getlength(c) for c in TITLE]
    w = int(sum(adv) / S + sp * (len(TITLE) - 1) + 40) + 20
    asc, desc = fb.getmetrics()
    big = Image.new("L", (w * S, asc + desc + 20 * S), 0); d = ImageDraw.Draw(big); x = 10.0 * S
    for c, ad in zip(TITLE, adv):
        d.text((x, 10 * S), c, font=fb, fill=255); x += ad + sp * S - (space * S if c == " " else 0)
    im = big.resize((big.width // S, big.height // S), Image.LANCZOS).point(lambda v: 255 if v >= 128 else 0)
    im = im.crop(im.getbbox())
    im = im.resize((int(im.width * 1.15), int(im.height * 1.12)), Image.LANCZOS).point(lambda v: 255 if v >= 128 else 0)
    maxw = W_PANEL - 2 * MARGIN - 14
    if im.width > maxw:
        im = im.resize((maxw, int(im.height * maxw / im.width)), Image.LANCZOS).point(lambda v: 255 if v >= 128 else 0)
    sh = 14; cut = im.height // 2
    out = Image.new("L", (im.width + sh, im.height), 0)
    out.paste(im.crop((0, 0, im.width, cut)), (0, 0)); out.paste(im.crop((0, cut, im.width, im.height)), (sh, cut))
    return out.crop(out.getbbox())
if a.style == "split":
    title = split_title()
fill = title
if a.style == "slant":
    sh = 0.32; fill = fill.transform((fill.width + int(fill.height * sh) + 2, fill.height), Image.AFFINE, (1, sh, 0, 0, 1, 0), Image.BICUBIC).point(lambda v: 255 if v >= 128 else 0)
    fill = ImageOps.mirror(fill) if False else fill
if a.style == "glitch":
    import random; rnd = random.Random(7); fill = fill.copy(); w_, h_ = fill.size; y = 0
    big = Image.new("L", (w_ + 24, h_), 0); big.paste(fill, (12, 0)); out = Image.new("L", big.size, 0)
    while y < h_:
        bh = rnd.choice([4, 6, 9, 14]); dx = rnd.choice([0, 0, 0, -9, -5, 6, 10]); out.paste(big.crop((0, y, big.width, y + bh)), (dx, y)); y += bh
    fill = out
if a.style == "round":                      # rounded corners: blur and threshold
    fill = fill.filter(ImageFilter.GaussianBlur(3.2)).point(lambda v: 255 if v >= 128 else 0)
if a.style == "slit":                       # horizontal cuts through the letters
    fill = fill.copy(); d = ImageDraw.Draw(fill)
    for fy_ in (0.30, 0.60):
        yy = int(fill.height * fy_); d.rectangle((0, yy, fill.width, yy + 3), fill=0)
title = fill if a.style == "split" else sub_(fill, erode(fill, OUTLINE))     # the contour (the split style is solid)
if a.style == "tron":                        # a second thin line inside
    ring = sub_(erode(fill, OUTLINE + 3), erode(fill, OUTLINE + 5))
    title = Image.composite(Image.new("L", title.size, 255), title, ring)
verh = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'ble_sys.h')).read()
ver = '%s.%s' % (re.search(r'FW_VERSION_MAJOR\s+(\d+)', verh).group(1), re.search(r'FW_VERSION_MINOR\s+(\d+)', verh).group(1))
foot, fsize = fit('built by steanlab, ver ' + ver, SUB_FONT, W_PANEL - 2 * MARGIN, 22)
fy = H_PANEL - MARGIN - foot.height
ty = (H_PANEL - title.height) // 2 - 8
tx = (W_PANEL - title.width) // 2
fx = tx                                       # the footer starts at the same left edge as the title
print("title %dx%d at (%d,%d) unit %d; footer %dx%d at (%d,%d) font %d" % (title.width, title.height, tx, ty, tsize, foot.width, foot.height, fx, fy, fsize))

def gecko_mask(x0, x1, cy, amp=30, lam=300, rmax=16, leg=34):
    """winding gecko silhouette (spine = sine, tapering body, head on the right, 4 legs, tail on the left)"""
    S = 2
    m = Image.new("L", (W_PANEL * S, H_PANEL * S), 0); d = ImageDraw.Draw(m)
    L = x1 - x0
    def sp(t):                                   # t 0 (tail) .. 1 (head)
        x = x0 + L * t
        return x, cy + amp * math.sin(2 * math.pi * (x - x0) / lam + 0.6)
    def rad(t):
        if t < 0.35: return 2 + (rmax - 2) * (t / 0.35) ** 1.2          # tail grows into the body
        if t < 0.88: return rmax - 3 * abs(t - 0.6)
        return rmax * 0.95 - (t - 0.88) / 0.12 * 3                      # neck, then the head
    N = 400
    pts = [sp(i / N) for i in range(N + 1)]
    for i, (x, y) in enumerate(pts):
        r = rad(i / N); d.ellipse(((x - r) * S, (y - r) * S, (x + r) * S, (y + r) * S), fill=255)
    hx, hy = pts[-1]; dx = pts[-1][0] - pts[-6][0]; dy = pts[-1][1] - pts[-6][1]; n = math.hypot(dx, dy); dx, dy = dx / n, dy / n
    hr = rmax * 0.85                              # head: an oval a little ahead of the neck
    cxh, cyh = hx + dx * hr * 0.9, hy + dy * hr * 0.9
    d.ellipse(((cxh - hr * 1.15) * S, (cyh - hr * 0.8) * S, (cxh + hr * 1.15) * S, (cyh + hr * 0.8) * S), fill=255)
    for t, side in ((0.78, -1), (0.78, 1), (0.42, -1), (0.42, 1)):          # legs: shoulder -> knee -> foot with toes
        x, y = sp(t); ang = math.atan2(*(lambda a, b: (b[1] - a[1], b[0] - a[0]))(sp(t - 0.01), sp(t + 0.01)))
        nx, ny = -math.sin(ang) * side, math.cos(ang) * side
        tx_, ty_ = math.cos(ang), math.sin(ang)
        kx, ky = x + nx * leg * 0.62 - tx_ * 8, y + ny * leg * 0.62 - ty_ * 8
        fx_, fy_ = kx + nx * leg * 0.3 + tx_ * 14, ky + ny * leg * 0.3 + ty_ * 14
        d.line(((x * S, y * S), (kx * S, ky * S), (fx_ * S, fy_ * S)), fill=255, width=9 * S)
        for k in (-1, 0, 1):
            ex, ey = fx_ + nx * 6 + tx_ * 7 * k, fy_ + ny * 6 + ty_ * 7 * k
            d.line(((fx_ * S, fy_ * S), (ex * S, ey * S)), fill=255, width=4 * S)
    return m.resize((W_PANEL, H_PANEL), Image.LANCZOS).point(lambda v: 255 if v >= 128 else 0)

def dilate(im, r):
    return im.filter(ImageFilter.MaxFilter(2 * r + 1)).point(lambda v: 255 if v else 0)

img = Image.new("L", (W_PANEL, H_PANEL), 0)
img.paste(title, (tx, ty))
if a.etch:
    fillc = Image.new("L", (W_PANEL, H_PANEL), 0); fillc.paste(fill, (tx, ty))
    cols = [fillc.crop((x, 0, x + 1, H_PANEL)).getbbox() is not None for x in range(W_PANEL)]
    runs = []; x = 0
    while x < W_PANEL:                                   # find the widest empty run inside the title = the word gap
        if not cols[x] and tx < x < tx + title.width:
            y = x
            while y < W_PANEL and not cols[y]: y += 1
            runs.append((y - x, x, y)); x = y
        else: x += 1
    gap = max(runs)
    x0, x1 = tx + 2, gap[1] - 3                           # the word GECKO only
    g = gecko_mask(x0, x1, ty + title.height // 2, amp=5, lam=170, rmax=8, leg=26)
    ring = sub_(g, erode(g, 2))                           # 2 px gecko outline
    eye = Image.new("L", g.size, 0)
    img = sub_(fillc, ring)                               # cut black out of the letters
    bb = img.getbbox(); title = img.crop(bb); tx, ty = bb[0], bb[1]
    img = Image.new("L", (W_PANEL, H_PANEL), 0); img.paste(title, (tx, ty))
if a.gecko != "none":
    fillc = Image.new("L", (W_PANEL, H_PANEL), 0); fillc.paste(fill, (tx, ty))
    g = gecko_mask(tx - 8, tx + title.width + 8, ty + title.height // 2 + 4)
    inner = erode(fillc, OUTLINE + 2)                     # letter interiors, with a black gap to the contour
    out = img
    if a.gecko in ("inside", "both"):
        out = Image.composite(Image.new("L", out.size, 255), out, Image.composite(g, Image.new("L", g.size, 0), inner))
    if a.gecko in ("line", "both"):
        free = sub_(g, dilate(fillc, 3))                  # solid gecko behind the letters, 3 px black gap around them
        out = Image.composite(Image.new("L", out.size, 255), out, free)
    img = out
    bb = img.crop((0, 0, W_PANEL, fy - 2)).getbbox()      # the title picture now includes the gecko
    title = img.crop(bb); tx, ty = bb[0], bb[1]
    img = Image.new("L", (W_PANEL, H_PANEL), 0); img.paste(title, (tx, ty))
img.paste(foot, (fx, fy))
if a.preview:
    rgb = Image.new("RGB", (W_PANEL, H_PANEL), (0, 0, 0))
    COL = {1: (255, 255, 255)}
    tp = title.load(); rp = rgb.load(); fp = foot.load()
    for u in range(title.width):
        for v in range(title.height):
            if tp[u, v]:
                rp[tx + u, ty + v] = COL[1]
    for u in range(foot.width):
        for v in range(foot.height):
            if fp[u, v]:
                rp[fx + u, fy + v] = COL[1]
    rgb.save(a.preview)

tw, th, thb, tdata = pack(title)
fw_, fh, fhb, fdata = pack(foot)
with open(a.out, "w") as f:
    f.write("/* GENERATED by tools/make_splash.py - do not edit. SES V300 boot splash, 720 x 256 landscape picture, white contour title and white text on black. */\n")
    f.write("#ifndef BLE_SPLASH_DATA_H\n#define BLE_SPLASH_DATA_H\n#include <stdint.h>\n\n")
    for nm, (x, y, w, h, hb) in (("TITLE", (tx, ty, tw, th, thb)), ("FOOT", (fx, fy, fw_, fh, fhb))):
        f.write("#define SPL_%s_X %d\n#define SPL_%s_Y %d\n#define SPL_%s_W %d\n#define SPL_%s_H %d\n#define SPL_%s_HB %d\n" % (nm, x, nm, y, nm, w, nm, h, nm, hb))
    f.write("\n")
    emit_array(f, "spl_title", tdata)
    emit_array(f, "spl_foot", fdata)
    f.write("\n#endif\n")
print("wrote", os.path.normpath(a.out), "(%d + %d bytes)" % (len(tdata), len(fdata)))
