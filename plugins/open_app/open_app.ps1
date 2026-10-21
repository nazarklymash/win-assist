[Console]::InputEncoding = New-Object System.Text.UTF8Encoding $false
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding $false
$ErrorActionPreference = 'Stop'

function Reply($status, $display) {
    [Console]::Out.WriteLine((@{ status = $status; display = $display } | ConvertTo-Json -Compress))
    exit 0
}

function Score($name, $query) {
    if ($name -ieq $query) { return 100 }
    if ($name.StartsWith($query, [StringComparison]::OrdinalIgnoreCase)) { return 80 }
    if ($name.IndexOf($query, [StringComparison]::OrdinalIgnoreCase) -ge 0) { return 60 }
    return 0
}

$request = [Console]::In.ReadLine() | ConvertFrom-Json
$appName = [string]$request.parameters.app_name
if ([string]::IsNullOrWhiteSpace($appName)) { Reply 'error' 'No application name given' }
$appName = $appName.Trim().TrimEnd('.')

# Built-in tools whose display name varies with the Windows language.
$aliases = @{
    'calculator'       = 'calc.exe'
    'calc'             = 'calc.exe'
    'notepad'          = 'notepad.exe'
    'paint'            = 'mspaint.exe'
    'settings'         = 'ms-settings:'
    'task manager'     = 'taskmgr.exe'
    'file explorer'    = 'explorer.exe'
    'explorer'         = 'explorer.exe'
    'command prompt'   = 'cmd.exe'
    'terminal'         = 'wt.exe'
    'powershell'       = 'powershell.exe'
    'control panel'    = 'control.exe'
    'snipping tool'    = 'ms-screenclip:'
    'registry editor'  = 'regedit.exe'
}
$alias = $aliases[$appName.ToLowerInvariant()]
if ($alias) {
    try {
        Start-Process -FilePath $alias
        Reply 'ok' "Opening $appName"
    } catch { }
}

# Start menu entries: covers classic shortcuts and Store apps, with localized names.
$candidates = @()
try {
    $candidates += Get-StartApps | Where-Object { $_.Name -notmatch 'uninstall' } | ForEach-Object {
        $score = Score $_.Name $appName
        if ($score -gt 0) { [PSCustomObject]@{ Target = "shell:AppsFolder\$($_.AppID)"; Name = $_.Name; Score = $score } }
    }
} catch { }

if (-not $candidates) {
    $dirs = @(
        "$env:ProgramData\Microsoft\Windows\Start Menu\Programs",
        "$env:APPDATA\Microsoft\Windows\Start Menu\Programs"
    )
    $candidates += foreach ($dir in $dirs) {
        Get-ChildItem -Path $dir -Filter *.lnk -Recurse -File -ErrorAction SilentlyContinue |
            Where-Object { $_.BaseName -notmatch 'uninstall' } |
            ForEach-Object {
                $score = Score $_.BaseName $appName
                if ($score -gt 0) { [PSCustomObject]@{ Target = $_.FullName; Name = $_.BaseName; Score = $score } }
            }
    }
}

$best = $candidates |
    Sort-Object -Property @{ Expression = 'Score'; Descending = $true }, @{ Expression = { $_.Name.Length } } |
    Select-Object -First 1

if ($best) {
    Start-Process -FilePath $best.Target
    Reply 'ok' "Opening $($best.Name)"
}

try {
    Start-Process -FilePath $appName
    Reply 'ok' "Opening $appName"
} catch {
    Reply 'error' "Could not find $appName"
}
