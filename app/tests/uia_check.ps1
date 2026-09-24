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
$IP = [System.Windows.Automation.InvokePattern]
$TS = [System.Windows.Automation.TreeScope]
# edit mode is driven the way ui_smoke.py drives it: posted characters and keys, WM_APP_QUERY for its state
Add-Type -Namespace FastMDCheck -Name User32 -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
'@
$U32 = [FastMDCheck.User32]
$Q = @{ EDITING = 42; EDIT_BAR = 47; UNDO_DEPTH = 48; EDIT_ACTIVE = 62 }
$CMD = @{ SELECT_ALL = 101; UNDO = 144 }

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

    # ---- edit mode (docs/EDIT-MODE.md §12.8, §14.4)
    function Query($name, $lp = 0) { $U32::SendMessageW($hwnd, 0x8040, [IntPtr]$Q[$name], [IntPtr]$lp).ToInt64() }
    function Command($name) { $U32::PostMessageW($hwnd, 0x111, [IntPtr]$CMD[$name], [IntPtr]::Zero) | Out-Null; Start-Sleep -Milliseconds 300 }
    function Key($vk) { $U32::PostMessageW($hwnd, 0x100, [IntPtr]$vk, [IntPtr]::Zero) | Out-Null; Start-Sleep -Milliseconds 60 }
    function TypeText($s, $gap = 15) {
        foreach ($c in $s.ToCharArray()) {
            $U32::PostMessageW($hwnd, 0x102, [IntPtr][int]$c, [IntPtr]::Zero) | Out-Null
            Start-Sleep -Milliseconds $gap
        }
    }
    function WaitFor($cond, $ms = 3000) {
        for ($t = 0; $t -lt $ms; $t += 50) { if (& $cond) { return $true }; Start-Sleep -Milliseconds 50 }
        return [bool](& $cond)
    }
    # a button of the document by its name (either UI language)
    function Button($ru, $en) {
        $c = New-Object System.Windows.Automation.OrCondition(
            (New-Object System.Windows.Automation.PropertyCondition($AE::NameProperty, $ru)),
            (New-Object System.Windows.Automation.PropertyCondition($AE::NameProperty, $en)))
        return $el.FindFirst($TS::Children, $c)
    }
    function ReadOnly() { $tp.DocumentRange.GetAttributeValue($TP::IsReadOnlyAttribute) }

    Check '2c reading mode: the text is read-only' ((ReadOnly) -eq $true)
    $pencil = Button 'Редактировать (F2 или двойной щелчок)' 'Edit (F2 or double-click)'
    Check '2c the pencil is a button found by its name, F2 its accelerator key' `
        ($null -ne $pencil -and $pencil.Current.ControlType.ProgrammaticName -eq 'ControlType.Button' -and
         $pencil.Current.AcceleratorKey -eq 'F2')
    if ($pencil) { $pencil.GetCurrentPattern($IP::Pattern).Invoke() }
    $entered = WaitFor { (Query EDITING) -eq 1 -and (Query EDIT_BAR) -eq 100 }
    Check '2c Invoke on the pencil enters edit mode, and the text is no longer read-only' ($entered -and (ReadOnly) -eq $false)

    TypeText 'Набрано '
    Start-Sleep -Milliseconds 400
    $all = $tp.DocumentRange.GetText(-1)
    Check '2c the document range holds what was typed' ($all -match 'Набрано') $all.Substring(0, [Math]::Min(40, $all.Length))

    # a range held across a deletion: every call works on what is left, nothing throws
    $old = $tp.DocumentRange.Clone()
    Command SELECT_ALL
    Key 0x2E  # Delete
    Start-Sleep -Milliseconds 400
    $errs = @()
    try { $null = $old.GetText(-1) } catch { $errs += "GetText: $_" }
    try { $null = $old.FindText('Конец', $false, $false) } catch { $errs += "FindText: $_" }
    try { $old.Select() } catch { $errs += "Select: $_" }
    $left = $tp.DocumentRange.GetText(-1)
    Check '2c a range made before a deletion still answers GetText, FindText and Select' `
        ($errs.Count -eq 0 -and $left.Length -lt 10 -and -not $proc.HasExited) ("{0}; left {1} characters" -f ($errs -join ', '), $left.Length)
    Command UNDO
    Start-Sleep -Milliseconds 300
    Check '2c ... and the undo brings the text back' ($tp.DocumentRange.GetText(-1) -match 'Конец документа')

    # a screen reader reading the whole text over and over, from another process, while the reader types
    $job = Start-Job -ArgumentList $hwnd.ToInt64() -ScriptBlock {
        param($h)
        Add-Type -AssemblyName UIAutomationClient
        Add-Type -AssemblyName UIAutomationTypes
        $e = [System.Windows.Automation.AutomationElement]::FromHandle([IntPtr]$h)
        $t = $e.GetCurrentPattern([System.Windows.Automation.TextPattern]::Pattern)
        $n = 0
        $end = (Get-Date).AddSeconds(6)
        while ((Get-Date) -lt $end) { $null = $t.DocumentRange.GetText(-1); $n++ }
        $n
    }
    for ($k = 0; $k -lt 6 -and $job.State -eq 'Running'; $k++) {
        TypeText 'печать поверх чтения ' 12
        for ($b = 0; $b -lt 6; $b++) { Key 0x08 }  # Backspace
    }
    $reads = Receive-Job $job -Wait -ErrorAction SilentlyContinue
    $jerr = $job.ChildJobs[0].Error.Count
    Remove-Job $job -Force
    Check '2c GetText in a loop from another process while typing: the window lives, every read answered' `
        (-not $proc.HasExited -and $jerr -eq 0 -and [int]($reads | Select-Object -Last 1) -gt 10) ("{0} reads, {1} errors" -f ($reads | Select-Object -Last 1), $jerr)

    # the toolbar: its buttons found by name and pressed through Invoke run what a click runs
    $undo = Button 'Отменить' 'Undo'
    $redo = Button 'Повторить' 'Redo'
    Check '2c the bar''s buttons are children of the document with their names, shortcuts and enabled states' `
        ($null -ne $undo -and $undo.Current.AcceleratorKey -eq 'Ctrl+Z' -and $undo.Current.IsEnabled -and
         $null -ne $redo -and -not $redo.Current.IsEnabled) ("undo {0}, redo enabled {1}" -f $undo.Current.AcceleratorKey, $redo.Current.IsEnabled)
    $depth = Query UNDO_DEPTH
    $undo.GetCurrentPattern($IP::Pattern).Invoke()
    Check '2c Invoke on Undo undoes a step (and Redo becomes possible)' `
        ((WaitFor { (Query UNDO_DEPTH) -eq $depth - 1 }) -and $redo.Current.IsEnabled) ("undo depth {0} -> {1}" -f $depth, (Query UNDO_DEPTH))
    if ($U32::GetForegroundWindow() -eq $hwnd) {  # (the point must show this window, not one over it)
        $r = $undo.Current.BoundingRectangle
        $at = $AE::FromPoint((New-Object System.Windows.Point(($r.Left + $r.Width / 2), ($r.Top + $r.Height / 2))))
        Check '2c the element under a button''s centre is that button' ($at.Current.Name -eq $undo.Current.Name) $at.Current.Name
    } else {
        Write-Host '[SKIP] 2c the element under a button: the window is not in front'
    }
    # 3a: a popover's rows are buttons too - the style popover's «Heading 2» found by its name, pressed through Invoke
    $style = Button 'Стиль абзаца' 'Paragraph style'
    if ($style) { $style.GetCurrentPattern($IP::Pattern).Invoke() }
    $h2 = $null
    for ($k = 0; $k -lt 30 -and $null -eq $h2; $k++) { Start-Sleep -Milliseconds 100; $h2 = Button 'Заголовок 2' 'Heading 2' }
    Check '3a the style popover''s rows are buttons of the document, with their names and shortcuts' `
        ($null -ne $style -and $null -ne $h2 -and $h2.Current.AcceleratorKey -eq 'Ctrl+2' -and $h2.Current.IsEnabled) `
        ("style {0}, row {1}" -f ($null -ne $style), $(if ($h2) { $h2.Current.AcceleratorKey } else { 'none' }))
    if ($h2) { $h2.GetCurrentPattern($IP::Pattern).Invoke() }
    $made = WaitFor { ((Query EDIT_ACTIVE) -shr 24) -eq 2 }
    Check '3a Invoke on a row runs it - a heading 2 - and the popover and its rows are gone' `
        ($made -and $null -eq (Button 'Заголовок 2' 'Heading 2')) ("style {0}" -f ((Query EDIT_ACTIVE) -shr 24))
    Command UNDO
    $close = Button 'Закончить редактирование' 'Finish editing'
    $esc = if ($close) { $close.Current.AcceleratorKey } else { '' }
    if ($close) { $close.GetCurrentPattern($IP::Pattern).Invoke() }
    $left = WaitFor { (Query EDITING) -eq 0 -and (Query EDIT_BAR) -eq 0 }  # (and the bar slid away)
    $gone = $null -eq (Button 'Отменить' 'Undo')
    Check '2c Invoke on the ✕ (Esc) leaves edit mode; the text is read-only again, the bar''s buttons are gone' `
        ($esc -eq 'Esc' -and $left -and (ReadOnly) -eq $true -and $gone) ("key {0}, left {1}, buttons gone {2}" -f $esc, $left, $gone)
} finally {
    if (-not $proc.HasExited) { $proc.CloseMainWindow() | Out-Null; Start-Sleep -Milliseconds 600 }
    if (-not $proc.HasExited) { $proc.Kill() }
}
Write-Host ("RESULT {0}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }))
if (-not $ok) { exit 1 }
