// The map's round trips (docs/EDIT-MODE.md §14.1 sweeps, §14.2 fuzzing). MapSelfCheck says the map is well formed;
// this says it is usable: from every caret stop of every block, SrcOfText in each of its four modes lands in the
// block's lines, and TextOfSrc of the caret's source offset comes back to the same stop (both directions); and
// TextOfSrc of any source offset at all is a caret stop. fastmd-edit-tests runs it over the corpus, the fuzzer over
// every input it makes - most map bugs pass the self-check and fail here.
//
// stride / sStride: every n-th caret stop and source offset only (big documents). report(msg) gets each failure and
// returns whether to go on. The stops and offsets checked are added to *stops / *offsets.
#pragma once
#include <cstdarg>
#include <cstdio>
#include <string>

#include "editcore.h"

namespace mapsweep {
inline std::string Msg(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return buf;
}

template <class Report>
void SweepMap(const Doc& d, const std::wstring& src, uint32_t stride, uint32_t sStride, size_t* stops, size_t* offsets,
              Report report) {
    const uint32_t n = (uint32_t)src.size();
    uint32_t tick = 0;
    for (uint32_t k = 0; k < d.blocks.size(); k++) {
        const Block& b = d.blocks[k];
        const BlockSrc& bs = d.blockSrc[k];
        if (bs.flags & BS_SYNTH) continue;
        std::vector<std::pair<int32_t, std::pair<uint32_t, uint32_t>>> ranges;  // cell, [beg, end]
        if (bs.flags & (BS_OBJECT | BS_RAW)) {
            ranges.push_back({-1, {b.textOff, b.textOff}});
        } else if (b.kind == BK_TABLE && b.aux < d.tables.size()) {
            const Table& tb = d.tables[b.aux];
            for (uint32_t c = 0; c < tb.rows * tb.cols; c++) {
                const Cell& cell = d.cells[tb.cellOff + c];
                ranges.push_back({(int32_t)c, {cell.textOff, cell.textOff + cell.textLen}});
            }
        } else {
            ranges.push_back({-1, {b.textOff, b.textOff + b.textLen}});
        }
        for (auto& r : ranges) {
            for (uint32_t t = r.second.first; t <= r.second.second; t++) {
                if (stride > 1 && (tick++ % stride) != 0) continue;
                TextPos pos{t, (int32_t)k, r.first};
                if (!CaretStop(d, pos)) continue;
                ++*stops;
                for (MapMode m : {MAP_CARET, MAP_OUTER_START, MAP_OUTER_END, MAP_INNER_START}) {
                    uint32_t s = SrcOfText(d, src, pos, m);
                    if ((s == UINT32_MAX || s > n || s < bs.line || s > bs.outerEnd) &&
                        !report(Msg("b%u t%u c%d mode %d: SrcOfText = %d outside the block's lines [%u,%u]", k, t, r.first,
                                    (int)m, (int)s, bs.line, bs.outerEnd)))
                        return;
                }
                uint32_t s = SrcOfText(d, src, pos, MAP_CARET);
                for (int dir : {-1, 1}) {
                    TextPos back = TextOfSrc(d, src, s, dir, nullptr);
                    int32_t wantBlock = (bs.flags & BS_RAW) && bs.rawId >= 0 ? bs.rawId : (int32_t)k;
                    bool same = back.t == (bs.flags & BS_RAW ? d.blocks[wantBlock].textOff : t) && back.block == wantBlock;
                    if (same && back.cell != r.first) {
                        // a cell a short row lacks shares its place with the end of the cell before it
                        const Table& tb = d.tables[b.aux];
                        same = r.first >= 0 && d.cellSrc[tb.cellOff + r.first].missing;
                    }
                    if (!same && !report(Msg("b%u t%u c%d -> s%u -> t%u b%d c%d (dir %d): the round trip does not come back",
                                             k, t, r.first, s, back.t, back.block, back.cell, dir)))
                        return;
                }
            }
        }
    }
    // any source offset maps to a caret stop (or into a folded block)
    for (uint32_t s = 0; s <= n; s += sStride) {
        for (int dir : {-1, 1}) {
            TextPos t = TextOfSrc(d, src, s, dir, nullptr);
            ++*offsets;
            if (t.block < 0) {
                if (!d.blockOrder.empty() && !report(Msg("TextOfSrc(%u, %d) found no block", s, dir))) return;
                continue;
            }
            const Block& b = d.blocks[t.block];
            bool hidden = b.details && !(b.details & 0x8000) && (uint32_t)(b.details & 0x7FFF) - 1 < d.detailsOpen.size() &&
                          !d.detailsOpen[(b.details & 0x7FFF) - 1];
            if (!hidden && !CaretStop(d, t) &&
                !report(Msg("TextOfSrc(%u, %d) = t%u b%d c%d is not a caret stop", s, dir, t.t, t.block, t.cell)))
                return;
        }
    }
}
}  // namespace mapsweep
