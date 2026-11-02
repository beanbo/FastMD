[Русский](04-markdown-pipeline.md) | **English**

# 04. The Markdown pipeline: parser, code highlighting, math, diagrams, images, huge documents

- Date: 2026-09-19. Machine: Windows 11 Pro 26200, Ryzen 9 7950X (16C/32T), 64 GB, NVMe, Defender real-time ON.
- Lab: `C:/Main/Projects/FastMD/research/lab/md-pipeline/`. It was started by a previous agent, who built the parsers and
  wrote the benchmarks but was stopped by a hook before writing the report. I reread its code, **remeasured everything from scratch**
  (the numbers below are mine, from this session) and added what was missing:
  a hands-on check of CommonMark/GFM spec compliance for 11 parsers,
  Lexilla as native highlighting, RaTeX as native math, markdown-rs scaling, DirectWrite memory,
  MathJax 4 without a11y enrichment, and a corrected Mermaid comparison. How to reproduce: Appendix A.
- Labels: **[measured]** — my measurement in this session; **[source]** — from documentation and articles (links in §11);
  **[report NN]** — a number from the neighboring report `research/NN-*.md`.
- **About noise.** Other agents were loading the machine in parallel. For the parsers the best median round of 10 is taken (2 series of 5);
  the series differ by 3–15%. For everything else, 2–5 repetitions, with the range given. Absolute numbers are a guide.
  Orders of magnitude and comparisons within one series are reliable.
- Corpus: `bench/corpus/medium.md` (53 KB, 1,416 lines, 24 code blocks in 7 languages, 24 tables),
  `large.md` (3.79 MB, 96,768 lines, 1,659 code blocks, 1,659 tables, 34 images).
  **The corpus has no math and no Mermaid**, so synthetic sets were used for those: 5 formulas × 100 and 2 diagrams
  (a 9-node flowchart and a 3-participant sequence diagram), identical in all tests.

---

## 0. In short (TL;DR)

1. **Parsing Markdown is never the bottleneck if the parser is native and streaming.**
   md4c (master 0.6.0-dev; the 0.5.3 release is the same) parses large.md (3.8 MB) in **10.6 ms** (342 MB/s), and renders it to HTML in **16.2 ms**.
   pulldown-cmark 0.13.4 takes 16.8 / 19.7 ms. medium.md (53 KB) takes **0.14–0.20 ms** with both [measured].
   Next by speed come cmark 98 ms, comrak 151 ms, cmark-gfm 283 ms, marked 286 ms, Markdig 304 ms, markdown-it 421 ms.
   micromark takes 4.8 s. **markdown-rs grows superlinearly: 16–26 s on large.md** [measured]. These two are not fit for FastMD.
2. **For managed runtimes what matters is the cold first call, not throughput** [measured, medium].
   Markdig: JIT — **~55 ms** (pipeline build 11.5 ms + first `ToHtml` 43 ms); ReadyToRun — ~19 ms; NativeAOT — **2 ms**.
   Node: markdown-it — `import` 26 ms + first render 28 ms; marked — 9 + 28 ms; micromark — 75 + 93 ms.
3. **Spec compliance** [measured, 652 CommonMark 0.31.2 examples + 24 GFM extension examples, cmark normalizer].
   If purely cosmetic HTML differences are forgiven (`&quot;`, `style` instead of `align`, `<s>` instead of `<del>`), then:
   - **comrak — 652/652 and 24/24**, the only flawless option;
   - markdown-rs and micromark — 652/652 and 23/24;
   - **md4c — 652/652 and 17/24** (the 0.5.3 release — 18/24; master has a regression with an autolink ending in a period):
     autolink edge cases (4 in master, 3 in 0.5.3), 2 table edge cases, no tagfilter;
   - **pulldown-cmark — 652/652 and 12/24**: GFM autolinks (`www.…`, bare URLs) are **missing entirely**, 0/11;
   - cmark-gfm — 641/652, it lags one spec version behind (0.29);
   - Markdig — 651/652 and 15/24, diverges from GFM on tables; marked — 640/652 (errors in entities) and 22/24.
4. **Code highlighting: native lexers are 1–2 orders of magnitude faster than grammar engines** [measured, 7 languages].
   **Lexilla 5.5.3** (the Scintilla/Notepad++ lexers): creating 7 lexers — 0.1–0.3 ms, first block — < 0.2 ms, ~30 MB/s.
   Next: tree-sitter ~3 MB/s, but compiling the queries costs 4–19 ms per language. Prism (V8) 5–7 MB/s, highlight.js 2–3 MB/s.
   syntect **0.9 MB/s** and 18–19 ms for lazy loading of syntaxes. Shiki 0.16–0.36 MB/s plus 100–150 ms of initialization.
5. **Math.** KaTeX — **~0.1 ms per formula** (in V8). MathJax 3/4 — ~0.8 ms.
   In the browser **MathJax 4 with its default a11y enrichment spends 1.08 s on 50 formulas, and 70 ms without it** [measured].
   Natively, without JS: **RaTeX 0.1.14** (Rust, KaTeX-compatible) — **0.6–1.0 ms** for the first formula cold
   and **~70 µs** per formula after that. The output is almost indistinguishable from KaTeX (`docs/ratex-vs-katex.png`) [measured].
6. **Mermaid.** `mermaid.min.js` weighs 5.58 MB and executes in **172 ms**; the first diagram appears 394 ms after page start.
   **The V8 code cache in a persistent profile cuts execution to 36–43 ms, but only from the third launch on** [measured].
   The native **mermaid-rs-renderer (mmdr) 0.3.1**: flowchart — 82–90 ms cold and ~40 ms warm, sequence — 0.3 ms [measured].
7. **A large document is bounded by layout and memory, not by the parser** [measured, large.md].
   DirectWrite lays out the first 40 blocks in **1.7 ms**, and all 101,235 blocks in **~1.0 s** on one thread (0.4 s on 16 threads).
   **Keeping every `IDWriteTextLayout` in memory costs +1.52 GB of private bytes.**
   In Chromium the whole document gives a frame after **1.85 s**; with `content-visibility:auto` — 0.73 s;
   with the "first 64 KB, the rest in chunks" technique — **0.26 s**.
8. **Images: WIC returns dimensions in 0.07–0.22 ms** (the first call, which loads the codec, takes 6–13 ms).
   Fully decoding a 12-megapixel JPEG takes 70–80 ms, a 4000×3000 PNG ~120 ms [measured].
   Hence the rule: dimensions synchronously, pixels asynchronously and only for what is visible.
9. **No mainstream Windows reader opens a file faster than ~0.6–1 s** [report 06, measured]: Typora 1.7 s,
   MarkText 3.8 s, Markdown Monster 4.5 s, PowerToys 0.6/1.4 s, Inlyne 1.2 s, Ferrite 1.3 s.
   Almost all of them are Electron/WebView2 plus a JS parser (markdown-it, marked, Markdig), highlight.js or Prism, MathJax or KaTeX, and mermaid.js.
10. **Recommendation (§9):**
    - parser: **md4c** for C++, or pulldown-cmark with a separate autolink pass for Rust;
    - highlighting: **Lexilla** (or tree-sitter, lazily and off the UI thread);
    - math: v1 shows the source, v1.x renders via **RaTeX** into a native display list;
    - Mermaid: v1 shows the source, v1.x uses mmdr, with a hidden WebView2 as fallback;
    - large files: full parse, layout of the visible part only, height estimates, background layout of the rest, an LRU cache of layouts.

---

## 1. Methodology and artifacts

| What | Files in `research/lab/md-pipeline/` | How it was measured |
|---|---|---|
| Parser speed | `c/bench_md4c.c`, `c/bench_cmark.c`, `c/bench_cmark_gfm.c`, `rust/src/bin/parsers.rs`, `dotnet/Program.cs`, `node/bench-md.mjs`, `run_parsers.py`, **`results_parsers.md`** | One process per (parser, document) pair. The file is read into memory beforehand. N iterations (medium 300, large 12), median within the process, the report gives the best round of 10. `first` is the first iteration (cold caches, and for .NET/V8 a JIT not yet warmed up). Runtime startup is **not included** here; it is in §2.2 |
| Scaling | `docs/medium_x4.md`, `docs/medium_x16.md` | medium concatenated 4 and 16 times |
| Spec compliance | **`compliance/spec_compliance.py`**, `rust/src/bin/specbatch.rs`, `compliance/dotnet-spec/`, `node/spec-batch.mjs`, **`compliance/results_compliance.{md,json}`**, `compliance/show_fails.py` | All 652 examples of `cmark/test/spec.txt` (CommonMark **0.31.2**) in CommonMark mode. 22 extension examples from `cmark-gfm/test/spec.txt` (GFM **0.29**: table, strikethrough, autolink, tagfilter) and 2 task list examples in GFM mode. Compared via `normalize_html` from cmark, as in its `spec_tests.py`. In addition, a "semantic" score forgives cosmetics; the list of rules is in §2.3 |
| Highlighting (Rust) | `rust/src/bin/hl.rs` | syntect 5.3 (Oniguruma, default syntaxes, InspiredGitHub theme) and tree-sitter-highlight 0.27 (5 grammars). All fenced blocks of medium and large |
| Highlighting (C++) | **`c/bench_lexilla.cpp`, `c/build_lexilla.cmd`** | Lexilla 5.5.3 + Scintilla 5.6.6 (headers only). md4c extracts the code blocks; the lexer runs on `TestDocument` from Lexilla itself. Two exes: all 125 lexers and a subset of 5 |
| Highlighting (JS) | `node/bench-hl.mjs`, `node/bench-browser.mjs` | Node 24.18 (V8) and headless Edge 153 (= the WebView2 Runtime 153 engine) |
| Math | `node/bench-math.mjs`, `rust/src/bin/hl.rs latex`, **`rust-ratex/`**, `node/shot-ratex.mjs` | 5 formulas × 100 = 500, display mode |
| Mermaid | `node/bench-browser.mjs`, `node/bench-codecache.mjs`, `rust-mmdr/`, `node/shot-mermaid.mjs` | Headless Edge 153 via puppeteer-core 25.11, a fresh profile for every run. For the code cache, one profile, 5 launches in a row |
| Images | `c/bench_native_io.cpp wic`, `gen_images.py` | WIC: header only, full decode to 32bppPBGRA, scaling to 796 px, `IWICBitmapSourceTransform` |
| Layout of a large document | `c/bench_native_io.cpp dwrite` | md4c collects the text of leaf blocks. One `IDWriteTextLayout` per block, 796 DIP wide, Segoe UI Variable Text 15 DIP, then `GetMetrics` |
| A huge document in Chromium | `node/bench-huge.mjs` | HTML of large.md from md4c (6.56 MB), 3 strategies, 3 runs each |
| C/C++ build | **`c/build_benches.cmd`** (verified: builds everything into `c/out-rebuild/`) | MSVC 14.44 via `bench/tools/msvc.cmd`, `/O2` |

Versions: md4c **0.6.0-dev** is master `7fc1815` of 2026-09-17. **There is no release-0.6.0 tag yet**; the latest release is 0.5.3
(`git ls-remote --tags`, the release is tracked in issue #422). cmark **0.31.2**, cmark-gfm **0.29.0.gfm.13**, pulldown-cmark **0.13.4** (feature `simd`),
comrak **0.55.0**, markdown-rs (`markdown`) **1.0.0**, Markdig **1.3.2** (.NET 8.0.424), markdown-it **15.0.2**, marked **18.0.13**,
micromark **4.0.2** + micromark-extension-gfm 3.0.0, commonmark.js **0.31.2**; syntect **5.3.0**, tree-sitter **0.27**,
Lexilla **5.5.3**, highlight.js **11.12.0**, Prism **1.30.0**, Shiki **4.4.3**; KaTeX **0.18.7**, Temml **0.13.5**,
MathJax **3.2.1** / **4.1.3**, pulldown-latex **0.8.0**, RaTeX **0.1.14**; Mermaid **12.0.0**, mermaid-rs-renderer **0.3.1**.
Rust 1.98.1: `opt-level=3`, `lto=fat`, `codegen-units=1`. .NET: `TieredPGO`, `InvariantGlobalization`.

---

## 2. Parsers

### 2.1 Speed [measured]

Parsing and rendering to HTML in process memory, the best median round of 10. The full table with both series is in `results_parsers.md`.

| Parser (language) | large.md, ms | MB/s | medium.md, ms | medium, first call, ms | Mode |
|---|---:|---:|---:|---:|---|
| **md4c** (master 0.6.0-dev), parse only (C) | **10.6** | 342 | **0.14** | 0.22 | GitHub dialect + `$…$`; empty callbacks |
| **md4c + md4c-html** (C) | **16.2** | 224 | 0.20 | 0.28 | same, output to a buffer |
| **pulldown-cmark 0.13.4**, events (Rust) | **16.8** | 215 | 0.16 | 0.33 | tables, tasklists, strikethrough, footnotes, GFM, math |
| pulldown-cmark, events + offsets | 16.2 | 223 | 0.23 | 0.19 | `into_offset_iter()` — **offsets are free** |
| pulldown-cmark → HTML | 19.7 | 184 | 0.17 | 0.23 | |
| cmark 0.31.2 → HTML (C) | 98.5 | 37 | 0.76 | 0.83 | CommonMark only |
| comrak 0.55, AST (Rust) | 126.9 | 29 | 0.96 | 1.32 | + autolink, tagfilter, footnotes, alerts |
| comrak 0.55 → HTML | 151.1 | 24 | 0.86 | 0.88 | |
| commonmark.js 0.31.2 (Node) | 198.9 | 18 | 1.16 | 10.8 | CommonMark only |
| cmark-gfm 0.29.gfm.13, AST (C) | 242.0 | 15 | 1.25 | 1.41 | 5 extensions + footnotes |
| cmark-gfm → HTML | 283.2 | 13 | 1.47 | 1.60 | |
| marked 18 (Node) | 286.3 | 13 | 3.29 | 27.9 | gfm |
| Markdig 1.3.2 NativeAOT (.NET) | 304.2 | 12 | 1.04 | 1.81 | PipeTables, EmphasisExtras, TaskLists, AutoLinks, Footnotes, AlertBlocks |
| Markdig 1.3.2 JIT | 355.8 | 10 | 1.07 | **41.5** | same |
| markdown-it 15 (Node) | 421.3 | 9 | 4.21 | 24.1 | html + linkify |
| micromark 4 + gfm (Node) | **4,839** | 0.7 | 45.5 | 76.5 | 3 iterations on large |
| markdown-rs 1.0 → HTML (Rust) | **17,851 – 25,798** | 0.1–0.2 | 18.3 | 19.0 | 3 iterations on large |

**markdown-rs scales superlinearly** [measured]: 53 KB → 18 ms; ×4 → 113 ms (6.2 times more);
×16 → 943 ms (another 8.3 times); large → 16–26 s. md4c and pulldown-cmark grow linearly on the same files:
0.20 → 0.95 → 3.8 → 16 ms and 0.17 → 1.0 → 5.0 → 20 ms. A 1 MB document already takes markdown-rs a second.

**Why cmark-gfm is 3 times slower than cmark** [measured, extension mask in `bench_cmark_gfm.exe`].
The 0.29 core without extensions takes 142–173 ms versus 98 ms for cmark 0.31. The table extension adds ~+70–100 ms,
autolink ~+50–80 ms, the rest are almost free. Together that comes to 318 ms.

**Interim conclusion.** Native streaming parsers (md4c, pulldown-cmark) are an order of magnitude faster than everything
that has an AST or DOM. comrak builds an arena AST, which costs ×8, but in absolute terms it is still small:
0.9 ms on 53 KB and 150 ms on 3.8 MB.

### 2.2 Cold start of the parser runtime [measured, medium.md, 1 iteration, 3 runs]

| Variant | Until `Main` / import | Setup | First render of medium | Total on top of native |
|---|---:|---:|---:|---:|
| md4c / pulldown-cmark (native) | — | — | 0.22–0.33 ms | ≈ 0 |
| Markdig **NativeAOT** (3.1 MB exe) | ~38–45 ms¹ | pipeline 0.1 ms | **2.0 ms** | **~2 ms** |
| Markdig **ReadyToRun** self-contained (75 MB on disk) | ~47–49 ms¹ | 6.7 ms | 12.3 ms | ~19 ms |
| Markdig **JIT** (framework-dependent) | ~48–56 ms¹ | 11.5–12.8 ms | 43–45 ms | **~55 ms** |
| markdown-it (Node 24) | import 26 ms | — | 27.6 ms | ~54 ms + Node startup |
| marked (Node 24) | import 8.5 ms | — | 27.9 ms | ~36 ms + Node startup |
| micromark (Node 24) | import 75 ms | — | 93 ms | ~168 ms + Node startup |
| commonmark.js (Node 24) | import 21 ms | — | 12.2 ms | ~33 ms + Node startup |

¹ `DateTime.Now − Process.StartTime`: millisecond precision, and Defender gets counted in too. Good only for comparing
the variants with each other. The precise anatomy of .NET startup is in report 05.

In WebView2 a JS parser also pays for executing the bundle. markdown-it UMD in Edge 153: the script has executed by **19 ms**,
the medium.md DOM is ready by **42 ms** from navigation start [measured, `bench-browser`]. So **parsing on the host side**
(md4c → HTML, as in the `cpp-webview2` prototype) is cheaper than pulling the parser into the page.

### 2.3 CommonMark 0.31.2 and GFM compliance [measured]

`python compliance/spec_compliance.py` → `compliance/results_compliance.md` (+ a `.json` with the diffs of every failure).
**strict** — as in cmark's `spec_tests.py`: `normalize_html` keeps all attributes and collapses whitespace.
**semantic** — additionally forgives whatever does not change the visible result:
`&quot;` instead of `"`, `class` attributes, `style="text-align:x"` instead of `align="x"`, `disabled` instead of `disabled=""`,
an empty `<tbody>`, `<s>` instead of `<del>`, a space after the checkbox.

| Parser | CommonMark 0.31.2, strict / semantic out of 652 | GFM extensions, strict / semantic out of 24 | table (8) | strike (2) | autolink (11) | tagfilter (1) | tasklist (2) |
|---|---:|---:|---:|---:|---:|---:|---:|
| **md4c 0.6.0-dev** (master, `md2html --github`) | **652 / 652** | 15 / 17 | 6 / 6 | 2 / 2 | 7 / 7 | 0 | 0 / 2 |
| **md4c 0.5.3** (release, as in `cpp-webview2`) | **652 / 652** | 16 / 18 | 6 / 6 | 2 / 2 | 8 / 8 | 0 | 0 / 2 |
| cmark 0.31.2 | 652 / 652 | — (no extensions) | | | | | |
| cmark-gfm 0.29.0.gfm.13 | 641 / 641 | **24 / 24** | 8 | 2 | 11 | 1 | 2 |
| **pulldown-cmark 0.13.4** | 631 / **652** | 10 / 12 | 6 / 8 | 2 / 2 | **0 / 0** | 0 | 2 / 2 |
| **comrak 0.55** | **652 / 652** | **24 / 24** | 8 | 2 | 11 | 1 | 2 |
| markdown-rs 1.0 | 652 / 652 | 23 / 23 | 8 | 2 | 10 | 1 | 2 |
| Markdig 1.3.2 | 651 / 651 | 12 / 15 | 3 / 4 | 2 / 2 | 7 / 7 | 0 | 0 / 2 |
| markdown-it 15.0.2 (+task-lists) | 652 / 652 | 11 / 15 | 7 / 8 | 1 / 2 | 3 / 3 | 0 | 0 / 2 |
| marked 18.0.13 | 640 / 640 | 22 / 22 | 8 | 2 | 10 | 0 | 2 |
| micromark 4.0.2 | 652 / 652 | 23 / 23 | 8 | 2 | 10 | 1 | 2 |
| commonmark.js 0.31.2 | 652 / 652 | — | | | | | |

What actually lies behind the failures (diffs analyzed with `show_fails.py`):

- **pulldown-cmark.** All 21 CommonMark "failures" are just `"` in text not escaped to `&quot;`. The HTML is correct,
  semantically 652/652. In GFM: tables differ only cosmetically (`style` instead of `align`, an empty `<tbody>`).
  **Literal autolinks (`www.x.org`, a bare `https://…`, e-mail) are missing entirely: 0/11**; the 0.13.4 `Options` has no such flag
  (checked against the crate source). For READMEs this is noticeable, since links there are often bare. The fix is our own pass over `Event::Text`,
  e.g. with the `linkify` crate.
- **md4c.** CommonMark — 100%. Tables: `\|` inside a code span in a cell keeps its backslash (example 200);
  a table whose header and delimiter row have different column counts is accepted as a table (203).
  Autolinks: 4 edge cases — a period at the end of the URL, unbalanced `))`, `&hl;`, `<` in the URL.
  **The trailing period is a regression in master**: the 0.5.3 release passes example 623 (`www.commonmark.org/a.b.` → a link without the period),
  master includes the period in the link [measured]. This is worth reporting upstream before 0.6.0 is released.
  tagfilter is not implemented; for a native renderer this doesn't matter, since we don't execute raw HTML anyway.
  Task lists differ only in markup (`class`, no space after `<input>`).
  The speed of 0.5.3 and master is the same within noise: 3 interleaved runs, large 11.2–12.5 vs 12.4–12.6 ms of parsing,
  medium 0.18 vs 0.18 ms [measured].
- **cmark-gfm.** All 11 failures are spec drift from 0.29 → 0.31: the emphasis rules for currency symbols and
  nested `__` changed, plus the entity `&#87654321;`. In CommonMark, cmark-gfm lags behind upstream cmark.
- **Markdig.** CommonMark: `[ẞ]` does not find the definition `[SS]` (Unicode case fold, example 540).
  Tables genuinely diverge from GFM: a line without `|` right after a table breaks the whole table (202);
  excess cells are not dropped (204); a column mismatch is accepted (203); `\|` in a code span (200).
  No e-mail autolinks.
- **markdown-it.** CommonMark — 100%. With `linkify: true` only 3 of the 11 autolink examples passed:
  in its default configuration linkify-it did not recognize `www.` links and trims the edges of URLs and e-mails differently.
  `<s>` instead of `<del>` is cosmetic.
- **marked.** Real CommonMark errors: it does not decode some named entities (`&Dcaron;`, `&HilbertSpace;`),
  `&#X22;` with an uppercase X, `&#0;`; it does not percent-encode non-ASCII in `href`; tabs in blockquotes. For reading ordinary
  documents this is almost unnoticeable.
- **markdown-rs / micromark / marked** do not turn `ftp://…` into a link (example 628). Everything else is 100% for them.

**Conclusion.** For "like on GitHub" the reference is comrak (and cmark-gfm, if GitHub's actual engine is needed).
md4c and pulldown-cmark are perfect on the CommonMark core. On GFM they have known, narrow gaps:
pulldown-cmark has no autolinks, md4c has edge cases. Both can be closed with our own code, and the amount of that code is clear.

### 2.4 Extensions, source positions, AOT suitability

| Parser | Tables / tasklist / strike | Footnotes | Math | Front matter | GitHub Alerts | Source positions | Model | Size / AOT | License |
|---|---|---|---|---|---|---|---|---|---|
| **md4c 0.6.0-dev** (master) | yes / yes / yes | **yes** (master only) | `$…$`, `$$…$$` (`MD_FLAG_LATEXMATHSPANS`) | **no** (strip `---…---` before parsing, that's 10 lines) | **yes**, `MD_FLAG_ADMONITIONS` (master only) | **no API**, but text pointers point into the source buffer [measured, §2.5] | SAX callbacks, no AST | C99, no dependencies; `md2html.exe` (parser + HTML + entity table) = **155 KB** with `/MD` | MIT |
| **pulldown-cmark 0.13.4** | yes / yes / yes | yes (GFM and old-style) | yes (`ENABLE_MATH`) | yes (YAML `---` and `+++`) | yes (`ENABLE_GFM`) | **yes**: `into_offset_iter()` → a byte `Range`, cost ≈ 0 [measured] | pull iterator of events | Rust, `simd` on x64 | MIT |
| **comrak 0.55** | yes / yes / yes | yes (+ inline) | yes (`$`, `` $`…`$ ``) | yes | yes | yes: `sourcepos` (line:column), `sourcepos_chars` | arena AST | Rust | BSD-2 |
| markdown-rs 1.0 | yes (gfm) | yes | yes | yes | no | yes (mdast `position`) | events → mdast | Rust; **superlinear** | MIT |
| cmark-gfm 0.29.gfm.13 | yes | yes | no | no | no (GitHub adds its own post-processing) | yes (`--sourcepos`, line:column) | AST | C | BSD-2 |
| cmark 0.31.2 | no | no | no | no | no | yes | AST | C | BSD-2 |
| Markdig 1.3.2 | yes (pipe + grid) | yes | yes (`UseMathematics`) | yes (`UseYamlFrontMatter`) | yes (`UseAlertBlocks`) | yes: `Span`/`Line`/`Column`, `UsePreciseSourceLocation` | AST | .NET; **NativeAOT builds and works** (3.1 MB exe) [measured] | BSD-2 |
| markdown-it 15 | yes (tasklist is a plugin) | plugin | plugins (texmath, katex) | plugin | plugin | line range only (`token.map`) | tokens | JS | MIT |
| marked 18 | yes | extension | extension | extension | extension | no (only `raw`) | tokens | JS | MIT |
| micromark 4 | yes (ext) | yes | yes (ext) | yes (ext) | third-party | yes (events and positions) | events | JS, slow | MIT |

In addition, md4c supports wiki links and underline (already in 0.5.x), and master 0.6.0-dev adds `==highlight==`, `^sup^`/`~sub~`, `||spoiler||`
and preserving blank lines [source: md4c CHANGELOG]. **The bench prototypes vendor the 0.5.2 / 0.5.3 releases** (`cpp-d2d`, `cpp-webview2`, `cpp-richedit`, `qt`).
**Footnotes and Alerts exist only in master**: either wait for the 0.6.0 tag or pin a verified master commit.
I checked the 0.5.3 release separately (§2.3): CommonMark is also 652/652, the speed is the same.

### 2.5 Source positions in md4c without an API [measured]

`c/md4c_ptrcheck.c` checks where the pointers in all md4c text callbacks point:

- medium: 1,946 callbacks point into the source buffer, 204 outside it;
- large: 131,769 inside, 13,746 outside.

**All the "outside" pointers are of type `MD_TEXT_CODE`** — the static string `"\n"` at the line ends of code blocks.
So the source offset of any text fragment is simply `ptr − src`; md4c does not need patching.
That is enough for scroll sync with an editor, for "open in editor at this line" and for highlighting
search results against the source. A block's position can be taken from its first text descendant.

---

## 3. Syntax highlighting

### 3.1 Measurements [measured]

Over all fenced blocks: medium — 24 blocks, 4.46 KB, 7 languages (c, csharp, js, json, powershell, python, rust);
large — 1,659 blocks, 297 KB. "Cold" is the first pass in the process, including lazy loading of grammars and themes.

| Engine | Initialization | medium: cold | medium: warm | large: warm | Speed (warm) | Model |
|---|---:|---:|---:|---:|---:|---|
| **Lexilla 5.5.3** (C++, the Notepad++/SciTE lexers) | 7 lexers: **0.11–0.34 ms** | **0.42–0.62 ms** (first block **0.06–0.17 ms**) | **0.12–0.13 ms** | **9–13 ms** (one outlier of 30 ms) | **~22–36 MB/s** | hand-written lexers, a style for every byte; the measurement also includes counting style runs |
| tree-sitter-highlight 0.27 (5 grammars) | queries: **34–48 ms** (rust 15–19, js 11–17, python 5–9, c 3–5, json 0.1) | 1.5–2.1 ms | 0.93–1.1 ms | 61–66 ms | ~3 MB/s | incremental GLR + queries |
| syntect 5.3 (Oniguruma) | 0.5–0.7 ms (lazy) | **18.4–19.5 ms** | 4.9–6.6 ms | 308–316 ms | **~0.9 MB/s** | Sublime/TextMate grammars |
| Prism 1.30 (Node, core + 7) | 13–16 ms | 38 ms | 0.85–0.88 ms | 39–45 ms | 5–7 MB/s | regexes |
| highlight.js 11.12 core + 7 (Node) | 8.5–11 ms | 34–39 ms | 1.7–2.0 ms | 88–91 ms | 2.1–3.3 MB/s | regexes |
| highlight.js 11.12 full, 192 languages (Node) | 114–135 ms (2.5 s from a cold disk) | 33–46 ms | 1.7–1.9 ms | 85–86 ms | 2.3–3.4 MB/s | regexes |
| Shiki 4.4 + Oniguruma WASM | 95–143 ms | 158–181 ms | 12 ms | 0.94 s | 0.3 MB/s | TextMate (like VS Code) |
| Shiki 4.4, precompiled JS regex | 145–158 ms | 424–436 ms | 20–21 ms | 1.78–1.84 s | 0.16 MB/s | TextMate |
| Shiki 4.4, JS regex engine | 76–96 ms | 574–671 ms | 23–26 ms | 1.81–1.85 s | 0.16 MB/s | TextMate |

In the browser (Edge 153, medium, all blocks) [measured, `bench-browser`]: highlight.js common (36 languages, 129 KB)
is loaded by 53 ms, `highlightAll` is done by **73 ms**, i.e. +20 ms for 24 blocks. Prism core + 7 languages: 54.5 → **69.5 ms**, i.e. +15 ms.

**Size.** Lexilla, all 125 lexers: exe **1.44 MB**; a subset of 5 lexers: **0.43 MB**. Both exes also contain md4c and the static CRT,
so the net cost of 5 lexers is on the order of 0.1–0.2 MB, and of all 125 ~+1.0 MB.
tree-sitter: the static grammar `.lib` files weigh rust 1.2 MB, c 0.72, python 0.54, js 0.50, **c-sharp 5.5 MB**, json 14 KB, runtime 0.6 MB.
These are parse tables, and they land in the exe almost in full. syntect: `default_newlines.packdump` = 368 KB (~75 syntaxes) plus Oniguruma ~1.1 MB `.lib`.
JS: highlight.js common 129 KB, Prism core 58 KB plus languages, Shiki — megabytes of grammars.

### 3.2 Quality and integration cost

- **Lexilla** works at the lexical level (keywords, strings, comments, numbers, preprocessor), i.e. the same as Prism or highlight.js.
  Downside: **the keyword lists are supplied by the host** (in SciTE via `*.properties`, in Notepad++ via `langs.model.xml`).
  A single `cpp` lexer serves C, C++, C#, Java, JS and TS through different word sets. You need your own thin `IDocument`
  (`TestDocument` from Lexilla is a ready 300-line example) and a mapping from style numbers to theme colors. The license is permissive, HPND-style: "Permission to use, copy, modify, and distribute… without fee"
  (Lexilla/Scintilla License.txt).
- **tree-sitter** — the best quality (a syntax tree, language injections), a C runtime, convenient from C++ and Rust.
  Downsides: megabytes per grammar, **query compilation of 4–19 ms per language on first use**
  (it must be done lazily and off the UI thread), and ~3 MB/s.
- **syntect** — Sublime grammars, as used by bat and Zola; it looks "like on GitHub". But it is the slowest native option
  (0.9 MB/s, 18–19 ms on first use of 7 languages). The `rust-webview` prototype highlights with syntect
  on a worker thread, off the critical path.
- **The `cpp-d2d` and `cpp-webview2` prototypes use hand-written mini-lexers** (16 languages and ~250 lines of C, respectively).
  That is fine for a prototype, but in a product the quality and language coverage would be worse. Lexilla is a direct upgrade at the same cost.
- **Shiki** gives VS Code quality, but it means 100–150 ms of initialization and is 10–20× slower than Prism. It must not be on the first-frame path.

### 3.3 The "visible first" strategy

The first screen of medium has 1–3 code blocks of 0.2–1 KB each. Hence:

- **Lexilla**: 0.06–0.17 ms for the first block cold. Highlight **synchronously, before the first frame**: then the frame
  is final right away and the colors don't "jump".
- **tree-sitter / syntect / JS**: the first frame draws code in monospace without color (or with colors from a cache keyed by content hash).
  Highlighting of the visible blocks runs on a background thread right after the frame, then the rest in order of proximity to the viewport.
  Load a language's grammar on first encounter. Cache the result by (block hash, language, theme).
- For huge files (1,659 blocks, 297 KB of code in large) **never highlight everything up front**:
  even Lexilla spends 9–13 ms, syntect 0.3 s, Shiki 1–2 s. Highlight at the viewport boundary ± 1–2 screens.

---

## 4. Math

### 4.1 JS engines [measured]

**Node 24, 500 formulas** (5 different × 100, display):

| Engine | Initialization | First formula | 500 formulas, first pass | Warm, per formula | Output |
|---|---:|---:|---:|---:|---|
| **KaTeX 0.18.7** | 12.6–14.5 ms | 4.7–5.2 ms | 76–78 ms | **0.095–0.12 ms** | HTML+CSS, 3.7 KB per formula |
| MathJax 3.2.1 → SVG | 200–207 ms | 12.7–17 ms | 525–778 ms | 0.86–1.17 ms | SVG 6.5 KB |
| MathJax 3.2.1 → CHTML | 199–205 ms | 12.5–13 ms | 517–563 ms | 0.77–0.85 ms | HTML 1.7 KB |
| MathJax 4.1.3 → SVG (base + ams, NewCM font)¹ | 159–171 ms | 12 ms | 470–490 ms | 0.82 ms | SVG 7.0 KB |

¹ `MathJax.init()` from `@mathjax/src` fails on Windows with absolute paths: "Received protocol 'c:'" from Node's ESM loader.
So the measurement was done through the direct modules (`@mathjax/src/mjs/...`), the same way as for v3.

**Edge 153 (= WebView2), 50 display formulas in a page** [measured, 5 runs, ms from navigation start]:

| Variant | Script executed | Formulas ready | Fonts ready | Traffic |
|---|---:|---:|---:|---:|
| **Temml 0.13.5** (MathML Core, laid out by the browser engine) | 28.5 | **37.2** (+9) | 72.6 | 177 KB |
| **KaTeX 0.18.7** | 26.6 | **51.7** (+25) | 132 | 379 KB |
| MathJax 3.2 tex-chtml | 73.4 | 166.7 (+93) | 269 | 1.26 MB |
| MathJax 4.1 tex-chtml, default | 87.0 | **1,081** (+994) | 1,082 | 1.11 MB |
| **MathJax 4.1, `enableEnrichment/Speech/Braille/Explorer/AssistiveMml: false`** | 82.1 | **151.6** (+70) | 213 | 1.11 MB |

Report 06 put MathJax 4 under "don't use" (962 ms for 50 formulas). **Almost all of that cost is the speech a11y enrichment
(speech-rule-engine), and it can be turned off in the settings.** Without it MathJax 4 is on par with MathJax 3.
KaTeX and Temml are still 2–5 times lighter.

### 4.2 Natively, without JS [measured + source]

| Variant | What it is | Measurement | Assessment |
|---|---|---|---|
| **RaTeX 0.1.14** | Rust, "> 99.5% of KaTeX syntax", KaTeX fonts, C ABI (`ratex-ffi`), output as display list / SVG / PNG / PDF, MIT [source] | parse + layout + display list: **first formula 0.61–1.04 ms cold**, then **68–73 µs** per formula; + SVG 80–87 µs; 12 elements per formula. Test exe 4.3 MB (with SVG export), the KaTeX TTFs weigh 514 KB | **The best native candidate.** The display list is glyphs (KaTeX font + character code + x/y/scale), lines and paths. It is drawn via DirectWrite + an in-memory font collection from the KaTeX TTFs. Visually it almost matches KaTeX: **`docs/ratex-vs-katex.png`**. Risk: the project is at 0.1.x (2026). Zed has a PR for rendering math via RaTeX (#58593, status not checked) |
| pulldown-latex 0.8 | LaTeX → MathML | **2.0–2.6 µs** per formula | Fast, but **Windows has no system MathML renderer** (report 06). Useful only as input for WebView2 (MathML Core in Chromium), and there Temml already solves this |
| MicroTeX | C++, GDI+ backend on Windows, MIT, 661★, last push 2024-08 [source, report 06] | not measured | Stagnating, GDI+ is outdated. Don't use |
| RichEdit math | Office math (UnicodeMath/LaTeX build-up) exists in Office's RichEdit. Some options also work in Windows `msftedit.dll` [source: Murray Sargent's blog] | not measured | Of interest only for the `cpp-richedit` branch. Not verified that the system `msftedit.dll` draws math zones without Office |

### 4.3 Conclusion on math

Math appears in 0–0.6% of documents (report 06). It must not weigh down the first-frame path by even a millisecond:
md4c can find `$$` / `$…$` by itself (`MD_FLAG_LATEXMATHSPANS`).
**v1:** show the formula source in monospace with a "formula" badge.
**v1.x (native):** RaTeX via the C ABI, lazily and in the background, with a display-list cache keyed by formula text; 70 µs per formula means
1,000 formulas in 70 ms. **Fallback in WebView2:** KaTeX or Temml; MathJax only with enrichment turned off.

---

## 5. Diagrams (Mermaid)

### 5.1 mermaid.js in Edge 153 [measured, 5 runs, ms from navigation start]

| Variant | Script executed | First diagram (flowchart, 9 nodes) | Both diagrams (+ sequence) | Traffic / requests |
|---|---:|---:|---:|---|
| IIFE `mermaid.min.js` (5.58 MB) | **171.9** | **393.6** (+222) | 419.2 (+26) | 5.58 MB / 3 |
| ESM `mermaid.esm.min.mjs` (30 KB entry, lazy chunks) | 63.7 | 383.3 (+320) | 416.8 | 2.58 MB / 33 |

ESM loads faster, but the first diagram still pulls in chunks (dagre, flowchart-db, etc.), and the result is the same.
There is a gain only if the document has no diagrams or they can be deferred.

**V8 code cache** [measured, `bench-codecache.mjs`, one profile, 5 Edge launches in a row, script load and execution time]:

| Library | Launches 1, 2 | Launches 3–5 |
|---|---|---|
| mermaid.min.js | 173–175, 169–178 ms | **36–43 ms** (4–4.5 times less) |
| MathJax 3 tex-chtml | 57–59, 63–79 ms | 22–30 ms |
| KaTeX | 13.6–16.5, 13–15 ms | 4.6–5.6 ms |
| highlight.js | 18.2–18.4, 15–16 ms | 4.6–7.1 ms |

This matches Chrome's documented heuristic [source, v8.dev]:
- 1st run (cold) — compilation only;
- 2nd (warm) — the code cache is written;
- from the 3rd on (hot) — the cache is used.

The cache is tied to the URL and the HTTP cache. For WebView2 this means: **a persistent user data folder, stable URLs
and resources served with cacheable headers**. Whether this works with `SetVirtualHostNameToFolderMapping` and
`WebResourceRequested` I **did not check**.

### 5.2 Native: mermaid-rs-renderer (mmdr) — what is in `rust-mmdr/`

`rust-mmdr/` is the previous agent's benchmark: mermaid-rs-renderer **0.3.1 is used as a library**
(`mermaid_rs_renderer::render`), not as a CLI. It renders the same flowchart and sequence as the browser test.

- **`rust-mmdr/flowchart.svg`, `rust-mmdr/sequence.svg`** — mmdr's output: SVG with `<text>` and a fixed width (flowchart 1543 × 176).
- **`docs/mermaid-compare.png`** — their comparison with mermaid.js 12 in Edge (`node/shot-mermaid.mjs`).
  In the old version of the image the mmdr flowchart was cut off: the SVG with `width=1543` did not fit the 900 px column. That was a bug
  in the comparison page, not in mmdr. I fixed it (`svg{max-width:100%}`) and retook the shot.
  Now all 9 nodes can be seen to be in place. Differences:
  - mmdr does not wrap labels;
  - the theme is neutral gray;
  - edges are routed more simply (at the "yes/no" diamond).

  The sequence diagram practically matches in structure.

| Diagram | Cold (first call in the process) | With `fast_text_metrics` | Warm (median of 20) | SVG |
|---|---:|---:|---:|---:|
| flowchart, 9 nodes | **82–90 ms** | 41–48 ms | **39–41 ms** | 9.0 KB |
| sequence, 3 participants | 0.35–0.41 ms | 0.28–0.38 ms | **0.25 ms** | 7.3 KB |

The first call is more expensive because of font metrics initialization; the flowchart layout takes ~40 ms.
Report 06 measured the CLI `mmdr --timing` on a 12-node flowchart: 110 ms, of which 107 ms was layout.
The test exe weighs 5.7 MB. The library supports 23 diagram types, MIT [source].
**SVG with `<text>` needs an SVG renderer that handles text** — lunasvg or resvg; Direct2D does not draw `<text>` (report 06).

### 5.3 Conclusion on diagrams

Mermaid appears in 0.3–1.8% of documents (report 06), and **it never gets onto the first-frame path**.

- **v1:** the source in a code block with a "diagram" badge.
- **v1.x:** mmdr in the background → SVG → lunasvg/resvg → a bitmap in the document. That is 40–90 ms per flowchart off the UI thread, no Chromium needed.
- **Fallback for fidelity:** a hidden WebView2 with a persistent profile, `mermaid.render()` → an SVG string (option A from report 06).
  The first two launches pay ~175 ms for the script, later ones ~40 ms.

---

## 6. Images: WIC [measured]

`bench_native_io.exe wic`, 3 repetitions each; CoInitialize 0.9 ms, the `CLSID_WICImagingFactory2` factory 3.7 ms.

| File | Dimensions only (`GetSize`) | Full decode to 32bppPBGRA | Scaling to 796 px (`IWICBitmapScaler`, Fant) | `IWICBitmapSourceTransform` |
|---|---:|---:|---:|---:|
| JPEG 4000×3000, 6.3 MB (synthetic, noisy) | 1st **12.9 ms** (codec load), then **0.19–0.21** | **70–80 ms** | 59–70 ms | 52–59 ms (1000×750, DCT scale 1/4) |
| PNG 4000×3000, 31.6 MB | 1st 6.0, then 0.16–0.22 | **119–127 ms** | 111–124 ms | no |
| PNG 1600×1200, 5.0 MB | 1st 6.7, then 0.18–0.19 | 17.5–20.6 ms | 19.6–21 ms | no |
| PNG from the corpus, 640×240 | 0.07–0.17 | **0.15–0.29 ms** | 0.25–0.32 ms | no |

Observations:

- **Dimensions cost tenths of a millisecond.** They can be fetched synchronously while building the document model, reserving space:
  the layout does not jump when the pixels arrive.
- The WIC scaler decodes the whole image anyway, so the savings are small.
  JPEG DCT scaling via `IWICBitmapSourceTransform` barely helped on **this** file: the image is noise at quality 90,
  and the time goes to entropy decoding. On ordinary photos a bigger gain is expected, but this is **not verified**.
- Decoding takes 20–130 ms per image → only in a thread pool and only for the visible area ± 1 screen.
  Cache by (path, mtime, width). The first codec call (6–13 ms) can be warmed up in the background right after the first frame.

---

## 7. Huge documents

### 7.1 Where the time goes [measured, large.md]

| Stage | Time | Comment |
|---|---:|---|
| md4c: parse + collect the text of 101,235 leaf blocks (UTF-8 → UTF-16) | 24–30 ms | the parse itself is 10.6 ms, the rest is copying and conversion |
| DirectWrite: `CreateTextLayout` + `GetMetrics` for the **first 40 blocks** | **1.6–2.0 ms** | that is more than one screen |
| The same for **all 101,235 blocks**, 1 thread | **0.95–1.06 s** | 2.35 million UTF-16 characters, height 2.29 million DIP |
| The same, 16 threads, shared factory | 0.38–0.45 s | a speedup of only 2.4× (probably the shared font cache under a lock) |
| The same, 16 threads, isolated factories | 0.50–0.59 s | worse: each factory has its own initialization |
| **Keeping all 101,235 `IDWriteTextLayout` alive** | 1.75–1.79 s | **+1.52 GB private bytes, +584 MB working set** (~15 KB per block) |
| medium.md, all 1,471 blocks | 15 ms | |

The measurement is optimistic: one font, no inline formatting, no fallback for CJK and emoji. In reality layout is more expensive.
But that does not change the ratios: **parsing a large file is cheap, laying out the whole file costs 1 s and 1.5 GB**.

### 7.2 Chromium (= WebView2) on the HTML of large.md [measured, Edge 153, 3 runs, ms]

| Strategy | HTML | DOM parsed | FCP | Frame after the document (2×rAF) | Everything inserted |
|---|---:|---:|---:|---:|---:|
| medium in full (for scale) | 98 KB | 15.5 | 236 | 189 | — |
| large in full | 6.56 MB, 158,861 nodes | 1,014 | 804 | **1,851** | — |
| large + `.markdown-body > * { content-visibility:auto; contain-intrinsic-size:auto 60px }` | 6.56 MB | 466 | 348 | **728** | — |
| large: first 64 KB in the page, the rest in 256 KB chunks (`insertAdjacentHTML` via `setTimeout(0)`) | 7.05 MB¹ | 181 | 252 | **263** | 1,308 |

¹ In this test the remainder sits in the page itself as a JS string, and parsing it is what ends up in the 181 ms. In the product the remainder should
be delivered via `postMessage` or fetch, which is even faster. The technique is already used by the `cpp-webview2` prototypes
(the first ~24 KB of HTML) and `rust-webview` (a first chunk of 64 KB).
`content-visibility` estimates the document height at 2.07 million px against the real 2.39 million. That is −14%: the scrollbar
"drifts" while the browser refines the heights.
A 2×rAF frame in headless Edge has a floor of ~190–230 ms even for light pages, so for comparing libraries
in §3–5 I used "script executed" and "work done".

### 7.3 Strategy for FastMD (native path)

1. **Parse the whole file at once.** md4c fits within 11 ms on 3.8 MB; an incremental parser is not needed.
   If building the document model is expensive, you can first build a prefix up to a safe block boundary (≥ 48 K characters),
   as `cpp-d2d` does. The cost: link reference definitions `[x]: url` in the tail of the document are resolved only after the full parse.
   Those need to be redrawn in the background.
2. **Lay out only the visible part and ± 1 screen** (~2 ms). **Estimate** the heights of the other blocks in O(1) per block:
   characters × average width / column width × line height; for images take the dimensions from WIC.
   Scrolling and the scrollbar work right away.
3. **Refine the heights in the background** in batches on a thread pool: on large.md that takes 0.4–1 s. Use the block at the top of the
   screen as the scroll anchor, so that refining heights does not shift what is visible.
4. **Don't keep the layouts of invisible blocks.** An LRU of a few screens; for the rest store only the height and offset.
   The difference is 1.5 GB versus a few megabytes.
5. Build the search index over the source text: md4c offsets are available (§2.5); map match highlighting onto blocks.
6. Finish highlighting, images, math and diagrams in the same "visible first" queue (§3.3, §4.3, §5.3, §6).

---

## 8. Survey of existing readers and their pipelines

Open-time measurements are from report 06 (medium.md, "visually ready", noisy machine). Complaints come from the sources collected there.

| Product | Engine | Parser | Highlighting | Math | Diagrams | Open time |
|---|---|---|---|---|---|---|
| Typora 1.14.10 | Electron 42 | closed | closed | MathJax 4 | Mermaid 11 | [report 06] VC **1.68 s**; large.md refused ("file too large"); complaints of 7–8 s |
| MarkText 0.20-rc.4 | Electron 42 | Muya: a **marked** fork for reading, turndown for writing [source] | Prism | KaTeX | Mermaid, Vega, PlantUML | [report 06] VC **3.8 s**; issue #3862: 12 s cold / 3 s warm |
| Obsidian 1.11 | Electron | not disclosed (editor on CodeMirror 6) | Prism (reportedly) | MathJax | Mermaid | forum: 4–5 … 20+ s |
| VS Code 1.121, preview | Electron | **markdown-it** | highlight.js | KaTeX (built-in markdown-math extension) [source] | extension | `baselines-external` report |
| Markdown Monster 4.5.3 | WPF .NET 10 + 2× WebView2 | **Markdig** | highlight.js | MathJax / KaTeX | Mermaid [source] | [report 06] p50 **4.5 s** |
| PowerToys 0.100.2, preview handler | .NET + WebView2 | **Markdig** | — | — | — | [report 06] p50 **0.62 s**, VC 1.4 s; #46087: ~400 ms even for .txt |
| QuickLook 4.5 + MarkdownViewer | WPF + WebView2 | markdown-it | highlight.js | MathJax | Mermaid | resident process |
| Inlyne 0.5.3 | Rust + wgpu, own layout | **comrak** | syntect | — | — | [report 06] **1.23 s**; large 3.6 s, 940 MB |
| Ferrite 0.3.0 | Rust + egui | **comrak** | syntect | — | native Mermaid | [report 06] **1.34 s**; large — no window within 30 s |
| Zed (preview) | Rust + GPUI | **pulldown-cmark** | tree-sitter | PR with RaTeX (#58593) | extension | not measured |
| Glow 3.0 (TUI) | Go | goldmark (via glamour) | chroma | — | — | [report 06] medium 0.27 s, large 9.5 s |
| Qt `QTextDocument::setMarkdown` | Qt | **md4c** (0.5.3 in Qt 6.11) [source] | — | — | — | `qt` prototype: ~190–210 ms |
| GitHub.com (the reference look) | server | **cmark-gfm** | Linguist grammars (TextMate) | MathJax | Mermaid in an iframe | — |
| MDHero (2026) | Tauri (WebView2) | not disclosed | ? | KaTeX | Mermaid | website: "any .md in two seconds" |

Conclusions from the survey:

1. **Every mainstream reader is a browser engine plus a JS pipeline** (markdown-it or marked, highlight.js or Prism,
   MathJax or KaTeX, mermaid.js). Their open time (0.6–5 s) is determined by the engine, not by the parser.
   The parser from §2.1 costs 1–4 ms on medium.
2. The native newcomers (Inlyne, Ferrite) take comrak and syntect, but lose on GPU initialization and on laying out the whole document
   (report 06). That is exactly what §7.3 proposes to avoid.
3. **None of them does "visible first"** for a large document. Typora refuses to open large.md,
   Ferrite hangs. This is where FastMD has room to pull clearly ahead.

---

## 9. Conclusions for FastMD

### 9.1 Parser

| UI stack | Recommendation | Why | What to add |
|---|---|---|---|
| **C++ (Win32 + DirectWrite / D2D, RichEdit, WebView2 host)** | **md4c** (0.6.0 once released; until then a pinned master commit) | Fastest of all: 342 MB/s parsing, 0.14 ms on 53 KB. 100% CommonMark 0.31.2. SAX callbacks build the render model without an intermediate AST. C99, ~0.1 MB, MIT. Footnotes and GitHub Alerts are in 0.6.0-dev. Text offsets are `ptr − src`. Already in all the C++ prototypes (0.5.x) | Move from the vendored 0.5.x to 0.6.0. Strip YAML front matter before parsing. Resolve the GFM autolink and table edge cases (4 + 2 examples) if parity with GitHub matters |
| **Rust (wry/WebView2, GPU UI)** | **pulldown-cmark 0.13** | 180–215 MB/s, byte ranges for free, CommonMark 100% semantically. Has alerts, math, front matter, footnotes, wikilinks. Already in `rust-webview` and `rust-gpu` | **A literal-autolink pass** (0/11 GFM): `linkify` over `Event::Text` outside code and links |
| Rust, "exactly like GitHub" needed | comrak 0.55 | The only one with 652/652 + 24/24, has sourcepos, all extensions | ×8 slower than pulldown-cmark, but 0.9 ms on 53 KB and 150 ms on 3.8 MB. Acceptable if parsing is off the UI thread |
| .NET (WPF, WinUI 3, Avalonia) | Markdig 1.3.2 **only with NativeAOT** (or at least ReadyToRun) | JIT adds ~55 ms to the first document, AOT 2 ms | Tables diverge from GFM (4/8), no e-mail autolinks |
| Web engine (Electron/Tauri) | **don't parse in the page**; if you must, markdown-it (100% CommonMark, plugins) or marked (faster, but entity errors) | A JS parser adds 25–55 ms of cold start | — |
| **Don't use** | markdown-rs, micromark | Superlinearity (16–26 s on 3.8 MB) and 0.7 MB/s | — |

### 9.2 Code highlighting

- **C++: Lexilla 5.5.3**, statically linked, a subset of lexers for the top 20–30 languages (estimated ~0.2–0.5 MB based on the 5- and 125-lexer measurements).
  Highlight **synchronously before the first frame**: < 0.2 ms for the first block, ~30 MB/s. Take keywords from the SciTE `.properties`,
  colors from the GitHub palette in `style.css`.
- **"IDE-grade" quality** (embedded languages, semantics) — tree-sitter, lazily per language, off the UI thread, with a cache.
  That is v2, not v1.
- **Rust:** syntect is already in `rust-webview`, but it is 30 times slower than Lexilla: background only and visible only.
  Alternatives are tree-sitter-highlight or Lexilla via `cc`.
- **Web path:** Prism (the fastest in V8, 58 KB) or highlight.js common (129 KB). Shiki — no.

### 9.3 Math and diagrams

- **v1:** the source of formulas and Mermaid in a styled block with a badge. Zero milliseconds added to the first frame.
  This covers the 98% of documents that have none, and doesn't break the other 2%.
- **v1.x, native, in the background:**
  - **RaTeX** (C ABI): 70 µs per formula, display list → DirectWrite with the KaTeX TTFs (514 KB);
  - **mmdr**: 40–90 ms per flowchart → SVG → lunasvg/resvg.
  Both are young projects (0.1.x / 0.3.x): pin the versions, run them on real corpora (report 06, §4), keep a fallback.
- **Fidelity fallback:** a hidden WebView2 with a persistent profile: KaTeX or Temml; MathJax only without enrichment;
  mermaid ESM or IIFE. The result (SVG or a snapshot) is embedded in the native document.
  The V8 code cache gives a ~4× gain from the 3rd launch on.

### 9.4 Large documents and images

- Parse in full (≤ 20 ms on 4 MB). Layout — **only the visible part ± 1 screen** (~2 ms). The rest — height estimates and background
  layout on a pool (0.4–1 s on large). **An LRU of layouts** instead of keeping them all (1.5 GB → megabytes).
- Images: `GetSize` synchronously (0.2 ms, the space is reserved), decoding asynchronously (20–130 ms) and only near the viewport.
  For JPEG — `IWICBitmapSourceTransform` (DCT scaling), cache by (path, mtime, width).
- If WebView2 is chosen: a first HTML chunk of 24–64 KB, the rest via `postMessage` in chunks + `content-visibility:auto`.
  A frame after 0.26 s instead of 1.85 s on large.

### 9.5 Pipeline budget for the first frame (native path, medium.md) [measured]

| Step | Time |
|---|---:|
| Reading the 53 KB file (3.8 MB: 1–2 ms via mmap) | < 1 ms |
| md4c → document model | 0.2–0.5 ms |
| Lexilla: visible code blocks | < 0.3 ms |
| WIC: dimensions of visible images (+6–13 ms if it is the first codec in the process; this call is better moved to a thread) | 0.2 ms per image |
| DirectWrite: layout of ~40 visible blocks | ~2 ms |
| **Total document work before drawing** | **~3–5 ms** (+ codec) |

The whole Markdown pipeline fits within 5 ms. The rest of the time to `t_content` is process startup, the window, the D2D device and `Present`
(reports 01, 03, 05 and the `cpp-d2d` prototype: 64–80 ms `t_content` against a `baseline-win32` floor of 70–90 ms).
**The choice of parser and highlighter has almost no effect on "bullet" speed, as long as they are native and work "visible first".**
What decides it is the UI stack and refusing to lay out the whole document.

### 9.6 Risks

- **GFM parity.** With md4c and pulldown-cmark some documents will not render 1:1 as on GitHub: autolinks, table edge cases.
  A regression suite is needed — the GFM spec plus real READMEs from the report 06 corpus.
- **Young native renderers** (RaTeX 0.1, mmdr 0.3): fidelity and maintenance. A WebView2 fallback is needed.
- **Lexilla:** keywords and colors are our responsibility. The quality is lexical, without embedded languages (JS in HTML, etc.).
- **Layout of real documents** (inline formatting, fallback fonts, emoji) is more expensive than my measurement.
  The §7.1 numbers are a lower bound.

---

## 10. Notes on the bench and the lab (for the orchestrator)

1. **CRLF in `c/cmark-gfm/test/spec.txt`.** The local checkout has CRLF, upstream has LF. If the spec is read with
   `newline='\n'`, as `spec_tests.py` does, the GFM examples reach the parsers with `\r\n`. My `spec_compliance.py` normalizes line endings.
2. **`node/bench-math.mjs` and `rust/src/bin/hl.rs`** (from the previous agent) wrote `\ ` (a TeX control space) in matrices
   instead of `\\`, so the matrices came out as a single row. Fixed; the numbers in §4 are after the fix.
   The `mathjax4-svg` variant did not work on Windows (§4.1, footnote 1); it was rewritten to use the direct modules.
3. **`docs/mermaid-compare.png`** was cut off by a bug in the comparison page. Fixed and reshot (§5.2).
4. **The corpus has no math, Mermaid, footnotes, Alerts, front matter or raw HTML.** For "rich mode" prototypes
   I propose adding `bench/corpus/rich.md`: 3 diagrams, 20 formulas, Alerts, footnotes, front matter, `<details>`, `<picture>`, SVG badges.
   Otherwise nobody measures the lazy paths. Verified: there is no raw HTML, no `[^x]` footnotes, no `[!NOTE]` and no front matter in either medium or large.
   Both files are generated, uniform sections `## N. Раздел N: …`, each with a table and a code block (24 and 1,659 sections).
   That is convenient for layout, but they don't give the variety of real documents.
5. **Headless Edge:** a 2×rAF frame at the start of a page has a floor of ~190–230 ms. For comparing JS libraries
   it is better to use marks inside the page. For prototypes the bench measures real presentation, so this note does not apply to them.
6. The prototypes vendor md4c 0.5.2 and 0.5.3. Footnotes and Alerts exist only in master (0.6.0-dev, no tag yet). The lab measured master `7fc1815`
   and, separately, the 0.5.3 release from `cpp-webview2/third_party/md4c` (read-only, built into `c/out-md4c053/`).
   A regression with an autolink ending in a period was found in master (GFM, example 623).
7. The C/C++ part of the lab now builds reproducibly: `c/build_benches.cmd` is verified and writes to the new folder `c/out-rebuild/`.
   Rust, .NET and Node — Appendix A.

---

## 11. Sources

- md4c: https://github.com/mity/md4c, CHANGELOG (0.6.0-dev: footnotes, admonitions, highlight, sub/sup, spoilers) — https://github.com/mity/md4c/blob/master/CHANGELOG.md ; 0.6.0 release plan — https://github.com/mity/md4c/issues/422 ; md4c in Qt 6.11 — https://doc.qt.io/qt-6/qtgui-attribution-md4c.html
- pulldown-cmark: https://github.com/pulldown-cmark/pulldown-cmark (source maps, SIMD); flags checked against the source `pulldown-cmark-0.13.4/src/lib.rs`
- comrak: https://github.com/kivikakk/comrak (options checked against `comrak-0.55.0/src/parser/options.rs`)
- markdown-rs: https://github.com/wooorm/markdown-rs ; micromark: https://github.com/micromark/micromark
- cmark / CommonMark 0.31.2: https://github.com/commonmark/cmark , https://spec.commonmark.org/0.31.2/ ; cmark-gfm / GFM spec 0.29: https://github.com/github/cmark-gfm , https://github.github.com/gfm/
- Markdig: https://github.com/xoofx/markdig (extension methods checked against the XML documentation of the 1.3.2 package)
- markdown-it: https://github.com/markdown-it/markdown-it ; marked: https://github.com/markedjs/marked ; commonmark.js: https://github.com/commonmark/commonmark.js
- Lexilla: https://www.scintilla.org/Lexilla.html , https://www.scintilla.org/lexilla553.zip ; Scintilla: https://www.scintilla.org/scintilla566.zip
- tree-sitter: https://tree-sitter.github.io/ ; syntect: https://github.com/trishume/syntect ; Shiki: https://shiki.style/ ; highlight.js: https://highlightjs.org/ ; Prism: https://prismjs.com/
- KaTeX: https://katex.org/ ; Temml: https://temml.org/ ; MathJax 4: https://docs.mathjax.org/ ; pulldown-latex: https://crates.io/crates/pulldown-latex
- RaTeX: https://github.com/erweixin/RaTeX (MIT, C ABI `ratex-ffi`, KaTeX fonts) ; Zed PR "Math rendering for Markdown preview using RaTeX": https://github.com/zed-industries/zed/pull/58593
- MicroTeX: https://github.com/NanoMichael/MicroTeX ; RichEdit math: https://devblogs.microsoft.com/math-in-office/setting-and-getting-text-in-various-formats/
- mermaid-rs-renderer (mmdr): https://github.com/1jehuang/mermaid-rs-renderer ; Mermaid: https://mermaid.js.org/
- V8 code caching (cold/warm/hot run, minimum 1 KiB, tied to the URL): https://v8.dev/blog/code-caching-for-devs
- `content-visibility`: https://web.dev/articles/content-visibility
- WIC `IWICBitmapSourceTransform`: https://learn.microsoft.com/windows/win32/api/wincodec/nn-wincodec-iwicbitmapsourcetransform ; DirectWrite `IDWriteTextLayout`: https://learn.microsoft.com/windows/win32/api/dwrite/nn-dwrite-idwritetextlayout
- Reader stacks: VS Code markdown-math — https://github.com/microsoft/vscode/blob/main/extensions/markdown-math/README.md ; MarkText Muya — https://github.com/marktext/muya ; Markdown Monster (WebView2, Mermaid, MathJax) — https://weblog.west-wind.com/posts/2021/Jul/22/Markdown-Monster-20-is-here ; Zed markdown — https://zed.dev/docs/languages/markdown
- Measurements of and complaints about competitors' open times: `research/06-product-ux-landscape.md` §2–3 (with links to forums and issues)

---

## Appendix A. How to reproduce

```powershell
# 0) C/C++: md4c, cmark, cmark-gfm, WIC/DirectWrite, Lexilla (MSVC через обёртку стенда)
C:\Main\Projects\FastMD\bench\tools\msvc.cmd research\lab\md-pipeline\c\build_benches.cmd out   # или out-rebuild
C:\Main\Projects\FastMD\bench\tools\msvc.cmd research\lab\md-pipeline\c\build_lexilla.cmd        # нужен c\lexilla-dl\ (URL в скрипте)
# md4c 0.5.3 (релиз из cpp-webview2, только чтение) -> c\out-md4c053\ : bench_md4c.exe и md4c_stdin.exe
$S='bench\protos\cpp-webview2\third_party\md4c'; $C='research\lab\md-pipeline\c'
bench\tools\msvc.cmd cl /nologo /utf-8 /O2 /MD /DNDEBUG /I$S /Fo$C\out-md4c053\ /Fe$C\out-md4c053\md4c_stdin.exe $C\md4c_stdin.c $S\md4c.c $S\md4c-html.c $S\entity.c
bench\tools\msvc.cmd cl /nologo /utf-8 /O2 /MD /DNDEBUG /I$S /Fo$C\out-md4c053\ /Fe$C\out-md4c053\bench_md4c.exe $C\bench_md4c.c $S\md4c.c $S\md4c-html.c $S\entity.c

# 1) Rust: parsers, hl, specbatch; mmdr; RaTeX
cd research/lab/md-pipeline/rust;       cargo build --release
cd ../rust-mmdr;                        cargo build --release; ./target/release/mmdrbench.exe
cd ../rust-ratex;                       cargo build --release; ./target/release/ratexbench.exe

# 2) .NET: Markdig (JIT, NativeAOT, ReadyToRun) + batch-раннер спецификаций
cd ../dotnet; dotnet build -c Release
dotnet publish -c Release -r win-x64 -p:PublishAot=true -o out-aot
dotnet publish -c Release -r win-x64 --self-contained -p:PublishReadyToRun=true -o out-r2r
cd ../compliance/dotnet-spec; dotnet build -c Release

# 3) Node: зависимости (package.json в node/)
cd ../../node; npm ci

# 4) Замеры
python research/lab/md-pipeline/run_parsers.py 5                      # §2.1  (дважды)
python research/lab/md-pipeline/compliance/spec_compliance.py         # §2.3  -> compliance/results_compliance.{md,json}
python research/lab/md-pipeline/compliance/spec_compliance.py 0.5.3   # только md4c 0.5.3 -> results_compliance_0_5_3.json
python research/lab/md-pipeline/compliance/show_fails.py pulldown cm  # диффы провалов
rust/target/release/parsers.exe docs/medium_x16.md 3 markdown-rs      # §2.1 масштабирование
rust/target/release/hl.exe <corpus>/large.md syntect|treesitter|latex # §3, §4
c/out-lexilla/bench_lexilla_subset.exe <corpus>\large.md              # §3
cd node; node bench-hl.mjs prism <corpus>/large.md                    # §3 (hljs-full|hljs-core7|prism|shiki-*)
node bench-math.mjs katex|mathjax3-svg|mathjax3-chtml|mathjax4-svg    # §4.1
node bench-browser.mjs 5; node bench-codecache.mjs; node bench-huge.mjs 3   # §3–5, §7.2 (headless Edge 153)
node shot-mermaid.mjs; node shot-ratex.mjs                            # картинки сравнения в docs/
python gen_images.py; c/out/bench_native_io.exe wic docs\photo_4000x3000.jpg ...   # §6 (43 MB картинок, потом удалить)
c/out/bench_native_io.exe dwrite <corpus>\large.md 40                 # §7.1
c/out/md4c_ptrcheck.exe <corpus>\large.md                             # §2.5
```

For `node bench-*.mjs` with headless Edge the local proxy from the environment is not needed: the scripts pass `--no-proxy-server`,
and for Node I set `NO_PROXY=127.0.0.1,localhost`.
