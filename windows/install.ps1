# Spyro: A Hero's Tail PC port - Windows installer (WSL2).
# Started by Install-Spyro.bat. Run it again any time to update the port; your saves are kept.
#   1. installs WSL2 + Ubuntu if you don't have them (a restart may be needed, then run it again)
#   2. asks for your game: an ISO, or a folder with the extracted files
#   3. builds the port inside Ubuntu, extracts your game there and makes a desktop shortcut
# Continue, not Stop: Windows PowerShell 5.1 turns a native program's stderr into errors under Stop
$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName System.Windows.Forms

function Fail($msg) { Write-Host ""; Write-Host $msg -ForegroundColor Red; Read-Host "Press Enter to close"; exit 1 }

# wsl.exe prints UTF-16; read it as that
function Get-Distros {
    $old = [Console]::OutputEncoding
    try {
        [Console]::OutputEncoding = [System.Text.Encoding]::Unicode
        $out = & wsl.exe --list --quiet
    } finally { [Console]::OutputEncoding = $old }
    if ($LASTEXITCODE -ne 0) { return @() }
    return @($out | ForEach-Object { ($_ -replace "`0", '').Trim() } | Where-Object { $_ })
}

Write-Host "Spyro: A Hero's Tail - PC port setup" -ForegroundColor Cyan
Write-Host ""

if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
    Fail "WSL isn't available on this Windows. It needs Windows 10 (version 2004 or newer) or Windows 11."
}

$distro = Get-Distros | Where-Object { $_ -like 'Ubuntu*' } | Select-Object -First 1
if (-not $distro) {
    Write-Host "Installing WSL2 and Ubuntu. Windows will ask for administrator rights."
    Start-Process wsl.exe -ArgumentList '--install', '-d', 'Ubuntu' -Verb RunAs -Wait
    Write-Host ""
    Write-Host "Next:" -ForegroundColor Yellow
    Write-Host "  1. Restart Windows if it asks you to."
    Write-Host "  2. Open 'Ubuntu' from the Start menu once and pick a user name and password."
    Write-Host "  3. Run Install-Spyro.bat again."
    Read-Host "Press Enter to close"
    exit 0
}
Write-Host "Using WSL distro: $distro"

# the game
$dlg = New-Object System.Windows.Forms.OpenFileDialog
$dlg.Title = "Pick your Spyro: A Hero's Tail (Xbox, NTSC-U) disc image - or Cancel to pick a folder"
$dlg.Filter = "Xbox disc image (*.iso;*.xiso)|*.iso;*.xiso|All files (*.*)|*.*"
if ($dlg.ShowDialog() -eq 'OK') {
    $game = $dlg.FileName
} else {
    $fb = New-Object System.Windows.Forms.FolderBrowserDialog
    $fb.Description = "Pick the folder with your extracted game (the one holding default.xbe)"
    if ($fb.ShowDialog() -ne 'OK') { Fail "No game picked." }
    $game = $fb.SelectedPath
}
Write-Host "Game: $game"

$repo = Split-Path -Parent $PSScriptRoot
# Linux programs print UTF-8 (a user name with accents would otherwise come back garbled)
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
# --exec runs the program directly, so paths with spaces or an apostrophe arrive intact
$repoW = "$(& wsl.exe -d $distro --exec wslpath -a "$repo")".Trim()
$gameW = "$(& wsl.exe -d $distro --exec wslpath -a "$game")".Trim()
if (-not $repoW -or -not $gameW) { Fail "Couldn't turn the Windows paths into WSL paths." }

& wsl.exe -d $distro --exec bash "$repoW/windows/wsl-setup.sh" "$repoW" "$gameW"
if ($LASTEXITCODE -ne 0) { Fail "Setup stopped with an error (see above)." }

# desktop shortcut
$desktop = [Environment]::GetFolderPath('Desktop')
$ws = New-Object -ComObject WScript.Shell
$lnk = $ws.CreateShortcut((Join-Path $desktop "Spyro A Hero's Tail.lnk"))
$lnk.TargetPath = (Get-Command wsl.exe).Source
$lnk.Arguments = "-d $distro --exec bash -lc ~/spyro-port/play.sh"
$lnk.Description = "Spyro: A Hero's Tail (PC port, runs in WSL)"
$lnk.WorkingDirectory = $env:USERPROFILE
$lnk.Save()

Write-Host ""
Write-Host "All set. Start the game with 'Spyro A Hero's Tail' on your desktop." -ForegroundColor Green
Write-Host "Keyboard: WASD move, Space jump, E attack, F1 debug menu. See docs\PLAYING.md."
Read-Host "Press Enter to close"
