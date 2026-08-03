#!/usr/bin/env python3
"""Generate src/fontflap.cpp/.h -- the Helvetica flap faces for the LCD surface.

The Matrix Gateway's bitmap faces (font1252.cpp, from X11 misc-fixed BDFs) top out
at 10x20 -- honest for LED-matrix cells, comical in the cells a 1280x800 panel
affords. This renders the SAME glyph set (order identical to font1252, so
FONT1252_INDEX and reelGlyph() work unchanged) from Helvetica Bold -- the typeface
real split-flap boards used -- into monospaced 1-bit faces, byte-packed rows
(MSB = leftmost), all glyphs of a face on one shared baseline.

Two faces, largest first: BIG fits the 85px-wide cells of a 15-column wall, MED
the 40px cells of a 32-column one. dispPlan picks the largest that fits the cell.
The 14 pictographs come from Apple Symbols (Helvetica has no heart), scaled to
sit like capitals.

Run from the project root with a Pillow-equipped python:
    <venv>/bin/python tools/genflapfont.py
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import genfont  # cp1252_printable() + EXTRA_GLYPHS: the single source of glyph order

from PIL import Image, ImageDraw, ImageFont

FACES = [("BIG", 72, 100), ("MED", 32, 46)]   # (name, box_w, box_h), largest first
HELV = "/System/Library/Fonts/Helvetica.ttc"
SYMS = "/System/Library/Fonts/Apple Symbols.ttf"
OUT_CPP = os.path.join(ROOT, "src", "fontflap.cpp")
OUT_H = os.path.join(ROOT, "src", "fontflap.h")


def bold_index(path):
    for i in range(8):
        try:
            f = ImageFont.truetype(path, 24, index=i)
        except Exception:
            break
        if "bold" in " ".join(f.getname()).lower():
            return i
    return 0


def fit_size(path, index, chars, box_w, box_h):
    """Largest px size where every glyph's ink fits box_w wide and the face's
    ascent+descent fits box_h tall (shared baseline, no per-glyph shifting)."""
    best = 8
    for px in range(8, 200):
        f = ImageFont.truetype(path, px, index=index)
        asc, desc = f.getmetrics()
        if asc + desc > box_h:
            break
        wide = 0
        for ch in chars:
            bb = f.getbbox(ch)
            if bb:
                wide = max(wide, bb[2] - bb[0])
        if wide > box_w:
            break
        best = px
    return best


def pack(img, box_w, box_h):
    bpr = (box_w + 7) // 8
    out = bytearray(bpr * box_h)
    px = img.load()
    for y in range(box_h):
        for x in range(box_w):
            if px[x, y] >= 128:
                out[y * bpr + (x >> 3)] |= 0x80 >> (x & 7)
    return bytes(out)


def render_face(box_w, box_h, printable, hidx):
    chars = [chr(cp) for _b, cp in printable]
    px = fit_size(HELV, hidx, chars, box_w, box_h)
    f = ImageFont.truetype(HELV, px, index=hidx)
    asc, desc = f.getmetrics()
    baseline = (box_h - (asc + desc)) // 2 + asc

    glyphs = []
    for ch in chars:
        img = Image.new("L", (box_w, box_h), 0)
        bb = f.getbbox(ch)
        if bb and bb[2] != bb[0]:
            x = (box_w - (bb[2] - bb[0])) // 2 - bb[0]
            ImageDraw.Draw(img).text((x, baseline), ch, font=f, fill=255, anchor="ls")
        glyphs.append(pack(img, box_w, box_h))

    # Pictographs: Apple Symbols, scaled to sit like capitals.
    for cp, name, _colour in genfont.EXTRA_GLYPHS:
        ch = chr(cp)
        spx = px
        while spx > 8:
            sf = ImageFont.truetype(SYMS, spx)
            bb = sf.getbbox(ch)
            if bb and bb[2] - bb[0] <= box_w and bb[3] - bb[1] <= int(asc * 1.05):
                break
            spx -= 2
        sf = ImageFont.truetype(SYMS, spx)
        img = Image.new("L", (box_w, box_h), 0)
        bb = sf.getbbox(ch)
        if not bb or bb[2] == bb[0]:
            sys.exit(f"pictograph {name} missing from Apple Symbols")
        x = (box_w - (bb[2] - bb[0])) // 2 - bb[0]
        y = baseline - asc + (asc - (bb[3] - bb[1])) // 2 - bb[1]
        ImageDraw.Draw(img).text((x, y), ch, font=sf, fill=255)
        glyphs.append(pack(img, box_w, box_h))

    # Validation, same spirit as genfont: every non-space glyph has ink, and no
    # accented capital collapsed onto its base letter.
    for i, (b, cp) in enumerate(printable):
        if cp != 0x20 and not any(glyphs[i]):
            sys.exit(f"{box_w}x{box_h}: glyph U+{cp:04X} rendered blank")
    for acc, base in ((0xC0, 0x41), (0xC8, 0x45), (0xD1, 0x4E), (0xE9, 0x65)):
        ia = next(i for i, (bb, cp) in enumerate(printable) if bb == acc)
        ib = next(i for i, (bb, cp) in enumerate(printable) if bb == base)
        if glyphs[ia] == glyphs[ib]:
            sys.exit(f"{box_w}x{box_h}: accent 0x{acc:02X} identical to base 0x{base:02X}")

    print(f"  {box_w}x{box_h}: helvetica {px}px, asc {asc} desc {desc}, "
          f"{len(glyphs) * box_h * ((box_w + 7) // 8)} bytes")
    return glyphs


def main():
    printable = genfont.cp1252_printable()
    hidx = bold_index(HELV)
    n = len(printable) + len(genfont.EXTRA_GLYPHS)

    rendered = [(name, w, h, render_face(w, h, printable, hidx)) for name, w, h in FACES]

    with open(OUT_H, "w") as h:
        h.write(f"""// fontflap.h -- GENERATED by tools/genflapfont.py; do not edit by hand.
// The Helvetica flap faces for the LCD surface: monospaced 1-bit boxes, byte-packed
// rows (MSB = leftmost), one shared baseline per face. Glyph order is IDENTICAL to
// font1252 ({n} glyphs: CP1252 then the pictographs), so FONT1252_INDEX and
// reelGlyph() resolve indices for every face. FONTFLAP_FACES is largest-first;
// dispPlan picks the first face whose box fits the cell.
#pragma once
#include <stdint.h>

#define FONTFLAP_N        {n}
#define FONTFLAP_FACE_CNT {len(rendered)}
#define FONTFLAP_MAX_W    {max(w for _, w, _h, _g in rendered)}

struct FlapFace {{
  uint16_t       w, h, bpr;   // glyph box and bytes per packed row
  const uint8_t* bits;        // glyph gi at bits[gi * h * bpr]
}};
extern const FlapFace FONTFLAP_FACES[FONTFLAP_FACE_CNT];
""")

    with open(OUT_CPP, "w") as c:
        c.write("// fontflap.cpp -- GENERATED by tools/genflapfont.py; do not edit by hand.\n")
        c.write('#include "fontflap.h"\n\n')
        for name, w, hh, glyphs in rendered:
            c.write(f"static const uint8_t BITS_{name}[] = {{\n")
            for i, g in enumerate(glyphs):
                tag = (f"0x{printable[i][0]:02X}" if i < len(printable)
                       else genfont.EXTRA_GLYPHS[i - len(printable)][1])
                c.write(f"  // {tag}\n  ")
                c.write(",".join(str(b) for b in g))
                c.write(",\n")
            c.write("};\n\n")
        c.write("const FlapFace FONTFLAP_FACES[FONTFLAP_FACE_CNT] = {\n")
        for name, w, hh, _g in rendered:
            c.write(f"  {{ {w}, {hh}, {(w + 7) // 8}, BITS_{name} }},\n")
        c.write("};\n")
    print(f"wrote {len(rendered)} faces x {n} glyphs -> src/fontflap.cpp")


if __name__ == "__main__":
    main()
