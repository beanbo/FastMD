"""Pixel regression of the Explorer thumbnail, straight through fastmd-preview.dll: no registry, no Explorer.

The preview DLL shares view.cpp, layout.cpp and parse.cpp with the reader, so a change made for the reader can move a
pixel of the pane. This loads the DLL with ctypes, asks it for its class factory (DllGetClassObject), creates the
thumbnail provider, hands it a file through IInitializeWithFile - exactly what the shell does - and compares the
picture GetThumbnail(1024) returns with a baseline taken from an earlier DLL.

Nothing is registered and nothing is written outside app/tests/out, so it is safe on a machine where FastMD is
installed (preview_check.py is not: it registers the handler of the build under test).

Usage:
  python -X utf8 app/tests/preview_direct.py --baseline     # take the baseline (with the DLL of the earlier build)
  python -X utf8 app/tests/preview_direct.py                # compare the current build with it (exit 1 on a change)
  python -X utf8 app/tests/preview_direct.py --dll <path>   # another fastmd-preview.dll"""
import ctypes
import ctypes.wintypes as wt
import pathlib
import shutil
import sys
import uuid

from PIL import Image, ImageChops

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]
OUT = HERE / "out"
WORK = OUT / "preview-direct"
BASE = OUT / "preview-baseline"
SIZE = 1024

CLSID_THUMB = "{6F1A2C35-9B84-4B0E-9F6D-1C2E0A7B5D41}"
IID_ICLASSFACTORY = "{00000001-0000-0000-C000-000000000046}"
IID_ITHUMBNAILPROVIDER = "{E357FCCD-A995-4576-B01F-234630154E96}"
IID_IINITIALIZEWITHFILE = "{B7D14566-0509-4CCE-A71F-0A554233BD9B}"

# the same documents the UI tests use: features.md covers most of the engine, math.md the formulas and a diagram
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


class GUID(ctypes.Structure):
    _fields_ = [("Data1", ctypes.c_uint32), ("Data2", ctypes.c_uint16), ("Data3", ctypes.c_uint16),
                ("Data4", ctypes.c_ubyte * 8)]


def guid(text):
    return GUID.from_buffer_copy(uuid.UUID(text).bytes_le)


def vcall(obj, index, restype, argtypes, *args):
    """calls method `index` of a COM interface pointer through its vtable"""
    vtbl = ctypes.cast(obj, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    fn = ctypes.WINFUNCTYPE(restype, ctypes.c_void_p, *argtypes)(vtbl[index])
    return fn(obj, *args)


def release(obj):
    if obj:
        vcall(obj, 2, ctypes.c_ulong, [])


class BIH(ctypes.Structure):
    _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG), ("biPlanes", wt.WORD),
                ("biBitCount", wt.WORD), ("biCompression", wt.DWORD), ("biSizeImage", wt.DWORD),
                ("biXPelsPerMeter", wt.LONG), ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD),
                ("biClrImportant", wt.DWORD)]


class BITMAP(ctypes.Structure):
    _fields_ = [("bmType", wt.LONG), ("bmWidth", wt.LONG), ("bmHeight", wt.LONG), ("bmWidthBytes", wt.LONG),
                ("bmPlanes", wt.WORD), ("bmBitsPixel", wt.WORD), ("bmBits", ctypes.c_void_p)]


gdi = ctypes.WinDLL("gdi32")
gdi.GetObjectW.argtypes = [wt.HANDLE, ctypes.c_int, ctypes.c_void_p]
gdi.CreateCompatibleDC.restype = wt.HDC
gdi.CreateCompatibleDC.argtypes = [wt.HDC]
gdi.GetDIBits.argtypes = [wt.HDC, wt.HBITMAP, wt.UINT, wt.UINT, ctypes.c_void_p, ctypes.c_void_p, wt.UINT]
gdi.DeleteObject.argtypes = [wt.HGDIOBJ]
gdi.DeleteDC.argtypes = [wt.HDC]
ole = ctypes.WinDLL("ole32")


def thumbnail(dll, path):
    """one GetThumbnail(SIZE) through the DLL's own class factory, as a PIL image"""
    factory = ctypes.c_void_p()
    hr = dll.DllGetClassObject(ctypes.byref(guid(CLSID_THUMB)), ctypes.byref(guid(IID_ICLASSFACTORY)),
                               ctypes.byref(factory))
    if hr != 0:
        raise RuntimeError(f"DllGetClassObject: 0x{hr & 0xFFFFFFFF:08x}")
    provider, init = ctypes.c_void_p(), ctypes.c_void_p()
    try:
        # IClassFactory::CreateInstance(outer, riid, ppv) is slot 3
        hr = vcall(factory, 3, ctypes.c_long, [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p],
                   None, ctypes.byref(guid(IID_ITHUMBNAILPROVIDER)), ctypes.byref(provider))
        if hr != 0:
            raise RuntimeError(f"CreateInstance: 0x{hr & 0xFFFFFFFF:08x}")
        hr = vcall(provider, 0, ctypes.c_long, [ctypes.c_void_p, ctypes.c_void_p],
                   ctypes.byref(guid(IID_IINITIALIZEWITHFILE)), ctypes.byref(init))
        if hr != 0:
            raise RuntimeError(f"QueryInterface(IInitializeWithFile): 0x{hr & 0xFFFFFFFF:08x}")
        # IInitializeWithFile::Initialize(path, STGM_READ) is slot 3
        hr = vcall(init, 3, ctypes.c_long, [ctypes.c_wchar_p, wt.DWORD], str(path), 0)
        if hr != 0:
            raise RuntimeError(f"Initialize: 0x{hr & 0xFFFFFFFF:08x}")
        bmp, alpha = wt.HBITMAP(), ctypes.c_int()
        # IThumbnailProvider::GetThumbnail(cx, HBITMAP*, WTS_ALPHATYPE*) is slot 3
        hr = vcall(provider, 3, ctypes.c_long, [wt.UINT, ctypes.c_void_p, ctypes.c_void_p],
                   SIZE, ctypes.byref(bmp), ctypes.byref(alpha))
        if hr != 0 or not bmp.value:
            raise RuntimeError(f"GetThumbnail: 0x{hr & 0xFFFFFFFF:08x}")
    finally:
        release(init.value)
        release(provider.value)
        release(factory.value)
    info = BITMAP()
    gdi.GetObjectW(bmp, ctypes.sizeof(info), ctypes.byref(info))
    w, h = info.bmWidth, abs(info.bmHeight)
    bi = BIH(ctypes.sizeof(BIH), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
    buf = ctypes.create_string_buffer(w * h * 4)
    dc = gdi.CreateCompatibleDC(None)
    got = gdi.GetDIBits(dc, bmp, 0, h, buf, ctypes.byref(bi), 0)
    gdi.DeleteDC(dc)
    gdi.DeleteObject(bmp)
    if got != h:
        raise RuntimeError("GetDIBits failed")
    return Image.frombuffer("RGB", (w, h), buf, "raw", "BGRX", 0, 1).copy()


def documents():
    """the inputs, copied next to each other so relative pictures resolve the same way every time"""
    WORK.mkdir(parents=True, exist_ok=True)
    (WORK / "img").mkdir(exist_ok=True)
    shutil.copy(REPO / "bench" / "corpus" / "img" / "diagram0.png", WORK / "img" / "diagram0.png")
    docs = []
    for name in ("features.md", "footnotes.md", "html.md"):
        shutil.copy(HERE / name, WORK / name)
        docs.append(WORK / name)
    (WORK / "math.md").write_text(MATH_DOC, encoding="utf-8")
    docs.append(WORK / "math.md")
    return docs


def main():
    args = sys.argv[1:]
    make_baseline = "--baseline" in args
    dll_path = HERE.parent / "build" / "Release" / "fastmd-preview.dll"
    if "--dll" in args:
        dll_path = pathlib.Path(args[args.index("--dll") + 1])
    ole.CoInitializeEx(None, 2)  # COINIT_APARTMENTTHREADED, as a shell host would be (WIC needs COM)
    dll = ctypes.WinDLL(str(dll_path))
    dll.DllGetClassObject.restype = ctypes.c_long
    dll.DllGetClassObject.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    BASE.mkdir(parents=True, exist_ok=True)
    ok = True
    for doc in documents():
        img = thumbnail(dll, doc)
        name = doc.stem
        if make_baseline:
            img.save(BASE / f"{name}.png")
            print(f"[BASE] {name}: {img.size[0]}x{img.size[1]} from {dll_path}")
            continue
        ref_path = BASE / f"{name}.png"
        if not ref_path.exists():
            print(f"[FAIL] {name}: no baseline at {ref_path} (run with --baseline first)")
            ok = False
            continue
        ref = Image.open(ref_path).convert("RGB")
        same = ref.size == img.size and ImageChops.difference(ref, img).getbbox() is None
        if not same:
            img.save(WORK / f"{name}-now.png")
            box = ImageChops.difference(ref, img).getbbox() if ref.size == img.size else "size"
            print(f"[FAIL] {name}: the thumbnail changed (difference {box}); see {WORK / (name + '-now.png')}")
            ok = False
        else:
            print(f"[OK] {name}: the thumbnail equals the baseline ({img.size[0]}x{img.size[1]})")
    if make_baseline:
        return 0
    print("RESULT", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
