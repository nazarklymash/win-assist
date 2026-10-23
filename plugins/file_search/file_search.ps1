[Console]::InputEncoding = New-Object System.Text.UTF8Encoding $false
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding $false
$ErrorActionPreference = 'Stop'

function Reply($status, $display) {
    [Console]::Out.WriteLine((@{ status = $status; display = $display } | ConvertTo-Json -Compress))
    exit 0
}

$request = [Console]::In.ReadLine() | ConvertFrom-Json
$query = [string]$request.parameters.query
if ([string]::IsNullOrWhiteSpace($query)) { Reply 'error' 'No search query given.' }
$query = $query.Trim() -replace '\s+(file|document|report|pdf)$', ''

$roots = @(
    [Environment]::GetFolderPath('UserProfile') + '\Downloads',
    [Environment]::GetFolderPath('MyDocuments'),
    [Environment]::GetFolderPath('Desktop'),
    "$env:APPDATA\Microsoft\Windows\Recent"
)

# Prefer Everything CLI when installed (voidtools).
$es = Get-Command es.exe -ErrorAction SilentlyContinue
if ($es) {
    $hit = & es.exe -n 1 -path $query 2>$null | Select-Object -First 1
    if ($hit -and (Test-Path -LiteralPath $hit)) {
        Start-Process -FilePath $hit
        Reply 'ok' "Opening $(Split-Path $hit -Leaf)"
    }
}

$needle = $query.ToLowerInvariant()
$candidates = foreach ($root in $roots) {
    if (-not (Test-Path -LiteralPath $root)) { continue }
    Get-ChildItem -LiteralPath $root -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName.Length -lt 240 } |
        Select-Object -First 400 |
        ForEach-Object {
            $name = $_.Name.ToLowerInvariant()
            $score = if ($name -eq $needle) { 100 }
                     elseif ($name.StartsWith($needle)) { 85 }
                     elseif ($name.Contains($needle)) { 70 }
                     elseif ($_.BaseName.ToLowerInvariant().Contains($needle)) { 55 }
                     else { 0 }
            if ($score -gt 0) {
                [PSCustomObject]@{ Path = $_.FullName; Name = $_.Name; Score = $score; Age = $_.LastWriteTimeUtc }
            }
        }
}

# Resolve .lnk targets in Recent.
$best = $candidates |
    Sort-Object -Property @{ Expression = 'Score'; Descending = $true }, @{ Expression = 'Age'; Descending = $true } |
    Select-Object -First 1

if (-not $best) { Reply 'error' "No file matching '$query'." }

$target = $best.Path
if ($target -like '*.lnk') {
    $sh = New-Object -ComObject WScript.Shell
    $resolved = $sh.CreateShortcut($target).TargetPath
    if ($resolved) { $target = $resolved }
}

if (-not (Test-Path -LiteralPath $target)) { Reply 'error' "Found a shortcut but the target is missing." }
Start-Process -FilePath $target
Reply 'ok' "Opening $($best.Name)"
