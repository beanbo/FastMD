"""Explorer preview handler check (plan 5.2): register it, create it through COM, show a document, measure, clean up.

Everything it touches is under HKCU and is removed again at the end, so the machine is left as it was found.

    python app/tests/preview_check.py [--build DIR]
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import pathlib
import time
import winreg

from PIL import Image

HERE = pathlib.Path(__file__).resolve().parent
OUT = HERE / "out"
CLSID_TEXT = "{6F1A2C34-9B84-4B0E-9F6D-1C2E0A7B5D41}"
IID_IPREVIEW = "{8895b1c6-b41f-4c1c-a562-0d564250836f}"
IID_IINIT_FILE = "{b7d14566-0509-4cce-a71f-0a554233bd9b}"
IID_ITHUMB = "{e357fccd-a995-4576-b01f-234630154e96}"
THUMB_CLSID_TEXT = "{6F1A2C35-9B84-4B0E-9F6D-1C2E0A7B5D41}"
ole32 = ctypes.WinDLL("ole32")
u32 = ctypes.WinDLL("user32", use_last_error=True)
gdi = ctypes.WinDLL("gdi32")
ok_all = True


class BITMAP(ctypes.Structure):
    _fields_ = [("bmType", ctypes.c_long), ("bmWidth", ctypes.c_long), ("bmHeight", ctypes.c_long),
                ("bmWidthBytes", ctypes.c_long), ("bmPlanes", ctypes.c_ushort), ("bmBitsPixel", ctypes.c_ushort),
                ("bmBits", ctypes.c_void_p)]


class BIH2(ctypes.Structure):
    _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG), ("biPlanes", wt.WORD),
                ("biBitCount", wt.WORD), ("biCompression", wt.DWORD), ("biSizeImage", wt.DWORD),
                ("biXPelsPerMeter", wt.LONG), ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD),
                ("biClrImportant", wt.DWORD)]


class GUID(ctypes.Structure):
    _fields_ = [("a", ctypes.c_ulong), ("b", ctypes.c_ushort), ("c", ctypes.c_ushort), ("d", ctypes.c_ubyte * 8)]


def guid(text):
    g = GUID()
    if ole32.CLSIDFromString(text, ctypes.byref(g)) != 0:
        raise RuntimeError(f"bad guid {text}")
    return g


def check(name, cond, info=""):
    global ok_all
    ok_all &= bool(cond)
    print(f"[{'OK' if cond else 'FAIL'}] {name}" + (f": {info}" if info else ""))
    return bool(cond)


def call(obj, index, *args):
    """call method #index of the COM object's vtable; every argument goes as a machine word"""
    vtable = ctypes.cast(obj, ctypes.POINTER(ctypes.c_void_p))[0]
    fn_ptr = ctypes.cast(vtable, ctypes.POINTER(ctypes.c_void_p))[index]
    proto = ctypes.WINFUNCTYPE(ctypes.c_long, *([ctypes.c_void_p] * (1 + len(args))))
    keep, words = [], []
    for a in args:
        if isinstance(a, int):
            words.append(ctypes.c_void_p(a))
        elif isinstance(a, str):
            buf = ctypes.create_unicode_buffer(a)
            keep.append(buf)
            words.append(ctypes.cast(buf, ctypes.c_void_p))
        else:
            words.append(ctypes.c_void_p(ctypes.addressof(a)))
    return proto(fn_ptr)(obj, *words) & 0xFFFFFFFF


def shot(hwnd, w, h, name):
    sdc = u32.GetDC(None)
    mdc = gdi.CreateCompatibleDC(sdc)
    bmp = gdi.CreateCompatibleBitmap(sdc, w, h)
    old = gdi.SelectObject(mdc, bmp)
    u32.PrintWindow(hwnd, mdc, 3)
    gdi.SelectObject(mdc, old)

    class BIH(ctypes.Structure):
        _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG), ("biPlanes", wt.WORD),
                    ("biBitCount", wt.WORD), ("biCompression", wt.DWORD), ("biSizeImage", wt.DWORD),
                    ("biXPelsPerMeter", wt.LONG), ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD),
                    ("biClrImportant", wt.DWORD)]

    bi = BIH(ctypes.sizeof(BIH), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi.GetDIBits(mdc, bmp, 0, h, buf, ctypes.byref(bi), 0)
    gdi.DeleteObject(bmp)
    gdi.DeleteDC(mdc)
    u32.ReleaseDC(None, sdc)
    img = Image.frombuffer("RGB", (w, h), buf, "raw", "BGRX", 0, 1)
    OUT.mkdir(exist_ok=True)
    img.save(OUT / f"{name}.png")
    return img


def registry_ok():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, rf"Software\Classes\CLSID\{CLSID_TEXT}\InprocServer32") as k:
            dll = winreg.QueryValueEx(k, "")[0]
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                            r"Software\Classes\.md\shellex\{8895b1c6-b41f-4c1c-a562-0d564250836f}") as k:
            handler = winreg.QueryValueEx(k, "")[0]
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                            r"Software\Classes\.md\shellex\{e357fccd-a995-4576-b01f-234630154e96}") as k:
            if winreg.QueryValueEx(k, "")[0] != THUMB_CLSID_TEXT:
                return None, None
        return dll, handler
    except OSError:
        return None, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=str(HERE.parent / "build" / "Release"))
    args = ap.parse_args()
    build = pathlib.Path(args.build)
    exe, dll = build / "FastMD.exe", build / "fastmd-preview.dll"
    check("5.2 the preview handler was built", dll.exists(), f"{dll.stat().st_size if dll.exists() else 0} bytes")
    if not dll.exists():
        return 1

    import subprocess
    subprocess.run([str(exe), "--register-preview"], check=False, timeout=60)
    got_dll, handler = registry_ok()
    check("5.2 registering puts it in HKCU, for .md and for the CLSID",
          got_dll and pathlib.Path(got_dll) == dll and handler == CLSID_TEXT, f"{got_dll} / {handler}")

    doc = OUT / "preview.md"
    doc.write_text("# Панель просмотра\n\nЭтот документ рисует тот же движок, что и окно программы.\n\n"
                   "| Колонка | Значение |\n|---|---|\n| Строка | Да |\n\n```python\nprint('код')\n```\n\n"
                   + "Абзац для длины. " * 80 + "\n\nКонец документа.\n", encoding="utf-8")

    ole32.CoInitializeEx(None, 2)
    obj = ctypes.c_void_p()
    hr = ole32.CoCreateInstance(ctypes.byref(guid(CLSID_TEXT)), None, 1, ctypes.byref(guid(IID_IINIT_FILE)),
                                ctypes.byref(obj))
    if not check("5.2 COM creates it from the registry alone", hr == 0 and obj, f"hr=0x{hr & 0xFFFFFFFF:08x}"):
        subprocess.run([str(exe), "--unregister-preview"], check=False, timeout=60)
        return 1
    call(obj, 3, str(doc), 0)  # IInitializeWithFile::Initialize

    prev = ctypes.c_void_p()
    iid = guid(IID_IPREVIEW)
    call(obj, 0, iid, prev)  # QueryInterface
    check("5.2 it answers as a preview handler", bool(prev))

    W, H = 700, 900
    host = u32.CreateWindowExW(0, "STATIC", "FastMD preview host", 0x90000000, 40, 40, W, H, None, None, None, None)
    rect = wt.RECT(0, 0, W, H)
    call(prev, 3, host, rect)  # SetWindow
    call(prev, 4, rect)                                        # SetRect
    t0 = time.perf_counter()
    hr = call(prev, 5)                                         # DoPreview
    ms = (time.perf_counter() - t0) * 1000
    check("5.2 the document is shown", hr == 0, f"hr=0x{hr:08x}")
    check("5.2 it is there in well under 100 ms", ms < 100, f"{ms:.1f} ms")

    msg = wt.MSG()
    deadline = time.time() + 0.5
    while time.time() < deadline and u32.PeekMessageW(ctypes.byref(msg), None, 0, 0, 1):
        u32.TranslateMessage(ctypes.byref(msg))
        u32.DispatchMessageW(ctypes.byref(msg))
    img = shot(host, W, H, "30-preview")
    px = img.load()
    ink = sum(1 for y in range(40, H - 40, 3) for x in range(20, W - 20, 3) if sum(px[x, y]) < 600)
    check("5.2 there is a document on it, not an empty pane", ink > 400, f"{ink} dark samples")


    # The pane has no worker threads, so pictures, formulas and diagrams are prepared while the document loads.
    # These two documents are small on purpose: anything drawn below the first line can only be the picture or the
    # formula itself.
    import shutil
    (OUT / "img").mkdir(exist_ok=True)
    shutil.copy(HERE.parents[1] / "bench" / "corpus" / "img" / "diagram0.png", OUT / "img" / "diagram0.png")
    for name, text, probe in (
            ("preview-pic", "# Картинка\n\n![схема](img/diagram0.png)\n",
             "5.2 a picture in the pane is really drawn"),
            ("preview-math", "# Формула\n\n$$\\frac{a}{b} = \\sqrt{2\\pi}$$\n",
             "5.2 a formula in the pane is really typeset")):
        small = OUT / f"{name}.md"
        small.write_text(text, encoding="utf-8")
        call(prev, 6)  # Unload
        call(obj, 3, str(small), 0)
        call(prev, 3, host, rect)
        call(prev, 4, rect)
        call(prev, 5)
        deadline = time.time() + 0.4
        while time.time() < deadline and u32.PeekMessageW(ctypes.byref(msg), None, 0, 0, 1):
            u32.TranslateMessage(ctypes.byref(msg))
            u32.DispatchMessageW(ctypes.byref(msg))
        img2 = shot(host, W, H, f"34-{name}")
        p2 = img2.load()
        bg = p2[W - 30, H - 30]
        drawn = sum(1 for y in range(90, 260) for x in range(20, 360)  # the band under the heading, pixel by pixel
                    if sum(abs(p2[x, y][i] - bg[i]) for i in range(3)) > 90)
        check(probe, drawn > 150, f"{drawn} pixels differ from the background")

    # ---- thumbnails (plan 5.3): the same DLL, its own class
    thumb = ctypes.c_void_p()
    hr = ole32.CoCreateInstance(ctypes.byref(guid(THUMB_CLSID_TEXT)), None, 1,
                                ctypes.byref(guid(IID_IINIT_FILE)), ctypes.byref(thumb))
    check("5.3 the thumbnail provider is registered too", hr == 0 and thumb, f"hr=0x{hr & 0xFFFFFFFF:08x}")
    if thumb:
        call(thumb, 3, str(doc), 0)  # Initialize
        tp = ctypes.c_void_p()
        iid_t = guid(IID_ITHUMB)
        call(thumb, 0, iid_t, tp)
        hbmp = ctypes.c_void_p()
        alpha = ctypes.c_ulong(0)
        t0 = time.perf_counter()
        hr = call(tp, 3, 256, hbmp, alpha)  # GetThumbnail
        tms = (time.perf_counter() - t0) * 1000
        check("5.3 a thumbnail comes back", hr == 0 and hbmp, f"hr=0x{hr:08x}, {tms:.1f} ms")
        if hbmp:
            bm = BITMAP()
            gdi.GetObjectW(hbmp, ctypes.sizeof(bm), ctypes.byref(bm))
            check("5.3 it is a page, not a square", bm.bmHeight == 256 and 150 < bm.bmWidth < 200,
                  f"{bm.bmWidth}x{bm.bmHeight}")
            w2, h2 = bm.bmWidth, bm.bmHeight
            buf = ctypes.create_string_buffer(w2 * h2 * 4)
            bi2 = BIH2(ctypes.sizeof(BIH2), w2, -h2, 1, 32, 0, 0, 0, 0, 0, 0)
            dc = u32.GetDC(None)
            gdi.GetDIBits(dc, hbmp, 0, h2, buf, ctypes.byref(bi2), 0)
            u32.ReleaseDC(None, dc)
            timg = Image.frombuffer("RGB", (w2, h2), buf, "raw", "BGRX", 0, 1)
            timg.save(OUT / "31-thumbnail.png")
            tpx = timg.load()
            dark = sum(1 for y in range(0, h2, 2) for x in range(0, w2, 2) if sum(tpx[x, y]) < 600)
            light = sum(1 for y in range(0, h2, 2) for x in range(0, w2, 2) if sum(tpx[x, y]) > 700)
            check("5.3 it looks like a light page with text on it", dark > 100 and light > dark,
                  f"{dark} dark, {light} light")
            gdi.DeleteObject(hbmp)
        call(tp, 2)
        call(thumb, 2)

    call(prev, 6)  # Unload
    call(prev, 2)  # Release
    call(obj, 2)
    u32.DestroyWindow(host)
    ole32.CoUninitialize()

    subprocess.run([str(exe), "--unregister-preview"], check=False, timeout=60)
    got_dll, handler = registry_ok()
    check("5.2 unregistering takes every key back out", got_dll is None and handler is None, f"{got_dll} / {handler}")
    print("RESULT", "PASS" if ok_all else "FAIL")
    return 0 if ok_all else 1


if __name__ == "__main__":
    raise SystemExit(main())
