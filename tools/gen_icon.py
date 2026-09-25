#!/usr/bin/env python3
"""Draws the TetherDesk app icon (two linked screens) and writes the macOS
.icns and Windows .ico files. Build-time tool; outputs are checked in."""
import os
from PIL import Image, ImageDraw

root = os.path.join(os.path.dirname(__file__), "..", "packaging")
S = 1024
img = Image.new("RGBA", (S, S), (0, 0, 0, 0))

# Background: rounded square with a vertical gradient.
bg = Image.new("RGBA", (S, S))
for y in range(S):
    t = y / S
    c = (int(22 + 10 * t), int(30 + 12 * t), int(52 + 30 * t), 255)
    ImageDraw.Draw(bg).line([(0, y), (S, y)], fill=c)
mask = Image.new("L", (S, S), 0)
ImageDraw.Draw(mask).rounded_rectangle([40, 40, S - 40, S - 40], radius=200, fill=255)
img.paste(bg, (0, 0), mask)

d = ImageDraw.Draw(img)
def screen(x, y, w, h, fill, alpha=255):
    layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ld = ImageDraw.Draw(layer)
    ld.rounded_rectangle([x, y, x + w, y + h], radius=56, fill=fill + (alpha,))
    ld.rounded_rectangle([x + 44, y + 44, x + w - 44, y + h - 44], radius=24, fill=(13, 17, 23, alpha))
    img.alpha_composite(layer)

screen(190, 230, 470, 350, (72, 145, 255))
screen(370, 430, 470, 350, (120, 178, 255), 235)
# The "tether": a link between the two screens.
d.line([(430, 405), (600, 405)], fill=(230, 235, 243, 255), width=34)
d.ellipse([405, 380, 455, 430], fill=(230, 235, 243, 255))
d.ellipse([575, 380, 625, 430], fill=(230, 235, 243, 255))

img.save(os.path.join(root, "tetherdesk.png"))
img.save(os.path.join(root, "macos", "tetherdesk.icns"))
img.save(os.path.join(root, "windows", "tetherdesk.ico"),
         sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
print("icons written")
