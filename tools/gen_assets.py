"""Preprocess AI pixel art into LVGL-ready assets.

The image generator is told to use a flat magenta backdrop, but it never emits
exactly #FF00FF, so the key colour is sampled from the border and removed with a
border-connected flood fill (interior pinks survive).

- garden_bg: key -> crop to the drawn circle -> fit 360x360 -> round mask
- frog sprites: key -> drop watermark -> crop -> resize
"""
from collections import namedtuple
import os

import numpy as np
from PIL import Image

SRC = os.path.join(os.path.dirname(__file__), "..", "assets", "generated")
DST = os.path.join(os.path.dirname(__file__), "..", "assets", "ui")
os.makedirs(DST, exist_ok=True)

HARD = 42.0   # colour distance below which keyed pixels go fully transparent
SOFT = 105.0  # colour distance below which a pixel may join the backdrop region

Keyp = namedtuple("Keyp", "rgb alpha")


def _border_color(rgb):
    border = np.concatenate([rgb[0], rgb[-1], rgb[:, 0], rgb[:, -1]])
    return np.median(border, axis=0)


def _flood_from_border(region):
    """4-connected pixels of `region` reachable from the image border."""
    reach = np.zeros_like(region)
    reach[0, :], reach[-1, :], reach[:, 0], reach[:, -1] = (
        region[0, :], region[-1, :], region[:, 0], region[:, -1])
    while True:
        grown = reach.copy()
        grown[1:, :] |= reach[:-1, :]
        grown[:-1, :] |= reach[1:, :]
        grown[:, 1:] |= reach[:, :-1]
        grown[:, :-1] |= reach[:, 1:]
        grown &= region
        grown &= ~reach
        if not grown.any():
            return reach
        reach |= grown


def key_out(rgb):
    """Remove the flat backdrop, returning rgb + alpha planes."""
    bg = _border_color(rgb)
    dist = np.sqrt(((rgb.astype(np.float32) - bg) ** 2).sum(-1))

    keyed = _flood_from_border(dist < SOFT)

    ramp = np.clip((dist - HARD) / (SOFT - HARD), 0.0, 1.0)
    alpha = np.where(keyed, ramp * 255.0, 255.0).astype(np.uint8)

    # Unmix the backdrop bleed from the semi-transparent rim.
    rim = (alpha > 0) & (alpha < 255)
    a = (alpha.astype(np.float32) / 255.0)[..., None]
    unbled = np.clip((rgb.astype(np.float32) - (1.0 - a) * bg) / np.maximum(a, 1e-3), 0, 255)
    rgb = np.where(rim[..., None], unbled, rgb.astype(np.float32)).astype(np.uint8)

    return Keyp(rgb, alpha)


def to_rgba(keyed):
    return Image.fromarray(np.dstack([keyed.rgb, keyed.alpha]), "RGBA")


def clear_zone(keyed, x0, y0):
    """Blank out a watermark area given in fractions of the image."""
    h, w = keyed.alpha.shape
    keyed.alpha[int(y0 * h):, int(x0 * w):] = 0


def content_bbox(alpha, pad=4):
    ys, xs = np.nonzero(alpha > 24)
    if xs.size == 0:
        return 0, 0, alpha.shape[1], alpha.shape[0]
    h, w = alpha.shape
    return (max(0, xs.min() - pad), max(0, ys.min() - pad),
            min(w, xs.max() + 1 + pad), min(h, ys.max() + 1 + pad))


def fit(img, box, size):
    """Resize into `box` (x, y, w, h) of a size x size RGBA canvas."""
    canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    scaled = img.resize((box[2], box[3]), Image.LANCZOS)
    canvas.paste(scaled, (box[0], box[1]), scaled)
    return canvas


def make_bg():
    src = Image.open(os.path.join(SRC, "garden_bg.png")).convert("RGB")
    keyed = key_out(np.asarray(src))
    x0, y0, x1, y1 = content_bbox(keyed.alpha)
    side = max(x1 - x0, y1 - y0)
    img = to_rgba(keyed).crop((x0, y0, x0 + side, y0 + side))

    # Overscan by 3% so the geometric round mask never exposes backdrop slivers.
    size = 360
    draw = size + int(size * 0.03)
    img = img.resize((draw, draw), Image.LANCZOS)
    canvas = Image.new("RGBA", (size, size), (0, 0, 0, 255))
    canvas.paste(img, ((size - draw) // 2, (size - draw) // 2), img)

    yy, xx = np.mgrid[0:size, 0:size]
    r = np.sqrt((xx - (size - 1) / 2.0) ** 2 + (yy - (size - 1) / 2.0) ** 2)
    outside = r > size / 2.0 - 0.5
    out = np.asarray(canvas).copy()
    out[outside] = (0, 0, 0, 255)
    Image.fromarray(out, "RGBA").save(os.path.join(DST, "garden_bg.png"), optimize=True)
    print("garden_bg.png", size, size, "circle px", int((~outside).sum()))


def make_sprite(src_name, dst_name, size, watermark=(0.70, 0.90), max_side=None):
    src = Image.open(os.path.join(SRC, src_name)).convert("RGB")
    keyed = key_out(np.asarray(src))
    if watermark:
        clear_zone(keyed, *watermark)
    x0, y0, x1, y1 = content_bbox(keyed.alpha)
    img = to_rgba(keyed).crop((x0, y0, x1, y1))
    w, h = img.size
    if max_side:
        img = img.crop(((w - max_side) // 2, (h - max_side) // 2,
                        (w - max_side) // 2 + max_side, (h - max_side) // 2 + max_side))
        w = h = max_side
    img = img.resize((round(w * size / h), size), Image.LANCZOS)
    img.save(os.path.join(DST, dst_name), optimize=True)
    print(dst_name, img.size)


def make_icons(src_name, names, size):
    """Split a 2x2 icon sheet (quadrant order = names) into keyed sprites."""
    src = Image.open(os.path.join(SRC, src_name)).convert("RGB")
    plane = np.asarray(src)
    h, w = plane.shape[:2]
    for idx, (row, col, out) in enumerate(names):
        quad = plane[row * h // 2:(row + 1) * h // 2, col * w // 2:(col + 1) * w // 2]
        keyed = key_out(quad)
        if out == "spd":                      # watermark sits in the sheet's bottom-right
            clear_zone(keyed, 0.55, 0.86)
        x0, y0, x1, y1 = content_bbox(keyed.alpha)
        img = to_rgba(keyed).crop((x0, y0, x1, y1))
        side = max(img.size)
        img = img.resize((round(img.size[0] * size / side), size), Image.LANCZOS)
        img.save(os.path.join(DST, out + ".png"), optimize=True)
        print(out + ".png", img.size)


if __name__ == "__main__":
    make_bg()
    make_sprite("frog_home.png", "frog_home.png", 120)
    make_sprite("frog_angry.png", "frog_angry.png", 120)
    make_sprite("clover.png", "clover.png", 26, max_side=900)
    make_icons("dock_icons.png",
               [(0, 0, "icon_shop"), (0, 1, "icon_go"),
                (1, 0, "icon_card"), (1, 1, "icon_spd")], 30)
