"""Generates the deterministic FastMD test corpus: bench/corpus/{small,medium,large}.md + img/*.png."""
import pathlib
import random

ROOT = pathlib.Path(__file__).resolve().parents[1] / "corpus"

CODE = {
    "c": 'static int count_lines(const char *s) {\n    int n = 0;\n    for (; *s; s++)\n        if (*s == \'\\n\') n++;\n    return n; /* комментарий */\n}',
    "rust": 'fn main() {\n    let words: Vec<&str> = "быстрый рендер markdown".split(\' \').collect();\n    for (i, w) in words.iter().enumerate() {\n        println!("{i}: {w}");\n    }\n}',
    "csharp": 'public sealed class Viewer : IDisposable\n{\n    private readonly string _path;\n    public Viewer(string path) => _path = path ?? throw new ArgumentNullException(nameof(path));\n    public async Task<string> LoadAsync() => await File.ReadAllTextAsync(_path);\n    public void Dispose() { }\n}',
    "python": 'def render(doc: str) -> list[str]:\n    """Разбить документ на блоки."""\n    blocks = [b.strip() for b in doc.split("\\n\\n")]\n    return [b for b in blocks if b]\n\nprint(render("# Привет\\n\\nмир"))',
    "js": 'const md = await fetch("/README.md").then(r => r.text());\nconst html = marked.parse(md, { gfm: true });\ndocument.querySelector("#out").innerHTML = html; // 🚀',
    "json": '{\n  "name": "fastmd",\n  "version": "0.1.0",\n  "features": ["gfm", "tables", "tasklists"],\n  "startup_ms": 42\n}',
    "powershell": 'Get-ChildItem -Recurse -Filter *.md |\n    Where-Object Length -gt 1MB |\n    ForEach-Object { "{0,-40} {1,10:N0}" -f $_.Name, $_.Length }',
}

RU = [
    "Быстрый просмотрщик Markdown должен открываться мгновенно, без заставок и прогрева движка.",
    "Типографика важна не меньше скорости: ровные поля, аккуратные заголовки, читаемый моноширинный шрифт.",
    "Съешь же ещё этих мягких французских булок, да выпей чаю — классическая панграмма для проверки шрифтов.",
    "Длинные абзацы должны переноситься по словам, а не по символам, и не вылезать за правую границу колонки.",
]
EN = [
    "The quick brown fox jumps over the lazy dog, then renders a thousand paragraphs without breaking a sweat.",
    "Markdown is a lightweight markup language with plain-text formatting syntax designed for readability.",
    "A good viewer shows the first screen immediately and lays out the rest of the document lazily.",
    "Rendering speed is dominated by runtime initialization, not by parsing: parsers process hundreds of MB/s.",
]
MIXED = [
    "Mixed script line: English, русский, 日本語のテキスト, 中文字符, and emoji 🚀✨📄 ✅ ❌ in one paragraph.",
    "Inline styles: **bold**, *italic*, ***both***, ~~strikethrough~~, `inline code`, and a [link](https://commonmark.org).",
]


def para(rng: random.Random, n: int) -> str:
    pool = RU + EN + MIXED
    return " ".join(rng.choice(pool) for _ in range(n))


def table(rng: random.Random, rows: int) -> str:
    head = "| Stack | Language | Startup (ms) | Size (MB) | Notes |\n|:--|:--:|--:|--:|:--|"
    stacks = ["Win32 + Direct2D", "WinUI 3", "WPF", "WebView2", "Electron", "Tauri", "egui", "Avalonia"]
    langs = ["C++", "C#", "Rust", "JS"]
    body = []
    for i in range(rows):
        body.append(f"| {rng.choice(stacks)} | {rng.choice(langs)} | {rng.randint(8, 1500)} | {rng.randint(1, 250)} | "
                    f"{rng.choice(['быстро', 'медленно', '**ok**', '`n/a`', 'see [docs](https://example.com)'])} |")
    return head + "\n" + "\n".join(body)


def section(rng: random.Random, idx: int, img: bool) -> str:
    lang = list(CODE)[idx % len(CODE)]
    out = [
        f"## {idx}. Раздел {idx}: {rng.choice(['Архитектура', 'Rendering pipeline', 'Типографика', 'Startup budget', 'Таблицы и списки'])}",
        para(rng, 3),
        f"### {idx}.1 Подраздел со списками",
        "- Первый пункт с `inline code`\n- Second item with **bold** text\n  - Вложенный пункт\n  - Nested item with a [link](https://example.com/docs)\n    1. Deep ordered one\n    2. Deep ordered two\n- Третий пункт 🚀",
        "1. Шаг первый\n2. Step two with *emphasis*\n3. Шаг третий",
        "- [x] Parse markdown\n- [x] Layout first viewport\n- [ ] Syntax highlighting\n- [ ] Math (KaTeX)",
        f"### {idx}.2 Code ({lang})",
        f"```{lang}\n{CODE[lang]}\n```",
        "> Цитата: «Преждевременная оптимизация — корень всех зол», но время запуска — не преждевременная.\n>\n> — *Donald Knuth (почти)*",
        para(rng, 2),
        f"#### {idx}.3 Таблица",
        table(rng, 6),
    ]
    if img:
        out.append(f"![diagram {idx % 2}](img/diagram{idx % 2}.png)")
    out += [
        "##### Мелкий заголовок h5",
        para(rng, 1),
        "###### Самый мелкий заголовок h6",
        "---",
    ]
    return "\n\n".join(out)


def make_images() -> None:
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        print("pillow missing: skipping images")
        return
    (ROOT / "img").mkdir(parents=True, exist_ok=True)
    for i, (c1, c2) in enumerate([((9, 105, 218), (130, 80, 223)), ((26, 127, 55), (191, 135, 0))]):
        w, h = 640, 240
        im = Image.new("RGB", (w, h), "white")
        d = ImageDraw.Draw(im)
        for x in range(w):
            t = x / (w - 1)
            d.line([(x, 0), (x, h)], fill=tuple(int(a + (b - a) * t) for a, b in zip(c1, c2)))
        for k in range(6):
            d.rounded_rectangle([30 + k * 100, 70, 110 + k * 100, 170], radius=12, outline="white", width=4)
        d.text((30, 20), f"FastMD test image #{i}", fill="white")
        im.save(ROOT / "img" / f"diagram{i}.png", optimize=True)


def main() -> None:
    ROOT.mkdir(parents=True, exist_ok=True)
    make_images()
    rng = random.Random(42)

    small = "\n\n".join([
        "# FastMD — README",
        "Молниеносный просмотрщик **Markdown** для Windows. " + para(rng, 2),
        "## Возможности",
        "- Мгновенный запуск ⚡\n- GFM: таблицы, списки задач, ~~зачёркивание~~\n- Подсветка `кода`",
        "## Пример",
        "```rust\n" + CODE["rust"] + "\n```",
        table(rng, 4),
        "> Tip: see the [CommonMark spec](https://spec.commonmark.org).",
        section(rng, 1, img=True),
    ]) + "\n"

    medium_parts = ["# FastMD Test Document — средний размер", MIXED[0], MIXED[1]]
    for i in range(1, 25):
        medium_parts.append(section(rng, i, img=(i % 6 == 1)))
    medium = "\n\n".join(medium_parts) + "\n"

    large_parts = ["# FastMD Stress Document — большой файл", MIXED[0]]
    i = 1
    while sum(len(p) for p in large_parts) < 3_000_000:
        large_parts.append(section(rng, i, img=(i % 50 == 1)))
        i += 1
    large = "\n\n".join(large_parts) + "\n"

    for name, text in (("small", small), ("medium", medium), ("large", large)):
        p = ROOT / f"{name}.md"
        p.write_text(text, encoding="utf-8", newline="\n")
        print(f"{p}  {len(text.encode('utf-8')) / 1024:,.1f} KiB  {text.count(chr(10)):,} lines")


if __name__ == "__main__":
    main()
