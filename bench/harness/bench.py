"""FastMD benchmark harness — see bench/PROTOCOL.md.

Windows only. Stdlib + ctypes (Pillow only for screenshots).
Every launched app runs inside a Job Object, so child processes (WebView2 / Electron renderers)
are accounted (CPU, peak commit) and torn down together.
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import datetime
import json
import os
import pathlib
import platform
import shutil
import statistics
import subprocess
import sys
import threading
import time

BENCH = pathlib.Path(__file__).resolve().parents[1]
PROTOS = BENCH / "protos"
CORPUS = BENCH / "corpus"
RESULTS = BENCH / "results"

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
u32 = ctypes.WinDLL("user32", use_last_error=True)
gdi = ctypes.WinDLL("gdi32", use_last_error=True)
dwm = ctypes.WinDLL("dwmapi")
adv = ctypes.WinDLL("advapi32", use_last_error=True)
ntdll = ctypes.WinDLL("ntdll")

# ----------------------------------------------------------------------------- Win32 declarations

CREATE_SUSPENDED = 0x4
CREATE_UNICODE_ENVIRONMENT = 0x400
JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x2000
WM_CLOSE = 0x0010
PW_RENDERFULLCONTENT = 2
DWMWA_EXTENDED_FRAME_BOUNDS = 9
DWMWA_CLOAKED = 14


class STARTUPINFOW(ctypes.Structure):
    _fields_ = [("cb", wt.DWORD), ("lpReserved", wt.LPWSTR), ("lpDesktop", wt.LPWSTR), ("lpTitle", wt.LPWSTR),
                ("dwX", wt.DWORD), ("dwY", wt.DWORD), ("dwXSize", wt.DWORD), ("dwYSize", wt.DWORD),
                ("dwXCountChars", wt.DWORD), ("dwYCountChars", wt.DWORD), ("dwFillAttribute", wt.DWORD),
                ("dwFlags", wt.DWORD), ("wShowWindow", wt.WORD), ("cbReserved2", wt.WORD),
                ("lpReserved2", ctypes.c_void_p), ("hStdInput", wt.HANDLE), ("hStdOutput", wt.HANDLE),
                ("hStdError", wt.HANDLE)]


class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [("hProcess", wt.HANDLE), ("hThread", wt.HANDLE), ("dwProcessId", wt.DWORD), ("dwThreadId", wt.DWORD)]


class JOBOBJECT_BASIC_LIMIT_INFORMATION(ctypes.Structure):
    _fields_ = [("PerProcessUserTimeLimit", ctypes.c_int64), ("PerJobUserTimeLimit", ctypes.c_int64),
                ("LimitFlags", wt.DWORD), ("MinimumWorkingSetSize", ctypes.c_size_t),
                ("MaximumWorkingSetSize", ctypes.c_size_t), ("ActiveProcessLimit", wt.DWORD),
                ("Affinity", ctypes.c_size_t), ("PriorityClass", wt.DWORD), ("SchedulingClass", wt.DWORD)]


class IO_COUNTERS(ctypes.Structure):
    _fields_ = [(n, ctypes.c_uint64) for n in ("ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
                                               "ReadTransferCount", "WriteTransferCount", "OtherTransferCount")]


class JOBOBJECT_EXTENDED_LIMIT_INFORMATION(ctypes.Structure):
    _fields_ = [("BasicLimitInformation", JOBOBJECT_BASIC_LIMIT_INFORMATION), ("IoInfo", IO_COUNTERS),
                ("ProcessMemoryLimit", ctypes.c_size_t), ("JobMemoryLimit", ctypes.c_size_t),
                ("PeakProcessMemoryUsed", ctypes.c_size_t), ("PeakJobMemoryUsed", ctypes.c_size_t)]


class JOBOBJECT_BASIC_ACCOUNTING_INFORMATION(ctypes.Structure):
    _fields_ = [("TotalUserTime", ctypes.c_int64), ("TotalKernelTime", ctypes.c_int64),
                ("ThisPeriodTotalUserTime", ctypes.c_int64), ("ThisPeriodTotalKernelTime", ctypes.c_int64),
                ("TotalPageFaultCount", wt.DWORD), ("TotalProcesses", wt.DWORD), ("ActiveProcesses", wt.DWORD),
                ("TotalTerminatedProcesses", wt.DWORD)]


class JOBOBJECT_BASIC_PROCESS_ID_LIST(ctypes.Structure):
    _fields_ = [("NumberOfAssignedProcesses", wt.DWORD), ("NumberOfProcessIdsInList", wt.DWORD),
                ("ProcessIdList", ctypes.c_size_t * 1024)]


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG), ("biPlanes", wt.WORD),
                ("biBitCount", wt.WORD), ("biCompression", wt.DWORD), ("biSizeImage", wt.DWORD),
                ("biXPelsPerMeter", wt.LONG), ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD),
                ("biClrImportant", wt.DWORD)]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", wt.DWORD * 3)]


WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

k32.GetSystemTimePreciseAsFileTime.argtypes = [ctypes.POINTER(ctypes.c_ulonglong)]
k32.CreateJobObjectW.restype = wt.HANDLE
k32.CreateJobObjectW.argtypes = [ctypes.c_void_p, wt.LPCWSTR]
k32.SetInformationJobObject.argtypes = [wt.HANDLE, ctypes.c_int, ctypes.c_void_p, wt.DWORD]
k32.QueryInformationJobObject.argtypes = [wt.HANDLE, ctypes.c_int, ctypes.c_void_p, wt.DWORD, ctypes.c_void_p]
k32.AssignProcessToJobObject.argtypes = [wt.HANDLE, wt.HANDLE]
k32.TerminateJobObject.argtypes = [wt.HANDLE, wt.UINT]
k32.CreateProcessW.argtypes = [wt.LPCWSTR, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, wt.BOOL, wt.DWORD,
                               ctypes.c_void_p, wt.LPCWSTR, ctypes.POINTER(STARTUPINFOW),
                               ctypes.POINTER(PROCESS_INFORMATION)]
k32.ResumeThread.argtypes = [wt.HANDLE]
k32.TerminateProcess.argtypes = [wt.HANDLE, wt.UINT]
k32.CloseHandle.argtypes = [wt.HANDLE]
k32.WaitForSingleObject.argtypes = [wt.HANDLE, wt.DWORD]
k32.WaitForSingleObject.restype = wt.DWORD
k32.GetExitCodeProcess.argtypes = [wt.HANDLE, ctypes.POINTER(wt.DWORD)]
k32.GetCurrentProcess.restype = wt.HANDLE

u32.EnumWindows.argtypes = [WNDENUMPROC, wt.LPARAM]
u32.IsWindowVisible.argtypes = [wt.HWND]
u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
u32.GetWindowTextLengthW.argtypes = [wt.HWND]
u32.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u32.PrintWindow.argtypes = [wt.HWND, wt.HDC, wt.UINT]
u32.GetDC.argtypes = [wt.HWND]
u32.GetDC.restype = wt.HDC
u32.ReleaseDC.argtypes = [wt.HWND, wt.HDC]
u32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u32.SetForegroundWindow.argtypes = [wt.HWND]
gdi.CreateCompatibleDC.argtypes = [wt.HDC]
gdi.CreateCompatibleDC.restype = wt.HDC
gdi.CreateCompatibleBitmap.argtypes = [wt.HDC, ctypes.c_int, ctypes.c_int]
gdi.CreateCompatibleBitmap.restype = wt.HBITMAP
gdi.SelectObject.argtypes = [wt.HDC, wt.HGDIOBJ]
gdi.SelectObject.restype = wt.HGDIOBJ
gdi.GetDIBits.argtypes = [wt.HDC, wt.HBITMAP, wt.UINT, wt.UINT, ctypes.c_void_p, ctypes.POINTER(BITMAPINFO), wt.UINT]
gdi.DeleteObject.argtypes = [wt.HGDIOBJ]
gdi.DeleteDC.argtypes = [wt.HDC]
dwm.DwmGetWindowAttribute.argtypes = [wt.HWND, wt.DWORD, ctypes.c_void_p, wt.DWORD]

try:  # physical pixels for window rects / screenshots
    u32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    pass


def now_ft() -> int:
    v = ctypes.c_ulonglong()
    k32.GetSystemTimePreciseAsFileTime(ctypes.byref(v))
    return v.value


def ms(a: int | None, b: int | None) -> float | None:
    return None if a is None or b is None else round((a - b) / 10_000, 2)


# ----------------------------------------------------------------------------- jobs & processes

def create_job():
    job = k32.CreateJobObjectW(None, None)
    if not job:
        raise ctypes.WinError(ctypes.get_last_error())
    info = JOBOBJECT_EXTENDED_LIMIT_INFORMATION()
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    k32.SetInformationJobObject(job, 9, ctypes.byref(info), ctypes.sizeof(info))
    return job


def job_pids(job) -> set[int]:
    info = JOBOBJECT_BASIC_PROCESS_ID_LIST()
    if not k32.QueryInformationJobObject(job, 3, ctypes.byref(info), ctypes.sizeof(info), None):
        return set()
    return set(info.ProcessIdList[:info.NumberOfProcessIdsInList])


def job_accounting(job) -> JOBOBJECT_BASIC_ACCOUNTING_INFORMATION:
    a = JOBOBJECT_BASIC_ACCOUNTING_INFORMATION()
    k32.QueryInformationJobObject(job, 1, ctypes.byref(a), ctypes.sizeof(a), None)
    return a


def job_peak_commit(job) -> int:
    e = JOBOBJECT_EXTENDED_LIMIT_INFORMATION()
    k32.QueryInformationJobObject(job, 9, ctypes.byref(e), ctypes.sizeof(e), None)
    return e.PeakJobMemoryUsed


def wait_job_empty(job, timeout_s: float) -> bool:
    end = time.perf_counter() + timeout_s
    while time.perf_counter() < end:
        if job_accounting(job).ActiveProcesses == 0:
            return True
        time.sleep(0.01)
    return job_accounting(job).ActiveProcesses == 0


def env_block(extra: dict[str, str]):
    env = dict(os.environ)
    env.update(extra)
    s = "".join(f"{k}={v}\0" for k, v in sorted(env.items(), key=lambda kv: kv[0].upper())) + "\0"
    return (ctypes.c_wchar * len(s))(*s)


def launch(cmdline: list[str], env: dict[str, str], cwd: str, job):
    cmd = subprocess.list2cmdline(cmdline)
    cmdbuf = ctypes.create_unicode_buffer(cmd)
    block = env_block(env)
    si = STARTUPINFOW()
    si.cb = ctypes.sizeof(si)
    pi = PROCESS_INFORMATION()
    t0 = now_ft()
    if not k32.CreateProcessW(None, cmdbuf, None, None, False, CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                              block, cwd, ctypes.byref(si), ctypes.byref(pi)):
        raise ctypes.WinError(ctypes.get_last_error())
    if not k32.AssignProcessToJobObject(job, pi.hProcess):
        err = ctypes.get_last_error()
        k32.TerminateProcess(pi.hProcess, 1)
        raise ctypes.WinError(err)
    k32.ResumeThread(pi.hThread)
    t_resumed = now_ft()
    k32.CloseHandle(pi.hThread)
    return t0, t_resumed, pi.hProcess, pi.dwProcessId


def exit_code(hproc) -> int | None:
    if k32.WaitForSingleObject(hproc, 0) != 0:
        return None
    c = wt.DWORD()
    k32.GetExitCodeProcess(hproc, ctypes.byref(c))
    return c.value


# ----------------------------------------------------------------------------- windows

def window_title(hwnd) -> str:
    n = u32.GetWindowTextLengthW(hwnd)
    buf = ctypes.create_unicode_buffer(n + 1)
    u32.GetWindowTextW(hwnd, buf, n + 1)
    return buf.value


def list_windows(pids: set[int] | None, title: str | None, ignore: set[int] = frozenset()):
    """Visible, non-cloaked, reasonably sized top-level windows, filtered by pid set or title substring."""
    found = []

    def cb(hwnd, _):
        if hwnd in ignore or not u32.IsWindowVisible(hwnd):
            return True
        cloaked = wt.DWORD()
        if dwm.DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, ctypes.byref(cloaked), 4) == 0 and cloaked.value:
            return True
        r = wt.RECT()
        u32.GetWindowRect(hwnd, ctypes.byref(r))
        w, h = r.right - r.left, r.bottom - r.top
        if w < 200 or h < 150:
            return True
        if title is not None:
            t = window_title(hwnd)
            # "FastMD (" = our own prototypes (PROTOCOL §5 title format) — never match them in external mode
            if title.lower() in t.lower() and "FastMD (" not in t:
                found.append((hwnd, w * h))
        elif pids:
            pid = wt.DWORD()
            u32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value in pids:
                found.append((hwnd, w * h))
        return True

    u32.EnumWindows(WNDENUMPROC(cb), 0)
    return found


class Observer(threading.Thread):
    """Polls (~1 ms) for the first qualifying window; independent cross-check of the self-reported marks."""

    def __init__(self, job, extra_pids: set[int], title: str | None, ignore: set[int]):
        super().__init__(daemon=True)
        self.job, self.extra, self.title, self.ignore = job, extra_pids, title, ignore
        self.t_first: int | None = None
        self.hwnd = None
        self._stop = threading.Event()

    def run(self):
        while not self._stop.is_set():
            pids = None if self.title else (job_pids(self.job) | self.extra)
            hits = list_windows(pids, self.title, self.ignore)
            if hits:
                self.t_first = now_ft()
                self.hwnd = max(hits, key=lambda h: h[1])[0]
                return
            time.sleep(0.001)

    def stop(self):
        self._stop.set()


def capture_window(hwnd, path: pathlib.Path) -> bool:
    from PIL import Image, ImageGrab

    r = wt.RECT()
    u32.GetWindowRect(hwnd, ctypes.byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    sdc = u32.GetDC(None)
    mdc = gdi.CreateCompatibleDC(sdc)
    bmp = gdi.CreateCompatibleBitmap(sdc, w, h)
    old = gdi.SelectObject(mdc, bmp)
    u32.PrintWindow(hwnd, mdc, PW_RENDERFULLCONTENT)
    gdi.SelectObject(mdc, old)
    bmi = BITMAPINFO()
    bmi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bmi.bmiHeader.biWidth, bmi.bmiHeader.biHeight = w, -h
    bmi.bmiHeader.biPlanes, bmi.bmiHeader.biBitCount = 1, 32
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi.GetDIBits(mdc, bmp, 0, h, buf, ctypes.byref(bmi), 0)
    gdi.DeleteObject(bmp)
    gdi.DeleteDC(mdc)
    u32.ReleaseDC(None, sdc)
    img = Image.frombuffer("RGB", (w, h), buf, "raw", "BGRX", 0, 1)
    ext = wt.RECT()
    if dwm.DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, ctypes.byref(ext), ctypes.sizeof(ext)) == 0:
        img = img.crop((ext.left - r.left, ext.top - r.top, ext.right - r.left, ext.bottom - r.top))
        box = (ext.left, ext.top, ext.right, ext.bottom)
    else:
        box = (r.left, r.top, r.right, r.bottom)
    if img.convert("L").getextrema()[1] < 8:  # PrintWindow gave a black image -> grab the screen instead
        u32.SetForegroundWindow(hwnd)
        time.sleep(0.4)
        img = ImageGrab.grab(bbox=box, all_screens=True)
    path.parent.mkdir(parents=True, exist_ok=True)
    img.save(path)
    return True


# ----------------------------------------------------------------------------- variants

def load_variants() -> dict[str, dict]:
    out = {}
    for pj in sorted(PROTOS.glob("*/proto.json")):
        try:
            proto = json.loads(pj.read_text(encoding="utf-8"))
        except Exception as e:  # noqa: BLE001
            print(f"!! bad {pj}: {e}", file=sys.stderr)
            continue
        for v in proto.get("variants", []):
            v = dict(v)
            v["_dir"] = pj.parent
            v["_proto"] = proto.get("id", pj.parent.name)
            v["_proto_name"] = proto.get("name", "")
            v.setdefault("mode", "self")
            pd = str(pj.parent.resolve())
            v["args"] = [str(a).replace("{proto_dir}", pd) for a in v.get("args", [])]
            v["env"] = {k: str(val).replace("{proto_dir}", pd) for k, val in v.get("env", {}).items()}
            for key in ("exe", "dist_dir"):  # %LOCALAPPDATA% etc.: keeps user-specific paths out of proto.json
                if key in v:
                    v[key] = os.path.expandvars(v[key])
            out[v["id"]] = v
    return out


def resolve_exe(v: dict) -> str:
    exe = v["exe"]
    p = (v["_dir"] / exe)
    if p.exists():
        return str(p.resolve())
    if pathlib.Path(exe).is_absolute() and pathlib.Path(exe).exists():
        return exe
    found = shutil.which(exe)
    if found:
        return found
    raise FileNotFoundError(f"{v['id']}: exe not found: {exe}")


def dist_size(v: dict) -> tuple[float | None, int | None]:
    d = v.get("dist_dir")
    if not d:
        return None, None
    p = v["_dir"] / d
    if not p.exists():
        return None, None
    files = [f for f in p.rglob("*") if f.is_file()]
    return round(sum(f.stat().st_size for f in files) / 1_048_576, 2), len(files)


def doc_path(name: str) -> pathlib.Path:
    p = pathlib.Path(name)
    if p.suffix != ".md":
        p = CORPUS / f"{name}.md"
    return p.resolve()


class Resident:
    def __init__(self, v: dict):
        self.v = v
        self.job = None
        self.pids: set[int] = set()

    def __enter__(self):
        spec = self.v.get("resident")
        if spec:
            self.job = create_job()
            exe = str((self.v["_dir"] / spec["exe"]).resolve())
            _, _, hproc, pid = launch([exe] + spec.get("args", []), spec.get("env", {}), str(pathlib.Path(exe).parent),
                                      self.job)
            k32.CloseHandle(hproc)
            time.sleep(spec.get("ready_wait_ms", 3000) / 1000)
            self.pids = job_pids(self.job) or {pid}
        return self

    def __exit__(self, *exc):
        if self.job:
            k32.TerminateJobObject(self.job, 1)
            wait_job_empty(self.job, 5)
            k32.CloseHandle(self.job)


def run_once(v: dict, doc: pathlib.Path, resident: Resident, *, bench_exit=True, shot: pathlib.Path | None = None,
             settle_ms=1500, timeout_s=30.0) -> dict:
    mode = v["mode"]
    tmpdir = RESULTS / "tmp"
    tmpdir.mkdir(parents=True, exist_ok=True)
    out = tmpdir / f"{v['id']}-{os.getpid()}.json"  # per-invocation: concurrent harnesses must not share it
    for f in (out, out.with_name(out.name + ".tmp")):
        if f.exists():
            f.unlink()
    env = dict(v["env"])
    if mode == "self":
        env["FASTMD_BENCH_OUT"] = str(out)
        env["FASTMD_BENCH_EXIT"] = "1" if bench_exit else "0"
    title = doc.name if mode == "external-title" else None
    ignore = {h for h, _ in list_windows(None, title)} if title else set()

    job = create_job()
    obs = Observer(job, resident.pids, title, ignore)
    obs.start()
    rec: dict = {"variant": v["id"], "doc": doc.stem, "ok": False, "error": None}
    hproc = None
    try:
        exe = resolve_exe(v)
        t0, t_res, hproc, pid = launch([exe] + v["args"] + [str(doc)], env, str(pathlib.Path(exe).parent), job)
        rec["launch_overhead_ms"] = ms(t_res, t0)
        deadline = time.perf_counter() + timeout_s
        result = None
        exited_at = None
        while time.perf_counter() < deadline:
            if mode == "self":
                if out.exists():
                    try:
                        result = json.loads(out.read_text(encoding="utf-8"))
                        break
                    except (json.JSONDecodeError, PermissionError):
                        pass
            elif obs.t_first is not None:
                break
            code = exit_code(hproc)
            if code is not None and not v.get("resident") and mode == "self":
                exited_at = exited_at or time.perf_counter()
                if code != 0 or time.perf_counter() - exited_at > 1.0:
                    # a launcher may exit early; give the job's children 1 s to report, unless it crashed
                    if code != 0 or job_accounting(job).ActiveProcesses == 0:
                        rec["error"] = f"process exited with code {code} before reporting"
                        break
            time.sleep(0.002)
        acc = job_accounting(job)
        rec["cpu_ms"] = round((acc.TotalUserTime + acc.TotalKernelTime) / 10_000, 1)
        rec["processes"] = acc.TotalProcesses
        rec["peak_commit_mb"] = round(job_peak_commit(job) / 1_048_576, 1)
        if mode == "self" and result is not None:
            tc = result.get("t_content")
            rec["content_ms"] = ms(tc, t0)
            rec["window_ms"] = ms(result.get("t_window"), t0)
            rec["ws_mb"] = round(result["ws_bytes"] / 1_048_576, 1) if result.get("ws_bytes") else None
            rec["marks_ms"] = {k: ms(val, t0) for k, val in (result.get("marks") or {}).items()}
            rec["notes"] = result.get("notes")
            rec["ok"] = isinstance(tc, int) and tc > t0
            if not rec["ok"]:
                rec["error"] = f"bad t_content {tc!r}"
        elif mode == "external-title" and obs.t_first is not None:
            rec["content_ms"] = ms(obs.t_first, t0)
            rec["ok"] = True
        elif rec["error"] is None:
            rec["error"] = "timeout"
        rec["observed_ms"] = ms(obs.t_first, t0)

        if shot is not None and rec["ok"]:
            time.sleep(settle_ms / 1000)
            hits = list_windows(None, title) if title else list_windows(job_pids(job) | resident.pids, None)
            if hits:
                capture_window(max(hits, key=lambda h: h[1])[0], shot)
                rec["shot"] = str(shot)
            else:
                rec["error"] = "no window to capture"

        if mode == "self" and bench_exit and rec["ok"] and not v.get("resident"):
            rec["clean_exit"] = wait_job_empty(job, 5)
            code = exit_code(hproc)
            rec["exit_code"] = code
        if mode == "external-title" and obs.hwnd:
            u32.PostMessageW(obs.hwnd, WM_CLOSE, 0, 0)
            time.sleep(0.5)
    except Exception as e:  # noqa: BLE001
        rec["error"] = f"{type(e).__name__}: {e}"
    finally:
        obs.stop()
        k32.TerminateJobObject(job, 1)
        wait_job_empty(job, 5)
        if hproc:
            k32.CloseHandle(hproc)
        k32.CloseHandle(job)
    return rec


# ----------------------------------------------------------------------------- statistics / report

def pct(xs: list[float], p: float) -> float:
    xs = sorted(xs)
    k = (len(xs) - 1) * p
    lo, hi = int(k), min(int(k) + 1, len(xs) - 1)
    return round(xs[lo] + (xs[hi] - xs[lo]) * (k - lo), 1)


def summarize(runs: list[dict], variants: dict[str, dict]) -> list[dict]:
    groups: dict[tuple[str, str], list[dict]] = {}
    for r in runs:
        groups.setdefault((r["variant"], r["doc"]), []).append(r)
    rows = []
    for (vid, doc), rs in groups.items():
        ok = [r for r in rs if r["ok"]]
        steady = [r["content_ms"] for r in ok[1:]] or [r["content_ms"] for r in ok]

        def med(key):
            xs = [r[key] for r in ok[1:] if r.get(key) is not None] or [r[key] for r in ok if r.get(key) is not None]
            return round(statistics.median(xs), 1) if xs else None

        size_mb, nfiles = dist_size(variants[vid]) if vid in variants else (None, None)
        rows.append({
            "variant": vid, "doc": doc, "runs": len(rs), "failed": len(rs) - len(ok),
            "first_ms": ok[0]["content_ms"] if ok else None,
            "median_ms": round(statistics.median(steady), 1) if steady else None,
            "p90_ms": pct(steady, 0.9) if steady else None,
            "min_ms": round(min(steady), 1) if steady else None,
            "stdev_ms": round(statistics.stdev(steady), 1) if len(steady) > 1 else None,
            "window_ms": med("window_ms"), "observed_ms": med("observed_ms"), "cpu_ms": med("cpu_ms"),
            "peak_commit_mb": med("peak_commit_mb"), "ws_mb": med("ws_mb"), "processes": med("processes"),
            "dist_mb": size_mb, "dist_files": nfiles,
            "errors": sorted({r["error"] for r in rs if r.get("error")}),
        })
    rows.sort(key=lambda r: (r["doc"], r["median_ms"] if r["median_ms"] is not None else 1e9))
    return rows


def print_table(rows: list[dict]) -> None:
    cols = [("variant", "variant"), ("doc", "doc"), ("n", "runs"), ("fail", "failed"), ("first", "first_ms"),
            ("median", "median_ms"), ("p90", "p90_ms"), ("min", "min_ms"), ("sd", "stdev_ms"),
            ("win", "window_ms"), ("obs", "observed_ms"), ("cpu", "cpu_ms"), ("commitMB", "peak_commit_mb"),
            ("wsMB", "ws_mb"), ("procs", "processes"), ("distMB", "dist_mb")]
    print("| " + " | ".join(c for c, _ in cols) + " |")
    print("|" + "|".join("---" for _ in cols) + "|")
    for r in rows:
        print("| " + " | ".join("" if r.get(k) is None else str(r.get(k)) for _, k in cols) + " |")
    errs = [(r["variant"], r["doc"], e) for r in rows for e in r["errors"]]
    for vid, doc, e in errs:
        print(f"  ! {vid}/{doc}: {e}")


def machine_meta() -> dict:
    return {"when": datetime.datetime.now().isoformat(timespec="seconds"), "host": platform.node(),
            "os": platform.platform(), "cpu_count": os.cpu_count(), "python": sys.version.split()[0]}


# ----------------------------------------------------------------------------- commands

def cmd_list(args, variants):
    for vid, v in variants.items():
        try:
            exe = resolve_exe(v)
            state = "ok"
        except FileNotFoundError:
            exe, state = v["exe"], "MISSING"
        size, n = dist_size(v)
        print(f"{vid:28} {state:8} {v['mode']:15} {size or '':>8} MB  {exe}")


def cmd_check(args, variants):
    v = variants[args.variant]
    doc = doc_path(args.doc)
    with Resident(v) as res:
        rec = run_once(v, doc, res, timeout_s=args.timeout)
    print(json.dumps(rec, indent=2, ensure_ascii=False))
    problems = []
    if not rec["ok"]:
        problems.append(f"FAILED: {rec.get('error')}")
    else:
        if v["mode"] == "self":
            if rec.get("window_ms") is None:
                problems.append("warn: t_window not reported (recommended)")
            elif rec["window_ms"] > rec["content_ms"] + 0.5:
                problems.append("warn: t_window is after t_content")
            for k, val in (rec.get("marks_ms") or {}).items():
                if val is not None and (val < 0 or val > rec["content_ms"] + 0.5):
                    problems.append(f"warn: mark {k}={val} ms outside [0, t_content]")
            if rec.get("observed_ms") is not None and rec["observed_ms"] > rec["content_ms"] + 50:
                problems.append("warn: harness saw the window >50 ms after t_content — is t_content too early?")
            if rec.get("clean_exit") is False:
                problems.append("warn: app did not exit within 5 s after reporting (was killed)")
            if rec.get("exit_code") not in (0, None):
                problems.append(f"warn: exit code {rec.get('exit_code')}")
    for p in problems:
        print(p)
    print("CHECK", "FAILED" if not rec["ok"] else "OK")
    return 0 if rec["ok"] else 1


def cmd_shot(args, variants):
    v = variants[args.variant]
    doc = doc_path(args.doc)
    path = pathlib.Path(args.out) if args.out else RESULTS / "shots" / f"{v['id']}-{doc.stem}.png"
    with Resident(v) as res:
        rec = run_once(v, doc, res, bench_exit=False, shot=path, settle_ms=args.settle, timeout_s=args.timeout)
    print(json.dumps(rec, indent=2, ensure_ascii=False))
    return 0 if rec.get("shot") else 1


def cmd_run(args, variants):
    ids = list(variants) if args.variant == "all" else args.variant.split(",")
    if args.skip:
        ids = [i for i in ids if i not in set(args.skip.split(","))]
    todo = []
    for vid in ids:
        v = variants[vid]
        try:
            resolve_exe(v)
            todo.append(v)
        except FileNotFoundError as e:
            print(f"skip {vid}: {e}", file=sys.stderr)
    docs = [doc_path(d) for d in args.doc.split(",")]
    total = args.runs + 1
    runs: list[dict] = []
    residents = {v["id"]: Resident(v).__enter__() for v in todo if v.get("resident")}
    try:
        if args.order == "grouped":
            order = [(v, d, i) for v in todo for d in docs for i in range(total)]
        else:  # interleaved: round-robin over variants to spread drift / background noise evenly
            order = [(v, d, i) for i in range(total) for d in docs for v in todo]
        for n, (v, d, i) in enumerate(order, 1):
            rec = run_once(v, d, residents.get(v["id"]) or Resident({}), timeout_s=args.timeout)
            rec["run"] = i
            runs.append(rec)
            flag = "ok" if rec["ok"] else f"FAIL {rec.get('error')}"
            print(f"[{n}/{len(order)}] {v['id']:28} {d.stem:7} #{i:<2} content={rec.get('content_ms')} ms  "
                  f"window={rec.get('window_ms')}  obs={rec.get('observed_ms')}  {flag}", flush=True)
            time.sleep(args.gap / 1000)
    finally:
        for r in residents.values():
            r.__exit__(None, None, None)
    rows = summarize(runs, variants)
    out = pathlib.Path(args.out) if args.out else RESULTS / f"run-{datetime.datetime.now():%Y%m%d-%H%M%S}.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps({"meta": machine_meta() | {"args": vars(args)}, "summary": rows, "runs": runs},
                              indent=1, ensure_ascii=False), encoding="utf-8")
    print()
    print_table(rows)
    print(f"\nsaved {out}")
    return 0


def cmd_report(args, variants):
    data = json.loads(pathlib.Path(args.file).read_text(encoding="utf-8"))
    print_table(summarize(data["runs"], variants))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    p = sub.add_parser("check")
    p.add_argument("variant")
    p.add_argument("--doc", default="medium")
    p.add_argument("--timeout", type=float, default=30)
    p = sub.add_parser("shot")
    p.add_argument("variant")
    p.add_argument("--doc", default="medium")
    p.add_argument("--settle", type=int, default=1500)
    p.add_argument("--out")
    p.add_argument("--timeout", type=float, default=30)
    p = sub.add_parser("run")
    p.add_argument("variant", help="variant id, comma list, or 'all'")
    p.add_argument("--doc", default="small,medium,large")
    p.add_argument("--runs", type=int, default=15, help="measured runs after the first one")
    p.add_argument("--gap", type=int, default=400, help="pause between runs, ms")
    p.add_argument("--order", choices=["interleaved", "grouped"], default="interleaved")
    p.add_argument("--skip", help="comma list of variant ids to skip")
    p.add_argument("--timeout", type=float, default=30)
    p.add_argument("--out")
    p = sub.add_parser("report")
    p.add_argument("file")
    args = ap.parse_args()
    variants = load_variants()
    if getattr(args, "variant", None) not in (None, "all") and args.cmd in ("check", "shot") \
            and args.variant not in variants:
        print(f"unknown variant {args.variant}; known: {', '.join(variants)}", file=sys.stderr)
        return 2
    return {"list": cmd_list, "check": cmd_check, "shot": cmd_shot, "run": cmd_run, "report": cmd_report}[args.cmd](
        args, variants)


if __name__ == "__main__":
    sys.exit(main())
