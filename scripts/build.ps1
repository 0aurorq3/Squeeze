param([switch]$RebuildEngine,[string]$ToolRoot = (Join-Path $env:LOCALAPPDATA 'SqueezeBuild'))
$ErrorActionPreference = 'Stop'
$taskProject = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if ($RebuildEngine -or -not (Test-Path -LiteralPath (Join-Path $taskProject 'generated\ffmpeg.exe'))) {
    $taskBash = Join-Path $ToolRoot 'msys64\usr\bin\bash.exe'
    if (-not (Test-Path -LiteralPath $taskBash)) { throw 'Prepare MSYS2 MINGW64 in ToolRoot; see docs/BUILD.md.' }
    $env:MSYSTEM = 'MINGW64'
    $env:CHERE_INVOKING = '1'
    $env:SQUEEZE_BUILD_ROOT = $ToolRoot
    $env:SQUEEZE_ENGINE_SCRIPT = Join-Path $PSScriptRoot 'build-engine.sh'
    & $taskBash -lc 'bash "$(cygpath -u "$SQUEEZE_ENGINE_SCRIPT")"'
    if ($LASTEXITCODE -ne 0) { throw 'Engine build failed' }
}
& (Join-Path $PSScriptRoot 'pack-engine.ps1')
& (Join-Path $PSScriptRoot 'build-native.cmd')
if ($LASTEXITCODE -ne 0) { throw 'Application build failed' }
& (Join-Path $taskProject 'build\SqueezeTests.exe')
if ($LASTEXITCODE -ne 0) { throw 'Core checks failed' }
$taskDist = Join-Path $taskProject 'dist'
New-Item -ItemType Directory -Path $taskDist -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $taskProject 'build\Squeeze.exe') -Destination (Join-Path $taskDist 'Squeeze.exe') -Force
Get-Item -LiteralPath (Join-Path $taskDist 'Squeeze.exe') | Select-Object Name,Length
