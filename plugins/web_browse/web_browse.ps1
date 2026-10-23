[Console]::InputEncoding = New-Object System.Text.UTF8Encoding $false
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding $false
$ErrorActionPreference = 'Stop'

function Reply($status, $display) {
    [Console]::Out.WriteLine((@{ status = $status; display = $display } | ConvertTo-Json -Compress))
    exit 0
}

function Open-Uri([string]$uri) {
    Start-Process $uri
}

$request = [Console]::In.ReadLine() | ConvertFrom-Json
$intent = [string]$request.intent
$p = $request.parameters

switch ($intent) {
    'open_url' {
        $url = [string]$p.url
        if ([string]::IsNullOrWhiteSpace($url)) { Reply 'error' 'No URL given.' }
        $url = $url.Trim()
        if ($url -notmatch '^[a-z]+://') { $url = 'https://' + $url }
        Open-Uri $url
        Reply 'ok' "Opening $url"
    }
    'web_search' {
        $q = [uri]::EscapeDataString([string]$p.query)
        Open-Uri "https://www.google.com/search?q=$q"
        Reply 'ok' "Searching the web for $($p.query)"
    }
    'youtube_search' {
        $q = [uri]::EscapeDataString([string]$p.query)
        Open-Uri "https://www.youtube.com/results?search_query=$q"
        Reply 'ok' "Searching YouTube for $($p.query)"
    }
    'open_spotify' {
        try {
            Start-Process 'spotify:'
            Reply 'ok' 'Opening Spotify'
        } catch {
            Open-Uri 'https://open.spotify.com'
            Reply 'ok' 'Opening Spotify in the browser'
        }
    }
    default { Reply 'error' "Unknown intent '$intent'" }
}
