"""UI smoke test for FastMD: drives real windows with posted input / WM_COMMAND, reads state with WM_APP_QUERY, checks
the clipboard and saves client-area screenshots to tests/out/.
Runs in its own profile (FASTMD_REGKEY / FASTMD_DATA), so it never touches the reader's settings, window placement,
reading positions or recent documents.
Usage: python app/tests/ui_smoke.py [path-to-FastMD.exe]"""
import ctypes
import ctypes.wintypes as wt
import os
import pathlib
import shutil
import subprocess
import sys
import time
import winreg

from PIL import Image, ImageChops

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]
EXE = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else HERE.parent / "build" / "Release" / "FastMD.exe"
OUT = HERE / "out"
MEDIUM = REPO / "bench" / "corpus" / "medium.md"
REGKEY = r"Software\FastMD-uitest"
DATA = OUT / "profile"
ENV = dict(os.environ, FASTMD_REGKEY=REGKEY, FASTMD_DATA=str(DATA))

u32 = ctypes.WinDLL("user32", use_last_error=True)
gdi = ctypes.WinDLL("gdi32")
k32 = ctypes.WinDLL("kernel32")
u32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))

WM_KEYDOWN, WM_SYSKEYDOWN, WM_CHAR, WM_COMMAND, WM_CLOSE = 0x100, 0x104, 0x102, 0x111, 0x0010
WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP, WM_MOUSEWHEEL, WM_MOUSEHWHEEL = 0x200, 0x201, 0x202, 0x20A, 0x20E
WM_GETTEXT, WM_APP_QUERY = 0x000D, 0x8000 + 64
MK_LBUTTON, MK_SHIFT = 1, 4
CMD = {"COPY": 100, "SELECT_ALL": 101, "OPEN": 102, "RELOAD": 103, "EDIT": 104, "FOLDER": 105, "FIND": 106,
       "THEME_SYSTEM": 107, "THEME_LIGHT": 108, "THEME_DARK": 109, "ZOOM_IN": 110, "ZOOM_OUT": 111,
       "ZOOM_RESET": 112, "BACK": 113, "FORWARD": 114, "LINK_COPY": 115, "ASSOCIATE": 116, "TOC": 117,
       "COL_NARROW": 118, "COL_NORMAL": 119, "COL_WIDE": 120, "COL_FULL": 121, "COL_NARROWER": 122, "COL_WIDER": 123,
       "WRAP": 124, "SETTINGS": 125, "LINK_OPEN": 126, "IMG_COPY": 127, "IMG_OPEN": 128, "FIND_CASE": 129,
       "FIND_WORD": 130, "FIND_NEXT": 131, "FIND_PREV": 132, "FIND_CLOSE": 133, "LINK_NEXT": 134, "LINK_PREV": 135}
Q = {"SCROLLY": 1, "DOCH": 2, "TOC_OPEN": 3, "TOC_DOCKED": 4, "TOC_COUNT": 5, "TOC_CURRENT": 6, "TOC_ITEM_Y": 7,
     "HSCROLL_BLOCK": 8, "HSCROLL_X": 9, "FOCUS_LINK": 10, "MATCHES": 11, "CUR_MATCH": 12, "TEXT_LEFT": 13,
     "TEXT_W": 14, "RECENT_COUNT": 15, "FIND_EDIT": 16, "SETTINGS_HWND": 17, "FIND_OPEN": 18, "COLUMN": 19,
     "FONT_SIZE": 20, "WRAP": 21, "LANG": 22, "FIND_PART_X": 23, "BLOCK_Y": 24, "RESTORED": 25, "THEME_DARK": 26,
     "TARGETY": 27, "SETTINGS_HIT": 28, "SITKA": 29, "SETTINGS_BTN": 30, "IMG_SCALED": 31, "FULL_REDRAW": 32}
FP_NEXT, FP_CASE = 7, 3  # FindPart

u32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u32.SendMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u32.SendMessageW.restype = ctypes.c_ssize_t
u32.ClientToScreen.argtypes = [wt.HWND, ctypes.POINTER(wt.POINT)]
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


def windows_of(pid):
    found = []
    cb_t = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

    def cb(h, _):
        p = wt.DWORD()
        u32.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid and u32.IsWindowVisible(h):
            found.append(h)
        return True

    u32.EnumWindows(cb_t(cb), 0)
    return found


def find_window(pid, timeout=5.0):
    end = time.time() + timeout
    while time.time() < end:
        found = windows_of(pid)
        if found:
            return found[0]
        time.sleep(0.05)
    raise RuntimeError("window not found")


def title_of(h):
    buf = ctypes.create_unicode_buffer(256)
    u32.GetWindowTextW(h, buf, 256)
    return buf.value


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


def open_clipboard():
    for _ in range(20):
        if u32.OpenClipboard(None):
            return True
        time.sleep(0.05)
    return False


def clipboard():
    if not open_clipboard():
        return ""
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


def clipboard_has(fmt):
    if not open_clipboard():
        return False
    try:
        return bool(u32.IsClipboardFormatAvailable(fmt))
    finally:
        u32.CloseClipboard()


def post(hwnd, msg, wp=0, lpv=0, wait=0.06):
    u32.PostMessageW(hwnd, msg, wp, lpv)
    time.sleep(wait)


def cmd(hwnd, name, wait=0.25):
    post(hwnd, WM_COMMAND, CMD[name], 0, wait)


def q(hwnd, name, arg=0):
    return u32.SendMessageW(hwnd, WM_APP_QUERY, Q[name], arg)


def screen_lp(hwnd, x, y):
    p = wt.POINT(int(x), int(y))
    u32.ClientToScreen(hwnd, ctypes.byref(p))
    return lp(p.x, p.y)


def wheel(hwnd, x, y, notches, keys=0, horizontal=False, wait=0.3):
    delta = int(notches * 120) & 0xFFFF
    post(hwnd, WM_MOUSEHWHEEL if horizontal else WM_MOUSEWHEEL, (delta << 16) | keys, screen_lp(hwnd, x, y), wait)


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


def type_text(hwnd, text, wait=0.3):
    for ch in text:
        post(hwnd, WM_CHAR, ord(ch), 0, 0.02)
    time.sleep(wait)


def find_color(img, rgb, box, tol=24):
    """first pixel close to rgb inside box (l, t, r, b), scanning rows top-down"""
    px = img.load()
    l, t, r, b = box
    for y in range(max(0, t), min(b, img.height)):
        for x in range(max(0, l), min(r, img.width)):
            p = px[x, y]
            if sum(abs(p[i] - rgb[i]) for i in range(3)) < tol:
                return x, y
    return None


def ink(img, box, bg=(255, 255, 255), tol=90):
    """pixels inside box (l, t, r, b) that clearly differ from the background: something is drawn there"""
    px = img.load()
    l, t, r, b = box
    return sum(1 for y in range(max(0, t), min(b, img.height)) for x in range(max(0, l), min(r, img.width))
               if sum(abs(px[x, y][i] - bg[i]) for i in range(3)) > tol)


def gear_box(hwnd):
    """client-pixel box of the settings button, None while it is not shown"""
    c = q(hwnd, "SETTINGS_BTN")
    if c < 0:
        return None
    x, y = c & 0xFFFF, c >> 16
    return (x - 15, y - 15, x + 15, y + 15)


def check(name, cond, info=""):
    print(f"[{'OK' if cond else 'FAIL'}] {name}" + (f": {info}" if info else ""))
    return bool(cond)


def window_rect(hwnd):
    r = wt.RECT()
    u32.GetWindowRect(hwnd, ctypes.byref(r))
    return (r.left, r.top, r.right, r.bottom)


def close_and_wait(proc, hwnd):
    post(hwnd, WM_CLOSE, 0, 0, 0.1)  # the viewer saves its settings, placement and reading position
    try:
        proc.wait(5)
    except subprocess.TimeoutExpired:
        proc.kill()


def launch(*args, size="--size=1000x800"):
    a = [str(EXE), "--light", "--zoom=100"] + ([size] if size else []) + [str(x) for x in args]
    proc = subprocess.Popen(a, env=ENV)
    hwnd = find_window(proc.pid)
    time.sleep(0.8)
    return proc, hwnd


def reset_profile():
    shutil.rmtree(DATA, ignore_errors=True)
    try:
        winreg.DeleteKey(winreg.HKEY_CURRENT_USER, REGKEY)
    except OSError:
        pass


def set_reg(name, value):
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, REGKEY) as k:
        if isinstance(value, int):
            winreg.SetValueEx(k, name, 0, winreg.REG_DWORD, value)
        else:
            winreg.SetValueEx(k, name, 0, winreg.REG_SZ, value)


def del_reg(name):
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, REGKEY, 0, winreg.KEY_SET_VALUE) as k:
            winreg.DeleteValue(k, name)
    except OSError:
        pass


# ------------------------------------------------------------------------------------------------ v0.1 basics
def test_basics(doc):
    ok = True
    proc, hwnd = launch(doc)
    try:
        shot(hwnd, "01-initial")
        drag(hwnd, 180, 150, 700, 175)
        shot(hwnd, "02-drag-selection")
        cmd(hwnd, "COPY")
        c = clipboard()
        ok &= check("drag selection copies text", len(c) > 10, repr(c[:80]))

        post(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp(200, 96), 0.01)
        post(hwnd, WM_LBUTTONUP, 0, lp(200, 96), 0.01)
        post(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp(200, 96), 0.01)
        post(hwnd, WM_LBUTTONUP, 0, lp(200, 96), 0.2)
        cmd(hwnd, "COPY")
        c = clipboard()
        ok &= check("double click selects one word", 0 < len(c) < 40 and " " not in c.strip(), repr(c))

        cmd(hwnd, "SELECT_ALL")
        cmd(hwnd, "COPY")
        c = clipboard()
        ok &= check("select all copies the document", "Конец документа." in c and "\tСтатус\t" in c, f"{len(c)} chars")
        post(hwnd, WM_KEYDOWN, 0x1B)  # Esc clears the selection

        cmd(hwnd, "FIND", 0.6)
        type_text(hwnd, "таблиц")
        buf = ctypes.create_unicode_buffer(64)
        u32.SendMessageW(q(hwnd, "FIND_EDIT"), WM_GETTEXT, 64, ctypes.cast(buf, ctypes.c_void_p).value)
        ok &= check("find: typing reaches the find box (own thread)", buf.value == "таблиц" and q(hwnd, "MATCHES") == 2,
                    f"{buf.value!r}, {q(hwnd, 'MATCHES')} matches")
        shot(hwnd, "03-find")
        post(hwnd, WM_KEYDOWN, 0x0D, 0, 0.3)  # Enter → next match
        shot(hwnd, "04-find-next")
        post(hwnd, WM_KEYDOWN, 0x1B, 0, 0.2)  # Esc closes find
        ok &= check("Esc closes the find bar", q(hwnd, "FIND_OPEN") == 0)

        post(hwnd, WM_KEYDOWN, 0x24, 0, 0.5)  # Home
        img = shot(hwnd, "05-top")
        pt = find_color(img, (0x09, 0x69, 0xDA), (150, 205, 900, 260))
        ok &= check("found a link on screen", pt is not None, str(pt))
        if pt:
            click(hwnd, pt[0] + 3, pt[1] + 2, 0.8)
            shot(hwnd, "06-after-anchor")
            ok &= check("anchor link scrolled the document", q(hwnd, "SCROLLY") > 100, str(q(hwnd, "SCROLLY")))

        cmd(hwnd, "ZOOM_IN")
        cmd(hwnd, "ZOOM_IN", 0.4)
        shot(hwnd, "07-zoom")
        cmd(hwnd, "ZOOM_RESET", 0.4)

        post(hwnd, WM_KEYDOWN, 0x24, 0, 0.5)
        cmd(hwnd, "THEME_DARK", 0.4)
        img = shot(hwnd, "08-dark")
        bg = img.getpixel((10, 400))
        ok &= check("dark theme background", sum(bg) < 120, str(bg))
        cmd(hwnd, "THEME_LIGHT", 0.4)

        shot(hwnd, "09-top-light")
        href = focus_link_href(hwnd, "other.md")  # by address, so the layout may change freely
        ok &= check("found the relative .md link", href == "other.md", repr(href))
        if href == "other.md":
            post(hwnd, WM_KEYDOWN, 0x0D, 0, 1.0)  # Enter opens the focused link
            shot(hwnd, "10-other-doc")
            ok &= check("navigated to other.md", title_of(hwnd).startswith("other.md"), title_of(hwnd))
            post(hwnd, WM_SYSKEYDOWN, 0x25, 0, 0.8)  # Alt+Left
            ok &= check("back to features.md", title_of(hwnd).startswith("features.md"), title_of(hwnd))

        with open(doc, "a", encoding="utf-8") as f:
            f.write("\n\n# Добавлено снаружи\n\nЭтот абзац дописан во время теста.\n")
        time.sleep(1.0)
        cmd(hwnd, "SELECT_ALL")
        cmd(hwnd, "COPY")
        ok &= check("live reload picked up the change", "дописан во время теста" in clipboard())
        post(hwnd, WM_KEYDOWN, 0x23, 0, 0.8)  # End
        shot(hwnd, "11-reloaded-end")
    finally:
        close_and_wait(proc, hwnd)
    return ok


# ------------------------------------------------------------------------------------------------ 1.2 horizontal scrolling
def test_hscroll(doc):
    ok = True
    proc, hwnd = launch(doc)
    try:
        post(hwnd, WM_KEYDOWN, 0x24, 0, 0.3)
        blk = q(hwnd, "HSCROLL_BLOCK")
        for _ in range(30):  # the wide code block into view (its layout exists once it was on screen)
            blk = q(hwnd, "HSCROLL_BLOCK")
            if blk >= 0 and 120 < q(hwnd, "BLOCK_Y", blk) < 450:
                break
            wheel(hwnd, 500, 400, -1, wait=0.12)
        time.sleep(0.3)
        ok &= check("1.2 a 200-char code line makes its block horizontally scrollable", blk >= 0, str(blk))
        y = q(hwnd, "BLOCK_Y", blk) + 20
        wheel(hwnd, 500, y, -2, keys=MK_SHIFT)
        x1 = q(hwnd, "HSCROLL_X", blk)
        ok &= check("1.2 Shift+wheel scrolls the block sideways", x1 > 0, str(x1))
        img = shot(hwnd, "12-hscroll-shift")
        ok &= check("1.2 the block's scrollbar shows while it scrolls",
                    find_color(img, (0xc8, 0xcd, 0xd3), (40, y - 10, 980, y + 90), tol=12) is not None)
        wheel(hwnd, 500, y, 2, horizontal=True)
        x2 = q(hwnd, "HSCROLL_X", blk)
        ok &= check("1.2 touchpad / tilt wheel (WM_MOUSEHWHEEL) scrolls further", x2 > x1, f"{x1} → {x2}")
        wheel(hwnd, 500, y, 30, keys=MK_SHIFT)  # back to the start
        ok &= check("1.2 scrolls back to the start", q(hwnd, "HSCROLL_X", blk) == 0)
        cmd(hwnd, "FIND", 0.6)
        type_text(hwnd, "КОНЕЦ200", 0.5)
        x3 = q(hwnd, "HSCROLL_X", blk)
        img = shot(hwnd, "13-hscroll-find-end")
        by = q(hwnd, "BLOCK_Y", blk)
        ok &= check("1.2 find reveals the end of the long line (block scrolled)", x3 > 0, str(x3))
        ok &= check("1.2 the match at the end of the line is visible",
                    find_color(img, (0xff, 0xa6, 0x57), (40, by, 980, by + 60)) is not None)
        post(hwnd, WM_KEYDOWN, 0x1B, 0, 0.2)
        cmd(hwnd, "WRAP", 0.6)
        ok &= check("1.2 'wrap lines in code' removes horizontal scrolling", q(hwnd, "HSCROLL_BLOCK") == -1 and q(hwnd, "WRAP") == 1)
        shot(hwnd, "14-wrap")
        cmd(hwnd, "WRAP", 0.6)
    finally:
        close_and_wait(proc, hwnd)
    return ok


# ------------------------------------------------------------------------------------------------ 1.3 outline
def test_outline():
    ok = True
    proc, hwnd = launch(MEDIUM, size="--size=1200x800")
    try:
        cmd(hwnd, "TOC", 0.5)
        n = q(hwnd, "TOC_COUNT")
        ok &= check("1.3 outline opens docked in a wide window", q(hwnd, "TOC_OPEN") == 1 and q(hwnd, "TOC_DOCKED") == 1)
        ok &= check("1.3 outline lists the headings of medium.md (24 sections + subsections)", n >= 24, str(n))
        y = q(hwnd, "TOC_ITEM_Y", 4)
        click(hwnd, 100, y, 1.2)
        cur = q(hwnd, "TOC_CURRENT")
        ok &= check("1.3 a click on the 5th item scrolls there and it becomes the current section",
                    cur == 4 and q(hwnd, "SCROLLY") > 0, f"current={cur} scrollY={q(hwnd, 'SCROLLY')}")
        img = shot(hwnd, "15-outline")
        ok &= check("1.3 the current item is highlighted", find_color(img, (0xdd, 0xf4, 0xff), (8, y - 8, 250, y + 8), tol=10) is not None)
        wheel(hwnd, 700, 400, -10, wait=0.8)
        ok &= check("1.3 the highlight follows the reading position", q(hwnd, "TOC_CURRENT") > 4, str(q(hwnd, "TOC_CURRENT")))
    finally:
        close_and_wait(proc, hwnd)
    proc, hwnd = launch(MEDIUM, size="--size=1200x800")
    try:
        ok &= check("1.3 the open outline is remembered", q(hwnd, "TOC_OPEN") == 1)
        cmd(hwnd, "TOC", 0.3)
    finally:
        close_and_wait(proc, hwnd)
    return ok


# ------------------------------------------------------------------------------------------------ 1.4 reading position
def test_positions(doc):
    ok = True
    proc, hwnd = launch(doc)
    try:
        post(hwnd, WM_KEYDOWN, 0x24, 0, 0.3)
        wheel(hwnd, 500, 400, -6, wait=0.8)
        before = q(hwnd, "SCROLLY")
    finally:
        close_and_wait(proc, hwnd)
    proc, hwnd = launch(doc)
    try:
        time.sleep(0.5)
        after = q(hwnd, "SCROLLY")
        ok &= check("1.4 reopening glides back to the reading position", q(hwnd, "RESTORED") == 1 and abs(after - before) <= 2,
                    f"{before} → {after}")
    finally:
        close_and_wait(proc, hwnd)
    text = doc.read_text(encoding="utf-8")  # the file changes above the section: the place is found by its heading
    doc.write_text(text.replace("# Проверка возможностей", "Новый абзац в начале.\n\nЕщё один.\n\n# Проверка возможностей", 1),
                   encoding="utf-8")
    proc, hwnd = launch(doc)
    try:
        time.sleep(0.5)
        moved = q(hwnd, "SCROLLY")
        ok &= check("1.4 a changed file is restored by the nearest heading", q(hwnd, "RESTORED") == 1 and moved > before,
                    f"{before} → {moved}")
    finally:
        close_and_wait(proc, hwnd)
    doc.write_text(text, encoding="utf-8")
    return ok


# ------------------------------------------------------------------------------------------------ 1.5 find 2.0
def test_find():
    ok = True
    proc, hwnd = launch(MEDIUM)
    try:
        cmd(hwnd, "FIND", 0.6)
        edit = q(hwnd, "FIND_EDIT")
        ok &= check("1.5 the find box is a real EDIT on its own thread (IME)", edit != 0)
        type_text(hwnd, "раздел")
        all_ = q(hwnd, "MATCHES")
        cmd(hwnd, "FIND_CASE", 0.4)
        case = q(hwnd, "MATCHES")
        ok &= check("1.5 'match case' narrows the matches", 0 < case < all_, f"{all_} → {case}")
        cmd(hwnd, "FIND_CASE", 0.3)
        cmd(hwnd, "FIND_WORD", 0.4)
        word = q(hwnd, "MATCHES")
        ok &= check("1.5 'whole word' narrows the matches", 0 < word < all_, f"{all_} → {word}")
        cmd(hwnd, "FIND_WORD", 0.3)
        c = q(hwnd, "FIND_PART_X", FP_NEXT)
        before = q(hwnd, "CUR_MATCH")
        click(hwnd, c & 0xFFFF, c >> 16, 0.3)
        ok &= check("1.5 the ↓ button goes to the next match", q(hwnd, "CUR_MATCH") == before + 1, f"{before} → {q(hwnd, 'CUR_MATCH')}")
        c = q(hwnd, "FIND_PART_X", FP_CASE)
        click(hwnd, c & 0xFFFF, c >> 16, 0.3)
        ok &= check("1.5 the Aa button toggles 'match case'", q(hwnd, "MATCHES") == case)
        click(hwnd, c & 0xFFFF, c >> 16, 0.3)
        img = shot(hwnd, "16-find-marks")
        ok &= check("1.5 matches are marked on the scrollbar", find_color(img, (0xbf, 0x87, 0x00), (980, 60, 1000, 790), tol=30) is not None)
        ok &= check("the find bar takes the settings button's corner", q(hwnd, "SETTINGS_BTN") == -1)
        cmd(hwnd, "FIND_CLOSE", 0.3)
        ok &= check("the settings button is back once find closes", q(hwnd, "SETTINGS_BTN") >= 0)
    finally:
        close_and_wait(proc, hwnd)
    proc, hwnd = launch(MEDIUM)
    try:
        cmd(hwnd, "FIND", 0.6)
        edit = q(hwnd, "FIND_EDIT")
        buf = ctypes.create_unicode_buffer(64)
        u32.SendMessageW(edit, WM_GETTEXT, 64, ctypes.cast(buf, ctypes.c_void_p).value)
        ok &= check("1.5 the query is kept for the next window", buf.value == "раздел" and q(hwnd, "MATCHES") == all_,
                    repr(buf.value))
    finally:
        close_and_wait(proc, hwnd)
    return ok


# ------------------------------------------------------------------------------------------------ 1.6 start screen
def test_start_screen():
    ok = True
    proc, hwnd = launch()
    try:
        n = q(hwnd, "RECENT_COUNT")
        ok &= check("1.6 without a file the start screen lists recent documents", n >= 2, str(n))
        img = shot(hwnd, "17-start-screen")
        box = gear_box(hwnd)
        ok &= check("the start screen has the settings button too", box is not None and ink(img, box) > 12,
                    f"{box} ink={ink(img, box) if box else 0}")
        type_text(hwnd, "medium")
        ok &= check("1.6 typing filters by name", q(hwnd, "RECENT_COUNT") == 1)
        post(hwnd, WM_KEYDOWN, 0x0D, 0, 0.8)
        ok &= check("1.6 Enter opens the document", title_of(hwnd).startswith("medium.md"), title_of(hwnd))
    finally:
        close_and_wait(proc, hwnd)
    # regression: an outline left open (nothing to show on the start screen) must not swallow the first click or Esc
    set_reg("Outline", 1)
    proc, hwnd = launch()
    try:
        click(hwnd, 500, 210, 0.8)  # the first recent document
        ok &= check("1.6 a click opens a recent document (outline left open)", title_of(hwnd) != "FastMD", title_of(hwnd))
    finally:
        close_and_wait(proc, hwnd)
    set_reg("Outline", 1)
    proc, hwnd = launch()
    post(hwnd, WM_KEYDOWN, 0x1B, 0, 0.1)
    try:
        proc.wait(3)
        closed = True
    except subprocess.TimeoutExpired:
        closed = False
        close_and_wait(proc, hwnd)
    ok &= check("1.6 the first Esc on the start screen closes the window (outline left open)", closed)
    del_reg("Outline")
    return ok


# ------------------------------------------------------------------------------------------------ 1.7 keyboard links, link / image menu
def test_links(doc):
    ok = True
    proc, hwnd = launch(doc)
    try:
        post(hwnd, WM_KEYDOWN, 0x24, 0, 0.4)
        post(hwnd, WM_KEYDOWN, 0x09, 0, 0.3)  # Tab
        ok &= check("1.7 Tab focuses the first link", q(hwnd, "FOCUS_LINK") == 0)
        shot(hwnd, "18-link-focus")
        cmd(hwnd, "LINK_COPY")
        ok &= check("1.7 'copy link address' copies the href", clipboard() == "#таблицы", repr(clipboard()))
        for _ in range(3):
            post(hwnd, WM_KEYDOWN, 0x09, 0, 0.15)
        ok &= check("1.7 Tab moves through the links", q(hwnd, "FOCUS_LINK") == 3)
        post(hwnd, WM_KEYDOWN, 0x0D, 0, 0.8)  # Enter
        ok &= check("1.7 Enter opens the focused link", title_of(hwnd).startswith("other.md"), title_of(hwnd))
        post(hwnd, WM_SYSKEYDOWN, 0x25, 0, 0.8)
        post(hwnd, WM_KEYDOWN, 0x23, 0, 1.0)  # End: the image at the bottom
        cmd(hwnd, "IMG_COPY", 0.4)
        ok &= check("1.7 'copy image' puts a bitmap on the clipboard", clipboard_has(8))  # CF_DIB
        # 2.8: the link icon beside a heading copies that heading's address
        cmd(hwnd, "TOC", 0.6)
        click(hwnd, 100, q(hwnd, "TOC_ITEM_Y", 1), 1.2)  # jump to a heading: it lands at the top of the view
        click(hwnd, q(hwnd, "TEXT_LEFT") - 14, 26, 0.6)
        ok &= check("2.8 the icon beside a heading copies its address", clipboard().startswith("features.md#"),
                    repr(clipboard()))
        cmd(hwnd, "TOC", 0.4)
    finally:
        close_and_wait(proc, hwnd)
    return ok


# ------------------------------------------------------------------------------------------------ 2.1 footnotes, alerts
def focus_link_href(hwnd, want, steps=40):
    """Tab through the links until the focused one has this href; returns the href actually found"""
    for _ in range(steps):
        post(hwnd, WM_KEYDOWN, 0x09, 0, 0.12)  # Tab
        cmd(hwnd, "LINK_COPY", 0.12)
        if clipboard() == want:
            return want
    return clipboard()


def test_footnotes():
    """footnote jumps, GitHub alerts after the md4c update, emoji shortcodes and formulas"""
    ok = True
    doc = OUT / "footnotes.md"
    shutil.copy(HERE / "footnotes.md", doc)
    proc, hwnd = launch(doc, size="--size=900x700")
    try:
        img = shot(hwnd, "26-alerts")
        left = q(hwnd, "TEXT_LEFT")
        ok &= check("2.1 GitHub alerts keep their bar and title (md4c admonitions)",
                    find_color(img, (0x09, 0x69, 0xda), (left - 2, 100, left + 8, 400), tol=30) is not None)
        href = focus_link_href(hwnd, "#fn-1")
        ok &= check("2.1 a footnote reference is a link to its definition", href == "#fn-1", repr(href))
        before = q(hwnd, "SCROLLY")
        post(hwnd, WM_KEYDOWN, 0x0D, 0, 1.0)  # Enter: open the focused link
        at_def = q(hwnd, "SCROLLY")
        ok &= check("2.1 it jumps down to the definition", at_def > before, f"{before} → {at_def}")
        shot(hwnd, "27-footnotes")
        href = focus_link_href(hwnd, "#fnref-1")
        ok &= check("2.1 the definition links back to the reference", href == "#fnref-1", repr(href))
        post(hwnd, WM_KEYDOWN, 0x0D, 0, 1.0)
        back = q(hwnd, "SCROLLY")
        ok &= check("2.1 the arrow jumps back up to the reference", back < at_def, f"{at_def} → {back}")
        cmd(hwnd, "SELECT_ALL", 0.3)
        cmd(hwnd, "COPY", 0.4)
        text = clipboard()
        ok &= check("2.6 emoji shortcodes become the emoji", "\U0001F680" in text and ":+1:" not in text,
                    repr(text[text.find("Шорткоды"):][:40]))
        ok &= check("2.6 shortcodes inside code stay as typed", ":rocket:" in text)
        ok &= check("2.9 a formula is kept as text", "E = mc^2" in text and "$100" in text)
    finally:
        close_and_wait(proc, hwnd)
    return ok


# ------------------------------------------------------------------------------------------------ 2.2 HTML
def test_html():
    """the HTML subset: centred blocks and images, <kbd>, <sub>/<sup>, headings, and nothing from <script>"""
    ok = True
    doc = OUT / "html.md"
    shutil.copy(HERE / "html.md", doc)
    proc, hwnd = launch(doc, size="--size=900x900")
    try:
        img = shot(hwnd, "28-html")
        cmd(hwnd, "SELECT_ALL", 0.3)
        cmd(hwnd, "COPY", 0.4)
        text = clipboard()
        ok &= check("2.2 script and style contents are dropped", "alert(" not in text and "color: red" not in text)
        ok &= check("2.2 tags are gone, their text stays", "<kbd>" not in text and "Ctrl" in text and "ссылкой" in text,
                    repr(text[:40]))
        ok &= check("2.2 an HTML comment is not shown", "комментарий" not in text)
        ok &= check("2.2 <h3> counts as a heading in the outline", q(hwnd, "TOC_COUNT") >= 2, str(q(hwnd, "TOC_COUNT")))
        # the centred logo: its own box is narrower than the column and sits in the middle of it
        left, width = q(hwnd, "TEXT_LEFT"), q(hwnd, "TEXT_W")
        row = img.crop((0, 100, img.width, 200)).convert("RGB")
        px = row.load()
        cols = [x for x in range(img.width) if sum(px[x, 50]) < 700]  # the picture is a saturated gradient
        centred = cols and abs((cols[0] + cols[-1]) // 2 - (left + width // 2)) <= 8
        ok &= check("2.2 <p align=center> centres the picture", centred, f"{cols[:1]}..{cols[-1:]} column {left}+{width}")
        ok &= check("2.2 a picture inside a line leaves no stray character in the text", "￼" not in text)
        # three 90 px badges in one line: their pixels span far more than one badge would
        band = img.crop((0, 240, img.width, 300)).convert("RGB")
        bp = band.load()
        xs = [x for x in range(img.width) for y in range(0, 60, 6) if sum(bp[x, y]) < 700]
        ok &= check("2.2 badges sit side by side on one line", xs and max(xs) - min(xs) > 200,
                    f"{min(xs) if xs else '-'}..{max(xs) if xs else '-'}")
        ok &= check("2.2 an HTML table becomes a table", "Возможность\tFastMD" in text, repr(text[-120:]))
        ok &= check("2.2 a folded <details> hides its text", "Спрятанный" not in text)
        # click the summary: the text appears and the document grows
        post(hwnd, WM_KEYDOWN, 0x23, 0, 1.2)  # End: the <details> is at the bottom
        post(hwnd, WM_KEYDOWN, 0x23, 0, 1.2)
        tall = q(hwnd, "DOCH")
        sh = shot(hwnd, "29-details")
        found = find_color(sh, (0x59, 0x63, 0x6e), (q(hwnd, "TEXT_LEFT") - 20, 0, q(hwnd, "TEXT_LEFT"), 900), tol=60)
        if found:
            click(hwnd, found[0] + 2, found[1] + 2, 1.0)
        cmd(hwnd, "SELECT_ALL", 0.3)
        cmd(hwnd, "COPY", 0.4)
        ok &= check("2.2 clicking the summary unfolds it", "Спрятанный" in clipboard() and q(hwnd, "DOCH") > tall,
                    f"{tall} → {q(hwnd, 'DOCH')}")
    finally:
        close_and_wait(proc, hwnd)
    return ok


# ------------------------------------------------------------------------------------------------ 2.3 remote pictures
def test_remote_images():
    """fetched after the first frame from a local server, kept in the disk cache, skipped when told not to"""
    import http.server
    import socketserver
    import threading
    ok = True
    png = (REPO / "bench" / "corpus" / "img" / "diagram0.png").read_bytes()

    class Handler(http.server.BaseHTTPRequestHandler):
        hits = 0

        def do_GET(self):
            Handler.hits += 1
            self.send_response(200)
            self.send_header("Content-Type", "image/png")
            self.send_header("Content-Length", str(len(png)))
            self.end_headers()
            self.wfile.write(png)

        def log_message(self, *a):
            pass

    srv = socketserver.TCPServer(("127.0.0.1", 0), Handler)
    port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    doc = OUT / "remote.md"
    doc.write_text(f"# Сеть\n\n![картинка](http://127.0.0.1:{port}/a.png)\n", encoding="utf-8")
    try:
        set_reg("RemoteImages", 2)  # never
        proc, hwnd = launch(doc)
        time.sleep(2.0)
        ok &= check("2.3 'never' asks the network for nothing", Handler.hits == 0, str(Handler.hits))
        close_and_wait(proc, hwnd)

        set_reg("RemoteImages", 0)  # always
        proc, hwnd = launch(doc)
        time.sleep(3.0)
        ok &= check("2.3 the picture is fetched after the first frame", Handler.hits == 1, str(Handler.hits))
        img = shot(hwnd, "30-remote")
        drawn = sum(1 for x in range(120, 800, 8) for y in range(60, 400, 8) if sum(img.getpixel((x, y))) < 700)
        ok &= check("2.3 and it is drawn", drawn > 200, str(drawn))
        close_and_wait(proc, hwnd)

        proc, hwnd = launch(doc)
        time.sleep(2.5)
        ok &= check("2.3 opening it again reads the disk cache", Handler.hits == 1, str(Handler.hits))
        close_and_wait(proc, hwnd)
    finally:
        srv.shutdown()
        del_reg("RemoteImages")
    return ok


# ------------------------------------------------------------------------------------------------ partial frames
def same_pixels(a, b):
    return ImageChops.difference(a.convert("RGB"), b.convert("RGB")).getbbox() is None


def test_scroll_frames():
    """a scrolled frame redraws only the strip that came into view: it must match a full redraw pixel for pixel"""
    ok = True
    proc, hwnd = launch(MEDIUM, size="--size=1100x800")
    try:
        for name, notches in (("down", -5), ("up", 2), ("far", -40)):
            wheel(hwnd, 500, 400, notches, wait=1.2)
            y = q(hwnd, "SCROLLY")
            partial = shot(hwnd, f"24-scroll-{name}")
            q(hwnd, "FULL_REDRAW")
            time.sleep(0.5)
            full = shot(hwnd, f"24-scroll-{name}-full")
            ok &= check(f"scrolling {name}: the partial frame matches a full redraw",
                        q(hwnd, "SCROLLY") == y and same_pixels(partial, full), f"scrollY={y}")
        cmd(hwnd, "TOC", 0.6)
        wheel(hwnd, 700, 400, -4, wait=1.2)
        partial = shot(hwnd, "25-scroll-outline")
        q(hwnd, "FULL_REDRAW")
        time.sleep(0.5)
        ok &= check("the same with the outline open", same_pixels(partial, shot(hwnd, "25-scroll-outline-full")))
        cmd(hwnd, "TOC", 0.4)
    finally:
        close_and_wait(proc, hwnd)
    return ok


# ------------------------------------------------------------------------------------------------ images at display size
def test_image_scaling(doc):
    """a picture wider than its column is drawn from a copy made in the background at exactly that size"""
    ok = True
    proc, hwnd = launch(doc, size="--size=520x700")
    try:
        post(hwnd, WM_KEYDOWN, 0x23, 0, 1.2)  # End: the picture is at the bottom
        post(hwnd, WM_KEYDOWN, 0x23, 0, 1.2)  # again, in case a reading position was still gliding into place
        time.sleep(1.2)
        w, col = q(hwnd, "IMG_SCALED"), q(hwnd, "TEXT_W")
        ok &= check("a picture wider than the column gets a copy at the drawn size", w > 0 and abs(w - col) <= 1,
                    f"{w} vs column {col}")
        shot(hwnd, "23-image-scaled")
        cmd(hwnd, "ZOOM_IN", 0.5)
        time.sleep(1.5)
        w2, col2 = q(hwnd, "IMG_SCALED"), q(hwnd, "TEXT_W")
        ok &= check("zooming remakes the copy at the new size", w2 > 0 and abs(w2 - col2) <= 1 and w2 != w,
                    f"{w} → {w2}, column {col2}")
        cmd(hwnd, "ZOOM_RESET", 0.4)
    finally:
        close_and_wait(proc, hwnd)
    return ok


# ------------------------------------------------------------------------------------------------ 1.8 column width
def test_columns(doc):
    ok = True
    proc, hwnd = launch(doc, size="--size=1300x800")
    try:
        normal = q(hwnd, "TEXT_W")
        cmd(hwnd, "COL_NARROW", 0.4)
        narrow = q(hwnd, "TEXT_W")
        cmd(hwnd, "COL_WIDER", 0.4)
        cmd(hwnd, "COL_WIDER", 0.4)
        wide = q(hwnd, "TEXT_W")
        cmd(hwnd, "COL_FULL", 0.4)
        full = q(hwnd, "TEXT_W")
        shot(hwnd, "19-column-full")
        cmd(hwnd, "COL_NORMAL", 0.4)
        ok &= check("1.8 column presets: narrow < normal < wide < full", narrow < normal < wide < full,
                    f"{narrow} / {normal} / {wide} / {full}")
        ok &= check("1.8 narrow is 560 DIP, wide is 900 DIP", abs(narrow - 560) <= 1 and abs(wide - 900) <= 1)
    finally:
        close_and_wait(proc, hwnd)
    return ok


# ------------------------------------------------------------------------------------------------ 1.9 settings window, 1.10 editor
def click_setting(hwnd, shwnd, id_):
    c = q(hwnd, "SETTINGS_HIT", id_)
    if c < 0:
        return False
    x, y = c & 0xFFFF, c >> 16
    post(shwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp(x, y), 0.05)
    post(shwnd, WM_LBUTTONUP, 0, lp(x, y), 0.5)
    return True


def test_settings(doc):
    ok = True
    proc, hwnd = launch(doc)
    try:
        box = gear_box(hwnd)
        img = shot(hwnd, "22-gear-button")
        ok &= check("the settings button is drawn in the top-right corner",
                    box is not None and box[0] > 900 and box[1] < 20 and ink(img, box) > 12,
                    f"{box} ink={ink(img, box) if box else 0}")
        if box:
            click(hwnd, (box[0] + box[2]) // 2, (box[1] + box[3]) // 2, 0.8)
        sh = q(hwnd, "SETTINGS_HWND")
        ok &= check("1.9 a click on the settings button opens the settings window", sh != 0)
        h0 = q(hwnd, "DOCH")
        click_setting(hwnd, sh, 2)      # theme: dark
        click_setting(hwnd, sh, 101)    # font: Sitka
        click_setting(hwnd, sh, 205)    # text size 20
        click_setting(hwnd, sh, 702)    # English
        shot(sh, "20-settings")
        shot(hwnd, "21-settings-applied")
        ok &= check("1.9 theme applies at once", q(hwnd, "THEME_DARK") == 1)
        ok &= check("1.9 the book font (Sitka) applies", q(hwnd, "SITKA") == 1)
        ok &= check("1.9 text size re-lays out the document", q(hwnd, "FONT_SIZE") == 20 and q(hwnd, "DOCH") > h0)
        ok &= check("1.9 interface language switches", q(hwnd, "LANG") == 1)
        post(sh, WM_KEYDOWN, 0x1B, 0, 0.4)
    finally:
        close_and_wait(proc, hwnd)
    proc, hwnd = launch(doc)
    try:
        ok &= check("1.9 settings persist", q(hwnd, "FONT_SIZE") == 20 and q(hwnd, "LANG") == 1 and q(hwnd, "SITKA") == 1)
        cmd(hwnd, "SETTINGS", 0.8)
        sh = q(hwnd, "SETTINGS_HWND")
        for id_ in (0, 100, 202, 700):  # back to the defaults
            click_setting(hwnd, sh, id_)
        post(sh, WM_KEYDOWN, 0x1B, 0, 0.3)
    finally:
        close_and_wait(proc, hwnd)
    # 1.10: the chosen editor gets the file (FastMD itself stands in for an editor)
    set_reg("Editor", str(EXE))
    proc, hwnd = launch(doc)
    try:
        cmd(hwnd, "EDIT", 1.5)
        others = []
        cb_t = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

        def cb(h, _):
            p = wt.DWORD()
            u32.GetWindowThreadProcessId(h, ctypes.byref(p))
            if p.value != proc.pid and u32.IsWindowVisible(h) and title_of(h).startswith("features.md — FastMD"):
                others.append((h, p.value))
            return True

        u32.EnumWindows(cb_t(cb), 0)
        ok &= check("1.10 Ctrl+E opens the document in the chosen editor", len(others) == 1, str(len(others)))
        for h, _ in others:
            post(h, WM_CLOSE, 0, 0, 0.5)
    finally:
        close_and_wait(proc, hwnd)
        del_reg("Editor")
    return ok


# ------------------------------------------------------------------------------------------------ window placement (v0.1.1)
def test_placement(doc):
    """last closed window's rect is restored; a second window cascades; maximized state is restored"""
    ok = True
    u32.SetWindowPos.argtypes = [wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wt.UINT]
    a, ha = launch(doc, size=None)
    target = (180, 140, 180 + 900, 140 + 680)
    u32.SetWindowPos(ha, None, target[0], target[1], target[2] - target[0], target[3] - target[1], 0x0014)  # NOZORDER|NOACTIVATE
    time.sleep(0.3)
    close_and_wait(a, ha)

    b, hb = launch(doc, size=None)
    rb = window_rect(hb)
    ok &= check("placement restored after reopen", all(abs(rb[i] - target[i]) <= 1 for i in range(4)), f"{rb} vs {target}")
    c, hc = launch(doc, size=None)
    rc = window_rect(hc)
    ok &= check("second window cascades", rc[0] == rb[0] + 28 and rc[1] == rb[1] + 28 and rc[2] - rc[0] == rb[2] - rb[0],
                f"{rc} vs {rb}")
    close_and_wait(c, hc)
    u32.ShowWindow(hb, 3)  # SW_MAXIMIZE
    time.sleep(0.3)
    close_and_wait(b, hb)

    d, hd = launch(doc, size=None)
    ok &= check("maximized state restored", bool(u32.IsZoomed(hd)))
    u32.ShowWindow(hd, 9)  # SW_RESTORE
    time.sleep(0.3)
    rd = window_rect(hd)
    ok &= check("restore returns to the saved normal rect", all(abs(rd[i] - target[i]) <= 1 for i in range(4)), f"{rd}")
    close_and_wait(d, hd)
    return ok


def main():
    OUT.mkdir(exist_ok=True)
    reset_profile()
    doc = OUT / "features.md"
    shutil.copy(HERE / "features.md", doc)
    shutil.copy(HERE / "other.md", OUT / "other.md")
    (OUT / "img").mkdir(exist_ok=True)
    shutil.copy(REPO / "bench" / "corpus" / "img" / "diagram0.png", OUT / "img" / "diagram0.png")
    ok = True
    for t in (lambda: test_basics(doc), lambda: test_hscroll(doc), test_outline, lambda: test_positions(doc), test_find,
              test_start_screen, lambda: test_links(doc), test_footnotes, test_html, test_remote_images,
              test_scroll_frames,
              lambda: test_image_scaling(doc),
              lambda: test_columns(doc),
              lambda: test_settings(doc),
              lambda: test_placement(doc)):
        ok &= t()
    reset_profile()
    print("RESULT", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
