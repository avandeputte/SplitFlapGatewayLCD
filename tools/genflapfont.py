#!/usr/bin/env python3
"""Generate src/fontflap.cpp/.h -- the BIG flap face for the LCD surface.

The Matrix Gateway's bitmap faces (font1252.cpp, from X11 misc-fixed BDFs) top out
at 10x20 -- honest for LED-matrix cells, comical in the 85x266 px cells the 10.1"
panel gives a 15x3 wall. This renders the SAME glyph set (order identical to
font1252, so FONT1252_INDEX and reelGlyph() work unchanged) from Helvetica Bold --
the typeface real split-flap boards used -- into a monospaced 1-bit face sized for
flap cards, byte-packed rows (MSB = leftmost), all glyphs on one shared baseline.

The 14 pictographs come from Apple Symbols (Helvetica has no heart), scaled to sit
like capitals.

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

BOX_W, BOX_H = 72, 100          # the fixed monospace flap box (fits an 85px cell + gutter)
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


def fit_size(path, index, chars):
    """Largest px size where every glyph's ink fits BOX_W wide and the face's
    ascent+descent fits BOX_H tall (shared baseline, no per-glyph shifting)."""
    best = 10
    for px in range(10, 200):
        f = ImageFont.truetype(path, px, index=index)
        asc, desc = f.getmetrics()
        if asc + desc > BOX_H:
            break
        wide = 0
        for ch in chars:
            bb = f.getbbox(ch)
            if bb:
                wide = max(wide, bb[2] - bb[0])
        if wide > BOX_W:
            break
        best = px
    return best


def render_glyph(f, ch, baseline):
    """One glyph in the box: horizontally centred ink, shared baseline."""
    img = Image.new("L", (BOX_W, BOX_H), 0)
    d = ImageDraw.Draw(img)
    bb = f.getbbox(ch)
    if not bb or bb[2] == bb[0]:
        return img                       # blank (space)
    x = (BOX_W - (bb[2] - bb[0])) // 2 - bb[0]
    d.text((x, baseline), ch, font=f, fill=255, anchor="ls")
    return img


def pack(img):
    bpr = (BOX_W + 7) // 8
    out = bytearray(bpr * BOX_H)
    px = img.load()
    for y in range(BOX_H):
        for x in range(BOX_W):
            if px[x, y] >= 128:
                out[y * bpr + (x >> 3)] |= 0x80 >> (x & 7)
    return bytes(out)


def main():
    printable = genfont.cp1252_printable()
    chars = [chr(cp) for _b, cp in printable]

    hidx = bold_index(HELV)
    px = fit_size(HELV, hidx, chars)
    f = ImageFont.truetype(HELV, px, index=hidx)
    asc, desc = f.getmetrics()
    baseline = (BOX_H - (asc + desc)) // 2 + asc
    print(f"helvetica bold(idx {hidx}) at {px}px, asc {asc} desc {desc}, baseline {baseline}")

    glyphs = [pack(render_glyph(f, ch, baseline)) for ch in chars]

    # Pictographs: Apple Symbols, scaled so they sit like capitals (cap-ish box).
    blanks = []
    for cp, name, _colour in genfont.EXTRA_GLYPHS:
        ch = chr(cp)
        spx = px
        while spx > 10:
            sf = ImageFont.truetype(SYMS, spx)
            bb = sf.getbbox(ch)
            if bb and bb[2] - bb[0] <= BOX_W and bb[3] - bb[1] <= int(asc * 1.05):
                break
            spx -= 2
        sf = ImageFont.truetype(SYMS, spx)
        img = Image.new("L", (BOX_W, BOX_H), 0)
        bb = sf.getbbox(ch)
        if not bb or bb[2] == bb[0]:
            blanks.append(name)
            glyphs.append(pack(img))
            continue
        x = (BOX_W - (bb[2] - bb[0])) // 2 - bb[0]
        # centre the symbol on the capital band rather than the text baseline
        y = baseline - asc + (asc - (bb[3] - bb[1])) // 2 - bb[1]
        ImageDraw.Draw(img).text((x, y), ch, font=sf, fill=255)
        glyphs.append(pack(img))
    if blanks:
        sys.exit(f"pictographs missing from Apple Symbols: {blanks}")

    # Validation, same spirit as genfont: every non-space glyph has ink, and no
    # accented capital collapsed onto its base letter.
    for i, (b, cp) in enumerate(printable):
        if cp != 0x20 and not any(glyphs[i]):
            sys.exit(f"glyph U+{cp:04X} rendered blank")
    for acc, base in ((0xC0, 0x41), (0xC8, 0x45), (0xD1, 0x4E), (0xE9, 0x65)):
        ia = next(i for i, (bb, cp) in enumerate(printable) if bb == acc)
        ib = next(i for i, (bb, cp) in enumerate(printable) if bb == base)
        if glyphs[ia] == glyphs[ib]:
            sys.exit(f"accent 0x{acc:02X} identical to base 0x{base:02X}")

    bpr = (BOX_W + 7) // 8
    n = len(glyphs)
    with open(OUT_H, "w") as h:
        h.write(f"""// fontflap.h -- GENERATED by tools/genflapfont.py; do not edit by hand.
// The BIG flap face for the LCD surface: Helvetica Bold rendered into a monospaced
// {BOX_W}x{BOX_H} 1-bit box, byte-packed rows (MSB = leftmost), one shared baseline.
// Glyph order is IDENTICAL to font1252 ({n} glyphs: CP1252 then the pictographs),
// so FONT1252_INDEX and reelGlyph() resolve indices for both faces.
#pragma once
#include <stdint.h>

#define FONTFLAP_W   {BOX_W}
#define FONTFLAP_H   {BOX_H}
#define FONTFLAP_BPR {bpr}
#define FONTFLAP_N   {n}

extern const uint8_t FONTFLAP_BITS[FONTFLAP_N][FONTFLAP_H * FONTFLAP_BPR];
""")
    with open(OUT_CPP, "w") as c:
        c.write("// fontflap.cpp -- GENERATED by tools/genflapfont.py; do not edit by hand.\n")
        c.write('#include "fontflap.h"\n\n')
        c.write("const uint8_t FONTFLAP_BITS[FONTFLAP_N][FONTFLAP_H * FONTFLAP_BPR] = {\n")
        for i, g in enumerate(glyphs):
            tag = (f"0x{printable[i][0]:02X}" if i < len(printable)
                   else genfont.EXTRA_GLYPHS[i - len(printable)][1])
            c.write(f"  {{ // {tag}\n    ")
            c.write(",".join(str(b) for b in g))
            c.write("\n  },\n")
        c.write("};\n")
    print(f"wrote {n} glyphs, {n * BOX_H * bpr} bytes -> src/fontflap.cpp")


if __name__ == "__main__":
    main()
