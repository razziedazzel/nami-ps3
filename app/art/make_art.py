#!/usr/bin/env python3
"""Make the XMB icon and background. Usage: make_art.py <output dir> [title for the icon]"""
import math
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

FONT = "/usr/share/fonts/google-noto/NotoSans-SemiBold.ttf"
VIOLET = (139, 124, 246)
SCALE = 4


def icon(path, title):
    W, H = 320 * SCALE, 176 * SCALE
    mark = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(mark)
    # A play triangle inside an open ring, off to the left of the name.
    cx, cy, r = 62 * SCALE, 88 * SCALE, 46 * SCALE
    t = int(6.5 * SCALE)
    d.ellipse((cx - r, cy - r, cx + r, cy + r), outline=255, width=t)
    tr = 23 * SCALE
    d.polygon([(cx - tr * 0.55, cy - tr), (cx - tr * 0.55, cy + tr), (cx + tr * 1.05, cy)], fill=255)

    font = ImageFont.truetype(FONT, 50 * SCALE)
    box = d.textbbox((0, 0), title, font=font)
    while box[2] - box[0] > 186 * SCALE:
        font = ImageFont.truetype(FONT, font.size - SCALE)
        box = d.textbbox((0, 0), title, font=font)
    x = 124 * SCALE - box[0]
    y = (H - (box[3] - box[1])) // 2 - box[1] - 6 * SCALE
    words = Image.new("L", (W, H), 0)
    wd = ImageDraw.Draw(words)
    wd.text((x, y), title, font=font, fill=255)
    # A thin wave under the name: "nami" means wave.
    left, right = x + box[0], x + box[2]
    base = y + box[3] + 13 * SCALE
    points = [(px, base + 4.5 * SCALE * math.sin((px - left) / (right - left) * 2 * math.pi * 1.5))
              for px in range(left, right + 1, SCALE)]
    wd.line(points, fill=255, width=int(2.5 * SCALE), joint="curve")

    # White shapes over a violet glow, on transparency, the way XMB icons sit on the wave.
    both = np.maximum(np.asarray(mark), np.asarray(words))
    glow = Image.fromarray(both).filter(ImageFilter.GaussianBlur(9 * SCALE))
    glow_a = np.clip(np.asarray(glow, dtype=np.float32) / 255.0 * 1.7, 0, 1) * 0.85
    shape_a = both.astype(np.float32) / 255.0

    # White fading slightly to violet-white towards the bottom, for a little depth.
    grad = np.linspace(0, 1, H)[:, None]
    shade = np.dstack([255 - grad * 45 * np.ones((H, W)), 255 - grad * 55 * np.ones((H, W)), 255 - grad * 5 * np.ones((H, W))])
    glow_rgb = np.ones((H, W, 3)) * np.array(VIOLET)

    alpha = shape_a + glow_a * (1 - shape_a)
    safe = np.where(alpha > 0, alpha, 1)[:, :, None]
    rgb = (shade * shape_a[:, :, None] + glow_rgb * (glow_a * (1 - shape_a))[:, :, None]) / safe
    out = np.dstack([rgb, alpha[:, :, None] * 255]).clip(0, 255).astype(np.uint8)
    Image.fromarray(out, "RGBA").resize((320, 176), Image.LANCZOS).save(path)


def background(path):
    W, H = 1920, 1080
    xs = np.arange(W, dtype=np.float32)
    ys = np.arange(H, dtype=np.float32)[:, None]
    # Deep night violet, with a glow low down.
    v = ys / H
    base = np.zeros((H, W, 3), dtype=np.float32)
    base[:, :, 0] = 12 + 20 * v
    base[:, :, 1] = 10 + 12 * v
    base[:, :, 2] = 24 + 44 * v
    # XMB-style ribbons: soft crests of light that thin out away from the wave.
    for i, (center, amp, wave, phase, width, power) in enumerate([
        (0.72, 80, 1500, 0.2, 90, 0.16),
        (0.76, 110, 1900, 1.6, 150, 0.11),
        (0.70, 55, 1150, 3.0, 46, 0.22),
        (0.79, 70, 1300, 4.4, 24, 0.34),
        (0.74, 95, 1700, 5.2, 9, 0.42),
    ]):
        line = center * H + amp * np.sin(xs / wave * 2 * math.pi + phase) + 0.25 * amp * np.sin(xs / (wave * 0.37) * 2 * math.pi + phase * 2)
        dist = ys - line[None, :]
        light = np.exp(-(dist / width) ** 2) * power
        base += light[:, :, None] * np.array([139, 124, 246], dtype=np.float32) * 0.9
        base += (light ** 3)[:, :, None] * np.array([255, 255, 255], dtype=np.float32) * 0.5
    # A little fine noise, so the gradients don't band on a TV.
    rng = np.random.default_rng(7)
    base += rng.normal(0, 1.2, (H, W, 1))
    Image.fromarray(base.clip(0, 255).astype(np.uint8), "RGB").save(path, optimize=True)


def main():
    out = sys.argv[1]
    title = sys.argv[2] if len(sys.argv) > 2 else "Nami"
    icon(f"{out}/ICON0.PNG", title)
    background(f"{out}/PIC1.PNG")


if __name__ == "__main__":
    main()
