# FastMD Test Document — средний размер

Mixed script line: English, русский, 日本語のテキスト, 中文字符, and emoji 🚀✨📄 ✅ ❌ in one paragraph.

Inline styles: **bold**, *italic*, ***both***, ~~strikethrough~~, `inline code`, and a [link](https://commonmark.org).

## 1. Раздел 1: Startup budget

The quick brown fox jumps over the lazy dog, then renders a thousand paragraphs without breaking a sweat. Rendering speed is dominated by runtime initialization, not by parsing: parsers process hundreds of MB/s. Markdown is a lightweight markup language with plain-text formatting syntax designed for readability.

### 1.1 Подраздел со списками

- Первый пункт с `inline code`
- Second item with **bold** text
  - Вложенный пункт
  - Nested item with a [link](https://example.com/docs)
    1. Deep ordered one
    2. Deep ordered two
- Третий пункт 🚀

1. Шаг первый
2. Step two with *emphasis*
3. Шаг третий

- [x] Parse markdown
- [x] Layout first viewport
- [ ] Syntax highlighting
- [ ] Math (KaTeX)

### 1.2 Code (rust)

```rust
fn main() {
    let words: Vec<&str> = "быстрый рендер markdown".split(' ').collect();
    for (i, w) in words.iter().enumerate() {
        println!("{i}: {w}");
    }
}
```

> Цитата: «Преждевременная оптимизация — корень всех зол», но время запуска — не преждевременная.
>
> — *Donald Knuth (почти)*

Съешь же ещё этих мягких французских булок, да выпей чаю — классическая панграмма для проверки шрифтов. Markdown is a lightweight markup language with plain-text formatting syntax designed for readability.

#### 1.3 Таблица

| Stack | Language | Startup (ms) | Size (MB) | Notes |
|:--|:--:|--:|--:|:--|
| Tauri | C# | 1380 | 69 | быстро |
| WPF | C# | 342 | 119 | `n/a` |
| Electron | C# | 1410 | 84 | быстро |
| WebView2 | C++ | 654 | 103 | **ok** |
| WinUI 3 | C# | 1169 | 225 | **ok** |
| WebView2 | JS | 818 | 227 | `n/a` |

![diagram 1](img/diagram1.png)

##### Мелкий заголовок h5

Съешь же ещё этих мягких французских булок, да выпей чаю — классическая панграмма для проверки шрифтов.

###### Самый мелкий заголовок h6

---

## 2. Раздел 2: Типографика

Съешь же ещё этих мягких французских булок, да выпей чаю — классическая панграмма для проверки шрифтов. Длинные абзацы должны переноситься по словам, а не по символам, и не вылезать за правую границу колонки. Mixed script line: English, русский, 日本語のテキスト, 中文字符, and emoji 🚀✨📄 ✅ ❌ in one paragraph.

### 2.1 Подраздел со списками

- Первый пункт с `inline code`
- Second item with **bold** text
  - Вложенный пункт
  - Nested item with a [link](https://example.com/docs)
    1. Deep ordered one
    2. Deep ordered two
- Третий пункт 🚀

1. Шаг первый
2. Step two with *emphasis*
3. Шаг третий

- [x] Parse markdown
- [x] Layout first viewport
- [ ] Syntax highlighting
- [ ] Math (KaTeX)

### 2.2 Code (csharp)

```csharp
public sealed class Viewer : IDisposable
{
    private readonly string _path;
    public Viewer(string path) => _path = path ?? throw new ArgumentNullException(nameof(path));
    public async Task<string> LoadAsync() => await File.ReadAllTextAsync(_path);
    public void Dispose() { }
}
```

> Цитата: «Преждевременная оптимизация — корень всех зол», но время запуска — не преждевременная.
>
> — *Donald Knuth (почти)*

Mixed script line: English, русский, 日本語のテキスト, 中文字符, and emoji 🚀✨📄 ✅ ❌ in one paragraph. The quick brown fox jumps over the lazy dog, then renders a thousand paragraphs without breaking a sweat.

#### 2.3 Таблица

| Stack | Language | Startup (ms) | Size (MB) | Notes |
|:--|:--:|--:|--:|:--|
| egui | JS | 749 | 57 | медленно |
| Avalonia | C++ | 104 | 221 | быстро |
| WPF | C# | 1401 | 109 | see [docs](https://example.com) |
| WinUI 3 | JS | 789 | 153 | `n/a` |
| Electron | C++ | 1401 | 185 | быстро |
| Electron | Rust | 236 | 76 | `n/a` |

##### Мелкий заголовок h5

Съешь же ещё этих мягких французских булок, да выпей чаю — классическая панграмма для проверки шрифтов.

###### Самый мелкий заголовок h6

---

## 3. Раздел 3: Startup budget

Быстрый просмотрщик Markdown должен открываться мгновенно, без заставок и прогрева движка. The quick brown fox jumps over the lazy dog, then renders a thousand paragraphs without breaking a sweat. Mixed script line: English, русский, 日本語のテキスト, 中文字符, and emoji 🚀✨📄 ✅ ❌ in one paragraph.

### 3.1 Подраздел со списками

- Первый пункт с `inline code`
- Second item with **bold** text
  - Вложенный пункт
  - Nested item with a [link](https://example.com/docs)
    1. Deep ordered one
    2. Deep ordered two
- Третий пункт 🚀

1. Шаг первый
2. Step two with *emphasis*
3. Шаг третий

- [x] Parse markdown
- [x] Layout first viewport
- [ ] Syntax highlighting
- [ ] Math (KaTeX)

### 3.2 Code (python)

```python
def render(doc: str) -> list[str]:
    """Разбить документ на блоки."""
    blocks = [b.strip() for b in doc.split("\n\n")]
    return [b for b in blocks if b]

print(render("# Привет\n\nмир"))
```

> Цитата: «Преждевременная оптимизация — корень всех зол», но время запуска — не преждевременная.
>
> — *Donald Knuth (почти)*

Съешь же ещё этих мягких французских булок, да выпей чаю — классическая панграмма для проверки шрифтов. Mixed script line: English, русский, 日本語のテキスト, 中文字符, and emoji 🚀✨📄 ✅ ❌ in one paragraph.

#### 3.3 Таблица

| Stack | Language | Startup (ms) | Size (MB) | Notes |
|:--|:--:|--:|--:|:--|
| WinUI 3 | Rust | 1316 | 130 | see [docs](https://example.com) |
| WebView2 | C# | 773 | 196 | медленно |
| Win32 + Direct2D | Rust | 1008 | 5 | быстро |
| Tauri | Rust | 498 | 15 | медленно |
| WinUI 3 | C++ | 1003 | 209 | быстро |
| WPF | C# | 1359 | 122 | see [docs](https://example.com) |

##### Мелкий заголовок h5

Съешь же ещё этих мягких французских булок, да выпей чаю — классическая панграмма для проверки шрифтов.

###### Самый мелкий заголовок h6

---

## 4. Раздел 4: Типографика

Mixed script line: English, русский, 日本語のテキスト, 中文字符, and emoji 🚀✨📄 ✅ ❌ in one paragraph. Inline styles: **bold**, *italic*, ***both***, ~~strikethrough~~, `inline code`, and a [link](https://commonmark.org). A good viewer shows the first screen immediately and lays out the rest of the document lazily.

### 4.1 Подраздел со списками

- Первый пункт с `inline code`
- Second item with **bold** text
  - Вложенный пункт
  - Nested item with a [link](https://example.com/docs)
    1. Deep ordered one
    2. Deep ordered two
- Третий пункт 🚀

1. Шаг первый
2. Step two with *emphasis*
3. Шаг третий

- [x] Parse markdown
- [x] Layout first viewport
- [ ] Syntax highlighting
- [ ] Math (KaTeX)

### 4.2 Code (js)

```js
const md = await fetch("/README.md").then(r => r.text());
const html = marked.parse(md, { gfm: true });
document.querySelector("#out").innerHTML = html; // 🚀
```

> Цитата: «Преждевременная оптимизация — корень всех зол», но время запуска — не преждевременная.
>
> — *Donald Knuth (почти)*

Длинные абзацы должны переноситься по словам, а не по символам, и не вылезать за правую границу колонки. Mixed script line: English, русский, 日本語のテキスト, 中文字符, and emoji 🚀✨📄 ✅ ❌ in one paragraph.

#### 4.3 Таблица

| Stack | Language | Startup (ms) | Size (MB) | Notes |
|:--|:--:|--:|--:|:--|
| WebView2 | Rust | 825 | 172 | **ok** |
| Avalonia | JS | 255 | 64 | медленно |
| WinUI 3 | Rust | 51 | 151 | see [docs](https://example.com) |
| WebView2 | C# | 22 | 19 | быстро |
| WebView2 | C++ | 72 | 221 | **ok** |
| WinUI 3 | C# | 578 | 172 | `n/a` |

##### Мелкий заголовок h5

Длинные абзацы должны переноситься по словам, а не по символам, и не вылезать за правую границу колонки.

###### Самый мелкий заголовок h6

---

## 5. Раздел 5: Таблицы и списки

Съешь же ещё этих мягких французских булок, да выпей чаю — классическая панграмма для проверки шрифтов. Inline styles: **bold**, *italic*, ***both***, ~~strikethrough~~, `inline code`, and a [link](https://commonmark.org). Inline styles: **bold**, *italic*, ***both***, ~~strikethrough~~, `inline code`, and a [link](https://commonmark.org).

### 5.1 Подраздел со списками

- Первый пункт с `inline code`
- Second item with **bold** text
  - Вложенный пункт
  - Nested item with a [link](https://example.com/docs)
    1. Deep ordered one
    2. Deep ordered two
- Третий пункт 🚀

1. Шаг первый
2. Step two with *emphasis*
3. Шаг третий

- [x] Parse markdown
- [x] Layout first viewport
- [ ] Syntax highlighting
- [ ] Math (KaTeX)

### 5.2 Code (json)

```json
{
  "name": "fastmd",
  "version": "0.1.0",
  "features": ["gfm", "tables", "tasklists"],
  "startup_ms": 42
}
```

> Цитата: «Преждевременная оптимизация — корень всех зол», но время запуска — не преждевременная.
>
> — *Donald Knuth (почти)*

Rendering speed is dominated by runtime initialization, not by parsing: parsers process hundreds of MB/s. Длинные абзацы должны переноситься по словам, а не по символам, и не вылезать за правую границу колонки.

#### 5.3 Таблица

| Stack | Language | Startup (ms) | Size (MB) | Notes |
|:--|:--:|--:|--:|:--|
| Avalonia | JS | 397 | 25 | быстро |
| egui | Rust | 875 | 106 | `n/a` |
| Win32 + Direct2D | C++ | 132 | 104 | **ok** |
| WinUI 3 | C# | 400 | 49 | see [docs](https://example.com) |
| Avalonia | C# | 872 | 47 | **ok** |
| Avalonia | C# | 162 | 114 | see [docs](https://example.com) |

##### Мелкий заголовок h5

Типографика важна не меньше скорости: ровные поля, аккуратные заголовки, читаемый моноширинный шрифт.

###### Самый мелкий заголовок h6

---

## 6. Раздел 6: Архитектура

Mixed script line: English, русский, 日本語のテキスト, 中文字符, and emoji 🚀✨📄 ✅ ❌ in one paragraph. Быстрый просмотрщик Markdown должен открываться мгновенно, без заставок и прогрева движка. Типографика важна не меньше скорости: ровные поля, аккуратные заголовки, читаемый моноширинный шрифт.

### 6.1 Подраздел со списками

- Первый пункт с `inline code`
- Second item with **bold** text
  - Вложенный пункт
  - Nested item with a [link](https://example.com/docs)
    1. Deep ordered one
    2. Deep ordered two
- Третий пункт 🚀

1. Шаг первый
2. Step two with *emphasis*
3. Шаг третий

- [x] Parse markdown
- [x] Layout first viewport
- [ ] Syntax highlighting
- [ ] Math (KaTeX)

### 6.2 Code (powershell)

```powershell
Get-ChildItem -Recurse -Filter *.md |
    Where-Object Length -gt 1MB |
    ForEach-Object { "{0,-40} {1,10:N0}" -f $_.Name, $_.Length }
```

> Цитата: «Преждевременная оптимизация — корень всех зол», но время запуска — не преждевременная.
>
> — *Donald Knuth (почти)*

Длинные абзацы должны переноситься по словам, а не по символам, и не вылезать за правую границу колонки. Съешь же ещё этих мягких французских булок, да выпей чаю — классическая панграмма для проверки шрифтов.

#### 6.3 Таблица

| Stack | Language | Startup (ms) | Size (MB) | Notes |
|:--|:--:|--:|--:|:--|
| egui | JS | 993 | 55 | `n/a` |
| Win32 + Direct2D | C# | 784 | 1 | `n/a` |
| Electron | JS | 592 | 109 | see [docs](https://example.com) |
| Avalonia | C# | 396 | 76 | медленно |
| Win32 + Direct2D | C++ | 650 | 15 | быстро |
| Avalonia | C# | 124 | 246 | see [docs](https://example.com) |

##### Мелкий заголовок h5

Типографика важна не меньше скорости: ровные поля, аккуратные заголовки, читаемый моноширинный шрифт.

###### Самый мелкий заголовок h6

---

## 7. Раздел 7: Rendering pipeline

Типографика важна не меньше скорости: ровные поля, аккуратные заголовки, читаемый моноширинный шрифт. Inline styles: **bold**, *italic*, ***both***, ~~strikethrough~~, `inline code`, and a [link](https://commonmark.org). Типографика важна не меньше скорости: ровные поля, аккуратные заголовки, читаемый моноширинный шрифт.

### 7.1 Подраздел со списками

- Первый пункт с `inline code`
- Second item with **bold** text
  - Вложенный пункт
  - Nested item with a [link](https://example.com/docs)
    1. Deep ordered one
    2. Deep ordered two
- Третий пункт 🚀

1. Шаг первый
2. Step two with *emphasis*
3. Шаг третий

- [x] Parse markdown
- [x] Layout first viewport
- [ ] Syntax highlighting
- [ ] Math (KaTeX)

### 7.2 Code (c)

```c
static int count_lines(const char *s) {
    int n = 0;
    for (; *s; s++)
        if (*s == '\n') n++;
    return n; /* комментарий */
}
```

> Цитата: «Преждевременная оптимизация — корень всех зол», но время запуска — не преждевременная.
>
> — *Donald Knuth (почти)*

Длинные абзацы должны переноситься по словам, а не по символам, и не вылезать за правую границу колонки. A good viewer shows the first screen immediately and lays out the rest of the document lazily.

#### 7.3 Таблица

| Stack | Language | Startup (ms) | Size (MB) | Notes |
|:--|:--:|--:|--:|:--|
| WinUI 3 | C# | 1193 | 153 | быстро |
| WinUI 3 | JS | 1354 | 150 | see [docs](https://example.com) |
| Tauri | Rust | 426 | 172 | **ok** |
| WebView2 | Rust | 818 | 34 | **ok** |
| Avalonia | Rust | 156 | 3 | `n/a` |
| WinUI 3 | C++ | 1109 | 55 | see [docs](https://example.com) |

![diagram 1](img/diagram1.png)

##### Мелкий заголовок h5

The quick brown fox jumps over the lazy dog, then renders a thousand paragraphs without breaking a sweat.

###### Самый мелкий заголовок h6

---

## 8. Раздел 8: Rendering pipeline

Markdown is a lightweight markup language with plain-text formatting syntax designed for readability. Типографика важна не меньше скорости: ровные поля, аккуратные заголовки, читаемый моноширинный шрифт. Длинные абзацы должны переноситься по словам, а не по символам, и не вылезать за правую границу колонки.

### 8.1 Подраздел со списками

- Первый пункт с `inline code`
- Second item with **bold** text
  - Вложенный пункт
  - Nested item with a [link](https://example.com/docs)
    1. Deep ordered one
    2. Deep ordered two
- Третий пункт 🚀

1. Шаг первый
2. Step two with *emphasis*
3. Шаг третий

- [x] Parse markdown
- [x] Layout first viewport
- [ ] Syntax highlighting
- [ ] Math (KaTeX)

### 8.2 Code (rust)

```rust
fn main() {
    let words: Vec<&str> = "быстрый рендер markdown".split(' ').collect();
    for (i, w) in words.iter().enumerate() {
        println!("{i}: {w}");
    }
}
```

> Цитата: «Преждевременная оптимизация — корень всех зол», но время запуска — не преждевременная.
>
> — *Donald Knuth (почти)*

Markdown is a lightweight markup language with plain-text formatting syntax designed for readability. The quick brown fox jumps over the lazy dog, then renders a thousand paragraphs without breaking a sweat.

#### 8.3 Таблица

| Stack | Language | Startup (ms) | Size (MB) | Notes |
|:--|:--:|--:|--:|:--|
| WPF | JS | 1120 | 181 | **ok** |
| Win32 + Direct2D | Rust | 1366 | 27 | медленно |
| Electron | C++ | 227 | 191 | see [docs](https://example.com) |
| WPF | Rust | 585 | 155 | медленно |
| Tauri | C# | 1415 | 163 | **ok** |
| Avalonia | Rust | 112 | 24 | `n/a` |

##### Мелкий заголовок h5

The quick brown fox jumps over the lazy dog, then renders a thousand paragraphs without breaking a sweat.

###### Самый мелкий заголовок h6

---

## 9. Раздел 9: Архитектура

Быстрый просмотрщик Markdown должен открываться мгновенно, без заставок и прогрева движка. Markdown is a lightweight markup language with plain-text formatting syntax designed for readability. Съешь же ещё этих мягких французских булок, да выпей чаю — классическая панграмма для проверки шрифтов.

### 9.1 Подраздел со списками

- Первый пункт с `inline code`
- Second item with **bold** text
  - Вложенный пункт
  - Nested item with a [link](https://example.com/docs)
    1. Deep ordered one
    2. Deep ordered two
- Третий пункт 🚀

1. Шаг первый
2. Step two with *emphasis*
3. Шаг третий

- [x] Parse markdown
- [x] Layout first viewport
- [ ] Syntax highlighting
- [ ] Math (KaTeX)

### 9.2 Code (csharp)

```csharp
public sealed class Viewer : IDisposable
{
    private readonly string _path;
    public Viewer(string path) => _path = path ?? throw new ArgumentNullException(nameof(path));
    public async Task<string> LoadAsync() => await File.ReadAllTextAsync(_path);
    public void Dispose() { }
}
```

> Цитата: «Преждевременная оптимизация — корень всех зол», но время запуска — не преждевременная.
>
> — *Donald Knuth (почти)*

The quick brown fox jumps over the lazy dog, then renders a thousand paragraphs without breaking a sweat. Съешь же ещё этих мягких французских булок, да выпей чаю — классическая панграмма для проверки шрифтов.

#### 9.3 Таблица

| Stack | Language | Startup (ms) | Size (MB) | Notes |
|:--|:--:|--:|--:|:--|
| Avalonia | JS | 1156 | 3 | быстро |
| WinUI 3 | C# | 1125 | 10 | **ok** |
| WPF | JS | 269 | 11 | **ok** |
| Tauri | C++ | 740 | 54 | медленно |
| WinUI 3 | Rust | 1154 | 227 | `n/a` |
| WPF | C# | 340 | 250 | медленно |

##### Мелкий заголовок h5

A good viewer shows the first screen immediately and lays out the rest of the document lazily.

###### Самый мелкий заголовок h6

---

## 10. Раздел 10: Архитектура

Съешь же ещё этих мягких французских булок, да выпей чаю — классическая панграмма для проверки шрифтов. Markdown is a lightweight markup language with plain-text formatting syntax designed for readability. A good viewer shows the first screen immediately and lays out the rest of the document lazily.

### 10.1 Подраздел со списками

- Первый пункт с `inline code`
- Second item with **bold** text
  - Вложенный пункт
  - Nested item with a [link](https://example.com/docs)
    1. Deep ordered one
    2. Deep ordered two
- Третий пункт 🚀

1. Шаг первый
2. Step two with *emphasis*
3. Шаг третий

- [x] Parse markdown
- [x] Layout first viewport
- [ ] Syntax highlighting
- [ ] Math (KaTeX)

### 10.2 Code (python)

```python
def render(doc: str) -> list[str]:
    """Разбить документ на блоки."""
    blocks = [b.strip() for b in doc.split("\n\n")]
    return [b for b in blocks if b]

print(render("# Привет\n\nмир"))
```

> Цитата: «Преждевременная оптимизация — корень всех зол», но время запуска — не преждевременная.
>
> — *Donald Knuth (почти)*

Длинные абзацы должны переноситься по словам, а не по символам, и не вылезать за правую границу колонки. The quick brown fox jumps over the lazy dog, then renders a thousand paragraphs without breaking a sweat.

#### 10.3 Таблица

| Stack | Language | Startup (ms) | Size (MB) | Notes |
|:--|:--:|--:|--:|:--|
| WPF | C++ | 791 | 224 | быстро |
| Avalonia | C# | 416 | 210 | `n/a` |
| Tauri | Rust | 474 | 58 | быстро |
| WebView2 | JS | 680 | 72 | быстро |
| Electron | Rust | 1321 | 131 | `n/a` |
| Tauri | C++ | 244 | 225 | **ok** |

##### Мелкий заголовок h5

Съешь же ещё этих мягких французских булок, да выпей чаю — классическая панграмма для проверки шрифтов.

###### Самый мелкий заголовок h6

---

## 11. Раздел 11: Таблицы и списки

The quick brown fox jumps over the lazy dog, then renders a thousand paragraphs without breaking a sweat. Быстрый просмотрщик Markdown должен открываться мгновенно, без заставок и прогрева движка. Типографика важна не меньше скорости: ровные поля, аккуратные заголовки, читаемый моноширинный шрифт.

### 11.1 Подраздел со списками

- Первый пункт с `inline code`
- Second item with **bold** text
  - Вложенный пункт
  - Nested item with a [link](https://example.com/docs)
    1. Deep ordered one
    2. Deep ordered two
- Третий пункт 🚀

1. Шаг первый
2. Step two with *emphasis*
3. Шаг третий

- [x] Parse markdown
- [x] Layout first viewport
- [ ] Syntax highlighting
- [ ] Math (KaTeX)

### 11.2 Code (js)

```js
const md = await fetch("/README.md").then(r => r.text());
const html = marked.parse(md, { gfm: true });
document.querySelector("#out").innerHTML = html; // 🚀
```

> Цитата: «Преждевременная оптимизация — корень всех зол», но время запуска — не преждевременная.
>
> — *Donald Knuth (почти)*

Inline styles: **bold**, *italic*, ***both***, ~~strikethrough~~, `inline code`, and a [link](https://commonmark.org). A good viewer shows the first screen immediately and lays out the rest of the document lazily.

#### 11.3 Таблица

| Stack | Language | Startup (ms) | Size (MB) | Notes |
|:--|:--:|--:|--:|:--|
| Tauri | Rust | 901 | 156 | see [docs](https://example.com) |
| WinUI 3 | JS | 1188 | 49 | **ok** |
| Win32 + Direct2D | JS | 11 | 134 | see [docs](https://example.com) |
| WebView2 | Rust | 891 | 18 | **ok** |
| Tauri | C++ | 1482 | 231 | **ok** |
| Electron | JS | 676 | 104 | **ok** |

##### Мелкий заголовок h5

Mixed script line: English, русский, 日本語のテキスト, 中文字符, and emoji 🚀✨📄 ✅ ❌ in one paragraph.

###### Самый мелкий заголовок h6

---

## 12. Раздел 12: Rendering pipeline

Длинные абзацы должны переноситься по словам, а не по символам, и не вылезать за правую границу колонки. A good viewer shows the first screen immediately and lays out the rest of the document lazily. A good viewer shows the first screen immediately and lays out the rest of the document lazily.

### 12.1 Подраздел со списками

- Первый пункт с `inline code`
- Second item with **bold** text
  - Вложенный пункт
  - Nested item with a [link](https://example.com/docs)
    1. Deep ordered one
    2. Deep ordered two
- Третий пункт 🚀

1. Шаг первый
2. Step two with *emphasis*
3. Шаг третий

- [x] Parse markdown
- [x] Layout first viewport
- [ ] Syntax highlighting
- [ ] Math (KaTeX)

### 12.2 Code (json)

```json
{
  "name": "fastmd",
  "version": "0.1.0",
  "features": ["gfm", "tables", "tasklists"],
  "startup_ms": 42
}
```

> Цитата: «Преждевременная оптимизация — корень всех зол», но время запуска — не преждевременная.
>
> — *Donald Knuth (почти)*

Съешь же ещё этих мягких французских булок, да выпей чаю — классическая панграмма для проверки шрифтов. Inline styles: **bold**, *italic*, ***both***, ~~strikethrough~~, `inline code`, and a [link](https://commonmark.org).

#### 12.3 Таблица

| Stack | Language | Startup (ms) | Size (MB) | Notes |
|:--|:--:|--:|--:|:--|
| Electron | JS | 1130 | 214 | быстро |
| Electron | Rust | 438 | 111 | see [docs](https://example.com) |
| Tauri | JS | 912 | 114 | медленно |
| Avalonia | C# | 1357 | 22 | **ok** |
| Tauri | C++ | 489 | 173 | **ok** |
| WebView2 | C# | 309 | 7 | быстро |

##### Мелкий заголовок h5

Длинные абзацы должны переноситься по словам, а не по символам, и не вылезать за правую границу колонки.

###### Самый мелкий заголовок h6

---

## 13. Раздел 13: Startup budget

Inline styles: **bold**, *italic*, ***both***, ~~strikethrough~~, `inline code`, and a [link](https://commonmark.org). Типографика важна не меньше скорости: ровные поля, аккуратные заголовки, читаемый моноширинный шрифт. Rendering speed is dominated by runtime initialization, not by parsing: parsers process hundreds of MB/s.

### 13.1 Подраздел со списками

- Первый пункт с `inline code`
- Second item with **bold** text
  - Вложенный пункт
  - Nested item with a [link](https://example.com/docs)
    1. Deep ordered one
    2. Deep ordered two
- Третий пункт 🚀

1. Шаг первый
2. Step two with *emphasis*
3. Шаг третий

- [x] Parse markdown
- [x] Layout first viewport
- [ ] Syntax highlighting
- [ ] Math (KaTeX)

### 13.2 Code (powershell)

```powershell
Get-ChildItem -Recurse -Filter *.md |
    Where-Object Length -gt 1MB |
    ForEach-Object { "{0,-40} {1,10:N0}" -f $_.Name, $_.Length }
```

> Цитата: «Преждевременная оптимизация — корень всех зол», но время запуска — не преждевременная.
>
> — *Donald Knuth (почти)*

A good viewer shows the first screen immediately and lays out the rest of the document lazily. Inline styles: **bold**, *italic*, ***both***, ~~strikethrough~~, `inline code`, and a [link](https://commonmark.org).

#### 13.3 Таблица

| Stack | Language | Startup (ms) | Size (MB) | Notes |
|:--|:--:|--:|--:|:--|
| WebView2 | JS | 1020 | 103 | медленно |
| WPF | C++ | 226 | 200 | `n/a` |
| WebView2 | C# | 1433 | 133 | `n/a` |
| Win32 + Direct2D | C# | 256 | 117 | медленно |
| Avalonia | Rust | 914 | 157 | see [docs](https://example.com) |
| egui | JS | 333 | 191 | `n/a` |

![diagram 1](img/diagram1.png)

##### Мелкий заголовок h5

Rendering speed is dominated by runtime initialization, not by parsing: parsers process hundreds of MB/s.

###### Самый мелкий заголовок h6

---

## 14. Раздел 14: Типографика

Длинные абзацы должны переноситься по словам, а не по символам, и не вылезать за правую границу колонки. The quick brown fox jumps over the lazy dog, then renders a thousand paragraphs without breaking a sweat. Mixed script line: English, русский, 日本語のテキスト, 中文字符, and emoji 🚀✨📄 ✅ ❌ in one paragraph.

### 14.1 Подраздел со списками

- Первый пункт с `inline code`
- Second item with **bold** text
  - Вложенный пункт
  - Nested item with a [link](https://example.com/docs)
    1. Deep ordered one
    2. Deep ordered two
- Третий пункт 🚀

1. Шаг первый
2. Step two with *emphasis*
3. Шаг третий

- [x] Parse markdown
- [x] Layout first viewport
- [ ] Syntax highlighting
- [ ] Math (KaTeX)

### 14.2 Code (c)

```c
static int count_lines(const char *s) {
    int n = 0;
    for (; *s; s++)
        if (*s == '\n') n++;
    return n; /* комментарий */
}
```

> Цитата: «Преждевременная оптимизация — корень всех зол», но время запуска — не преждевременная.
>
> — *Donald Knuth (почти)*

Rendering speed is dominated by runtime initialization, not by parsing: parsers process hundreds of MB/s. Длинные абзацы должны переноситься по словам, а не по символам, и не вылезать за правую границу колонки.

#### 14.3 Таблица

| Stack | Language | Startup (ms) | Size (MB) | Notes |
|:--|:--:|--:|--:|:--|
| Electron | JS | 166 | 183 | **ok** |
| WebView2 | Rust | 695 | 82 | see [docs](https://example.com) |
| WinUI 3 | C# | 316 | 60 | `n/a` |
| WPF | C# | 139 | 107 | `n/a` |
| Tauri | JS | 859 | 16 | медленно |
| egui | JS | 1204 | 243 | быстро |

##### Мелкий заголовок h5

Inline styles: **bold**, *italic*, ***both***, ~~strikethrough~~, `inline code`, and a [link](https://commonmark.org).

###### Самый мелкий заголовок h6

---

## 15. Раздел 15: Startup budget

Rendering speed is dominated by runtime initialization, not by parsing: parsers process hundreds of MB/s. Быстрый просмотрщик Markdown должен открываться мгновенно, без заставок и прогрева движка. Markdown is a lightweight markup language with plain-text formatting syntax designed for readability.

### 15.1 Подраздел со списками

- Первый пункт с `inline code`
- Second item with **bold** text
  - Вложенный пункт
  - Nested item with a [link](https://example.com/docs)
    1. Deep ordered one
    2. Deep ordered two
- Третий пункт 🚀

1. Шаг первый
2. Step two with *emphasis*
3. Шаг третий

- [x] Parse markdown
- [x] Layout first viewport
- [ ] Syntax highlighting
- [ ] Math (KaTeX)

### 15.2 Code (rust)

```rust
fn main() {
    let words: Vec<&str> = "быстрый рендер markdown".split(' ').collect();
    for (i, w) in words.iter().enumerate() {
        println!("{i}: {w}");
    }
}
```

> Цитата: «Преждевременная оптимизация — корень всех зол», но время запуска — не преждевременная.
>
> — *Donald Knuth (почти)*

The quick brown fox jumps over the lazy dog, then renders a thousand paragraphs without breaking a sweat. A good viewer shows the first screen immediately and lays out the rest of the document lazily.

#### 15.3 Таблица

| Stack | Language | Startup (ms) | Size (MB) | Notes |
|:--|:--:|--:|--:|:--|
| egui | C# | 1007 | 57 | **ok** |
| egui | JS | 67 | 100 | **ok** |
| egui | C# | 965 | 236 | медленно |
| Win32 + Direct2D | JS | 1220 | 145 | быстро |
| WinUI 3 | JS | 285 | 222 | `n/a` |
| WPF | C++ | 540 | 98 | **ok** |

##### Мелкий заголовок h5

Длинные абзацы должны переноситься по словам, а не по символам, и не вылезать за правую границу колонки.

###### Самый мелкий заголовок h6

---

## 16. Раздел 16: Startup budget

Markdown is a lightweight markup language with plain-text formatting syntax designed for readability. Markdown is a lightweight markup language with plain-text formatting syntax designed for readability. A good viewer shows the first screen immediately and lays out the rest of the document lazily.

### 16.1 Подраздел со списками

- Первый пункт с `inline code`
- Second item with **bold** text
  - Вложенный пункт
  - Nested item with a [link](https://example.com/docs)
    1. Deep ordered one
    2. Deep ordered two
- Третий пункт 🚀

1. Шаг первый
2. Step two with *emphasis*
3. Шаг третий

- [x] Parse markdown
- [x] Layout first viewport
- [ ] Syntax highlighting
- [ ] Math (KaTeX)

### 16.2 Code (csharp)

```csharp
public sealed class Viewer : IDisposable
{
    private readonly string _path;
    public Viewer(string path) => _path = path ?? throw new ArgumentNullException(nameof(path));
    public async Task<string> LoadAsync() => await File.ReadAllTextAsync(_path);
    public void Dispose() { }
}
```

> Цитата: «Преждевременная оптимизация — корень всех зол», но время запуска — не преждевременная.
>
> — *Donald Knuth (почти)*

The quick brown fox jumps over the lazy dog, then renders a thousand paragraphs without breaking a sweat. A good viewer shows the first screen immediately and lays out the rest of the document lazily.

#### 16.3 Таблица

| Stack | Language | Startup (ms) | Size (MB) | Notes |
|:--|:--:|--:|--:|:--|
| Electron | C++ | 971 | 5 | see [docs](https://example.com) |
| Win32 + Direct2D | Rust | 467 | 167 | быстро |
| Win32 + Direct2D | C++ | 514 | 52 | быстро |
| WPF | C# | 266 | 122 | быстро |
| WebView2 | JS | 1440 | 66 | **ok** |
| WPF | C++ | 343 | 247 | **ok** |

##### Мелкий заголовок h5

Типографика важна не меньше скорости: ровные поля, аккуратные заголовки, читаемый моноширинный шрифт.

###### Самый мелкий заголовок h6

---

## 17. Раздел 17: Таблицы и списки

Быстрый просмотрщик Markdown должен открываться мгновенно, без заставок и прогрева движка. The quick brown fox jumps over the lazy dog, then renders a thousand paragraphs without breaking a sweat. Inline styles: **bold**, *italic*, ***both***, ~~strikethrough~~, `inline code`, and a [link](https://commonmark.org).

### 17.1 Подраздел со списками

- Первый пункт с `inline code`
- Second item with **bold** text
  - Вложенный пункт
  - Nested item with a [link](https://example.com/docs)
    1. Deep ordered one
    2. Deep ordered two
- Третий пункт 🚀

1. Шаг первый
2. Step two with *emphasis*
3. Шаг третий

- [x] Parse markdown
- [x] Layout first viewport
- [ ] Syntax highlighting
- [ ] Math (KaTeX)

### 17.2 Code (python)

```python
def render(doc: str) -> list[str]:
    """Разбить документ на блоки."""
    blocks = [b.strip() for b in doc.split("\n\n")]
    return [b for b in blocks if b]

print(render("# Привет\n\nмир"))
```

> Цитата: «Преждевременная оптимизация — корень всех зол», но время запуска — не преждевременная.
>
> — *Donald Knuth (почти)*

A good viewer shows the first screen immediately and lays out the rest of the document lazily. A good viewer shows the first screen immediately and lays out the rest of the document lazily.

#### 17.3 Таблица

| Stack | Language | Startup (ms) | Size (MB) | Notes |
|:--|:--:|--:|--:|:--|
| WebView2 | C++ | 1220 | 177 | медленно |
| WinUI 3 | Rust | 1409 | 154 | быстро |
| Win32 + Direct2D | Rust | 1099 | 110 | **ok** |
| WinUI 3 | Rust | 33 | 218 | `n/a` |
| Avalonia | C++ | 895 | 246 | **ok** |
| Avalonia | C# | 899 | 46 | see [docs](https://example.com) |

##### Мелкий заголовок h5

The quick brown fox jumps over the lazy dog, then renders a thousand paragraphs without breaking a sweat.

###### Самый мелкий заголовок h6

---

## 18. Раздел 18: Таблицы и списки

Mixed script line: English, русский, 日本語のテキスト, 中文字符, and emoji 🚀✨📄 ✅ ❌ in one paragraph. Rendering speed is dominated by runtime initialization, not by parsing: parsers process hundreds of MB/s. Rendering speed is dominated by runtime initialization, not by parsing: parsers process hundreds of MB/s.

### 18.1 Подраздел со списками

- Первый пункт с `inline code`
- Second item with **bold** text
  - Вложенный пункт
  - Nested item with a [link](https://example.com/docs)
    1. Deep ordered one
    2. Deep ordered two
- Третий пункт 🚀

1. Шаг первый
2. Step two with *emphasis*
3. Шаг третий

- [x] Parse markdown
- [x] Layout first viewport
- [ ] Syntax highlighting
- [ ] Math (KaTeX)

### 18.2 Code (js)

```js
const md = await fetch("/README.md").then(r => r.text());
const html = marked.parse(md, { gfm: true });
document.querySelector("#out").innerHTML = html; // 🚀
```

> Цитата: «Преждевременная оптимизация — корень всех зол», но время запуска — не преждевременная.
>
> — *Donald Knuth (почти)*

A good viewer shows the first screen immediately and lays out the rest of the document lazily. Inline styles: **bold**, *italic*, ***both***, ~~strikethrough~~, `inline code`, and a [link](https://commonmark.org).

#### 18.3 Таблица

| Stack | Language | Startup (ms) | Size (MB) | Notes |
|:--|:--:|--:|--:|:--|
| Electron | Rust | 510 | 213 | быстро |
| Electron | JS | 507 | 193 | `n/a` |
| egui | Rust | 66 | 127 | **ok** |
| WPF | JS | 442 | 91 | **ok** |
| Tauri | Rust | 1228 | 180 | **ok** |
| Win32 + Direct2D | C# | 183 | 62 | `n/a` |

##### Мелкий заголовок h5

Rendering speed is dominated by runtime initialization, not by parsing: parsers process hundreds of MB/s.

###### Самый мелкий заголовок h6

---

## 19. Раздел 19: Таблицы и списки

Длинные абзацы должны переноситься по словам, а не по символам, и не вылезать за правую границу колонки. Rendering speed is dominated by runtime initialization, not by parsing: parsers process hundreds of MB/s. Rendering speed is dominated by runtime initialization, not by parsing: parsers process hundreds of MB/s.

### 19.1 Подраздел со списками

- Первый пункт с `inline code`
- Second item with **bold** text
  - Вложенный пункт
  - Nested item with a [link](https://example.com/docs)
    1. Deep ordered one
    2. Deep ordered two
- Третий пункт 🚀

1. Шаг первый
2. Step two with *emphasis*
3. Шаг третий

- [x] Parse markdown
- [x] Layout first viewport
- [ ] Syntax highlighting
- [ ] Math (KaTeX)

### 19.2 Code (json)

```json
{
  "name": "fastmd",
  "version": "0.1.0",
  "features": ["gfm", "tables", "tasklists"],
  "startup_ms": 42
}
```

> Цитата: «Преждевременная оптимизация — корень всех зол», но время запуска — не преждевременная.
>
> — *Donald Knuth (почти)*

Rendering speed is dominated by runtime initialization, not by parsing: parsers process hundreds of MB/s. Быстрый просмотрщик Markdown должен открываться мгновенно, без заставок и прогрева движка.

#### 19.3 Таблица

| Stack | Language | Startup (ms) | Size (MB) | Notes |
|:--|:--:|--:|--:|:--|
| WinUI 3 | Rust | 461 | 104 | медленно |
| Electron | Rust | 977 | 142 | see [docs](https://example.com) |
| Tauri | JS | 1135 | 85 | **ok** |
| Avalonia | Rust | 635 | 65 | медленно |
| WinUI 3 | C# | 654 | 31 | see [docs](https://example.com) |
| WPF | C# | 451 | 190 | `n/a` |

![diagram 1](img/diagram1.png)

##### Мелкий заголовок h5

The quick brown fox jumps over the lazy dog, then renders a thousand paragraphs without breaking a sweat.

###### Самый мелкий заголовок h6

---

## 20. Раздел 20: Таблицы и списки

Mixed script line: English, русский, 日本語のテキスト, 中文字符, and emoji 🚀✨📄 ✅ ❌ in one paragraph. Inline styles: **bold**, *italic*, ***both***, ~~strikethrough~~, `inline code`, and a [link](https://commonmark.org). The quick brown fox jumps over the lazy dog, then renders a thousand paragraphs without breaking a sweat.

### 20.1 Подраздел со списками

- Первый пункт с `inline code`
- Second item with **bold** text
  - Вложенный пункт
  - Nested item with a [link](https://example.com/docs)
    1. Deep ordered one
    2. Deep ordered two
- Третий пункт 🚀

1. Шаг первый
2. Step two with *emphasis*
3. Шаг третий

- [x] Parse markdown
- [x] Layout first viewport
- [ ] Syntax highlighting
- [ ] Math (KaTeX)

### 20.2 Code (powershell)

```powershell
Get-ChildItem -Recurse -Filter *.md |
    Where-Object Length -gt 1MB |
    ForEach-Object { "{0,-40} {1,10:N0}" -f $_.Name, $_.Length }
```

> Цитата: «Преждевременная оптимизация — корень всех зол», но время запуска — не преждевременная.
>
> — *Donald Knuth (почти)*

Типографика важна не меньше скорости: ровные поля, аккуратные заголовки, читаемый моноширинный шрифт. Длинные абзацы должны переноситься по словам, а не по символам, и не вылезать за правую границу колонки.

#### 20.3 Таблица

| Stack | Language | Startup (ms) | Size (MB) | Notes |
|:--|:--:|--:|--:|:--|
| Electron | C# | 747 | 46 | **ok** |
| Win32 + Direct2D | C# | 569 | 12 | быстро |
| Electron | C# | 1314 | 223 | `n/a` |
| WinUI 3 | C++ | 1183 | 73 | `n/a` |
| Avalonia | JS | 705 | 48 | быстро |
| Electron | JS | 241 | 211 | быстро |

##### Мелкий заголовок h5

A good viewer shows the first screen immediately and lays out the rest of the document lazily.

###### Самый мелкий заголовок h6

---

## 21. Раздел 21: Startup budget

Типографика важна не меньше скорости: ровные поля, аккуратные заголовки, читаемый моноширинный шрифт. Inline styles: **bold**, *italic*, ***both***, ~~strikethrough~~, `inline code`, and a [link](https://commonmark.org). Быстрый просмотрщик Markdown должен открываться мгновенно, без заставок и прогрева движка.

### 21.1 Подраздел со списками

- Первый пункт с `inline code`
- Second item with **bold** text
  - Вложенный пункт
  - Nested item with a [link](https://example.com/docs)
    1. Deep ordered one
    2. Deep ordered two
- Третий пункт 🚀

1. Шаг первый
2. Step two with *emphasis*
3. Шаг третий

- [x] Parse markdown
- [x] Layout first viewport
- [ ] Syntax highlighting
- [ ] Math (KaTeX)

### 21.2 Code (c)

```c
static int count_lines(const char *s) {
    int n = 0;
    for (; *s; s++)
        if (*s == '\n') n++;
    return n; /* комментарий */
}
```

> Цитата: «Преждевременная оптимизация — корень всех зол», но время запуска — не преждевременная.
>
> — *Donald Knuth (почти)*

Съешь же ещё этих мягких французских булок, да выпей чаю — классическая панграмма для проверки шрифтов. Съешь же ещё этих мягких французских булок, да выпей чаю — классическая панграмма для проверки шрифтов.

#### 21.3 Таблица

| Stack | Language | Startup (ms) | Size (MB) | Notes |
|:--|:--:|--:|--:|:--|
| Electron | C++ | 516 | 31 | see [docs](https://example.com) |
| egui | C# | 1078 | 98 | `n/a` |
| Avalonia | Rust | 1213 | 110 | **ok** |
| Win32 + Direct2D | C++ | 433 | 161 | медленно |
| Electron | C++ | 329 | 62 | медленно |
| WinUI 3 | C# | 13 | 105 | `n/a` |

##### Мелкий заголовок h5

Inline styles: **bold**, *italic*, ***both***, ~~strikethrough~~, `inline code`, and a [link](https://commonmark.org).

###### Самый мелкий заголовок h6

---

## 22. Раздел 22: Startup budget

The quick brown fox jumps over the lazy dog, then renders a thousand paragraphs without breaking a sweat. Быстрый просмотрщик Markdown должен открываться мгновенно, без заставок и прогрева движка. Длинные абзацы должны переноситься по словам, а не по символам, и не вылезать за правую границу колонки.

### 22.1 Подраздел со списками

- Первый пункт с `inline code`
- Second item with **bold** text
  - Вложенный пункт
  - Nested item with a [link](https://example.com/docs)
    1. Deep ordered one
    2. Deep ordered two
- Третий пункт 🚀

1. Шаг первый
2. Step two with *emphasis*
3. Шаг третий

- [x] Parse markdown
- [x] Layout first viewport
- [ ] Syntax highlighting
- [ ] Math (KaTeX)

### 22.2 Code (rust)

```rust
fn main() {
    let words: Vec<&str> = "быстрый рендер markdown".split(' ').collect();
    for (i, w) in words.iter().enumerate() {
        println!("{i}: {w}");
    }
}
```

> Цитата: «Преждевременная оптимизация — корень всех зол», но время запуска — не преждевременная.
>
> — *Donald Knuth (почти)*

The quick brown fox jumps over the lazy dog, then renders a thousand paragraphs without breaking a sweat. The quick brown fox jumps over the lazy dog, then renders a thousand paragraphs without breaking a sweat.

#### 22.3 Таблица

| Stack | Language | Startup (ms) | Size (MB) | Notes |
|:--|:--:|--:|--:|:--|
| Avalonia | C++ | 1415 | 60 | **ok** |
| WebView2 | JS | 243 | 140 | медленно |
| WPF | Rust | 299 | 19 | быстро |
| WPF | Rust | 1226 | 192 | see [docs](https://example.com) |
| Electron | JS | 262 | 120 | **ok** |
| egui | Rust | 1032 | 139 | `n/a` |

##### Мелкий заголовок h5

Rendering speed is dominated by runtime initialization, not by parsing: parsers process hundreds of MB/s.

###### Самый мелкий заголовок h6

---

## 23. Раздел 23: Архитектура

Inline styles: **bold**, *italic*, ***both***, ~~strikethrough~~, `inline code`, and a [link](https://commonmark.org). Быстрый просмотрщик Markdown должен открываться мгновенно, без заставок и прогрева движка. A good viewer shows the first screen immediately and lays out the rest of the document lazily.

### 23.1 Подраздел со списками

- Первый пункт с `inline code`
- Second item with **bold** text
  - Вложенный пункт
  - Nested item with a [link](https://example.com/docs)
    1. Deep ordered one
    2. Deep ordered two
- Третий пункт 🚀

1. Шаг первый
2. Step two with *emphasis*
3. Шаг третий

- [x] Parse markdown
- [x] Layout first viewport
- [ ] Syntax highlighting
- [ ] Math (KaTeX)

### 23.2 Code (csharp)

```csharp
public sealed class Viewer : IDisposable
{
    private readonly string _path;
    public Viewer(string path) => _path = path ?? throw new ArgumentNullException(nameof(path));
    public async Task<string> LoadAsync() => await File.ReadAllTextAsync(_path);
    public void Dispose() { }
}
```

> Цитата: «Преждевременная оптимизация — корень всех зол», но время запуска — не преждевременная.
>
> — *Donald Knuth (почти)*

Markdown is a lightweight markup language with plain-text formatting syntax designed for readability. Inline styles: **bold**, *italic*, ***both***, ~~strikethrough~~, `inline code`, and a [link](https://commonmark.org).

#### 23.3 Таблица

| Stack | Language | Startup (ms) | Size (MB) | Notes |
|:--|:--:|--:|--:|:--|
| Electron | C++ | 195 | 59 | see [docs](https://example.com) |
| Win32 + Direct2D | Rust | 1188 | 11 | медленно |
| Avalonia | JS | 577 | 47 | see [docs](https://example.com) |
| egui | JS | 194 | 121 | **ok** |
| egui | Rust | 665 | 172 | быстро |
| WPF | Rust | 851 | 178 | `n/a` |

##### Мелкий заголовок h5

The quick brown fox jumps over the lazy dog, then renders a thousand paragraphs without breaking a sweat.

###### Самый мелкий заголовок h6

---

## 24. Раздел 24: Startup budget

Mixed script line: English, русский, 日本語のテキスト, 中文字符, and emoji 🚀✨📄 ✅ ❌ in one paragraph. Быстрый просмотрщик Markdown должен открываться мгновенно, без заставок и прогрева движка. Rendering speed is dominated by runtime initialization, not by parsing: parsers process hundreds of MB/s.

### 24.1 Подраздел со списками

- Первый пункт с `inline code`
- Second item with **bold** text
  - Вложенный пункт
  - Nested item with a [link](https://example.com/docs)
    1. Deep ordered one
    2. Deep ordered two
- Третий пункт 🚀

1. Шаг первый
2. Step two with *emphasis*
3. Шаг третий

- [x] Parse markdown
- [x] Layout first viewport
- [ ] Syntax highlighting
- [ ] Math (KaTeX)

### 24.2 Code (python)

```python
def render(doc: str) -> list[str]:
    """Разбить документ на блоки."""
    blocks = [b.strip() for b in doc.split("\n\n")]
    return [b for b in blocks if b]

print(render("# Привет\n\nмир"))
```

> Цитата: «Преждевременная оптимизация — корень всех зол», но время запуска — не преждевременная.
>
> — *Donald Knuth (почти)*

Типографика важна не меньше скорости: ровные поля, аккуратные заголовки, читаемый моноширинный шрифт. Markdown is a lightweight markup language with plain-text formatting syntax designed for readability.

#### 24.3 Таблица

| Stack | Language | Startup (ms) | Size (MB) | Notes |
|:--|:--:|--:|--:|:--|
| Electron | Rust | 245 | 249 | `n/a` |
| Win32 + Direct2D | JS | 854 | 14 | медленно |
| Tauri | JS | 1288 | 114 | быстро |
| WebView2 | Rust | 1132 | 34 | **ok** |
| Avalonia | JS | 256 | 8 | see [docs](https://example.com) |
| WebView2 | C# | 644 | 142 | быстро |

##### Мелкий заголовок h5

Mixed script line: English, русский, 日本語のテキスト, 中文字符, and emoji 🚀✨📄 ✅ ❌ in one paragraph.

###### Самый мелкий заголовок h6

---
