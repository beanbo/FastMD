"""Screenshots of every edit-mode visual (docs/EDIT-MODE.md §15.1 item 9, §15.2 Phase 4) at two document zooms and in
both themes, for a person to look at before a release:

    python -X utf8 app/tests/edit_shots.py [--zoom=100,150] [--theme=light,dark] [--only=bar,popups,...] [FastMD.exe]

The bar, its tooltip and active states, the caret in a heading, every popover, the link bubble, a table being edited,
the phantom row, the find bar, the narrow bar and its "…", every source popup (and a failed formula), every strip, the
pencil and the first-use toasts. The pictures go to app/tests/out/edit-final/<scene>-<zoom>-<theme>.png. Nothing is
checked here: ui_smoke.py tests the behaviour, this script only makes the pictures. It runs in a profile of its own
(FASTMD_REGKEY, FASTMD_DATA), never the reader's, and works on copies of its own documents.
"""
import os
import pathlib
import stat
import subprocess
import sys
import time

from PIL import Image

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import ui_smoke as u  # noqa: E402

WORK = u.OUT / "edit-shots-docs"
FINAL = u.OUT / "edit-final"
u.REGKEY = r"Software\FastMD-editshots"  # (set_reg / reset_profile read the module's globals)
u.DATA = u.OUT / "edit-shots-profile"
u.ENV.update(FASTMD_REGKEY=u.REGKEY, FASTMD_DATA=str(u.DATA))
u.OUT = FINAL  # u.shot saves there

ARGS = [a for a in sys.argv[1:] if a.startswith("--")]
ZOOMS = next((a[7:].split(",") for a in ARGS if a.startswith("--zoom=")), ["100", "150"])
THEMES = next((a[8:].split(",") for a in ARGS if a.startswith("--theme=")), ["light", "dark"])
ONLY = next((a[7:].split(",") for a in ARGS if a.startswith("--only=")), [])
u.EXE = pathlib.Path(next((a for a in sys.argv[1:] if a.endswith(".exe")),
                          HERE.parent / "build" / "Release" / "FastMD.exe"))  # (ui_smoke took argv[1] for it)
AWAY = (30, 700)  # a point over nothing: the left margin, below the bar
TOAST = 4.5  # s: the first-use toasts stand 4 s

MAIN_DOC = ("# Заголовок первого уровня\n\n"
            "Первый абзац с **полужирным словом**, *курсивом* и [ссылкой на документ](other.md).\n\n"
            "## Подзаголовок\n\n"
            "- пункт один\n- пункт два\n\n"
            "| Имя | Значение |\n| --- | --- |\n| альфа | 1 |\n| бета | 2 |\n\n"
            "```python\nprint(\"код\")\n```\n\n"
            "Ссылка [по метке][ref] в последнем абзаце.\n\n"
            "[ref]: http://ref.example\n")
B_H1, B_PARA, B_H2, B_ITEM1, B_ITEM2, B_TABLE, B_CODE, B_LAST = range(8)

POPUP_DOC = ("---\ntitle: Пример свойств\ntags: [правка, снимки]\n---\n\n"
             "# Всплывающие окна\n\n"
             "Формула в строке $a+b$ тут.\n\n"
             "$$\n\\int_0^1 x^2\\,dx = \\frac{1}{3}\n$$\n\n"
             "```mermaid\ngraph TD\n    A[Начало] --> B{Выбор}\n    B --> C[Да]\n```\n\n"
             "<div align=\"center\">HTML блок</div>\n\n"
             "Картинка ![кот](img/diagram0.png) в строке.\n\n"
             "---\n\n"
             "Последний абзац.\n")
P_FRONT, P_H1, P_INLINE, P_DISPLAY, P_MERMAID, P_HTML, P_PICTURE, P_HR, P_LAST = range(9)

STRIP_DOC = ("# Полосы\n\nПервый абзац для набора текста.\n\nВторой абзац с текстом.\n\nПоследний абзац.\n")


def launch(doc, zoom, theme, size="1000x800", extra=None):
    """a window of the test profile: a steady caret, posted hovers kept (TEST_HOOKS), every question answered"""
    env = dict(u.ENV, **u.EDIT_ENV, FASTMD_TEST_HOOKS="1")
    env.update(extra or {})
    args = [str(u.EXE), f"--{theme}", f"--zoom={zoom}", f"--size={size}"] + ([str(doc)] if doc else [])
    proc = subprocess.Popen(args, env=env)
    hwnd = u.find_window(proc.pid)
    time.sleep(0.8)
    return proc, hwnd


def fresh(name, text, encoding="utf-8", variant=""):
    """a document of the scene's own; a name per zoom and theme, so no reading position of an earlier run moves it"""
    p = WORK / (name[:-3] + variant + ".md")
    if p.exists():
        os.chmod(p, stat.S_IWRITE)
    p.write_bytes(text.encode(encoding))
    return p


class Scene:
    def __init__(self, zoom, theme):
        self.zoom, self.theme = zoom, theme
        self.k = int(zoom) / 100  # (offsets into the page grow with the zoom; the bar's do not)

    def z(self, v):
        return int(v * self.k)

    def doc(self, name, text, encoding="utf-8"):
        return fresh(name, text, encoding, f"-{self.zoom}-{self.theme}")

    def snap(self, hwnd, name):
        if not u.u32.IsWindow(hwnd):
            raise RuntimeError(f"{name}: the window is gone")
        u.shot(hwnd, f"{name}-{self.zoom}-{self.theme}")

    def away(self, hwnd, wait=0.3):
        u.post(hwnd, u.WM_MOUSEMOVE, 0, u.lp(*AWAY), wait)

    def hover(self, hwnd, xy, wait=0.35):
        if xy:
            u.post(hwnd, u.WM_MOUSEMOVE, 0, u.lp(*xy), wait)

    def esc(self, hwnd, wait=0.3):
        u.post(hwnd, u.WM_KEYDOWN, u.VK["esc"], 0, wait)

    def popover(self, hwnd, name, menu, row=0):
        """a popover open, a row of it hot; closed again by Esc"""
        u.cmd(hwnd, menu, 0.4)
        if row:
            self.hover(hwnd, u.tool_xy(hwnd, menu, row))
        self.snap(hwnd, name)
        self.away(hwnd, 0.1)
        self.esc(hwnd)

    def caret_in(self, hwnd, block, right=0):
        """the caret at a block's (first) line start, then `right` characters on"""
        u.click(hwnd, u.q(hwnd, "TEXT_LEFT") + 2, u.q(hwnd, "BLOCK_Y", block) + self.z(10), 0.2)
        u.post(hwnd, u.WM_KEYDOWN, u.VK["home"], 0, 0.1)
        for _ in range(right):
            u.post(hwnd, u.WM_KEYDOWN, u.VK["right"], 0, 0.03)
        time.sleep(0.2)


# ------------------------------------------------------------------------------------------------ scenes
def reading(s):
    """reading mode: the pencil and its tooltip; F2 on a fresh profile: the first entry's toast; the first edit's toast;
    the settings window with the autosave row"""
    u.del_reg("EditHintShown")
    doc = s.doc("reading.md", MAIN_DOC)
    proc, hwnd = launch(doc, s.zoom, s.theme)
    try:
        u.settle(hwnd)
        pencil = u.q(hwnd, "EDIT_TOOL", u.CMD["EDIT_TOGGLE"])
        s.hover(hwnd, (pencil & 0xFFFF, pencil >> 16) if pencil > 0 else None, 0.5)
        s.snap(hwnd, "a01-reading-pencil")
        s.away(hwnd)
        u.post(hwnd, u.WM_KEYDOWN, u.VK["f2"], 0, 0.1)
        u.wait_for(lambda: u.q(hwnd, "EDITING") == 1 and u.q(hwnd, "EDIT_BAR") == 100, 3.0, 0.05)
        time.sleep(0.3)
        s.snap(hwnd, "a02-toast-first-entry")
        time.sleep(TOAST)
        u.type_text(hwnd, "Новое ", 0.3)
        s.snap(hwnd, "a03-toast-first-edit")
        time.sleep(TOAST)
        u.cmd(hwnd, "UNDO", 0.4)
        u.cmd(hwnd, "SAVE", 0.4)
        s.esc(hwnd, 0.5)
        u.cmd(hwnd, "SETTINGS", 0.5)
        u.wait_for(lambda: u.q(hwnd, "SETTINGS_HWND") != 0, 3.0)
        sh = u.q(hwnd, "SETTINGS_HWND")
        if sh:
            time.sleep(0.6)
            s.snap(sh, "a04-settings-autosave")
            u.post(sh, u.WM_CLOSE, 0, 0, 0.4)
    finally:
        u.close_edit(proc, hwnd)
        u.set_reg("EditHintShown", 1)


def bar(s):
    """the bar and its states, the caret in a heading, the popovers, the bubble, a table being edited, the phantom row
    and the find bar"""
    doc = s.doc("bar.md", MAIN_DOC)
    proc, hwnd = launch(doc, s.zoom, s.theme)
    try:
        u.enter_edit(hwnd, B_PARA, dx=1)
        s.away(hwnd)
        u.settle(hwnd)
        s.snap(hwnd, "b01-bar")
        s.hover(hwnd, u.tool_xy(hwnd, "FMT_BOLD"))
        s.snap(hwnd, "b02-bar-tooltip")
        s.away(hwnd)
        # the caret in a heading: the style button says so
        s.caret_in(hwnd, B_H1, 8)
        s.snap(hwnd, "b03-caret-heading")
        s.caret_in(hwnd, B_H2, 4)
        s.snap(hwnd, "b04-caret-heading2")
        # a word of the bold run selected: Bold active
        s.caret_in(hwnd, B_PARA)
        for _ in range(3):
            u.testkey(hwnd, u.VK["right"], u.KM_CTRL, 0.1)
        u.testkey(hwnd, u.VK["right"], u.KM_CTRL | u.KM_SHIFT, 0.3)
        s.snap(hwnd, "b05-active-bold")
        # the popovers (the style one on a text paragraph, «Heading 2» hot)
        s.popover(hwnd, "b06-popover-style", "BLOCK_MENU", 3)
        u.cmd(hwnd, "LINK", 0.5)
        u.popup_open(hwnd)
        s.snap(hwnd, "b07-popover-link")
        u.cmd(hwnd, "POPUP_CANCEL", 0.4)
        s.caret_in(hwnd, B_PARA)
        s.popover(hwnd, "b08-popover-table-grid", "TABLE_MENU", 3 << 4 | 4)
        s.popover(hwnd, "b09-popover-formula", "FORMULA_MENU", 1)
        s.popover(hwnd, "b10-popover-diagram", "DIAGRAM_MENU", 4)
        # the link bubble: the caret in the link's text
        u.post(hwnd, u.WM_KEYDOWN, u.VK["end"], 0, 0.1)
        for _ in range(4):
            u.post(hwnd, u.WM_KEYDOWN, u.VK["left"], 0, 0.03)
        u.wait_for(lambda: u.q(hwnd, "EDIT_BUBBLE") == 1, 2.5, 0.1)
        s.snap(hwnd, "b11-link-bubble")
        # a reference link's popover: the notice line
        s.caret_in(hwnd, B_LAST, 9)
        u.cmd(hwnd, "LINK", 0.5)
        u.popup_open(hwnd)
        s.snap(hwnd, "b12-popover-link-reference")
        u.cmd(hwnd, "POPUP_CANCEL", 0.4)
        # the code language popover
        u.click(hwnd, u.q(hwnd, "TEXT_LEFT") + s.z(40), u.q(hwnd, "BLOCK_Y", B_CODE) + s.z(16), 0.3)
        u.cmd(hwnd, "CODE_LANG", 0.5)
        u.popup_open(hwnd)
        s.snap(hwnd, "b13-popover-code-language")
        u.cmd(hwnd, "POPUP_CANCEL", 0.4)
        # a table being edited: the caret in a body cell after typing, the actions popover, a greyed button's reason
        u.click(hwnd, u.q(hwnd, "TEXT_LEFT") + s.z(14), u.q(hwnd, "BLOCK_Y", B_TABLE) + s.z(56), 0.3)
        u.post(hwnd, u.WM_KEYDOWN, u.VK["end"], 0, 0.1)
        u.type_text(hwnd, "-гамма", 0.4)
        time.sleep(TOAST)  # (the first edit's toast gone)
        s.snap(hwnd, "b14-table-editing")
        s.popover(hwnd, "b15-popover-table-actions", "TABLE_MENU", 2)
        s.hover(hwnd, u.tool_xy(hwnd, "LIST_BULLET"))
        s.snap(hwnd, "b16-table-disabled-tip")
        s.away(hwnd)
        # a new table, its header being typed
        s.caret_in(hwnd, B_ITEM2)
        u.post(hwnd, u.WM_KEYDOWN, u.VK["end"], 0, 0.1)
        u.cmd(hwnd, "NEW_PARAGRAPH", 0.4)
        u.type_text(hwnd, "Новая таблица:", 0.3)
        u.cmd_arg(hwnd, "INS_TABLE", 3 << 4 | 3, 0.6)
        u.type_text(hwnd, "Имя", 0.4)
        u.settle(hwnd)
        s.snap(hwnd, "b17-table-new")
        # the phantom row after the last block, styled as a heading
        u.testkey(hwnd, u.VK["end"], u.KM_CTRL, 0.3)
        u.cmd(hwnd, "NEW_PARAGRAPH", 0.4)
        u.cmd(hwnd, "BLOCK_H2", 0.4)
        u.settle(hwnd)
        s.snap(hwnd, "b18-phantom-heading")
        u.cmd(hwnd, "BLOCK_H2", 0.3)
        # the find bar under the bar
        u.cmd(hwnd, "FIND", 0.5)
        u.type_text(hwnd, "абзац", 0.5)
        s.snap(hwnd, "b19-find")
        s.esc(hwnd, 0.4)
        u.cmd(hwnd, "SAVE", 0.4)
        s.esc(hwnd, 0.5)
    finally:
        u.close_edit(proc, hwnd)


def narrow(s):
    """a narrow window: the collapsed bar and its "…" popover"""
    doc = s.doc("narrow.md", MAIN_DOC)
    proc, hwnd = launch(doc, s.zoom, s.theme, size="430x600")
    try:
        u.enter_edit(hwnd, B_PARA, dx=1)
        s.away(hwnd)
        s.snap(hwnd, "c01-bar-narrow")
        s.popover(hwnd, "c02-popover-more", "EDIT_MORE", 1)
        s.esc(hwnd, 0.4)
    finally:
        u.close_edit(proc, hwnd)


def popups(s):
    """every source popup, a failed formula, a selected rule"""
    doc = s.doc("popups.md", POPUP_DOC)
    (WORK / "img").mkdir(exist_ok=True)
    # a picture small enough to stay in its line (a wide one wraps onto a line of its own, whose layout is not the
    # popup's business: docs/EDIT-MODE.md Appendix B, Phase 4 notes)
    Image.open(u.REPO / "bench" / "corpus" / "img" / "diagram0.png").resize((96, 36)).save(WORK / "img" / "diagram0.png")
    proc, hwnd = launch(doc, s.zoom, s.theme)
    try:
        u.wait_for(lambda: u.q(hwnd, "MATH", 1) >= 3, 10.0, 0.1)
        u.enter_edit(hwnd, P_H1, dx=1)
        s.away(hwnd)

        def opened(name):
            u.popup_open(hwnd)
            u.popup_settled(hwnd)
            s.away(hwnd, 0.2)
            s.snap(hwnd, name)

        u.atom_after(hwnd, P_INLINE, 18)
        u.cmd(hwnd, "ATOM_EDIT", 0.4)
        opened("d01-popup-formula")
        u.popup_set(hwnd, "\\frac{")
        u.popup_settled(hwnd)
        u.settle(hwnd)
        s.snap(hwnd, "d02-popup-formula-error")
        u.cmd(hwnd, "POPUP_CANCEL", 0.4)  # (the caret after the object, nothing selected: an Esc now would leave)
        time.sleep(TOAST)  # (the first edit's toast gone)
        for block, name, dy in ((P_DISPLAY, "d03-popup-formula-block", 20), (P_MERMAID, "d04-popup-diagram", 30),
                                (P_HTML, "d05-popup-html", 8)):
            u.click(hwnd, u.q(hwnd, "TEXT_LEFT") + s.z(40), u.q(hwnd, "BLOCK_Y", block) + s.z(dy), 0.5)
            opened(name)
            u.cmd(hwnd, "POPUP_CANCEL", 0.4)
        u.atom_after(hwnd, P_PICTURE, 10)
        u.cmd(hwnd, "ATOM_EDIT", 0.4)
        opened("d06-popup-image")
        u.cmd(hwnd, "POPUP_CANCEL", 0.4)
        u.click(hwnd, u.q(hwnd, "TEXT_LEFT") + s.z(60), u.q(hwnd, "BLOCK_Y", P_HR) + s.z(2), 0.4)  # (on its 4 px line)
        s.away(hwnd)
        s.snap(hwnd, "d07-rule-selected")
        s.esc(hwnd)
        u.testkey(hwnd, u.VK["home"], u.KM_CTRL, 0.4)
        u.click(hwnd, u.q(hwnd, "TEXT_LEFT") + s.z(40), u.q(hwnd, "BLOCK_Y", P_FRONT) + s.z(12), 0.5)
        opened("d08-popup-front-matter")
        u.cmd(hwnd, "POPUP_CANCEL", 0.4)
        s.esc(hwnd, 0.5)
    finally:
        u.close_edit(proc, hwnd)


def strips(s):
    """every strip: conflict, encoding, read-only (and its status), leave, missing, recovery, another window"""
    no = {"FASTMD_TEST_ANSWER": "leave:no"}
    # conflict
    doc = s.doc("strip-conflict.md", STRIP_DOC)
    proc, hwnd = launch(doc, s.zoom, s.theme, extra=dict(no, FASTMD_AUTOSAVE_MS="60000"))
    try:
        u.enter_edit(hwnd, 1, dx=1)
        u.type_text(hwnd, "наше ", 0.3)
        doc.write_bytes(STRIP_DOC.replace("Последний абзац.", "Последний абзац от другой программы.").encode("utf-8"))
        u.wait_for(lambda: u.q(hwnd, "EDIT_CONFLICT") == 1, 3.0)
        s.away(hwnd, 0.5)
        s.snap(hwnd, "e01-strip-conflict")
        u.cmd(hwnd, "CONFLICT_LOAD", 0.5)
        s.esc(hwnd, 0.4)
    finally:
        u.close_edit(proc, hwnd)
    # encoding
    doc = s.doc("strip-encoding.md", STRIP_DOC, "cp1251")
    proc, hwnd = launch(doc, s.zoom, s.theme, extra=dict(no, FASTMD_ACP="1251", FASTMD_AUTOSAVE_MS="300"))
    try:
        u.enter_edit(hwnd, 1, dx=1)
        u.type_text(hwnd, "✓", 0.1)
        u.wait_for(lambda: u.q(hwnd, "EDIT_STRIP") == u.STRIP["ENCODING"], 3.0)
        s.away(hwnd, 0.5)
        s.snap(hwnd, "e02-strip-encoding")
        u.cmd(hwnd, "ENC_REMOVE_CHAR", 0.8)
        s.esc(hwnd, 0.4)
    finally:
        u.close_edit(proc, hwnd)
    # read-only, then the leave strip
    doc = s.doc("strip-readonly.md", STRIP_DOC)
    os.chmod(doc, stat.S_IREAD)
    proc, hwnd = launch(doc, s.zoom, s.theme, extra=no)
    try:
        u.enter_edit(hwnd, 1, dx=1)
        s.away(hwnd, 0.3)
        s.snap(hwnd, "e03-strip-readonly")
        u.type_text(hwnd, "только чтение ", 0.8)
        s.snap(hwnd, "e04-status-readonly")
        s.esc(hwnd, 0.6)
        s.snap(hwnd, "e05-strip-leave")
        u.cmd(hwnd, "DISCARD_EDITS", 0.6)
    finally:
        u.close_edit(proc, hwnd)
        os.chmod(doc, stat.S_IWRITE)
    # missing
    doc = s.doc("strip-missing.md", STRIP_DOC)
    away = doc.with_name(doc.stem + "-away.md")
    if away.exists():
        away.unlink()
    proc, hwnd = launch(doc, s.zoom, s.theme, extra=no)
    try:
        u.enter_edit(hwnd, 1, dx=1)
        u.type_text(hwnd, "пропал ", 0.3)
        u.saved(hwnd)
        doc.rename(away)
        u.type_text(hwnd, "ещё ", 0.2)
        u.wait_for(lambda: u.q(hwnd, "EDIT_STRIP") == u.STRIP["MISSING"], 4.0)
        s.away(hwnd, 0.5)
        s.snap(hwnd, "e06-strip-missing")
        away.rename(doc)
        u.saved(hwnd, 6.0)
        s.esc(hwnd, 0.4)
    finally:
        u.close_edit(proc, hwnd)
        if away.exists() and not doc.exists():
            away.rename(doc)
    # recovery: autosave off, the journal written, the process gone; the next open offers the edits
    doc = s.doc("strip-recovery.md", STRIP_DOC)
    rec = u.DATA / "recovery"
    for f in rec.glob("*") if rec.exists() else []:
        f.unlink()
    u.set_reg("Autosave", 0)
    try:
        proc, hwnd = launch(doc, s.zoom, s.theme, extra=no)
        u.enter_edit(hwnd, 1, dx=1)
        u.type_text(hwnd, "журнал ", 0.1)
        time.sleep(3.5)
        u.note_selfcheck(hwnd)
        proc.kill()
        proc.wait(5)
    finally:
        u.del_reg("Autosave")
    proc, hwnd = launch(doc, s.zoom, s.theme, extra=no)
    try:
        u.wait_for(lambda: u.q(hwnd, "EDIT_STRIP") == u.STRIP["RECOVERY"], 3.0)
        s.away(hwnd, 0.5)
        s.snap(hwnd, "e07-strip-recovery")
        u.cmd(hwnd, "RECOVERY_DELETE", 0.6)
    finally:
        u.close_edit(proc, hwnd)
    # another window edits the file
    doc = s.doc("strip-two.md", STRIP_DOC)
    a, ha = launch(doc, s.zoom, s.theme, extra=no)
    try:
        u.enter_edit(ha, 1, dx=1)
        b, hb = launch(doc, s.zoom, s.theme, extra=no)
        try:
            u.post(hb, u.WM_KEYDOWN, u.VK["f2"], 0, 0.6)
            s.away(hb, 0.4)
            s.snap(hb, "e08-strip-other-window")
        finally:
            u.close_edit(b, hb)
        s.esc(ha, 0.4)
    finally:
        u.close_edit(a, ha)


SCENES = [("reading", reading), ("bar", bar), ("narrow", narrow), ("popups", popups), ("strips", strips)]


def main():
    WORK.mkdir(parents=True, exist_ok=True)
    FINAL.mkdir(parents=True, exist_ok=True)
    u.reset_profile()
    (WORK / "other.md").write_bytes("# Другой документ\n".encode("utf-8"))
    u.set_reg("EditHintShown", 1)
    u.set_reg("UpdateCheck", 0)  # (a picture run asks GitHub nothing)
    for zoom in ZOOMS:
        for theme in THEMES:
            for name, scene in SCENES:
                if not ONLY or name in ONLY:
                    print(f"{name} {zoom} % {theme}", flush=True)
                    scene(Scene(zoom, theme))
    dumps = sorted((u.DATA / "crashes").glob("*.dmp")) if (u.DATA / "crashes").exists() else []
    problems = u.SELFCHECK + u.CLOSE_PROBLEMS + [f"crash dump {d}" for d in dumps]
    for p in problems:
        print("problem:", p)
    if not problems:
        u.reset_profile()  # (kept when something went wrong: the dumps are in it)
    print(f"{len(list(FINAL.glob('*.png')))} pictures in {FINAL}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
