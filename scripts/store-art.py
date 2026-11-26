#!/usr/bin/env python3
"""Draws the app store icons and banner into store/.

The icons are the menu icon's framed landscape, drawn smoothly at store
sizes. The banner shows store/screenshots/emery_1.png and gabbro_2.png (made
by scripts/store-screenshots.js) in simple watch outlines next to the name.

Usage: scripts/store-art.py
Needs Pillow (pip install pillow).
"""

import os

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STORE = os.path.join(ROOT, "store")

# The menu icon's colors, from Pebble's 64-color palette.
FRAME = (255, 170, 85)
SKY = (85, 170, 255)
SUN = (255, 255, 0)
HILL = (0, 170, 85)
HILL_DARK = (0, 85, 0)

BANNER_SIZE = (720, 320)
BANNER_TOP = (24, 24, 32)
BANNER_BOTTOM = (8, 8, 12)
BEZEL = (52, 52, 60)

FONTS = {
    "bold": ["/usr/share/fonts/opentype/inter/Inter-Bold.otf",
             "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
             "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"],
    "regular": ["/usr/share/fonts/opentype/inter/Inter-Regular.otf",
                "/System/Library/Fonts/Supplemental/Arial.ttf",
                "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"],
}


def font(style, size):
    for path in FONTS[style]:
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    return ImageFont.load_default(size)


def icon(size):
    """The framed landscape, drawn at 8x and scaled down for smooth edges."""
    s = size * 8
    unit = s / 25  # the 25px menu icon's grid
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)

    def p(x, y):
        return (x * unit, y * unit)

    d.rounded_rectangle([p(1, 3), p(24, 22)], radius=2 * unit, fill=FRAME)
    d.rectangle([p(3, 5), p(22, 20)], fill=SKY)
    d.ellipse([p(15, 6), p(20, 11)], fill=SUN)
    d.polygon([p(3, 20), p(3, 13), p(8.5, 8), p(14.5, 20)], fill=HILL_DARK)
    d.polygon([p(7.5, 20), p(14, 11.5), p(22, 16), p(22, 20)], fill=HILL)
    return im.resize((size, size), Image.LANCZOS)


def vertical_gradient(size, top, bottom):
    w, h = size
    column = Image.new("RGB", (1, h))
    for y in range(h):
        t = y / (h - 1)
        column.putpixel((0, y), tuple(round(a + (b - a) * t) for a, b in zip(top, bottom)))
    return column.resize(size)


def banner():
    scale = 4  # draw large for smooth outlines, then scale down
    w, h = BANNER_SIZE
    big = vertical_gradient((w * scale, h * scale), BANNER_TOP, BANNER_BOTTOM).convert("RGBA")
    d = ImageDraw.Draw(big)

    # Pebble Time 2 on the left, Pebble Round 2 on the right, each showing a
    # different photo.
    rect_shot = Image.open(os.path.join(STORE, "screenshots", "emery_1.png")).convert("RGBA")
    sw, sh = 170 * scale, 194 * scale
    sx, sy = 306 * scale, 63 * scale
    bezel = 12 * scale
    d.rounded_rectangle([sx - bezel, sy - bezel, sx + sw + bezel, sy + sh + bezel],
                        radius=22 * scale, fill=BEZEL)
    big.paste(rect_shot.resize((sw, sh), Image.NEAREST), (sx, sy))

    round_shot = Image.open(os.path.join(STORE, "screenshots", "gabbro_2.png")).convert("RGBA")
    rd = 190 * scale
    rx, ry = 512 * scale, 65 * scale
    d.ellipse([rx - bezel, ry - bezel, rx + rd + bezel, ry + rd + bezel], fill=BEZEL)
    mask = Image.new("L", (rd, rd), 0)
    ImageDraw.Draw(mask).ellipse([0, 0, rd - 1, rd - 1], fill=255)
    big.paste(round_shot.resize((rd, rd), Image.NEAREST), (rx, ry), mask)

    im = big.resize(BANNER_SIZE, Image.LANCZOS).convert("RGB")
    d = ImageDraw.Draw(im)
    logo = icon(56)
    im.paste(logo, (30, 70), logo)
    d.text((30, 136), "WristFrame", font=font("bold", 44), fill=(255, 255, 255))
    d.text((32, 192), "Your photos, full screen", font=font("regular", 20), fill=(200, 200, 210))
    d.text((32, 218), "on your wrist.", font=font("regular", 20), fill=(200, 200, 210))
    d.text((32, 262), "Pebble Time 2 · Pebble Round 2", font=font("regular", 14), fill=(140, 140, 155))
    return im


def main():
    for size in (48, 80, 144):
        path = os.path.join(STORE, "icon-{}.png".format(size))
        icon(size).save(path, optimize=True)
        print(os.path.relpath(path, ROOT))
    path = os.path.join(STORE, "banner.png")
    banner().save(path, optimize=True)
    print(os.path.relpath(path, ROOT))


if __name__ == "__main__":
    main()
