# Downloads Piper TTS (CPU) + a small English voice into build/<preset>/bin/tts.
# Ukrainian: pass -Voice uk_UA-ukrainian_tts-medium if published for your piper version.
param(
    [string]$Preset = 'release',
    [string]$Voice = 'en_US-lessac-medium',
    [string]$PiperVersion = '1.2.0'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root "build\$Preset\bin\tts"
New-Item -ItemType Directory -Force -Path $dest | Out-Null

$zip = Join-Path $env:TEMP "piper-$PiperVersion-windows.zip"
$url = "https://github.com/rhasspy/piper/releases/download/v$PiperVersion/piper_windows_amd64.zip"
if (-not (Test-Path (Join-Path $dest 'piper.exe'))) {
    Write-Host "Downloading Piper $PiperVersion..."
    Invoke-WebRequest -Uri $url -OutFile $zip
    Expand-Archive -Path $zip -DestinationPath $dest -Force
    # Releases nest piper.exe one folder deep sometimes.
    Get-ChildItem $dest -Recurse -Filter piper.exe | Select-Object -First 1 | ForEach-Object {
        Copy-Item $_.FullName (Join-Path $dest 'piper.exe') -Force
        $dlls = Join-Path $_.DirectoryName '*.dll'
        Copy-Item $dlls $dest -Force -ErrorAction SilentlyContinue
    }
}

$voiceBase = "https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/medium"
$onnx = Join-Path $dest "$Voice.onnx"
$json = Join-Path $dest "$Voice.onnx.json"
if (-not (Test-Path $onnx)) {
    Write-Host "Downloading voice $Voice..."
    Invoke-WebRequest -Uri "$voiceBase/en_US-lessac-medium.onnx" -OutFile $onnx
    Invoke-WebRequest -Uri "$voiceBase/en_US-lessac-medium.onnx.json" -OutFile $json
}
Write-Host "Piper ready in $dest"
Write-Host "Set tts.engine to auto (default) or piper in %APPDATA%\win-assist\config.json"
