// Formulas and diagrams (plan 4.1, 4.2). Both are turned into SVG by small Rust libraries that live beside the exe:
// fastmd-tex.dll (RaTeX, KaTeX-compatible) and fastmd-mermaid.dll. They are loaded the first time a document actually
// contains a formula or a diagram - never on the start-up path - and the SVG is drawn by the renderer we already have.
#pragma once
#include <string>
#include <vector>

// Is the library there at all? Only looks at the file, without loading it: the parser asks this while the window is
// still being created, and decides whether a formula becomes a picture or stays as its own source text.
bool TexAvailable();
bool MermaidAvailable();

// Worker threads only. `fontPx` is the size the surrounding text is drawn at, `rgb` the colour it is drawn in.
// `ascent` comes back as the part of the formula's height that stands above the text baseline.
bool TexSvg(const std::string& tex, bool display, float fontPx, uint32_t rgb, std::vector<uint8_t>& svg, float* w,
            float* h, float* ascent);
bool MermaidSvg(const std::string& src, bool dark, std::vector<uint8_t>& svg);
