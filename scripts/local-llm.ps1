# Offload small generation to local Ollama (default: huihui_ai/qwen3-abliterated:8b).
# Used by agents following the win-assist skill вЂ” not by the daemon itself.
param(
    [Parameter(Mandatory, ParameterSetName = 'File')][string]$PromptFile,
    [Parameter(Mandatory, ParameterSetName = 'Inline')][string]$Prompt,
    [string]$OutFile,
    [string]$Model = 'huihui_ai/qwen3-abliterated:8b',
    [string]$OllamaHost = 'http://127.0.0.1:11434',
    [double]$Temperature = 0.2,
    [int]$NumCtx = 8192,
    [int]$TimeoutSec = 600,
    # Keep <think> blocks and code fences as the model emitted them.
    [switch]$Raw
)
$ErrorActionPreference = 'Stop'

$text = if ($PSCmdlet.ParameterSetName -eq 'File') { Get-Content -Raw -Encoding UTF8 $PromptFile } else { $Prompt }
$body = @{
    model   = $Model
    prompt  = "/no_think`n" + $text
    stream  = $false
    options = @{ temperature = $Temperature; num_ctx = $NumCtx }
} | ConvertTo-Json -Depth 5

# Ollama must be up; starting it is cheap and idempotent, but only makes sense
# when we are talking to a local server.
$isLocal = $OllamaHost -match '://(127\.0\.0\.1|localhost)(:|/|$)'
try {
    Invoke-RestMethod -Uri "$OllamaHost/api/version" -TimeoutSec 3 | Out-Null
} catch {
    if (-not $isLocal) { throw }
    Start-Process -WindowStyle Hidden -FilePath 'ollama' -ArgumentList 'serve' -ErrorAction SilentlyContinue
    for ($i = 0; $i -lt 30; $i++) {
        Start-Sleep -Milliseconds 500
        try { Invoke-RestMethod -Uri "$OllamaHost/api/version" -TimeoutSec 3 | Out-Null; break } catch {}
    }
}

$resp = Invoke-RestMethod -Uri "$OllamaHost/api/generate" -Method Post `
    -Body ([Text.Encoding]::UTF8.GetBytes($body)) `
    -ContentType 'application/json; charset=utf-8' -TimeoutSec $TimeoutSec

$out = $resp.response
if (-not $Raw) {
    $out = $out -replace '(?s)<think>.*?</think>', ''
    $out = $out.Trim()
    if ($out -match '(?s)^```[a-zA-Z0-9]*\r?\n(.*?)\r?\n```$') { $out = $Matches[1] }
}
$out = $out.Trim()

if ($OutFile) {
    $dir = Split-Path -Parent $OutFile
    if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    # Write a BOM: PowerShell 5.1 reads BOM-less UTF-8 as ANSI and mangles Cyrillic.
    [IO.File]::WriteAllText($OutFile, $out + "`n", (New-Object Text.UTF8Encoding $true))
    "wrote $OutFile ($($out.Length) chars)"
} else {
    $out
}

