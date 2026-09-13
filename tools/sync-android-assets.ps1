# Sync web/ front-end into the Android APK assets folder.
#
# Why this script exists (instead of typing Copy-Item by hand):
# The APK uses android/app/src/main/assets/web/, a plain COPY of web/ with no
# automatic link. Forgetting to sync means "desktop updated, APK still old, and
# nothing reports an error" -- this actually happened: the style.css inside the
# APK was stuck at 11869 bytes while web/ was already 33 KB, so the packaged app
# still showed a UI from several rounds earlier.
#
# Usage (from the project root):
#     powershell -NoProfile -ExecutionPolicy Bypass -File tools\sync-android-assets.ps1
#
# This script only copies and verifies. It does NOT build.
# Build command: see android/README.md
#
# NOTE: keep this file pure ASCII, like every other .ps1 in this repo.
# Windows PowerShell 5.1 reads .ps1 as ANSI, so non-ASCII turns into mojibake
# and breaks the parser.

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$src = Join-Path $root 'web'
$dst = Join-Path $root 'android\app\src\main\assets\web'

if (-not (Test-Path $src)) { throw "Front-end folder not found: $src" }
if (-not (Test-Path $dst)) { throw "APK assets folder not found: $dst" }

Copy-Item (Join-Path $src '*') -Destination $dst -Recurse -Force

# web/README.md is developer documentation; it must not ship inside the APK.
$doc = Join-Path $dst 'README.md'
if (Test-Path $doc) { Remove-Item $doc -Force }

# Verify file by file. "The command did not fail" and "the content matches"
# are two different things.
$mismatch = @()
$checked = 0
Get-ChildItem $src -Recurse -File | Where-Object { $_.Name -ne 'README.md' } | ForEach-Object {
    $rel = $_.FullName.Substring($src.Length + 1)
    $target = Join-Path $dst $rel
    $checked++
    if (-not (Test-Path $target)) {
        $mismatch += "$rel (missing on APK side)"
    } elseif ((Get-Item $target).Length -ne $_.Length) {
        $mismatch += "$rel (size differs: web=$($_.Length) assets=$((Get-Item $target).Length))"
    }
}

if ($mismatch.Count -gt 0) {
    Write-Host "SYNC FAILED, these files do not match:" -ForegroundColor Red
    $mismatch | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    exit 1
}

Write-Host "Synced $checked files to android/app/src/main/assets/web/" -ForegroundColor Green
Write-Host "Next: rebuild the APK (command in android/README.md)" -ForegroundColor Yellow
