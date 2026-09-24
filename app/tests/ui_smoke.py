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
       "FIND_WORD": 130, "FIND_NEXT": 131, "FIND_PREV": 132, "FIND_CLOSE": 133, "LINK_NEXT": 134, "LINK_PREV": 135,
       "LOAD_REMOTE": 136, "COPY_MD": 137, "PRINT": 138, "EXPORT_PDF": 139, "UPDATE": 140}
# edit mode (docs/EDIT-MODE.md §13.1): the complete v1 list, mirrored from app.h before the features exist
CMD.update({"EDIT_TOGGLE": 141, "EDIT_HERE": 142, "EDIT_EXIT": 143, "UNDO": 144, "REDO": 145, "CUT": 146,
            "PASTE": 147, "SAVE": 148, "SAVE_AS": 149, "FMT_BOLD": 150, "FMT_ITALIC": 151, "FMT_STRIKE": 152,
            "FMT_CODE": 153, "LINK": 154, "LINK_REMOVE": 155, "BLOCK_P": 156, "BLOCK_H1": 157, "BLOCK_H2": 158,
            "BLOCK_H3": 159, "BLOCK_H4": 160, "BLOCK_H5": 161, "BLOCK_H6": 162, "LIST_BULLET": 163, "LIST_NUMBER": 164,
            "LIST_TASK": 165, "QUOTE": 166, "CODEBLOCK": 167, "CODE_LANG": 168, "INS_TABLE": 169, "INS_FORMULA": 170,
            "INS_FORMULA_BLOCK": 171, "INS_DIAGRAM": 172, "INS_IMAGE": 173, "INS_HR": 174, "NEW_PARAGRAPH": 175,
            "TABLE_ROW_ABOVE": 176, "TABLE_ROW_BELOW": 177, "TABLE_COL_LEFT": 178, "TABLE_COL_RIGHT": 179,
            "TABLE_DEL_ROW": 180, "TABLE_DEL_COL": 181, "TABLE_ALIGN_L": 182, "TABLE_ALIGN_C": 183,
            "TABLE_ALIGN_R": 184, "TABLE_DEL": 185, "BLOCK_MENU": 186, "TABLE_MENU": 187, "FORMULA_MENU": 188,
            "DIAGRAM_MENU": 189, "EDIT_MORE": 190, "ATOM_EDIT": 191, "POPUP_DONE": 192, "POPUP_CANCEL": 193,
            "CONFLICT_LOAD": 194, "CONFLICT_KEEP": 195, "SAVE_RETRY": 196, "DISCARD_EDITS": 197, "ENC_UTF8": 198,
            "ENC_REMOVE_CHAR": 199, "RECOVERY_OPEN": 200, "RECOVERY_RESTORE": 201, "RECOVERY_DELETE": 202,
            "OTHER_WINDOW": 203, "STRIP_CLOSE": 204})
Q = {"SCROLLY": 1, "DOCH": 2, "TOC_OPEN": 3, "TOC_DOCKED": 4, "TOC_COUNT": 5, "TOC_CURRENT": 6, "TOC_ITEM_Y": 7,
     "HSCROLL_BLOCK": 8, "HSCROLL_X": 9, "FOCUS_LINK": 10, "MATCHES": 11, "CUR_MATCH": 12, "TEXT_LEFT": 13,
     "TEXT_W": 14, "RECENT_COUNT": 15, "FIND_EDIT": 16, "SETTINGS_HWND": 17, "FIND_OPEN": 18, "COLUMN": 19,
     "FONT_SIZE": 20, "WRAP": 21, "LANG": 22, "FIND_PART_X": 23, "BLOCK_Y": 24, "RESTORED": 25, "THEME_DARK": 26,
     "TARGETY": 27, "SETTINGS_HIT": 28, "SITKA": 29, "SETTINGS_BTN": 30, "IMG_SCALED": 31, "FULL_REDRAW": 32, "SEL_ANCHOR": 33, "SEL_FOCUS": 34, "CARET": 35, "DRAG": 36, "MATH": 37, "UPDATE": 38,
     "TASK": 39, "TASK_BOX": 40, "DOC_SERIAL": 41}
# edit mode (§13.2): a query whose feature is not built yet answers -1
Q.update({"EDITING": 42, "EDIT_DIRTY": 43, "EDIT_CARET_SRC": 44, "EDIT_ANCHOR_SRC": 45, "EDIT_TOOL": 46,
          "EDIT_BAR": 47, "UNDO_DEPTH": 48, "RELOADS": 49, "SAVES": 50, "EDIT_POPUP": 51, "MAP_SELFCHECK": 52,
          "SRC_HASH": 53, "SRC_LEN": 54, "EDIT_BUSY": 55, "EDIT_PHANTOM": 56, "BLOCK_COUNT": 57, "EDIT_SAVE_STATE": 58,
          "EDIT_CONFLICT": 59, "EDIT_ENC": 60, "EDIT_EOL": 61, "EDIT_ACTIVE": 62, "LAST_PROMPT": 63,
          "RELAYOUT_ALL": 64, "FRAME_STATS": 65, "RENDERS": 66, "EDIT_STATS": 67, "EDIT_CARET_VISIBLE": 68,
          "EDIT_CARET_PHASE": 69, "EDIT_POPUP_STATE": 70, "EDIT_ATOM": 71, "EDIT_STRIP": 72, "EDIT_RAW": 73,
          "EDIT_BUBBLE": 74, "EDIT_COLLAPSE": 75})
FP_NEXT, FP_CASE = 7, 3  # FindPart
US_CHECKING, US_LATEST, US_AVAILABLE, US_CHECK_FAILED = 1, 2, 3, 7  # UpdateStatus
ACCENT = (0x09, 0x69, 0xDA)  # P_ACCENT, light theme

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


def clipboard_format(name):
    """contents of a registered clipboard format (HTML Format, Rich Text Format) as text"""
    fmt = u32.RegisterClipboardFormatW(name)
    if not fmt or not open_clipboard():
        return ""
    try:
        h = u32.GetClipboardData(fmt)
        if not h:
            return ""
        p = k32.GlobalLock(h)
        data = ctypes.string_at(p)
        k32.GlobalUnlock(h)
        return data.decode("utf-8", "replace")
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


VK = {"left": 0x25, "up": 0x26, "right": 0x27, "down": 0x28, "home": 0x24, "end": 0x23, "esc": 0x1B,
      "back": 0x08, "delete": 0x2E, "return": 0x0D, "tab": 0x09, "f2": 0x71, "oem_3": 0xC0}


def keys(hwnd, vks, shift=False, ctrl=False, wait=0.12):
    """posted key presses with real modifier state: the app reads GetKeyState, so the input queues are attached"""
    tid = u32.GetWindowThreadProcessId(hwnd, None)
    me = k32.GetCurrentThreadId()
    st = (ctypes.c_ubyte * 256)()
    u32.AttachThreadInput(me, tid, True)
    try:
        u32.GetKeyboardState(ctypes.byref(st))
        for vk, on in ((0x10, shift), (0xA0, shift), (0x11, ctrl), (0xA2, ctrl)):
            st[vk] = 0x80 if on else 0
        u32.SetKeyboardState(ctypes.byref(st))
        for vk in vks:
            post(hwnd, WM_KEYDOWN, vk, 0, wait)
        for vk in (0x10, 0xA0, 0x11, 0xA2):
            st[vk] = 0
        u32.SetKeyboardState(ctypes.byref(st))
    finally:
        u32.AttachThreadInput(me, tid, False)


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


def reg_value(name):
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, REGKEY) as k:
            return winreg.QueryValueEx(k, name)[0]
    except OSError:
        return None


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
        cols = [x for x in range(left + width) if sum(px[x, 50]) < 700]  # the picture is a saturated gradient
        centred = cols and abs((cols[0] + cols[-1]) // 2 - (left + width // 2)) <= 8
        ok &= check("2.2 <p align=center> centres the picture", centred, f"{cols[:1]}..{cols[-1:]} column {left}+{width}")
        ok &= check("2.2 a picture inside a line leaves no stray character in the text", "￼" not in text)
        # three 90 px badges in one line: their pixels span far more than one badge would
        band = img.crop((0, 240, img.width, 300)).convert("RGB")
        bp = band.load()
        xs = [x for x in range(left + width) for y in range(0, 60, 6) if sum(bp[x, y]) < 700]
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


# ------------------------------------------------------------------------------------------------ 2.4 SVG
SVG_BADGE = ('<svg xmlns="http://www.w3.org/2000/svg" width="104" height="20">'
             '<rect width="62" height="20" fill="#555555"/><rect x="62" width="42" height="20" fill="#44cc11"/>'
             '<text x="8" y="14" fill="#ffffff" font-family="Verdana" font-size="11">build</text>'
             '<text x="70" y="14" fill="#ffffff" font-family="Verdana" font-size="11">ok</text></svg>')


def test_svg():
    """SVG is drawn by fastmd-svg.dll: from a file, from the web, and crisply at any size"""
    import http.server
    import socketserver
    import threading
    ok = True
    body = SVG_BADGE.encode()

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            self.send_response(200)
            self.send_header("Content-Type", "image/svg+xml")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *a):
            pass

    srv = socketserver.TCPServer(("127.0.0.1", 0), Handler)
    port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    (OUT / "img").mkdir(exist_ok=True)
    (OUT / "img" / "badge.svg").write_text(SVG_BADGE, encoding="utf-8")
    doc = OUT / "svg.md"
    doc.write_text(f'# SVG\n\nВ строке: <img src="img/badge.svg"> и дальше текст.\n\n'
                   f'![из сети](http://127.0.0.1:{port}/b.svg)\n\n'
                   f'<img src="img/badge.svg" width="312">\n', encoding="utf-8")
    try:
        proc, hwnd = launch(doc)
        time.sleep(3.0)
        img = shot(hwnd, "31-svg")
        green = [(x, y) for x in range(100, 900, 3) for y in range(60, 500, 3)
                 if abs(img.getpixel((x, y))[0] - 0x44) < 40 and img.getpixel((x, y))[1] > 0xA0
                 and img.getpixel((x, y))[2] < 0x60]
        rows = sorted({y for _, y in green})
        ok &= check("2.4 SVG from a file and from the web is drawn", len(green) > 30 and len(rows) > 6,
                    f"{len(green)} pixels on {len(rows)} rows")
        # the third one asks for 312 px: three times the natural width, drawn again rather than blown up
        wide = [x for x, y in green if y > max(rows) - 40]
        ok &= check("2.4 a bigger SVG is redrawn at that size", wide and max(wide) - min(wide) > 100,
                    f"{min(wide) if wide else '-'}..{max(wide) if wide else '-'}")
    finally:
        close_and_wait(proc, hwnd)
        srv.shutdown()
    return ok


# ------------------------------------------------------------------------------------------------ 2.5 languages
def test_languages():
    """code blocks in languages added in 2.5 are highlighted and labelled with their language"""
    ok = True
    doc = OUT / "langs.md"
    doc.write_text("# Языки\n\n```php\n<?php\n// комментарий\nfunction greet(string $name): string {\n"
                   "    return \"Привет\";\n}\n```\n\n```mermaid\ngraph TD\n  A[Начало] --> B[Конец]\n```\n",
                   encoding="utf-8")
    proc, hwnd = launch(doc, size="--size=820x600")
    try:
        img = shot(hwnd, "32-languages")
        left, width = q(hwnd, "TEXT_LEFT"), q(hwnd, "TEXT_W")
        kw = find_color(img, (0xcf, 0x22, 0x2e), (left, 80, left + width, 300), tol=40)
        ok &= check("2.5 a php block is highlighted", kw is not None, str(kw))
        # the language sits in the block's top-right corner: muted pixels there
        corner = [1 for x in range(left + width - 120, left + width - 8)
                  for y in range(90, 130) if sum(img.getpixel((x, y))) < 620]
        ok &= check("2.5 the block is labelled with its language", len(corner) > 10, str(len(corner)))
    finally:
        close_and_wait(proc, hwnd)
    return ok


# ------------------------------------------------------------------------------------------------ 3.1, 3.2 clipboard
def test_clipboard(doc):
    """copying keeps the formatting (CF_HTML + RTF) and can hand back the Markdown source"""
    ok = True
    proc, hwnd = launch(doc)
    try:
        cmd(hwnd, "SELECT_ALL", 0.3)
        cmd(hwnd, "COPY", 0.6)
        html, rtf, text = clipboard_format("HTML Format"), clipboard_format("Rich Text Format"), clipboard()
        ok &= check("3.1 plain text is still there", "Проверка возможностей" in text)
        ok &= check("3.1 CF_HTML with its header and markup",
                    html.startswith("Version:0.9") and "<!--StartFragment-->" in html and "<h1>" in html
                    and "<strong>" in html and "<code>" in html, repr(html[:60]))
        raw = html.encode("utf-8")  # the offsets in the header are byte offsets, and the text is Russian
        off = lambda name: int(html.split(name)[1][:10]) if name in html else -1
        ok &= check("3.1 the HTML offsets point at the fragment",
                    off("StartFragment:") == raw.find(b"<!--StartFragment-->") + len("<!--StartFragment-->")
                    and off("EndFragment:") == raw.find(b"<!--EndFragment-->")
                    and off("StartHTML:") == raw.find(b"<html>") and off("EndHTML:") == len(raw),
                    f'{off("StartFragment:")} {off("EndFragment:")} {len(raw)}')
        ok &= check("3.1 RTF with bold, a table and a link",
                    rtf.startswith("{\\rtf1") and "\\b " in rtf and "HYPERLINK" in rtf, repr(rtf[:40]))
        cmd(hwnd, "COPY_MD", 0.6)
        md = clipboard()
        src = doc.read_text(encoding="utf-8")
        ok &= check("3.2 copy as Markdown gives the source back",
                    "# Проверка возможностей" in md and "| Функция | Статус |" in md and "**Ctrl+F**" in md,
                    repr(md[:60]))
        ok &= check("3.2 it is the author's own text, not a rebuild",
                    md.replace("\r\n", "\n").strip()[:200] in src, repr(md[:40]))
    finally:
        close_and_wait(proc, hwnd)
    return ok


# ------------------------------------------------------------------------------------------------ 3.3 keyboard
def test_key_selection():
    """selecting from the keyboard: Shift+arrows, whole words, line ends — and a caret that is really drawn"""
    ok = True
    doc = OUT / "keys.md"  # its own file: other tests edit features.md and leave a reading position behind
    doc.write_text("# Заголовок\n\nСсылки: к разделу, сайт и почта.\n\nВторой абзац, чтобы курсору было куда "
                   "спуститься строкой ниже.\n\nКонец документа.\n", encoding="utf-8")
    proc, hwnd = launch(doc)
    try:
        click(hwnd, q(hwnd, "TEXT_LEFT") + 3, q(hwnd, "BLOCK_Y", 1) + 8)  # into the first paragraph
        start = q(hwnd, "SEL_FOCUS")
        ok &= check("3.3 the mouse leaves no caret behind", q(hwnd, "CARET") == -1)
        keys(hwnd, [VK["right"]] * 5, shift=True)
        ok &= check("3.3 Shift+Right selects five characters",
                    q(hwnd, "SEL_ANCHOR") == start and q(hwnd, "SEL_FOCUS") == start + 5,
                    f'{start} → {q(hwnd, "SEL_FOCUS")}')
        cmd(hwnd, "COPY", 0.4)
        ok &= check("3.3 what is selected is what gets copied", clipboard() == "Ссылк", repr(clipboard()))
        keys(hwnd, [VK["right"]], shift=True, ctrl=True)
        cmd(hwnd, "COPY", 0.4)
        ok &= check("3.3 Ctrl+Shift+Right takes the rest of the word, punctuation stays behind",
                    clipboard() == "Ссылки", repr(clipboard()))
        f1 = q(hwnd, "SEL_FOCUS")
        keys(hwnd, [VK["down"]], shift=True)
        ok &= check("3.3 Shift+Down moves a line down and keeps the anchor",
                    q(hwnd, "SEL_ANCHOR") == start and q(hwnd, "SEL_FOCUS") > f1,
                    f'{f1} → {q(hwnd, "SEL_FOCUS")}')
        keys(hwnd, [VK["up"]], shift=True)
        ok &= check("3.3 Shift+Up comes back to the same place", q(hwnd, "SEL_FOCUS") == f1,
                    f'{f1} → {q(hwnd, "SEL_FOCUS")}')
        keys(hwnd, [VK["end"]], shift=True)
        cmd(hwnd, "COPY", 0.4)
        ok &= check("3.3 Shift+End reaches the end of the line",
                    clipboard().endswith("почта.") and "\r\n" not in clipboard(), repr(clipboard()[-20:]))
        c = q(hwnd, "CARET")
        box = (c & 0xFFFF, (c >> 16) - 5, (c & 0xFFFF) + 4, (c >> 16) + 5)
        with_caret = shot(hwnd, "25-caret")
        keys(hwnd, [VK["esc"]])
        ok &= check("3.3 Esc drops the selection and the caret",
                    q(hwnd, "CARET") == -1 and q(hwnd, "SEL_ANCHOR") == q(hwnd, "SEL_FOCUS"))
        d = ImageChops.difference(with_caret.crop(box), shot(hwnd, "25-caret-off").crop(box))
        ok &= check("3.3 the caret is drawn where it says it is",
                    sum(1 for p in d.getdata() if sum(p) > 60) >= 6, f"{box}")
        keys(hwnd, [VK["end"]], shift=True, ctrl=True)
        cmd(hwnd, "COPY", 0.4)
        ok &= check("3.3 Ctrl+Shift+End selects to the end of the document",
                    clipboard().endswith("Конец документа."), repr(clipboard()[-20:]))
    finally:
        close_and_wait(proc, hwnd)
    return ok


# ------------------------------------------------------------------------------------------------ 3.4 print / PDF
def test_pdf():
    """exporting to PDF goes through the printer driver: several pages, with real text on them"""
    ok = True
    doc = OUT / "print.md"
    body = "".join(f"Абзац {i}: печать раскладывает документ заново под ширину бумаги и режет его по блокам.\n\n"
                   for i in range(1, 61))
    doc.write_text("# Печатный документ\n\n" + body + "| Колонка | Значение |\n|---|---|\n| Строка | Да |\n\n"
                   "```python\nprint('на бумаге тоже код')\n```\n\n![Схема](img/diagram0.png)\n\n"
                   "Конец документа.\n", encoding="utf-8")
    out = OUT / "print.pdf"
    out.unlink(missing_ok=True)
    ENV["FASTMD_PDF_OUT"] = str(out)
    try:
        proc, hwnd = launch(doc)
        try:
            wheel(hwnd, 500, 400, -3, wait=0.6)
            before = q(hwnd, "TEXT_W"), q(hwnd, "SCROLLY")
            cmd(hwnd, "EXPORT_PDF", 0.5)
            for _ in range(60):  # the spooler writes the file after the job ends
                if out.exists() and out.stat().st_size > 1000:
                    break
                time.sleep(0.5)
            time.sleep(0.5)
            after = (q(hwnd, "TEXT_W"), q(hwnd, "SCROLLY"))
            # if this ever fails, say what happened to the window: zeros mean it stopped answering, not that the
            # layout came back wrong
            ok &= check("3.4 the window gets its own layout back", after == before,
                        f'{before} → {after}, process alive={proc.poll() is None}, window={bool(u32.IsWindow(hwnd))}')
        finally:
            close_and_wait(proc, hwnd)
    finally:
        ENV.pop("FASTMD_PDF_OUT", None)
    ok &= check("3.4 the PDF is written", out.exists() and out.stat().st_size > 1000,
                f"{out.stat().st_size if out.exists() else 'missing'}")
    if not out.exists():
        return False
    from pypdf import PdfReader
    r = PdfReader(str(out))
    pages = [p.extract_text() or "" for p in r.pages]
    ok &= check("3.4 it runs to several pages", len(pages) >= 2, f"{len(pages)}")
    ok &= check("3.4 the text is text, not a picture of one",
                "Печатный документ" in pages[0] and "Абзац 1:" in pages[0], repr(pages[0][:60]))
    ok &= check("3.4 the document ends on the last page", "Конец документа." in pages[-1], repr(pages[-1][-60:]))
    ok &= check("3.4 every page is signed with the file name and its number",
                all("print.md" in p for p in pages) and f"2 из {len(pages)}" in pages[1],
                repr(pages[1][:40]))
    ok &= check("3.4 the table and the code came through",
                any("Колонка" in p and "Значение" in p for p in pages) and any("на бумаге тоже код" in p for p in pages))
    ok &= check("3.4 the picture is on the page too",
                any("/XObject" in str(p["/Resources"]) for p in r.pages))
    return ok


# --------------------------------------------------------------------------------------- 6.5 what fuzzing found
def test_broken_html():
    """a document that used to spin the parser forever still opens (the fuzz finding of 21.09.2026)"""
    ok = True
    doc = OUT / "broken-html.md"
    # an HTML block whose last tag never ends: the walker used to sit on that '<' and never move on
    doc.write_text('<p align="center">\n ; <i>красиво<\n\nОбычный текст после него.\n', encoding="utf-8")
    try:
        proc, hwnd = launch(doc)
    except RuntimeError:
        return check("6.5 a document with an unfinished tag opens at all", False, "no window: the parser hung")
    try:
        ok &= check("6.5 a document with an unfinished tag opens at all", title_of(hwnd).startswith("broken-html.md"),
                    title_of(hwnd))
        cmd(hwnd, "SELECT_ALL", 0.3)
        cmd(hwnd, "COPY", 0.4)
        ok &= check("6.5 and the text after it is still there", "Обычный текст после него." in clipboard(),
                    repr(clipboard()[:60]))
    finally:
        close_and_wait(proc, hwnd)
    return ok


# ------------------------------------------------------------------------------------------------ task lists
TASKS_MD = """---
title: Задачи
---

# Список дел

Кириллица перед списком: смещения в байтах и в символах здесь расходятся.

- [ ] Первая задача
- [x] Вторая, уже сделана
- [X] Третья, отмечена заглавной
  - [ ] Вложенная
1. [ ] Нумерованная

> - [ ] В цитате

```text
- [ ] это код, а не задача
```
"""
TASK_LINES = ["Первая задача", "Вторая, уже сделана", "Третья, отмечена заглавной", "Вложенная", "Нумерованная",
              "В цитате"]


def task_box(hwnd, k):
    c = q(hwnd, "TASK_BOX", k)
    return None if c < 0 else (c & 0xFFFF, c >> 16)


def task_text(marks):
    """TASKS_MD with the boxes set to marks (one character per task, in document order)"""
    text = TASKS_MD
    for line, m in zip(TASK_LINES, marks):
        at = text.index("] " + line)
        text = text[:at - 1] + m + text[at:]
    return text


def test_tasks():
    """a click on a task box ticks the item in the file: one character changes, in the file's own encoding. Since edit
    mode's phase 1c the tick is a splice, a save and a model swap (EDIT-MODE.md T2): "no reload" is Q_RELOADS"""
    ok = True
    acp = k32.GetACP()
    variants = [  # name, how the text becomes the file's bytes, Q_EDIT_ENC (code page | BOM bytes << 24), Q_EDIT_EOL
        ("utf8", lambda t: t.encode("utf-8"), 65001, 0),
        ("bom-crlf", lambda t: b"\xef\xbb\xbf" + t.replace("\n", "\r\n").encode("utf-8"), 65001 | 3 << 24, 1),
        ("utf16", lambda t: b"\xff\xfe" + t.replace("\n", "\r\n").encode("utf-16-le"), 1200 | 2 << 24, 1),
        ("ansi", lambda t: t.encode(f"cp{acp}", errors="replace"), acp, 0),  # not UTF-8: read in the ANSI code page
    ]
    start = " xX   "
    for name, encode, enc, eol in variants:
        doc = OUT / f"tasks-{name}.md"
        doc.write_bytes(encode(TASKS_MD))
        proc, hwnd = launch(doc, size="--size=900x800")
        try:
            states = [q(hwnd, "TASK", k) for k in range(7)]
            ok &= check(f"tasks ({name}): six boxes, the code block has none, [X] counts as ticked",
                        states == [0, 1, 1, 0, 0, 0, -1], str(states))
            ok &= check(f"tasks ({name}): the encoding and the line end are known (Q_EDIT_ENC, Q_EDIT_EOL)",
                        q(hwnd, "EDIT_ENC") == enc and q(hwnd, "EDIT_EOL") == eol,
                        f'{q(hwnd, "EDIT_ENC"):#x} / {q(hwnd, "EDIT_EOL")}, want {enc:#x} / {eol}')
            reloads = q(hwnd, "RELOADS")
            marks = list(start)
            for k in (0, 2, 5, 0, 3):  # tick, untick the capital X, the quoted one, the first one back, the nested
                box = task_box(hwnd, k)
                if not box:
                    ok &= check(f"tasks ({name}): box {k} is on screen", False)
                    break
                click(hwnd, box[0], box[1], 0.5)
                marks[k] = " " if marks[k] != " " else "x"
            want = encode(task_text("".join(marks)))
            got = doc.read_bytes()
            ok &= check(f"tasks ({name}): the file changed in the marks only, byte for byte", got == want,
                        f"{len(got)} vs {len(want)} bytes" if got != want else "")
            states = [q(hwnd, "TASK", k) for k in range(6)]
            ok &= check(f"tasks ({name}): the boxes on screen follow", states == [int(m != " ") for m in marks],
                        f"{states} for {marks!r}")
            time.sleep(0.4)  # the watcher saw our own writes: they must not reload the document
            ok &= check(f"tasks ({name}): our own write does not reload the document",
                        q(hwnd, "RELOADS") == reloads and q(hwnd, "SAVES") == 5,
                        f'reloads {reloads} → {q(hwnd, "RELOADS")}, saves {q(hwnd, "SAVES")}')
            ok &= check(f"tasks ({name}): the source in memory is the file (Q_SRC_HASH), nothing unsaved",
                        q(hwnd, "SRC_HASH", 0) == q(hwnd, "SRC_HASH", 1) and q(hwnd, "EDIT_DIRTY") == 0 and
                        q(hwnd, "EDIT_ENC") == enc)
            if name != "utf8":
                continue
            # the Markdown source in memory follows too: "copy as Markdown" gives the new mark
            cmd(hwnd, "SELECT_ALL", 0.3)
            cmd(hwnd, "COPY_MD", 0.4)
            ok &= check("tasks: copy as Markdown gives the ticked source", "- [x] Вложенная" in clipboard(),
                        repr(clipboard()[:80]))
            post(hwnd, WM_KEYDOWN, 0x1B, 0, 0.2)
            # a press on a box that is let go somewhere else is not a click. (The accent the box takes under the pointer
            # is not checked here: a posted WM_MOUSEMOVE is followed at once by WM_MOUSELEAVE, the real pointer being
            # elsewhere, and the test does not move the reader's mouse.)
            box = task_box(hwnd, 4)
            before = doc.read_bytes()
            post(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp(*box), 0.05)
            post(hwnd, WM_MOUSEMOVE, MK_LBUTTON, lp(box[0] + 200, box[1] + 40), 0.05)
            post(hwnd, WM_LBUTTONUP, 0, lp(box[0] + 200, box[1] + 40), 0.5)
            ok &= check("tasks: pressed on a box and let go elsewhere changes nothing",
                        doc.read_bytes() == before and q(hwnd, "TASK", 4) == 0)
            # a read-only file: nothing is written, and the box stays as the file says
            os.chmod(doc, 0o444)
            try:
                click(hwnd, box[0], box[1], 0.5)
                ok &= check("tasks: a read-only file is left alone", doc.read_bytes() == before and q(hwnd, "TASK", 4) == 0)
            finally:
                os.chmod(doc, 0o666)
            # a file another program holds without letting others write: the same
            k32.CreateFileW.restype = wt.HANDLE
            k32.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, ctypes.c_void_p, wt.DWORD, wt.DWORD, wt.HANDLE]
            k32.CloseHandle.argtypes = [wt.HANDLE]
            h = k32.CreateFileW(str(doc), 0x80000000, 1, None, 3, 0, None)  # GENERIC_READ, FILE_SHARE_READ only
            try:
                click(hwnd, box[0], box[1], 0.5)
                ok &= check("tasks: a file locked by another program is left alone",
                            doc.read_bytes() == before and q(hwnd, "TASK", 4) == 0)
            finally:
                k32.CloseHandle(h)
            shot(hwnd, "59-tasks-busy-toast")
            # changed on disk behind the window's back: the click must not write into a file it has not seen
            reloads = q(hwnd, "RELOADS")
            changed = before + "\nДописано снаружи.\n".encode("utf-8")
            doc.write_bytes(changed)
            post(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp(*box), 0)  # before the watcher's reload (120 ms debounce)
            post(hwnd, WM_LBUTTONUP, 0, lp(*box), 0.8)
            ok &= check("tasks: a file changed on disk is reloaded, not written",
                        doc.read_bytes() == changed and q(hwnd, "RELOADS") > reloads and q(hwnd, "TASK", 4) == 0,
                        f'reloads {reloads} → {q(hwnd, "RELOADS")}')
            click(hwnd, box[0], box[1], 0.5)  # and after the reload the same click works
            ok &= check("tasks: after the reload the box can be ticked",
                        doc.read_bytes() == changed.replace("[ ] Нумерованная".encode("utf-8"),
                                                            "[x] Нумерованная".encode("utf-8"))
                        and q(hwnd, "TASK", 4) == 1)
            shot(hwnd, "60-tasks-light")
            cmd(hwnd, "THEME_DARK", 0.5)
            shot(hwnd, "61-tasks-dark")
        finally:
            close_and_wait(proc, hwnd)
    return ok


# ------------------------------------------------------------------------------------------------ 5.6 updates
def test_update():
    """the update check: one request, a newer version noticed, the installer downloaded and checked by its hash"""
    import hashlib
    import http.server
    import socketserver
    import threading
    ok = True
    payload = b"MZ" + b"fake installer, only here to be hashed" * 256  # over the updater's 4 KB floor
    digest = hashlib.sha256(payload).hexdigest()

    class Handler(http.server.BaseHTTPRequestHandler):
        hits = 0
        tag = "v99.9.9"  # what /latest answers; "" = the server fails

        def do_GET(self):
            Handler.hits += 1
            host = f"http://127.0.0.1:{self.server.server_address[1]}"
            if self.path.endswith("/latest") and not Handler.tag:
                self.send_response(500)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            if self.path.endswith("/latest"):
                body = ('{"tag_name": "' + Handler.tag + '", "name": "FastMD", "assets": ['
                        '{"name": "FastMD-99.9.9-win-x64.zip", "browser_download_url": "' + host + '/FastMD.zip"},'
                        '{"name": "FastMD-Setup.exe", "browser_download_url": "' + host + '/FastMD-Setup.exe"},'
                        '{"name": "FastMD-Setup.exe.sha256", "browser_download_url": "' + host + '/FastMD-Setup.exe.sha256"}]}'
                        ).encode()
            elif self.path.endswith("Setup.exe"):
                body = payload
            elif self.path.endswith(".sha256"):
                body = (digest + "  FastMD-Setup.exe\n").encode()
            else:
                body = b"{}"
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *a):
            pass

    srv = socketserver.TCPServer(("127.0.0.1", 0), Handler)
    port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    doc = OUT / "other.md"
    ENV["FASTMD_UPDATE_URL"] = f"http://127.0.0.1:{port}/releases/latest"
    try:
        set_reg("UpdateSeenLo", 0)  # "checked today" is remembered in the profile: forget it
        set_reg("UpdateSeenHi", 0)
        proc, hwnd = launch(doc)
        try:
            for _ in range(40):
                if q(hwnd, "UPDATE", 0):
                    break
                time.sleep(0.25)
            ok &= check("5.6 a newer release is noticed", q(hwnd, "UPDATE", 0) == 1)
            q(hwnd, "UPDATE", 1)  # download and verify, without running anything
            for _ in range(40):
                if q(hwnd, "UPDATE", 2):
                    break
                time.sleep(0.25)
            ok &= check("5.6 the installer is downloaded and its hash matches", q(hwnd, "UPDATE", 2) == 1)
            got = DATA / "update" / "FastMD-Setup.exe"
            ok &= check("5.6 it lands in the profile, byte for byte",
                        got.exists() and got.read_bytes() == payload, str(got))
            ok &= check("5.6 one check, one installer, one hash", Handler.hits == 3, f"{Handler.hits}")
            ok &= check("5.6 the version found is remembered", reg_value("UpdateVersion") == "99.9.9",
                        str(reg_value("UpdateVersion")))
        finally:
            close_and_wait(proc, hwnd)
        # the next window knows about it at once, without asking GitHub again the same day
        Handler.hits = 0
        proc, hwnd = launch(doc)
        try:
            time.sleep(1.0)
            ok &= check("5.6 a later window shows the remembered update without asking",
                        q(hwnd, "UPDATE", 0) == 1 and q(hwnd, "UPDATE", 3) == US_AVAILABLE and Handler.hits == 0,
                        f"found={q(hwnd, 'UPDATE', 0)} status={q(hwnd, 'UPDATE', 3)} hits={Handler.hits}")
            img = shot(hwnd, "70-update-dot")
            box = gear_box(hwnd)
            ok &= check("5.6 the gear button carries a dot", box is not None and
                        find_color(img, ACCENT, (box[2] - 12, box[1], box[2], box[1] + 12), 90) is not None,
                        str(box))
            cmd(hwnd, "SETTINGS", 0.8)
            sh = q(hwnd, "SETTINGS_HWND")
            ok &= check("5.6 the settings offer the update", sh != 0 and q(hwnd, "SETTINGS_HIT", 800) > 0)
            shot(sh, "71-settings-update-available")
            # check now (the button's state when nothing is known): GitHub now says this is the latest
            Handler.tag = "v0.0.1"
            q(hwnd, "UPDATE", 4)
            for _ in range(40):
                if q(hwnd, "UPDATE", 3) != US_CHECKING:
                    break
                time.sleep(0.25)
            ok &= check("5.6 check now: up to date, the remembered version is forgotten",
                        q(hwnd, "UPDATE", 3) == US_LATEST and q(hwnd, "UPDATE", 0) == 0 and
                        reg_value("UpdateVersion") is None, f"status={q(hwnd, 'UPDATE', 3)}")
            # the button itself: "Check now" asks again, GitHub has a newer one now
            Handler.tag = "v99.9.9"
            time.sleep(0.3)
            click_setting(hwnd, sh, 800)
            for _ in range(40):
                if q(hwnd, "UPDATE", 3) != US_CHECKING:
                    break
                time.sleep(0.25)
            ok &= check("5.6 the settings button checks right away and finds it", q(hwnd, "UPDATE", 3) == US_AVAILABLE,
                        f"status={q(hwnd, 'UPDATE', 3)}")
            shot(sh, "72-settings-after-check")
            post(sh, WM_KEYDOWN, 0x1B, 0, 0.3)
        finally:
            close_and_wait(proc, hwnd)
        # GitHub does not answer: tried again an hour later, not the next day
        Handler.tag = ""
        set_reg("UpdateSeenLo", 0)
        set_reg("UpdateSeenHi", 0)
        del_reg("UpdateVersion")
        proc, hwnd = launch(doc)
        try:
            for _ in range(40):
                if q(hwnd, "UPDATE", 3) == US_CHECK_FAILED:
                    break
                time.sleep(0.25)
            seen = ((reg_value("UpdateSeenHi") or 0) << 32) | (reg_value("UpdateSeenLo") or 0)
            now = (int(time.time()) + 11644473600) * 10_000_000
            ago = (now - seen) / 36e9  # hours the stored stamp lies in the past
            ok &= check("5.6 a failed check is tried again in an hour", q(hwnd, "UPDATE", 3) == US_CHECK_FAILED and
                        22.5 < ago < 23.5, f"status={q(hwnd, 'UPDATE', 3)} stamp {ago:.2f} h ago")
        finally:
            close_and_wait(proc, hwnd)
        Handler.tag = "v99.9.9"
        # switched off in the settings: no request at all
        Handler.hits = 0
        set_reg("UpdateCheck", 0)
        set_reg("UpdateSeenLo", 0)
        set_reg("UpdateSeenHi", 0)
        proc, hwnd = launch(doc)
        time.sleep(2.0)
        ok &= check("5.6 switched off, nothing is asked", Handler.hits == 0 and q(hwnd, "UPDATE", 0) == 0)
        close_and_wait(proc, hwnd)
        set_reg("UpdateCheck", 1)
    finally:
        ENV.pop("FASTMD_UPDATE_URL", None)
        srv.shutdown()
    return ok


# ------------------------------------------------------------------------------------------------ 4.1, 4.2 math
MATH_DOC = r"""# Формулы

В строке: $E = mc^2$ — и дальше текст.

$$\sum_{i=1}^{n} \frac{x_i}{\sqrt{2\pi}}$$

```mermaid
graph TD;
  A[Начало] --> B{Готово?};
  B -->|да| C[Конец];
```

Сломанная: $\oops{\bad$ — остаётся исходником.

Конец документа.
"""


def test_math():
    """formulas and Mermaid diagrams are typeset into the page; what cannot be typeset stays as its source"""
    ok = True
    doc = OUT / "math.md"
    doc.write_text(MATH_DOC, encoding="utf-8")
    proc, hwnd = launch(doc, size="--size=900x900")
    try:
        for _ in range(40):  # they are drawn on a worker thread after the first frame
            if q(hwnd, "MATH", 1) >= 3:
                break
            time.sleep(0.25)
        ok &= check("4.1 the document has four formulas and diagrams", q(hwnd, "MATH", 0) == 4, f'{q(hwnd, "MATH", 0)}')
        ok &= check("4.1 three of them are drawn", q(hwnd, "MATH", 1) == 3, f'{q(hwnd, "MATH", 1)}')
        ok &= check("4.1 the broken one is not, and says so", q(hwnd, "MATH", 2) == 1, f'{q(hwnd, "MATH", 2)}')
        q(hwnd, "FULL_REDRAW")
        time.sleep(0.4)
        img = shot(hwnd, "26-math")
        left = q(hwnd, "TEXT_LEFT")
        # the display formula stands on its own line under "В строке:", the diagram below it
        y1, y2 = q(hwnd, "BLOCK_Y", 2), q(hwnd, "BLOCK_Y", 3)
        ok &= check("4.1 the formula of its own is drawn", ink(img, (left, y1, left + 300, y2 - 8)) > 200,
                    f"{ink(img, (left, y1, left + 300, y2 - 8))}")
        ok &= check("4.2 the diagram is drawn", ink(img, (left, y2, left + 400, y2 + 300)) > 2000,
                    f"{ink(img, (left, y2, left + 400, y2 + 300))}")
        ok &= check("4.1 a formula in the line does not break the line",
                    q(hwnd, "BLOCK_Y", 2) - q(hwnd, "BLOCK_Y", 1) < 80,
                    f'{q(hwnd, "BLOCK_Y", 2) - q(hwnd, "BLOCK_Y", 1)}')
    finally:
        close_and_wait(proc, hwnd)
    return ok


# ------------------------------------------------------------------------------------------------ 3.5 drag out
DF_TEXT, DF_HTML, DF_RTF, DF_URL, DF_DIB, DF_FILE = 1, 2, 4, 8, 16, 32



WIDE_DIAGRAM_DOC = """# Широкая диаграмма

```mermaid
graph LR
    A[Первый очень длинный узел] --> B[Второй очень длинный узел]
    B --> C[Третий очень длинный узел]
    C --> D[Четвёртый очень длинный узел]
    D --> E[Пятый очень длинный узел]
    E --> F[Шестой очень длинный узел]
```

Хвост документа.
"""


def test_wide_diagram():
    """a diagram too wide for the column keeps its own size and scrolls sideways instead of shrinking (plan 4.2)"""
    ok = True
    doc = OUT / "wide-diagram.md"
    doc.write_text(WIDE_DIAGRAM_DOC, encoding="utf-8")
    proc, hwnd = launch(doc)
    try:
        blk = -1
        for _ in range(40):  # drawn on a worker thread after the first frame
            blk = q(hwnd, "HSCROLL_BLOCK")
            if blk >= 0:
                break
            time.sleep(0.25)
        ok &= check("4.2 a diagram wider than the column scrolls sideways", blk >= 0, str(blk))
        if blk < 0:
            return False
        tall = q(hwnd, "BLOCK_Y", blk + 1) - q(hwnd, "BLOCK_Y", blk)
        ok &= check("4.2 it keeps its own size, so the labels stay readable", tall > 60, f"{tall} DIP tall")
        y = q(hwnd, "BLOCK_Y", blk) + 30
        before = shot(hwnd, "27-wide-diagram")
        wheel(hwnd, 500, y, -3, keys=MK_SHIFT)
        x1 = q(hwnd, "HSCROLL_X", blk)
        ok &= check("4.2 Shift+wheel moves the diagram", x1 > 0, str(x1))
        wheel(hwnd, 500, y, 2, horizontal=True)
        x2 = q(hwnd, "HSCROLL_X", blk)
        ok &= check("4.2 the touchpad moves it further", x2 > x1, f"{x1} → {x2}")
        after = shot(hwnd, "28-wide-diagram-scrolled")
        box = (40, q(hwnd, "BLOCK_Y", blk), 980, q(hwnd, "BLOCK_Y", blk) + tall)
        moved = sum(1 for p in ImageChops.difference(before.crop(box).convert("RGB"),
                                                     after.crop(box).convert("RGB")).getdata() if sum(p) > 60)
        ok &= check("4.2 what the pane shows really changes", moved > 500, f"{moved} pixels")
        wheel(hwnd, 500, y, 30, keys=MK_SHIFT)
        ok &= check("4.2 and back to the beginning", q(hwnd, "HSCROLL_X", blk) == 0)
    finally:
        close_and_wait(proc, hwnd)
    return ok

def test_drag(doc):
    """what a drag out of the window would carry: the shell runs the drag itself, the formats are ours"""
    ok = True
    proc, hwnd = launch(doc)
    try:
        ok &= check("3.5 nothing is dragged out of an empty selection", q(hwnd, "DRAG", 0) == 0)
        cmd(hwnd, "SELECT_ALL", 0.4)
        ok &= check("3.5 the selection goes out as text, HTML and RTF",
                    q(hwnd, "DRAG", 0) == DF_TEXT | DF_HTML | DF_RTF, f'{q(hwnd, "DRAG", 0)}')
        ok &= check("3.5 a link goes out as its address", q(hwnd, "DRAG", 1 << 16) == DF_TEXT | DF_URL,
                    f'{q(hwnd, "DRAG", 1 << 16)}')
        post(hwnd, WM_KEYDOWN, 0x23, 0, 0.8)  # End: the picture at the bottom of the document
        ok &= check("3.5 a picture goes out as a bitmap and as the file it came from",
                    q(hwnd, "DRAG", (2 << 16) | 0xFFFF) == DF_DIB | DF_FILE, f'{q(hwnd, "DRAG", (2 << 16) | 0xFFFF)}')
    finally:
        close_and_wait(proc, hwnd)
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
        click_setting(hwnd, sh, 902)    # English (row 9: theme, font, size, column, wrap, smooth, remote, update, version, language)
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
        for id_ in (0, 100, 202, 900):  # back to the defaults
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


# ------------------------------------------------------------------------------------------------ edit mode, phase 1a
def dbl_click(hwnd, x, y, wait=0.3):
    """the app counts clicks itself (no CS_DBLCLKS): two quick press/release pairs at the same point"""
    post(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp(x, y), 0.01)
    post(hwnd, WM_LBUTTONUP, 0, lp(x, y), 0.01)
    post(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp(x, y), 0.01)
    post(hwnd, WM_LBUTTONUP, 0, lp(x, y), wait)


def wait_for(pred, timeout=5.0, step=0.1):
    end = time.time() + timeout
    while time.time() < end:
        if pred():
            return True
        time.sleep(step)
    return bool(pred())


def test_map_selfcheck():
    """edit mode's text <-> source map holds on real documents: the app parses the whole source with the map and
    checks every invariant of EDIT-MODE.md §4.5 (Q_MAP_SELFCHECK); reading mode itself is untouched"""
    ok = True
    (OUT / "map-math.md").write_text(MATH_DOC, encoding="utf-8")
    for name in ("features.md", "footnotes.md", "html.md"):
        shutil.copy(HERE / name, OUT / f"map-{name}")
    docs = [OUT / "map-features.md", REPO / "bench" / "corpus" / "medium.md", OUT / "map-footnotes.md",
            OUT / "map-html.md", OUT / "map-math.md", REPO / "bench" / "corpus" / "large.md"]
    for doc in docs:
        proc, hwnd = launch(doc)
        try:
            if doc.name == "large.md":  # the first screen comes from a prefix; the full parse follows on a thread
                counts = []
                wait_for(lambda: counts.append(q(hwnd, "BLOCK_COUNT")) or (len(counts) > 3 and counts[-1] == counts[-4]
                                                                            and counts[-1] > 2000), 8.0, 0.25)
            ok &= check(f"edit 1a: the map of {doc.name} passes its self-check", q(hwnd, "MAP_SELFCHECK", 0) == 1,
                        f'{q(hwnd, "MAP_SELFCHECK", 0)} (blocks {q(hwnd, "BLOCK_COUNT")})')
            ok &= check(f"edit 1a: {doc.name} is read, not edited; no self-check failures counted",
                        q(hwnd, "EDITING") == 0 and q(hwnd, "EDIT_CARET_SRC") == -1 and q(hwnd, "MAP_SELFCHECK", 1) == 0)
        finally:
            close_and_wait(proc, hwnd)
    return ok


COPY_MD_DOC = ("# Заголовок первой строки\n\nСущность &copy; и слово после неё.\n\nФлаг :england: и слово\n\n"
               "[ref]: http://example.com\n")
FRONT_DOC = "---\ntitle: Привет\nauthor: Кто-то\n---\n\n# Заголовок\n\nТекст.\n"


def test_copy_md_exact():
    """copy as Markdown gives back exactly the source lines: the file's first line whole, the text after an emoji
    whose picture is longer than its :shortcode: and after an entity where it really is; a selection inside the
    front-matter table gives its own line instead of the whole file"""
    ok = True
    doc = OUT / "copy-md.md"
    doc.write_text(COPY_MD_DOC, encoding="utf-8")
    proc, hwnd = launch(doc)
    try:
        cmd(hwnd, "SELECT_ALL", 0.3)
        cmd(hwnd, "COPY_MD", 0.5)
        want = "# Заголовок первой строки\n\nСущность &copy; и слово после неё.\n\nФлаг :england: и слово"
        md = clipboard().replace("\r\n", "\n")
        ok &= check("edit 1a: copy as Markdown of everything is the source, first line whole, nothing after the emoji",
                    md == want, repr(md[-40:] if md.startswith("# ") else md[:40]))
        # (no Esc between the steps: with nothing selected it would close the window; a double click replaces the
        # selection anyway)
        dbl_click(hwnd, q(hwnd, "TEXT_LEFT") + 12, q(hwnd, "BLOCK_Y", 0) + 20)
        has = q(hwnd, "SEL_ANCHOR") != q(hwnd, "SEL_FOCUS")
        cmd(hwnd, "COPY_MD", 0.5)
        md = clipboard().replace("\r\n", "\n")
        ok &= check("edit 1a: a word on the first line copies that line whole", has and md == "# Заголовок первой строки",
                    repr(md[:40]))
        time.sleep(0.6)  # not a third click
        dbl_click(hwnd, q(hwnd, "TEXT_LEFT") + 12, q(hwnd, "BLOCK_Y", 1) + 10)
        has = q(hwnd, "SEL_ANCHOR") != q(hwnd, "SEL_FOCUS")
        cmd(hwnd, "COPY_MD", 0.5)
        md = clipboard().replace("\r\n", "\n")
        ok &= check("edit 1a: a word before an entity copies its own line", has and md == "Сущность &copy; и слово после неё.",
                    repr(md[:60]))
    finally:
        close_and_wait(proc, hwnd)
    doc = OUT / "copy-md-front.md"
    doc.write_text(FRONT_DOC, encoding="utf-8")
    proc, hwnd = launch(doc)
    try:
        # a word in the first cell of the property table (its header row: "title | Привет")
        x, y = q(hwnd, "TEXT_LEFT"), q(hwnd, "BLOCK_Y", 0)
        has = False
        for dx, dy in ((18, 20), (26, 22), (14, 16)):
            dbl_click(hwnd, x + dx, y + dy)
            if q(hwnd, "SEL_ANCHOR") != q(hwnd, "SEL_FOCUS"):
                has = True
                break
            time.sleep(0.6)
        cmd(hwnd, "COPY_MD", 0.5)
        md = clipboard().replace("\r\n", "\n")
        ok &= check("edit 1a: a word in the front-matter table copies its property line, not the whole file",
                    has and md == "title: Привет", repr(md[:60]))
    finally:
        close_and_wait(proc, hwnd)
    return ok


# ------------------------------------------------------------------------------------------------ edit mode, phase 1b
def test_image_race():
    """the picture worker keeps going while the document changes under it (EDIT-MODE.md §5.2): a theme switch (which
    reloads a document with formulas), reloads and a zoom while renders are still queued. Every render is slowed to
    300 ms (FASTMD_TEST_SLOW), so they really are pending; results made for an older load are dropped, and each source
    is rendered a bounded number of times"""
    ok = True
    doc = OUT / "race-math.md"
    doc.write_text(MATH_DOC, encoding="utf-8")
    ENV["FASTMD_TEST_SLOW"] = "images:300"
    try:
        proc, hwnd = launch(doc, size="--size=900x900")
        try:
            pending = q(hwnd, "MATH", 1) < 3
            cmd(hwnd, "THEME_DARK", 0.1)
            cmd(hwnd, "RELOAD", 0.1)
            cmd(hwnd, "ZOOM_IN", 0.1)
            cmd(hwnd, "THEME_LIGHT", 0.1)
            cmd(hwnd, "RELOAD", 0.1)
            done = wait_for(lambda: q(hwnd, "MATH", 1) == 3 and q(hwnd, "MATH", 2) == 1, 10.0, 0.2)
            renders = q(hwnd, "RENDERS")
            ok &= check("edit 1b: a theme switch, reloads and a zoom while renders are pending: the window lives",
                        pending and proc.poll() is None, f"pending at the switch: {pending}")
            ok &= check("edit 1b: ... and the three pictures of the last load are drawn, the broken one failed", done,
                        f'drawn {q(hwnd, "MATH", 1)}, failed {q(hwnd, "MATH", 2)}')
            # 4 sources in 2 contexts (light, dark); a job queued for an older load is skipped, not rendered
            ok &= check("edit 1b: ... after a bounded number of renders", 0 < renders <= 2 * 4 * 2, f"{renders} (at most 16)")
            q(hwnd, "FULL_REDRAW")
            time.sleep(0.4)
            img = shot(hwnd, "80-image-race")
            left, y1, y2 = q(hwnd, "TEXT_LEFT"), q(hwnd, "BLOCK_Y", 2), q(hwnd, "BLOCK_Y", 3)
            ok &= check("edit 1b: ... and the formula of its own is really drawn",
                        ink(img, (left, y1, left + 300, y2 - 8)) > 200, f"{ink(img, (left, y1, left + 300, y2 - 8))}")
            cmd(hwnd, "ZOOM_RESET", 0.3)
        finally:
            close_and_wait(proc, hwnd)
    finally:
        ENV.pop("FASTMD_TEST_SLOW", None)
    return ok


def test_reload_during_update():
    """a reload waits for no network: not for the update check (detached) and not for a picture being downloaded
    (the picture worker is detached too, and a reload only skips what it had queued). GitHub and the picture server
    both answer after 5 s here, and a reload in the middle returns at once"""
    import http.server
    import socketserver
    import threading
    ok = True
    png = (REPO / "bench" / "corpus" / "img" / "diagram0.png").read_bytes()

    class Handler(http.server.BaseHTTPRequestHandler):
        latest = 0
        pictures = 0

        def do_GET(self):
            if self.path.endswith("/latest"):
                Handler.latest += 1
                body, kind = b'{"tag_name": "v0.0.1", "name": "FastMD", "assets": []}', "application/json"
            else:
                Handler.pictures += 1
                body, kind = png, "image/png"
            time.sleep(5.0)
            try:
                self.send_response(200)
                self.send_header("Content-Type", kind)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            except OSError:
                pass  # the window was closed meanwhile

        def log_message(self, *a):
            pass

    class Server(socketserver.ThreadingMixIn, socketserver.TCPServer):
        daemon_threads = True
        block_on_close = False

    srv = Server(("127.0.0.1", 0), Handler)
    port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    doc = OUT / "slow-picture.md"
    doc.write_text(f"# Медленная картинка\n\n![картинка](http://127.0.0.1:{port}/slow.png)\n\nКонец.\n", encoding="utf-8")
    ENV["FASTMD_UPDATE_URL"] = f"http://127.0.0.1:{port}/releases/latest"
    try:
        set_reg("UpdateSeenLo", 0)  # "checked today" is remembered in the profile: forget it
        set_reg("UpdateSeenHi", 0)
        proc, hwnd = launch(doc)
        try:
            asked = wait_for(lambda: Handler.latest >= 1 and Handler.pictures >= 1, 5.0)
            waiting = q(hwnd, "UPDATE", 3) == US_CHECKING
            serial = q(hwnd, "DOC_SERIAL")
            t0 = time.perf_counter()
            u32.SendMessageW(hwnd, WM_COMMAND, CMD["RELOAD"], 0)
            dt = time.perf_counter() - t0
            ok &= check("edit 1b: the update check and the picture are both waiting for their servers",
                        asked and waiting, f"latest {Handler.latest}, pictures {Handler.pictures}, checking {waiting}")
            ok &= check("edit 1b: a reload meanwhile returns at once", dt < 0.5 and q(hwnd, "DOC_SERIAL") != serial,
                        f"{dt * 1000:.0f} ms")
            drawn = wait_for(lambda: q(hwnd, "DRAG", (2 << 16) | 0xFFFF) & DF_DIB, 12.0, 0.25)
            ok &= check("edit 1b: the picture still arrives, from one download", drawn and Handler.pictures == 1,
                        f"downloads {Handler.pictures}")
        finally:
            close_and_wait(proc, hwnd)
    finally:
        ENV.pop("FASTMD_UPDATE_URL", None)
        srv.shutdown()
    return ok


# ------------------------------------------------------------------------------------------------ edit mode, phase 1c
WM_COPYDATA = 0x004A


class COPYDATASTRUCT(ctypes.Structure):
    _fields_ = [("dwData", ctypes.c_size_t), ("cbData", wt.DWORD), ("lpData", ctypes.c_void_p)]


def src_hash(text):
    """FNV-1a-32 over the UTF-16LE bytes of the text: what Q_SRC_HASH answers for the app's source"""
    h = 2166136261
    for b in text.encode("utf-16-le"):
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def copydata(hwnd, kind, payload=""):
    """FASTMD_TEST_HOOKS: kind 1 = splice "at\\tlen\\ttext" (UTF-16 offsets), 2 = save now → 1 + the save state"""
    data = ctypes.create_unicode_buffer(payload, len(payload) + 1)
    cds = COPYDATASTRUCT(kind, len(payload.encode("utf-16-le")), ctypes.cast(data, ctypes.c_void_p))
    return u32.SendMessageW(hwnd, WM_COPYDATA, 0, ctypes.addressof(cds))


WM_APP_TESTKEY = 0x8000 + 12
KM_CTRL, KM_SHIFT, KM_ALT = 1, 2, 4


def testkey(hwnd, vk, mods=0, wait=0.3):
    """FASTMD_TEST_HOOKS: a key press with exactly these modifiers (the app does not read the keyboard state for it)"""
    u32.SendMessageW(hwnd, WM_APP_TESTKEY, vk, mods)
    time.sleep(wait)


def u16(s):
    return len(s.encode("utf-16-le")) // 2


def splice(hwnd, src, at, length, text):
    """the same splice on the app's source (through the hook) and on the test's copy; at / length in UTF-16 units of
    a BMP-only text, so they are Python indices too"""
    r = copydata(hwnd, 1, f"{at}\t{length}\t{text}")
    return r, src[:at] + text + src[at + length:]


def settle(hwnd, timeout=5.0):
    """nothing on its way any more: no glide, no measuring, no picture pending (Q_EDIT_BUSY), and a frame drawn"""
    done = wait_for(lambda: q(hwnd, "TARGETY") == q(hwnd, "SCROLLY") and q(hwnd, "EDIT_BUSY") == 0, timeout, 0.05)
    time.sleep(0.25)
    return done


def relayout_same(hwnd, name):
    """the oracle of the model swap (T7): the frame now == the frame after every layout was made anew"""
    q(hwnd, "FULL_REDRAW")
    time.sleep(0.3)
    a = shot(hwnd, f"{name}-swap")
    y = q(hwnd, "SCROLLY")
    q(hwnd, "RELAYOUT_ALL")
    time.sleep(0.4)
    b = shot(hwnd, f"{name}-relayout")
    return same_pixels(a, b) and q(hwnd, "SCROLLY") == y


TASK_SWAP_DOC = ("# Задачи рядом с формулами\n\n- [ ] Посчитать $a^2 + b^2$ до обеда\n- [x] Готово: $\\sqrt{x}$\n\n"
                 "$$\\int_0^1 x\\,dx$$\n\n" +
                 "".join(f"## Раздел {k}\n\nАбзац {k}: достаточно текста, чтобы документ прокручивался, и строка "
                         f"переносилась хотя бы один раз в узкой колонке окна.\n\n" +
                         (f"- [ ] Задача в середине {k}\n- [ ] Ещё одна {k}\n\n" if k in (8, 9, 10) else "")
                         for k in range(1, 21)) + "Конец.\n")


def test_task_swap():
    """a tick is a model swap, not a reload (EDIT-MODE.md §5.5, T2): the formulas beside it are not typeset again,
    and in the middle of the document the view stays put and draws exactly what a fresh layout draws"""
    ok = True
    doc = OUT / "task-swap.md"
    doc.write_bytes(TASK_SWAP_DOC.encode("utf-8"))
    proc, hwnd = launch(doc, size="--size=900x700")
    try:
        drawn = wait_for(lambda: q(hwnd, "MATH", 1) == 3, 8.0)
        settle(hwnd)
        renders, reloads = q(hwnd, "RENDERS"), q(hwnd, "RELOADS")
        box = task_box(hwnd, 0)
        if box:
            click(hwnd, box[0], box[1], 0.6)
        settle(hwnd)
        ok &= check("edit 1c: a tick next to a formula: saved, nothing typeset again, no reload",
                    drawn and box and q(hwnd, "TASK", 0) == 1 and q(hwnd, "RENDERS") == renders and
                    q(hwnd, "RELOADS") == reloads and q(hwnd, "MATH", 1) == 3 and "- [x] Посчитать" in doc.read_text("utf-8"),
                    f'drawn {drawn}, renders {renders} → {q(hwnd, "RENDERS")}, math {q(hwnd, "MATH", 1)}')
        ok &= check("edit 1c: ... and the frame is what a fresh layout draws", relayout_same(hwnd, "81-task-formula"))
        # the middle of the document: the ticked box is on screen, the view must not move
        mid = 2  # the first task of section 8
        for _ in range(40):
            b = task_box(hwnd, mid)
            if b and 150 < b[1] < 500:
                break
            wheel(hwnd, 450, 350, -1, wait=0.25)
        settle(hwnd)
        b = task_box(hwnd, mid)
        y = q(hwnd, "SCROLLY")
        if b:
            click(hwnd, b[0], b[1], 0.6)
        settle(hwnd)
        ok &= check("edit 1c: a tick in the middle of the document leaves the scroll position alone",
                    b is not None and y > 0 and q(hwnd, "SCROLLY") == y and q(hwnd, "TASK", mid) == 1 and
                    q(hwnd, "RELOADS") == reloads, f'{y} → {q(hwnd, "SCROLLY")}, box {b}')
        ok &= check("edit 1c: ... and the frame is what a fresh layout draws", relayout_same(hwnd, "82-task-middle"))
        ok &= check("edit 1c: ... the file and the source in memory agree",
                    q(hwnd, "SRC_HASH", 0) == src_hash(doc.read_bytes().decode("utf-8")) and q(hwnd, "EDIT_DIRTY") == 0)
    finally:
        close_and_wait(proc, hwnd)
    return ok


SPLICE_DOC = ("# Документ для правок\n\nПервый абзац с **жирным** словом и [ссылкой](other.md).\n\n"
              "Бейдж в строке: <img src=\"img/diagram0.png\" width=\"40\"> и текст после него.\n\n"
              "1. Первый пункт\n2. Второй пункт\n3. Третий пункт\n\n"
              "| Колонка | Значение |\n|---|---|\n| а | 1 |\n| б | 2 |\n\n"
              "Формула: $x^2$ в строке.\n\nПоследний абзац.\n")


def test_splice_hook():
    """20 scripted splices through the test hook (reading mode, nothing saved): after each the app's source is the
    expected text, the map passes its self-check, and at checkpoints the frame is what a fresh layout draws"""
    ok = True
    doc = OUT / "splice-hook.md"
    doc.write_bytes(SPLICE_DOC.encode("utf-8"))
    original = doc.read_bytes()
    ENV.update(FASTMD_TEST_HOOKS="1", FASTMD_EDIT_SELFCHECK="1")
    try:
        proc, hwnd = launch(doc, size="--size=900x800")
        try:
            wait_for(lambda: q(hwnd, "MATH", 1) == 1, 8.0)
            settle(hwnd)
            src = SPLICE_DOC
            reloads = q(hwnd, "RELOADS")
            para = src.index("Первый абзац")
            steps = [  # (what, at, length, text); each applied to the source as it is after the ones before
                ("type in a paragraph", para + 7, 0, "новый "),
                ("type more", para + 13, 0, "текст "),
                ("a heading appears", para, 0, "## "),
                ("the heading goes", para, 3, ""),
                ("a picture before the badge", src.index("Бейдж"), 0, "![](img/diagram0.png)\n\n"),
                ("the badge's text", None, None, "Значок"),
                ("a list item in front", None, None, "1. Нулевой пункт\n"),
                ("renumber", None, None, "5"),
                ("a table row", None, None, "| в | 3 |\n"),
                ("a cell", None, None, "42"),
                ("the formula", None, None, "y^3"),
                ("a new formula", None, None, " и $z$"),
                ("a block goes", None, None, ""),
                ("a paragraph at the end", None, None, "\nНовый конец.\n"),
                ("a quote", None, None, "> Цитата\n\n"),
                ("into the quote", None, None, " длиннее"),
                ("a code block", None, None, "```\nкод\n```\n\n"),
                ("into the code", None, None, "ещё "),
                ("the first line", 0, 1, "###"),
                ("the picture goes", None, None, ""),
            ]
            for k, (what, at, length, text) in enumerate(steps):
                # where the later steps go is found in the text as it is by then
                if at is None:
                    at, length = {
                        "the badge's text": lambda: (src.index("Бейдж"), len("Бейдж")),
                        "a list item in front": lambda: (src.index("1. Первый"), 0),
                        "renumber": lambda: (src.index("3. Третий"), 1),
                        "a table row": lambda: (src.index("| б | 2 |\n") + len("| б | 2 |\n"), 0),
                        "a cell": lambda: (src.index("| а | 1 |") + 6, 1),
                        "the formula": lambda: (src.index("$x^2$") + 1, 3),
                        "a new formula": lambda: (src.index(" в строке."), 0),
                        "a block goes": lambda: (src.index("Последний абзац.\n"), len("Последний абзац.\n")),
                        "a paragraph at the end": lambda: (len(src), 0),
                        "a quote": lambda: (src.index("| Колонка"), 0),
                        "into the quote": lambda: (src.index("> Цитата") + len("> Цитата"), 0),
                        "a code block": lambda: (src.index("Формула:"), 0),
                        "into the code": lambda: (src.index("код\n```"), 0),
                        "the picture goes": lambda: (src.index("![](img/diagram0.png)\n\n"), len("![](img/diagram0.png)\n\n")),
                    }[what]()
                r, src = splice(hwnd, src, at, length, text)
                okk = r == 1 and q(hwnd, "SRC_HASH", 0) == src_hash(src) and q(hwnd, "SRC_LEN", 0) == u16(src)
                okk = okk and q(hwnd, "MAP_SELFCHECK", 0) == 1
                if k % 4 == 3 or k == len(steps) - 1:
                    settle(hwnd)
                    okk = okk and relayout_same(hwnd, f"83-splice-{k:02d}")
                ok &= check(f"edit 1c: splice {k + 1:2d} ({what}): the source is right, the map holds"
                            + (", the frame is a fresh layout's" if k % 4 == 3 or k == len(steps) - 1 else ""), okk,
                            f"hook {r}, hash {q(hwnd, 'SRC_HASH', 0):#x} vs {src_hash(src):#x}")
            ok &= check("edit 1c: 20 splices: no reload, nothing saved, the file untouched, no self-check failure",
                        q(hwnd, "RELOADS") == reloads and q(hwnd, "SAVES") == 0 and doc.read_bytes() == original and
                        q(hwnd, "MAP_SELFCHECK", 1) == 0 and q(hwnd, "EDIT_DIRTY") == 1 and
                        q(hwnd, "SRC_HASH", 1) == src_hash(SPLICE_DOC),
                        f'reloads {q(hwnd, "RELOADS")}, saves {q(hwnd, "SAVES")}, failures {q(hwnd, "MAP_SELFCHECK", 1)}')
            stats = [q(hwnd, "EDIT_STATS", k) for k in range(7)]
            print(f"       swap cost over {stats[3]} swaps: median {stats[0]} µs, p95 {stats[1]} µs, max {stats[2]} µs "
                  f"(parse {stats[4]}, carry+diff {stats[5]}, install+layout {stats[6]})")
            r, src = splice(hwnd, src, len(src), 0, "Строка с CRLF\r\n")
            ok &= check("edit 1c: a splice that would cut a CRLF in two, or run past the end, is refused",
                        r == 1 and copydata(hwnd, 1, f"{len(src) - 1}\t0\tx") == 0 and
                        copydata(hwnd, 1, f"{len(src)}\t1\tx") == 0 and q(hwnd, "SRC_HASH", 0) == src_hash(src))
            # the hook's save: the whole edit in one go, byte for byte
            r = copydata(hwnd, 2)
            ok &= check("edit 1c: the save hook writes the edits, byte for byte",
                        r == 1 and doc.read_bytes() == src.encode("utf-8") and q(hwnd, "SAVES") == 1 and
                        q(hwnd, "EDIT_DIRTY") == 0, f"hook {r}")
            time.sleep(0.5)
            ok &= check("edit 1c: ... and the watcher does not reload our own write", q(hwnd, "RELOADS") == reloads)
            testkey(hwnd, ord("F"), KM_CTRL)  # a chord with its modifiers named, whatever the keyboard says (T5)
            opened = q(hwnd, "FIND_OPEN") == 1
            testkey(hwnd, VK["esc"])
            ok &= check("edit 1c: WM_APP_TESTKEY delivers Ctrl+F and Esc", opened and q(hwnd, "FIND_OPEN") == 0)
        finally:
            close_and_wait(proc, hwnd)
    finally:
        ENV.pop("FASTMD_TEST_HOOKS", None)
        ENV.pop("FASTMD_EDIT_SELFCHECK", None)
    return ok


def recovery_files():
    d = DATA / "recovery"
    return sorted(d.glob("*.rec")) if d.exists() else []


def test_save_fault():
    """FASTMD_TEST_FAIL_WRITE (§13.5): a write that fails half-way is rolled back - the original bytes, no recovery
    file - and the next save goes through; with ",norollback" the torn file and its recovery file stay, and the next
    open offers the recovery strip, whose Restore gives the original bytes back"""
    ok = True
    doc = OUT / "save-fault.md"
    text = "# Сбой записи\n\nАбзац, который будет дописан.\n\nВторой абзац.\n"
    doc.write_bytes(text.encode("utf-8"))
    original = doc.read_bytes()
    for f in recovery_files():
        f.unlink()
    ENV.update(FASTMD_TEST_HOOKS="1", FASTMD_TEST_FAIL_WRITE="partial:10")
    try:
        proc, hwnd = launch(doc)
        try:
            at = text.index("дописан.") + len("дописан")
            r, new = splice(hwnd, text, at, 0, " и расширен до длинного текста")
            s1 = copydata(hwnd, 2)
            ok &= check("edit 1c: a write that fails after 10 bytes: FAILED, the original bytes, still dirty, "
                        "no recovery file", r == 1 and s1 == 1 + 9 and doc.read_bytes() == original and
                        q(hwnd, "EDIT_SAVE_STATE") == 9 and q(hwnd, "EDIT_DIRTY") == 1 and not recovery_files(),
                        f"hook {r}/{s1}, state {q(hwnd, 'EDIT_SAVE_STATE')}, recovery {recovery_files()}")
            s2 = copydata(hwnd, 2)
            ok &= check("edit 1c: ... the next save goes through", s2 == 1 and doc.read_bytes() == new.encode("utf-8") and
                        q(hwnd, "EDIT_SAVE_STATE") == 0 and q(hwnd, "SAVES") == 1, f"hook {s2}")
        finally:
            close_and_wait(proc, hwnd)
        doc.write_bytes(original)
        ENV["FASTMD_TEST_FAIL_WRITE"] = "partial:10,norollback"
        proc, hwnd = launch(doc)
        try:
            r, new = splice(hwnd, text, at, 0, " и расширен до длинного текста")
            s1 = copydata(hwnd, 2)
            torn = doc.read_bytes()
            ok &= check("edit 1c: partial:10,norollback: a torn file and its recovery file",
                        s1 == 1 + 9 and torn != original and torn != new.encode("utf-8") and len(recovery_files()) == 1,
                        f"hook {s1}, recovery {recovery_files()}")
        finally:
            close_and_wait(proc, hwnd)
    finally:
        ENV.pop("FASTMD_TEST_HOOKS", None)
        ENV.pop("FASTMD_TEST_FAIL_WRITE", None)
    proc, hwnd = launch(doc)
    try:
        shown = wait_for(lambda: q(hwnd, "EDIT_STRIP") == 6, 3.0)
        shot(hwnd, "84-recovery-strip")
        c = q(hwnd, "EDIT_TOOL", CMD["RECOVERY_RESTORE"])
        ok &= check("edit 1c: reopening shows the recovery strip with its buttons", shown and c > 0,
                    f'strip {q(hwnd, "EDIT_STRIP")}, restore at {c}')
        # Open the copy: the file as it was before that save, rebuilt in %TEMP%\FastMD and opened in a window of its own
        tmp = pathlib.Path(os.environ["TEMP"]) / "FastMD"
        for f in tmp.glob("save-fault (*).md"):
            f.unlink()
        cmd(hwnd, "RECOVERY_OPEN", 1.5)
        copies = list(tmp.glob("save-fault (*).md"))
        others = []
        cb_t = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

        def cb(h, _):
            if u32.IsWindowVisible(h) and title_of(h).startswith("save-fault ("):
                others.append(h)
            return True

        u32.EnumWindows(cb_t(cb), 0)
        ok &= check("edit 1c: Open the copy rebuilds the file before the save and opens it in a new window",
                    len(copies) == 1 and copies[0].read_bytes() == original and len(others) == 1 and
                    q(hwnd, "EDIT_STRIP") == 6, f"copies {copies}, windows {len(others)}")
        for h in others:
            post(h, WM_CLOSE, 0, 0, 0.5)
        reloads = q(hwnd, "RELOADS")
        if c > 0:
            click(hwnd, c & 0xFFFF, c >> 16, 0.8)  # the real button (its command is CMD_RECOVERY_RESTORE)
        ok &= check("edit 1c: Restore gives the original bytes back, reloads, and the strip and the recovery file go",
                    doc.read_bytes() == original and q(hwnd, "EDIT_STRIP") == 0 and not recovery_files() and
                    q(hwnd, "RELOADS") == reloads + 1 and q(hwnd, "SRC_HASH", 0) == src_hash(text),
                    f'strip {q(hwnd, "EDIT_STRIP")}, recovery {recovery_files()}')
    finally:
        close_and_wait(proc, hwnd)
    return ok


def test_reading_touch():
    """reading mode (§10.7): a file touched without a change is not reloaded, a changed one is; a file that is gone
    for a moment is not replaced by the error page; a file locked while it changes is read once it is free"""
    ok = True
    doc = OUT / "touch.md"
    text = "# Касание\n\nТекст не меняется, меняется только время файла.\n"
    doc.write_bytes(text.encode("utf-8"))
    proc, hwnd = launch(doc)
    try:
        reloads = q(hwnd, "RELOADS")
        st = doc.stat()
        os.utime(doc, (st.st_atime + 60, st.st_mtime + 60))
        time.sleep(0.6)
        ok &= check("edit 1c: a touched file with the same text is not reloaded", q(hwnd, "RELOADS") == reloads)
        moved = OUT / "touch-moved.md"
        doc.rename(moved)
        time.sleep(0.6)
        gone = q(hwnd, "RELOADS") == reloads and q(hwnd, "SRC_HASH", 0) == src_hash(text)
        moved.rename(doc)
        time.sleep(0.6)
        ok &= check("edit 1c: a file gone for a moment is not replaced by the error page, nor reloaded when it is back",
                    gone and q(hwnd, "RELOADS") == reloads, f'reloads {q(hwnd, "RELOADS")}')
        text2 = text.replace("не меняется", "меняется")
        doc.write_bytes(text2.encode("utf-8"))
        ok &= check("edit 1c: a changed file is reloaded",
                    wait_for(lambda: q(hwnd, "RELOADS") == reloads + 1 and q(hwnd, "SRC_HASH", 0) == src_hash(text2), 3.0))
        # changed and locked at once: not read while locked, read (once) when free
        k32.CreateFileW.restype = wt.HANDLE
        k32.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, ctypes.c_void_p, wt.DWORD, wt.DWORD, wt.HANDLE]
        k32.CloseHandle.argtypes = [wt.HANDLE]
        text3 = text2 + "\nЕщё строка.\n"
        doc.write_bytes(text3.encode("utf-8"))
        h = k32.CreateFileW(str(doc), 0x80000000, 0, None, 3, 0, None)  # GENERIC_READ, no sharing at all
        time.sleep(1.0)
        locked = q(hwnd, "RELOADS") == reloads + 1
        k32.CloseHandle(h)
        ok &= check("edit 1c: a file locked while it changed is read once it is free",
                    locked and wait_for(lambda: q(hwnd, "RELOADS") == reloads + 2 and
                                        q(hwnd, "SRC_HASH", 0) == src_hash(text3), 6.0),
                    f'locked {locked}, reloads {q(hwnd, "RELOADS")}')
    finally:
        close_and_wait(proc, hwnd)
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
    tests = [("basics", lambda: test_basics(doc)), ("hscroll", lambda: test_hscroll(doc)), ("outline", test_outline),
             ("positions", lambda: test_positions(doc)), ("find", test_find), ("start_screen", test_start_screen),
             ("links", lambda: test_links(doc)), ("footnotes", test_footnotes), ("html", test_html),
             ("remote_images", test_remote_images), ("svg", test_svg), ("languages", test_languages),
             ("clipboard", lambda: test_clipboard(doc)), ("key_selection", test_key_selection), ("pdf", test_pdf),
             ("drag", lambda: test_drag(doc)), ("math", test_math), ("wide_diagram", test_wide_diagram),
             ("update", test_update), ("broken_html", test_broken_html), ("tasks", test_tasks),
             ("scroll_frames", test_scroll_frames), ("image_scaling", lambda: test_image_scaling(doc)),
             ("columns", lambda: test_columns(doc)), ("settings", lambda: test_settings(doc)),
             ("placement", lambda: test_placement(doc)), ("map_selfcheck", test_map_selfcheck),
             ("copy_md_exact", test_copy_md_exact), ("image_race", test_image_race),
             ("reload_during_update", test_reload_during_update), ("task_swap", test_task_swap),
             ("splice_hook", test_splice_hook), ("save_fault", test_save_fault), ("reading_touch", test_reading_touch)]
    only = [n for n in os.environ.get("FASTMD_ONLY", "").split(",") if n]  # e.g. FASTMD_ONLY=update,settings
    for name, t in tests:
        if not only or name in only:
            ok &= t()
    reset_profile()
    print("RESULT", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
