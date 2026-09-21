// SVG through fastmd-svg.dll (lunasvg + plutovg). The library is loaded the first time a document needs it, which
// is always after the first frame, so the exe stays small and the launch path untouched.
#pragma once
#include "common.h"

bool IsSvgData(const uint8_t* data, size_t n);
bool SvgMeasure(const uint8_t* data, size_t n, float* w, float* h);
// renders into out (w * h premultiplied BGRA pixels)
bool SvgRender(const uint8_t* data, size_t n, int w, int h, std::vector<uint32_t>& out);
