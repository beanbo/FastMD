"""Generates res/fastmd.ico: a rounded square with a blue→violet gradient, the Markdown mark (M + arrow) and a bolt."""
import pathlib

from PIL import Image, ImageDraw

OUT = pathlib.Path(__file__).resolve().parents[1] / "res" / "fastmd.ico"


def draw(size: int) -> Image.Image:
    s = size * 4  # supersample
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    grad = Image.new("RGBA", (s, s))
    gd = ImageDraw.Draw(grad)
    c1, c2 = (9, 105, 218), (130, 80, 223)
    for y in range(s):
        t = y / (s - 1)
        gd.line([(0, y), (s, y)], fill=tuple(int(a + (b - a) * t) for a, b in zip(c1, c2)) + (255,))
    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, s - 1, s - 1], radius=int(s * 0.22), fill=255)
    im.paste(grad, (0, 0), mask)
    d = ImageDraw.Draw(im)
    w = s
    # Markdown mark: "M" (left) + down arrow (right)
    lw = max(2, int(w * 0.085))
    x0, x1, top, bot = w * 0.17, w * 0.52, w * 0.30, w * 0.70
    mid = (x0 + x1) / 2
    d.line([(x0, bot), (x0, top), (mid, top + (bot - top) * 0.42), (x1, top), (x1, bot)], fill="white", width=lw,
           joint="curve")
    ax = w * 0.70
    d.line([(ax, top), (ax, bot - w * 0.06)], fill="white", width=lw)
    d.polygon([(ax - w * 0.12, bot - w * 0.13), (ax + w * 0.12, bot - w * 0.13), (ax, bot + w * 0.02)], fill="white")
    # small bolt in the top-right corner
    b = [(w * 0.80, w * 0.06), (w * 0.69, w * 0.23), (w * 0.77, w * 0.23), (w * 0.72, w * 0.36), (w * 0.86, w * 0.17),
         (w * 0.78, w * 0.17)]
    d.polygon(b, fill=(255, 214, 10, 255))
    return im.resize((size, size), Image.LANCZOS)


def main() -> None:
    OUT.parent.mkdir(parents=True, exist_ok=True)
    sizes = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
    big = draw(256)
    big.save(OUT, sizes=[(s, s) for s in sizes], append_images=[draw(s) for s in sizes])
    draw(256).save(OUT.with_suffix(".png"))
    print(OUT)


if __name__ == "__main__":
    main()
