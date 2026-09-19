"""UI smoke test for FastMD: drives the real window with posted input / WM_COMMAND, checks the clipboard and saves
client-area screenshots to tests/out/. Usage: python app/tests/ui_smoke.py [path-to-FastMD.exe]"""
import ctypes
import ctypes.wintypes as wt
import pathlib
import shutil
import subprocess
import sys
import time

from PIL import Image

HERE = pathlib.Path(__file__).resolve().parent
EXE = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else HERE.parent / "build" / "Release" / "FastMD.exe"
OUT = HERE / "out"

u32 = ctypes.WinDLL("user32", use_last_error=True)
gdi = ctypes.WinDLL("gdi32")
k32 = ctypes.WinDLL("kernel32")
u32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))

WM_KEYDOWN, WM_SYSKEYDOWN, WM_CHAR, WM_COMMAND = 0x100, 0x104, 0x102, 0x111
WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP = 0x200, 0x201, 0x202
MK_LBUTTON = 1
CMD = {"COPY": 100, "SELECT_ALL": 101, "OPEN": 102, "RELOAD": 103, "EDIT": 104, "FOLDER": 105, "FIND": 106,
       "THEME_SYSTEM": 107, "THEME_LIGHT": 108, "THEME_DARK": 109, "ZOOM_IN": 110, "ZOOM_OUT": 111,
       "ZOOM_RESET": 112, "BACK": 113, "FORWARD": 114}

u32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u32.GetDC.restype = wt.HDC
u32.GetDC.argtypes = [wt.HWND]
gdi.CreateCompatibleDC.restype = wt.HDC
gdi.CreateCompatibleDC.argtypes = [wt.HDC]
gdi.CreateCompatibleBitmap.restype = wt.HBITMAP
gdi.CreateCompatibleBitmap.argtypes = [wt.HDC, ctypes.c_int, ctypes.c_int]
gdi.SelectObject.restype = wt.HGDIOBJ
gdi.SelectObject.argtypes = [wt.HDC, wt.HGDIOBJ]
u32.PrintWindow.argtypes = [wt.HWND, wt.HDC, wt.UINT]
u32.ReleaseDC.argtypes = [wt.HWND, wt.HDC]
gdi.GetDIBits.argtypes = [wt.HDC, wt.HBITMAP, wt.UINT, wt.UINT, ctypes.c_void_p, ctypes.c_void_p, wt.UINT]
gdi.DeleteObject.argtypes = [wt.HGDIOBJ]
gdi.DeleteDC.argtypes = [wt.HDC]
k32.GlobalLock.restype = ctypes.c_void_p
k32.GlobalLock.argtypes = [wt.HGLOBAL]
k32.GlobalUnlock.argtypes = [wt.HGLOBAL]
u32.GetClipboardData.restype = wt.HANDLE


def lp(x, y):
    return (int(y) << 16) | (int(x) & 0xFFFF)


def find_window(pid, timeout=5.0):
    found = []
    cb_t = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

    def cb(h, _):
        p = wt.DWORD()
        u32.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid and u32.IsWindowVisible(h):
            found.append(h)
        return True

    end = time.time() + timeout
    while time.time() < end:
        found.clear()
        u32.EnumWindows(cb_t(cb), 0)
        if found:
            return found[0]
        time.sleep(0.05)
    raise RuntimeError("window not found")


def shot(hwnd, name):
    r = wt.RECT()
    u32.GetClientRect(hwnd, ctypes.byref(r))
    w, h = r.right, r.bottom
    sdc = u32.GetDC(None)
    mdc = gdi.CreateCompatibleDC(sdc)
    bmp = gdi.CreateCompatibleBitmap(sdc, w, h)
    old = gdi.SelectObject(mdc, bmp)
    u32.PrintWindow(hwnd, mdc, 3)  # PW_CLIENTONLY | PW_RENDERFULLCONTENT
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


def clipboard():
    for _ in range(20):
        if u32.OpenClipboard(None):
            break
        time.sleep(0.05)
    try:
        h = u32.GetClipboardData(13)
        if not h:
            return ""
        p = k32.GlobalLock(h)
        s = ctypes.wstring_at(p)
        k32.GlobalUnlock(h)
        return s
    finally:
        u32.CloseClipboard()


def post(hwnd, msg, wp=0, lpv=0, wait=0.06):
    u32.PostMessageW(hwnd, msg, wp, lpv)
    time.sleep(wait)


def cmd(hwnd, name, wait=0.25):
    post(hwnd, WM_COMMAND, CMD[name], 0, wait)


def click(hwnd, x, y, wait=0.3):
    post(hwnd, WM_MOUSEMOVE, 0, lp(x, y), 0.02)
    post(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp(x, y), 0.02)
    post(hwnd, WM_LBUTTONUP, 0, lp(x, y), wait)


def drag(hwnd, x0, y0, x1, y1):
    post(hwnd, WM_MOUSEMOVE, 0, lp(x0, y0), 0.02)
    post(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp(x0, y0), 0.02)
    for k in range(1, 9):
        post(hwnd, WM_MOUSEMOVE, MK_LBUTTON, lp(x0 + (x1 - x0) * k / 8, y0 + (y1 - y0) * k / 8), 0.02)
    post(hwnd, WM_LBUTTONUP, 0, lp(x1, y1), 0.15)


def find_color(img, rgb, box, tol=24):
    """first pixel close to rgb inside box (l, t, r, b), scanning rows top-down"""
    px = img.load()
    l, t, r, b = box
    for y in range(t, min(b, img.height)):
        for x in range(l, min(r, img.width)):
            p = px[x, y]
            if sum(abs(p[i] - rgb[i]) for i in range(3)) < tol:
                return x, y
    return None


def check(name, cond, info=""):
    print(f"[{'OK' if cond else 'FAIL'}] {name}" + (f": {info}" if info else ""))
    return cond


def window_rect(hwnd):
    r = wt.RECT()
    u32.GetWindowRect(hwnd, ctypes.byref(r))
    return (r.left, r.top, r.right, r.bottom)


def close_and_wait(proc, hwnd):
    post(hwnd, 0x0010, 0, 0, 0.1)  # WM_CLOSE → the viewer saves its placement
    try:
        proc.wait(5)
    except subprocess.TimeoutExpired:
        proc.kill()


def test_placement(doc):
    """last closed window's rect is restored; a second window cascades; maximized state is restored"""
    ok = True
    u32.SetWindowPos.argtypes = [wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wt.UINT]
    a = subprocess.Popen([str(EXE), "--light", str(doc)])
    ha = find_window(a.pid)
    time.sleep(0.5)
    target = (180, 140, 180 + 900, 140 + 680)
    u32.SetWindowPos(ha, None, target[0], target[1], target[2] - target[0], target[3] - target[1], 0x0014)  # NOZORDER|NOACTIVATE
    time.sleep(0.3)
    close_and_wait(a, ha)

    b = subprocess.Popen([str(EXE), "--light", str(doc)])
    hb = find_window(b.pid)
    time.sleep(0.4)
    rb = window_rect(hb)
    ok &= check("placement restored after reopen", all(abs(rb[i] - target[i]) <= 1 for i in range(4)), f"{rb} vs {target}")
    c = subprocess.Popen([str(EXE), "--light", str(doc)])
    hc = find_window(c.pid)
    time.sleep(0.4)
    rc = window_rect(hc)
    ok &= check("second window cascades", rc[0] == rb[0] + 28 and rc[1] == rb[1] + 28 and rc[2] - rc[0] == rb[2] - rb[0],
                f"{rc} vs {rb}")
    close_and_wait(c, hc)
    u32.ShowWindow(hb, 3)  # SW_MAXIMIZE
    time.sleep(0.3)
    close_and_wait(b, hb)

    d = subprocess.Popen([str(EXE), "--light", str(doc)])
    hd = find_window(d.pid)
    time.sleep(0.4)
    ok &= check("maximized state restored", bool(u32.IsZoomed(hd)))
    u32.ShowWindow(hd, 9)  # SW_RESTORE
    time.sleep(0.3)
    rd = window_rect(hd)
    ok &= check("restore returns to the saved normal rect", all(abs(rd[i] - target[i]) <= 1 for i in range(4)), f"{rd}")
    u32.SetWindowPos(hd, None, 120, 100, 1016, 839, 0x0014)  # leave a sane placement behind
    time.sleep(0.2)
    close_and_wait(d, hd)
    return ok


def main():
    OUT.mkdir(exist_ok=True)
    doc = OUT / "features.md"
    shutil.copy(HERE / "features.md", doc)
    shutil.copy(HERE / "other.md", OUT / "other.md")
    proc = subprocess.Popen([str(EXE), "--light", "--size=1000x800", "--zoom=100", str(doc)])
    ok = True
    try:
        hwnd = find_window(proc.pid)
        time.sleep(0.8)
        img = shot(hwnd, "01-initial")

        # selection by drag + Ctrl+C (via WM_COMMAND)
        drag(hwnd, 180, 150, 700, 175)
        shot(hwnd, "02-drag-selection")
        cmd(hwnd, "COPY")
        c = clipboard()
        ok &= check("drag selection copies text", len(c) > 10, repr(c[:80]))

        # double click selects a word
        post(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp(200, 96), 0.01)
        post(hwnd, WM_LBUTTONUP, 0, lp(200, 96), 0.01)
        post(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp(200, 96), 0.01)
        post(hwnd, WM_LBUTTONUP, 0, lp(200, 96), 0.2)
        cmd(hwnd, "COPY")
        c = clipboard()
        ok &= check("double click selects one word", 0 < len(c) < 40 and " " not in c.strip(), repr(c))

        # select all + copy
        cmd(hwnd, "SELECT_ALL")
        cmd(hwnd, "COPY")
        c = clipboard()
        ok &= check("select all copies the document", "Конец документа." in c and "\tСтатус\t" in c, f"{len(c)} chars")
        post(hwnd, WM_KEYDOWN, 0x1B)  # Esc clears the selection

        # find
        cmd(hwnd, "FIND")
        for ch in "таблиц":
            post(hwnd, WM_CHAR, ord(ch), 0, 0.02)
        time.sleep(0.3)
        shot(hwnd, "03-find")
        post(hwnd, WM_KEYDOWN, 0x0D, 0, 0.3)  # Enter → next match
        shot(hwnd, "04-find-next")
        post(hwnd, WM_KEYDOWN, 0x1B, 0, 0.2)  # Esc closes find

        # anchor link (first link in the document): scroll to "Таблицы"
        post(hwnd, WM_KEYDOWN, 0x24, 0, 0.5)  # Home
        img = shot(hwnd, "05-top")
        pt = find_color(img, (0x09, 0x69, 0xDA), (150, 205, 900, 260))
        ok &= check("found a link on screen", pt is not None, str(pt))
        if pt:
            click(hwnd, pt[0] + 3, pt[1] + 2, 0.8)
            shot(hwnd, "06-after-anchor")

        # zoom
        cmd(hwnd, "ZOOM_IN")
        cmd(hwnd, "ZOOM_IN", 0.4)
        shot(hwnd, "07-zoom")
        cmd(hwnd, "ZOOM_RESET", 0.4)

        # dark theme
        post(hwnd, WM_KEYDOWN, 0x24, 0, 0.5)
        cmd(hwnd, "THEME_DARK", 0.4)
        img = shot(hwnd, "08-dark")
        bg = img.getpixel((10, 400))
        ok &= check("dark theme background", sum(bg) < 120, str(bg))
        cmd(hwnd, "THEME_LIGHT", 0.4)

        # relative .md link → other.md, then Back
        img = shot(hwnd, "09-top-light")
        second = find_color(img, (0x09, 0x69, 0xDA), (150, 262, 900, 320))
        if second:
            second = (second[0] + 3, second[1] + 2)
        ok &= check("found the relative .md link", second is not None, str(second))
        if second:
            click(hwnd, *second, 0.8)
            shot(hwnd, "10-other-doc")
            title = ctypes.create_unicode_buffer(256)
            u32.GetWindowTextW(hwnd, title, 256)
            ok &= check("navigated to other.md", title.value.startswith("other.md"), title.value)
            post(hwnd, WM_SYSKEYDOWN, 0x25, 0, 0.8)  # Alt+Left
            u32.GetWindowTextW(hwnd, title, 256)
            ok &= check("back to features.md", title.value.startswith("features.md"), title.value)

        # live reload
        with open(doc, "a", encoding="utf-8") as f:
            f.write("\n\n# Добавлено снаружи\n\nЭтот абзац дописан во время теста.\n")
        time.sleep(1.0)
        cmd(hwnd, "SELECT_ALL")
        cmd(hwnd, "COPY")
        c = clipboard()
        ok &= check("live reload picked up the change", "дописан во время теста" in c)
        post(hwnd, WM_KEYDOWN, 0x23, 0, 0.8)  # End
        shot(hwnd, "11-reloaded-end")
    finally:
        proc.kill()
    ok &= test_placement(doc)
    print("RESULT", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
