# Screen-reader check (plan 6.1): drives the window through UI Automation, the way Narrator and NVDA do.
# Uses the UIAutomationClient assembly of the .NET Framework, so it runs under Windows PowerShell:
#   powershell -ExecutionPolicy Bypass -File app\tests\uia_check.ps1
param([string]$Exe = "$PSScriptRoot\..\build\Release\FastMD.exe")

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
$AE = [System.Windows.Automation.AutomationElement]
$TP = [System.Windows.Automation.TextPattern]
$TU = [System.Windows.Automation.Text.TextUnit]

$ok = $true
function Check($name, $cond, $info = '') {
    $script:ok = $script:ok -and [bool]$cond
    $mark = if ($cond) { 'OK' } else { 'FAIL' }
    Write-Host ("[{0}] {1}{2}" -f $mark, $name, $(if ($info) { ": $info" } else { '' }))
}

$out = Join-Path $PSScriptRoot 'out'
New-Item -ItemType Directory -Force $out | Out-Null
$doc = Join-Path $out 'uia.md'
@'
# Первый заголовок

Обычный абзац, который диктор должен прочитать целиком.

## Второй заголовок

Второй абзац с несколькими словами.

Конец документа.
'@ | Set-Content -Path $doc -Encoding utf8

$env:FASTMD_REGKEY = 'Software\FastMD-uitest'
$env:FASTMD_DATA = Join-Path $out 'profile'
$proc = Start-Process -FilePath $Exe -ArgumentList '--light', '--size=900x800', $doc -PassThru
try {
    $hwnd = [IntPtr]::Zero
    for ($i = 0; $i -lt 60 -and $hwnd -eq [IntPtr]::Zero; $i++) {
        Start-Sleep -Milliseconds 200
        $proc.Refresh()
        $hwnd = $proc.MainWindowHandle
    }
    Check 'the window is up' ($hwnd -ne [IntPtr]::Zero)
    Start-Sleep -Milliseconds 600

    $el = $AE::FromHandle($hwnd)
    Check '6.1 UI Automation finds the window' ($null -ne $el)
    $name = $el.Current.Name
    $type = $el.Current.ControlType.ProgrammaticName
    Check '6.1 it is a document, named after the file' ($type -eq 'ControlType.Document' -and $name -eq 'uia.md') "$type / $name"

    $tp = $el.GetCurrentPattern($TP::Pattern)
    Check '6.1 it offers the text pattern a screen reader needs' ($null -ne $tp)

    $all = $tp.DocumentRange.GetText(-1)
    Check '6.1 the whole document can be read' ($all -match 'Первый заголовок' -and $all -match 'Конец документа') `
        ("{0} символов" -f $all.Length)

    $visible = $tp.GetVisibleRanges()
    Check '6.1 it reports what is on screen' ($visible.Count -ge 1 -and $visible[0].GetText(-1).Length -gt 0)

    $r = $tp.DocumentRange.Clone()
    $r.ExpandToEnclosingUnit($TU::Paragraph)
    $first = $r.GetText(-1).Trim()
    Check '6.1 by paragraph: the first one is the heading' ($first -eq 'Первый заголовок') $first

    # The heading level goes out as the UIA StyleId attribute (StyleId_Heading1..6) - that is how Narrator and NVDA
    # jump from heading to heading. The .NET client here is UI Automation 1.0 and cannot ask for that attribute, so
    # what is checked instead is the walk it is built on: every paragraph in order, headings among them.
    $walk = @()
    $w2 = $tp.DocumentRange.Clone()
    $w2.ExpandToEnclosingUnit($TU::Paragraph)
    for ($k = 0; $k -lt 12; $k++) {
        $text = $w2.GetText(-1).Trim()
        if ($text) { $walk += $text }
        if ($w2.Move($TU::Paragraph, 1) -ne 1) { break }
    }
    $i1 = [array]::IndexOf($walk, 'Первый заголовок')
    $i2 = [array]::IndexOf($walk, 'Второй заголовок')
    Check '6.1 every paragraph can be walked, headings included' ($i1 -eq 0 -and $i2 -gt $i1) ($walk -join ' | ')

    $moved = $r.Move($TU::Paragraph, 1)
    $second = $r.GetText(-1).Trim()
    Check '6.1 moving a paragraph forward lands on the next one' ($moved -eq 1 -and $second -like 'Обычный абзац*') $second

    $w = $tp.DocumentRange.Clone()
    $w.ExpandToEnclosingUnit($TU::Word)
    Check '6.1 by word' ($w.GetText(-1).Trim() -eq 'Первый') $w.GetText(-1)
    $w.Move($TU::Word, 1) | Out-Null
    Check '6.1 and the next word' ($w.GetText(-1).Trim() -eq 'заголовок') $w.GetText(-1)

    $l = $tp.DocumentRange.Clone()
    $l.ExpandToEnclosingUnit($TU::Line)
    Check '6.1 by line' ($l.GetText(-1).Trim().Length -gt 0) $l.GetText(-1).Trim()

    $rects = $r.GetBoundingRectangles()
    Check '6.1 it can point at where the text is on screen' ($rects.Count -ge 1) ("{0} прямоугольник(ов)" -f $rects.Count)

    $r.Select()
    Start-Sleep -Milliseconds 300
    $sel = $tp.GetSelection()
    Check '6.1 selecting through the reader works' ($sel.Count -ge 1 -and $sel[0].GetText(-1).Trim() -eq $second) `
        $sel[0].GetText(-1).Trim()
} finally {
    if (-not $proc.HasExited) { $proc.CloseMainWindow() | Out-Null; Start-Sleep -Milliseconds 600 }
    if (-not $proc.HasExited) { $proc.Kill() }
}
Write-Host ("RESULT {0}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }))
if (-not $ok) { exit 1 }
