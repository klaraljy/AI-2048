# Replace WEIGHT_TABLE in web/js/game.js with the freshly exported tables.
#
# Why a script instead of hand-editing: the table has 1301 entries. A single
# typo silently changes the spawn distribution, and a wrong spawn distribution
# invalidates every benchmark. Scripted replacement + entry-count verification
# is the only reliable way.
#
# NOTE: keep this file PURE ASCII. Windows PowerShell 5.1 reads .ps1 as ANSI,
# so non-ASCII characters get mangled and break parsing.
param(
  [string]$GameJs = 'E:\DeepSeekProjects\AI-2048\web\js\game.js',
  [string]$TableFile = 'E:\DeepSeekProjects\_tmp\AI-2048\weight-table-new.txt'
)

$utf8 = New-Object System.Text.UTF8Encoding($false)

# 1) Extract the EASY / HARD number sequences from the exported file.
$lines = [System.IO.File]::ReadAllLines($TableFile, $utf8)
$tables = @{}
$current = $null
foreach ($line in $lines) {
  if ($line -match '^const (\w+)_WEIGHT_TABLE = \[') {
    $current = $Matches[1]
    $tables[$current] = New-Object System.Collections.ArrayList
    continue
  }
  if ($null -eq $current) { continue }
  if ($line -match '^\];') { $current = $null; continue }
  foreach ($m in [regex]::Matches($line, '-?\d+')) {
    [void]$tables[$current].Add([int64]$m.Value)
  }
}

foreach ($k in @('EASY', 'HARD')) {
  if (-not $tables.ContainsKey($k)) { throw "table $k not found in export file" }
  Write-Host "$k entries = $($tables[$k].Count)"
}

# 2) Render JS literals, 16 per line (same style as the original).
function Format-Table([System.Collections.ArrayList]$values) {
  $sb = New-Object System.Text.StringBuilder
  for ($i = 0; $i -lt $values.Count; $i++) {
    if ($i % 16 -eq 0) { [void]$sb.Append('  ') }
    [void]$sb.Append($values[$i])
    [void]$sb.Append(',')
    if ($i % 16 -eq 15) { [void]$sb.Append("`n") }
  }
  if ($values.Count % 16 -ne 0) { [void]$sb.Append("`n") }
  return $sb.ToString().TrimEnd("`n")
}

$newTables = @"
const EASY_WEIGHT_TABLE = [
$(Format-Table $tables['EASY'])
];
const HARD_WEIGHT_TABLE = [
$(Format-Table $tables['HARD'])
];
"@

# 3) Locate and replace: from `const WEIGHT_TABLE = [` to the first `];`.
$text = [System.IO.File]::ReadAllText($GameJs, $utf8)
$pattern = '(?s)const WEIGHT_TABLE = \[.*?\n\];'
if (-not [regex]::IsMatch($text, $pattern)) { throw 'WEIGHT_TABLE block not found in game.js' }
$oldLen = [regex]::Match($text, $pattern).Length
$text = [regex]::Replace($text, $pattern, { param($m) $newTables }, 1)
[System.IO.File]::WriteAllText($GameJs, $text, $utf8)
Write-Host "replaced (old block was $oldLen chars)"

# 4) Verify: both tables present, entry counts match the export.
$check = [System.IO.File]::ReadAllText($GameJs, $utf8)
foreach ($k in @('EASY', 'HARD')) {
  $m = [regex]::Match($check, "(?s)const ${k}_WEIGHT_TABLE = \[(.*?)\n\];")
  if (-not $m.Success) { throw "${k}_WEIGHT_TABLE not found after replace" }
  $n = [regex]::Matches($m.Groups[1].Value, '-?\d+').Count
  $expect = $tables[$k].Count
  if ($n -ne $expect) { throw "${k} count mismatch: file has $n, expected $expect" }
  Write-Host "${k} verified ($n entries)"
}
Write-Host 'done'
