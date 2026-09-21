# winget: манифест FastMD (этап 5.7)

Три файла рядом — готовый манифест для `microsoft/winget-pkgs`. В них подставляются три значения, которые появляются
только вместе с выпуском:

| Плейсхолдер | Откуда брать |
|---|---|
| `VERSION` | версия выпуска без `v`, например `1.0.0` (она же в `app/CMakeLists.txt`) |
| `SHA256` | из `FastMD-Setup.exe.sha256`, который кладёт `app/package.ps1` |
| `DATE` | дата выпуска, `ГГГГ-ММ-ДД` |

## Как выпустить

```powershell
pwsh -File app\package.ps1                      # соберёт zip, FastMD-Setup.exe и оба .sha256
$ver  = '1.0.0'
$hash = (Get-Content app\out\dist\FastMD-Setup.exe.sha256).Split(' ')[0]
Get-ChildItem packaging\winget\*.yaml | ForEach-Object {
    (Get-Content $_ -Raw).Replace('VERSION', $ver).Replace('SHA256', $hash).
        Replace('DATE', (Get-Date -Format 'yyyy-MM-dd')) | Set-Content $_.FullName -Encoding utf8
}
winget validate --manifest packaging\winget
winget install --manifest packaging\winget      # проверить установку у себя
```

Дальше манифест кладётся в форк `microsoft/winget-pkgs` по пути
`manifests/b/beanbo/FastMD/<версия>/` и отправляется пул-реквестом. **Это делает владелец проекта:** публикация в
общий каталог — действие от его имени, и в ней участвует живой ревьюер Microsoft.

Полезно знать:

- `Scope: user` — установщик ставит программу в профиль пользователя, права администратора не нужны;
- `Silent: /S` — тот же ключ, которым пользуется автообновление;
- `UpgradeBehavior: install` — новая версия ставится поверх, настройки и позиции чтения остаются в профиле;
- пока exe не подписан (задача 5.5), при установке будет предупреждение SmartScreen; подпись снимет и его.
