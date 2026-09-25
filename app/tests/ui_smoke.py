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
import threading
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
gdi.BitBlt.argtypes = [wt.HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wt.HDC, ctypes.c_int, ctypes.c_int,
                       wt.DWORD]
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


def shot(hwnd, name, paint=True):
    """the client area as an image, saved to tests/out. paint=True: PrintWindow, which makes the app render a whole
    frame of its own; paint=False: the pixels the window shows now, copied from its DC - the only way to see a frame
    the app produced by scrolling (a partial frame), since PrintWindow always gets a full one"""
    r = wt.RECT()
    u32.GetClientRect(hwnd, ctypes.byref(r))
    w, h = r.right, r.bottom
    sdc = u32.GetDC(None if paint else hwnd)
    mdc = gdi.CreateCompatibleDC(sdc)
    bmp = gdi.CreateCompatibleBitmap(sdc, w, h)
    old = gdi.SelectObject(mdc, bmp)
    if paint:
        u32.PrintWindow(hwnd, mdc, 3)  # PW_CLIENTONLY | PW_RENDERFULLCONTENT
    else:
        gdi.BitBlt(mdc, 0, 0, w, h, sdc, 0, 0, 0x00CC0020)  # SRCCOPY
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
    u32.ReleaseDC(None if paint else hwnd, sdc)
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


k32.GlobalAlloc.restype = wt.HGLOBAL
k32.GlobalAlloc.argtypes = [wt.UINT, ctypes.c_size_t]
u32.SetClipboardData.restype = wt.HANDLE
u32.SetClipboardData.argtypes = [wt.UINT, wt.HANDLE]


u32.CreateWindowExW.restype = wt.HWND
u32.CreateWindowExW.argtypes = [wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                ctypes.c_int, wt.HWND, wt.HMENU, wt.HINSTANCE, wt.LPVOID]
u32.OpenClipboard.argtypes = [wt.HWND]
u32.GetMessageW.argtypes = [ctypes.POINTER(wt.MSG), wt.HWND, wt.UINT, wt.UINT]
u32.DispatchMessageW.argtypes = [ctypes.POINTER(wt.MSG)]
CLIP_OWNER = []  # a message-only window of this process: the clipboard's owner while the test puts text on it


def clip_owner():
    """the window set_clipboard opens the clipboard with. Its own thread pumps its messages: the next owner's
    EmptyClipboard sends it WM_DESTROYCLIPBOARD and waits for the answer - a window of the test's thread, which sleeps,
    would hold the app up in its Cut until the test's next SendMessage"""
    if not CLIP_OWNER:
        import threading
        ready = threading.Event()

        def pump():
            CLIP_OWNER.append(u32.CreateWindowExW(0, "STATIC", "fastmd-uitest-clipboard", 0, 0, 0, 0, 0,
                                                  wt.HWND(-3), None, None, None))  # HWND_MESSAGE
            ready.set()
            msg = wt.MSG()
            while u32.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
                u32.DispatchMessageW(ctypes.byref(msg))

        threading.Thread(target=pump, daemon=True).start()
        ready.wait(2.0)
    return CLIP_OWNER[0] if CLIP_OWNER else None


def set_clipboard(text):
    """plain text on the clipboard (CF_UNICODETEXT), as another program would put it there (§13.6). The clipboard is
    opened with a window of this process: opened with none, EmptyClipboard leaves it ownerless and SetClipboardData
    fails"""
    data = text.encode("utf-16-le") + b"\0\0"
    owner = clip_owner()
    for _ in range(20):
        if u32.OpenClipboard(owner):
            break
        time.sleep(0.05)
    else:
        return False
    try:
        u32.EmptyClipboard()
        h = k32.GlobalAlloc(0x0002, len(data))  # GMEM_MOVEABLE
        p = k32.GlobalLock(h)
        ctypes.memmove(p, data, len(data))
        k32.GlobalUnlock(h)
        return bool(u32.SetClipboardData(13, h))
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


def type_text(hwnd, text, wait=0.3, gap=0.02):
    """posted WM_CHARs, one per UTF-16 code unit: a character outside the BMP arrives as its two halves (T18)"""
    units = text.encode("utf-16-le")
    for k in range(0, len(units), 2):
        post(hwnd, WM_CHAR, units[k] | (units[k + 1] << 8), 0, gap)
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

        # a double click enters edit mode (EDIT-MODE.md §2.1; the word selection moved to test_edit_selection), and Esc
        # leaves it without writing a byte (T24)
        before, saves = doc.read_bytes(), q(hwnd, "SAVES")
        time.sleep(0.6)  # not a third click of the drag above
        hx, hy = q(hwnd, "TEXT_LEFT") + 20, q(hwnd, "BLOCK_Y", 1) + 15  # the heading's text (block 0: front matter)
        post(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp(hx, hy), 0.01)
        post(hwnd, WM_LBUTTONUP, 0, lp(hx, hy), 0.01)
        post(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp(hx, hy), 0.01)
        post(hwnd, WM_LBUTTONUP, 0, lp(hx, hy), 0.3)
        editing = q(hwnd, "EDITING")
        post(hwnd, WM_KEYDOWN, 0x1B, 0, 0.4)  # Esc leaves edit mode
        ok &= check("double click enters edit mode, Esc leaves it; the file is untouched",
                    editing == 1 and q(hwnd, "EDITING") == 0 and doc.read_bytes() == before and q(hwnd, "SAVES") == saves,
                    f"editing {editing} → {q(hwnd, 'EDITING')}, saves {q(hwnd, 'SAVES')}")
        time.sleep(0.4)

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
            q(hwnd, "FRAME_STATS", 0)
            wheel(hwnd, 500, 400, notches, wait=1.2)
            y = q(hwnd, "SCROLLY")
            partial = shot(hwnd, f"24-scroll-{name}", paint=False)  # what the scrolled frames left on screen
            frames = q(hwnd, "FRAME_STATS", 0)
            full = shot(hwnd, f"24-scroll-{name}-full")               # PrintWindow: a full frame drawn now
            ok &= check(f"scrolling {name}: the partial frame matches a full redraw",
                        frames > 0 and q(hwnd, "SCROLLY") == y and same_pixels(partial, full),
                        f"scrollY={y}, partial frames {frames}")
        cmd(hwnd, "TOC", 0.6)
        wheel(hwnd, 700, 400, -4, wait=1.2)
        partial = shot(hwnd, "25-scroll-outline", paint=False)
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
        # (no Esc between the steps: with nothing selected it would close the window; a new selection replaces the old
        # one anyway. A double click enters edit mode, so the words are selected by dragging over them - after a click
        # that collapses select-all: a press inside a selection would drag it out of the window.)
        click(hwnd, q(hwnd, "TEXT_LEFT") + 6, q(hwnd, "BLOCK_Y", 0) + 20, 0.6)
        drag(hwnd, q(hwnd, "TEXT_LEFT") + 6, q(hwnd, "BLOCK_Y", 0) + 20, q(hwnd, "TEXT_LEFT") + 60, q(hwnd, "BLOCK_Y", 0) + 20)
        has = q(hwnd, "SEL_ANCHOR") != q(hwnd, "SEL_FOCUS")
        cmd(hwnd, "COPY_MD", 0.5)
        md = clipboard().replace("\r\n", "\n")
        ok &= check("edit 1a: a word on the first line copies that line whole", has and md == "# Заголовок первой строки",
                    repr(md[:40]))
        time.sleep(0.6)  # not a double click
        drag(hwnd, q(hwnd, "TEXT_LEFT") + 2, q(hwnd, "BLOCK_Y", 1) + 10, q(hwnd, "TEXT_LEFT") + 30, q(hwnd, "BLOCK_Y", 1) + 10)
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
            drag(hwnd, x + dx - 4, y + dy, x + dx + 16, y + dy)
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
    ENV["FASTMD_TEST_SLOW"] = "images:250"  # a formula typeset anew takes a while: what shows meanwhile is seen
    try:
        proc, hwnd = launch(doc, size="--size=900x700")
    finally:
        ENV.pop("FASTMD_TEST_SLOW", None)
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
        # another text size (the settings of another window: registry + broadcast, no reload), then a tick at the top:
        # the formulas keep their pictures until they are typeset for the new size - none goes blank (review of 1c)
        wheel(hwnd, 450, 350, 40, wait=0.4)
        settle(hwnd)
        set_reg("Theme", 1)  # the window runs --light: the registry says so too, or the broadcast switches the theme
        set_reg("FontSize", 20)
        post(hwnd, u32.RegisterWindowMessageW("FastMD.SettingsChanged"), 0, 0, 0.6)
        settle(hwnd)
        drawn, renders = q(hwnd, "MATH", 1), q(hwnd, "RENDERS")
        box = task_box(hwnd, 0)
        if box:
            click(hwnd, box[0], box[1], 0.02)
        kept = q(hwnd, "MATH", 1)
        fresh = wait_for(lambda: q(hwnd, "MATH", 3) == 0, 5.0)  # typeset for the new size (the old pictures meanwhile)
        ok &= check("edit 1c: a tick after a text-size change: the formulas keep their pictures, then get new ones",
                    q(hwnd, "FONT_SIZE") == 20 and box and drawn == 3 and kept == 3 and q(hwnd, "MATH", 1) == 3 and
                    fresh and q(hwnd, "TASK", 0) == 0 and q(hwnd, "RELOADS") == reloads,
                    f'drawn {drawn}, right after the tick {kept}, stale {q(hwnd, "MATH", 3)}, renders {renders} → '
                    f'{q(hwnd, "RENDERS")}')
    finally:
        close_and_wait(proc, hwnd)
        del_reg("FontSize")  # after the close: the window writes its settings back when it closes
        del_reg("Theme")
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
            # the recovery file is the only copy of the old bytes now: on the strip at once, and no save goes over it
            # (refused while the edits are still there; once the watcher has reloaded the torn file nothing is dirty)
            shown_now = wait_for(lambda: q(hwnd, "EDIT_STRIP") == 6, 2.0)
            s2 = copydata(hwnd, 2)
            time.sleep(0.6)
            ok &= check("edit 1c: ... the strip shows at once, and no save goes over the recovery file",
                        shown_now and s2 in (1 + 9, 1 + 0) and doc.read_bytes() == torn and len(recovery_files()) == 1 and
                        q(hwnd, "EDIT_STRIP") == 6, f'strip {q(hwnd, "EDIT_STRIP")}, hook {s2}, recovery {recovery_files()}')
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
    # A torn file that another program has written since: putting the old bytes back would lose that write, so the
    # strip offers only the copy (and Delete); the file is never touched (review of 1c: RecoveryRestore)
    ENV.update(FASTMD_TEST_HOOKS="1", FASTMD_TEST_FAIL_WRITE="partial:10,norollback")
    try:
        proc, hwnd = launch(doc)
        try:
            splice(hwnd, text, at, 0, " и расширен до длинного текста")
            copydata(hwnd, 2)
        finally:
            close_and_wait(proc, hwnd)
    finally:
        ENV.pop("FASTMD_TEST_HOOKS", None)
        ENV.pop("FASTMD_TEST_FAIL_WRITE", None)
    written = "Строка, дописанная другой программой\n".encode("utf-8") + doc.read_bytes()
    doc.write_bytes(written)
    proc, hwnd = launch(doc)
    try:
        shown = wait_for(lambda: q(hwnd, "EDIT_STRIP") == 6, 3.0)
        shot(hwnd, "85-recovery-strip-changed")
        restore, copy_btn = q(hwnd, "EDIT_TOOL", CMD["RECOVERY_RESTORE"]), q(hwnd, "EDIT_TOOL", CMD["RECOVERY_OPEN"])
        ok &= check("edit 1c: a torn file written since by another program: the strip offers the copy, not Restore",
                    shown and restore == -1 and copy_btn > 0 and len(recovery_files()) == 1,
                    f'strip {q(hwnd, "EDIT_STRIP")}, restore {restore}, copy {copy_btn}')
        cmd(hwnd, "RECOVERY_RESTORE", 0.5)  # even asked for directly: refused
        ok &= check("edit 1c: ... Restore asked for anyway changes nothing", doc.read_bytes() == written and
                    len(recovery_files()) == 1)
        cmd(hwnd, "RECOVERY_DELETE", 0.5)
        ok &= check("edit 1c: ... Delete removes the recovery file and the strip, the file stays as it is",
                    q(hwnd, "EDIT_STRIP") == 0 and not recovery_files() and doc.read_bytes() == written)
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


def test_locked_at_open():
    """a file that is there but cannot be read when it is opened (another program holds it with no sharing): the error
    page, no reload loop (review of 1c: the watcher's baseline), and the document once the file is free"""
    ok = True
    doc = OUT / "locked-open.md"
    text = "# Заперт\n\nЭтот файл был занят другой программой, когда его открывали.\n"
    doc.write_bytes(text.encode("utf-8"))
    k32.CreateFileW.restype = wt.HANDLE
    k32.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, ctypes.c_void_p, wt.DWORD, wt.DWORD, wt.HANDLE]
    k32.CloseHandle.argtypes = [wt.HANDLE]
    h = k32.CreateFileW(str(doc), 0x80000000, 0, None, 3, 0, None)  # GENERIC_READ, no sharing at all
    proc, hwnd = launch(doc)
    try:
        time.sleep(2.0)
        reloads = q(hwnd, "RELOADS")
        ok &= check("edit 1c: a locked file opens as the error page and is not reloaded over and over",
                    reloads <= 1 and q(hwnd, "SRC_HASH", 0) != src_hash(text), f"{reloads} reloads in 2 s")
        k32.CloseHandle(h)
        h = None
        ok &= check("edit 1c: ... and it is read once it is free",
                    wait_for(lambda: q(hwnd, "SRC_HASH", 0) == src_hash(text), 6.0),
                    f'reloads {q(hwnd, "RELOADS")}')
    finally:
        if h:
            k32.CloseHandle(h)
        close_and_wait(proc, hwnd)
    return ok


def test_modal_scope():
    """§10.10: inside a modal loop (a test hook runs one, as a menu or a dialog would) the reload waits and a splice is
    refused; the reload happens once the loop is over (review of 1c: editModal was never raised)"""
    ok = True
    doc = OUT / "modal.md"
    text = "# Модальное окно\n\nПока открыт диалог, модель не меняется.\n"
    doc.write_bytes(text.encode("utf-8"))
    ENV.update(FASTMD_TEST_HOOKS="1")
    try:
        proc, hwnd = launch(doc)
        try:
            settle(hwnd)
            reloads = q(hwnd, "RELOADS")
            t = threading.Thread(target=lambda: copydata(hwnd, 3, "2000"))  # the loop runs on the app's thread
            t.start()
            time.sleep(0.3)
            refused = copydata(hwnd, 1, "0\t0\tx") == 0
            text2 = text.replace("не меняется", "не меняется никогда")
            doc.write_bytes(text2.encode("utf-8"))
            time.sleep(1.0)
            waited = q(hwnd, "RELOADS") == reloads and q(hwnd, "SRC_HASH", 0) == src_hash(text)
            t.join()
            ok &= check("edit 1c: inside a modal loop a splice is refused and a changed file is not reloaded",
                        refused and waited, f"refused {refused}, reloads {q(hwnd, 'RELOADS')}")
            ok &= check("edit 1c: ... the reload comes once the loop is over",
                        wait_for(lambda: q(hwnd, "RELOADS") == reloads + 1 and q(hwnd, "SRC_HASH", 0) == src_hash(text2), 3.0))
        finally:
            close_and_wait(proc, hwnd)
    finally:
        ENV.pop("FASTMD_TEST_HOOKS", None)
    return ok


# ------------------------------------------------------------------------------------------------ edit mode, phase 2a
VK.update({"f5": 0x74, "space": 0x20, "prior": 0x21, "next": 0x22})
WM_KILLFOCUS, WM_SETFOCUS, WM_QUERYENDSESSION, WM_ENDSESSION = 0x0008, 0x0007, 0x0011, 0x0016
SS = {"SAVED": 0, "PENDING": 1, "SAVING": 2, "BUSY": 3, "DENIED": 4, "READONLY": 5, "MISSING": 6, "CONFLICT": 7,
      "UNENCODABLE": 8, "FAILED": 9, "UNKNOWN": 10, "OFF": 11}
STRIP = {"CONFLICT": 1, "ENCODING": 2, "LEAVE": 3, "READONLY": 4, "MISSING": 5, "RECOVERY": 6, "OTHER_WINDOW": 7}
EDIT_ENV = {"FASTMD_CARET_STEADY": "1", "FASTMD_EDIT_SELFCHECK": "1", "FASTMD_TEST_ANSWER": "leave:cancel"}


def launch_edit(doc, extra=None, steady=True, size="--size=1000x800"):
    """a window for an edit test (§13.6): a steady caret, the map checked after every swap, every question answered"""
    env = dict(EDIT_ENV)
    if not steady:
        env.pop("FASTMD_CARET_STEADY")
    env.update(extra or {})
    saved = {k: ENV.get(k) for k in env}
    ENV.update(env)
    try:
        return launch(*([doc] if doc else []), size=size)
    finally:
        for k, v in saved.items():
            if v is None:
                ENV.pop(k, None)
            else:
                ENV[k] = v


SELFCHECK = []  # windows whose map failed its self-check after a swap (FASTMD_EDIT_SELFCHECK), checked by main()
CLOSE_PROBLEMS = []  # edit windows that asked a question at close, hung, crashed or exited with an error (main())


def note_selfcheck(hwnd):
    """the map's self-check failures of a window about to be closed or killed some other way than close_edit"""
    fails = q(hwnd, "MAP_SELFCHECK", 1)
    if fails:
        SELFCHECK.append(f"{title_of(hwnd)}: {fails}")


def close_edit(proc, hwnd):
    """the failures of the map's self-check after the swaps are noted; then the window closes - without a question
    (every edit test leaves its window with nothing unsaved), within 5 s and with exit code 0: anything else is noted
    for main() (§13.6: an unexpected prompt fails the test instead of hanging it)"""
    if proc.poll() is not None:
        return
    note_selfcheck(hwnd)
    name, prompts = title_of(hwnd), q(hwnd, "LAST_PROMPT", 1)
    post(hwnd, WM_CLOSE, 0, 0, 0.1)
    try:
        proc.wait(5)
    except subprocess.TimeoutExpired:
        asked = q(hwnd, "LAST_PROMPT", 1) - prompts
        CLOSE_PROBLEMS.append(f"{name}: still open 5 s after WM_CLOSE" + (f" ({asked} question(s) asked)" if asked else ""))
        proc.kill()
        proc.wait(5)
        return
    if proc.returncode != 0:
        CLOSE_PROBLEMS.append(f"{name}: exit code {proc.returncode:#x}")


def shot_dark(hwnd, name, back="THEME_LIGHT"):
    """the same view in the dark theme too (§15.1 item 9: every new visual in light and dark)"""
    cmd(hwnd, "THEME_DARK", 0.6)
    img = shot(hwnd, name)
    cmd(hwnd, back, 0.5)
    return img


def enter_edit(hwnd, block, dx=2, dy=12, wait=True):
    """a double click on the text of a block; waits for the bar to be down"""
    x, y = q(hwnd, "TEXT_LEFT") + dx, q(hwnd, "BLOCK_Y", block) + dy
    dbl_click(hwnd, x, y, 0.2)
    if wait:
        wait_for(lambda: q(hwnd, "EDITING") == 1 and q(hwnd, "EDIT_BAR") == 100, 3.0, 0.05)
    return q(hwnd, "EDITING") == 1


def saved(hwnd, timeout=5.0):
    return wait_for(lambda: q(hwnd, "EDIT_DIRTY") == 0 and q(hwnd, "EDIT_SAVE_STATE") == SS["SAVED"], timeout, 0.05)


def clear_recovery():
    """what earlier tests kept aside (a .theirs copy, say) is not this test's business"""
    rec = DATA / "recovery"
    for f in rec.glob("*") if rec.exists() else []:
        f.unlink()


def toast_shown(img, hwnd):
    """the toast pill is drawn at the bottom middle of the document column"""
    r = wt.RECT()
    u32.GetClientRect(hwnd, ctypes.byref(r))
    return ink(img, (r.right // 2 - 60, r.bottom - 70, r.right // 2 + 60, r.bottom - 36)) > 30


EDIT_DOC = ("# Заголовок правки\n\nПервый абзац для набора текста.\n\nВторой абзац со **словом** и "
            "[ссылкой](other.md).\n\n- пункт один\n- пункт два\n\nПоследний абзац.\n")


def test_edit_enter_leave():
    """§2.1, §2.2: into edit mode by a double click (at the press point), F2, the pencil and "Edit here"; out by the ✕,
    Esc (after the find bar) and F2; three Esc presses keep the window; the refusals say why; a triple click is a
    reading action"""
    ok = True
    doc = OUT / "edit-enter.md"
    doc.write_bytes(EDIT_DOC.encode("utf-8"))
    original = doc.read_bytes()
    proc, hwnd = launch_edit(doc)
    try:
        para = EDIT_DOC.index("Первый")
        # the press point in the middle of a word: reading mode's hit test says which character that is (T8)
        x0, y1 = q(hwnd, "TEXT_LEFT"), q(hwnd, "BLOCK_Y", 1) + 12
        click(hwnd, x0 + 1, y1, 0.3)
        pos0 = q(hwnd, "SEL_FOCUS")
        click(hwnd, x0 + 75, y1, 0.3)
        pos1 = q(hwnd, "SEL_FOCUS")
        time.sleep(0.7)  # (not a double click with the last one)
        dbl_click(hwnd, x0 + 75, y1, 0.2)
        entered = wait_for(lambda: q(hwnd, "EDITING") == 1 and q(hwnd, "EDIT_BAR") == 100, 3.0, 0.05)
        ok &= check("edit 2a: a double click enters edit mode at the press point (mid-word), and the bar slides down",
                    entered and pos1 > pos0 and q(hwnd, "EDIT_CARET_SRC") == para + pos1 - pos0,
                    f'editing {q(hwnd, "EDITING")}, bar {q(hwnd, "EDIT_BAR")}, caret {q(hwnd, "EDIT_CARET_SRC")} '
                    f'(want {para} + {pos1 - pos0})')
        shot(hwnd, "90-edit-bar-light")
        c = q(hwnd, "EDIT_TOOL", CMD["EDIT_EXIT"])
        if c > 0:
            click(hwnd, c & 0xFFFF, c >> 16, 0.5)
        ok &= check("edit 2a: the ✕ leaves edit mode (Q_EDIT_TOOL finds it)", c > 0 and q(hwnd, "EDITING") == 0 and
                    wait_for(lambda: q(hwnd, "EDIT_BAR") == 0, 2.0), f"✕ at {c}")
        post(hwnd, WM_KEYDOWN, VK["f2"], 0, 0.4)
        f2 = q(hwnd, "EDITING") == 1 and q(hwnd, "EDIT_CARET_SRC") >= 0
        post(hwnd, WM_KEYDOWN, VK["f2"], 0, 0.4)
        ok &= check("edit 2a: F2 enters (at the caret left behind), F2 leaves", f2 and q(hwnd, "EDITING") == 0)
        time.sleep(1.1)  # past the Esc guard
        pen = q(hwnd, "EDIT_TOOL", CMD["EDIT_TOGGLE"])
        img = shot(hwnd, "91-edit-pencil")
        shot_dark(hwnd, "91-edit-pencil-dark")
        if pen > 0:
            click(hwnd, pen & 0xFFFF, pen >> 16, 0.5)
        ok &= check("edit 2a: the pencil (left of the gear) enters edit mode", pen > 0 and q(hwnd, "EDITING") == 1,
                    f"pencil at {pen}")
        # the Esc chain: the find bar first, then out
        keys(hwnd, [ord("F")], ctrl=True, wait=0.5)
        opened = q(hwnd, "FIND_OPEN") == 1
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.4)  # (the find box has the keyboard: it closes the bar)
        find_first = opened and q(hwnd, "FIND_OPEN") == 0 and q(hwnd, "EDITING") == 1
        u32.SetForegroundWindow(hwnd)
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.4)
        ok &= check("edit 2a: Esc closes the find bar first, then leaves edit mode", find_first and q(hwnd, "EDITING") == 0,
                    f"find opened {opened}, then editing {q(hwnd, 'EDITING')}")
        # "Edit here" of the menu a right click opened: at that point (T8)
        post(hwnd, 0x0204, 2, lp(x0 + 75, y1), 0.05)  # WM_RBUTTONDOWN
        post(hwnd, 0x0205, 0, lp(x0 + 75, y1), 0.5)   # WM_RBUTTONUP: the context menu
        u32.SendMessageW(hwnd, 0x001F, 0, 0)          # WM_CANCELMODE: the menu goes
        time.sleep(0.2)
        cmd(hwnd, "EDIT_HERE", 0.5)
        here = q(hwnd, "EDITING") == 1 and q(hwnd, "EDIT_CARET_SRC") == para + pos1 - pos0
        for _ in range(3):
            post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.15)
        time.sleep(0.5)
        ok &= check("edit 2a: \"Edit here\" enters at the right-click point; three Esc presses leave edit mode and keep the "
                    "window", here and proc.poll() is None and q(hwnd, "EDITING") == 0)
        time.sleep(1.1)
        # a triple click: its third press right after the double click that entered cancels the entry (UX-5)
        x, y = q(hwnd, "TEXT_LEFT") + 30, q(hwnd, "BLOCK_Y", q(hwnd, "BLOCK_COUNT") - 1) + 12  # «Последний абзац.»
        for k in range(3):
            post(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp(x, y), 0.01)
            post(hwnd, WM_LBUTTONUP, 0, lp(x, y), 0.01)
        time.sleep(0.5)
        a, f = q(hwnd, "SEL_ANCHOR"), q(hwnd, "SEL_FOCUS")
        ok &= check("edit 2a: a triple click selects the paragraph in reading mode, nothing saved",
                    q(hwnd, "EDITING") == 0 and q(hwnd, "SAVES") == 0 and abs(f - a) >= len("Последний абзац.") and
                    doc.read_bytes() == original, f"editing {q(hwnd, 'EDITING')}, selection {a}..{f}")
        # At the top of a document the bar slides the page down under the pointer: a triple click at a normal pace
        # (120 ms) must still select the paragraph that was clicked, and on a fresh profile the entry's hint is not
        # used up by an entry that was taken back (review of 2a: UX)
        x, y = q(hwnd, "TEXT_LEFT") + 30, q(hwnd, "BLOCK_Y", 2) + 12  # «Второй абзац…»
        for k in range(3):
            post(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp(x, y), 0.01)
            post(hwnd, WM_LBUTTONUP, 0, lp(x, y), 0.01)
        want = (q(hwnd, "SEL_ANCHOR"), q(hwnd, "SEL_FOCUS"))  # (the pace the app's own geometry cannot fool)
        time.sleep(1.1)
        del_reg("EditHintShown")
        for k in range(3):
            post(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp(x, y), 0.01)
            post(hwnd, WM_LBUTTONUP, 0, lp(x, y), 0.11)
        time.sleep(0.8)
        got = (q(hwnd, "SEL_ANCHOR"), q(hwnd, "SEL_FOCUS"))
        ok &= check("edit 2a review: a triple click at 120 ms selects the clicked paragraph; the hint stays for the next entry",
                    q(hwnd, "EDITING") == 0 and got == want and want[0] != want[1] and reg_value("EditHintShown") is None,
                    f"selection {got}, want {want}, hint shown {reg_value('EditHintShown')}")
        # dark theme: the bar in its dark look (its fill, not the page's, T7)
        cmd(hwnd, "THEME_DARK", 0.5)
        time.sleep(1.1)
        post(hwnd, WM_KEYDOWN, VK["f2"], 0, 0.6)
        img = shot(hwnd, "92-edit-bar-dark")
        bar = img.getpixel((500, 20))
        ok &= check("edit 2a: the bar in the dark theme", q(hwnd, "EDITING") == 1 and
                    all(abs(bar[i] - (0x1c, 0x21, 0x29)[i]) <= 6 for i in range(3)), f"{bar}, page {img.getpixel((500, 700))}")
        ok &= check("edit 2a review: the first entry ever shows its hint once the double click can no longer be a triple one",
                    wait_for(lambda: reg_value("EditHintShown") == 1, 2.0), f"{reg_value('EditHintShown')}")
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.4)
        cmd(hwnd, "THEME_LIGHT", 0.4)
        # a double click on a link: its first click opened it; the second is a click on what is there now (§2.1)
        time.sleep(1.1)
        img = shot(hwnd, "92b-edit-link")
        pt = find_color(img, ACCENT, (100, q(hwnd, "BLOCK_Y", 2), 900, q(hwnd, "BLOCK_Y", 2) + 30))
        if pt:
            dbl_click(hwnd, pt[0] + 3, pt[1] + 2, 1.0)
        ok &= check("edit 2a review: a double click on a link opens it and never enters edit mode",
                    pt and title_of(hwnd).startswith("other.md") and q(hwnd, "EDITING") == 0, f"link at {pt}, title "
                    f"{title_of(hwnd)!r}, editing {q(hwnd, 'EDITING')}")
        cmd(hwnd, "BACK", 1.0)
    finally:
        close_edit(proc, hwnd)
    # refusals, each with a toast (UX-22)
    proc, hwnd = launch_edit(None)
    try:
        post(hwnd, WM_KEYDOWN, VK["f2"], 0, 0.4)
        ok &= check("edit 2a: F2 on the start screen does nothing", proc.poll() is None and q(hwnd, "EDITING") == 0)
    finally:
        close_and_wait(proc, hwnd)
    # (the toast is told by the hash of its text, Q_LAST_PROMPT lp 2: each refusal says its own reason, T12)
    cases = [("edit-missing.md", None, "load-failed", "Файл не загрузился — править нечего",
              "The file did not load — nothing to edit"),
             ("edit-utf16be.md", b"\xfe\xff" + "# BE\n\nтекст\n".encode("utf-16-be"), "UTF-16 BE",
              "UTF-16 BE не поддерживается для правки", "UTF-16 BE files cannot be edited"),
             ("edit-nul.md", b"# NUL\n\nabc\x00def\n", "NUL bytes", "Файл похож на двоичный — правка отключена",
              "The file looks binary — editing is off"),
             ("edit-1251.md", "# Кодировка\n\nТекст в 1251.\n".encode("cp1251"), "1251 under FASTMD_ACP=65001",
              "Кодировку файла нельзя сохранить без потерь — правка отключена",
              "This file's encoding cannot be saved without loss — editing is off")]
    for name, data, what, ru, en in cases:
        p = OUT / name
        if data is None:
            if p.exists():
                p.unlink()
        else:
            p.write_bytes(data)
        proc, hwnd = launch_edit(p, {"FASTMD_ACP": "65001"})
        try:
            post(hwnd, WM_KEYDOWN, VK["f2"], 0, 0.15)
            img = shot(hwnd, f"93-edit-refused-{name[5:-3]}")
            said = q(hwnd, "LAST_PROMPT", 2) & 0xFFFFFFFF
            ok &= check(f"edit 2a: {what}: edit mode is refused, and a toast says why",
                        q(hwnd, "EDITING") == 0 and toast_shown(img, hwnd) and (data is None or p.read_bytes() == data) and
                        said == src_hash(en if q(hwnd, "LANG") == 1 else ru), f"toast hash {said:#x}")
        finally:
            close_and_wait(proc, hwnd)
    return ok


def test_edit_fullpending():
    """§2.1: a double click while a big document's full parse runs shows the toast, and edit mode follows by itself once
    the full model is there (FASTMD_TEST_SLOW=fullparse:1500)"""
    ok = True
    doc = OUT / "edit-large.md"
    shutil.copy(REPO / "bench" / "corpus" / "large.md", doc)
    proc, hwnd = launch_edit(doc, {"FASTMD_TEST_SLOW": "fullparse:1500"})
    try:
        first = q(hwnd, "BLOCK_COUNT")
        enter_edit(hwnd, 2, dx=4, wait=False)
        img = shot(hwnd, "94-edit-fullpending")
        early = q(hwnd, "EDITING") == 0 and toast_shown(img, hwnd)
        t0 = time.perf_counter()
        came = wait_for(lambda: q(hwnd, "EDITING") == 1, 4.0, 0.05)
        dt = time.perf_counter() - t0
        ok &= check("edit 2a: during the full parse the entry waits (toast), then happens by itself",
                    early and came and q(hwnd, "BLOCK_COUNT") > first, f"early {early}, came {came} after {dt:.2f} s, "
                    f"blocks {first} → {q(hwnd, 'BLOCK_COUNT')}")
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.8)
    finally:
        close_edit(proc, hwnd)
    return ok


def edit_typing(kind):
    """§14.3 typing: the source is right at once; after the save only the edited bytes differ, in the file's own
    encoding, BOM and line ends; the title has its star only while dirty; one save for the burst; no reload"""
    ok = True
    text = EDIT_DOC
    env = {}
    if kind == "utf8":
        enc = lambda t: t.encode("utf-8")
        want_enc, want_eol = 65001, 0
    elif kind == "bomcrlf":
        text = text.replace("\n", "\r\n")
        enc = lambda t: b"\xef\xbb\xbf" + t.encode("utf-8")
        want_enc, want_eol = 65001 | 3 << 24, 1
    elif kind == "utf16":
        enc = lambda t: b"\xff\xfe" + t.encode("utf-16-le")
        want_enc, want_eol = 1200 | 2 << 24, 0
    else:  # ANSI: characters that round-trip in 1251 and bytes that are no UTF-8 (T18)
        enc = lambda t: t.encode("cp1251")
        env["FASTMD_ACP"] = "1251"
        want_enc, want_eol = 1251, 0
    doc = OUT / f"edit-typing-{kind}.md"
    doc.write_bytes(enc(text))
    proc, hwnd = launch_edit(doc, env)
    try:
        reloads = q(hwnd, "RELOADS")
        at = text.index("Первый") + len("Первый")
        entered = enter_edit(hwnd, 1, dx=1)
        post(hwnd, WM_KEYDOWN, VK["home"], 0, 0.1)
        for _ in range(len("Первый")):
            post(hwnd, WM_KEYDOWN, VK["right"], 0, 0.03)
        typed = " «Проба» ёж"
        type_text(hwnd, typed, 0.05)
        want = text[:at] + typed + text[at:]
        right_now = q(hwnd, "SRC_HASH", 0) == src_hash(want) and q(hwnd, "EDIT_DIRTY") == 1
        star = title_of(hwnd).startswith(doc.name + "*")
        done = saved(hwnd)
        ok &= check(f"edit 2a ({kind}): the typing is in the source at once, the title shows a star",
                    entered and right_now and star, f"caret {q(hwnd, 'EDIT_CARET_SRC')}, title {title_of(hwnd)!r}")
        ok &= check(f"edit 2a ({kind}): saved byte for byte in the file's encoding, one save, no reload, no star",
                    done and doc.read_bytes() == enc(want) and q(hwnd, "SAVES") == 1 and q(hwnd, "RELOADS") == reloads and
                    not title_of(hwnd).startswith(doc.name + "*"),
                    f"saves {q(hwnd, 'SAVES')}, reloads {q(hwnd, 'RELOADS')}, title {title_of(hwnd)!r}")
        ok &= check(f"edit 2a ({kind}): Q_EDIT_ENC / Q_EDIT_EOL", q(hwnd, "EDIT_ENC") == want_enc and
                    q(hwnd, "EDIT_EOL") == want_eol, f"{q(hwnd, 'EDIT_ENC'):#x} {q(hwnd, 'EDIT_EOL')}")
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.5)
    finally:
        close_edit(proc, hwnd)
    return ok


def test_edit_keys_once():
    """§12.5: a posted key acts once (its WM_CHAR is dropped); Ctrl+Backspace writes no DEL character; Tab in a
    paragraph does nothing; Space types a blank and never pages"""
    ok = True
    doc = OUT / "edit-keys.md"
    doc.write_bytes(EDIT_DOC.encode("utf-8"))
    proc, hwnd = launch_edit(doc, {"FASTMD_AUTOSAVE_MS": "60000"})
    try:
        enter_edit(hwnd, 1, dx=1)
        base = EDIT_DOC
        at = base.index("Первый")
        type_text(hwnd, "abc", 0.2)
        post(hwnd, WM_KEYDOWN, VK["back"], 0, 0.2)
        s1 = base[:at] + "ab" + base[at:]
        ok &= check("edit 2a: a posted Backspace deletes one character", q(hwnd, "SRC_HASH", 0) == src_hash(s1))
        post(hwnd, WM_KEYDOWN, VK["left"], 0, 0.1)
        post(hwnd, WM_KEYDOWN, VK["delete"], 0, 0.2)
        s2 = base[:at] + "a" + base[at:]
        ok &= check("edit 2a: a posted Delete deletes one character", q(hwnd, "SRC_HASH", 0) == src_hash(s2))
        post(hwnd, WM_KEYDOWN, VK["end"], 0, 0.1)
        post(hwnd, WM_KEYDOWN, VK["left"], 0, 0.1)  # before the period, a word of its own by Windows' rules
        keys(hwnd, [VK["back"]], ctrl=True, wait=0.3)
        s3 = s2.replace("набора текста.", "набора .", 1)
        ok &= check("edit 2a: Ctrl+Backspace deletes the word before, and writes no 0x7F",
                    q(hwnd, "SRC_HASH", 0) == src_hash(s3), f'len {q(hwnd, "SRC_LEN", 0)} vs {u16(s3)}')
        post(hwnd, WM_KEYDOWN, VK["tab"], 0, 0.2)
        ok &= check("edit 2a: Tab in a paragraph does nothing", q(hwnd, "SRC_HASH", 0) == src_hash(s3))
        y = q(hwnd, "SCROLLY")
        post(hwnd, WM_KEYDOWN, VK["space"], 0, 0.3)
        at2 = s3.index("набора .") + len("набора ")
        s4 = s3[:at2] + " " + s3[at2:]
        ok &= check("edit 2a: Space types a blank and does not page", q(hwnd, "SRC_HASH", 0) == src_hash(s4) and
                    q(hwnd, "SCROLLY") == y, f'scroll {y} → {q(hwnd, "SCROLLY")}')
        cmd(hwnd, "SAVE", 0.4)
        ok &= check("edit 2a: Ctrl+S (CMD_SAVE) saves at once", doc.read_bytes() == s4.encode("utf-8"))
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.4)
    finally:
        close_edit(proc, hwnd)
    return ok


def test_edit_surrogates():
    """§2.7: a high surrogate waits for its low half; alone it never reaches the file"""
    ok = True
    doc = OUT / "edit-surrogates.md"
    doc.write_bytes(EDIT_DOC.encode("utf-8"))
    original = doc.read_bytes()
    proc, hwnd = launch_edit(doc, {"FASTMD_AUTOSAVE_MS": "300"})
    try:
        enter_edit(hwnd, 1, dx=1)
        post(hwnd, WM_CHAR, 0xD83D, 0, 0.9)
        ok &= check("edit 2a: a lone high surrogate: nothing typed, the file unchanged after the autosave delay",
                    doc.read_bytes() == original and q(hwnd, "EDIT_DIRTY") == 0)
        post(hwnd, WM_CHAR, 0xDE00, 0, 0.1)
        at = EDIT_DOC.index("Первый")
        want = EDIT_DOC[:at] + "\U0001F600" + EDIT_DOC[at:]
        ok &= check("edit 2a: the low half follows: the emoji is in the file", saved(hwnd) and doc.read_bytes() == want.encode("utf-8"))
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.4)
    finally:
        close_edit(proc, hwnd)
    return ok


def test_edit_undo():
    """§11: «Новый мир» is two steps, one undo leaves «Новый »; the history survives saves and leaving; a reload clears
    it; an external change adopted while clean is undone first, then the typing"""
    ok = True
    doc = OUT / "edit-undo.md"
    doc.write_bytes(EDIT_DOC.encode("utf-8"))
    proc, hwnd = launch_edit(doc)
    try:
        enter_edit(hwnd, 1, dx=1)
        at = EDIT_DOC.index("Первый")
        type_text(hwnd, "Новый мир", 0.2)
        depth = q(hwnd, "UNDO_DEPTH", 0)
        cmd(hwnd, "UNDO", 0.3)
        one = EDIT_DOC[:at] + "Новый " + EDIT_DOC[at:]
        ok &= check("edit 2a: «Новый мир» makes two steps; one undo leaves «Новый »",
                    depth == 2 and q(hwnd, "SRC_HASH", 0) == src_hash(one), f"depth {depth}")
        cmd(hwnd, "REDO", 0.3)
        two = EDIT_DOC[:at] + "Новый мир" + EDIT_DOC[at:]
        saved(hwnd)
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.5)
        time.sleep(1.1)
        post(hwnd, WM_KEYDOWN, VK["f2"], 0, 0.5)
        ok &= check("edit 2a: redo; the history survives the save and leaving and re-entering",
                    q(hwnd, "UNDO_DEPTH", 0) == 2 and q(hwnd, "SRC_HASH", 0) == src_hash(two) and
                    doc.read_bytes() == two.encode("utf-8"), f'depth {q(hwnd, "UNDO_DEPTH", 0)}')
        # an external change while nothing is unsaved: adopted as a step of its own
        reloads = q(hwnd, "RELOADS")
        three = two.replace("Последний абзац.", "Последний абзац, изменённый снаружи.")
        doc.write_bytes(three.encode("utf-8"))
        adopted = wait_for(lambda: q(hwnd, "SRC_HASH", 0) == src_hash(three), 3.0)
        ok &= check("edit 2a: a clean document adopts an external change as one undo step, no reload",
                    adopted and q(hwnd, "RELOADS") == reloads and q(hwnd, "UNDO_DEPTH", 0) == 3 and q(hwnd, "EDITING") == 1,
                    f'depth {q(hwnd, "UNDO_DEPTH", 0)}, reloads {q(hwnd, "RELOADS")}')
        cmd(hwnd, "UNDO", 0.3)
        first = q(hwnd, "SRC_HASH", 0) == src_hash(two)
        cmd(hwnd, "UNDO", 0.3)
        ok &= check("edit 2a: undo takes back the adopted change first, then the typing",
                    first and q(hwnd, "SRC_HASH", 0) == src_hash(one))
        cmd(hwnd, "RELOAD", 1.0)
        ok &= check("edit 2a: a reload (F5 saves first) clears the history",
                    q(hwnd, "UNDO_DEPTH", 0) == 0 and q(hwnd, "UNDO_DEPTH", 1) == 0 and q(hwnd, "EDITING") == 0 and
                    doc.read_bytes() == one.encode("utf-8"), f'depth {q(hwnd, "UNDO_DEPTH", 0)}/{q(hwnd, "UNDO_DEPTH", 1)}')
    finally:
        close_edit(proc, hwnd)
    return ok


GUARDS_DOC = ("# Охрана правок\n\nАбзац для набора и [ссылка на другой](other.md).\n\n- [ ] задача в списке\n\n"
              "Формула: $x^2$ в строке.\n")


def test_edit_guards():
    """§10.8, §8.10, T20: F5 saves, leaves and reloads; Ctrl+click on a link saves and navigates; Backspace never
    navigates; a theme switch re-parses instead of reloading; a task click is an undoable splice; a reading-mode tick
    and then editing raise no conflict; Ctrl+E saves, leaves and starts the editor"""
    ok = True
    doc = OUT / "edit-guards.md"
    doc.write_bytes(GUARDS_DOC.encode("utf-8"))
    shutil.copy(HERE / "other.md", OUT / "other.md")
    proc, hwnd = launch_edit(doc, {"FASTMD_AUTOSAVE_MS": "60000"})
    try:
        wait_for(lambda: q(hwnd, "MATH", 1) == 1, 5.0)
        enter_edit(hwnd, 1, dx=1)
        type_text(hwnd, "Ф5 ", 0.2)
        reloads = q(hwnd, "RELOADS")
        post(hwnd, WM_KEYDOWN, VK["f5"], 0, 1.0)
        s = GUARDS_DOC.replace("Абзац для", "Ф5 Абзац для")
        ok &= check("edit 2a: F5 saves, leaves edit mode and reloads (the disk holds the edits)",
                    q(hwnd, "RELOADS") == reloads + 1 and q(hwnd, "EDITING") == 0 and doc.read_bytes() == s.encode("utf-8"))
        # a task box: an undoable splice, saved by autosave's rule (here Ctrl+S)
        enter_edit(hwnd, 1, dx=1)
        box = task_box(hwnd, 0)
        if box:
            click(hwnd, box[0], box[1], 0.4)
        ticked = q(hwnd, "TASK", 0) == 1 and q(hwnd, "UNDO_DEPTH", 0) >= 1
        cmd(hwnd, "UNDO", 0.3)
        ok &= check("edit 2a: a task click in edit mode is an undoable splice", box and ticked and q(hwnd, "TASK", 0) == 0,
                    f"box {box}")
        # the theme: formulas re-render, nothing is read from disk
        reloads = q(hwnd, "RELOADS")
        cmd(hwnd, "THEME_DARK", 0.8)
        ok &= check("edit 2a: a theme switch keeps editing, no reload", q(hwnd, "EDITING") == 1 and q(hwnd, "RELOADS") == reloads)
        cmd(hwnd, "THEME_LIGHT", 0.5)
        # Ctrl+click on a link to another document: saved, then the other document
        type_text(hwnd, "клик ", 0.2)
        s2 = s.replace("Ф5 Абзац", "клик Ф5 Абзац")
        img = shot(hwnd, "95-edit-guards-link")
        pt = find_color(img, ACCENT, (100, 100, 900, 200))
        if pt:
            post(hwnd, WM_LBUTTONDOWN, MK_LBUTTON | 0x0008, lp(pt[0] + 3, pt[1] + 2), 0.02)  # MK_CONTROL
            post(hwnd, WM_LBUTTONUP, 0x0008, lp(pt[0] + 3, pt[1] + 2), 1.0)
        ok &= check("edit 2a: Ctrl+click on a link to another .md saves the edits and navigates",
                    pt and title_of(hwnd).startswith("other.md") and doc.read_bytes() == s2.encode("utf-8"),
                    f"link at {pt}, title {title_of(hwnd)!r}")
        # Backspace in edit mode never goes back in history (T20); at the heading's start it makes the heading a
        # paragraph (§7.7, Phase 2b), which Undo takes back
        enter_edit(hwnd, 0, dx=1)
        post(hwnd, WM_KEYDOWN, VK["home"], 0, 0.1)
        post(hwnd, WM_KEYDOWN, VK["back"], 0, 0.5)
        ok &= check("edit 2a: a posted Backspace in edit mode does not navigate back",
                    title_of(hwnd).startswith("other.md") and q(hwnd, "EDITING") == 1)
        cmd(hwnd, "UNDO", 0.3)
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.3)
        cmd(hwnd, "BACK", 1.0)
        # a reading-mode tick, then F2 and typing: no conflict
        box = task_box(hwnd, 0)
        if box:
            click(hwnd, box[0], box[1], 0.6)
        post(hwnd, WM_KEYDOWN, VK["f2"], 0, 0.5)
        type_text(hwnd, "после отметки ", 0.2)
        cmd(hwnd, "SAVE", 0.5)
        ok &= check("edit 2a: a reading-mode tick, then F2 and typing: saved, no conflict",
                    title_of(hwnd).startswith("edit-guards.md") and q(hwnd, "EDIT_CONFLICT") == 0 and
                    "- [x] задача" in doc.read_text("utf-8") and "после отметки" in doc.read_text("utf-8"),
                    f'conflict {q(hwnd, "EDIT_CONFLICT")}, title {title_of(hwnd)!r}')
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.4)
    finally:
        close_edit(proc, hwnd)
    # Ctrl+E with an "editor" that proves it ran: pythonw executes the document, which is Python as well as Markdown
    doc = OUT / "edit-ctrl-e.md"
    marker = OUT / "edit-ctrl-e.md.launched"
    if marker.exists():
        marker.unlink()
    doc.write_bytes('# Заголовок для Ctrl+E\n\n"""\n\nАбзац, в который печатают.\n\n"""\n\nopen(__file__ + ".launched", "w", '
                    'encoding="utf-8").write(open(__file__, encoding="utf-8").read())\n'.encode("utf-8"))
    pythonw = pathlib.Path(sys.executable).with_name("pythonw.exe")
    set_reg("Editor", str(pythonw))
    try:
        proc, hwnd = launch_edit(doc, {"FASTMD_AUTOSAVE_MS": "60000"})
        try:
            enter_edit(hwnd, 2, dx=1)
            type_text(hwnd, "Ctrl+E ", 0.2)
            keys(hwnd, [ord("E")], ctrl=True, wait=0.3)
            launched = wait_for(lambda: marker.exists(), 5.0)
            ok &= check("edit 2a: Ctrl+E saves, leaves edit mode and starts the editor on the saved file",
                        launched and q(hwnd, "EDITING") == 0 and "Ctrl+E Абзац" in marker.read_text("utf-8"),
                        f"editor ran {launched}")
        finally:
            close_edit(proc, hwnd)
    finally:
        del_reg("Editor")
    return ok


def test_edit_conflict():
    """§10.7 (AUTOSAVE_MS=60000, T8): an external write under unsaved edits is a conflict - saving leaves the file
    alone; "overwrite" writes ours and keeps theirs aside; "load" takes the disk's text and undo brings ours back; a
    same-size change behind the stamp is caught by the save itself; a clean document adopts without a reload"""
    ok = True
    doc = OUT / "edit-conflict.md"
    doc.write_bytes(EDIT_DOC.encode("utf-8"))
    for f in (DATA / "recovery").glob("*.theirs") if (DATA / "recovery").exists() else []:
        f.unlink()
    # (the test hooks keep a posted hover: the strip's cut text is looked at in its tooltip)
    proc, hwnd = launch_edit(doc, {"FASTMD_AUTOSAVE_MS": "60000", "FASTMD_TEST_HOOKS": "1"})
    try:
        enter_edit(hwnd, 1, dx=1)
        type_text(hwnd, "наше ", 0.2)
        ours = EDIT_DOC.replace("Первый", "наше Первый")
        theirs = EDIT_DOC.replace("Последний абзац.", "Последний абзац от другой программы.")
        doc.write_bytes(theirs.encode("utf-8"))
        conflict = wait_for(lambda: q(hwnd, "EDIT_CONFLICT") == 1, 3.0)
        img = shot(hwnd, "96-edit-conflict-strip")
        shot_dark(hwnd, "96-edit-conflict-strip-dark")
        # at 1000 px the strip's text is cut before the sizes: the tooltip pill has it whole (review of 2a: UX)
        r = wt.RECT()
        u32.GetClientRect(hwnd, ctypes.byref(r))
        pill = (20, r.bottom - 50, 280, r.bottom - 25)  # (the left end of the pill, clear of a toast in the middle)
        before = shot(hwnd, "96-edit-conflict-tip-before")
        post(hwnd, WM_MOUSEMOVE, 0, lp(150, 44 + 18), 0.4)
        tip = shot(hwnd, "96-edit-conflict-tip")
        post(hwnd, WM_MOUSEMOVE, 0, lp(450, 500), 0.3)
        diff = ImageChops.difference(before.crop(pill), tip.crop(pill)).getbbox()
        ok &= check("edit 2a review: the conflict strip's cut text is whole in the tooltip under the pointer", diff is not None,
                    f"pill box {pill}")
        cmd(hwnd, "SAVE", 0.4)
        ok &= check("edit 2a: an external write under our edits: the conflict strip, and a save leaves the file alone",
                    conflict and q(hwnd, "EDIT_STRIP") == STRIP["CONFLICT"] and doc.read_bytes() == theirs.encode("utf-8") and
                    q(hwnd, "EDIT_SAVE_STATE") == SS["CONFLICT"], f'strip {q(hwnd, "EDIT_STRIP")}')
        cmd(hwnd, "CONFLICT_KEEP", 0.5)
        kept = list((DATA / "recovery").glob("*.theirs")) if (DATA / "recovery").exists() else []
        ok &= check("edit 2a: \"overwrite\" writes ours and keeps theirs aside",
                    doc.read_bytes() == ours.encode("utf-8") and len(kept) == 1 and kept[0].read_bytes() == theirs.encode("utf-8")
                    and q(hwnd, "EDIT_CONFLICT") == 0 and q(hwnd, "EDIT_STRIP") == 0, f"kept {kept}")
        type_text(hwnd, "ещё ", 0.2)
        ours2 = ours.replace("наше Первый", "наше ещё Первый")
        doc.write_bytes(theirs.encode("utf-8"))
        wait_for(lambda: q(hwnd, "EDIT_CONFLICT") == 1, 3.0)
        cmd(hwnd, "CONFLICT_LOAD", 0.5)
        loaded = q(hwnd, "SRC_HASH", 0) == src_hash(theirs) and q(hwnd, "EDIT_CONFLICT") == 0 and q(hwnd, "EDIT_DIRTY") == 0
        cmd(hwnd, "UNDO", 0.4)
        ok &= check("edit 2a: \"load\" takes the disk's version; undo brings our edits back",
                    loaded and q(hwnd, "SRC_HASH", 0) == src_hash(ours2), f"loaded {loaded}")
        # a change of the same size with the old stamp: the watcher cannot see it, the save does ("а" → "ы": both two
        # bytes in UTF-8, so the watcher's size compare does not give it away first - T1)
        st = doc.stat()
        same = theirs.replace("другой программы", "другой прогрыммы")
        doc.write_bytes(same.encode("utf-8"))
        os.utime(doc, ns=(st.st_atime_ns, st.st_mtime_ns))
        time.sleep(0.5)
        unseen = q(hwnd, "EDIT_CONFLICT") == 0 and len(same.encode("utf-8")) == len(theirs.encode("utf-8"))
        cmd(hwnd, "SAVE", 0.4)
        ok &= check("edit 2a: a same-size change behind the old stamp: the save reports the conflict and writes nothing",
                    unseen and q(hwnd, "EDIT_SAVE_STATE") == SS["CONFLICT"] and doc.read_bytes() == same.encode("utf-8"),
                    f'unseen by the watcher {unseen}, state {q(hwnd, "EDIT_SAVE_STATE")}')
        cmd(hwnd, "CONFLICT_LOAD", 0.5)
        reloads, depth = q(hwnd, "RELOADS"), q(hwnd, "UNDO_DEPTH", 0)
        clean = same.replace("Первый", "Самый первый")
        doc.write_bytes(clean.encode("utf-8"))
        ok &= check("edit 2a: a clean document adopts an external change: one undo step, no reload",
                    wait_for(lambda: q(hwnd, "SRC_HASH", 0) == src_hash(clean), 3.0) and q(hwnd, "RELOADS") == reloads and
                    q(hwnd, "UNDO_DEPTH", 0) == depth + 1, f'depth {depth} → {q(hwnd, "UNDO_DEPTH", 0)}')
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.4)
    finally:
        close_edit(proc, hwnd)
    left = list((DATA / "recovery").glob("*.theirs")) if (DATA / "recovery").exists() else []
    ok &= check("edit 2a review: the other program's version kept by \"overwrite\" goes when the document is closed (§10.5)",
                not left, f"{left}")
    return ok


k32.CreateFileW.restype = wt.HANDLE
k32.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, ctypes.c_void_p, wt.DWORD, wt.DWORD, wt.HANDLE]
k32.CloseHandle.argtypes = [wt.HANDLE]


def test_edit_busy_retry():
    """§10.4: a file another program holds without sharing: BUSY, one toast, and saved once it is free"""
    ok = True
    doc = OUT / "edit-busy.md"
    doc.write_bytes(EDIT_DOC.encode("utf-8"))
    proc, hwnd = launch_edit(doc, {"FASTMD_AUTOSAVE_MS": "300"})
    h = None
    try:
        enter_edit(hwnd, 1, dx=1)
        type_text(hwnd, "занято ", 0.02)
        h = k32.CreateFileW(str(doc), 0x80000000, 0, None, 3, 0, None)  # GENERIC_READ, no sharing
        busy = wait_for(lambda: q(hwnd, "EDIT_SAVE_STATE") == SS["BUSY"], 3.0)
        time.sleep(1.2)
        k32.CloseHandle(h)
        h = None
        want = EDIT_DOC.replace("Первый", "занято Первый")
        done = saved(hwnd, 10.0)
        ok &= check("edit 2a: a locked file: BUSY, one toast, then saved once it is free",
                    busy and done and doc.read_bytes() == want.encode("utf-8") and q(hwnd, "EDIT_SAVE_STATE", 1) == 1,
                    f'busy {busy}, saved {done}, toasts {q(hwnd, "EDIT_SAVE_STATE", 1)}')
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.4)
    finally:
        if h:
            k32.CloseHandle(h)
        close_edit(proc, hwnd)
    return ok


def test_edit_readonly_missing():
    """§10.4, §10.7: a read-only file enters with the READONLY strip and is never written; Save As moves the document;
    a file renamed away is MISSING, renamed back it is saved; closing with the answer "no" discards (exit code 0)"""
    ok = True
    import stat
    # closing with unsaved edits asks (T3): with the answer "cancel" the window stays, still dirty
    ask = OUT / "edit-close-ask.md"
    ask_away = OUT / "edit-close-ask-away.md"
    for p in (ask, ask_away):
        if p.exists():
            p.unlink()
    ask.write_bytes(EDIT_DOC.encode("utf-8"))
    proc, hwnd = launch_edit(ask)
    try:
        enter_edit(hwnd, 1, dx=1)
        type_text(hwnd, "спросить ", 0.2)
        ask.rename(ask_away)
        wait_for(lambda: q(hwnd, "EDIT_STRIP") == STRIP["MISSING"], 4.0)
        prompts = q(hwnd, "LAST_PROMPT", 1)
        post(hwnd, WM_CLOSE, 0, 0, 1.0)
        ok &= check("edit 2a review: WM_CLOSE with unsaved edits asks (LEAVE); \"cancel\" keeps the window and the edits",
                    proc.poll() is None and q(hwnd, "LAST_PROMPT") == 1 and q(hwnd, "LAST_PROMPT", 1) == prompts + 1 and
                    q(hwnd, "EDIT_DIRTY") == 1, f'prompt {q(hwnd, "LAST_PROMPT")}, count {prompts} → {q(hwnd, "LAST_PROMPT", 1)}')
        ask_away.rename(ask)
        wait_for(lambda: q(hwnd, "EDIT_DIRTY") == 0, 6.0)
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.4)
    finally:
        close_edit(proc, hwnd)
    doc = OUT / "edit-readonly.md"
    other = OUT / "edit-readonly-saved-as.md"
    moved = OUT / "edit-readonly-away.md"
    for p in (doc, other, moved):
        if p.exists():
            os.chmod(p, stat.S_IWRITE)
            p.unlink()
    doc.write_bytes(EDIT_DOC.encode("utf-8"))
    os.chmod(doc, stat.S_IREAD)
    proc, hwnd = launch_edit(doc, {"FASTMD_SAVE_AS": str(other), "FASTMD_TEST_ANSWER": "leave:no"})
    try:
        enter_edit(hwnd, 1, dx=1)
        img = shot(hwnd, "97-edit-readonly-strip")
        strip = q(hwnd, "EDIT_STRIP")
        type_text(hwnd, "только чтение ", 0.8)
        shot(hwnd, "97-edit-readonly-typed")  # (the status slot says why, whole: review of 2a)
        shot_dark(hwnd, "97-edit-readonly-typed-dark")
        ok &= check("edit 2a: a read-only file: the READONLY strip at entry, the edits are not written",
                    strip == STRIP["READONLY"] and q(hwnd, "EDIT_SAVE_STATE") == SS["READONLY"] and
                    doc.read_bytes() == EDIT_DOC.encode("utf-8"), f'strip {strip}, state {q(hwnd, "EDIT_SAVE_STATE")}')
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.5)  # leaving cannot save: the LEAVE strip says so
        leave = q(hwnd, "EDIT_STRIP") == STRIP["LEAVE"] and q(hwnd, "EDITING") == 1
        shot(hwnd, "97-edit-leave-strip")
        shot_dark(hwnd, "97-edit-leave-strip-dark")
        ok &= check("edit 2a review: Esc with edits that cannot be saved: the LEAVE strip, still editing", leave,
                    f'strip {q(hwnd, "EDIT_STRIP")}')
        cmd(hwnd, "SAVE_AS", 0.8)
        want = EDIT_DOC.replace("Первый", "только чтение Первый")
        ok &= check("edit 2a: Save As writes the edits to the new file and moves the document there",
                    other.exists() and other.read_bytes() == want.encode("utf-8") and
                    title_of(hwnd).startswith(other.name) and q(hwnd, "EDIT_STRIP") == 0 and q(hwnd, "EDITING") == 1,
                    f'title {title_of(hwnd)!r}, strip {q(hwnd, "EDIT_STRIP")}')
        other.rename(moved)
        type_text(hwnd, "пропал ", 0.2)
        missing = wait_for(lambda: q(hwnd, "EDIT_STRIP") == STRIP["MISSING"], 4.0)
        img = shot(hwnd, "98-edit-missing-strip")
        shot_dark(hwnd, "98-edit-missing-strip-dark")
        moved.rename(other)
        want2 = want.replace("только чтение Первый", "только чтение пропал Первый")
        back = wait_for(lambda: other.read_bytes() == want2.encode("utf-8"), 6.0)
        ok &= check("edit 2a: renamed away: MISSING; renamed back: saved", missing and back and q(hwnd, "EDIT_STRIP") == 0,
                    f'missing {missing}, saved {back}, strip {q(hwnd, "EDIT_STRIP")}')
        other.rename(moved)
        type_text(hwnd, "лишнее ", 0.2)
        wait_for(lambda: q(hwnd, "EDIT_STRIP") == STRIP["MISSING"], 4.0)
        note_selfcheck(hwnd)
        post(hwnd, WM_CLOSE, 0, 0, 0.1)
        try:
            proc.wait(5)
        except subprocess.TimeoutExpired:
            pass
        moved.rename(other)
        ok &= check("edit 2a: closing with unsaved edits and the answer \"no\": exit code 0, the file as it was",
                    proc.returncode == 0 and other.read_bytes() == want2.encode("utf-8"), f"rc {proc.returncode}")
    finally:
        if proc.poll() is None:
            close_and_wait(proc, hwnd)
        for p in (doc, other, moved):
            if p.exists():
                os.chmod(p, stat.S_IWRITE)
    return ok


def test_edit_encoding_strip():
    """§10.4 (FASTMD_ACP=1251): a character the file's encoding cannot hold: the ENCODING strip, no dialog; "Save as
    UTF-8" writes EF BB BF and UTF-8; "Remove the character" (another run) leaves the bytes as they were"""
    ok = True
    doc = OUT / "edit-encoding.md"
    text = "# Кодировка\n\nАбзац в кодировке 1251.\n"
    for run in ("utf8", "remove"):
        doc.write_bytes(text.encode("cp1251"))
        proc, hwnd = launch_edit(doc, {"FASTMD_ACP": "1251", "FASTMD_AUTOSAVE_MS": "300"})
        try:
            enter_edit(hwnd, 1, dx=1)
            type_text(hwnd, "✓", 0.1)
            strip = wait_for(lambda: q(hwnd, "EDIT_STRIP") == STRIP["ENCODING"], 3.0)
            if run == "utf8":
                shot(hwnd, "99-edit-encoding-strip")
                shot_dark(hwnd, "99-edit-encoding-strip-dark")
                state = q(hwnd, "EDIT_SAVE_STATE")
                cmd(hwnd, "ENC_UTF8", 0.5)
                want = text.replace("Абзац", "✓Абзац")
                ok &= check("edit 2a: ✓ in a 1251 file: UNENCODABLE and the strip, no dialog; Save as UTF-8 converts",
                            strip and state == SS["UNENCODABLE"] and doc.read_bytes() == b"\xef\xbb\xbf" + want.encode("utf-8")
                            and q(hwnd, "EDIT_STRIP") == 0 and q(hwnd, "EDIT_ENC") == 65001 | 3 << 24 and
                            q(hwnd, "LAST_PROMPT", 1) == 0, f'state {state}, enc {q(hwnd, "EDIT_ENC"):#x}, '
                            f'questions {q(hwnd, "LAST_PROMPT", 1)}')
            else:
                cmd(hwnd, "ENC_REMOVE_CHAR", 0.8)
                ok &= check("edit 2a: Remove the character: the bytes as they were, nothing unsaved",
                            strip and doc.read_bytes() == text.encode("cp1251") and q(hwnd, "EDIT_DIRTY") == 0 and
                            q(hwnd, "EDIT_STRIP") == 0)
            post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.4)
        finally:
            close_edit(proc, hwnd)
    return ok


def test_edit_close_session():
    """§10.9: WM_CLOSE right after typing saves (exit code 0); a session end saves without UI and leaves no journal;
    after a clean leave the recovery folder is empty"""
    ok = True
    doc = OUT / "edit-close.md"
    doc.write_bytes(EDIT_DOC.encode("utf-8"))
    rec = DATA / "recovery"
    clear_recovery()
    proc, hwnd = launch_edit(doc)
    enter_edit(hwnd, 1, dx=1)
    type_text(hwnd, "закрыть ", 0.1)
    note_selfcheck(hwnd)
    post(hwnd, WM_CLOSE, 0, 0, 0.1)
    try:
        proc.wait(5)
    except subprocess.TimeoutExpired:
        proc.kill()
    want = EDIT_DOC.replace("Первый", "закрыть Первый")
    ok &= check("edit 2a: WM_CLOSE 100 ms after typing: saved, exit code 0",
                proc.returncode == 0 and doc.read_bytes() == want.encode("utf-8"), f"rc {proc.returncode}")
    proc, hwnd = launch_edit(doc, {"FASTMD_AUTOSAVE_MS": "60000"})
    try:
        enter_edit(hwnd, 1, dx=1)
        type_text(hwnd, "сеанс ", 0.1)
        r1 = u32.SendMessageW(hwnd, WM_QUERYENDSESSION, 0, 0)
        u32.SendMessageW(hwnd, WM_ENDSESSION, 1, 0)
        want2 = want.replace("закрыть Первый", "сеанс закрыть Первый")
        journals = list(rec.glob("*.unsaved")) if rec.exists() else []
        ok &= check("edit 2a: WM_QUERYENDSESSION + WM_ENDSESSION: saved without a word, no journal left",
                    r1 == 1 and doc.read_bytes() == want2.encode("utf-8") and not journals and q(hwnd, "LAST_PROMPT", 1) == 0,
                    f'answer {r1}, journals {journals}, questions {q(hwnd, "LAST_PROMPT", 1)}')
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.5)
        left = list(rec.glob("*")) if rec.exists() else []
        ok &= check("edit 2a: after a clean leave the recovery folder is empty", not left, f"{left}")
    finally:
        close_edit(proc, hwnd)
    return ok


def test_edit_recovery():
    """§10.6: with autosave off the journal keeps the edits (3 s after the last one); the process dies; the next open
    offers them, and Restore brings them back into edit mode"""
    ok = True
    doc = OUT / "edit-journal.md"
    doc.write_bytes(EDIT_DOC.encode("utf-8"))
    rec = DATA / "recovery"
    clear_recovery()
    set_reg("Autosave", 0)
    try:
        proc, hwnd = launch_edit(doc)
        enter_edit(hwnd, 1, dx=1)
        type_text(hwnd, "журнал ", 0.1)
        time.sleep(3.5)
        journals = list(rec.glob("*.unsaved")) if rec.exists() else []
        state = q(hwnd, "EDIT_SAVE_STATE")
        note_selfcheck(hwnd)
        proc.kill()
        proc.wait(5)
        ok &= check("edit 2a: autosave off: the state is OFF, and 3 s later the journal holds the edits",
                    state == SS["OFF"] and len(journals) == 1 and doc.read_bytes() == EDIT_DOC.encode("utf-8"),
                    f"state {state}, journals {journals}")
        # the file changed since the journal was written: a copy can be opened, but Restore is not offered and does
        # nothing (the journal fits only the text it was written against, §10.6; T13)
        changed = EDIT_DOC.replace("Последний абзац.", "Последний абзац, изменённый потом.")
        doc.write_bytes(changed.encode("utf-8"))
        proc, hwnd = launch_edit(doc)
        try:
            shown = wait_for(lambda: q(hwnd, "EDIT_STRIP") == STRIP["RECOVERY"], 3.0)
            restore = q(hwnd, "EDIT_TOOL", CMD["RECOVERY_RESTORE"])
            cmd(hwnd, "RECOVERY_RESTORE", 0.8)
            ok &= check("edit 2a review: a journal of a file changed since: no Restore on the strip, the command does nothing",
                        shown and restore == -1 and q(hwnd, "EDITING") == 0 and q(hwnd, "SRC_HASH", 0) == src_hash(changed)
                        and doc.read_bytes() == changed.encode("utf-8"), f"strip {shown}, restore button {restore}")
        finally:
            close_edit(proc, hwnd)
        doc.write_bytes(EDIT_DOC.encode("utf-8"))
        proc, hwnd = launch_edit(doc)
        try:
            shown = wait_for(lambda: q(hwnd, "EDIT_STRIP") == STRIP["RECOVERY"], 3.0)
            shot(hwnd, "100-edit-journal-strip")
            shot_dark(hwnd, "100-edit-journal-strip-dark")
            cmd(hwnd, "RECOVERY_RESTORE", 0.8)
            want = EDIT_DOC.replace("Первый", "журнал Первый")
            ok &= check("edit 2a: the next open offers the unsaved edits; Restore puts them back in edit mode",
                        shown and q(hwnd, "EDITING") == 1 and q(hwnd, "SRC_HASH", 0) == src_hash(want) and
                        q(hwnd, "EDIT_DIRTY") == 1 and q(hwnd, "UNDO_DEPTH", 0) == 1,
                        f'strip {shown}, editing {q(hwnd, "EDITING")}')
            cmd(hwnd, "SAVE", 0.4)
            post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.4)
            ok &= check("edit 2a: ... saved, and the journals are gone", doc.read_bytes() == want.encode("utf-8") and
                        not list(rec.glob("*.unsaved")))
        finally:
            close_edit(proc, hwnd)
    finally:
        del_reg("Autosave")
    return ok


def test_edit_two_windows():
    """§10.11: a second window on a file another one edits cannot enter edit mode: its strip says so (and leaves the
    corner buttons usable); it can once the first leaves, or dies; Save As never writes over a file another window edits"""
    ok = True
    doc = OUT / "edit-two.md"
    doc.write_bytes(EDIT_DOC.encode("utf-8"))
    mine = OUT / "edit-two-mine.md"
    mine.write_bytes(EDIT_DOC.replace("Первый", "Мой").encode("utf-8"))
    a, ha = launch_edit(doc)
    try:
        enter_edit(ha, 1, dx=1)
        b, hb = launch_edit(doc)
        try:
            post(hb, WM_KEYDOWN, VK["f2"], 0, 0.5)
            img = shot(hb, "101-edit-other-window-strip")
            shot_dark(hb, "101-edit-other-window-strip-dark")
            ok &= check("edit 2a: the second window cannot enter; the OTHER WINDOW strip",
                        q(hb, "EDITING") == 0 and q(hb, "EDIT_STRIP") == STRIP["OTHER_WINDOW"] and q(ha, "EDITING") == 1)
            # the strip stops short of the gear and the pencil: they stay drawn and clickable (review of 2a: UX)
            gear = gear_box(hb)
            if gear:
                click(hb, (gear[0] + gear[2]) // 2, (gear[1] + gear[3]) // 2, 0.8)
            sh = q(hb, "SETTINGS_HWND")
            ok &= check("edit 2a review: beside a reading-mode strip the gear still opens the settings",
                        gear and sh != 0 and q(hb, "EDIT_TOOL", CMD["EDIT_TOGGLE"]) > 0, f"gear {gear}, settings {sh}")
            if sh:
                post(sh, WM_KEYDOWN, 0x1B, 0, 0.3)
            cmd(hb, "STRIP_CLOSE", 0.3)
            ok &= check("edit 2a: [Read only] closes the strip", q(hb, "EDIT_STRIP") == 0)
            # the first window leaves: now the second can enter
            post(ha, WM_KEYDOWN, VK["esc"], 0, 0.5)
            post(hb, WM_KEYDOWN, VK["f2"], 0, 0.5)
            ok &= check("edit 2a review: once the first window leaves, the second enters",
                        q(ha, "EDITING") == 0 and q(hb, "EDITING") == 1, f'a {q(ha, "EDITING")}, b {q(hb, "EDITING")}')
            post(hb, WM_KEYDOWN, VK["esc"], 0, 0.5)
        finally:
            close_edit(b, hb)
        # a window editing another file does Save As onto this one, which the first window edits: refused (T6)
        time.sleep(1.1)
        post(ha, WM_KEYDOWN, VK["f2"], 0, 0.5)
        c, hc = launch_edit(mine, {"FASTMD_SAVE_AS": str(doc)})
        try:
            enter_edit(hc, 1, dx=1)
            type_text(hc, "чужое ", 0.2)
            cmd(hc, "SAVE_AS", 0.8)
            ok &= check("edit 2a review: Save As onto a file another window edits is refused; both stay as they were",
                        q(ha, "EDITING") == 1 and title_of(hc).startswith(mine.name) and
                        doc.read_bytes() == EDIT_DOC.encode("utf-8") and q(ha, "SRC_HASH", 0) == src_hash(EDIT_DOC),
                        f"title {title_of(hc)!r}")
            post(hc, WM_KEYDOWN, VK["esc"], 0, 0.5)
        finally:
            close_edit(c, hc)
        # the first window dies while editing: its claim on the file goes with it
        note_selfcheck(ha)
        a.kill()
        a.wait(5)
        b, hb = launch_edit(doc)
        try:
            post(hb, WM_KEYDOWN, VK["f2"], 0, 0.5)
            ok &= check("edit 2a review: after the editing window was killed another one enters", q(hb, "EDITING") == 1)
            post(hb, WM_KEYDOWN, VK["esc"], 0, 0.5)
        finally:
            close_edit(b, hb)
    finally:
        if a.poll() is None:
            close_edit(a, ha)
    return ok


FRAMES_DOC = ("# Кадры правки\n\nПервый абзац текста, который достаточно длинный, чтобы переноситься в колонке окна "
              "на следующую строку.\n\nБейдж в строке: <img src=\"img/diagram0.png\" width=\"40\"> и текст.\n\n"
              "1. Первый\n2. Второй\n3. Третий\n\n| Колонка | Значение |\n|---|---|\n| а | 1 |\n| б | 2 |\n\n"
              "Формула: $x^2$ в строке.\n\n" +
              "".join(f"## Раздел {k}\n\nАбзац {k} для прокрутки, с текстом на пару строк в колонке окна.\n\n" for k in range(1, 16)))


def test_edit_frames():
    """§12.3, T7: in edit mode a scroll is still a partial frame and equals a full one; after typing and structural
    changes the frame is what a fresh layout draws; the bar collapses in a narrow window"""
    ok = True
    doc = OUT / "edit-frames.md"
    doc.write_bytes(FRAMES_DOC.encode("utf-8"))
    set_reg("EditHintShown", 1)  # the first-use toast lies over the text: its frames are full ones
    proc, hwnd = launch_edit(doc, {"FASTMD_TEST_HOOKS": "1"}, size="--size=900x800")
    try:
        wait_for(lambda: q(hwnd, "MATH", 1) == 1, 5.0)
        enter_edit(hwnd, 1, dx=1)
        settle(hwnd)
        same = []
        for n in (-3, 1):  # down, then up (the frames the scroll left on screen, not a frame drawn for the shot: A2)
            q(hwnd, "FRAME_STATS", 0)
            wheel(hwnd, 450, 400, n, wait=0.6)
            settle(hwnd)
            partial = q(hwnd, "FRAME_STATS", 0)
            a = shot(hwnd, f"102-edit-scrolled{n}", paint=False)
            b = shot(hwnd, f"102-edit-full{n}")
            same.append((partial, same_pixels(a, b)))
        ok &= check("edit 2a: a wheel scroll in edit mode is a partial frame, the same as a full one",
                    all(p > 0 and s for p, s in same), f"(partial frames, same) {same}")
        wheel(hwnd, 450, 400, 10, wait=0.6)
        settle(hwnd)
        src = FRAMES_DOC
        type_text(hwnd, "набор ", 0.3)
        at = src.index("Первый абзац")
        src = src[:at] + "набор " + src[at:]
        settle(hwnd)
        ok &= check("edit 2a: after typing the frame is a fresh layout's", relayout_same(hwnd, "103-edit-typed"))
        steps = [("a heading appears", lambda s: (s.index("Бейдж"), 0, "## ")),
                 ("the heading goes", lambda s: (s.index("## Бейдж"), 3, "")),
                 ("a picture before the badge", lambda s: (s.index("Бейдж"), 0, "![](img/diagram0.png)\n\n")),
                 ("a list renumber", lambda s: (s.index("3. Третий"), 1, "7")),
                 ("a table row", lambda s: (s.index("| б | 2 |\n") + len("| б | 2 |\n"), 0, "| в | 3 |\n")),
                 ("a formula edit", lambda s: (s.index("$x^2$") + 1, 3, "y^3"))]
        for what, where in steps:
            at, length, text = where(src)
            r, src = splice(hwnd, src, at, length, text)
            settle(hwnd)
            ok &= check(f"edit 2a: {what}: the source is right and the frame is a fresh layout's",
                        r == 1 and q(hwnd, "SRC_HASH", 0) == src_hash(src) and relayout_same(hwnd, f"104-edit-{what[:12]}"))
        # a tooltip under the hovered button (the test hooks keep the hover: a posted move is followed by a leave); told
        # by what changed against the same view without it, in a box clear of the scrollbar (T7)
        tips = []
        for theme in ("THEME_LIGHT", "THEME_DARK"):
            cmd(hwnd, theme, 0.6)
            post(hwnd, WM_MOUSEMOVE, 0, lp(450, 500), 0.3)
            plain = shot(hwnd, f"105-edit-tip-none-{theme[6:].lower()}")
            for name in ("UNDO", "FMT_BOLD", "EDIT_EXIT"):
                c = q(hwnd, "EDIT_TOOL", CMD[name])
                x, y = c & 0xFFFF, c >> 16
                post(hwnd, WM_MOUSEMOVE, 0, lp(x, y), 0.3)
                img = shot(hwnd, f"105-edit-tip-{name.lower()}-{theme[6:].lower()}")
                box = (x - 60, y + 25, min(x + 30, img.width - 20), y + 45)
                d = ImageChops.difference(plain.crop(box), img.crop(box)).convert("L").point(lambda v: 255 if v > 40 else 0)
                tips.append(sum(1 for v in d.getdata() if v) > 20)
            post(hwnd, WM_MOUSEMOVE, 0, lp(450, 500), 0.3)
        cmd(hwnd, "THEME_LIGHT", 0.6)
        ok &= check("edit 2a: a tooltip under each hovered button (undo, bold, ✕; light and dark)", all(tips), f"{tips}")
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.5)
    finally:
        close_edit(proc, hwnd)
    proc, hwnd = launch_edit(doc, size="--size=400x600")
    try:
        enter_edit(hwnd, 1, dx=1)
        img = shot(hwnd, "106-edit-bar-narrow")
        ok &= check("edit 2a: a 400 px window collapses the bar (\"…\" shown)", q(hwnd, "EDIT_COLLAPSE") >= 3 and
                    q(hwnd, "EDIT_TOOL", CMD["EDIT_MORE"]) > 0 and q(hwnd, "EDIT_TOOL", CMD["EDIT_EXIT"]) > 0,
                    f'level {q(hwnd, "EDIT_COLLAPSE")}')
        cmd(hwnd, "THEME_DARK", 0.6)
        shot(hwnd, "106-edit-bar-narrow-dark")
        cmd(hwnd, "THEME_LIGHT", 0.4)
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.4)
    finally:
        close_edit(proc, hwnd)
    return ok


def test_edit_blink():
    """§12.2 (no STEADY): the caret blinks; it hides without the keyboard focus and shows again with it"""
    ok = True
    doc = OUT / "edit-blink.md"
    doc.write_bytes(EDIT_DOC.encode("utf-8"))
    proc, hwnd = launch_edit(doc, steady=False)
    try:
        enter_edit(hwnd, 1, dx=1)
        u32.GetCaretBlinkTime.restype = wt.UINT
        blink = u32.GetCaretBlinkTime()
        steady = blink in (0, 0xFFFFFFFF)  # blinking switched off in Windows: the caret must stay put (T11)
        p0 = q(hwnd, "EDIT_CARET_PHASE")
        changed = wait_for(lambda: q(hwnd, "EDIT_CARET_PHASE") != p0, 1.2 if steady else 2 * blink / 1000.0 + 0.3, 0.02)
        post(hwnd, WM_KILLFOCUS, 0, 0, 0.2)
        hidden = q(hwnd, "EDIT_CARET_VISIBLE") == 0
        post(hwnd, WM_SETFOCUS, 0, 0, 0.1)
        ok &= check("edit 2a: the caret blinks (or, with blinking off in Windows, never); without the keyboard it hides",
                    changed != steady and hidden and q(hwnd, "EDIT_CARET_VISIBLE") == 1,
                    f"blink {blink} ms, changed {changed}, hidden {hidden}")
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.4)
    finally:
        close_edit(proc, hwnd)
    return ok


def test_edit_perf():
    """§5.8: typing into medium.md (median < 3 ms, p95 < 8 ms per swap); into large.md the keystrokes are deferred and
    the 150 ms tick is measured"""
    ok = True
    doc = OUT / "edit-medium.md"
    shutil.copy(MEDIUM, doc)
    proc, hwnd = launch_edit(doc, {"FASTMD_AUTOSAVE_MS": "60000", "FASTMD_EDIT_SELFCHECK": "0"})
    try:
        enter_edit(hwnd, 2, dx=2)
        h0 = q(hwnd, "SRC_HASH", 0)
        type_text(hwnd, ("быстрый набор " * 20)[:200], 0.5, gap=0.01)
        stats = [q(hwnd, "EDIT_STATS", k) for k in range(7)]
        print(f"       medium.md, 200 characters: median {stats[0]} µs, p95 {stats[1]} µs, max {stats[2]} µs over {stats[3]} "
              f"swaps (parse {stats[4]}, carry+diff {stats[5]}, install+layout {stats[6]})")
        # the ring holds the last 128 swaps: all of them typing's, and the text did change (T9)
        ok &= check("edit 2a: typing into medium.md: median < 3000 µs, p95 < 8000 µs", stats[0] < 3000 and stats[1] < 8000 and
                    stats[3] == 128 and q(hwnd, "SRC_HASH", 0) != h0, f"{stats[0]} / {stats[1]} µs over {stats[3]} swaps")
        cmd(hwnd, "SAVE", 0.5)
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.5)
    finally:
        close_edit(proc, hwnd)
    doc = OUT / "edit-large.md"
    shutil.copy(REPO / "bench" / "corpus" / "large.md", doc)
    proc, hwnd = launch_edit(doc, {"FASTMD_AUTOSAVE_MS": "60000", "FASTMD_EDIT_SELFCHECK": "0"})
    try:
        counts = []
        wait_for(lambda: counts.append(q(hwnd, "BLOCK_COUNT")) or (len(counts) > 3 and counts[-1] == counts[-4] and counts[-1] > 2000), 8.0, 0.25)
        enter_edit(hwnd, 2, dx=2)
        n0 = q(hwnd, "EDIT_STATS", 3)
        t0 = time.perf_counter()
        type_text(hwnd, ("большой " * 7)[:50], 0.6, gap=0.01)
        stats = [q(hwnd, "EDIT_STATS", k) for k in range(7)]
        print(f"       large.md, 50 characters: {stats[3] - n0} swaps; median {stats[0]} µs, p95 {stats[1]} µs "
              f"(parse {stats[4]}, carry+diff {stats[5]}, install+layout {stats[6]})")
        ok &= check("edit 2a: typing into large.md is deferred: far fewer swaps than keystrokes, the tick < 100 ms",
                    0 < stats[3] - n0 < 20 and stats[1] < 100000, f"{stats[3] - n0} swaps, p95 {stats[1]} µs")
        # appending at the end of a line - the commonest place to write - is deferred too (review of 2a: A3)
        post(hwnd, WM_KEYDOWN, VK["end"], 0, 0.5)
        settle(hwnd)
        n1 = q(hwnd, "EDIT_STATS", 3)
        type_text(hwnd, "конецстроки" + "x" * 9, 0.6, gap=0.01)
        settle(hwnd)
        swaps = q(hwnd, "EDIT_STATS", 3) - n1
        print(f"       large.md, 20 characters at a line's end: {swaps} swaps")
        ok &= check("edit 2a review: typing at the end of a line of large.md is deferred as well", 0 < swaps < 6,
                    f"{swaps} swaps")
        cmd(hwnd, "SAVE", 1.0)
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 1.0)
    finally:
        close_edit(proc, hwnd)
    return ok


def test_edit_debounced():
    """§5.7 (DEBOUNCE_CHARS=0: every keystroke deferred): "ab" after an emoji, End, "c" gives the exact text; the
    deferred re-parse is pending right after typing and done within 300 ms"""
    ok = True
    doc = OUT / "edit-debounced.md"
    text = "# Отложенный разбор\n\nСмайлик \U0001F600 и хвост\n"
    doc.write_bytes(text.encode("utf-8"))
    proc, hwnd = launch_edit(doc, {"FASTMD_EDIT_DEBOUNCE_CHARS": "0", "FASTMD_AUTOSAVE_MS": "60000"})
    try:
        enter_edit(hwnd, 1, dx=1)
        post(hwnd, WM_KEYDOWN, VK["home"], 0, 0.1)
        for _ in range(9):  # "Смайлик " and the emoji
            post(hwnd, WM_KEYDOWN, VK["right"], 0, 0.03)
        type_text(hwnd, "ab", 0.0, gap=0.005)
        busy = q(hwnd, "EDIT_BUSY") & 1
        cleared = wait_for(lambda: not (q(hwnd, "EDIT_BUSY") & 1), 0.3, 0.01)
        post(hwnd, WM_KEYDOWN, VK["end"], 0, 0.1)
        type_text(hwnd, "c", 0.3)
        want = text.replace("\U0001F600 и хвост", "\U0001F600ab и хвостc")
        cmd(hwnd, "SAVE", 0.4)
        ok &= check("edit 2a: deferred typing: the re-parse is pending, then done within 300 ms; the text is exact",
                    busy and cleared and doc.read_bytes() == want.encode("utf-8"), f"busy {busy}, cleared {cleared}")
        # a lone low surrogate in a burst becomes U+FFFD in the source and in the history alike: undo still fits (T4)
        time.sleep(1.6)  # (a step of its own)
        depth = q(hwnd, "UNDO_DEPTH", 0)
        post(hwnd, WM_KEYDOWN, VK["home"], 0, 0.1)
        for _ in range(2):  # mid-line, where a burst always went on deferred
            post(hwnd, WM_KEYDOWN, VK["right"], 0, 0.03)
        type_text(hwnd, "z", 0.0, gap=0.005)
        post(hwnd, WM_CHAR, 0xDC00, 0, 0.4)
        typed = q(hwnd, "SRC_HASH", 0) == src_hash(want.replace("Смайлик", "Смz�айлик"))
        cmd(hwnd, "UNDO", 0.4)
        ok &= check("edit 2a review: a lone low surrogate typed in a burst: U+FFFD, and undo takes it back (history kept)",
                    typed and q(hwnd, "SRC_HASH", 0) == src_hash(want) and q(hwnd, "UNDO_DEPTH", 0) == depth,
                    f'typed {typed}, depth {depth} → {q(hwnd, "UNDO_DEPTH", 0)}')
        # a deferred Backspace takes the cluster typed in the burst, a letter with its combining accent (§5.7)
        type_text(hwnd, "é", 0.0, gap=0.005)
        post(hwnd, WM_KEYDOWN, VK["back"], 0, 0.4)
        ok &= check("edit 2a review: a deferred Backspace takes a whole cluster (e + combining acute)",
                    q(hwnd, "SRC_HASH", 0) == src_hash(want))
        cmd(hwnd, "SAVE", 0.4)
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.4)
    finally:
        close_edit(proc, hwnd)
    return ok


def test_settings_autosave():
    """§2.12: the settings row (hit ids 1200 / 1201) switches autosave; off, nothing is written until Ctrl+S or leaving"""
    ok = True
    doc = OUT / "edit-autosave.md"
    doc.write_bytes(EDIT_DOC.encode("utf-8"))
    proc, hwnd = launch_edit(doc, {"FASTMD_AUTOSAVE_MS": "300"})
    try:
        cmd(hwnd, "SETTINGS", 0.8)
        sh = q(hwnd, "SETTINGS_HWND")
        shot(sh, "107-settings-autosave")
        # the new row in the dark theme too (§15.1 item 9): the settings window follows the theme at once
        cmd(hwnd, "THEME_DARK", 0.6)
        shot(sh, "107-settings-autosave-dark")
        cmd(hwnd, "THEME_LIGHT", 0.5)
        click_setting(hwnd, sh, 1200)
        off = reg_value("Autosave") == 0
        post(sh, WM_KEYDOWN, 0x1B, 0, 0.3)
        enter_edit(hwnd, 1, dx=1)
        type_text(hwnd, "вручную ", 1.0)
        want = EDIT_DOC.replace("Первый", "вручную Первый")
        unsaved = doc.read_bytes() == EDIT_DOC.encode("utf-8") and q(hwnd, "EDIT_SAVE_STATE") == SS["OFF"]
        cmd(hwnd, "SAVE", 0.4)
        ok &= check("edit 2a: autosave off (1200): nothing written until Ctrl+S; state OFF",
                    off and unsaved and doc.read_bytes() == want.encode("utf-8"), f"off {off}, unsaved {unsaved}")
        type_text(hwnd, "ещё ", 0.8)
        want2 = want.replace("вручную Первый", "вручную ещё Первый")
        unsaved = doc.read_bytes() == want.encode("utf-8")
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.5)
        ok &= check("edit 2a: ... and leaving edit mode saves", unsaved and doc.read_bytes() == want2.encode("utf-8"))
        cmd(hwnd, "SETTINGS", 0.8)
        sh = q(hwnd, "SETTINGS_HWND")
        click_setting(hwnd, sh, 1201)
        ok &= check("edit 2a: autosave on again (1201)", reg_value("Autosave") == 1)
        post(sh, WM_KEYDOWN, 0x1B, 0, 0.3)
    finally:
        close_edit(proc, hwnd)
    return ok


# ------------------------------------------------------------------------------------------------ the review of 2a
REVIEW_DOC = ("# Проверка правки\n\n"
              "Длинный абзац, который обязательно переносится в колонке окна на несколько строк, потому что в нём "
              "много слов, и они никак не помещаются в одну строку ширины колонки документа, даже широкой.\n\n"
              "Второй абзац для перехода вниз, тоже достаточно длинный, чтобы колонка переносила его хотя бы на одну "
              "следующую строку текста.\n\n---\n\nПосле линии абзац.\n\n"
              "<details open><summary>Сводка</summary>\n\nТело раскрытого блока.\n\n</details>\n\nКонец начала.\n\n" +
              "".join(f"## Раздел {k}\n\nАбзац {k} для прокрутки, с текстом на пару строк в колонке окна.\n\n"
                      for k in range(1, 26)))


def caret_xy(hwnd):
    c = q(hwnd, "CARET")
    return (c & 0xFFFF, c >> 16) if c >= 0 else None


def test_edit_review_keys():
    """what the review of 2a found in the keys and the mouse (§2.1, §2.7, §6.2, §12.1, §12.4): End on a wrapped line,
    ↓ after typing, the arrows after Backspace selected a rule, F2 on a selected object, folding a <details>, a right
    click on the bar, dragging a selection over the bar, the pencil far from the old caret, an outline jump"""
    ok = True
    doc = OUT / "edit-review.md"
    doc.write_bytes(REVIEW_DOC.encode("utf-8"))
    set_reg("EditHintShown", 1)
    proc, hwnd = launch_edit(doc, {"FASTMD_AUTOSAVE_MS": "60000"})
    try:
        enter_edit(hwnd, 1, dx=2)
        post(hwnd, WM_KEYDOWN, VK["home"], 0, 0.2)
        home = caret_xy(hwnd)
        post(hwnd, WM_KEYDOWN, VK["end"], 0, 0.2)
        end1 = caret_xy(hwnd)
        post(hwnd, WM_KEYDOWN, VK["end"], 0, 0.2)
        end2 = caret_xy(hwnd)
        shot(hwnd, "109-edit-end-wrapped")
        post(hwnd, WM_KEYDOWN, VK["home"], 0, 0.2)
        home2 = caret_xy(hwnd)
        ok &= check("edit 2a review: End on a wrapped line: at the end of that line (again: the same), Home: its start",
                    home and end1 and end1[1] == home[1] and end1[0] > home[0] + 100 and end2 == end1 and home2 == home,
                    f"home {home}, End {end1}, End {end2}, Home {home2}")
        post(hwnd, WM_KEYDOWN, VK["down"], 0, 0.2)
        type_text(hwnd, "0123456789", 0.3)
        before = caret_xy(hwnd)
        post(hwnd, WM_KEYDOWN, VK["down"], 0, 0.3)
        after = caret_xy(hwnd)
        ok &= check("edit 2a review: ↓ after typing goes down from where the typing left the caret",
                    before and after and after[1] > before[1] and abs(after[0] - before[0]) < 16, f"{before} → {after}")
        # Backspace at the start of the paragraph after the rule selects the rule; → goes on from the rule
        click(hwnd, q(hwnd, "TEXT_LEFT") + 1, q(hwnd, "BLOCK_Y", 4) + 10, 0.3)
        start = q(hwnd, "EDIT_CARET_SRC")
        post(hwnd, WM_KEYDOWN, VK["back"], 0, 0.3)
        atom = q(hwnd, "EDIT_ATOM")
        post(hwnd, WM_KEYDOWN, VK["right"], 0, 0.3)
        ok &= check("edit 2a review: Backspace selects the rule before the paragraph; → goes to the paragraph after the rule",
                    atom == 0x40000000 | 3 and q(hwnd, "EDIT_ATOM") == -1 and q(hwnd, "EDIT_CARET_SRC") == start,
                    f'atom {atom:#x}, caret {start} → {q(hwnd, "EDIT_CARET_SRC")}')
        # F2 with an object selected leaves edit mode (its popup is 3b's)
        post(hwnd, WM_KEYDOWN, VK["back"], 0, 0.3)
        selected = q(hwnd, "EDIT_ATOM") >= 0
        post(hwnd, WM_KEYDOWN, VK["f2"], 0, 0.6)
        ok &= check("edit 2a review: F2 with an object selected leaves edit mode", selected and q(hwnd, "EDITING") == 0)
        time.sleep(1.1)
        text = doc.read_bytes().decode("utf-8")  # (leaving saved the digits)
        post(hwnd, WM_KEYDOWN, VK["f2"], 0, 0.6)
        # a <details> folded in edit mode: the caret leaves the text it hid, onto the summary
        click(hwnd, q(hwnd, "TEXT_LEFT") + 1, q(hwnd, "BLOCK_Y", 6) + 10, 0.3)
        inside = q(hwnd, "EDIT_CARET_SRC") == text.find("Тело")
        click(hwnd, q(hwnd, "TEXT_LEFT") + 5, q(hwnd, "BLOCK_Y", 5) + 10, 0.5)
        folded = q(hwnd, "EDIT_CARET_SRC") == text.find("<details") and q(hwnd, "EDIT_ATOM") == 0x40000000 | 5
        shot(hwnd, "110-edit-details-folded")
        shot_dark(hwnd, "110-edit-details-folded-dark")  # (the selected object's outline in the dark theme too)
        kept = q(hwnd, "EDIT_ATOM")
        click(hwnd, q(hwnd, "TEXT_LEFT") + 5, q(hwnd, "BLOCK_Y", 5) + 10, 0.5)  # open again
        ok &= check("edit 2a review: folding a <details> takes the caret out of it, onto its summary", inside and folded,
                    f'inside {inside}, caret {q(hwnd, "EDIT_CARET_SRC")}')
        ok &= check("edit 2a review: the theme's re-parse keeps the summary selected", kept == 0x40000000 | 5,
                    f"atom {kept:#x}")
        # a right click on the bar: the menu, but the caret and the view stay (the text under the bar is hidden)
        wheel(hwnd, 450, 400, -3, wait=0.8)
        settle(hwnd)
        c0, y0 = q(hwnd, "EDIT_CARET_SRC"), q(hwnd, "SCROLLY")
        bx = (q(hwnd, "EDIT_TOOL", CMD["SAVE"]) & 0xFFFF) - 160
        post(hwnd, 0x0204, 2, lp(bx, 20), 0.05)  # WM_RBUTTONDOWN
        post(hwnd, 0x0205, 0, lp(bx, 20), 0.6)   # WM_RBUTTONUP: the context menu
        u32.SendMessageW(hwnd, 0x001F, 0, 0)     # WM_CANCELMODE: the menu goes
        time.sleep(0.3)
        ok &= check("edit 2a review: a right click on the bar moves neither the caret nor the view",
                    q(hwnd, "EDIT_CARET_SRC") == c0 and q(hwnd, "SCROLLY") == y0,
                    f'caret {c0} → {q(hwnd, "EDIT_CARET_SRC")}, scroll {y0} → {q(hwnd, "SCROLLY")}')
        # dragging a selection up over the bar scrolls, and the selection never reaches under the bar (the autoscroll
        # timer reads the real cursor: it is put there for the moment)
        wheel(hwnd, 450, 400, -6, wait=0.8)
        settle(hwnd)
        y0 = q(hwnd, "SCROLLY")
        pt, old = wt.POINT(450, 20), wt.POINT()
        u32.ClientToScreen(hwnd, ctypes.byref(pt))
        u32.GetCursorPos(ctypes.byref(old))
        u32.SetCursorPos(pt.x, pt.y)
        try:
            post(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp(450, 400), 0.05)
            post(hwnd, WM_MOUSEMOVE, MK_LBUTTON, lp(450, 20), 0.5)
            post(hwnd, WM_LBUTTONUP, 0, lp(450, 20), 0.3)
        finally:
            u32.SetCursorPos(old.x, old.y)
        c = caret_xy(hwnd)
        ok &= check("edit 2a review: a drag up over the bar scrolls; the selection ends in text that is in sight",
                    q(hwnd, "SCROLLY") < y0 and c and c[1] >= 44, f'scroll {y0} → {q(hwnd, "SCROLLY")}, caret {c}')
        # the pencil far below the caret the last session left: it enters where the reader looks
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.6)
        time.sleep(1.1)
        wheel(hwnd, 450, 400, -25, wait=1.2)
        settle(hwnd)
        y0 = q(hwnd, "SCROLLY")
        pen = q(hwnd, "EDIT_TOOL", CMD["EDIT_TOGGLE"])
        if pen > 0:
            click(hwnd, pen & 0xFFFF, pen >> 16, 0.4)
        wait_for(lambda: q(hwnd, "EDIT_BAR") == 100, 2.0)
        settle(hwnd)
        c = caret_xy(hwnd)
        ok &= check("edit 2a review: the pencil enters where the reader looks, not at the caret left far above",
                    q(hwnd, "EDITING") == 1 and abs(q(hwnd, "SCROLLY") - y0) < 60 and c and 44 <= c[1] <= 800,
                    f'scroll {y0} → {q(hwnd, "SCROLLY")}, caret {c}')
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.5)
    finally:
        close_edit(proc, hwnd)
    # an outline jump in edit mode lands below the bar (§12.1)
    doc.write_bytes(REVIEW_DOC.encode("utf-8"))
    proc, hwnd = launch_edit(doc, {"FASTMD_AUTOSAVE_MS": "60000"})
    try:
        cmd(hwnd, "TOC", 0.8)
        enter_edit(hwnd, 1, dx=2)
        docked = q(hwnd, "TOC_DOCKED")
        item = 12
        click(hwnd, 100, q(hwnd, "TOC_ITEM_Y", item), 1.2)
        settle(hwnd)
        by = q(hwnd, "BLOCK_Y", 8 + 2 * (item - 1))
        shot(hwnd, "111-edit-outline-jump")
        ok &= check("edit 2a review: an outline jump in edit mode puts the heading below the bar, and it is the current one",
                    docked == 1 and 44 <= by <= 90 and q(hwnd, "TOC_CURRENT") == item,
                    f'docked {docked}, heading at {by}, current {q(hwnd, "TOC_CURRENT")}')
        cmd(hwnd, "TOC", 0.5)
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.5)
    finally:
        close_edit(proc, hwnd)
        del_reg("Outline")
    return ok


def test_edit_review_doc():
    """more of the review of 2a: the outline neither docks nor undocks while editing (§12.7); an emptied document takes
    typing (§7.3); the typing check's fallbacks in the app itself (§7.3 step 5)"""
    ok = True
    doc = OUT / "edit-one-heading.md"
    doc.write_bytes("# Единственный заголовок\n\nАбзац под ним.\n".encode("utf-8"))
    set_reg("EditHintShown", 1)
    proc, hwnd = launch_edit(doc, {"FASTMD_TEST_HOOKS": "1"})
    try:
        cmd(hwnd, "TOC", 0.8)
        enter_edit(hwnd, 1, dx=2)
        docked, left = q(hwnd, "TOC_DOCKED"), q(hwnd, "TEXT_LEFT")
        r = copydata(hwnd, 1, "0\t2\t")  # "# " goes: no heading left
        settle(hwnd)
        frozen = r == 1 and q(hwnd, "TOC_DOCKED") == docked == 1 and q(hwnd, "TEXT_LEFT") == left
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.6)
        ok &= check("edit 2a review: the outline stays docked while editing removes the only heading; leaving lets it go",
                    frozen and q(hwnd, "EDITING") == 0 and q(hwnd, "TOC_DOCKED") == 0, f"frozen {frozen}")
    finally:
        close_edit(proc, hwnd)
        del_reg("Outline")
    doc = OUT / "edit-emptied.md"
    doc.write_bytes(b"x\n")
    proc, hwnd = launch_edit(doc, {"FASTMD_AUTOSAVE_MS": "60000"})
    try:
        enter_edit(hwnd, 0, dx=1)
        post(hwnd, WM_KEYDOWN, VK["end"], 0, 0.2)
        post(hwnd, WM_KEYDOWN, VK["back"], 0, 0.3)
        empty = q(hwnd, "BLOCK_COUNT") == 0
        type_text(hwnd, "yz", 0.3)
        cmd(hwnd, "SAVE", 0.4)
        ok &= check("edit 2a review: the last character of the document gone, typing starts it again",
                    empty and doc.read_bytes() == b"yz\n", f"empty {empty}, file {doc.read_bytes()!r}")
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.4)
    finally:
        close_edit(proc, hwnd)
    doc = OUT / "edit-fallbacks.md"
    text = "**API**s и формула\n\nthe $E$ is\n"
    doc.write_bytes(text.encode("utf-8"))
    proc, hwnd = launch_edit(doc, {"FASTMD_AUTOSAVE_MS": "60000"})
    try:
        wait_for(lambda: q(hwnd, "MATH", 1) == 1, 5.0)
        enter_edit(hwnd, 0, dx=1)
        post(hwnd, WM_KEYDOWN, VK["home"], 0, 0.1)
        for _ in range(3):
            post(hwnd, WM_KEYDOWN, VK["right"], 0, 0.05)
        type_text(hwnd, ".", 0.3)
        click(hwnd, q(hwnd, "TEXT_LEFT") + 1, q(hwnd, "BLOCK_Y", 1) + 10, 0.3)
        post(hwnd, WM_KEYDOWN, VK["home"], 0, 0.1)
        for _ in range(5):  # "the ", then over the formula
            post(hwnd, WM_KEYDOWN, VK["right"], 0, 0.05)
        type_text(hwnd, "x", 0.3)
        cmd(hwnd, "SAVE", 0.4)
        want = "**API**.s и формула\n\nthe $E$ x is\n"
        ok &= check("edit 2a review: typing's fallbacks in the app: after a closer (**API**.s), a blank beside a formula",
                    doc.read_bytes() == want.encode("utf-8"), f"file {doc.read_bytes().decode('utf-8')!r}")
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.4)
    finally:
        close_edit(proc, hwnd)
    # a hard break of two blanks: End stops at its left edge, → crosses it in one press, typing keeps it (§6.6);
    # at a paragraph's end a blank and the letter after it cost one swap each (no typing check for them)
    doc = OUT / "edit-hard-break.md"
    doc.write_bytes(b"abc  \ndef\n\nend of it\n")
    proc, hwnd = launch_edit(doc, {"FASTMD_AUTOSAVE_MS": "60000"})
    try:
        enter_edit(hwnd, 0, dx=1)
        post(hwnd, WM_KEYDOWN, VK["end"], 0, 0.2)
        type_text(hwnd, "x", 0.2)
        post(hwnd, WM_KEYDOWN, VK["home"], 0, 0.1)
        for _ in range(5):  # a, b, c, x - then over the break
            post(hwnd, WM_KEYDOWN, VK["right"], 0, 0.05)
        type_text(hwnd, "y", 0.2)
        click(hwnd, q(hwnd, "TEXT_LEFT") + 1, q(hwnd, "BLOCK_Y", 1) + 10, 0.3)
        post(hwnd, WM_KEYDOWN, VK["end"], 0, 0.2)
        swaps = []
        for s in (" ", "z"):
            n0 = q(hwnd, "EDIT_STATS", 3)
            type_text(hwnd, s, 0.3)
            swaps.append(q(hwnd, "EDIT_STATS", 3) - n0)
        cmd(hwnd, "SAVE", 0.4)
        ok &= check("edit 2a review: a hard break: End stops before it, → crosses it at once, the break stays; a blank and "
                    "a letter at a paragraph's end: one swap each", doc.read_bytes() == b"abcx  \nydef\n\nend of it z\n" and
                    swaps == [1, 1], f"file {doc.read_bytes()!r}, swaps {swaps}")
        # Ctrl+Shift+A is edit mode's select all (not reading mode's, which the source caret would not follow)
        keys(hwnd, [ord("A")], shift=True, ctrl=True, wait=0.3)
        ok &= check("edit 2a review: Ctrl+Shift+A selects all in edit mode",
                    q(hwnd, "EDIT_ANCHOR_SRC") == 0 and q(hwnd, "EDIT_CARET_SRC") == len("abcx  \nydef\n\nend of it z"),
                    f'{q(hwnd, "EDIT_ANCHOR_SRC")}..{q(hwnd, "EDIT_CARET_SRC")}')
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.4)
    finally:
        close_edit(proc, hwnd)
    return ok


ZOOM_DOC = "# Начало\n\n" + "".join(f"## Раздел {k}\n\nАбзац {k}: достаточно текста, чтобы документ прокручивался, и "
                                    f"строка переносилась хотя бы один раз в узкой колонке окна.\n\n" for k in range(1, 40))


def test_edit_chrome_zoom():
    """§12.1, §12.3 (the review of 2a): with a strip under the bar and the zoom changed while editing, scrolled frames
    - caught without making the app paint - equal full ones, leaving does not move the page, and a click on the lower
    half of a strip's button is the button's"""
    import stat
    ok = True
    doc = OUT / "edit-zoom.md"
    saved_as = OUT / "edit-zoom-saved.md"
    for p in (doc, saved_as):
        if p.exists():
            os.chmod(p, stat.S_IWRITE)
            p.unlink()
    doc.write_bytes(ZOOM_DOC.encode("utf-8"))
    os.chmod(doc, stat.S_IREAD)  # the READONLY strip at entry
    set_reg("EditHintShown", 1)
    proc, hwnd = launch_edit(doc, {"FASTMD_SAVE_AS": str(saved_as)})
    try:
        wheel(hwnd, 500, 400, -6, wait=1.0)
        post(hwnd, WM_KEYDOWN, VK["f2"], 0, 0.6)
        strip = q(hwnd, "EDIT_STRIP") == STRIP["READONLY"]
        for _ in range(2):
            cmd(hwnd, "ZOOM_OUT", 0.4)  # 80 %
        settle(hwnd)
        time.sleep(1.0)  # (the zoom's toast is drawn over the text: those frames are full ones)
        same = []
        for n in (-3, 2, 1):  # down, then up twice
            q(hwnd, "FRAME_STATS", 0)
            wheel(hwnd, 500, 500, n, wait=0.8)
            settle(hwnd)
            frames = q(hwnd, "FRAME_STATS", 0)
            a = shot(hwnd, f"108-edit-zoom-strip{n}", paint=False)
            b = shot(hwnd, f"108-edit-zoom-strip{n}-full")
            same.append((frames, same_pixels(a, b)))
        ok &= check("edit 2a review: a strip under the bar at 80 %: scrolled frames (down, up) equal full ones",
                    strip and all(f > 0 and s for f, s in same), f"strip {strip}, (partial frames, same) {same}")
        k = next((b for b in range(q(hwnd, "BLOCK_COUNT")) if q(hwnd, "BLOCK_Y", b) > 200), 5)
        y0 = q(hwnd, "BLOCK_Y", k)
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.2)
        wait_for(lambda: q(hwnd, "EDIT_BAR") == 0, 2.0)
        settle(hwnd)
        y1 = q(hwnd, "BLOCK_Y", k)
        ok &= check("edit 2a review: after a zoom change while editing, leaving does not move the page",
                    q(hwnd, "EDITING") == 0 and abs(y1 - y0) <= 1, f"block {k}: {y0} → {y1}")
        time.sleep(1.1)
        post(hwnd, WM_KEYDOWN, VK["f2"], 0, 0.6)
        for _ in range(3):
            cmd(hwnd, "ZOOM_OUT", 0.4)  # 50 %
        settle(hwnd)
        c = q(hwnd, "EDIT_TOOL", CMD["SAVE_AS"])
        if c > 0:
            click(hwnd, c & 0xFFFF, (c >> 16) + 9, 0.8)
        ok &= check("edit 2a review: at 50 % a click 9 px below the middle of the strip's «Save as…» runs it",
                    c > 0 and q(hwnd, "LAST_PROMPT") == 4 and saved_as.exists() and title_of(hwnd).startswith(saved_as.name),
                    f'button at {c}, prompt {q(hwnd, "LAST_PROMPT")}, title {title_of(hwnd)!r}')
        cmd(hwnd, "ZOOM_RESET", 0.4)
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.4)
    finally:
        close_edit(proc, hwnd)
        for p in (doc, saved_as):
            if p.exists():
                os.chmod(p, stat.S_IWRITE)
    return ok


def test_edit_modal():
    """§10.10 in edit mode (T13): inside a modal loop the autosave waits and a close waits; once the loop is over the
    close goes through - the edits saved, exit code 0"""
    ok = True
    doc = OUT / "edit-modal.md"
    doc.write_bytes(EDIT_DOC.encode("utf-8"))
    set_reg("EditHintShown", 1)
    proc, hwnd = launch_edit(doc, {"FASTMD_TEST_HOOKS": "1", "FASTMD_AUTOSAVE_MS": "400"})
    try:
        enter_edit(hwnd, 1, dx=1)
        type_text(hwnd, "модально ", 0.1)  # the autosave is armed for 400 ms after the last character
        typed = q(hwnd, "SRC_HASH", 0) == src_hash(EDIT_DOC.replace("Первый", "модально Первый"))
        note_selfcheck(hwnd)
        loop = threading.Thread(target=lambda: copydata(hwnd, 3, "1500"))  # a modal loop of 1.5 s in the app
        loop.start()
        time.sleep(0.9)
        post(hwnd, WM_CLOSE, 0, 0, 0.2)
        inside = proc.poll() is None and doc.read_bytes() == EDIT_DOC.encode("utf-8")
        loop.join(5)
        try:
            proc.wait(5)
        except subprocess.TimeoutExpired:
            pass
        want = EDIT_DOC.replace("Первый", "модально Первый")
        ok &= check("edit 2a review: inside a modal loop the autosave and a close wait; after it the close saves and goes",
                    typed and inside and proc.returncode == 0 and doc.read_bytes() == want.encode("utf-8"),
                    f"typed {typed}, inside {inside}, rc {proc.returncode}")
    finally:
        if proc.poll() is None:
            close_edit(proc, hwnd)
    return ok


def journals():
    rec = DATA / "recovery"
    return sorted(rec.glob("*.unsaved")) if rec.exists() else []


def fastmd_windows(prefix):
    out = []
    proto = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)

    def cb(h, _):
        cls = ctypes.create_unicode_buffer(64)
        u32.GetClassNameW(h, cls, 64)
        if cls.value == "FastMD.Document" and title_of(h).startswith(prefix):
            out.append(h)
        return True
    u32.EnumWindows(proto(cb), 0)
    return out


def test_edit_recovery_guards():
    """the review of 2a, data safety (§10.5, §10.6, §10.7): no entry on a torn file whose save is waiting on the strip;
    a journal restores only onto the text it was written against, decided when asked; a copy is never written over;
    the entry's purge keeps what the strip offers; a journal whose change starts inside an emoji restores; Discard with
    a save in flight; the disk version that edit mode cannot hold keeps the edits in the journal; with autosave off a
    file that comes back is not written"""
    ok = True
    set_reg("EditHintShown", 1)
    orig = b"# Title\n\nFirst paragraph for typing text.\n\nSecond paragraph here.\n"
    # a torn save: at the next open the strip; F2 is refused until the reader decides; Restore gives the original
    doc = OUT / "edit-torn.md"
    doc.write_bytes(orig)
    clear_recovery()
    p, h = launch_edit(doc, {"FASTMD_TEST_FAIL_WRITE": "partial:5,norollback", "FASTMD_AUTOSAVE_MS": "60000"})
    enter_edit(h, 1, dx=1)
    post(h, WM_KEYDOWN, VK["home"], 0, 0.1)
    post(h, WM_KEYDOWN, VK["delete"], 0, 0.2)
    cmd(h, "SAVE", 0.6)
    note_selfcheck(h)
    p.kill()
    p.wait(5)
    torn = doc.read_bytes() != orig
    p, h = launch_edit(doc)
    try:
        shown = wait_for(lambda: q(h, "EDIT_STRIP") == STRIP["RECOVERY"], 3.0)
        post(h, WM_KEYDOWN, VK["f2"], 0, 0.5)
        said = q(h, "LAST_PROMPT", 2) & 0xFFFFFFFF
        refused = q(h, "EDITING") == 0 and said == src_hash(
            "First decide what to do with the interrupted save (the strip above)" if q(h, "LANG") == 1 else
            "Сначала решите, что делать с прерванным сохранением (полоса сверху)")
        cmd(h, "RECOVERY_RESTORE", 1.0)
        ok &= check("edit 2a review: a torn file's save waits on the strip: no entry until then; Restore gives the original",
                    torn and shown and refused and doc.read_bytes() == orig, f"torn {torn}, strip {shown}, refused {refused}")
    finally:
        close_edit(p, h)
    # a journal, then the edits saved in edit mode past the strip: Restore would undo that save - not offered any more
    doc = OUT / "edit-journal-stale.md"
    doc.write_bytes(EDIT_DOC.encode("utf-8"))
    clear_recovery()
    set_reg("Autosave", 0)
    try:
        p, h = launch_edit(doc)
        enter_edit(h, 1, dx=1)
        type_text(h, "журнал ", 0.1)
        time.sleep(3.6)
        note_selfcheck(h)
        p.kill()
        p.wait(5)
    finally:
        del_reg("Autosave")
    p, h = launch_edit(doc, {"FASTMD_AUTOSAVE_MS": "200"})
    try:
        offered = wait_for(lambda: q(h, "EDIT_STRIP") == STRIP["RECOVERY"], 3.0) and \
            q(h, "EDIT_TOOL", CMD["RECOVERY_RESTORE"]) > 0
        enter_edit(h, 1, dx=1)
        type_text(h, "сохранено ", 0.2)
        saved(h)
        on_disk = doc.read_bytes()
        gone = q(h, "EDIT_TOOL", CMD["RECOVERY_RESTORE"]) == -1
        cmd(h, "RECOVERY_RESTORE", 0.8)
        ok &= check("edit 2a review: a journal is restorable only onto the text it was written against, asked when used",
                    offered and gone and doc.read_bytes() == on_disk and "сохранено" in on_disk.decode("utf-8") and
                    "журнал" not in on_disk.decode("utf-8"), f"offered {offered}, gone after the save {gone}")
        cmd(h, "RECOVERY_DELETE", 0.4)
        post(h, WM_KEYDOWN, VK["esc"], 0, 0.4)
    finally:
        close_edit(p, h)
    # "Open the copy" twice: the second copy has a name of its own, the first (edited in its window) stays
    import pathlib as pl
    doc = OUT / "edit-copy.md"
    doc.write_bytes(EDIT_DOC.encode("utf-8"))
    clear_recovery()
    tmp = pl.Path(os.environ["TEMP"]) / "FastMD"
    for f in tmp.glob("edit-copy (*") if tmp.exists() else []:
        f.unlink()
    set_reg("Autosave", 0)
    try:
        p, h = launch_edit(doc)
        enter_edit(h, 1, dx=1)
        type_text(h, "копия ", 0.1)
        time.sleep(3.6)
        note_selfcheck(h)
        p.kill()
        p.wait(5)
    finally:
        del_reg("Autosave")
    p, h = launch_edit(doc)
    try:
        wait_for(lambda: q(h, "EDIT_STRIP") == STRIP["RECOVERY"], 3.0)
        cmd(h, "RECOVERY_OPEN", 1.5)
        wins = []
        wait_for(lambda: bool(wins.extend(fastmd_windows("edit-copy (")) or wins), 5.0, 0.2)
        copies = sorted(tmp.glob("edit-copy (*"))
        kept = False
        if wins and copies:
            w2 = wins[0]
            enter_edit(w2, 1, dx=1)
            type_text(w2, "работа в копии ", 0.1)
            cmd(w2, "SAVE", 0.5)
            post(w2, WM_KEYDOWN, VK["esc"], 0, 0.4)
            cmd(h, "RECOVERY_OPEN", 1.5)
            kept = "работа в копии" in copies[0].read_bytes().decode("utf-8-sig") and \
                len(sorted(tmp.glob("edit-copy (*"))) == 2
        ok &= check("edit 2a review: \"Open the copy\" again makes a copy of its own; the first, edited, stays as saved",
                    kept, f"windows {len(wins)}, copies {[c.name for c in sorted(tmp.glob('edit-copy (*'))]}")
        cmd(h, "RECOVERY_DELETE", 0.4)
    finally:
        for w in fastmd_windows("edit-copy ("):
            post(w, WM_CLOSE, 0, 0, 0.5)
        close_edit(p, h)
    # a journal 20 days old on the strip: the entry's purge of old recovery files keeps it
    doc = OUT / "edit-journal-old.md"
    doc.write_bytes(EDIT_DOC.encode("utf-8"))
    clear_recovery()
    set_reg("Autosave", 0)
    try:
        p, h = launch_edit(doc)
        enter_edit(h, 1, dx=1)
        type_text(h, "старое ", 0.1)
        time.sleep(3.6)
        note_selfcheck(h)
        p.kill()
        p.wait(5)
    finally:
        del_reg("Autosave")
    old = time.time() - 20 * 86400
    for f in journals():
        os.utime(f, (old, old))
    p, h = launch_edit(doc)
    try:
        wait_for(lambda: q(h, "EDIT_STRIP") == STRIP["RECOVERY"], 3.0)
        post(h, WM_KEYDOWN, VK["f2"], 0, 0.5)
        ok &= check("edit 2a review: entering edit mode does not purge the journal its own strip offers",
                    q(h, "EDITING") == 1 and len(journals()) == 1, f"{journals()}")
        post(h, WM_KEYDOWN, VK["esc"], 0, 0.4)
        cmd(h, "RECOVERY_DELETE", 0.4)
    finally:
        close_edit(p, h)
    # a journal whose change starts inside an emoji's surrogate pair restores
    doc = OUT / "edit-journal-emoji.md"
    text = "# Смайлы\n\nСмайлик \U0001F600 и хвост\n"
    doc.write_bytes(text.encode("utf-8"))
    clear_recovery()
    set_reg("Autosave", 0)
    try:
        p, h = launch_edit(doc)
        enter_edit(h, 1, dx=1)
        post(h, WM_KEYDOWN, VK["home"], 0, 0.1)
        for _ in range(8):
            post(h, WM_KEYDOWN, VK["right"], 0, 0.03)
        type_text(h, "\U0001F601", 0.2)
        time.sleep(3.6)
        note_selfcheck(h)
        p.kill()
        p.wait(5)
    finally:
        del_reg("Autosave")
    want = text.replace("\U0001F600", "\U0001F601\U0001F600")
    p, h = launch_edit(doc)
    try:
        wait_for(lambda: q(h, "EDIT_STRIP") == STRIP["RECOVERY"], 3.0)
        cmd(h, "RECOVERY_RESTORE", 0.8)
        ok &= check("edit 2a review: a journal whose change starts inside an emoji's surrogate pair restores",
                    q(h, "EDITING") == 1 and q(h, "SRC_HASH", 0) == src_hash(want))
        cmd(h, "SAVE", 0.4)
        post(h, WM_KEYDOWN, VK["esc"], 0, 0.4)
    finally:
        close_edit(p, h)
    # Discard while a save of a big document is in flight on the worker: the file and the window agree afterwards
    doc = OUT / "edit-discard-big.md"
    para = "Lorem ipsum dolor sit amet consectetur adipiscing elit sed do eiusmod tempor incididunt ut labore. " * 8
    parts, n, k = ["# Big\n\nFirst paragraph here.\n\n"], 30, 0
    while n < 1_150_000:
        s = f"## Section {k}\n\n{para}\n\n"
        parts.append(s)
        n += len(s)
        k += 1
    doc.write_bytes("".join(parts).encode("utf-8"))
    p, h = launch_edit(doc, {"FASTMD_TEST_FAIL_WRITE": "busy", "FASTMD_TEST_SLOW": "save:3000",
                             "FASTMD_AUTOSAVE_MS": "60000"})
    try:
        counts = []
        wait_for(lambda: counts.append(q(h, "BLOCK_COUNT")) or (len(counts) > 3 and counts[-1] == counts[-4]), 8.0, 0.25)
        enter_edit(h, 1, dx=1)
        type_text(h, "ZZZ ", 0.2)
        post(h, WM_KEYDOWN, VK["esc"], 0, 0.3)
        leave = q(h, "EDIT_STRIP") == STRIP["LEAVE"]
        time.sleep(1.0)  # the 0.5 s retry started a worker save that sleeps 3 s
        cmd(h, "DISCARD_EDITS", 4.5)
        disk = doc.read_bytes().decode("utf-8")
        ok &= check("edit 2a review: Discard with a save in flight: the file and the window agree, nothing unsaved",
                    leave and q(h, "EDITING") == 0 and q(h, "SRC_HASH", 0) == src_hash(disk) and q(h, "EDIT_DIRTY") == 0 and
                    "*" not in title_of(h).split(" — ")[0], f'leave strip {leave}, title {title_of(h)!r}')
    finally:
        close_edit(p, h)
    # the other program wrote bytes edit mode cannot hold: "load the disk version" keeps the edits in the journal
    doc = OUT / "edit-load-binary.md"
    doc.write_bytes(EDIT_DOC.encode("utf-8"))
    clear_recovery()
    p, h = launch_edit(doc, {"FASTMD_AUTOSAVE_MS": "60000"})
    try:
        enter_edit(h, 1, dx=1)
        type_text(h, "наше ", 0.2)
        binary = b"# Other\n\nabc\x00def\n"
        doc.write_bytes(binary)
        wait_for(lambda: q(h, "EDIT_CONFLICT") == 1, 3.0)
        cmd(h, "CONFLICT_LOAD", 0.8)
        offered = wait_for(lambda: q(h, "EDIT_STRIP") == STRIP["RECOVERY"], 3.0)
        j = journals()
        kept = bool(j) and "наше Первый".encode("utf-16-le") in j[0].read_bytes()
        ok &= check("edit 2a review: loading a disk version edit mode cannot hold: reading mode, the edits in the journal",
                    q(h, "EDITING") == 0 and doc.read_bytes() == binary and offered and kept, f"strip {offered}, journal {j}")
        cmd(h, "RECOVERY_DELETE", 0.4)
    finally:
        close_edit(p, h)
    # autosave off: a file that comes back after MISSING is not written until Ctrl+S (§2.12)
    doc = OUT / "edit-off-back.md"
    away = OUT / "edit-off-back-away.md"
    for f in (doc, away):
        if f.exists():
            f.unlink()
    doc.write_bytes(EDIT_DOC.encode("utf-8"))
    set_reg("Autosave", 0)
    try:
        p, h = launch_edit(doc)
        try:
            enter_edit(h, 1, dx=1)
            type_text(h, "выкл ", 0.2)
            doc.rename(away)
            wait_for(lambda: q(h, "EDIT_STRIP") == STRIP["MISSING"], 4.0)
            away.rename(doc)
            back = wait_for(lambda: q(h, "EDIT_STRIP") != STRIP["MISSING"], 4.0)
            time.sleep(1.0)
            ok &= check("edit 2a review: autosave off: the file back after MISSING is not written without Ctrl+S",
                        back and doc.read_bytes() == EDIT_DOC.encode("utf-8") and q(h, "EDIT_SAVE_STATE") == SS["OFF"],
                        f'state {q(h, "EDIT_SAVE_STATE")}')
            cmd(h, "SAVE", 0.4)
            post(h, WM_KEYDOWN, VK["esc"], 0, 0.4)
        finally:
            close_edit(p, h)
        # ... nor a read-only file that became writable, when the window is activated again
        import stat
        ro = OUT / "edit-off-readonly.md"
        if ro.exists():
            os.chmod(ro, stat.S_IWRITE)
        ro.write_bytes(EDIT_DOC.encode("utf-8"))
        os.chmod(ro, stat.S_IREAD)
        p, h = launch_edit(ro)
        try:
            enter_edit(h, 1, dx=1)
            type_text(h, "выкл ", 0.2)
            was = q(h, "EDIT_STRIP") == STRIP["READONLY"]
            os.chmod(ro, stat.S_IWRITE)
            post(h, 0x0006, 1, 0, 0.6)  # WM_ACTIVATE (WA_ACTIVE)
            ok &= check("edit 2a review: autosave off: a read-only file that became writable is not written on activation",
                        was and q(h, "EDIT_STRIP") == 0 and ro.read_bytes() == EDIT_DOC.encode("utf-8") and
                        q(h, "EDIT_SAVE_STATE") == SS["OFF"], f'strip {q(h, "EDIT_STRIP")}, state {q(h, "EDIT_SAVE_STATE")}')
            cmd(h, "SAVE", 0.4)
            post(h, WM_KEYDOWN, VK["esc"], 0, 0.4)
        finally:
            close_edit(p, h)
            os.chmod(ro, stat.S_IWRITE)
    finally:
        del_reg("Autosave")
    clear_recovery()
    return ok


# ------------------------------------------------------------------------------------------------ edit mode, phase 2b
STRUCT_DOC = ("# Структура\n\nАбзац раз.\n\n- пункт а\n- пункт б\n- \n- пункт в\n\n"
              "| x | y |\n|---|---|\n|  | z |\n")


def active(hwnd, bit):
    return bool(q(hwnd, "EDIT_ACTIVE") & (1 << bit))


def test_edit_structure():
    """§6.7, §7.6-§7.8 (2b): Enter at a paragraph's end makes a phantom row and writes nothing, typing makes it real;
    Enter in a list; Tab in a list; ↑/↓ stop on an empty item and an empty cell; ↓ out of a table that ends the document
    and a click below the last block make a phantom; Backspace at a heading's start; Enter in a mixed-EOL file writes
    the line's own ending"""
    ok = True
    doc = OUT / "edit-structure.md"
    doc.write_bytes(STRUCT_DOC.encode("utf-8"))
    set_reg("EditHintShown", 1)
    proc, hwnd = launch_edit(doc, {"FASTMD_AUTOSAVE_MS": "60000"})
    try:
        text = STRUCT_DOC
        enter_edit(hwnd, 1, dx=1)
        post(hwnd, WM_KEYDOWN, VK["end"], 0, 0.1)
        post(hwnd, WM_KEYDOWN, VK["return"], 0, 0.4)
        c = caret_xy(hwnd)
        ok &= check("edit 2b: Enter at a paragraph's end: a phantom row after it with the caret in it, the file unchanged",
                    q(hwnd, "EDIT_PHANTOM", 0) == 1 and active(hwnd, 15) and q(hwnd, "SRC_HASH", 0) == src_hash(text) and
                    q(hwnd, "EDIT_DIRTY") == 0 and c and q(hwnd, "BLOCK_Y", 1) + 20 < c[1] < q(hwnd, "BLOCK_Y", 2),
                    f'phantom {q(hwnd, "EDIT_PHANTOM", 0)}, caret {c}, blocks at {q(hwnd, "BLOCK_Y", 1)}, '
                    f'{q(hwnd, "BLOCK_Y", 2)}')
        shot(hwnd, "112-edit-phantom")
        shot_dark(hwnd, "112-edit-phantom-dark")
        type_text(hwnd, "Новый", 0.3)
        text = text.replace("Абзац раз.\n", "Абзац раз.\n\nНовый\n", 1)
        ok &= check("edit 2b: typing makes the phantom real: a paragraph of its own, blank lines around it",
                    q(hwnd, "SRC_HASH", 0) == src_hash(text) and q(hwnd, "EDIT_PHANTOM", 0) == -1,
                    f'len {q(hwnd, "SRC_LEN", 0)} vs {u16(text)}, phantom {q(hwnd, "EDIT_PHANTOM", 0)}')
        # Enter at an item's end writes an empty item (undone again)
        post(hwnd, WM_KEYDOWN, VK["down"], 0, 0.2)
        post(hwnd, WM_KEYDOWN, VK["end"], 0, 0.1)
        post(hwnd, WM_KEYDOWN, VK["return"], 0, 0.4)
        s1 = text.replace("- пункт а\n", "- пункт а\n- \n", 1)
        ok &= check("edit 2b: Enter at a list item's end writes an empty item, the caret in it",
                    q(hwnd, "SRC_HASH", 0) == src_hash(s1) and q(hwnd, "EDIT_CARET_SRC") == s1.index("- пункт а\n- \n") + 12,
                    f'caret {q(hwnd, "EDIT_CARET_SRC")}, len {q(hwnd, "SRC_LEN", 0)} vs {u16(s1)}')
        cmd(hwnd, "UNDO", 0.3)
        # Tab nests an item under the one before it (undone again)
        post(hwnd, WM_KEYDOWN, VK["down"], 0, 0.2)
        post(hwnd, WM_KEYDOWN, VK["tab"], 0, 0.4)
        s2 = text.replace("- пункт б\n", "  - пункт б\n", 1)
        ok &= check("edit 2b: Tab in a list item nests it under the item before", q(hwnd, "SRC_HASH", 0) == src_hash(s2),
                    f'len {q(hwnd, "SRC_LEN", 0)} vs {u16(s2)}')
        cmd(hwnd, "UNDO", 0.3)
        undone = q(hwnd, "SRC_HASH", 0) == src_hash(text)
        # ↓ from «пункт б»: the empty item, «пункт в», the table's first row, the empty cell of its second
        post(hwnd, WM_KEYDOWN, VK["down"], 0, 0.2)
        empty_item = q(hwnd, "EDIT_CARET_SRC") == text.index("- \n- пункт в") + 2
        post(hwnd, WM_KEYDOWN, VK["down"], 0, 0.2)
        post(hwnd, WM_KEYDOWN, VK["home"], 0, 0.1)
        post(hwnd, WM_KEYDOWN, VK["down"], 0, 0.2)
        x_cell = text.index("| x |") + 2  # (the column the caret keeps is right of the narrow «x»: before or after it)
        row0 = x_cell <= q(hwnd, "EDIT_CARET_SRC") <= x_cell + 1
        post(hwnd, WM_KEYDOWN, VK["down"], 0, 0.2)
        at, row1 = q(hwnd, "EDIT_CARET_SRC"), text.index("|  | z |")
        empty_cell = row1 < at < row1 + 4 and active(hwnd, 12)
        ok &= check("edit 2b: ↓ stops on an empty item and on an empty cell (the undos restored the text)",
                    undone and empty_item and row0 and empty_cell, f"undone {undone}, empty item {empty_item}, row 0 "
                    f"{row0}, caret {at} (row at {row1}), active {q(hwnd, 'EDIT_ACTIVE'):#x}")
        post(hwnd, WM_KEYDOWN, VK["up"], 0, 0.2)
        up = x_cell <= q(hwnd, "EDIT_CARET_SRC") <= x_cell + 1
        post(hwnd, WM_KEYDOWN, VK["down"], 0, 0.2)
        post(hwnd, WM_KEYDOWN, VK["down"], 0, 0.2)  # (§6.7: first to the last stop, the table's last cell's end)
        at_last = q(hwnd, "EDIT_CARET_SRC") == text.index("z |") + 1 and q(hwnd, "EDIT_PHANTOM", 0) == -1
        post(hwnd, WM_KEYDOWN, VK["down"], 0, 0.4)
        last = q(hwnd, "BLOCK_COUNT") - 1
        ok &= check("edit 2b: ↑ back into the row above; ↓ to the last stop, then out of a table that ends the document: a "
                    "phantom after it", up and at_last and q(hwnd, "EDIT_PHANTOM", 0) == last and active(hwnd, 15) and
                    q(hwnd, "SRC_HASH", 0) == src_hash(text), f'up {up}, at the last stop {at_last}, phantom '
                    f'{q(hwnd, "EDIT_PHANTOM", 0)} (last {last})')
        # the caret elsewhere: the phantom goes; a click well below the last block brings one back
        click(hwnd, q(hwnd, "TEXT_LEFT") + 1, q(hwnd, "BLOCK_Y", 1) + 10, 0.3)
        gone = q(hwnd, "EDIT_PHANTOM", 0) == -1
        click(hwnd, q(hwnd, "TEXT_LEFT") + 40, q(hwnd, "BLOCK_Y", last) + 160, 0.4)
        ok &= check("edit 2b: a click below the last block: a phantom after it, the caret in it",
                    gone and q(hwnd, "EDIT_PHANTOM", 0) == last and active(hwnd, 15), f'gone {gone}, phantom '
                    f'{q(hwnd, "EDIT_PHANTOM", 0)}')
        # Backspace at a heading's start makes it a paragraph
        click(hwnd, q(hwnd, "TEXT_LEFT") + 1, q(hwnd, "BLOCK_Y", 0) + 14, 0.3)
        post(hwnd, WM_KEYDOWN, VK["home"], 0, 0.1)
        post(hwnd, WM_KEYDOWN, VK["back"], 0, 0.4)
        text = text[2:]
        ok &= check("edit 2b: Backspace at a heading's start makes it a paragraph",
                    q(hwnd, "SRC_HASH", 0) == src_hash(text) and q(hwnd, "EDIT_CARET_SRC") == 0,
                    f'caret {q(hwnd, "EDIT_CARET_SRC")}, len {q(hwnd, "SRC_LEN", 0)} vs {u16(text)}')
        cmd(hwnd, "SAVE", 0.4)
        ok &= check("edit 2b: saved as it reads", doc.read_bytes() == text.encode("utf-8"))
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.4)
    finally:
        close_edit(proc, hwnd)
    # a file with both line ends: Enter writes the line's own (§7.6, D15)
    doc = OUT / "edit-mixed-eol.md"
    doc.write_bytes("Первая строка\r\n\r\nВторая строка\n\nТретья\n".encode("utf-8"))
    proc, hwnd = launch_edit(doc, {"FASTMD_AUTOSAVE_MS": "60000"})
    try:
        enter_edit(hwnd, 0, dx=1)
        post(hwnd, WM_KEYDOWN, VK["home"], 0, 0.1)
        for _ in range(3):
            post(hwnd, WM_KEYDOWN, VK["right"], 0, 0.05)
        post(hwnd, WM_KEYDOWN, VK["return"], 0, 0.4)
        click(hwnd, q(hwnd, "TEXT_LEFT") + 1, q(hwnd, "BLOCK_Y", 2) + 10, 0.3)
        post(hwnd, WM_KEYDOWN, VK["home"], 0, 0.1)
        for _ in range(3):
            post(hwnd, WM_KEYDOWN, VK["right"], 0, 0.05)
        post(hwnd, WM_KEYDOWN, VK["return"], 0, 0.4)
        cmd(hwnd, "SAVE", 0.4)
        want = "Пер\r\n\r\nвая строка\r\n\r\nВто\n\nрая строка\n\nТретья\n".encode("utf-8")
        ok &= check("edit 2b: in a mixed-EOL file Enter writes the line ending of the line it splits",
                    doc.read_bytes() == want, f"file {doc.read_bytes()!r}")
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.4)
    finally:
        close_edit(proc, hwnd)
    return ok


SEL_DOC = ("# Выделение\n\nПервый абзац со **словом жирным** внутри.\n\n[опора]: https://example.com\n\n"
           "Второй абзац по [опора].\n")


def test_edit_selection():
    """§2.8, §7.9 (2b; the word selection moved here from test_basics, T24): in edit mode a double click selects a word,
    a triple click the paragraph; typing over a bold selection stays bold; deleting across two paragraphs keeps the
    reference definition between them"""
    ok = True
    doc = OUT / "edit-selection.md"
    doc.write_bytes(SEL_DOC.encode("utf-8"))
    set_reg("EditHintShown", 1)
    proc, hwnd = launch_edit(doc, {"FASTMD_AUTOSAVE_MS": "60000"})
    try:
        text = SEL_DOC
        enter_edit(hwnd, 1, dx=1)
        time.sleep(0.7)  # (not a triple click with the entry's double click)
        x, y = q(hwnd, "TEXT_LEFT") + 20, q(hwnd, "BLOCK_Y", 1) + 12
        dbl_click(hwnd, x, y, 0.4)
        i1 = text.index("Первый")
        a, f = q(hwnd, "EDIT_ANCHOR_SRC"), q(hwnd, "EDIT_CARET_SRC")
        ok &= check("edit 2b: a double click in edit mode selects the word", a == i1 and f in (i1 + 6, i1 + 7),
                    f"selection {a}..{f}, word at {i1}")
        time.sleep(0.7)
        for k in range(3):
            post(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp(x, y), 0.01)
            post(hwnd, WM_LBUTTONUP, 0, lp(x, y), 0.01)
        time.sleep(0.4)
        a, f = q(hwnd, "EDIT_ANCHOR_SRC"), q(hwnd, "EDIT_CARET_SRC")
        ok &= check("edit 2b: a triple click selects the paragraph's text",
                    a == i1 and f == text.index("внутри.") + len("внутри."), f"selection {a}..{f}")
        # typing over a selection that starts in bold text stays bold
        post(hwnd, WM_KEYDOWN, VK["home"], 0, 0.1)
        for _ in range(len("Первый абзац со ")):
            post(hwnd, WM_KEYDOWN, VK["right"], 0, 0.03)
        keys(hwnd, [VK["right"]] * len("словом"), shift=True, wait=0.05)
        type_text(hwnd, "делом", 0.4)
        text = text.replace("**словом жирным**", "**делом жирным**", 1)
        ok &= check("edit 2b: typing over a selected bold word keeps it bold", q(hwnd, "SRC_HASH", 0) == src_hash(text),
                    f'len {q(hwnd, "SRC_LEN", 0)} vs {u16(text)}')
        # a selection from the first paragraph into the second, over the reference definition between them
        post(hwnd, WM_KEYDOWN, VK["home"], 0, 0.1)
        for _ in range(5):
            post(hwnd, WM_KEYDOWN, VK["right"], 0, 0.03)
        keys(hwnd, [VK["down"]], shift=True, wait=0.3)
        a, f = q(hwnd, "EDIT_ANCHOR_SRC"), q(hwnd, "EDIT_CARET_SRC")
        spans = a == i1 + 5 and text.index("Второй") < f < text.index("[опора].")
        post(hwnd, WM_KEYDOWN, VK["delete"], 0, 0.4)
        want = text[:a] + text[f:].rstrip("\n") + "\n\n[опора]: https://example.com\n"
        ok &= check("edit 2b: deleting across two paragraphs joins them and keeps the reference definition",
                    spans and q(hwnd, "SRC_HASH", 0) == src_hash(want) and q(hwnd, "EDIT_CARET_SRC") == a,
                    f'selection {a}..{f}, len {q(hwnd, "SRC_LEN", 0)} vs {u16(want)}')
        cmd(hwnd, "SAVE", 0.4)
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.4)
    finally:
        close_edit(proc, hwnd)
    return ok


PASTE_DOC = "# Вставка\n\n> Цитата здесь\n\n| a | b |\n|---|---|\n| c | d |\n\nАбзац для вырезания.\n"


def test_edit_paste_plain():
    """§7.11 (2b): plain text pasted into a quote gets the quote's prefix on every line; into a cell its line ends
    become <br> and a pipe \\|; Cut takes the selection to the clipboard"""
    ok = True
    doc = OUT / "edit-paste.md"
    doc.write_bytes(PASTE_DOC.encode("utf-8"))
    set_reg("EditHintShown", 1)
    proc, hwnd = launch_edit(doc, {"FASTMD_AUTOSAVE_MS": "60000"})
    try:
        text = PASTE_DOC
        enter_edit(hwnd, 1, dx=1)
        post(hwnd, WM_KEYDOWN, VK["end"], 0, 0.1)
        put = set_clipboard("один\r\n\r\nдва")
        cmd(hwnd, "PASTE", 0.4)
        text = text.replace("> Цитата здесь\n", "> Цитата здесьодин\n>\n> два\n", 1)
        ok &= check("edit 2b: a paste into a quote: every line gets the quote's prefix, the file's line ends",
                    put and q(hwnd, "SRC_HASH", 0) == src_hash(text) and q(hwnd, "EDIT_CARET_SRC") == text.index("два") + 3,
                    f'on the clipboard {put}, caret {q(hwnd, "EDIT_CARET_SRC")}, len {q(hwnd, "SRC_LEN", 0)} vs '
                    f'{u16(text)}')
        table = q(hwnd, "BLOCK_COUNT") - 2
        click(hwnd, q(hwnd, "TEXT_LEFT") + 16, q(hwnd, "BLOCK_Y", table) + 14, 0.3)
        post(hwnd, WM_KEYDOWN, VK["end"], 0, 0.2)
        at = q(hwnd, "EDIT_CARET_SRC")
        put = set_clipboard("x|y\nz")
        cmd(hwnd, "PASTE", 0.4)
        want_at = text.index("| a |") + 3
        text = text.replace("| a |", "| ax\\|y<br>z |", 1)
        ok &= check("edit 2b: a paste into a cell: its line end becomes <br>, its pipe \\|",
                    at in (want_at, want_at + 1) and put and q(hwnd, "SRC_HASH", 0) == src_hash(text), f'caret {at} (want {want_at}), '
                    f'on the clipboard {put}, len {q(hwnd, "SRC_LEN", 0)} vs {u16(text)}')
        last = q(hwnd, "BLOCK_COUNT") - 1
        click(hwnd, q(hwnd, "TEXT_LEFT") + 1, q(hwnd, "BLOCK_Y", last) + 10, 0.3)
        post(hwnd, WM_KEYDOWN, VK["home"], 0, 0.1)
        for _ in range(len("Абзац ")):
            post(hwnd, WM_KEYDOWN, VK["right"], 0, 0.03)
        keys(hwnd, [VK["right"]] * len("для "), shift=True, wait=0.05)
        cmd(hwnd, "CUT", 0.4)
        text = text.replace("Абзац для вырезания.", "Абзац вырезания.", 1)
        got = clipboard()
        ok &= check("edit 2b: Cut takes the selection out and puts it on the clipboard",
                    q(hwnd, "SRC_HASH", 0) == src_hash(text) and got == "для ", f"clipboard {got!r}, len "
                    f'{q(hwnd, "SRC_LEN", 0)} vs {u16(text)}')
        cmd(hwnd, "SAVE", 0.4)
        ok &= check("edit 2b: the pastes saved", doc.read_bytes() == text.encode("utf-8"),
                    f"file {doc.read_bytes().decode('utf-8')!r}")
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.4)
    finally:
        close_edit(proc, hwnd)
    return ok


TABLE_DOC = ("Таблица:\n\n| Имя | Число |\n|---|---|\n| а | 1 |\n| б |\n\n"
             "|  | Итого |\n|---|---|\n| 5 | 6 |\n")


def test_edit_table_typing():
    """§7.10, §7.6, §7.8 (2b): a typed pipe is written \\|; typing into a missing cell completes the row; Enter goes
    down a column; Tab selects the next cell's text; Enter on an empty last row takes it away and makes a phantom after
    the table; an empty header cell shows its column's placeholder while editing"""
    ok = True
    doc = OUT / "edit-table.md"
    doc.write_bytes(TABLE_DOC.encode("utf-8"))
    set_reg("EditHintShown", 1)
    proc, hwnd = launch_edit(doc, {"FASTMD_AUTOSAVE_MS": "60000"})
    try:
        text = TABLE_DOC
        enter_edit(hwnd, 0, dx=1)
        post(hwnd, WM_KEYDOWN, VK["home"], 0, 0.1)
        post(hwnd, WM_KEYDOWN, VK["down"], 0, 0.2)
        first = q(hwnd, "EDIT_CARET_SRC") == text.index("Имя")
        post(hwnd, WM_KEYDOWN, VK["tab"], 0, 0.3)
        a, f = q(hwnd, "EDIT_ANCHOR_SRC"), q(hwnd, "EDIT_CARET_SRC")
        ok &= check("edit 2b: Tab in a cell selects the next cell's text",
                    first and a == text.index("Число") and f == a + len("Число"), f"first cell {first}, selection {a}..{f}")
        post(hwnd, WM_KEYDOWN, VK["right"], 0, 0.1)
        post(hwnd, WM_KEYDOWN, VK["return"], 0, 0.3)
        row1 = text.index("| а | 1 |")
        down = q(hwnd, "EDIT_CARET_SRC") == row1 + 6
        type_text(hwnd, "|", 0.3)
        text = text.replace("| а | 1 |", "| а | \\|1 |", 1)
        ok &= check("edit 2b: Enter goes down the column; a typed pipe is written \\|",
                    down and q(hwnd, "SRC_HASH", 0) == src_hash(text), f"down {down}, len "
                    f'{q(hwnd, "SRC_LEN", 0)} vs {u16(text)}')
        post(hwnd, WM_KEYDOWN, VK["down"], 0, 0.2)
        type_text(hwnd, "2", 0.3)
        text = text.replace("| б |\n", "| б | 2 |\n", 1)
        ok &= check("edit 2b: typing into a cell the row lacks completes the row", q(hwnd, "SRC_HASH", 0) == src_hash(text),
                    f'caret {q(hwnd, "EDIT_CARET_SRC")}, len {q(hwnd, "SRC_LEN", 0)} vs {u16(text)}')
        post(hwnd, WM_KEYDOWN, VK["return"], 0, 0.3)
        grown = q(hwnd, "SRC_HASH", 0) == src_hash(text.replace("| б | 2 |\n", "| б | 2 |\n|  |  |\n", 1))
        post(hwnd, WM_KEYDOWN, VK["return"], 0, 0.4)
        style = (q(hwnd, "EDIT_ACTIVE") >> 24) & 0xFF
        ok &= check("edit 2b: Enter on the last row adds a row; on that empty row it goes again, a phantom after the table "
                    "(the style box says «Текст»)", grown and q(hwnd, "SRC_HASH", 0) == src_hash(text) and
                    q(hwnd, "EDIT_PHANTOM", 0) == 1 and active(hwnd, 15) and style == 0,
                    f'grown {grown}, phantom {q(hwnd, "EDIT_PHANTOM", 0)}, style {style}')
        # the second table's empty header cell: «Столбец 1» / "Column 1", muted, only while editing
        cell0 = lambda y: (q(hwnd, "TEXT_LEFT") + 4, y + 6, q(hwnd, "TEXT_LEFT") + 30, y + 32)  # (inside its borders)
        box = cell0(q(hwnd, "BLOCK_Y", 2))
        img = shot(hwnd, "115-edit-column-placeholder")
        shot_dark(hwnd, "115-edit-column-placeholder-dark")
        editing_ink = ink(img, box)
        cmd(hwnd, "SAVE", 0.4)
        ok &= check("edit 2b: the table edits saved", doc.read_bytes() == text.encode("utf-8"))
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.6)
        wait_for(lambda: q(hwnd, "EDIT_BAR") == 0, 2.0)
        time.sleep(0.6)
        click(hwnd, q(hwnd, "TEXT_LEFT") + 1, q(hwnd, "BLOCK_Y", 0) + 10, 0.4)  # (reading mode's caret out of the cell)
        box = cell0(q(hwnd, "BLOCK_Y", 2))  # (the bar and the phantom row went: the table moved up)
        reading_ink = ink(shot(hwnd, "115-edit-column-placeholder-reading"), box)
        ok &= check("edit 2b: an empty header cell shows its column's placeholder while editing, not while reading",
                    editing_ink > 20 and reading_ink < 5, f"ink {editing_ink} editing, {reading_ink} reading")
    finally:
        close_edit(proc, hwnd)
    return ok


RAW_DOC = "# Сырой ввод\n\nПервый абзац.\n\nПоследний абзац.\n"


def test_edit_raw_typing():
    """§6.9 (2b): a picture typed as source stays source while the caret is in it, and becomes a picture once the caret
    leaves; `<!--` typed in a new paragraph stays text (with a warning colour) and the blocks after it still render"""
    ok = True
    doc = OUT / "edit-raw.md"
    doc.write_bytes(RAW_DOC.encode("utf-8"))
    set_reg("EditHintShown", 1)
    proc, hwnd = launch_edit(doc, {"FASTMD_AUTOSAVE_MS": "60000"})
    try:
        text = RAW_DOC
        enter_edit(hwnd, 2, dx=1)
        post(hwnd, WM_KEYDOWN, VK["end"], 0, 0.1)
        post(hwnd, WM_KEYDOWN, VK["return"], 0, 0.3)
        type_text(hwnd, "![x](img/diagram0.png)", 0.5)
        text = text + "\n![x](img/diagram0.png)\n"
        picture = lambda b: q(hwnd, "DRAG", (2 << 16) | b)  # formats of a picture block; 0 for any other block
        raw = q(hwnd, "EDIT_RAW") == 1 and picture(3) == 0 and q(hwnd, "BLOCK_COUNT") == 4
        ok &= check("edit 2b: a picture typed as source stays text while the caret is in it, the caret after the `)`",
                    raw and q(hwnd, "SRC_HASH", 0) == src_hash(text) and q(hwnd, "EDIT_CARET_SRC") == len(text) - 1,
                    f'raw {q(hwnd, "EDIT_RAW")}, picture {picture(3)}, blocks {q(hwnd, "BLOCK_COUNT")}, caret '
                    f'{q(hwnd, "EDIT_CARET_SRC")}, len {q(hwnd, "SRC_LEN", 0)} vs {u16(text)}')
        shot(hwnd, "114-edit-raw-picture")
        shot_dark(hwnd, "114-edit-raw-picture-dark")
        post(hwnd, WM_KEYDOWN, VK["up"], 0, 0.4)
        became = wait_for(lambda: picture(3) & DF_DIB, 5.0)
        ok &= check("edit 2b: the caret leaves it: a picture", q(hwnd, "EDIT_RAW") == 0 and became,
                    f'raw {q(hwnd, "EDIT_RAW")}, picture {picture(3)}')
        # an HTML comment opened in a new paragraph: nothing closes it, yet the rest of the document still renders
        click(hwnd, q(hwnd, "TEXT_LEFT") + 1, q(hwnd, "BLOCK_Y", 1) + 10, 0.3)
        post(hwnd, WM_KEYDOWN, VK["end"], 0, 0.1)
        post(hwnd, WM_KEYDOWN, VK["return"], 0, 0.3)
        type_text(hwnd, "<!--", 0.5)
        text = text.replace("Первый абзац.\n", "Первый абзац.\n\n<!--\n", 1)
        n = q(hwnd, "BLOCK_COUNT")
        ys = [q(hwnd, "BLOCK_Y", k) for k in range(n)]
        ok &= check("edit 2b: `<!--` typed in a new paragraph stays text, and the blocks after it still render",
                    q(hwnd, "EDIT_RAW") == 1 and q(hwnd, "SRC_HASH", 0) == src_hash(text) and n == 5 and
                    q(hwnd, "EDIT_CARET_SRC") == text.index("<!--") + 4 and
                    ys == sorted(ys) and picture(4) != 0, f'raw {q(hwnd, "EDIT_RAW")}, blocks {n} at {ys}, len '
                    f'{q(hwnd, "SRC_LEN", 0)} vs {u16(text)}')
        shot(hwnd, "113-edit-raw-comment")
        shot_dark(hwnd, "113-edit-raw-comment-dark")
        cmd(hwnd, "SAVE", 0.4)
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.4)
    finally:
        close_edit(proc, hwnd)
    return ok


# ------------------------------------------------------------------------------------------------ edit mode, phase 2c
FIND_DOC = ("# Поиск в правке\n\nВступление без искомого.\n\nЕщё вступление.\n\n[Документация проекта](https://example.com/docs)\n\n"
            + "".join(f"Абзац {k}: слово здесь, и текст дальше.\n\n" for k in range(1, 31)))


def find_box_text(hwnd):
    buf = ctypes.create_unicode_buffer(64)
    u32.SendMessageW(q(hwnd, "FIND_EDIT"), WM_GETTEXT, 64, ctypes.cast(buf, ctypes.c_void_p).value)
    return buf.value


def pill_right(hwnd, name, at, away):
    """the right edge of the pill a hover at `at` shows at the bottom left: what changed against a hover at `away`"""
    post(hwnd, WM_MOUSEMOVE, 0, lp(*away), 0.3)
    plain = shot(hwnd, name + "-none")
    post(hwnd, WM_MOUSEMOVE, 0, lp(*at), 0.4)
    img = shot(hwnd, name)
    box = (0, img.height - 28, img.width - 24, img.height - 22)  # (the pill's middle rows: never a toast's)
    diff = ImageChops.difference(plain.crop(box), img.crop(box)).convert("L").point(lambda v: 255 if v > 30 else 0)
    bb = diff.getbbox()
    return bb[2] if bb else 0


def test_edit_find():
    """§12.5, §12.6 (2c, T15): with the find box focused, characters reach the box and the document stays as it is; after
    a click into the document they edit it, the matches follow the text (the current one stays the one it was) and the
    view does not move; the find bar sits under the toolbar. Over a link the pill says how Ctrl+click opens it (UX-19)"""
    ok = True
    doc = OUT / "edit-find.md"
    doc.write_bytes(FIND_DOC.encode("utf-8"))
    set_reg("EditHintShown", 1)
    # (the test hooks keep a posted hover: the link's pill is looked at)
    proc, hwnd = launch_edit(doc, {"FASTMD_AUTOSAVE_MS": "60000", "FASTMD_TEST_HOOKS": "1"})
    try:
        text = FIND_DOC
        enter_edit(hwnd, 1, dx=1)
        cmd(hwnd, "FIND", 0.6)
        type_text(hwnd, "слово", 0.5)  # posted to the document window: the box has the keyboard
        box, matches = find_box_text(hwnd), q(hwnd, "MATCHES")
        ok &= check("edit 2c: with the find box focused, typed characters reach the box, not the document",
                    box == "слово" and matches == 30 and q(hwnd, "SRC_HASH", 0) == src_hash(text) and
                    q(hwnd, "EDIT_DIRTY") == 0, f"box {box!r}, {matches} matches, dirty {q(hwnd, 'EDIT_DIRTY')}")
        shot(hwnd, "116-edit-find")
        shot_dark(hwnd, "116-edit-find-dark")
        r = wt.RECT()
        u32.GetWindowRect(q(hwnd, "FIND_EDIT"), ctypes.byref(r))
        p = wt.POINT(r.left, r.top)
        u32.ScreenToClient(hwnd, ctypes.byref(p))
        bar_bottom = 2 * (q(hwnd, "EDIT_TOOL", CMD["EDIT_EXIT"]) >> 16)  # (the ✕ is centred in the bar)
        ok &= check("edit 2c: the find bar sits under the toolbar", q(hwnd, "EDIT_BAR") == 100 and p.y > bar_bottom > 0,
                    f"the box's top {p.y}, the bar's bottom {bar_bottom}")
        # a click into the document: the keyboard goes there, and typing edits the document
        cur, y0 = q(hwnd, "CUR_MATCH"), q(hwnd, "SCROLLY")
        click(hwnd, q(hwnd, "TEXT_LEFT") + 4, q(hwnd, "BLOCK_Y", 1) + 10, 0.4)
        post(hwnd, WM_KEYDOWN, VK["end"], 0, 0.1)
        type_text(hwnd, " слово", 0.5)
        text = text.replace("Вступление без искомого.", "Вступление без искомого. слово", 1)
        typed = q(hwnd, "SRC_HASH", 0) == src_hash(text)
        m1, c1, y1 = q(hwnd, "MATCHES"), q(hwnd, "CUR_MATCH"), q(hwnd, "SCROLLY")
        ok &= check("edit 2c: after a click into the document, typing edits it; the matches follow, the current one is "
                    "still the one it was (one further on), the view stays", typed and find_box_text(hwnd) == "слово" and
                    m1 == 31 and c1 == cur + 1 and y1 == y0, f"typed {typed}, matches {m1}, current {cur} → {c1}, "
                    f"scroll {y0} → {y1}")
        for _ in range(len(" слово")):
            post(hwnd, WM_KEYDOWN, VK["back"], 0, 0.08)
        time.sleep(0.4)
        text = FIND_DOC
        m2, c2, y2 = q(hwnd, "MATCHES"), q(hwnd, "CUR_MATCH"), q(hwnd, "SCROLLY")
        ok &= check("edit 2c: deleting before the current match keeps it current (its offset shifted with the edit)",
                    q(hwnd, "SRC_HASH", 0) == src_hash(text) and m2 == 30 and c2 == cur and y2 == y0,
                    f"matches {m2}, current {c2} (want {cur}), scroll {y2}")
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.3)  # the find bar first,
        still = q(hwnd, "EDITING") == 1 and q(hwnd, "FIND_OPEN") == 0
        # the link's pill in edit mode says how the link opens, then where it goes: longer than reading mode's
        link = lambda: (q(hwnd, "TEXT_LEFT") + 30, q(hwnd, "BLOCK_Y", 3) + 12)
        away = lambda: (q(hwnd, "TEXT_LEFT") + 30, q(hwnd, "BLOCK_Y", 6) + 12)
        edit_light = pill_right(hwnd, "118-edit-link-tip", link(), away())
        cmd(hwnd, "THEME_DARK", 0.6)
        edit_dark = pill_right(hwnd, "118-edit-link-tip-dark", link(), away())
        cmd(hwnd, "THEME_LIGHT", 0.6)
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.5)  # then edit mode
        ok &= check("edit 2c: Esc closes the find bar first, then leaves edit mode", still and q(hwnd, "EDITING") == 0)
        wait_for(lambda: q(hwnd, "EDIT_BAR") == 0, 2.0)
        reading = pill_right(hwnd, "118-edit-link-tip-reading", link(), away())
        ok &= check("edit 2c: over a link in edit mode the pill says how Ctrl+click opens it, before the address (light "
                    "and dark)", edit_light > reading + 120 and edit_dark > reading + 120 and reading > 60,
                    f"the pill ends at {edit_light} / {edit_dark} px, reading mode's (the address only) at {reading}")
    finally:
        close_edit(proc, hwnd)
    return ok


OUTLINE_DOC = "# Обзор\n\n" + "".join(f"## Раздел {k}\n\nТекст раздела {k} для набора.\n\n" for k in range(1, 31))


def test_edit_outline():
    """§12.7 (2c, R21): while editing the outline keeps its list where the reader left it - no re-centring on entry or
    while typing - and the current heading stays; a heading made by typing joins the list; deleting the only heading
    neither undocks the panel nor moves the column; after leaving the panel follows the headings again"""
    ok = True
    doc = OUT / "edit-outline.md"
    doc.write_bytes(OUTLINE_DOC.encode("utf-8"))
    set_reg("EditHintShown", 1)
    proc, hwnd = launch_edit(doc, {"FASTMD_AUTOSAVE_MS": "60000"})
    try:
        text = OUTLINE_DOC
        if not q(hwnd, "TOC_OPEN"):
            cmd(hwnd, "TOC", 0.8)
        docked, count = q(hwnd, "TOC_DOCKED"), q(hwnd, "TOC_COUNT")
        wheel(hwnd, 100, 400, -3, wait=0.4)  # the reader scrolled the list: its first items out of sight
        y_item = q(hwnd, "TOC_ITEM_Y", 0)
        enter_edit(hwnd, 2, dx=1)
        settle(hwnd)
        cur, y_entry = q(hwnd, "TOC_CURRENT"), q(hwnd, "TOC_ITEM_Y", 0)
        post(hwnd, WM_KEYDOWN, VK["end"], 0, 0.1)
        type_text(hwnd, " и ещё немного", 0.5)
        text = text.replace("Текст раздела 1 для набора.", "Текст раздела 1 для набора. и ещё немного", 1)
        ok &= check("edit 2c: the outline keeps its list where the reader scrolled it on entry and while typing; the "
                    "current heading stays", docked == 1 and y_item < 0 and y_entry == y_item and
                    q(hwnd, "TOC_ITEM_Y", 0) == y_item and q(hwnd, "TOC_CURRENT") == cur and
                    q(hwnd, "SRC_HASH", 0) == src_hash(text), f"docked {docked}, item 0 at {y_item} → {y_entry} → "
                    f"{q(hwnd, 'TOC_ITEM_Y', 0)}, current {cur} → {q(hwnd, 'TOC_CURRENT')}")
        shot(hwnd, "117-edit-outline")
        shot_dark(hwnd, "117-edit-outline-dark")
        # a heading typed at a paragraph's start joins the list; the undo takes it out again
        post(hwnd, WM_KEYDOWN, VK["home"], 0, 0.1)
        type_text(hwnd, "## ", 0.5)
        grown = q(hwnd, "TOC_COUNT") == count + 1 and q(hwnd, "TOC_DOCKED") == 1
        for _ in range(3):
            if q(hwnd, "SRC_HASH", 0) != src_hash(text):
                cmd(hwnd, "UNDO", 0.3)
        ok &= check("edit 2c: a heading made by typing joins the list, the undo takes it out",
                    grown and q(hwnd, "TOC_COUNT") == count and q(hwnd, "SRC_HASH", 0) == src_hash(text),
                    f"grown {grown}, count {q(hwnd, 'TOC_COUNT')} (was {count})")
        cmd(hwnd, "SAVE", 0.4)
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.5)
    finally:
        close_edit(proc, hwnd)
    # the only heading deleted with the keys: the panel stays docked and the column where it was until edit mode ends
    doc = OUT / "edit-outline-one.md"
    doc.write_bytes("# Единственный заголовок\n\nАбзац под ним.\n".encode("utf-8"))
    proc, hwnd = launch_edit(doc, {"FASTMD_AUTOSAVE_MS": "60000"})
    try:
        if not q(hwnd, "TOC_OPEN"):
            cmd(hwnd, "TOC", 0.8)
        docked, left = q(hwnd, "TOC_DOCKED"), q(hwnd, "TEXT_LEFT")
        enter_edit(hwnd, 0, dx=1)
        post(hwnd, WM_KEYDOWN, VK["home"], 0, 0.1)
        post(hwnd, WM_KEYDOWN, VK["back"], 0, 0.5)  # (a heading's start: it becomes a paragraph)
        gone = q(hwnd, "SRC_HASH", 0) == src_hash("Единственный заголовок\n\nАбзац под ним.\n")
        ok &= check("edit 2c: deleting the only heading leaves the outline docked and the column in place",
                    docked == 1 and gone and q(hwnd, "TOC_DOCKED") == 1 and q(hwnd, "TEXT_LEFT") == left and
                    q(hwnd, "TOC_COUNT") == 0, f"docked {docked} → {q(hwnd, 'TOC_DOCKED')}, heading gone {gone}, "
                    f"left {left} → {q(hwnd, 'TEXT_LEFT')}, items {q(hwnd, 'TOC_COUNT')}")
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.6)
        ok &= check("edit 2c: after leaving, the panel follows the headings: none left, it goes",
                    q(hwnd, "EDITING") == 0 and q(hwnd, "TOC_DOCKED") == 0 and q(hwnd, "TEXT_LEFT") != left,
                    f"docked {q(hwnd, 'TOC_DOCKED')}, left {q(hwnd, 'TEXT_LEFT')}")
    finally:
        close_edit(proc, hwnd)
        del_reg("Outline")
    return ok


# ------------------------------------------------------------------------------------------------ edit mode, phase 3a
def cmd_arg(hwnd, name, arg, wait=0.35):
    """WM_COMMAND with its argument in the high word (a table's size, a diagram's template)"""
    post(hwnd, WM_COMMAND, CMD[name] | (arg << 16), 0, wait)


def tool_xy(hwnd, name, row=0):
    """the centre of a bar button, or of a row (from 1) of its open popover - the size grid's cell: rows << 4 | cols"""
    c = q(hwnd, "EDIT_TOOL", CMD[name] | (row << 16))
    return (c & 0xFFFF, (c >> 16) & 0xFFFF) if c > 0 else None


def saved_text(hwnd, doc):
    cmd(hwnd, "SAVE", 0.4)
    return doc.read_bytes().decode("utf-8")


def popover_shots(hwnd, name, menu, row=0):
    """a popover in light and dark: a theme switch closes it (§2.4), so it is opened in each theme; row: the hot one"""
    for theme, suffix in (("THEME_LIGHT", ""), ("THEME_DARK", "-dark")):
        if suffix:
            cmd(hwnd, theme, 0.6)
        cmd(hwnd, menu, 0.4)
        xy = tool_xy(hwnd, menu, row) if row else None
        if xy:
            post(hwnd, WM_MOUSEMOVE, 0, lp(*xy), 0.3)
        shot(hwnd, name + suffix)
        if suffix:
            cmd(hwnd, menu, 0.3)  # (closed again)
            cmd(hwnd, "THEME_LIGHT", 0.6)
    cmd(hwnd, menu, 0.4)  # open, in the light theme, as it was before the pictures


CMD_DOC = ("# Команды\n\nПервый абзац с текстом.\n\nВторой абзац.\n\n| a | b |\n| --- | --- |\n| c | d |\n\n"
           "```\ncode\n```\n")


def select_word(hwnd, words_before):
    """the caret to the paragraph's start, over `words_before` words, then a word selected (Ctrl+Shift+→)"""
    post(hwnd, WM_KEYDOWN, VK["home"], 0, 0.1)
    for _ in range(words_before):
        testkey(hwnd, VK["right"], KM_CTRL, 0.1)
    testkey(hwnd, VK["right"], KM_CTRL | KM_SHIFT, 0.3)


def test_edit_commands():
    """§8.1-§8.7 (3a): every format, block style, list, quote and code-block command - Q_EDIT_ACTIVE shows it on, the
    file gets its Markdown -, a second press takes it off (Ctrl+2 twice returns to text), a pending format for the next
    typed character, the style popover's rows, the commands the context matrix greys change nothing in a cell or code;
    the bar's active states and the style popover in light and dark"""
    ok = True
    doc = OUT / "edit-commands.md"
    doc.write_bytes(CMD_DOC.encode("utf-8"))
    set_reg("EditHintShown", 1)
    proc, hwnd = launch_edit(doc, {"FASTMD_AUTOSAVE_MS": "60000", "FASTMD_TEST_HOOKS": "1"})
    try:
        text = CMD_DOC
        enter_edit(hwnd, 1, dx=1)
        # the inline formats on a selected word: on (active, the bytes), then off again by the undo
        marks = {"FMT_BOLD": ("**", 0), "FMT_ITALIC": ("*", 1), "FMT_STRIKE": ("~~", 2), "FMT_CODE": ("`", 3)}
        for name, (m, bit) in marks.items():
            select_word(hwnd, 1)  # «абзац »
            cmd(hwnd, name, 0.4)
            want = text.replace("Первый абзац с", f"Первый {m}абзац{m} с", 1)
            on = active(hwnd, bit) and q(hwnd, "SRC_HASH", 0) == src_hash(want)
            if name == "FMT_BOLD":
                shot(hwnd, "119-edit-active-bold")
                shot_dark(hwnd, "119-edit-active-bold-dark")
            cmd(hwnd, name, 0.4)  # (the selection kept: every character has it now, so it goes)
            off = not active(hwnd, bit) and q(hwnd, "SRC_HASH", 0) == src_hash(text)
            ok &= check(f"edit 3a: {name} on a selected word writes {m}…{m}, shows it active, and a second press takes "
                        f"it off", on and off, f"on {on}, off {off}, active {q(hwnd, 'EDIT_ACTIVE'):#x}")
        # a pending format: nothing written until the next character, which it wraps
        post(hwnd, WM_KEYDOWN, VK["end"], 0, 0.1)
        type_text(hwnd, " ", 0.2)
        cmd(hwnd, "FMT_BOLD", 0.3)
        pend = active(hwnd, 0) and q(hwnd, "SRC_HASH", 0) == src_hash(text.replace("текстом.", "текстом. ", 1))
        type_text(hwnd, "жирно", 0.4)
        text = text.replace("текстом.", "текстом. **жирно**", 1)
        ok &= check("edit 3a: a toggle with no selection is pending (active, nothing written) and wraps what is typed next",
                    pend and q(hwnd, "SRC_HASH", 0) == src_hash(text) and active(hwnd, 0),
                    f"pending {pend}, len {q(hwnd, 'SRC_LEN', 0)} vs {u16(text)}")
        # block styles: Ctrl+2 twice returns to text; the style id is in Q_EDIT_ACTIVE's high byte
        cmd(hwnd, "BLOCK_H2", 0.4)
        h2 = q(hwnd, "EDIT_ACTIVE") >> 24 == 2 and q(hwnd, "SRC_HASH", 0) == src_hash(text.replace("Первый", "## Первый", 1))
        shot(hwnd, "120-edit-heading2")
        shot_dark(hwnd, "120-edit-heading2-dark")
        cmd(hwnd, "BLOCK_H2", 0.4)
        ok &= check("edit 3a: heading 2 and back (the same level again returns to text)",
                    h2 and q(hwnd, "EDIT_ACTIVE") >> 24 == 0 and q(hwnd, "SRC_HASH", 0) == src_hash(text),
                    f"h2 {h2}, style {q(hwnd, 'EDIT_ACTIVE') >> 24}")
        # lists and the quote: on with their bit, off again with a second press
        for name, prefix, bit in (("LIST_BULLET", "- ", 8), ("LIST_NUMBER", "1. ", 9), ("LIST_TASK", "- [ ] ", 10),
                                  ("QUOTE", "> ", 11)):
            cmd(hwnd, name, 0.4)
            on = active(hwnd, bit) and q(hwnd, "SRC_HASH", 0) == src_hash(text.replace("Первый", prefix + "Первый", 1))
            if name == "LIST_TASK":
                shot(hwnd, "121-edit-active-task")
                shot_dark(hwnd, "121-edit-active-task-dark")
            cmd(hwnd, name, 0.4)
            ok &= check(f"edit 3a: {name} writes «{prefix.strip()}», shows it active, and off again",
                        on and not active(hwnd, bit) and q(hwnd, "SRC_HASH", 0) == src_hash(text),
                        f"on {on}, active {q(hwnd, 'EDIT_ACTIVE'):#x}")
        # the style popover: its rows are buttons (Q_EDIT_TOOL row 3 = Heading 2), the current one marked
        popover_shots(hwnd, "122-edit-style-popover", "BLOCK_MENU", 3)
        row = tool_xy(hwnd, "BLOCK_MENU", 3)
        if row:
            click(hwnd, *row, 0.4)
        ok &= check("edit 3a: the style popover's «Heading 2» row makes a heading 2 (and the popover closes)",
                    row is not None and q(hwnd, "SRC_HASH", 0) == src_hash(text.replace("Первый", "## Первый", 1)) and
                    tool_xy(hwnd, "BLOCK_MENU", 3) is None, f"row {row}")
        cmd(hwnd, "UNDO", 0.3)
        # the popover's keys: ↓ moves the hot row, Enter runs it, Esc closes
        cmd(hwnd, "BLOCK_MENU", 0.3)
        post(hwnd, WM_KEYDOWN, VK["down"], 0, 0.1)  # (from the current «Normal text» to «Heading 1»)
        post(hwnd, WM_KEYDOWN, VK["return"], 0, 0.4)
        keyed = q(hwnd, "SRC_HASH", 0) == src_hash(text.replace("Первый", "# Первый", 1))
        cmd(hwnd, "UNDO", 0.3)
        cmd(hwnd, "BLOCK_MENU", 0.3)
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.3)
        ok &= check("edit 3a: the popover's keys: ↓ and Enter run a row, Esc closes it and stays in edit mode",
                    keyed and tool_xy(hwnd, "BLOCK_MENU", 1) is None and q(hwnd, "EDITING") == 1 and
                    q(hwnd, "SRC_HASH", 0) == src_hash(text), f"keyed {keyed}")
        # the code block: the paragraph wrapped in a fence, and unwrapped again
        click(hwnd, q(hwnd, "TEXT_LEFT") + 4, q(hwnd, "BLOCK_Y", 2) + 10, 0.3)
        cmd(hwnd, "CODEBLOCK", 0.4)
        fenced = active(hwnd, 13) and q(hwnd, "SRC_HASH", 0) == src_hash(text.replace("Второй абзац.", "```\nВторой абзац.\n```", 1))
        cmd(hwnd, "CODEBLOCK", 0.4)
        ok &= check("edit 3a: the code block command wraps the paragraph in a fence, and unwraps it again",
                    fenced and q(hwnd, "SRC_HASH", 0) == src_hash(text), f"fenced {fenced}")
        # in a cell: style, lists and quote are greyed (and do nothing); bold works
        click(hwnd, q(hwnd, "TEXT_LEFT") + 12, q(hwnd, "BLOCK_Y", 3) + 44, 0.3)
        in_cell = active(hwnd, 12)
        for name in ("BLOCK_H1", "LIST_BULLET", "LIST_NUMBER", "QUOTE", "BLOCK_MENU"):
            cmd(hwnd, name, 0.3)
        bullet = tool_xy(hwnd, "LIST_BULLET")
        if bullet:  # a greyed button's tooltip says why (the test hooks keep a posted hover)
            post(hwnd, WM_MOUSEMOVE, 0, lp(*bullet), 0.3)
        shot(hwnd, "123-edit-cell-disabled")
        shot_dark(hwnd, "123-edit-cell-disabled-dark")
        post(hwnd, WM_MOUSEMOVE, 0, lp(500, 600), 0.2)
        ok &= check("edit 3a: in a table cell the block commands are disabled: nothing changes",
                    in_cell and q(hwnd, "SRC_HASH", 0) == src_hash(text) and tool_xy(hwnd, "BLOCK_MENU", 1) is None,
                    f"in a cell {in_cell}")
        # in code: the inline formats are disabled
        click(hwnd, q(hwnd, "TEXT_LEFT") + 30, q(hwnd, "BLOCK_Y", 4) + 20, 0.3)
        in_code = active(hwnd, 13)
        for name in ("FMT_BOLD", "FMT_ITALIC", "LIST_TASK"):
            cmd(hwnd, name, 0.3)
        ok &= check("edit 3a: in a code block the inline formats and lists are disabled: nothing changes",
                    in_code and q(hwnd, "SRC_HASH", 0) == src_hash(text), f"in code {in_code}")
        ok &= check("edit 3a: saved as it reads", saved_text(hwnd, doc) == text)
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.4)
    finally:
        close_edit(proc, hwnd)
    return ok


TABLE_INS_DOC = "# Таблицы\n\nАбзац перед таблицей.\n\nАбзац после.\n"


def test_edit_table():
    """§8.8, §8.9 (3a): CMD_INS_TABLE with a size in its argument; the size grid's cells as buttons (Q_EDIT_TOOL); in a
    table the actions popover, and every CMD_TABLE_* on the bytes; the popovers in light and dark"""
    ok = True
    doc = OUT / "edit-table.md"
    doc.write_bytes(TABLE_INS_DOC.encode("utf-8"))
    set_reg("EditHintShown", 1)
    proc, hwnd = launch_edit(doc, {"FASTMD_AUTOSAVE_MS": "60000", "FASTMD_TEST_HOOKS": "1"})
    try:
        text = TABLE_INS_DOC
        enter_edit(hwnd, 1, dx=1)
        cmd_arg(hwnd, "INS_TABLE", 2 << 4 | 3)  # 2 rows (the header's included), 3 columns
        t23 = "|  |  |  |\n| --- | --- | --- |\n|  |  |  |\n"
        want = text.replace("таблицей.\n", "таблицей.\n\n" + t23, 1)
        ok &= check("edit 3a: CMD_INS_TABLE 2×3 after the paragraph, the caret in its first header cell",
                    q(hwnd, "SRC_HASH", 0) == src_hash(want) and active(hwnd, 12) and
                    q(hwnd, "EDIT_CARET_SRC") == want.index(t23) + 2, f"caret {q(hwnd, 'EDIT_CARET_SRC')}")
        cmd(hwnd, "UNDO", 0.4)
        # the size grid: its cell 3 rows × 4 columns is a button
        click(hwnd, q(hwnd, "TEXT_LEFT") + 4, q(hwnd, "BLOCK_Y", 1) + 10, 0.3)
        popover_shots(hwnd, "124-edit-table-grid", "TABLE_MENU", 3 << 4 | 4)
        cell = tool_xy(hwnd, "TABLE_MENU", 3 << 4 | 4)
        if cell:
            click(hwnd, *cell, 0.5)
        t34 = "|  |  |  |  |\n| --- | --- | --- | --- |\n|  |  |  |  |\n|  |  |  |  |\n"
        text = text.replace("таблицей.\n", "таблицей.\n\n" + t34, 1)
        ok &= check("edit 3a: the size grid's cell 3 × 4 inserts a table of 3 rows and 4 columns", cell is not None and
                    q(hwnd, "SRC_HASH", 0) == src_hash(text), f"cell {cell}, len {q(hwnd, 'SRC_LEN', 0)} vs {u16(text)}")
        # a little text in it: the header's first cell
        type_text(hwnd, "A", 0.3)
        text = text.replace("|  |  |  |  |\n| ---", "| A |  |  |  |\n| ---", 1)
        # in the table: the actions popover, a row of it clicked (insert a row below the header)
        popover_shots(hwnd, "125-edit-table-actions", "TABLE_MENU", 2)
        row = tool_xy(hwnd, "TABLE_MENU", 2)
        listed = tool_xy(hwnd, "TABLE_MENU", 10) is not None and tool_xy(hwnd, "TABLE_MENU", 11) is None
        if row:
            click(hwnd, *row, 0.5)
        header, delim = "| A |  |  |  |\n", "| --- | --- | --- | --- |\n"
        body = "|  |  |  |  |\n"
        text = text.replace(header + delim, header + delim + body, 1)
        ok &= check("edit 3a: the actions popover lists its ten rows; «Insert row below» on the header row: a row after "
                    "the delimiter row", row is not None and listed and q(hwnd, "SRC_HASH", 0) == src_hash(text),
                    f"row {row}, listed {listed}, len {q(hwnd, 'SRC_LEN', 0)} vs {u16(text)}")
        # every table operation on the bytes, the caret in the first body row's first cell
        def table_now():
            s = saved_text(hwnd, doc)
            a = s.index("таблицей.\n\n") + len("таблицей.\n\n")
            return s[a:s.index("\n\nАбзац после")]
        post(hwnd, WM_KEYDOWN, VK["down"], 0, 0.3)  # (into the first body row)
        by = {}
        for name in ("TABLE_ROW_ABOVE", "TABLE_DEL_ROW", "TABLE_COL_RIGHT", "TABLE_COL_LEFT", "TABLE_ALIGN_C",
                     "TABLE_ALIGN_R", "TABLE_ALIGN_L", "TABLE_DEL_COL", "TABLE_ROW_BELOW"):
            before = table_now()
            cmd(hwnd, name, 0.4)
            by[name] = (before, table_now())
        rows_of = lambda t: t.split("\n")
        ok &= check("edit 3a: row above adds a row before the caret's, delete row takes one away",
                    len(rows_of(by["TABLE_ROW_ABOVE"][1])) == len(rows_of(by["TABLE_ROW_ABOVE"][0])) + 1 and
                    by["TABLE_DEL_ROW"][1] == by["TABLE_ROW_ABOVE"][0], f"{by['TABLE_ROW_ABOVE']!r}")
        cols = lambda t: [r.count("|") - 1 for r in rows_of(t)]
        ok &= check("edit 3a: column right / left add a column in every row, delete column takes one away",
                    all(c == 5 for c in cols(by["TABLE_COL_RIGHT"][1])) and all(c == 6 for c in cols(by["TABLE_COL_LEFT"][1]))
                    and all(c == 5 for c in cols(by["TABLE_DEL_COL"][1])), f"{cols(by['TABLE_COL_LEFT'][1])}")
        d_cell = lambda t: rows_of(t)[1].split("|")[2].strip()  # (the caret's column: the second, after column left)
        ok &= check("edit 3a: centre / right / left alignment rewrite the caret's column in the delimiter row, its width kept",
                    d_cell(by["TABLE_ALIGN_C"][1]) == ":---:" and d_cell(by["TABLE_ALIGN_R"][1]) == "----:" and
                    d_cell(by["TABLE_ALIGN_L"][1]) == ":----", f"{d_cell(by['TABLE_ALIGN_C'][1])!r} "
                    f"{d_cell(by['TABLE_ALIGN_R'][1])!r} {d_cell(by['TABLE_ALIGN_L'][1])!r}")
        ok &= check("edit 3a: row below adds a row after the caret's",
                    len(rows_of(by["TABLE_ROW_BELOW"][1])) == len(rows_of(by["TABLE_ROW_BELOW"][0])) + 1)
        cmd(hwnd, "TABLE_DEL", 0.5)
        s1 = saved_text(hwnd, doc)
        ok &= check("edit 3a: delete table takes all its lines, the paragraphs around stay apart",
                    s1 == TABLE_INS_DOC and not active(hwnd, 12), f"{s1!r}")
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.4)
    finally:
        close_edit(proc, hwnd)
    return ok


HR_DOC = "# Вставки\n\nАбзац.\n\nПоследний абзац.\n"


def test_edit_hr():
    """§8.8 (3a, UX-3): CMD_INS_HR puts a rule after the block and a phantom row after the rule with the caret in it;
    typing there writes a paragraph after the rule. The formula and diagram inserts write their source (their popups are
    3b's); every diagram template renders; the formula and diagram popovers and the "…" popover in light and dark"""
    ok = True
    doc = OUT / "edit-hr.md"
    doc.write_bytes(HR_DOC.encode("utf-8"))
    set_reg("EditHintShown", 1)
    proc, hwnd = launch_edit(doc, {"FASTMD_AUTOSAVE_MS": "60000", "FASTMD_TEST_HOOKS": "1"})
    try:
        text = HR_DOC
        enter_edit(hwnd, 1, dx=1)
        blocks = q(hwnd, "BLOCK_COUNT")
        cmd(hwnd, "INS_HR", 0.5)
        want = text.replace("Абзац.\n", "Абзац.\n\n---\n", 1)
        ok &= check("edit 3a: CMD_INS_HR: a rule after the block, a phantom row after it with the caret in it",
                    q(hwnd, "SRC_HASH", 0) == src_hash(want) and q(hwnd, "EDIT_PHANTOM", 0) == 2 and active(hwnd, 15) and
                    q(hwnd, "BLOCK_COUNT") == blocks + 1, f"phantom {q(hwnd, 'EDIT_PHANTOM', 0)}, blocks "
                    f"{q(hwnd, 'BLOCK_COUNT')}")
        shot(hwnd, "126-edit-hr-phantom")
        shot_dark(hwnd, "126-edit-hr-phantom-dark")
        type_text(hwnd, "После линии", 0.4)
        text = text.replace("Абзац.\n", "Абзац.\n\n---\n\nПосле линии\n", 1)
        ok &= check("edit 3a: typing in the phantom after the rule writes a paragraph after it",
                    q(hwnd, "SRC_HASH", 0) == src_hash(text), f"len {q(hwnd, 'SRC_LEN', 0)} vs {u16(text)}")
        # a styled phantom: Enter at a paragraph's end, heading 2 - the placeholder in the row
        post(hwnd, WM_KEYDOWN, VK["end"], 0, 0.1)
        post(hwnd, WM_KEYDOWN, VK["return"], 0, 0.3)
        cmd(hwnd, "BLOCK_H2", 0.4)
        styled = q(hwnd, "EDIT_PHANTOM", 2) == 2 and q(hwnd, "EDIT_ACTIVE") >> 24 == 2
        shot(hwnd, "127-edit-phantom-heading")
        shot_dark(hwnd, "127-edit-phantom-heading-dark")
        cmd(hwnd, "QUOTE", 0.4)
        quoted = q(hwnd, "EDIT_PHANTOM", 2) == 10 and active(hwnd, 11)  # (style 10: the quote, §6.7)
        shot(hwnd, "127-edit-phantom-quote")
        shot_dark(hwnd, "127-edit-phantom-quote-dark")
        cmd(hwnd, "LIST_TASK", 0.4)
        shot(hwnd, "127-edit-phantom-task")
        shot_dark(hwnd, "127-edit-phantom-task-dark")
        type_text(hwnd, "дело", 0.4)
        text = text.replace("После линии\n", "После линии\n\n- [ ] дело\n", 1)
        ok &= check("edit 3a: a phantom takes a style (heading 2, a quote, then a task) and typing writes it",
                    styled and quoted and q(hwnd, "SRC_HASH", 0) == src_hash(text), f"styled {styled}, quoted {quoted}")
        # the formula popover; an inline formula at the caret
        click(hwnd, q(hwnd, "TEXT_LEFT") + 4, q(hwnd, "BLOCK_Y", 1) + 10, 0.3)
        post(hwnd, WM_KEYDOWN, VK["end"], 0, 0.1)
        type_text(hwnd, " x", 0.3)
        popover_shots(hwnd, "128-edit-formula-popover", "FORMULA_MENU", 1)
        row = tool_xy(hwnd, "FORMULA_MENU", 1)
        if row:
            click(hwnd, *row, 0.5)
        text = text.replace("Абзац.\n", "Абзац. x $x$\n", 1)  # (a blank apart from the letter before it, F9-4)
        ok &= check("edit 3a: the formula popover's «Inline» writes $x$ at the caret", row is not None and
                    q(hwnd, "SRC_HASH", 0) == src_hash(text), f"len {q(hwnd, 'SRC_LEN', 0)} vs {u16(text)}")
        cmd(hwnd, "INS_FORMULA_BLOCK", 0.5)
        text = text.replace("Абзац. x $x$\n", "Абзац. x $x$\n\n$$\nx\n$$\n", 1)
        ok &= check("edit 3a: CMD_INS_FORMULA_BLOCK writes $$ x $$ after the block, the new formula selected",
                    q(hwnd, "SRC_HASH", 0) == src_hash(text) and q(hwnd, "EDIT_ATOM") >= 0, f"atom {q(hwnd, 'EDIT_ATOM')}")
        # the diagram popover, then every template
        popover_shots(hwnd, "129-edit-diagram-popover", "DIAGRAM_MENU", 4)
        cmd(hwnd, "DIAGRAM_MENU", 0.3)  # (a second press closes it)
        closed = tool_xy(hwnd, "DIAGRAM_MENU", 1) is None
        for k in range(9):
            cmd_arg(hwnd, "INS_DIAGRAM", k, 0.4)
        s = saved_text(hwnd, doc)
        n = s.count("```mermaid\n")
        ok &= check("edit 3a: the diagram popover closes on a second press; CMD_INS_DIAGRAM writes the nine templates",
                    closed and n == 9 and "flowchart TD\n    A[Start] --> B{Choice}" in s and "timeline\n    title History" in s,
                    f"closed {closed}, {n} mermaid blocks")
        rendered = wait_for(lambda: q(hwnd, "MATH", 1) + q(hwnd, "MATH", 2) >= q(hwnd, "MATH", 0) > 0, 20.0, 0.2)
        ok &= check("edit 3a: every diagram template renders (none failed)", rendered and q(hwnd, "MATH", 2) == 0,
                    f"math {q(hwnd, 'MATH', 0)}, drawn {q(hwnd, 'MATH', 1)}, failed {q(hwnd, 'MATH', 2)}")
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.4)
    finally:
        close_edit(proc, hwnd)
    # a narrow window: the "…" popover holds what the bar has no room for
    proc, hwnd = launch_edit(doc, {"FASTMD_AUTOSAVE_MS": "60000", "FASTMD_TEST_HOOKS": "1"}, size="--size=430x600")
    try:
        enter_edit(hwnd, 1, dx=1)
        popover_shots(hwnd, "130-edit-more-popover", "EDIT_MORE", 1)
        row = tool_xy(hwnd, "EDIT_MORE", 1)
        ok &= check("edit 3a: in a narrow window the «…» popover lists the collapsed buttons", q(hwnd, "EDIT_COLLAPSE") >= 3
                    and row is not None and tool_xy(hwnd, "EDIT_MORE", 6) is not None, f"collapse {q(hwnd, 'EDIT_COLLAPSE')}")
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.3)
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.4)
    finally:
        close_edit(proc, hwnd)
    return ok


# ------------------------------------------------------------------------------------------------ edit mode, phase 3b
EM_SETSEL, EM_REPLACESEL, WM_DROPFILES = 0x00B1, 0x00C2, 0x0233
POPUP = {"NONE": 0, "OK": 1, "ERROR": 2, "PENDING": 3}


def popup_set(hwnd, text, field=0):
    """the popup field's text replaced as if typed (§9.2, T23): EM_SETSEL(0, -1) + EM_REPLACESEL raise EN_CHANGE"""
    e = q(hwnd, "EDIT_POPUP", field)
    if not e:
        return False
    u32.SendMessageW(e, EM_SETSEL, 0, -1)
    buf = ctypes.create_unicode_buffer(text.replace("\n", "\r\n"))
    u32.SendMessageW(e, EM_REPLACESEL, 1, ctypes.addressof(buf))
    return True


def popup_text(hwnd, field=0):
    e = q(hwnd, "EDIT_POPUP", field)
    if not e:
        return None
    buf = ctypes.create_unicode_buffer(4096)
    u32.SendMessageW(e, WM_GETTEXT, 4096, ctypes.addressof(buf))
    return buf.value.replace("\r\n", "\n")


def popup_open(hwnd, timeout=3.0):
    return wait_for(lambda: q(hwnd, "EDIT_POPUP", 0) != 0, timeout, 0.05)


def popup_settled(hwnd, timeout=8.0):
    """the popup's text is in the source and its picture has arrived (or failed)"""
    return wait_for(lambda: q(hwnd, "EDIT_POPUP_STATE") in (POPUP["OK"], POPUP["ERROR"]) and not q(hwnd, "EDIT_BUSY") & (16 | 256),
                    timeout, 0.05)


def clipboard_formats(items):
    """several formats on the clipboard at once, as another program (or FastMD) would put them: [(format, bytes)]"""
    owner = clip_owner()
    for _ in range(20):
        if u32.OpenClipboard(owner):
            break
        time.sleep(0.05)
    else:
        return False
    try:
        u32.EmptyClipboard()
        for fmt, data in items:
            h = k32.GlobalAlloc(0x0002, len(data))
            p = k32.GlobalLock(h)
            ctypes.memmove(p, data, len(data))
            k32.GlobalUnlock(h)
            u32.SetClipboardData(fmt, h)
        return True
    finally:
        u32.CloseClipboard()


def hdrop(files, x=0, y=0):
    """a DROPFILES block (CF_HDROP, WM_DROPFILES): the header, then the paths, wide, NUL-separated"""
    import struct
    return struct.pack("<IiiII", 20, x, y, 0, 1) + ("".join(str(f) + "\0" for f in files) + "\0").encode("utf-16-le")


def clipboard_wide(name):
    """a registered clipboard format holding UTF-16 text (FastMD's own "FastMD Markdown")"""
    fmt = u32.RegisterClipboardFormatW(name)
    if not fmt or not open_clipboard():
        return ""
    try:
        h = u32.GetClipboardData(fmt)
        if not h:
            return ""
        p = k32.GlobalLock(h)
        s = ctypes.wstring_at(p)
        k32.GlobalUnlock(h)
        return s
    finally:
        u32.CloseClipboard()


def toast_said(hwnd, ru, en):
    """the last toast's text, in the UI's language (Q_LAST_PROMPT lp 2: its hash)"""
    return q(hwnd, "LAST_PROMPT", 2) & 0xFFFFFFFF == src_hash(en if q(hwnd, "LANG") == 1 else ru)


def popover_pair(hwnd, name, opener):
    """a popover in dark and light: a theme switch closes it (§2.4), so it opens in each theme; it is left open, light"""
    cmd(hwnd, "THEME_DARK", 0.6)
    cmd(hwnd, opener, 0.5)
    popup_open(hwnd)
    shot(hwnd, name + "-dark")
    cmd(hwnd, "POPUP_CANCEL", 0.3)
    cmd(hwnd, "THEME_LIGHT", 0.6)
    cmd(hwnd, opener, 0.5)
    ok = popup_open(hwnd)
    shot(hwnd, name)
    return ok


def atom_after(hwnd, block, chars):
    """the caret to a paragraph's start, over `chars` characters (an object in the line counts one), then Backspace: the
    object before the caret selected; returns the caret's point before the Backspace (the object's right edge)"""
    click(hwnd, q(hwnd, "TEXT_LEFT") + 2, q(hwnd, "BLOCK_Y", block) + 10, 0.2)
    post(hwnd, WM_KEYDOWN, VK["home"], 0, 0.1)
    for _ in range(chars):
        post(hwnd, WM_KEYDOWN, VK["right"], 0, 0.03)
    time.sleep(0.2)
    xy = caret_xy(hwnd)
    post(hwnd, WM_KEYDOWN, VK["back"], 0, 0.3)
    return xy


POPUP_DOC = ("# Всплывающие окна\n\nФормула в строке $a+b$ тут.\n\n$$\nx^2\n$$\n\n```mermaid\ngraph TD\n    A --> B\n```\n\n"
             "<div>HTML блок</div>\n\nКартинка ![кот](img/diagram0.png) в строке.\n\n" +
             "".join(f"Абзац {k} для прокрутки документа вниз.\n\n" for k in range(40)) + "Последний абзац.\n")


def test_edit_popups():
    """§9, §2.10 (3b): a click on a formula opens its source popup; its text re-renders the formula (the preview
    worker: Q_RENDERS + 1); broken TeX shows the error and keeps the last good picture outlined (Q_MATH lp 3); Esc puts
    the text back with no undo step; Ctrl+Enter keeps it as one; an emptied formula goes with its $…$; every diagram
    template renders; a picture's popup edits its alt text and path; a wheel scroll that hides the object closes the
    popup and keeps its text; WM_CLOSE right after a change saves it (D20); the popups in light and dark"""
    ok = True
    doc = OUT / "edit-popups.md"
    doc.write_bytes(POPUP_DOC.encode("utf-8"))
    set_reg("EditHintShown", 1)
    proc, hwnd = launch_edit(doc, {"FASTMD_AUTOSAVE_MS": "60000", "FASTMD_TEST_HOOKS": "1"})
    try:
        text = POPUP_DOC
        enter_edit(hwnd, 1, dx=1)
        wait_for(lambda: q(hwnd, "MATH", 1) >= 3, 10.0, 0.1)
        # the formula's box: right of it the caret stands after "Формула в строке " and the formula
        xy = atom_after(hwnd, 1, 18)
        selected = q(hwnd, "EDIT_ATOM") == 0
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.2)  # (deselected: the click selects it again, and opens it)
        if xy:
            click(hwnd, xy[0] - 6, xy[1], 0.3)
        opened = popup_open(hwnd)
        ok &= check("edit 3b: a click on a formula opens its source popup (Q_EDIT_POPUP), showing the TeX",
                    selected and opened and popup_text(hwnd) == "a+b" and q(hwnd, "EDIT_POPUP_STATE") == POPUP["OK"],
                    f"selected {selected}, popup {q(hwnd, 'EDIT_POPUP', 0):#x}, text {popup_text(hwnd)!r}")
        shot(hwnd, "131-edit-popup-formula")
        shot_dark(hwnd, "131-edit-popup-formula-dark")
        r0, undo0 = q(hwnd, "RENDERS"), q(hwnd, "UNDO_DEPTH")
        popup_set(hwnd, "a+b+c")
        settled = popup_settled(hwnd)
        ok &= check("edit 3b: the popup's text goes into the source and the preview worker renders it (Q_RENDERS + 1, state 1)",
                    settled and q(hwnd, "RENDERS") == r0 + 1 and q(hwnd, "SRC_HASH", 0) == src_hash(text.replace("$a+b$", "$a+b+c$", 1))
                    and q(hwnd, "EDIT_POPUP_STATE") == POPUP["OK"], f"renders +{q(hwnd, 'RENDERS') - r0}, state {q(hwnd, 'EDIT_POPUP_STATE')}")
        popup_set(hwnd, "\\frac{")
        popup_settled(hwnd)
        broken = q(hwnd, "EDIT_POPUP_STATE") == POPUP["ERROR"] and q(hwnd, "MATH", 3) == 1
        shot(hwnd, "132-edit-popup-error")
        shot_dark(hwnd, "132-edit-popup-error-dark")
        settle(hwnd)
        kept = q(hwnd, "MATH", 3) == 1 and q(hwnd, "MATH", 2) == 0 and q(hwnd, "EDIT_POPUP_STATE") == POPUP["ERROR"]
        ok &= check("edit 3b: broken TeX: the error line (state 2), the last good picture kept and outlined (Q_MATH lp 3 == 1), "
                    "through a theme switch there and back", broken and kept,
                    f"broken {broken}, then state {q(hwnd, 'EDIT_POPUP_STATE')}, stale {q(hwnd, 'MATH', 3)}, failed {q(hwnd, 'MATH', 2)}")
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.5)  # (the document's Esc: the popup cancels, as its own Esc does)
        ok &= check("edit 3b: Esc puts back the text the popup opened with, and leaves no undo step",
                    q(hwnd, "EDIT_POPUP", 0) == 0 and q(hwnd, "SRC_HASH", 0) == src_hash(text) and q(hwnd, "UNDO_DEPTH") == undo0,
                    f"popup {q(hwnd, 'EDIT_POPUP', 0)}, undo {q(hwnd, 'UNDO_DEPTH')} vs {undo0}")
        # its own Esc (the key the popup's field gets), then Ctrl+Enter's command keeps the text as one step
        atom_after(hwnd, 1, 18)
        cmd(hwnd, "ATOM_EDIT", 0.4)
        popup_open(hwnd)
        popup_set(hwnd, "y")
        post(q(hwnd, "EDIT_POPUP", 0), WM_KEYDOWN, VK["esc"], 0, 0.5)
        own_esc = q(hwnd, "EDIT_POPUP", 0) == 0 and q(hwnd, "SRC_HASH", 0) == src_hash(text)
        atom_after(hwnd, 1, 18)
        post(hwnd, WM_KEYDOWN, VK["return"], 0, 0.4)  # (Enter on a selected object opens its source, §2.7)
        popup_open(hwnd)
        popup_set(hwnd, "c")
        popup_settled(hwnd)
        cmd(hwnd, "POPUP_DONE", 0.4)
        text = text.replace("$a+b$", "$c$", 1)
        ok &= check("edit 3b: the field's Esc cancels; Enter opens it again, Ctrl+Enter keeps the text as one undo step, the "
                    "caret after the formula", own_esc and q(hwnd, "EDIT_POPUP", 0) == 0 and q(hwnd, "SRC_HASH", 0) == src_hash(text)
                    and q(hwnd, "UNDO_DEPTH") == undo0 + 1 and q(hwnd, "EDIT_ATOM") == -1,
                    f"own Esc {own_esc}, undo {q(hwnd, 'UNDO_DEPTH')}, atom {q(hwnd, 'EDIT_ATOM')}")
        # a click beside the popup (on the heading) keeps its text, as one undo step
        atom_after(hwnd, 1, 18)
        cmd(hwnd, "ATOM_EDIT", 0.4)
        popup_open(hwnd)
        popup_set(hwnd, "d")
        popup_settled(hwnd)
        click(hwnd, q(hwnd, "TEXT_LEFT") + 30, q(hwnd, "BLOCK_Y", 0) + 14, 0.5)
        beside = q(hwnd, "EDIT_POPUP", 0) == 0 and q(hwnd, "SRC_HASH", 0) == src_hash(text.replace("$c$", "$d$", 1)) and \
            q(hwnd, "UNDO_DEPTH") == undo0 + 2
        cmd(hwnd, "UNDO", 0.4)
        ok &= check("edit 3b: a click beside the popup keeps its text as one undo step", beside and
                    q(hwnd, "SRC_HASH", 0) == src_hash(text), f"undo {q(hwnd, 'UNDO_DEPTH')}")
        # an inline formula emptied goes with its $…$ and one blank
        atom_after(hwnd, 1, 18)
        cmd(hwnd, "ATOM_EDIT", 0.4)
        popup_open(hwnd)
        popup_set(hwnd, "")
        time.sleep(0.3)
        cmd(hwnd, "POPUP_DONE", 0.5)
        gone = q(hwnd, "SRC_HASH", 0) == src_hash(text.replace("строке $c$ тут", "строке тут", 1))
        cmd(hwnd, "UNDO", 0.4)
        ok &= check("edit 3b: an inline formula emptied in its popup goes, with its $…$ and one blank; undo brings it back",
                    gone and q(hwnd, "SRC_HASH", 0) == src_hash(text), f"gone {gone}")
        # a diagram's popup (the block after the display formula)
        click(hwnd, q(hwnd, "TEXT_LEFT") + 30, q(hwnd, "BLOCK_Y", 3) + 30, 0.5)
        dia = popup_open(hwnd) and popup_text(hwnd) == "graph TD\n    A --> B"
        shot(hwnd, "133-edit-popup-diagram")
        shot_dark(hwnd, "133-edit-popup-diagram-dark")
        popup_set(hwnd, "graph LR\n    A --> B")
        popup_settled(hwnd)
        cmd(hwnd, "POPUP_DONE", 0.4)
        text = text.replace("graph TD\n    A --> B", "graph LR\n    A --> B", 1)
        ok &= check("edit 3b: a click on a diagram opens its lines; the change is written back", dia and
                    q(hwnd, "SRC_HASH", 0) == src_hash(text), f"dia {dia}, len {q(hwnd, 'SRC_LEN', 0)} vs {u16(text)}")
        # the picture's popup: alt text and path, each a field
        atom_after(hwnd, 5, 10)
        cmd(hwnd, "ATOM_EDIT", 0.4)
        pic = popup_open(hwnd) and popup_text(hwnd, 0) == "кот" and popup_text(hwnd, 1) == "img/diagram0.png"
        shot(hwnd, "134-edit-popup-image")
        shot_dark(hwnd, "134-edit-popup-image-dark")
        popup_set(hwnd, "собака", 0)
        popup_set(hwnd, "img/Новая картинка.png", 1)
        popup_settled(hwnd)
        cmd(hwnd, "POPUP_DONE", 0.4)
        text = text.replace("![кот](img/diagram0.png)", "![собака](<img/Новая картинка.png>)", 1)
        ok &= check("edit 3b: a picture's popup edits its alt text and its path (a path with blanks in <…>)",
                    pic and q(hwnd, "SRC_HASH", 0) == src_hash(text), f"pic {pic}, len {q(hwnd, 'SRC_LEN', 0)} vs {u16(text)}")
        cmd(hwnd, "UNDO", 0.4)
        text = text.replace("![собака](<img/Новая картинка.png>)", "![кот](img/diagram0.png)", 1)
        # the HTML block's popup
        click(hwnd, q(hwnd, "TEXT_LEFT") + 30, q(hwnd, "BLOCK_Y", 4) + 8, 0.5)
        html = popup_open(hwnd) and popup_text(hwnd) == "<div>HTML блок</div>"
        cmd(hwnd, "POPUP_CANCEL", 0.3)
        ok &= check("edit 3b: a click on an HTML block opens its source", html, f"{popup_text(hwnd)!r}")
        # a wheel scroll that takes the object out of sight closes the popup, keeping its text
        atom_after(hwnd, 1, 18)
        cmd(hwnd, "ATOM_EDIT", 0.4)
        popup_open(hwnd)
        popup_set(hwnd, "keep")
        time.sleep(0.3)
        for _ in range(6):
            wheel(hwnd, 400, 400, -3, wait=0.15)
        closed = wait_for(lambda: q(hwnd, "EDIT_POPUP", 0) == 0, 3.0, 0.1)
        text = text.replace("$c$", "$keep$", 1)
        ok &= check("edit 3b: a wheel scroll that hides the formula closes its popup and keeps the text, still editing",
                    closed and q(hwnd, "SRC_HASH", 0) == src_hash(text) and q(hwnd, "EDITING") == 1,
                    f"closed {closed}, len {q(hwnd, 'SRC_LEN', 0)} vs {u16(text)}")
        ok &= check("edit 3b: saved as it reads", saved_text(hwnd, doc) == text)
        # every diagram template, inserted at the end with its popup kept, is drawn
        testkey(hwnd, VK["end"], KM_CTRL, 0.3)
        for k in range(9):
            cmd_arg(hwnd, "INS_DIAGRAM", k, 0.4)
            popup_open(hwnd)
            cmd(hwnd, "POPUP_DONE", 0.3)
        drawn = wait_for(lambda: q(hwnd, "MATH", 0) == 12 and q(hwnd, "MATH", 1) == 12, 15.0, 0.2)
        ok &= check("edit 3b: the nine diagram templates, inserted, are drawn (Q_MATH lp 2 == 0)",
                    drawn and q(hwnd, "MATH", 2) == 0, f"{q(hwnd, 'MATH', 0)} objects, {q(hwnd, 'MATH', 1)} drawn, "
                    f"{q(hwnd, 'MATH', 2)} failed")
        for _ in range(9):
            cmd(hwnd, "UNDO", 0.2)
        ok &= check("edit 3b: ... and undone", saved_text(hwnd, doc) == text)
        cmd(hwnd, "EDIT_EXIT", 0.5)
    finally:
        close_edit(proc, hwnd)
    # D20: a change the popup holds when the window closes 50 ms later is saved
    proc, hwnd = launch_edit(doc, {"FASTMD_AUTOSAVE_MS": "60000"})
    try:
        enter_edit(hwnd, 1, dx=1)
        atom_after(hwnd, 1, 18)
        cmd(hwnd, "ATOM_EDIT", 0.4)
        popup_open(hwnd)
        popup_set(hwnd, "zz")
        post(hwnd, WM_CLOSE, 0, 0, 0.05)
        try:
            proc.wait(5)
        except subprocess.TimeoutExpired:
            pass
        ok &= check("edit 3b: WM_CLOSE 50 ms after a change in the popup saves it (D20), with exit code 0",
                    proc.poll() == 0 and "$zz$" in doc.read_bytes().decode("utf-8"), f"rc {proc.poll()}")
    finally:
        close_edit(proc, hwnd)
    # the front matter's popup: longer than the field, so it scrolls (dark scrollbars in the dark theme); a --- line
    # would end the block, so it is refused with a line that says why; a click beside the popup keeps what it holds
    yaml = "".join(f"key{k}: value {k}\n" for k in range(20))
    front = f"---\n{yaml}---\n\n# Заголовок\n\nТекст после свойств.\n"
    doc = OUT / "edit-popup-front.md"
    doc.write_bytes(front.encode("utf-8"))
    proc, hwnd = launch_edit(doc, {"FASTMD_AUTOSAVE_MS": "60000"})
    try:
        # (§2.7: a double click on an object in reading mode enters edit mode with its popup open)
        dbl_click(hwnd, q(hwnd, "TEXT_LEFT") + 30, q(hwnd, "BLOCK_Y", 0) + 12, 0.2)
        wait_for(lambda: q(hwnd, "EDITING") == 1 and q(hwnd, "EDIT_BAR") == 100, 3.0, 0.05)
        undo0 = q(hwnd, "UNDO_DEPTH")
        shown = popup_open(hwnd) and popup_text(hwnd) == yaml.rstrip("\n")
        shot(hwnd, "139-edit-popup-front")
        shot_dark(hwnd, "139-edit-popup-front-dark")
        popup_set(hwnd, yaml + "---\nx: 1")
        popup_settled(hwnd)
        refused = q(hwnd, "EDIT_POPUP_STATE") == POPUP["ERROR"] and q(hwnd, "SRC_HASH", 0) == src_hash(front)
        popup_set(hwnd, yaml.replace("value 0", "значение", 1).rstrip("\n"))
        popup_settled(hwnd)
        cmd(hwnd, "POPUP_DONE", 0.5)
        front = front.replace("value 0", "значение", 1)
        ok &= check("edit 3b: the front matter's popup (opened by a double click in reading mode, over the lower part of "
                    "its tall block): its lines; a --- line refused with the reason; kept as one undo step",
                    shown and refused and q(hwnd, "EDIT_POPUP", 0) == 0 and
                    q(hwnd, "SRC_HASH", 0) == src_hash(front) and q(hwnd, "UNDO_DEPTH") == undo0 + 1,
                    f"shown {shown}, refused {refused}, popup {q(hwnd, 'EDIT_POPUP', 0)}, len {q(hwnd, 'SRC_LEN', 0)} vs "
                    f"{u16(front)}, undo {q(hwnd, 'UNDO_DEPTH')} vs {undo0}")
        ok &= check("edit 3b: saved as it reads", saved_text(hwnd, doc) == front)
    finally:
        close_edit(proc, hwnd)
    return ok


LINK_DOC = ("# Ссылки\n\nТекст для первой ссылки здесь.\n\nЕсть [старая](http://old.example) ссылка.\n\n"
            "Ссылка [по метке][ref] тут.\n\n[ref]: http://ref.example\n\nАвто <http://auto.example> тут.\n\n"
            "```\nprint(1)\n```\n")


def test_edit_link():
    """§8.3 (3b): the link popover makes `[selection](url)`, an address with blanks in <…>; in a link it rewrites the
    destination; a reference link's notice needs a first Enter; Remove link keeps the text; an autolink removed stays text
    by an escape; the popover in light and dark"""
    ok = True
    doc = OUT / "edit-link.md"
    doc.write_bytes(LINK_DOC.encode("utf-8"))
    set_reg("EditHintShown", 1)
    proc, hwnd = launch_edit(doc, {"FASTMD_AUTOSAVE_MS": "60000", "FASTMD_TEST_HOOKS": "1"})
    try:
        text = LINK_DOC
        enter_edit(hwnd, 1, dx=1)
        select_word(hwnd, 2)  # «первой »
        opened = popover_pair(hwnd, "135-edit-link-popover", "LINK")
        popup_set(hwnd, "https://new.example/путь")
        cmd(hwnd, "POPUP_DONE", 0.5)
        text = text.replace("для первой ссылки", "для [первой](https://new.example/путь) ссылки", 1)
        ok &= check("edit 3b: the link popover wraps the selection: [text](url)", opened and q(hwnd, "SRC_HASH", 0) == src_hash(text),
                    f"opened {opened}, len {q(hwnd, 'SRC_LEN', 0)} vs {u16(text)}")
        select_word(hwnd, 0)  # «Текст »
        cmd(hwnd, "LINK", 0.5)
        popup_open(hwnd)
        popup_set(hwnd, "my file.md")
        cmd(hwnd, "POPUP_DONE", 0.5)
        text = text.replace("Текст для", "[Текст](<my file.md>) для", 1)
        ok &= check("edit 3b: an address with blanks goes in <…>", q(hwnd, "SRC_HASH", 0) == src_hash(text),
                    f"len {q(hwnd, 'SRC_LEN', 0)} vs {u16(text)}")
        # in an inline link: its address shown, and only the destination rewritten
        click(hwnd, q(hwnd, "TEXT_LEFT") + 2, q(hwnd, "BLOCK_Y", 2) + 10, 0.2)
        post(hwnd, WM_KEYDOWN, VK["home"], 0, 0.1)
        for _ in range(7):
            post(hwnd, WM_KEYDOWN, VK["right"], 0, 0.03)
        cmd(hwnd, "LINK", 0.5)
        shown = popup_open(hwnd) and popup_text(hwnd) == "http://old.example"
        popup_set(hwnd, "http://new.example")
        cmd(hwnd, "POPUP_DONE", 0.5)
        text = text.replace("(http://old.example)", "(http://new.example)", 1)
        ok &= check("edit 3b: in a link the popover shows its address and rewrites only the destination",
                    shown and q(hwnd, "SRC_HASH", 0) == src_hash(text), f"shown {shown}")
        cmd(hwnd, "LINK_REMOVE", 0.4)
        text = text.replace("[старая](http://new.example)", "старая", 1)
        ok &= check("edit 3b: Remove link: the delimiters go, the text stays", q(hwnd, "SRC_HASH", 0) == src_hash(text),
                    f"len {q(hwnd, 'SRC_LEN', 0)} vs {u16(text)}")
        # a reference link: the notice, the first Enter confirms it, the second changes the definition
        click(hwnd, q(hwnd, "TEXT_LEFT") + 2, q(hwnd, "BLOCK_Y", 3) + 10, 0.2)
        post(hwnd, WM_KEYDOWN, VK["home"], 0, 0.1)
        for _ in range(9):
            post(hwnd, WM_KEYDOWN, VK["right"], 0, 0.03)
        ref = popover_pair(hwnd, "136-edit-link-reference", "LINK") and popup_text(hwnd) == "http://ref.example"
        popup_set(hwnd, "http://ref2.example")
        cmd(hwnd, "POPUP_DONE", 0.4)
        first = q(hwnd, "EDIT_POPUP", 0) != 0 and q(hwnd, "SRC_HASH", 0) == src_hash(text)
        cmd(hwnd, "POPUP_DONE", 0.5)
        text = text.replace("[ref]: http://ref.example", "[ref]: http://ref2.example", 1)
        ok &= check("edit 3b: a reference link: its definition's address with the notice; the first Enter only confirms it, "
                    "the second changes the definition", ref and first and q(hwnd, "SRC_HASH", 0) == src_hash(text),
                    f"ref {ref}, first {first}")
        # an autolink removed: escaped, so it does not link again
        click(hwnd, q(hwnd, "TEXT_LEFT") + 2, q(hwnd, "BLOCK_Y", 4) + 10, 0.2)
        post(hwnd, WM_KEYDOWN, VK["home"], 0, 0.1)
        for _ in range(8):
            post(hwnd, WM_KEYDOWN, VK["right"], 0, 0.03)
        cmd(hwnd, "LINK_REMOVE", 0.4)
        text = text.replace("<http://auto.example>", "http\\://auto.example", 1)
        ok &= check("edit 3b: an autolink removed stays text: `<http://x>` → `http\\://x`", q(hwnd, "SRC_HASH", 0) == src_hash(text),
                    f"len {q(hwnd, 'SRC_LEN', 0)} vs {u16(text)}")
        # the code-language popover (§8.7): the fence's info string
        click(hwnd, q(hwnd, "TEXT_LEFT") + 30, q(hwnd, "BLOCK_Y", 5) + 14, 0.3)
        lang = popover_pair(hwnd, "138-edit-code-language", "CODE_LANG") and popup_text(hwnd) == ""
        popup_set(hwnd, "python")
        cmd(hwnd, "POPUP_DONE", 0.5)
        text = text.replace("```\nprint(1)", "```python\nprint(1)", 1)
        ok &= check("edit 3b: the code-language popover writes the fence's info string", lang and q(hwnd, "SRC_HASH", 0) == src_hash(text),
                    f"lang {lang}, len {q(hwnd, 'SRC_LEN', 0)} vs {u16(text)}")
        ok &= check("edit 3b: saved as it reads", saved_text(hwnd, doc) == text)
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.4)
    finally:
        close_edit(proc, hwnd)
    return ok


IMAGE_DOC = "# Картинки\n\nАбзац перед картинками.\n\nВторой абзац.\n\nТретий абзац.\n"


def test_edit_image():
    """§8.8, §7.11, §2.8 (3b): the picture dialog (FASTMD_OPEN_FILE) puts `![name](<img/…>)` in; a dropped picture goes
    in where it was dropped; CF_HDROP pasted puts pictures in, other files get a toast; a bitmap alone gets its toast and
    changes nothing"""
    ok = True
    doc = OUT / "edit-image.md"
    doc.write_bytes(IMAGE_DOC.encode("utf-8"))
    pic = OUT / "img" / "Снимок экрана.png"
    shutil.copy(OUT / "img" / "diagram0.png", pic)
    set_reg("EditHintShown", 1)
    proc, hwnd = launch_edit(doc, {"FASTMD_AUTOSAVE_MS": "60000", "FASTMD_OPEN_FILE": str(pic)})
    try:
        text = IMAGE_DOC
        enter_edit(hwnd, 1, dx=1)
        post(hwnd, WM_KEYDOWN, VK["end"], 0, 0.2)
        prompts = q(hwnd, "LAST_PROMPT", 1)
        cmd(hwnd, "INS_IMAGE", 0.6)
        text = text.replace("перед картинками.", "перед картинками.![Снимок экрана](<img/Снимок экрана.png>)", 1)
        ok &= check("edit 3b: the picture dialog (FASTMD_OPEN_FILE): ![stem](<relative path with blanks>) at the caret",
                    q(hwnd, "SRC_HASH", 0) == src_hash(text) and q(hwnd, "LAST_PROMPT", 0) == 5 and
                    q(hwnd, "LAST_PROMPT", 1) == prompts + 1, f"len {q(hwnd, 'SRC_LEN', 0)} vs {u16(text)}")
        # a dropped picture: where it was dropped (the end of the second paragraph)
        y = q(hwnd, "BLOCK_Y", 2) + 10
        data = hdrop([OUT / "img" / "diagram0.png"], q(hwnd, "TEXT_LEFT") + 600, y)
        h = k32.GlobalAlloc(0x0042, len(data))
        p = k32.GlobalLock(h)
        ctypes.memmove(p, data, len(data))
        k32.GlobalUnlock(h)
        post(hwnd, WM_DROPFILES, h, 0, 0.6)
        text = text.replace("Второй абзац.", "Второй абзац.![diagram0](img/diagram0.png)", 1)
        ok &= check("edit 3b: a picture file dropped goes in where it was dropped", q(hwnd, "SRC_HASH", 0) == src_hash(text),
                    f"len {q(hwnd, 'SRC_LEN', 0)} vs {u16(text)}")
        # CF_HDROP pasted: the pictures; another file only gets a toast
        click(hwnd, q(hwnd, "TEXT_LEFT") + 2, q(hwnd, "BLOCK_Y", 3) + 10, 0.2)
        post(hwnd, WM_KEYDOWN, VK["end"], 0, 0.2)
        clipboard_formats([(15, hdrop([OUT / "img" / "diagram0.png"]))])  # CF_HDROP
        cmd(hwnd, "PASTE", 0.5)
        text = text.replace("Третий абзац.", "Третий абзац.![diagram0](img/diagram0.png)", 1)
        pasted = q(hwnd, "SRC_HASH", 0) == src_hash(text)
        clipboard_formats([(15, hdrop([doc]))])
        cmd(hwnd, "PASTE", 0.5)
        only = toast_said(hwnd, "Вставить можно только картинки", "Only pictures can be pasted") and \
            q(hwnd, "SRC_HASH", 0) == src_hash(text)
        ok &= check("edit 3b: pasted picture files (CF_HDROP) go in; another file: the toast, nothing changes", pasted and only,
                    f"pasted {pasted}, toast {only}")
        # a bitmap alone: the toast that says what works instead, nothing changes
        dib = bytes(ctypes.c_uint32(40)) + bytes(ctypes.c_int32(1)) + bytes(ctypes.c_int32(1)) + b"\x01\x00\x20\x00" + bytes(24) + b"\xff\xff\xff\xff"
        clipboard_formats([(8, dib)])  # CF_DIB
        cmd(hwnd, "PASTE", 0.5)
        toast = toast_said(hwnd, "Вставка картинок из буфера пока не поддерживается — перетащите файл сюда",
                           "Pasting pictures from the clipboard is not supported yet — drop the file here")
        ok &= check("edit 3b: a bitmap alone on the clipboard: the toast, nothing changes",
                    toast and q(hwnd, "SRC_HASH", 0) == src_hash(text), f"toast {toast}")
        ok &= check("edit 3b: saved as it reads", saved_text(hwnd, doc) == text)
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.4)
    finally:
        close_edit(proc, hwnd)
        if pic.exists():
            pic.unlink()
    return ok


PRIVATE_DOC = "# Копирование\n\nТекст **жирный кусок** и хвост.\n\nКонец.\n"


def test_edit_paste_private():
    """§7.11 (3b): a copy in edit mode puts FastMD's own format on the clipboard - the selection's source balanced - and a
    paste inside FastMD takes it first, so the markup stays balanced; the plain text is what other programs get"""
    ok = True
    doc = OUT / "edit-private.md"
    doc.write_bytes(PRIVATE_DOC.encode("utf-8"))
    set_reg("EditHintShown", 1)
    proc, hwnd = launch_edit(doc, {"FASTMD_AUTOSAVE_MS": "60000", "FASTMD_TEST_HOOKS": "1"})
    try:
        text = PRIVATE_DOC
        enter_edit(hwnd, 1, dx=1)
        post(hwnd, WM_KEYDOWN, VK["home"], 0, 0.1)
        for _ in range(13):  # «Текст жирный »: into the bold
            post(hwnd, WM_KEYDOWN, VK["right"], 0, 0.03)
        for _ in range(8):  # «кусок и »: out of it
            testkey(hwnd, VK["right"], KM_SHIFT, 0.03)
        time.sleep(0.2)
        cmd(hwnd, "COPY", 0.4)
        private, plain = clipboard_wide("FastMD Markdown"), clipboard()
        ok &= check("edit 3b: the copy holds the balanced source in FastMD's own format, and plain text for others",
                    private == "**кусок** и " and plain == "кусок и ", f"private {private!r}, plain {plain!r}")
        click(hwnd, q(hwnd, "TEXT_LEFT") + 2, q(hwnd, "BLOCK_Y", 2) + 10, 0.2)
        post(hwnd, WM_KEYDOWN, VK["end"], 0, 0.2)
        cmd(hwnd, "PASTE", 0.5)
        text = text.replace("Конец.", "Конец.**кусок** и ", 1)
        ok &= check("edit 3b: pasted inside FastMD, the markup stays balanced", q(hwnd, "SRC_HASH", 0) == src_hash(text),
                    f"len {q(hwnd, 'SRC_LEN', 0)} vs {u16(text)}")
        ok &= check("edit 3b: saved as it reads", saved_text(hwnd, doc) == text)
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.4)
    finally:
        close_edit(proc, hwnd)
    return ok


BINDINGS_DOC = "# Клавиши\n\nПервое слово тут.\n\nВторой абзац.\n"


def test_edit_bindings():
    """T5 (3b): the chords as real posted keys, with the modifiers in the keyboard state the app reads - Ctrl+Z / Y / B /
    I / K / S / 1, Ctrl+Shift+7 / 8 / 9 / Q / K, Ctrl+T / M, Ctrl+`, Ctrl+Enter; each its effect on the source. The file
    is reset and the app launched anew for a retry (a real keyboard state can be disturbed from outside)"""
    ok = False
    for attempt in range(2):
        doc = OUT / "edit-bindings.md"
        doc.write_bytes(BINDINGS_DOC.encode("utf-8"))
        set_reg("EditHintShown", 1)
        proc, hwnd = launch_edit(doc, {"FASTMD_AUTOSAVE_MS": "60000", "FASTMD_TEST_HOOKS": "1"})
        results = {}
        try:
            text = BINDINGS_DOC
            enter_edit(hwnd, 1, dx=1)
            select_word(hwnd, 0)  # «Первое »
            base = q(hwnd, "SRC_HASH", 0)

            def chord(name, vk, shift=False, want=None, undo=True, wait=0.4):
                keys(hwnd, [vk], shift=shift, ctrl=True, wait=wait)
                got = q(hwnd, "SRC_HASH", 0)
                results[name] = want(got) if want else got != base
                if undo and got != base:
                    keys(hwnd, [ord("Z")], ctrl=True, wait=0.4)
            chord("Ctrl+B", ord("B"), want=lambda h: h == src_hash(text.replace("Первое", "**Первое**", 1)))
            chord("Ctrl+I", ord("I"), want=lambda h: h == src_hash(text.replace("Первое", "*Первое*", 1)))
            chord("Ctrl+`", 0xC0, want=lambda h: h == src_hash(text.replace("Первое", "`Первое`", 1)))
            chord("Ctrl+1", ord("1"), want=lambda h: h == src_hash(text.replace("Первое", "# Первое", 1)))
            chord("Ctrl+Shift+7", ord("7"), shift=True, want=lambda h: h == src_hash(text.replace("Первое", "1. Первое", 1)))
            chord("Ctrl+Shift+8", ord("8"), shift=True, want=lambda h: h == src_hash(text.replace("Первое", "- Первое", 1)))
            chord("Ctrl+Shift+9", ord("9"), shift=True, want=lambda h: h == src_hash(text.replace("Первое", "- [ ] Первое", 1)))
            chord("Ctrl+Shift+Q", ord("Q"), shift=True, want=lambda h: h == src_hash(text.replace("Первое", "> Первое", 1)))
            chord("Ctrl+Shift+K", ord("K"), shift=True,
                  want=lambda h: h == src_hash(text.replace("Первое слово тут.", "```\nПервое слово тут.\n```", 1)))
            chord("Ctrl+T", ord("T"))
            # Ctrl+Z / Ctrl+Y: the bold again, undone, redone
            keys(hwnd, [ord("B")], ctrl=True, wait=0.4)
            bold = q(hwnd, "SRC_HASH", 0)
            keys(hwnd, [ord("Z")], ctrl=True, wait=0.4)
            undone = q(hwnd, "SRC_HASH", 0) == base
            keys(hwnd, [ord("Y")], ctrl=True, wait=0.4)
            results["Ctrl+Z, Ctrl+Y"] = undone and q(hwnd, "SRC_HASH", 0) == bold
            keys(hwnd, [ord("Z")], ctrl=True, wait=0.4)
            # Ctrl+K: the link popover (Esc closes it); Ctrl+M: a formula with its popup (Esc keeps the formula)
            select_word(hwnd, 0)
            keys(hwnd, [ord("K")], ctrl=True, wait=0.5)
            results["Ctrl+K"] = popup_open(hwnd, 2.0)
            post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.4)
            post(hwnd, WM_KEYDOWN, VK["end"], 0, 0.2)
            keys(hwnd, [ord("M")], ctrl=True, wait=0.6)
            results["Ctrl+M"] = popup_open(hwnd, 2.0) and q(hwnd, "SRC_HASH", 0) != base and q(hwnd, "EDIT_ATOM") >= 0
            post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.4)
            keys(hwnd, [ord("Z")], ctrl=True, wait=0.4)
            # Ctrl+Enter: a new paragraph after the block (a phantom); Ctrl+S: a save
            keys(hwnd, [VK["return"]], ctrl=True, wait=0.4)
            results["Ctrl+Enter"] = q(hwnd, "EDIT_PHANTOM", 0) >= 0
            post(hwnd, WM_KEYDOWN, VK["up"], 0, 0.2)
            type_text(hwnd, "!", 0.3)
            saves = q(hwnd, "SAVES")
            keys(hwnd, [ord("S")], ctrl=True, wait=0.5)
            results["Ctrl+S"] = q(hwnd, "SAVES") == saves + 1 and q(hwnd, "EDIT_DIRTY") == 0
            post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.4)
        finally:
            close_edit(proc, hwnd)
        ok = all(results.values()) and len(results) == 15
        if ok or attempt:
            break
        print(f"  (a retry: {[k for k, v in results.items() if not v]})")
    return check("edit 3b: the chords as real keys: " + ", ".join(k for k in results), ok,
                 ", ".join(f"{k} {'ok' if v else 'FAIL'}" for k, v in results.items()))


def test_edit_race_images():
    """T13 (3b): the formulas a popup is typing, a picture from the network arriving after 2 s, a theme switch and 30
    undos while the picture worker and the preview worker are slowed down (300 ms a job): the window lives, the three
    drawable formulas and diagrams are drawn in the end, the renders stay bounded, the map is never wrong"""
    import http.server
    import socketserver
    import threading
    ok = True
    png = (REPO / "bench" / "corpus" / "img" / "diagram0.png").read_bytes()

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            time.sleep(2.0)
            self.send_response(200)
            self.send_header("Content-Type", "image/png")
            self.send_header("Content-Length", str(len(png)))
            self.end_headers()
            self.wfile.write(png)

        def log_message(self, *a):
            pass

    srv = socketserver.ThreadingTCPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    doc = OUT / "edit-race.md"
    url = f"http://127.0.0.1:{srv.server_address[1]}/race-{time.time_ns()}.png"  # (never in the disk cache)
    text = MATH_DOC.replace("Конец документа.", f"![сеть]({url})\n\nКонец документа.")
    doc.write_bytes(text.encode("utf-8"))
    set_reg("EditHintShown", 1)
    set_reg("RemoteImages", 0)  # always
    proc, hwnd = launch_edit(doc, {"FASTMD_AUTOSAVE_MS": "60000", "FASTMD_TEST_SLOW": "images:300,preview:300"})
    try:
        enter_edit(hwnd, 1, dx=1)
        atom_after(hwnd, 1, 11)  # «В строке: » and the formula
        cmd(hwnd, "ATOM_EDIT", 0.4)
        popup_open(hwnd)
        tex = "E = mc^2"
        for k in range(30):  # 30 characters, a change each (the preview worker's latest job wins)
            tex += "+abcdefghij"[k % 11]
            popup_set(hwnd, tex)
            time.sleep(0.03)
        cmd(hwnd, "THEME_DARK", 0.2)  # (the popup stays; the first undo closes it, keeping its text, and takes it back)
        for _ in range(30):
            cmd(hwnd, "UNDO", 0.05)
        cmd(hwnd, "THEME_LIGHT", 0.2)
        done = wait_for(lambda: q(hwnd, "MATH", 1) == 3 and not q(hwnd, "EDIT_BUSY") & (16 | 32), 20.0, 0.2)
        renders = q(hwnd, "RENDERS")
        ok &= check("edit 3b: typing in a formula's popup, a theme switch and 30 undos while renders are pending: the window "
                    "lives, the three drawable formulas and diagrams are drawn", proc.poll() is None and done and
                    q(hwnd, "SRC_HASH", 0) == src_hash(text), f"drawn {q(hwnd, 'MATH', 1)}, busy {q(hwnd, 'EDIT_BUSY')}, "
                    f"len {q(hwnd, 'SRC_LEN', 0)} vs {u16(text)}, undo {q(hwnd, 'UNDO_DEPTH')}")
        ok &= check("edit 3b: ... after a bounded number of renders", 0 < renders <= 60, f"{renders}")
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.4)
    finally:
        close_edit(proc, hwnd)
        srv.shutdown()
        del_reg("RemoteImages")
    return ok


BUBBLE_DOC = "# Пузырь\n\nАбзац со [ссылкой на документ](other.md) внутри.\n\nВторой абзац.\n"


def test_edit_bubble():
    """§2.11 (3b): with the caret in a link's text, a second after the typing, the bubble under the link: its address,
    [Edit] (the link popover), [Remove]; it hides while typing; light and dark"""
    ok = True
    doc = OUT / "edit-bubble.md"
    doc.write_bytes(BUBBLE_DOC.encode("utf-8"))
    set_reg("EditHintShown", 1)
    proc, hwnd = launch_edit(doc, {"FASTMD_AUTOSAVE_MS": "60000", "FASTMD_TEST_HOOKS": "1"})
    try:
        text = BUBBLE_DOC
        enter_edit(hwnd, 1, dx=1)
        post(hwnd, WM_KEYDOWN, VK["home"], 0, 0.1)
        for _ in range(12):  # «Абзац со сс»: in the link
            post(hwnd, WM_KEYDOWN, VK["right"], 0, 0.03)
        shown = wait_for(lambda: q(hwnd, "EDIT_BUBBLE") == 1, 2.0, 0.1)
        shot(hwnd, "137-edit-bubble")
        shot_dark(hwnd, "137-edit-bubble-dark")
        type_text(hwnd, "ы", 0.2)
        hidden = q(hwnd, "EDIT_BUBBLE") == 0
        back = wait_for(lambda: q(hwnd, "EDIT_BUBBLE") == 1, 3.0, 0.1)
        text = text.replace("[ссылкой", "[ссыылкой", 1)
        edit = tool_xy(hwnd, "LINK", 1)
        if edit:
            click(hwnd, *edit, 0.5)
        popover = popup_open(hwnd, 2.0) and popup_text(hwnd) == "other.md"
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.3)
        wait_for(lambda: q(hwnd, "EDIT_BUBBLE") == 1, 3.0, 0.1)
        remove = tool_xy(hwnd, "LINK_REMOVE")
        if remove:
            click(hwnd, *remove, 0.5)
        text = text.replace("[ссыылкой на документ](other.md)", "ссыылкой на документ", 1)
        ok &= check("edit 3b: the link bubble: shown with the caret in a link, hidden while typing and back a second later; "
                    "its [Edit] opens the popover with the address, [Remove] removes the link",
                    shown and hidden and back and popover and q(hwnd, "SRC_HASH", 0) == src_hash(text) and q(hwnd, "EDIT_BUBBLE") == 0,
                    f"shown {shown}, hidden {hidden}, back {back}, edit {edit}, popover {popover}, remove {remove}")
        post(hwnd, WM_KEYDOWN, VK["esc"], 0, 0.4)
    finally:
        close_edit(proc, hwnd)
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
             ("splice_hook", test_splice_hook), ("save_fault", test_save_fault), ("reading_touch", test_reading_touch),
             ("locked_at_open", test_locked_at_open), ("modal_scope", test_modal_scope),
             ("edit_enter_leave", test_edit_enter_leave), ("edit_fullpending", test_edit_fullpending),
             ("edit_typing_utf8", lambda: edit_typing("utf8")), ("edit_typing_bomcrlf", lambda: edit_typing("bomcrlf")),
             ("edit_typing_utf16", lambda: edit_typing("utf16")), ("edit_typing_ansi", lambda: edit_typing("ansi")),
             ("edit_keys_once", test_edit_keys_once), ("edit_surrogates", test_edit_surrogates),
             ("edit_undo", test_edit_undo), ("edit_guards", test_edit_guards), ("edit_conflict", test_edit_conflict),
             ("edit_busy_retry", test_edit_busy_retry), ("edit_readonly_missing", test_edit_readonly_missing),
             ("edit_encoding_strip", test_edit_encoding_strip), ("edit_close_session", test_edit_close_session),
             ("edit_recovery", test_edit_recovery), ("edit_two_windows", test_edit_two_windows),
             ("edit_frames", test_edit_frames), ("edit_blink", test_edit_blink), ("edit_perf", test_edit_perf),
             ("edit_debounced", test_edit_debounced), ("settings_autosave", test_settings_autosave),
             ("edit_review_keys", test_edit_review_keys), ("edit_review_doc", test_edit_review_doc),
             ("edit_chrome_zoom", test_edit_chrome_zoom), ("edit_modal", test_edit_modal),
             ("edit_recovery_guards", test_edit_recovery_guards), ("edit_structure", test_edit_structure),
             ("edit_selection", test_edit_selection), ("edit_paste_plain", test_edit_paste_plain),
             ("edit_table_typing", test_edit_table_typing), ("edit_raw_typing", test_edit_raw_typing),
             ("edit_find", test_edit_find), ("edit_outline", test_edit_outline),
             ("edit_commands", test_edit_commands), ("edit_table", test_edit_table), ("edit_hr", test_edit_hr),
             ("edit_popups", test_edit_popups), ("edit_link", test_edit_link), ("edit_image", test_edit_image),
             ("edit_paste_private", test_edit_paste_private), ("edit_bindings", test_edit_bindings),
             ("edit_race_images", test_edit_race_images), ("edit_bubble", test_edit_bubble)]
    only = [n for n in os.environ.get("FASTMD_ONLY", "").split(",") if n]  # e.g. FASTMD_ONLY=update,settings
    for name, t in tests:
        if not only or name in only:
            ok &= t()
    if any(name.startswith(("edit_", "settings_autosave")) for name, _ in tests if not only or name in only):
        ok &= check("edit 2a: no edit window's map failed its self-check after a swap", not SELFCHECK, "; ".join(SELFCHECK))
        ok &= check("edit 2a: every edit window closed at once, without a question, with exit code 0", not CLOSE_PROBLEMS,
                    "; ".join(CLOSE_PROBLEMS))
    dumps = sorted((DATA / "crashes").glob("*.dmp")) if (DATA / "crashes").exists() else []
    ok &= check("no crash dump in the test profile", not dumps, ", ".join(d.name for d in dumps))
    reset_profile()
    print("RESULT", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
