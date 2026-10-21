param(
    [ValidateSet('tiny', 'tiny.en', 'base', 'base.en', 'small', 'small.en', 'medium', 'large-v3-turbo')]
    [string]$Whisper = 'base',
    [string]$Dest = (Join-Path $PSScriptRoot '..\models')
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

New-Item -ItemType Directory -Force -Path $Dest | Out-Null
$Dest = (Resolve-Path $Dest).Path

$fileName = "ggml-$Whisper.bin"
$filePath = Join-Path $Dest $fileName
$partPath = "$filePath.part"

if ((Test-Path $filePath) -and (Get-Item $filePath).Length -gt 1MB) {
    Write-Host "Already present: $filePath"
} else {
    Write-Host "Downloading $fileName ..."
    try {
        Invoke-WebRequest -UseBasicParsing -Uri "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/$fileName" -OutFile $partPath
        Move-Item -Force -Path $partPath -Destination $filePath
    } catch {
        Remove-Item -Force -Path $partPath -ErrorAction SilentlyContinue
        throw
    }
    $sizeMb = [math]::Round((Get-Item $filePath).Length / 1MB, 1)
    Write-Host "$filePath ($sizeMb MB)"
}

Write-Host "LLM fallback uses Ollama's local model files (llamacpp.model = 'ollama:<name>') or any GGUF path; no download needed."
