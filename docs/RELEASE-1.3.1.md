# FastMD 1.3.1

*Release notes as published on 03.10.2026: <https://github.com/beanbo/FastMD/releases/tag/v1.3.1>. Release commands are
at the end of `docs/RELEASE-1.0.0.md` (substitute the version). По-русски — ниже.*

---

A small release: the keyboard layout can be switched in edit mode, code blocks in HTML, YAML and shell are highlighted
the way GitHub does it, and FastMD is now under the MIT License.

## Fixed

- **The keyboard layout switches in edit mode.** The language hotkey from Windows' settings — Ctrl+Shift or
  Alt+Shift — did nothing over the page, so typing went on in the layout the window was opened with; only Win+Space
  worked. To open faster, FastMD keeps Windows' text services (TSF) out of the window until the first frame is on
  screen, and that hotkey reaches a window only through them. Now FastMD turns them on by itself right after the first
  frame: about 5 ms that start-up does not see. Input methods for Chinese, Japanese and Korean, the emoji panel and
  dictation still do not type into the page itself; they do in the search box and in the source panels.
- **HTML is highlighted as HTML.** In an ```` ```html ```` block every capitalised word between the tags was painted
  as a type and every year as a number: HTML had no lexer of its own and went through the generic rules for code.
  HTML, XML, SVG and XAML now have one, with GitHub's colours — tag names (a new colour), attributes, values,
  comments, CDATA and entities like `&amp;` — while the text between the tags stays plain. `<script>` and `<style>` are
  highlighted as JavaScript (JSON for `application/ld+json` and import maps) and CSS.
- **YAML as on GitHub.** Keys in the tag colour; values are strings, except numbers, `true`/`false`, `yes`/`no`,
  `null` and `~`; comments, anchors and aliases (`&base`, `*defaults`), tags (`!!str`), flow collections `[…]` and
  `{…}` with keys inside, block scalars `|` and `>`. Capitalised words in values are no longer types, and a `#` inside
  a URL is no longer a comment. Front matter that does not fold into a table is drawn the same way.
- **Shell and config files.** In bash/sh/zsh, bat/cmd, Dockerfile, Makefile, CMake, INI/TOML, nginx/apache, `.conf`,
  `.env` and TeX, a capitalised word is no longer a type, and a word followed by a space and a parenthesis is no
  longer a function call. Apache directives (`RewriteEngine`, `Directory`…) are highlighted as keywords: their list
  never matched before because of letter case.

## License: MIT

FastMD is now distributed under the [MIT License](https://github.com/beanbo/FastMD/blob/main/LICENSE) instead of the
GNU GPL v3.0: the code may be used, changed and passed on freely, in open and closed projects alike, commercial ones
included, as long as the copyright notice and the license text stay with it. Releases up to 1.3.0 came out under
GPL-3.0. Every bundled library was under a permissive license already: md4c, lunasvg, plutovg, RaTeX and
mermaid-rs-renderer under MIT, the Rust crates under MIT, Apache-2.0 and the like.

## Numbers

- **Start-up.** The only change on the way to the first frame is the new highlighting; the text services come
  after it. Start-up guard from explorer's context, 12 interleaved runs per document against the untuned empty
  Win32 window, medians: medium 62.6 ms against 84.4, large (3.7 MB) 65.5 against 84.6, small 65.2 against 85.0.
- **Checked with real keys** from explorer's context, in edit mode: F, Ctrl+Shift, F, Ctrl+Shift, F. 1.3.0 typed
  «ааа», 1.3.1 types «аfа»; Win+Space gives «аfа» in both.
- **Highlighting:** all 1,698 code blocks found in the repository's documents (C, C++, C#, JS, Rust, Python,
  PowerShell, JSON) are coloured exactly as before; the new lexers took 500,000 random inputs under
  AddressSanitizer.
- **Tests:** 517 UI checks, among them a new one that the window's text services are up after the first frame;
  1,437,943 checks of the edit core. The exe grew by 8.7 KB, to 1.46 MB.

Everything else is as in [1.3.0](https://github.com/beanbo/FastMD/releases/tag/v1.3.0).

## Install

| How | What to do |
|---|---|
| Installer | download `FastMD-Setup.exe` and run it. No admin rights needed |
| Without installing | unpack `FastMD-1.3.1-win-x64.zip` anywhere and run `FastMD.exe` |

Each file has a `.sha256` beside it to check the download. The exe is not signed: SmartScreen warns on the first run
(More info → Run anyway). FastMD 1.3.0 offers this update by itself: a dot on the gear button, and «Обновление» in the
settings. 1.0.0–1.2.0 find it once a day, if checking for updates is on in the settings.

---

## По-русски

Небольшой выпуск: в режиме правки переключается раскладка, блоки кода на HTML, YAML и shell подсвечиваются как на
GitHub, а FastMD теперь под лицензией MIT.

**Раскладка переключается в режиме правки.** Сочетание переключения языка из настроек Windows — Ctrl+Shift или
Alt+Shift — над страницей ничего не делало, и текст набирался на том языке, с которым окно открылось; работал только
Win+Space. Чтобы открываться быстрее, FastMD не пускает в окно службы ввода Windows (TSF), пока на экране нет первого
кадра, а это сочетание доходит до окна только через них. Теперь FastMD включает их сам сразу после первого кадра: около
5 мс, которых запуск не замечает. Методы ввода для китайского, японского и корейского, панель эмодзи и диктовка
по-прежнему не печатают в саму страницу; в поле поиска и в панелях исходника — печатают.

**HTML подсвечивается как HTML.** В блоке ```` ```html ```` каждое слово с большой буквы между тегами красилось как
«тип», а каждый год — как число: своего разбора у HTML не было, и текст шёл через общие правила для кода. Теперь у
HTML, XML, SVG и XAML свой разборщик и цвета как на GitHub — имена тегов (новый цвет), атрибуты, значения,
комментарии, CDATA и сущности вроде `&amp;`, — а текст между тегами остаётся обычным. `<script>` и `<style>`
подсвечиваются как JavaScript (JSON — для `application/ld+json` и import maps) и CSS.

**YAML — как на GitHub.** Ключи — цветом тегов; значения — строки, кроме чисел, `true`/`false`, `yes`/`no`, `null` и
`~`; комментарии, якоря и ссылки (`&base`, `*defaults`), теги (`!!str`), списки `[…]` и `{…}` с ключами внутри, блоки
`|` и `>`. Слова с большой буквы в значениях больше не «типы», а `#` внутри адреса — не комментарий. Так же рисуется
front matter, который не складывается в таблицу.

**Shell и конфиги.** В bash/sh/zsh, bat/cmd, Dockerfile, Makefile, CMake, INI/TOML, nginx/apache, `.conf`, `.env` и
TeX слово с большой буквы больше не считается типом, а слово перед скобкой через пробел — вызовом функции. Директивы
Apache (`RewriteEngine`, `Directory`…) подсвечиваются как ключевые слова: раньше их список не совпадал из-за регистра.

**Лицензия — MIT.** FastMD теперь распространяется по [лицензии MIT](https://github.com/beanbo/FastMD/blob/main/LICENSE)
вместо GNU GPL v3.0: код можно свободно использовать, менять и передавать дальше — в открытых и закрытых проектах, в
том числе коммерческих, — если вместе с ним остаются уведомление об авторских правах и текст лицензии. Выпуски до 1.3.0
включительно вышли под GPL-3.0. Все библиотеки в составе и так были под разрешительными лицензиями: md4c, lunasvg,
plutovg, RaTeX и mermaid-rs-renderer — под MIT, библиотеки Rust — под MIT, Apache-2.0 и подобными.

**Цифры.** На пути к первому кадру изменилась только подсветка, службы ввода включаются после него. Замер запуска из
контекста Проводника, по 12 чередующихся запусков на документ против ненастроенного пустого окна Win32, медианы: medium
62,6 мс против 84,4, large (3,7 МБ) 65,5 против 84,6, small 65,2 против 85,0. Проверено настоящими нажатиями клавиш из
контекста Проводника, в режиме правки: F, Ctrl+Shift, F, Ctrl+Shift, F — 1.3.0 набирал «ааа», 1.3.1 набирает «аfа»;
Win+Space даёт «аfа» в обеих версиях. Все 1 698 блоков кода из документов репозитория (C, C++, C#, JS, Rust, Python,
PowerShell, JSON) раскрашиваются в точности как раньше; новые разборщики прошли 500 000 случайных входов под
AddressSanitizer. Тесты: 517 проверок UI-теста, среди них новая — что службы ввода окна включаются после первого кадра;
1 437 943 проверки ядра правки. Exe вырос на 8,7 КБ, до 1,46 МБ.

Всё остальное — как в [1.3.0](https://github.com/beanbo/FastMD/releases/tag/v1.3.0).

| Способ | Что делать |
|---|---|
| Установщик | скачайте `FastMD-Setup.exe` и запустите. Права администратора не нужны |
| Без установки | распакуйте `FastMD-1.3.1-win-x64.zip` куда угодно и запустите `FastMD.exe` |

Рядом с каждым файлом лежит `.sha256`, чтобы проверить загруженное. Exe не подписан: SmartScreen предупредит при
первом запуске («Подробнее» → «Выполнить в любом случае»). FastMD 1.3.0 сам предложит это обновление: точка на
шестерёнке и строка «Обновление» в настройках. 1.0.0–1.2.0 найдут его раз в сутки, если проверка обновлений включена в
настройках.
