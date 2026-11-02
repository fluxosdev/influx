#!/usr/bin/env python3
"""Render the SVG icon sources (Kora icon theme) and the inFlux logo to PNG.

Only needed when assets/icons/svg or assets/logo.svg change; the rendered
PNGs are committed so a normal build needs no Python/cairo at all.

    python3 tools/render_icons.py
"""
import os, sys
import cairosvg

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SVG_DIR = os.path.join(ROOT, "assets", "icons", "svg")
PNG_DIR = os.path.join(ROOT, "assets", "icons", "png")
SIZES = (32, 64, 128)

os.makedirs(PNG_DIR, exist_ok=True)
for name in sorted(os.listdir(SVG_DIR)):
    if not name.endswith(".svg"):
        continue
    base = name[:-4]
    src = os.path.join(SVG_DIR, name)
    for s in SIZES:
        out = os.path.join(PNG_DIR, f"{base}-{s}.png")
        cairosvg.svg2png(url=src, write_to=out, output_width=s, output_height=s)
    print("rendered", base)

logo = os.path.join(ROOT, "assets", "logo.svg")
for s in (64, 256):
    cairosvg.svg2png(url=logo, write_to=os.path.join(PNG_DIR, f"logo-{s}.png"),
                     output_width=s, output_height=s)
print("rendered logo")
