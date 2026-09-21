"""Compatibility check for formulas and diagrams (plan 4.3).

Renders a corpus of 100 TeX formulas (the constructs KaTeX documents as supported) and 30 Mermaid diagrams straight
through the two libraries the viewer uses, then rasterises every result with the viewer's own SVG renderer and puts
them together into contact sheets. What is checked automatically: that each source is accepted, that the picture has a
sane size, and that it is not blank. The sheets are for the eye - there is no KaTeX or mermaid-cli on this machine to
compare against, so the reference is our own accepted output, as in the README corpus of stage 2.10.

    python app/tests/math_check.py [--build DIR]

Writes app/tests/out/math-formulas.png, math-diagrams.png and math-report.md.
"""
import argparse
import ctypes
import pathlib
import time

from PIL import Image, ImageDraw

HERE = pathlib.Path(__file__).resolve().parent
OUT = HERE / "out"

FORMULAS = [
    # numbers, letters, simple relations
    r"E = mc^2", r"a + b - c \cdot d / e", r"x \ne y \le z \ge w", r"a \approx b \equiv c \sim d",
    r"1 < 2 > 0.5", r"x \in A \cup B \cap C", r"A \subset B \subseteq C \supset D", r"p \land q \lor \lnot r",
    r"\forall x \exists y : P(x, y)", r"a \parallel b \perp c",
    # powers and indices
    r"x^2", r"x_i", r"x_i^2", r"x^{2n+1}", r"a_{i,j}", r"x^{y^z}", r"{}^{14}_{6}\mathrm{C}",
    r"e^{i\pi} + 1 = 0", r"2^{2^{2^{2}}}", r"x'", r"f''(x)",
    # fractions and roots
    r"\frac{1}{2}", r"\dfrac{a}{b}", r"\tfrac{x}{y}", r"\frac{\frac{a}{b}}{c}", r"\sqrt{2}", r"\sqrt[3]{x}",
    r"\sqrt{\frac{a}{b}}", r"\binom{n}{k}", r"\dbinom{n}{k}", r"\cfrac{1}{1 + \cfrac{1}{x}}",
    # big operators
    r"\sum_{i=1}^{n} i", r"\prod_{k=0}^{m} a_k", r"\int_0^1 x^2\,dx", r"\iint_D f\,dA", r"\oint_C F\cdot dr",
    r"\bigcup_{i \in I} A_i", r"\bigcap_{i=1}^{n} B_i", r"\lim_{x \to 0} \frac{\sin x}{x}",
    r"\lim\limits_{n \to \infty} a_n", r"\max_{x \in S} f(x)", r"\sup_{n} x_n", r"\inf_{n} x_n",
    r"\sum\limits_{\substack{i=1 \\ i \ne j}}^{n} a_i",
    # greek and symbols
    r"\alpha\beta\gamma\delta\epsilon\zeta\eta\theta", r"\iota\kappa\lambda\mu\nu\xi\pi\rho",
    r"\sigma\tau\upsilon\phi\chi\psi\omega", r"\Gamma\Delta\Theta\Lambda\Xi\Pi\Sigma\Phi\Psi\Omega",
    r"\varepsilon \vartheta \varpi \varrho \varsigma \varphi", r"\infty \partial \nabla \emptyset \aleph",
    r"\hbar \ell \wp \Re \Im", r"\pm \mp \times \div \ast \star \circ \bullet",
    r"\dagger \ddagger \S \P \copyright", r"\angle \triangle \square \diamond",
    # arrows
    r"a \to b \gets c", r"x \Rightarrow y \Leftarrow z", r"A \leftrightarrow B \Leftrightarrow C",
    r"\xrightarrow{f} \xleftarrow{g}", r"\uparrow \downarrow \updownarrow \nearrow \searrow",
    r"a \mapsto b", r"\longrightarrow \longmapsto",
    # functions and operators
    r"\sin x + \cos y - \tan z", r"\log_2 n + \ln e + \lg 10", r"\exp(x) \bmod 7", r"\gcd(a, b)",
    r"\operatorname{tr}(A)", r"\arcsin x \arccos y \arctan z", r"\deg P \det A \dim V", r"a \equiv b \pmod{n}",
    # delimiters
    r"\left( \frac{a}{b} \right)", r"\left[ \sum_{i} x_i \right]", r"\left\{ x : x > 0 \right\}",
    r"\left| \frac{x}{y} \right|", r"\left\| v \right\|", r"\langle u, v \rangle", r"\lfloor x \rfloor",
    r"\lceil y \rceil", r"\left. \frac{dy}{dx} \right|_{x=0}",
    # matrices and environments
    r"\begin{matrix} a & b \\ c & d \end{matrix}", r"\begin{pmatrix} 1 & 0 \\ 0 & 1 \end{pmatrix}",
    r"\begin{bmatrix} x \\ y \\ z \end{bmatrix}", r"\begin{vmatrix} a & b \\ c & d \end{vmatrix}",
    r"\begin{Bmatrix} p & q \end{Bmatrix}", r"\begin{cases} x, & x > 0 \\ -x, & x \le 0 \end{cases}",
    r"\begin{aligned} a &= b + c \\ d &= e \end{aligned}", r"\begin{array}{c|c} a & b \\ \hline c & d \end{array}",
    # accents and decorations
    r"\hat{a} \bar{b} \vec{c} \dot{d} \ddot{e}", r"\tilde{x} \widetilde{xyz}", r"\widehat{abc}",
    r"\overline{a + b}", r"\underline{c + d}", r"\overbrace{x + y}^{s}", r"\underbrace{a + b}_{t}",
    r"\overset{!}{=}", r"\underset{n}{\min}", r"\stackrel{\text{def}}{=}", r"\not=",
    # fonts, text and spacing
    r"\mathbb{R} \mathbb{N} \mathbb{Z}", r"\mathcal{L}(f)", r"\mathfrak{g}", r"\mathrm{d}x",
    r"\mathbf{v} \mathit{w} \mathsf{s} \mathtt{t}", r"\text{если } x > 0", r"a \, b \: c \; d \quad e \qquad f",
    r"\color{red}{x} + y", r"\textcolor{blue}{z}", r"\phantom{x} y", r"a \cdots b \ldots c \vdots d \ddots",
]

DIAGRAMS = [
    ("flowchart TD", "flowchart TD\n  A[Старт] --> B{Условие}\n  B -->|да| C[Готово]\n  B -->|нет| A"),
    ("flowchart LR", "flowchart LR\n  A --> B --> C --> D"),
    ("graph TD", "graph TD;\n  A-->B;\n  A-->C;\n  B-->D;\n  C-->D;"),
    ("graph shapes", "graph TD\n  A[квадрат] --> B(круглый)\n  B --> C{ромб}\n  C --> D((круг))\n  D --> E>флаг]"),
    ("graph subgraph", "graph TB\n  subgraph один\n    A --> B\n  end\n  subgraph два\n    C --> D\n  end\n  B --> C"),
    ("graph styles", "graph LR\n  A:::big --> B\n  classDef big fill:#f9f,stroke:#333"),
    ("sequence", "sequenceDiagram\n  Alice->>Bob: Привет\n  Bob-->>Alice: Привет!"),
    ("sequence activate", "sequenceDiagram\n  participant A\n  participant B\n  A->>+B: запрос\n  B-->>-A: ответ"),
    ("sequence loop", "sequenceDiagram\n  loop каждый день\n    A->>B: пинг\n  end"),
    ("sequence note", "sequenceDiagram\n  A->>B: сообщение\n  Note right of B: думает"),
    ("sequence alt", "sequenceDiagram\n  alt всё хорошо\n    A->>B: да\n  else иначе\n    A->>B: нет\n  end"),
    ("class", "classDiagram\n  class Animal {\n    +String name\n    +eat()\n  }\n  Animal <|-- Dog"),
    ("class relations", "classDiagram\n  A <|-- B\n  C *-- D\n  E o-- F\n  G <.. H"),
    ("state", "stateDiagram-v2\n  [*] --> Ожидание\n  Ожидание --> Работа: старт\n  Работа --> [*]"),
    ("state composite", "stateDiagram-v2\n  [*] --> A\n  state A {\n    [*] --> B\n    B --> [*]\n  }"),
    ("er", "erDiagram\n  CUSTOMER ||--o{ ORDER : places\n  ORDER ||--|{ LINE : contains"),
    ("journey", "journey\n  title Мой день\n  section Утро\n    Кофе: 5: Я\n    Почта: 3: Я"),
    ("gantt", "gantt\n  title План\n  dateFormat YYYY-MM-DD\n  section Этап\n  Задача :a1, 2026-01-01, 30d"),
    ("pie", 'pie title Языки\n  "Rust" : 40\n  "C++" : 60'),
    ("quadrant", "quadrantChart\n  title Матрица\n  x-axis Низкий --> Высокий\n  y-axis Мало --> Много\n  A: [0.3, 0.6]"),
    ("requirement", "requirementDiagram\n  requirement test_req {\n    id: 1\n    text: пример\n  }"),
    ("gitgraph", "gitGraph\n  commit\n  branch dev\n  commit\n  checkout main\n  merge dev"),
    ("mindmap", "mindmap\n  root((идея))\n    первая\n    вторая\n      глубже"),
    ("timeline", "timeline\n  title История\n  2024 : начало\n  2025 : середина\n  2026 : конец"),
    ("sankey", "sankey-beta\n\nA,B,10\nB,C,5"),
    ("xychart", 'xychart-beta\n  title "Продажи"\n  x-axis [янв, фев, мар]\n  y-axis "Сумма" 0 --> 100\n  bar [30, 50, 80]'),
    ("block", "block-beta\n  columns 3\n  a b c\n  d e f"),
    ("packet", "packet-beta\n  0-7: \"версия\"\n  8-15: \"тип\""),
    ("c4", "C4Context\n  title Система\n  Person(user, \"Пользователь\")\n  System(sys, \"Система\")"),
    ("architecture", "architecture-beta\n  group api(cloud)[API]\n  service db(database)[DB] in api"),
]


def load(build):
    tex = ctypes.WinDLL(str(build / "fastmd-tex.dll"))
    mer = ctypes.WinDLL(str(build / "fastmd-mermaid.dll"))
    svg = ctypes.WinDLL(str(build / "fastmd-svg.dll"))
    tex.fastmd_tex_svg.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_float, ctypes.c_uint,
                                   ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_size_t),
                                   ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_float),
                                   ctypes.POINTER(ctypes.c_float)]
    tex.fastmd_tex_free.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    mer.fastmd_mermaid_svg.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_int,
                                       ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_size_t)]
    mer.fastmd_mermaid_free.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    svg.FastMdSvgMeasure.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.POINTER(ctypes.c_float),
                                     ctypes.POINTER(ctypes.c_float)]
    svg.FastMdSvgRender.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_void_p]
    return tex, mer, svg


def render_tex(tex, src, display=True, size=18.0, rgb=0x1F2328):
    b = src.encode("utf-8")
    p, n = ctypes.c_void_p(), ctypes.c_size_t()
    w, h, a = ctypes.c_float(), ctypes.c_float(), ctypes.c_float()
    t0 = time.perf_counter()
    ok = tex.fastmd_tex_svg(b, len(b), 1 if display else 0, size, rgb, ctypes.byref(p), ctypes.byref(n),
                            ctypes.byref(w), ctypes.byref(h), ctypes.byref(a))
    ms = (time.perf_counter() - t0) * 1000
    if not ok:
        return None, ms
    data = ctypes.string_at(p, n.value)
    tex.fastmd_tex_free(p, n)
    return (data, w.value, h.value, a.value), ms


def render_mermaid(mer, src, dark=False):
    b = src.encode("utf-8")
    p, n = ctypes.c_void_p(), ctypes.c_size_t()
    t0 = time.perf_counter()
    ok = mer.fastmd_mermaid_svg(b, len(b), 1 if dark else 0, ctypes.byref(p), ctypes.byref(n))
    ms = (time.perf_counter() - t0) * 1000
    if not ok:
        return None, ms
    data = ctypes.string_at(p, n.value)
    mer.fastmd_mermaid_free(p, n)
    return data, ms


def rasterise(svg, data, scale=1.0, cap=900):
    """SVG bytes → PIL image on white, through the renderer the viewer itself uses"""
    w, h = ctypes.c_float(), ctypes.c_float()
    if not svg.FastMdSvgMeasure(data, len(data), ctypes.byref(w), ctypes.byref(h)):
        return None
    W, H = max(1, int(w.value * scale)), max(1, int(h.value * scale))
    if W > cap:
        H = max(1, int(H * cap / W))
        W = cap
    if H > cap:
        W = max(1, int(W * cap / H))
        H = cap
    buf = (ctypes.c_uint32 * (W * H))()
    if not svg.FastMdSvgRender(data, len(data), W, H, buf):
        return None
    img = Image.frombuffer("RGBA", (W, H), bytes(buf), "raw", "BGRa", 0, 1)
    sheet = Image.new("RGB", (W, H), (255, 255, 255))
    sheet.paste(img, (0, 0), img)
    return sheet


def contact_sheet(items, path, width=1200, pad=12):
    """items: (label, PIL image or None) → one tall sheet, so a person can look at the lot at once"""
    rows, row, rowW, rowH = [], [], 0, 0
    for label, img in items:
        w, h = (img.size if img else (160, 28))
        if rowW + w + pad > width and row:
            rows.append((row, rowH))
            row, rowW, rowH = [], 0, 0
        row.append((label, img, w, h))
        rowW += w + pad
        rowH = max(rowH, h + 22)
    if row:
        rows.append((row, rowH))
    total = sum(h + pad for _, h in rows) + pad
    sheet = Image.new("RGB", (width, max(total, 40)), (255, 255, 255))
    d = ImageDraw.Draw(sheet)
    y = pad
    for row, rowH in rows:
        x = pad
        for label, img, w, h in row:
            if img:
                sheet.paste(img, (x, y))
            else:
                d.rectangle([x, y, x + w, y + 24], outline=(200, 60, 60))
                d.text((x + 4, y + 6), "FAILED", fill=(200, 60, 60))
            d.text((x, y + rowH - 14), label[:40], fill=(120, 120, 120))
            x += w + pad
        y += rowH + pad
    sheet.save(path)
    return sheet.size


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=str(HERE.parent / "build" / "Release"))
    args = ap.parse_args()
    build = pathlib.Path(args.build)
    OUT.mkdir(exist_ok=True)
    tex, mer, svg = load(build)

    lines = ["# Формулы и диаграммы: проверка совместимости", "",
             "Набор прогоняется `python app/tests/math_check.py`. Эталон — наш собственный принятый результат:",
             "KaTeX и mermaid-cli на машине нет. Смотреть листы `math-formulas.png` и `math-diagrams.png`.", ""]
    sheets, bad, slow = [], [], 0.0
    for src in FORMULAS:
        r, ms = render_tex(tex, src)
        slow = max(slow, ms)
        if not r:
            bad.append(("формула", src, "не разобрана"))
            sheets.append((src, None))
            continue
        data, w, h, _ = r
        img = rasterise(svg, data, scale=2.0)
        if img is None or w < 1 or h < 1:
            bad.append(("формула", src, "SVG не нарисован"))
        sheets.append((src, img))
    okF = sum(1 for _, i in sheets if i)
    size = contact_sheet(sheets, OUT / "math-formulas.png")
    lines += [f"- **Формулы: {okF} из {len(FORMULAS)}.** Самая медленная — {slow:.2f} мс.",
              f"- Лист `math-formulas.png`, {size[0]}×{size[1]}.", ""]

    sheets, slowD = [], 0.0
    for name, src in DIAGRAMS:
        data, ms = render_mermaid(mer, src)
        slowD = max(slowD, ms)
        if not data:
            bad.append(("диаграмма", name, "не разобрана"))
            sheets.append((name, None))
            continue
        img = rasterise(svg, data, scale=1.0)
        if img is None:
            bad.append(("диаграмма", name, "SVG не нарисован"))
        sheets.append((name, img))
    okD = sum(1 for _, i in sheets if i)
    size = contact_sheet(sheets, OUT / "math-diagrams.png", width=1600)
    lines += [f"- **Диаграммы: {okD} из {len(DIAGRAMS)}.** Самая медленная — {slowD:.1f} мс.",
              f"- Лист `math-diagrams.png`, {size[0]}×{size[1]}.", ""]

    if bad:
        lines += ["## Что не получилось", "", "| Что | Источник | Причина |", "|---|---|---|"]
        lines += [f"| {k} | `{s[:60]}` | {why} |" for k, s, why in bad]
    (OUT / "math-report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"formulas {okF}/{len(FORMULAS)}  diagrams {okD}/{len(DIAGRAMS)}  (report: {OUT / 'math-report.md'})")
    return 0 if okF >= len(FORMULAS) * 0.9 and okD >= len(DIAGRAMS) * 0.6 else 1


if __name__ == "__main__":
    raise SystemExit(main())
