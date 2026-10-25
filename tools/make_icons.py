#!/usr/bin/env python3
"""Draws the app icons (webos/icon.png 80x80, webos/largeIcon.png 130x130).

An original design: a small house under a round tree on a green hill at dusk.
It deliberately uses none of the game's logos or artwork.

Usage: python3 tools/make_icons.py   (needs Pillow)
"""
from pathlib import Path

from PIL import Image, ImageDraw

SCALE = 8  # draw large, then downsample for smooth edges
CANVAS = 130


def draw_icon() -> Image.Image:
    """Returns the icon at CANVAS * SCALE pixels."""
    size = CANVAS * SCALE
    image = Image.new("RGBA", (size, size))
    draw = ImageDraw.Draw(image)

    def box(x0: float, y0: float, x1: float, y1: float) -> tuple[int, int, int, int]:
        return (int(x0 * SCALE), int(y0 * SCALE), int(x1 * SCALE), int(y1 * SCALE))

    # Dusk sky: vertical gradient from warm orange to soft blue.
    top, bottom = (120, 170, 230), (250, 196, 140)
    for y in range(size):
        t = y / (size - 1)
        colour = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3))
        draw.line([(0, y), (size, y)], fill=colour + (255,))

    draw.ellipse(box(90, 16, 112, 38), fill=(255, 238, 170, 255))          # sun
    draw.ellipse(box(-40, 78, 170, 200), fill=(92, 168, 84, 255))           # hill
    draw.ellipse(box(-20, 96, 150, 220), fill=(70, 146, 66, 255))           # front hill

    # Tree: trunk and round crown.
    draw.rectangle(box(88, 66, 96, 96), fill=(120, 82, 52, 255))
    draw.ellipse(box(70, 34, 114, 76), fill=(52, 128, 60, 255))
    draw.ellipse(box(78, 40, 100, 58), fill=(78, 156, 78, 255))

    # House: walls, door, window, roof.
    draw.rectangle(box(30, 70, 72, 100), fill=(250, 236, 206, 255))
    draw.rectangle(box(46, 82, 56, 100), fill=(150, 96, 60, 255))
    draw.rectangle(box(60, 78, 68, 86), fill=(140, 200, 235, 255))
    draw.polygon([(26 * SCALE, 72 * SCALE), (51 * SCALE, 50 * SCALE), (76 * SCALE, 72 * SCALE)],
                 fill=(200, 72, 60, 255))

    # Rounded-corner mask, like the TV's other icons.
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, size - 1, size - 1), radius=22 * SCALE, fill=255)
    image.putalpha(mask)
    return image


def main() -> None:
    out_dir = Path(__file__).resolve().parent.parent / "webos"
    out_dir.mkdir(exist_ok=True)
    icon = draw_icon()
    icon.resize((130, 130), Image.LANCZOS).save(out_dir / "largeIcon.png")
    icon.resize((80, 80), Image.LANCZOS).save(out_dir / "icon.png")


if __name__ == "__main__":
    main()
