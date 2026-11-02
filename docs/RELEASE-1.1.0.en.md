[Русский](RELEASE-1.1.0.md) | **English**

# FastMD 1.1.0 — release notes

*Release notes as published on 21.09.2026: <https://github.com/beanbo/FastMD/releases/tag/v1.1.0>. The release
commands are at the end of `docs/RELEASE-1.0.0.md` (substitute the version); the next release is [1.1.1](RELEASE-1.1.1.md).*

---

## Mermaid diagrams are drawn a new way

`graph` / `flowchart` diagrams — the most common kind — are now placed by our own layout engine rather than by a third-party
library. The library placed the nodes once and then pushed them apart until nothing overlapped; on a
real architecture diagram — with nested subgraphs and arrows leading into a subgraph — this produced frames
lying across one another, nodes outside their own subgraph and a canvas several times larger than needed.

Now it is the classic layered layout, as in dot and dagre, and as Mermaid itself does it: break the cycles, split
into ranks, order the ranks so that arrows cross less often, and place everything horizontally. Each subgraph
is laid out on its own and reaches its parent as a single box — so a subgraph's content stays inside its frame.

A diagram with 32 nodes and 5 subgraphs:

| | before | after |
|---|---|---|
| size | 4246 × 5308 (over the rasterization limit — the diagram was squashed as well) | 2404 × 1718 |
| time | 623 ms | 2.8 ms |
| simple diagram | 17 ms | 0.06 ms |

Also: a label's width is measured with the very font file that `fastmd-svg.dll` later draws it with, so the text does not
spill over the frame; nodes in a rank follow the source order, not the alphabet; and arrows no longer go up first
and then down — two arrows between the same pair are spread apart after being clipped to the frames, along the visible segment,
not along the segment between the centers (a subgraph's center lies deep inside its frame). The other 22 kinds of diagrams
(sequence, class, state, Gantt, ER and the rest) are still drawn by the library.

## Full-width diagrams with sideways scrolling

A diagram takes the full width of the window between the margins, not the text column plus a little extra. If it still does not fit,
it no longer shrinks into illegibility: it is drawn at its natural size, and the block scrolls sideways — Shift+wheel,
the touchpad's horizontal scroll or dragging the bar, exactly like a wide table or code block. On paper and in the
Explorer pane there is nothing to scroll with, so there the diagram still shrinks to fit the width.

## Explorer preview pane

A fix found a few minutes after 1.0.0. The program's own window never had these bugs — only the pane did.

- **The pane showed no images, formulas or diagrams.** The libraries `fastmd-svg.dll`, `fastmd-tex.dll` and
  `fastmd-mermaid.dll` were looked for next to the process, but the pane's host is Explorer's `prevhost.exe`, not FastMD. Now
  they are looked for next to the library that calls them, and are found everywhere: in the pane and in the window.
- **The pane prepared nothing in the background.** It has no background threads, so images and formulas stayed as empty
  frames. Now it draws them right away when the document loads — the pane has no first frame to protect.

Measurements after the fix (a 700×900 window): **6 ms** for a document without formulas, **≈40 ms** for the first document with a formula in the
process (the typesetting library is loaded), then single-digit milliseconds again. PowerToys Markdown Preview takes 0.6–1.4 s.

Everything else is as in [1.0.0](https://github.com/beanbo/FastMD/releases/tag/v1.0.0).

## Install

| How | What to do |
|---|---|
| Installer | download `FastMD-Setup.exe` and run it. No admin rights needed |
| Without installing | unpack `FastMD-1.1.0-win-x64.zip` anywhere and run `FastMD.exe` |

The `.sha256` files next to them let you check the download. The exe is not signed: SmartScreen will warn you on the first run
("More info" → "Run anyway").
