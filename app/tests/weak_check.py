"""What can be measured about weak hardware on this machine (plan 6.6).

A real check needs a laptop with an integrated GPU, four cores and a 4K screen at 150 %. Short of that, this measures
the two things that hardware would stress, on the hardware there is:

  * scrolling: frame times for a full-screen window, from the viewer's own --scroll-test;
  * memory: working set and commit after a 3.7 MB document is open and fully measured.

    python app/tests/weak_check.py [--exe PATH] [--frames 200]

The startup side is measured by the harness instead, with the process pinned to fewer cores:
    python bench/harness/bench.py run fastmd-app,baseline-win32 --doc medium --runs 10 --cpus 4
"""
import argparse
import ctypes
import os
import pathlib
import subprocess
import time

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]


def scroll_frames(exe, doc, frames, size):
    out = pathlib.Path(os.environ["TEMP"]) / "fastmd-scroll.txt"
    out.unlink(missing_ok=True)
    env = dict(os.environ, FASTMD_REGKEY=r"Software\FastMD-uitest", FASTMD_DATA=str(HERE / "out" / "profile"))
    p = subprocess.Popen([str(exe), f"--scroll-test={frames}", f"--size={size}", "--light", str(doc)], env=env)
    p.wait(timeout=180)
    if not out.exists():
        return None
    # the viewer writes one line per run: "<w>x<h> px blocks=N render+blit p50=… p95=… max=… ms"
    last = [l for l in out.read_text(encoding="utf-8").splitlines() if "render+blit" in l]
    return last[-1] if last else None


def memory_of(exe, doc):
    env = dict(os.environ, FASTMD_REGKEY=r"Software\FastMD-uitest", FASTMD_DATA=str(HERE / "out" / "profile"))
    p = subprocess.Popen([str(exe), "--light", "--size=1400x900", str(doc)], env=env)
    time.sleep(6.0)  # let the background measuring of a big document finish
    k32 = ctypes.WinDLL("kernel32")
    psapi = ctypes.WinDLL("psapi")

    class PMC(ctypes.Structure):
        _fields_ = [("cb", ctypes.c_uint32), ("PageFaultCount", ctypes.c_uint32),
                    ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                    ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                    ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t)]

    h = k32.OpenProcess(0x1000 | 0x0010, False, p.pid)
    pmc = PMC()
    pmc.cb = ctypes.sizeof(pmc)
    ok = psapi.GetProcessMemoryInfo(ctypes.c_void_p(h), ctypes.byref(pmc), ctypes.sizeof(pmc))
    k32.CloseHandle(ctypes.c_void_p(h))
    p.terminate()
    p.wait(timeout=30)
    return (pmc.WorkingSetSize / 1e6, pmc.PeakWorkingSetSize / 1e6, pmc.PagefileUsage / 1e6) if ok else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=str(HERE.parent / "build" / "Release" / "FastMD.exe"))
    ap.add_argument("--frames", type=int, default=200)
    ap.add_argument("--size", default="3440x1400")
    args = ap.parse_args()
    exe = pathlib.Path(args.exe)
    medium, large = REPO / "bench" / "corpus" / "medium.md", REPO / "bench" / "corpus" / "large.md"

    line = scroll_frames(exe, medium, args.frames, args.size)
    print(f"scroll {args.frames} frames: {line}" if line else "scroll: no frames measured")

    mem = memory_of(exe, large)
    if mem:
        print(f"memory on large.md (3.7 MB): working set {mem[0]:.0f} MB, peak {mem[1]:.0f} MB, commit {mem[2]:.0f} MB")
    else:
        print("memory: not measured")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
