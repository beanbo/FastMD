# FastMD — README

Молниеносный просмотрщик **Markdown** для Windows. Типографика важна не меньше скорости: ровные поля, аккуратные заголовки, читаемый моноширинный шрифт. Быстрый просмотрщик Markdown должен открываться мгновенно, без заставок и прогрева движка.

## Возможности

- Мгновенный запуск ⚡
- GFM: таблицы, списки задач, ~~зачёркивание~~
- Подсветка `кода`

## Пример

```rust
fn main() {
    let words: Vec<&str> = "быстрый рендер markdown".split(' ').collect();
    for (i, w) in words.iter().enumerate() {
        println!("{i}: {w}");
    }
}
```

| Stack | Language | Startup (ms) | Size (MB) | Notes |
|:--|:--:|--:|--:|:--|
| Electron | C# | 465 | 36 | быстро |
| WinUI 3 | JS | 73 | 8 | быстро |
| WebView2 | C# | 1042 | 155 | быстро |
| WebView2 | JS | 459 | 115 | see [docs](https://example.com) |

> Tip: see the [CommonMark spec](https://spec.commonmark.org).

## 1. Раздел 1: Типографика

Быстрый просмотрщик Markdown должен открываться мгновенно, без заставок и прогрева движка. Съешь же ещё этих мягких французских булок, да выпей чаю — классическая панграмма для проверки шрифтов. A good viewer shows the first screen immediately and lays out the rest of the document lazily.

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

Markdown is a lightweight markup language with plain-text formatting syntax designed for readability. The quick brown fox jumps over the lazy dog, then renders a thousand paragraphs without breaking a sweat.

#### 1.3 Таблица

| Stack | Language | Startup (ms) | Size (MB) | Notes |
|:--|:--:|--:|--:|:--|
| WPF | C# | 697 | 27 | быстро |
| egui | C++ | 743 | 217 | **ok** |
| Electron | C++ | 948 | 138 | быстро |
| egui | C++ | 1138 | 76 | see [docs](https://example.com) |
| Tauri | C# | 1450 | 18 | быстро |
| WebView2 | Rust | 171 | 219 | медленно |

![diagram 1](img/diagram1.png)

##### Мелкий заголовок h5

Типографика важна не меньше скорости: ровные поля, аккуратные заголовки, читаемый моноширинный шрифт.

###### Самый мелкий заголовок h6

---
