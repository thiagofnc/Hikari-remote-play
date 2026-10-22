"""Builds the Windows icon and transparent logo variants from assets/hikari_logo.png.

    python scripts/make_icons.py

Outputs:
    assets/hikari.ico              multi-resolution app icon (16..256 px)
    assets/hikari_logo_256.png     transparent, tightly cropped (used by the app UI)
    assets/hikari_logo_mark.png    transparent, 512 px (README / docs)
"""

from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "assets" / "hikari_logo.png"


BRAND = (251, 187, 4)  # the logo's amber


def clean_alpha(img: Image.Image) -> Image.Image:
    """Normalises the logo to one flat brand colour with clean edges.

    The source is already transparent but carries faint stray speckles
    (alpha < ~20%) around the mark; those are dropped and the edge ramp is
    re-expanded so anti-aliasing stays smooth. Every visible pixel is painted
    the exact brand amber.
    """
    src = img.convert("RGBA")
    out = Image.new("RGBA", src.size, (0, 0, 0, 0))
    sp, op = src.load(), out.load()
    w, h = src.size
    for y in range(h):
        for x in range(w):
            a = sp[x, y][3] / 255.0
            cov = (a - 0.2) / 0.8
            if cov <= 0:
                continue
            op[x, y] = (*BRAND, min(255, round(cov * 255)))
    return out


def square_crop(img: Image.Image, pad_ratio: float = 0.04) -> Image.Image:
    bbox = img.getbbox()
    img = img.crop(bbox)
    side = int(max(img.size) * (1 + 2 * pad_ratio))
    canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    canvas.paste(img, ((side - img.width) // 2, (side - img.height) // 2), img)
    return canvas


def main() -> None:
    logo = square_crop(clean_alpha(Image.open(SRC)))
    big = logo.resize((512, 512), Image.LANCZOS)
    big.save(ROOT / "assets" / "hikari_logo_mark.png")
    logo.resize((256, 256), Image.LANCZOS).save(ROOT / "assets" / "hikari_logo_256.png")
    sizes = [(16, 16), (20, 20), (24, 24), (32, 32), (40, 40), (48, 48), (64, 64), (128, 128), (256, 256)]
    big.save(ROOT / "assets" / "hikari.ico", sizes=sizes)
    print("icons written")


if __name__ == "__main__":
    main()
