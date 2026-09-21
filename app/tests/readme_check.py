"""Renders a corpus of real READMEs (plan 2.10) and compares each one with the previous run.

    python app/tests/readme_check.py [--update] [path-to-FastMD.exe]

Downloads the list in readmes.txt into out/readmes (cached), opens each in a real window, saves the screenshot to
out/readme-shots and reports: how much of the page is not blank, whether anything is left as a placeholder, and how
many pixels differ from the accepted picture in out/readme-refs. --update accepts the current pictures as the new
reference. The comparison is against our own previous output: it catches changes we did not mean to make.
"""
import pathlib
import subprocess
import sys
import time
import urllib.request

from PIL import Image, ImageChops

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import ui_smoke as u  # noqa: E402

DOCS = u.OUT / "readmes"
SHOTS = u.OUT / "readme-shots"
REFS = u.OUT / "readme-refs"
UPDATE = "--update" in sys.argv
EXE = next((a for a in sys.argv[1:] if a.endswith(".exe")), str(HERE.parent / "build" / "Release" / "FastMD.exe"))


def fetch(url: str) -> pathlib.Path | None:
    name = url.rstrip("/").split("/")[-4] + "-" + url.rstrip("/").split("/")[-1]
    dst = DOCS / name
    if dst.exists() and dst.stat().st_size > 0:
        return dst
    try:
        with urllib.request.urlopen(url, timeout=20) as r:
            dst.write_bytes(r.read())
        return dst
    except Exception as e:  # noqa: BLE001 - the corpus is best effort, a missing README is not a failure
        print(f"  download failed: {name}: {e}")
        return None


def render(doc: pathlib.Path, name: str) -> Image.Image:
    proc = subprocess.Popen([EXE, "--light", "--zoom=100", "--size=1000x900", str(doc)], env=u.ENV)
    hwnd = u.find_window(proc.pid)
    time.sleep(4.0)  # remote pictures and badges have time to arrive
    u.OUT = SHOTS
    img = u.shot(hwnd, name)
    u.close_and_wait(proc, hwnd)
    return img


def main() -> int:
    for d in (DOCS, SHOTS, REFS):
        d.mkdir(parents=True, exist_ok=True)
    urls = [ln.strip() for ln in (HERE / "readmes.txt").read_text(encoding="utf-8").splitlines()
            if ln.strip() and not ln.startswith("#")]
    bad = 0
    print(f"{'document':34} {'ink %':>6} {'diff px':>9}")
    for url in urls:
        doc = fetch(url)
        if not doc:
            continue
        name = doc.stem
        img = render(doc, name).convert("RGB")
        px = img.load()
        ink = sum(1 for x in range(0, img.width, 4) for y in range(0, img.height, 4)
                  if sum(px[x, y]) < 720) / ((img.width // 4) * (img.height // 4)) * 100
        ref = REFS / f"{name}.png"
        diff = "-"
        if ref.exists() and not UPDATE:
            box = ImageChops.difference(Image.open(ref).convert("RGB"), img)
            diff = str(sum(1 for p in box.getdata() if p != (0, 0, 0)))
        if UPDATE or not ref.exists():
            img.save(ref)
            diff = "saved"
        blank = ink < 1.2  # a page with a logo and a couple of headings is around 2.5 %
        bad += blank
        print(f"{name:34} {ink:6.1f} {diff:>9}{'  BLANK PAGE' if blank else ''}")
    print("RESULT", "FAIL" if bad else "OK")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
