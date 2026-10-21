param(
    [ValidateSet('release', 'debug', 'release-cuda')]
    [string]$Preset = 'release',
    [switch]$Test,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')

function Import-VsDevEnv {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) { return }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'Visual Studio with the C++ x64 toolset was not found.' }
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    cmd /c "`"$vcvars`" >nul && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2]) }
    }
    # Keep MinGW/MSYS toolchains from shadowing MSVC's cmake/ninja/link.
    $env:Path = ($env:Path -split ';' | Where-Object { $_ -notmatch 'mingw|msys' }) -join ';'
}

Import-VsDevEnv
Push-Location $root
try {
    if ($Clean) { Remove-Item -Recurse -Force "build\$Preset" -ErrorAction SilentlyContinue }
    cmake --preset $Preset
    if ($LASTEXITCODE) { throw "configure failed" }
    cmake --build --preset $Preset
    if ($LASTEXITCODE) { throw "build failed" }
    if ($Test) {
        ctest --preset ($Preset -replace '-cuda', '')
        if ($LASTEXITCODE) { throw "tests failed" }
    }
    Write-Host "Output: $root\build\$Preset\bin"
} finally {
    Pop-Location
}
