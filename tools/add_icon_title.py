#!/usr/bin/env python3
"""Writes "Animal Crossing" in the grass of the app icons.

Reads x65943's artwork without lettering (tools/icon-art/icon.png 80x80,
largeIcon.png 130x130) and writes webos/icon.png and webos/largeIcon.png.
The lettering is drawn at SCALE times the icon's size and shrunk onto the
untouched artwork: two lines in a rounded typeface (Fredoka, SIL Open Font
Licence, tools/fonts/), cream with a dark-green outline and a soft shadow,
in the icon's own palette, centred in the grass below the house and tree.

Usage: python3 tools/add_icon_title.py   (needs Pillow with FreeType)
"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parent.parent
ART = ROOT / "tools" / "icon-art"
FONT = ROOT / "tools" / "fonts" / "Fredoka.ttf"
ICONS = ("icon.png", "largeIcon.png")

SCALE = 8
LINES = ("Animal", "Crossing")

# Layout on the 130-pixel icon, scaled for the 80-pixel one.
REFERENCE = 130
CENTRE_X = 65
TOP = 98.5             # the first line's top
LINE_HEIGHT = 14.5
FONT_SIZE = 16
STROKE = 1.5           # outline width
SHADOW = (0.0, 1.2)    # shadow offset
SHADOW_BLUR = 0.8

FILL = (255, 249, 232, 255)        # warm cream, like the clouds
OUTLINE = (52, 112, 56, 255)       # the grass's darker green
SHADOW_COLOUR = (30, 70, 34, 110)

# Fredoka is a variable font: weight 300-700, width 75-125.
WEIGHT = 650
WIDTH = 100


def load_font(size: float) -> ImageFont.FreeTypeFont:
    """Fredoka at @p size pixels, between semi-bold and bold."""
    face = ImageFont.truetype(str(FONT), int(round(size)))
    face.set_variation_by_axes([WEIGHT, WIDTH])
    return face


def lettering(icon_size: int) -> Image.Image:
    """The lettering layer for an icon of @p icon_size pixels."""
    k = icon_size / REFERENCE * SCALE
    size = icon_size * SCALE
    text = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    shadow = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    face = load_font(FONT_SIZE * k)
    stroke = int(round(STROKE * k))
    for i, line in enumerate(LINES):
        x = CENTRE_X * k
        y = (TOP + i * LINE_HEIGHT) * k
        ImageDraw.Draw(shadow).text((x + SHADOW[0] * k, y + SHADOW[1] * k), line, font=face, anchor="mt",
                                    fill=SHADOW_COLOUR, stroke_width=stroke, stroke_fill=SHADOW_COLOUR)
        ImageDraw.Draw(text).text((x, y), line, font=face, anchor="mt", fill=FILL, stroke_width=stroke, stroke_fill=OUTLINE)
    shadow = shadow.filter(ImageFilter.GaussianBlur(SHADOW_BLUR * k))
    return Image.alpha_composite(shadow, text).resize((icon_size, icon_size), Image.LANCZOS)


def main() -> None:
    for name in ICONS:
        art = Image.open(ART / name).convert("RGBA")
        icon = Image.alpha_composite(art, lettering(art.width))
        icon.putalpha(art.getchannel("A"))  # keep the artwork's rounded corners
        icon.save(ROOT / "webos" / name, optimize=True)
        print(f"wrote webos/{name} ({art.width}x{art.height})")


if __name__ == "__main__":
    main()
