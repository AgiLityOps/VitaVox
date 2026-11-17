#!/usr/bin/env python3
"""Generates the LiveArea art in sce_sys/ and the in-app header logo.

LiveArea PNGs are 8-bit indexed, as the Vita requires (icon0 and bg have no
alpha; startup keeps a tRNS chunk for its rounded corners). pngquant is used
when it is on PATH; otherwise Pillow's quantizer is used.

Needs Pillow and the Manrope fonts in assets/fonts/.
"""
import os
import shutil
import subprocess

from PIL import Image, ImageDraw, ImageFont

ORANGE = (245, 166, 35)
DARK = (28, 20, 10)
WHITE = (240, 240, 240)
SUB = (170, 170, 175)
FONT = "assets/fonts/Manrope-700.ttf"
FONT_REG = "assets/fonts/Manrope-500.ttf"

# In-app header logo: same box as gfx_icon(IC_BOOK, 36, 38, 28, ...)
LOGO_PX = 28


def logo(S, bgfill=True):
    """Headphones around an open book, in a square of side S."""
    im = Image.new("RGBA", (S, S), ORANGE + (255,) if bgfill else (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    if not bgfill:
        d.ellipse([0, 0, S - 1, S - 1], fill=ORANGE)
    s = S / 512
    w = int(34 * s)
    d.arc([96 * s, 78 * s, 416 * s, 398 * s], start=180, end=360, fill=DARK, width=w)
    d.rectangle([96 * s, 238 * s, 96 * s + w, 300 * s], fill=DARK)
    d.rectangle([416 * s - w, 238 * s, 416 * s, 300 * s], fill=DARK)
    d.rounded_rectangle([70 * s, 270 * s, 140 * s, 390 * s], radius=int(26 * s), fill=DARK)
    d.rounded_rectangle([372 * s, 270 * s, 442 * s, 390 * s], radius=int(26 * s), fill=DARK)
    d.polygon([(160 * s, 236 * s), (250 * s, 256 * s), (250 * s, 386 * s), (160 * s, 366 * s)], fill=DARK)
    d.polygon([(262 * s, 256 * s), (352 * s, 236 * s), (352 * s, 366 * s), (262 * s, 386 * s)], fill=DARK)
    return im


def font(path, sz):
    return ImageFont.truetype(path, sz)


def save_vita_indexed(im, path, keep_alpha=False, colors=255):
    """Write an 8-bit indexed PNG. keep_alpha reserves a transparent palette index."""
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    pngquant = shutil.which("pngquant")
    if pngquant:
        tmp = path + ".src.png"
        if keep_alpha:
            im.convert("RGBA").save(tmp, format="PNG")
        else:
            im.convert("RGB").save(tmp, format="PNG")
        r = subprocess.run(
            [pngquant, "--force", "--quality=80-100", "--output", path, tmp],
            capture_output=True,
        )
        os.remove(tmp)
        if r.returncode == 0:
            if not keep_alpha:
                # Belt and braces: drop a tRNS chunk if pngquant added one.
                q = Image.open(path)
                if q.mode == "P" and "transparency" in q.info:
                    q.info.pop("transparency", None)
                    pixels = q.tobytes()
                    out = Image.new("P", q.size)
                    pal = q.getpalette() or []
                    out.putpalette((pal + [0, 0, 0] * 256)[:768])
                    out.frombytes(pixels)
                    out.save(path, format="PNG", optimize=True)
            return
        print("pngquant failed (%s); falling back to Pillow" % r.returncode)

    if not keep_alpha:
        q = im.convert("RGB").quantize(colors=colors, method=Image.Quantize.MEDIANCUT)
        q.info.pop("transparency", None)
        q.save(path, format="PNG", optimize=True)
        return

    rgba = im.convert("RGBA")
    alpha = rgba.getchannel("A")
    q = rgba.convert("RGB").quantize(colors=colors, method=Image.Quantize.MEDIANCUT)
    used = set(q.tobytes())
    trans = next(i for i in range(256) if i not in used)
    src_idx, a_bytes = q.tobytes(), alpha.tobytes()
    outb = bytearray(src_idx)
    for i, a in enumerate(a_bytes):
        if a < 128:
            outb[i] = trans
    p = Image.new("P", q.size)
    pal = q.getpalette() or []
    p.putpalette((pal + [0, 0, 0] * 256)[:768])
    p.frombytes(bytes(outb))
    p.save(path, format="PNG", optimize=True, transparency=trans)


def make_icon():
    return logo(1024).convert("RGB").resize((128, 128), Image.LANCZOS)


def make_startup():
    st = Image.new("RGBA", (280, 158), (0, 0, 0, 0))
    d = ImageDraw.Draw(st)
    d.rounded_rectangle([0, 0, 279, 157], radius=14, fill=(27, 30, 38, 255))
    c = logo(512, False).resize((84, 84), Image.LANCZOS)
    st.alpha_composite(c, (22, 37))
    d.text((120, 79), "Vita Vox", font=font(FONT, 34), fill=WHITE, anchor="lm")
    return st


def make_bg():
    bg = Image.new("RGB", (840, 500))
    px = bg.load()
    for y in range(500):
        t = y / 499
        col = tuple(int(a * (1 - t) + b * t) for a, b in zip((38, 28, 14), (18, 18, 20)))
        for x in range(840):
            px[x, y] = col
    bc = logo(512, False).resize((200, 200), Image.LANCZOS)
    bg.paste(bc, (590, 130), bc)
    d = ImageDraw.Draw(bg)
    d.text((62, 380), "Vita Vox", font=font(FONT, 52), fill=WHITE, anchor="ls")
    d.text((62, 442), "Audiobook Player for PS VITA", font=font(FONT_REG, 22), fill=SUB, anchor="ls")
    return bg


def make_inapp_logo():
    # Downscale from a large drawing so the 28 px mark stays sharp.
    return logo(512, False).resize((LOGO_PX, LOGO_PX), Image.LANCZOS)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)
    save_vita_indexed(make_icon(), "sce_sys/icon0.png", keep_alpha=False)
    save_vita_indexed(make_bg(), "sce_sys/livearea/contents/bg.png", keep_alpha=False)
    save_vita_indexed(make_startup(), "sce_sys/livearea/contents/startup.png", keep_alpha=True)
    os.makedirs("assets", exist_ok=True)
    make_inapp_logo().save("assets/logo.png", format="PNG")
    print("art written to sce_sys/ and assets/logo.png")


if __name__ == "__main__":
    main()
