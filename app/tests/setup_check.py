"""Installer check (plan 5.4): install, look at what landed where, run it, uninstall, look again.

The install is per-user and touches nothing outside %LOCALAPPDATA%\\Programs\\FastMD, the Start menu shortcut and one
HKCU key. It is run with /noassoc, so the .md association of the machine is left exactly as it was.

    python app/tests/setup_check.py [--build DIR]
"""
import argparse
import os
import pathlib
import subprocess
import time
import winreg

HERE = pathlib.Path(__file__).resolve().parent
UNINSTALL_KEY = r"Software\Microsoft\Windows\CurrentVersion\Uninstall\FastMD"
PAYLOAD = ["FastMD.exe", "fastmd-svg.dll", "fastmd-tex.dll", "fastmd-mermaid.dll", "LICENSE.txt",
           "THIRD-PARTY-md4c.txt", "THIRD-PARTY-lunasvg.txt", "THIRD-PARTY-plutovg.txt", "THIRD-PARTY-ratex.txt",
           "THIRD-PARTY-katex-fonts.txt", "THIRD-PARTY-mermaid-rs-renderer.txt", "FastMD-Setup.exe"]
ok_all = True


def check(name, cond, info=""):
    global ok_all
    ok_all &= bool(cond)
    print(f"[{'OK' if cond else 'FAIL'}] {name}" + (f": {info}" if info else ""))
    return bool(cond)


def reg_value(name):
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, UNINSTALL_KEY) as k:
            return winreg.QueryValueEx(k, name)[0]
    except OSError:
        return None


def association_command():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Classes\FastMD.Markdown\shell\open\command") as k:
            return winreg.QueryValueEx(k, "")[0]
    except OSError:
        return None


def run(exe, *args, wait=True):
    p = subprocess.Popen([str(exe), *args])
    if wait:
        p.wait(timeout=120)
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=str(HERE.parent / "build" / "Release"))
    args = ap.parse_args()
    setup = pathlib.Path(args.build) / "FastMD-Setup.exe"
    target = pathlib.Path(os.environ["LOCALAPPDATA"]) / "Programs" / "FastMD"
    shortcut = pathlib.Path(os.environ["APPDATA"]) / "Microsoft/Windows/Start Menu/Programs/FastMD.lnk"
    assoc_before = association_command()

    check("5.4 the installer was built", setup.exists(), f"{setup.stat().st_size if setup.exists() else 0} bytes")
    if not setup.exists():
        return 1
    if target.exists():
        print(f"note: {target} already exists — the check will overwrite and then remove it")

    run(setup, "/S", "/noassoc")
    time.sleep(1.0)
    missing = [n for n in PAYLOAD if not (target / n).exists()]
    check("5.4 every file is in place", not missing, ", ".join(missing) or str(target))
    check("5.4 the program is the one that was built",
          (target / "FastMD.exe").stat().st_size == (pathlib.Path(args.build) / "FastMD.exe").stat().st_size)
    check("5.4 a shortcut is in the Start menu", shortcut.exists(), str(shortcut))
    check("5.4 it shows up in Apps with a way to remove it",
          reg_value("DisplayName") == "FastMD" and reg_value("UninstallString", ) and
          "FastMD-Setup.exe" in (reg_value("UninstallString") or ""), f'{reg_value("DisplayVersion")}')
    check("5.4 it knows where it put itself", (reg_value("InstallLocation") or "").rstrip("\\").lower() ==
          str(target).rstrip("\\").lower(), reg_value("InstallLocation"))
    check("5.4 /noassoc left the .md association alone", association_command() == assoc_before,
          f"{assoc_before} → {association_command()}")

    # the installed copy has to actually run: open a document and see the window come up
    doc = HERE / "out" / "setup-probe.md"
    doc.parent.mkdir(exist_ok=True)
    doc.write_text("# Установленная копия\n\nОкно открылось.\n", encoding="utf-8")
    proc = run(target / "FastMD.exe", str(doc), wait=False)
    time.sleep(2.5)
    import ctypes
    import ctypes.wintypes as wt
    u32 = ctypes.WinDLL("user32", use_last_error=True)
    found = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def each(h, _):
        pid = wt.DWORD()
        u32.GetWindowThreadProcessId(h, ctypes.byref(pid))
        if pid.value == proc.pid and u32.IsWindowVisible(h):
            buf = ctypes.create_unicode_buffer(300)
            u32.GetWindowTextW(h, buf, 300)
            if buf.value:
                found.append(buf.value)
        return True

    u32.EnumWindows(each, 0)
    check("5.4 the installed copy opens a document", any("setup-probe.md" in t for t in found), str(found))
    for h in ():
        pass
    proc.terminate()
    proc.wait(timeout=30)

    run(setup, "/uninstall", "/S")
    for _ in range(30):  # the uninstaller re-runs itself from %TEMP%, so give it a moment
        if not target.exists():
            break
        time.sleep(0.5)
    check("5.4 uninstalling removes the folder", not target.exists(), str(target))
    check("5.4 and the shortcut", not shortcut.exists())
    check("5.4 and the entry in Apps", reg_value("DisplayName") is None)
    check("5.4 and still leaves the .md association alone", association_command() == assoc_before)
    print("RESULT", "PASS" if ok_all else "FAIL")
    return 0 if ok_all else 1


if __name__ == "__main__":
    raise SystemExit(main())
