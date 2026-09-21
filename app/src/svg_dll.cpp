// fastmd-svg.dll: SVG → pixels, with lunasvg and plutovg inside. It lives outside the exe on purpose: the process
// pays for every megabyte of its image at each launch (research/05), and SVG is only ever needed after the first
// frame. The app loads this library the first time a document holds an SVG.
#include "../third_party/lunasvg/include/lunasvg.h"

#include <string>
#include <windows.h>

extern "C" {

// Natural size of the drawing (its width/height or viewBox). Returns 0 when the data is not an SVG we can read.
__declspec(dllexport) int FastMdSvgMeasure(const void* data, int len, float* w, float* h) {
    if (!data || len <= 0) return 0;
    auto doc = lunasvg::Document::loadFromData((const char*)data, (size_t)len);
    if (!doc) return 0;
    if (w) *w = doc->width();
    if (h) *h = doc->height();
    return 1;
}

// Renders into out (w * h pixels, premultiplied BGRA, stride w * 4). Returns 0 on failure.
__declspec(dllexport) int FastMdSvgRender(const void* data, int len, int w, int h, void* out) {
    if (!data || len <= 0 || w <= 0 || h <= 0 || !out) return 0;
    auto doc = lunasvg::Document::loadFromData((const char*)data, (size_t)len);
    if (!doc) return 0;
    lunasvg::Bitmap bitmap((uint8_t*)out, w, h, w * 4);
    bitmap.clear(0x00000000);
    // render() draws at the document's own size, so the scale to the asked-for box goes in the matrix
    float dw = doc->width(), dh = doc->height();
    float sx = dw > 0.f ? (float)w / dw : 1.f, sy = dh > 0.f ? (float)h / dh : 1.f;
    doc->render(bitmap, lunasvg::Matrix(sx, 0.f, 0.f, sy, 0.f, 0.f));
    return 1;
}

// A font file for the <text> inside badges; family may be "" to serve as the fallback for every family.
__declspec(dllexport) int FastMdSvgAddFont(const char* family, int bold, int italic, const wchar_t* file) {
    if (!file) return 0;
    char path[MAX_PATH * 2] = {};
    if (!WideCharToMultiByte(CP_UTF8, 0, file, -1, path, sizeof(path) - 1, nullptr, nullptr)) return 0;
    return lunasvg_add_font_face_from_file(family ? family : "", bold != 0, italic != 0, path) ? 1 : 0;
}

}  // extern "C"

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
