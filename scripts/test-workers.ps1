# Smoke-tests the wa-stt and wa-llm workers directly over their pipe protocol,
# without the daemon. STT input is synthesized with the Windows speech engine.
#   powershell -ExecutionPolicy Bypass -File scripts\test-workers.ps1 [-Preset release] [-Skip stt|llm]
param(
    [string]$Preset = 'release',
    [string]$Phrase = 'Please open the calculator and set the volume to forty percent.',
    [string]$LlmModel = 'ollama:qwen3.5:4b',
    [ValidateSet('', 'stt', 'llm')][string]$Skip = ''
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Core, System.Speech

$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "build\$Preset\bin"
$utf8 = New-Object System.Text.UTF8Encoding $false

function Write-Frame($stream, [byte[]]$payload) {
    $stream.Write([BitConverter]::GetBytes([uint32]$payload.Length), 0, 4)
    $stream.Write($payload, 0, $payload.Length)
    $stream.Flush()
}

function Read-Exact($stream, [int]$count) {
    $buf = New-Object byte[] $count
    $off = 0
    while ($off -lt $count) {
        $n = $stream.Read($buf, $off, $count - $off)
        if ($n -le 0) { throw 'pipe closed' }
        $off += $n
    }
    return , $buf
}

function Read-Json($stream) {
    $len = [BitConverter]::ToUInt32((Read-Exact $stream 4), 0)
    if ($len -eq 0) { return $null }
    return $utf8.GetString((Read-Exact $stream ([int]$len))) | ConvertFrom-Json
}

function Send-Json($stream, $obj) { Write-Frame $stream $utf8.GetBytes(($obj | ConvertTo-Json -Compress -Depth 8)) }

function Start-Worker([string]$exe, [string[]]$workerArgs) {
    $name = "wa-test-$PID-$([guid]::NewGuid().ToString('N').Substring(0, 8))"
    $server = New-Object System.IO.Pipes.NamedPipeServerStream($name, [System.IO.Pipes.PipeDirection]::InOut, 1)
    $all = @('--pipe', "\\.\pipe\$name") + $workerArgs
    $proc = Start-Process -FilePath (Join-Path $bin $exe) -ArgumentList $all -NoNewWindow -PassThru `
        -RedirectStandardError "$env:TEMP\$name.err"
    $server.WaitForConnection()
    $ready = Read-Json $server
    if (-not $ready.ok) { throw "$exe failed to start: $($ready.error)" }
    return @{ Pipe = $server; Process = $proc }
}

function Stop-Worker($w) {
    try { Send-Json $w.Pipe @{ op = 'shutdown' } } catch { }
    $w.Pipe.Dispose()
    if (-not $w.Process.WaitForExit(5000)) { $w.Process.Kill() }
}

function Resolve-OllamaModel([string]$spec) {
    if (-not $spec.StartsWith('ollama:')) { return $spec }
    $name = $spec.Substring(7)
    $tag = 'latest'
    if ($name.Contains(':')) { $tag = $name.Split(':')[1]; $name = $name.Split(':')[0] }
    if (-not $name.Contains('/')) { $name = "library/$name" }
    $store = if ($env:OLLAMA_MODELS) { $env:OLLAMA_MODELS } else { Join-Path $env:USERPROFILE '.ollama\models' }
    $manifest = Get-Content (Join-Path $store "manifests\registry.ollama.ai\$name\$tag") -Raw | ConvertFrom-Json
    $layer = $manifest.layers | Where-Object { $_.mediaType -eq 'application/vnd.ollama.image.model' } | Select-Object -First 1
    return Join-Path $store ('blobs\' + $layer.digest.Replace(':', '-'))
}

$failed = $false

if ($Skip -ne 'stt') {
    Write-Host '== wa-stt ==' -ForegroundColor Cyan
    $wav = Join-Path $env:TEMP 'wa-test-speech.wav'
    $synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
    $format = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(16000, [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, [System.Speech.AudioFormat.AudioChannel]::Mono)
    $synth.SetOutputToWaveFile($wav, $format)
    $synth.Speak($Phrase)
    $synth.Dispose()

    # 16-bit PCM WAV -> float32 samples.
    $bytes = [IO.File]::ReadAllBytes($wav)
    $pos = 12
    while ([Text.Encoding]::ASCII.GetString($bytes, $pos, 4) -ne 'data') {
        $pos += 8 + [BitConverter]::ToInt32($bytes, $pos + 4)
    }
    $start = $pos + 8
    $samples = [int]([BitConverter]::ToInt32($bytes, $pos + 4) / 2)
    $pcm = New-Object byte[] ($samples * 4)
    for ($i = 0; $i -lt $samples; $i++) {
        $f = [float]([BitConverter]::ToInt16($bytes, $start + 2 * $i) / 32768.0)
        [BitConverter]::GetBytes($f).CopyTo($pcm, 4 * $i)
    }

    $model = Join-Path $bin 'models\ggml-base.bin'
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $w = Start-Worker 'wa-stt.exe' @('--model', $model, '--gpu', '0')
    $load = $sw.ElapsedMilliseconds
    Send-Json $w.Pipe @{ op = 'transcribe'; language = 'en'; n_samples = $samples }
    Write-Frame $w.Pipe $pcm
    $reply = Read-Json $w.Pipe
    $rss = [math]::Round($w.Process.WorkingSet64 / 1MB)
    Stop-Worker $w
    Write-Host ("load {0} ms, transcribe {1} ms, {2:N1} s audio, worker RSS {3} MB" -f $load, $reply.ms, ($samples / 16000), $rss)
    Write-Host "spoken : $Phrase"
    Write-Host "decoded: $($reply.text)"
    if (-not $reply.ok -or -not $reply.text) { $failed = $true; Write-Host 'STT FAILED' -ForegroundColor Red }
}

if ($Skip -ne 'llm') {
    Write-Host '== wa-llm ==' -ForegroundColor Cyan
    $gguf = Resolve-OllamaModel $LlmModel
    Write-Host "model: $gguf"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $w = Start-Worker 'wa-llm.exe' @('--model', $gguf, '--ctx', '2048', '--gpu-layers', '0')
    $load = $sw.ElapsedMilliseconds

    $sw.Restart()
    Send-Json $w.Pipe @{ op = 'chat'; max_tokens = 64; json = $false; stream = $true; messages = @(
            @{ role = 'system'; content = 'Answer in one short sentence.' },
            @{ role = 'user'; content = 'What is the capital of France?' }) }
    $deltas = 0
    do {
        $msg = Read-Json $w.Pipe
        if ($msg.delta) { $deltas++ }
    } until ($msg.done)
    $chatMs = $sw.ElapsedMilliseconds
    Write-Host "load $load ms, chat $chatMs ms, $deltas streamed deltas"
    Write-Host "text : $($msg.text)"
    if (-not $msg.ok -or -not $msg.text) { $failed = $true; Write-Host "LLM chat FAILED: $($msg.error)" -ForegroundColor Red }

    $sw.Restart()
    Send-Json $w.Pipe @{ op = 'chat'; max_tokens = 96; json = $true; stream = $false; messages = @(
            @{ role = 'system'; content = 'Reply with JSON only: {"tool": name, "arguments": {...}} to call a tool, or {"reply": text}. Tools: set_volume(level: integer).' },
            @{ role = 'user'; content = 'make the volume 30 percent' }) }
    $msg = Read-Json $w.Pipe
    Write-Host "json ($($sw.ElapsedMilliseconds) ms): $($msg.text)"
    try { $null = $msg.text | ConvertFrom-Json } catch { $failed = $true; Write-Host 'LLM JSON mode produced invalid JSON' -ForegroundColor Red }
    Stop-Worker $w
}

if ($failed) { exit 1 }
Write-Host 'workers OK' -ForegroundColor Green
