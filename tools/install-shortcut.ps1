# Puts an "AI-2048" shortcut on the Desktop, pointing at start-ai2048.bat
# and carrying the custom 2048 icon.
#
# ## Why a shortcut is required
#
# Windows cannot give a .bat file a custom icon -- the shell takes a batch
# file's icon from its file type, and there is no per-file icon slot. Only a
# .lnk (or an .exe) can carry one. So the "start button" with the 2048 icon
# has to be a shortcut.
#
# ## Usage
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\install-shortcut.ps1
#
# Optional: -Desktop <path> to install somewhere other than the Desktop
# (useful for testing, and for Start Menu installs).
#
# ASCII-only on purpose: Windows PowerShell 5.1 reads .ps1 as ANSI unless it
# has a UTF-8 BOM, so non-ASCII here would break under some code pages.

param(
    [string]$Desktop = [Environment]::GetFolderPath('Desktop')
)

$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
$Target = Join-Path $RepoRoot 'start-ai2048.bat'
$Icon = Join-Path $RepoRoot 'assets\icon\ai2048.ico'
$LinkName = 'AI-2048.lnk'
$LinkPath = Join-Path $Desktop $LinkName

if (-not (Test-Path $Target)) { throw "Not found: $Target" }
if (-not (Test-Path $Icon)) {
    throw "Icon not found: $Icon  (run tools\make-icon.ps1 first)"
}

$shell = New-Object -ComObject WScript.Shell
$sc = $shell.CreateShortcut($LinkPath)

# Point at cmd.exe explicitly rather than the .bat directly.
#
# A .lnk whose target is a .bat works, but it inherits whatever console the
# shell hands it, and there is no supported way to pass arguments through
# cleanly later. Going via cmd.exe keeps the window, the working directory and
# any future flags under our control.
$sc.TargetPath = "$env:SystemRoot\System32\cmd.exe"
$sc.Arguments = '/c "' + $Target + '"'
$sc.WorkingDirectory = $RepoRoot
$sc.IconLocation = "$Icon,0"
$sc.Description = 'AI-2048 -- play against the C++ engine'
$sc.WindowStyle = 1
$sc.Save()

Write-Host ''
Write-Host ("Shortcut created: {0}" -f $LinkPath)
Write-Host ("  target : {0}" -f $sc.TargetPath)
Write-Host ("  args   : {0}" -f $sc.Arguments)
Write-Host ("  icon   : {0}" -f $sc.IconLocation)

# Read it back so the report reflects what is actually on disk rather than
# what we intended to write.
$check = $shell.CreateShortcut($LinkPath)
Write-Host ''
Write-Host 'Verified by re-reading the .lnk:'
Write-Host ("  target : {0}" -f $check.TargetPath)
Write-Host ("  icon   : {0}" -f $check.IconLocation)
if (Test-Path $check.IconLocation.Split(',')[0]) {
    Write-Host '  icon file exists: yes'
} else {
    Write-Host '  icon file exists: NO -- the shortcut will show a default icon'
}
