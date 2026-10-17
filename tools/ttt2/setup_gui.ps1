# Tekken Tag Tournament 2 port: the graphical setup (Setup-TTT2.bat in the repository's root starts it).
# A window around tools\ttt2\build-windows.bat: pick the extracted game, check it, build, make a desktop
# shortcut, play. Windows PowerShell 5.1 and WinForms only: nothing to install first.
# The build's whole output goes to <build folder>\setup.log (attach it to bug reports).

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()
# anything unexpected: say so in a window (the console is hidden)
trap { [void][System.Windows.Forms.MessageBox]::Show("Setup error: $($_.Exception.Message)`r`n$($_.InvocationInfo.PositionMessage)", 'TTT2 setup'); exit 1 }

$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Repo = (Resolve-Path (Join-Path $Here '..\..')).Path
$BuildBat = Join-Path $Here 'build-windows.bat'
$RpxSha256 = 'C3AFC72F30AA1AF84901BFC0775AEAE7A359D396923A387CFBE71034175886AB'
$NeedGB = 15
$SettingsFile = Join-Path $Repo '.tools\setup.ini'

$Ru = (Get-Culture).TwoLetterISOLanguageName -eq 'ru' -or (Get-UICulture).TwoLetterISOLanguageName -eq 'ru'
function L([string]$ru, [string]$en) { if ($script:Ru) { $ru } else { $en } }

# keeps the PC awake while it builds (an hour on a laptop is common)
Add-Type -Namespace TTT2 -Name Power -MemberDefinition '[DllImport("kernel32.dll")] public static extern uint SetThreadExecutionState(uint flags);'

# ---------------------------------------------------------------- settings (last game folder, build folder)
function Load-Settings {
    $s = @{}
    if (Test-Path -LiteralPath $SettingsFile) {
        foreach ($line in [IO.File]::ReadAllLines($SettingsFile, [Text.Encoding]::UTF8)) {
            $i = $line.IndexOf('=')
            if ($i -gt 0) { $s[$line.Substring(0, $i)] = $line.Substring($i + 1) }
        }
    }
    $s
}
function Save-Settings($game, $work) {
    New-Item -ItemType Directory -Force -Path (Split-Path $SettingsFile) | Out-Null
    [IO.File]::WriteAllLines($SettingsFile, [string[]]@("game=$game", "work=$work"), (New-Object Text.UTF8Encoding $false))
}

# ---------------------------------------------------------------- look
$Bg      = [Drawing.Color]::FromArgb(24, 26, 31)
$Panel   = [Drawing.Color]::FromArgb(34, 37, 44)
$Field   = [Drawing.Color]::FromArgb(44, 48, 57)
$Text    = [Drawing.Color]::FromArgb(232, 234, 238)
$Dim     = [Drawing.Color]::FromArgb(150, 156, 168)
$Accent  = [Drawing.Color]::FromArgb(226, 64, 47)
$AccentH = [Drawing.Color]::FromArgb(242, 92, 74)
$Good    = [Drawing.Color]::FromArgb(92, 200, 120)
$Bad     = [Drawing.Color]::FromArgb(240, 96, 88)
$Warn    = [Drawing.Color]::FromArgb(236, 180, 70)
$Font    = New-Object Drawing.Font('Segoe UI', 10)
$FontB   = New-Object Drawing.Font('Segoe UI Semibold', 10.5)
$FontH   = New-Object Drawing.Font('Segoe UI Semibold', 18)
$FontMono = New-Object Drawing.Font('Consolas', 9)

# (parameters must not be named like the colour and font variables above: PowerShell names ignore case)
function New-Label($caption, $x, $y, $w, $h, $useFont = $null, $useColor = $null) {
    if (-not $useFont) { $useFont = $script:Font }
    if (-not $useColor) { $useColor = $script:Text }
    $l = New-Object Windows.Forms.Label
    $l.Text = $caption; $l.Location = New-Object Drawing.Point($x, $y); $l.Size = New-Object Drawing.Size($w, $h)
    $l.Font = $useFont; $l.ForeColor = $useColor; $l.BackColor = [Drawing.Color]::Transparent
    $l
}
function New-Button($caption, $x, $y, $w, $h, [bool]$primary = $false) {
    $b = New-Object Windows.Forms.Button
    $b.Text = $caption; $b.Location = New-Object Drawing.Point($x, $y); $b.Size = New-Object Drawing.Size($w, $h)
    $b.FlatStyle = 'Flat'; $b.Font = if ($primary) { $FontB } else { $Font }
    $b.ForeColor = $Text; $b.Cursor = [Windows.Forms.Cursors]::Hand
    $b.BackColor = if ($primary) { $Accent } else { $Field }
    $b.FlatAppearance.BorderSize = 0
    $b.FlatAppearance.MouseOverBackColor = if ($primary) { $AccentH } else { [Drawing.Color]::FromArgb(58, 63, 74) }
    $b
}
function New-TextBox($x, $y, $w) {
    $t = New-Object Windows.Forms.TextBox
    $t.Location = New-Object Drawing.Point($x, $y); $t.Size = New-Object Drawing.Size($w, 28)
    $t.Font = $Font; $t.BackColor = $Field; $t.ForeColor = $Text; $t.BorderStyle = 'FixedSingle'
    $t
}

# ---------------------------------------------------------------- window
$form = New-Object Windows.Forms.Form
$form.Text = L 'Tekken Tag Tournament 2 — сборка порта' 'Tekken Tag Tournament 2 — port setup'
$form.ClientSize = New-Object Drawing.Size(760, 640)
$form.StartPosition = 'CenterScreen'
$form.FormBorderStyle = 'FixedSingle'
$form.MaximizeBox = $false
$form.BackColor = $Bg
$form.Font = $Font
$form.AutoScaleMode = 'Dpi'
$exe = Join-Path $Repo 'build-ttt2\TTT2-port\TTT2.ico'
if (Test-Path -LiteralPath $exe) { try { $form.Icon = New-Object Drawing.Icon($exe) } catch {} }

$header = New-Object Windows.Forms.Panel
$header.Location = New-Object Drawing.Point(0, 0); $header.Size = New-Object Drawing.Size(760, 84); $header.BackColor = $Panel
$stripe = New-Object Windows.Forms.Panel
$stripe.Location = New-Object Drawing.Point(0, 80); $stripe.Size = New-Object Drawing.Size(760, 4); $stripe.BackColor = $Accent
$header.Controls.Add($stripe)
$header.Controls.Add((New-Label 'TEKKEN TAG TOURNAMENT 2' 24 12 700 36 $FontH))
$header.Controls.Add((New-Label (L 'Нативный порт для Windows · собирается из вашей собственной копии игры' 'Native Windows port · built from your own copy of the game') 26 48 700 22 $Font $Dim))
$form.Controls.Add($header)

# 1. game folder
$form.Controls.Add((New-Label (L '1. Папка с игрой' '1. Game folder') 24 100 400 24 $FontB))
$form.Controls.Add((New-Label (L 'Распакованный дамп Wii U (папки code, content, meta), версия EUR [AKNPAF] без обновлений.' 'Your extracted Wii U dump (code, content, meta folders), EUR [AKNPAF], no update.') 24 124 712 20 $Font $Dim))
$gameBox = New-TextBox 24 150 590
$gameBtn = New-Button (L 'Выбрать…' 'Browse…') 624 149 112 30
$gameStatus = New-Label '' 24 184 712 22
$dumpLink = New-Object Windows.Forms.LinkLabel
$dumpLink.Text = L 'Как сделать дамп игры со своей Wii U?' 'How do I dump the game from my Wii U?'
$dumpLink.Location = New-Object Drawing.Point(24, 206); $dumpLink.Size = New-Object Drawing.Size(400, 20)
$dumpLink.LinkColor = $AccentH; $dumpLink.ActiveLinkColor = $Text; $dumpLink.Font = $Font
$dumpLink.Add_LinkClicked({ Start-Process 'https://cemu.cfw.guide/dumping-games.html' })
$form.Controls.AddRange(@($gameBox, $gameBtn, $gameStatus, $dumpLink))

# 2. build folder
$form.Controls.Add((New-Label (L '2. Куда собрать' '2. Build folder') 24 238 400 24 $FontB))
$workBox = New-TextBox 24 264 590
$workBtn = New-Button (L 'Выбрать…' 'Browse…') 624 263 112 30
$workStatus = New-Label '' 24 298 712 22
$form.Controls.AddRange(@($workBox, $workBtn, $workStatus))

$shortcutBox = New-Object Windows.Forms.CheckBox
$shortcutBox.Text = L 'Создать ярлык на рабочем столе' 'Create a desktop shortcut'
$shortcutBox.Location = New-Object Drawing.Point(24, 326); $shortcutBox.Size = New-Object Drawing.Size(400, 26)
$shortcutBox.ForeColor = $Text; $shortcutBox.Checked = $true
$form.Controls.Add($shortcutBox)

# build / progress
$buildBtn = New-Button (L 'Собрать игру' 'Build the game') 24 362 220 44 $true
$cancelBtn = New-Button (L 'Отмена' 'Cancel') 254 362 120 44
$cancelBtn.Visible = $false
$playBtn = New-Button (L '▶  Играть' '▶  Play') 516 362 220 44 $true
$playBtn.Visible = $false
$folderBtn = New-Button (L 'Открыть папку' 'Open folder') 384 362 122 44
$folderBtn.Visible = $false
$form.Controls.AddRange(@($buildBtn, $cancelBtn, $playBtn, $folderBtn))

$stageLabel = New-Label '' 24 418 560 22 $FontB
$timeLabel = New-Label '' 586 418 150 22 $Font $Dim
$timeLabel.TextAlign = 'TopRight'
$barBack = New-Object Windows.Forms.Panel
$barBack.Location = New-Object Drawing.Point(24, 444); $barBack.Size = New-Object Drawing.Size(712, 10); $barBack.BackColor = $Field
$barFill = New-Object Windows.Forms.Panel
$barFill.Location = New-Object Drawing.Point(0, 0); $barFill.Size = New-Object Drawing.Size(0, 10); $barFill.BackColor = $Accent
$barBack.Controls.Add($barFill)
$form.Controls.AddRange(@($stageLabel, $timeLabel, $barBack))

$logBox = New-Object Windows.Forms.TextBox
$logBox.Multiline = $true; $logBox.ReadOnly = $true; $logBox.ScrollBars = 'Vertical'; $logBox.WordWrap = $false
$logBox.Location = New-Object Drawing.Point(24, 466); $logBox.Size = New-Object Drawing.Size(712, 156)
$logBox.BackColor = [Drawing.Color]::FromArgb(16, 17, 21); $logBox.ForeColor = $Dim; $logBox.Font = $FontMono
$logBox.BorderStyle = 'None'
$form.Controls.Add($logBox)

# ---------------------------------------------------------------- checks
$script:GameOk = $false
$script:WorkOk = $false
$script:Proc = $null

function Set-Status($label, $text, $color) { $label.Text = $text; $label.ForeColor = $color }

function Find-Game([string]$path) {
    # the folder itself, or one level below it (the user picked the folder that holds the dump)
    if (-not $path) { return $null }
    $path = $path.Trim().Trim('"')
    if (-not (Test-Path -LiteralPath $path -PathType Container)) { return $null }
    if (Test-Path -LiteralPath (Join-Path $path 'code\Tekken.rpx')) { return $path }
    foreach ($d in Get-ChildItem -LiteralPath $path -Directory -ErrorAction SilentlyContinue) {
        if (Test-Path -LiteralPath (Join-Path $d.FullName 'code\Tekken.rpx')) { return $d.FullName }
    }
    $null
}

function Check-Game {
    $script:GameOk = $false
    $raw = $gameBox.Text
    if (-not $raw.Trim()) { Set-Status $gameStatus (L 'Укажите папку с игрой.' 'Choose the game folder.') $Dim; Update-Buttons; return }
    $found = Find-Game $raw
    if (-not $found) {
        Set-Status $gameStatus (L '✗ Здесь нет code\Tekken.rpx — выберите распакованную папку игры.' '✗ No code\Tekken.rpx here: choose the extracted game folder.') $Bad
        Update-Buttons; return
    }
    if ($found -ne $raw.Trim().Trim('"')) { $gameBox.Text = $found }
    Set-Status $gameStatus (L 'Проверяю игру…' 'Checking the game…') $Dim
    $form.Refresh()
    $hash = (Get-FileHash -LiteralPath (Join-Path $found 'code\Tekken.rpx') -Algorithm SHA256).Hash
    if ($hash -ne $RpxSha256) {
        Set-Status $gameStatus (L '✗ Другая версия игры. Нужна Tekken Tag Tournament 2 Wii U Edition EUR [AKNPAF] без обновлений.' '✗ Another version of the game. The EUR release [AKNPAF] without updates is needed.') $Bad
    } elseif (-not (Test-Path -LiteralPath (Join-Path $found 'content'))) {
        Set-Status $gameStatus (L '✗ Нет папки content: дамп неполный.' '✗ No content folder: the dump is incomplete.') $Bad
    } else {
        Set-Status $gameStatus (L '✓ Tekken Tag Tournament 2 Wii U Edition [AKNPAF] — подходит' '✓ Tekken Tag Tournament 2 Wii U Edition [AKNPAF]: supported') $Good
        $script:GameOk = $true
    }
    Update-Buttons
}

function Port-Dir { Join-Path $workBox.Text.Trim().Trim('"') 'TTT2-port' }
function Is-Built { $w = $workBox.Text.Trim(); $w -and (Test-Path -LiteralPath (Join-Path (Port-Dir) 'TTT2.exe')) }

function Check-Work {
    $script:WorkOk = $false
    $w = $workBox.Text.Trim().Trim('"')
    if (-not $w) { Set-Status $workStatus (L 'Укажите папку для сборки.' 'Choose a build folder.') $Dim; Update-Buttons; return }
    try { $full = [IO.Path]::GetFullPath($w) } catch { Set-Status $workStatus (L '✗ Неверный путь.' '✗ Invalid path.') $Bad; Update-Buttons; return }
    $root = [IO.Path]::GetPathRoot($full)
    $free = -1
    try { $free = [math]::Floor((New-Object IO.DriveInfo($root)).AvailableFreeSpace / 1GB) } catch {}
    $notes = @()
    if ($full.Length -gt 60) { $notes += (L 'длинный путь: если сборка упадёт, возьмите короткий, например C:\TTT2' 'long path: if the build fails, use a short one such as C:\TTT2') }
    if ($full -match '[^\x00-\x7F]' -or $full.Contains(' ')) { $notes += (L 'в пути пробелы или кириллица — лучше без них' 'spaces or non-English letters in the path: better without') }
    if (Is-Built) {
        Set-Status $workStatus ((L '✓ Игра уже собрана здесь. Можно играть или пересобрать.' '✓ The game is already built here. Play, or build again.')) $Good
        $script:WorkOk = $true
    } elseif ($free -ge 0 -and $free -lt $NeedGB) {
        Set-Status $workStatus ((L "✗ На диске $root свободно $free ГБ, нужно около $NeedGB ГБ." "✗ $free GB free on $root, about $NeedGB GB needed.")) $Bad
    } else {
        $msg = if ($free -ge 0) { L "✓ Свободно $free ГБ (нужно около $NeedGB ГБ)" "✓ $free GB free (about $NeedGB GB needed)" } else { '✓' }
        if ($notes.Count) { Set-Status $workStatus ($msg + ' · ' + ($notes -join ' · ')) $Warn } else { Set-Status $workStatus $msg $Good }
        $script:WorkOk = $true
    }
    Update-Buttons
}

function Update-Buttons {
    $running = $script:Proc -ne $null
    $built = Is-Built
    $buildBtn.Enabled = (-not $running) -and $script:GameOk -and $script:WorkOk
    $buildBtn.Text = if ($built) { L 'Пересобрать' 'Build again' } else { L 'Собрать игру' 'Build the game' }
    $buildBtn.BackColor = if ($buildBtn.Enabled) { if ($built) { $Field } else { $Accent } } else { [Drawing.Color]::FromArgb(60, 60, 66) }
    $cancelBtn.Visible = $running
    $playBtn.Visible = $built -and -not $running
    $folderBtn.Visible = $built -and -not $running
    $gameBox.Enabled = -not $running; $gameBtn.Enabled = -not $running
    $workBox.Enabled = -not $running; $workBtn.Enabled = -not $running
}

function Pick-Folder($box, $description) {
    $dlg = New-Object Windows.Forms.FolderBrowserDialog
    $dlg.Description = $description
    $dlg.ShowNewFolderButton = $true
    if ($box.Text -and (Test-Path -LiteralPath $box.Text.Trim('"'))) { $dlg.SelectedPath = $box.Text.Trim('"') }
    if ($dlg.ShowDialog($form) -eq 'OK') { $box.Text = $dlg.SelectedPath; return $true }
    $false
}

$gameBtn.Add_Click({ if (Pick-Folder $gameBox (L 'Папка с распакованной игрой (code, content, meta)' 'The extracted game folder (code, content, meta)')) { Check-Game } })
$workBtn.Add_Click({ if (Pick-Folder $workBox (L 'Папка, куда собрать игру' 'Where to build the game')) { Check-Work } })
$gameBox.Add_Leave({ Check-Game })
$workBox.Add_Leave({ Check-Work })
# dragging a folder from Explorer onto the window fills the game field
$form.AllowDrop = $true
$form.Add_DragEnter({ param($s, $e) if ($e.Data.GetDataPresent([Windows.Forms.DataFormats]::FileDrop)) { $e.Effect = 'Copy' } })
$form.Add_DragDrop({ param($s, $e) $p = $e.Data.GetData([Windows.Forms.DataFormats]::FileDrop); if ($p) { $gameBox.Text = $p[0]; Check-Game } })

# ---------------------------------------------------------------- the build
$script:LogPath = $null
$script:LogPos = 0
$script:Started = $null
$script:Pct = 0.0
$timer = New-Object Windows.Forms.Timer
$timer.Interval = 400

function Set-Progress([double]$pct, [string]$stage) {
    if ($pct -gt $script:Pct) { $script:Pct = [math]::Min($pct, 100) }
    $barFill.Width = [int]($barBack.Width * $script:Pct / 100)
    if ($stage) { $stageLabel.Text = $stage }
}

function Parse-Line([string]$line) {
    if ($line -match '^== downloading' -or $line -match '^Downloading Python') { Set-Progress 1 (L 'Скачиваю инструменты сборки (компилятор, CMake, Python)…' 'Downloading the build tools (compiler, CMake, Python)…') }
    elseif ($line -match '^== unpacking') { Set-Progress 3 $null }
    elseif ($line -match '^== translating') { Set-Progress 5 (L 'Перевожу код игры в C (10–20 минут)…' 'Translating the game code to C (10-20 minutes)…') }
    elseif ($line -match '^\[\d+\] file (\d+)/(\d+)') { Set-Progress (5 + 30 * [double]$Matches[1] / [double]$Matches[2]) $null }
    elseif ($line -match '^translated in' -or $line -match '^== game code already translated') { Set-Progress 35 $null }
    elseif ($line -match '^== configuring') { Set-Progress 36 (L 'Готовлю сборку…' 'Configuring…') }
    elseif ($line -match '^== compiling') { Set-Progress 37 (L 'Компилирую (20–90 минут, один раз)…' 'Compiling (20-90 minutes, once)…') }
    elseif ($line -match '^\[(\d+)/(\d+)\]') {
        $n = [double]$Matches[1]; $m = [double]$Matches[2]
        if ($m -gt 50) { Set-Progress (37 + 62 * $n / $m) ((L 'Компилирую' 'Compiling') + " — $([int]$n) / $([int]$m)") }
    }
    elseif ($line -match '^== done in') { Set-Progress 100 (L 'Готово!' 'Done!') }
}

function Read-Log {
    if (-not $script:LogPath -or -not (Test-Path -LiteralPath $script:LogPath)) { return }
    $fs = $null
    try {
        $fs = New-Object IO.FileStream($script:LogPath, 'Open', 'Read', 'ReadWrite, Delete')
        if ($fs.Length -le $script:LogPos) { return }
        $fs.Position = $script:LogPos
        $buf = New-Object byte[] ($fs.Length - $script:LogPos)
        $n = $fs.Read($buf, 0, $buf.Length)
        $last = [Array]::LastIndexOf($buf, [byte]10, $n - 1)
        if ($last -lt 0) { return }  # no complete line yet
        $script:LogPos += $last + 1
        $text = [Text.Encoding]::UTF8.GetString($buf, 0, $last + 1)
        $lines = $text -split "`r?`n"
        $show = New-Object Text.StringBuilder
        foreach ($l in $lines) {
            if (-not $l) { continue }
            Parse-Line $l
            # the compiler's per-file lines are many: the box keeps the rest
            if ($l -match '^\[\d+/\d+\] Building') { continue }
            [void]$show.AppendLine($l)
        }
        if ($show.Length) {
            if ($logBox.TextLength -gt 120000) { $logBox.Text = $logBox.Text.Substring($logBox.TextLength - 60000) }
            $logBox.AppendText($show.ToString())
        }
    } catch {} finally { if ($fs) { $fs.Dispose() } }
}

function Format-Elapsed {
    if (-not $script:Started) { return '' }
    $t = (Get-Date) - $script:Started
    '{0}:{1:00}:{2:00}' -f [int][math]::Floor($t.TotalHours), $t.Minutes, $t.Seconds
}

function Make-Shortcut {
    $port = Port-Dir
    $desk = [Environment]::GetFolderPath('Desktop')
    $ws = New-Object -ComObject WScript.Shell
    $lnk = $ws.CreateShortcut((Join-Path $desk 'Tekken Tag Tournament 2.lnk'))
    $lnk.TargetPath = Join-Path $port 'run_TTT2.bat'
    $lnk.WorkingDirectory = $port
    $ico = Join-Path $port 'TTT2.ico'
    $lnk.IconLocation = if (Test-Path -LiteralPath $ico) { "$ico,0" } else { (Join-Path $port 'TTT2.exe') + ',0' }
    $lnk.WindowStyle = 7  # the console window minimised
    $lnk.Description = 'Tekken Tag Tournament 2 (native port)'
    $lnk.Save()
}

function Finish-Build([int]$code) {
    $timer.Stop()
    Read-Log
    [void][TTT2.Power]::SetThreadExecutionState([uint32]2147483648)  # ES_CONTINUOUS: may sleep again
    $script:Proc = $null
    if ($code -eq 0 -and (Is-Built)) {
        Set-Progress 100 ((L 'Готово! Игра собрана за ' 'Done! Built in ') + (Format-Elapsed))
        $barFill.BackColor = $Good
        if ($shortcutBox.Checked) {
            try { Make-Shortcut; $logBox.AppendText((L "Ярлык создан на рабочем столе.`r`n" "Desktop shortcut created.`r`n")) }
            catch { $logBox.AppendText("Shortcut: $($_.Exception.Message)`r`n") }
        }
        Check-Work
    } elseif ($script:Cancelled) {
        $stageLabel.Text = L 'Сборка отменена. Повторный запуск продолжит с того же места.' 'Build cancelled. Running it again continues where it stopped.'
        $stageLabel.ForeColor = $Warn
    } else {
        $stageLabel.Text = L "Ошибка сборки (код $code). Подробности ниже и в файле setup.log." "The build failed (code $code). Details below and in setup.log."
        $stageLabel.ForeColor = $Bad
        $barFill.BackColor = $Bad
    }
    Update-Buttons
}

$timer.Add_Tick({
    Read-Log
    $timeLabel.Text = Format-Elapsed
    if ($script:Proc -and $script:Proc.HasExited) { Finish-Build $script:Proc.ExitCode }
})

$buildBtn.Add_Click({
    Check-Game; Check-Work
    if (-not ($script:GameOk -and $script:WorkOk)) { return }
    $game = $gameBox.Text.Trim().Trim('"')
    $work = [IO.Path]::GetFullPath($workBox.Text.Trim().Trim('"'))
    Save-Settings $game $work
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $script:LogPath = Join-Path $work 'setup.log'
    [IO.File]::WriteAllText($script:LogPath, '')
    $script:LogPos = 0
    $script:Pct = 0
    $script:Cancelled = $false
    $logBox.Clear()
    $stageLabel.ForeColor = $Text
    $barFill.BackColor = $Accent
    Set-Progress 0 (L 'Запускаю…' 'Starting…')
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $env:ComSpec
    $psi.Arguments = '/d /s /c ""' + $BuildBat + '" "' + $game + '" > "' + $script:LogPath + '" 2>&1"'
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.WorkingDirectory = $Repo
    $psi.EnvironmentVariables['TTT2_NONINTERACTIVE'] = '1'
    $psi.EnvironmentVariables['TTT2_WORK'] = $work
    $script:Proc = [Diagnostics.Process]::Start($psi)
    $script:Started = Get-Date
    [void][TTT2.Power]::SetThreadExecutionState([uint32]2147483649)  # ES_CONTINUOUS | ES_SYSTEM_REQUIRED
    Update-Buttons
    $timer.Start()
})

$cancelBtn.Add_Click({
    if (-not $script:Proc) { return }
    $sure = [Windows.Forms.MessageBox]::Show($form, (L 'Остановить сборку? Готовые шаги сохранятся.' 'Stop the build? Finished steps are kept.'), $form.Text, 'YesNo', 'Question')
    if ($sure -ne 'Yes') { return }
    $script:Cancelled = $true
    & taskkill.exe /PID $script:Proc.Id /T /F | Out-Null
})

$playBtn.Add_Click({
    $port = Port-Dir
    Start-Process -FilePath (Join-Path $port 'run_TTT2.bat') -WorkingDirectory $port -WindowStyle Minimized
})
$folderBtn.Add_Click({ Start-Process explorer.exe -ArgumentList ('"' + (Port-Dir) + '"') })

$form.Add_FormClosing({
    param($s, $e)
    if ($script:Proc -and -not $script:Proc.HasExited) {
        $r = [Windows.Forms.MessageBox]::Show($form, (L 'Сборка ещё идёт. Прервать её и закрыть окно?' 'The build is still running. Stop it and close?'), $form.Text, 'YesNo', 'Warning')
        if ($r -ne 'Yes') { $e.Cancel = $true; return }
        & taskkill.exe /PID $script:Proc.Id /T /F | Out-Null
    }
    [void][TTT2.Power]::SetThreadExecutionState([uint32]2147483648)
})

# ---------------------------------------------------------------- start
$settings = Load-Settings
$workBox.Text = if ($settings['work']) { $settings['work'] } else { Join-Path $Repo 'build-ttt2' }
if ($settings['game']) { $gameBox.Text = $settings['game'] }
else {
    # a dump next to this folder or in Downloads, as a guess
    foreach ($guess in @((Split-Path $Repo), (Join-Path $env:USERPROFILE 'Downloads'))) {
        $g = Find-Game $guess
        if ($g) { $gameBox.Text = $g; break }
    }
}
$form.Add_Shown({
    Check-Work
    if ($gameBox.Text) { Check-Game } else { Set-Status $gameStatus (L 'Укажите папку с игрой или перетащите её в это окно.' 'Choose the game folder, or drag it onto this window.') $Dim }
    if (Is-Built) { $stageLabel.Text = L 'Игра уже собрана.' 'The game is already built.' }
    Update-Buttons
})
[void]$form.ShowDialog()
