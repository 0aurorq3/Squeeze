$ErrorActionPreference = 'Stop'
$taskProject = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$taskStage = Join-Path $taskProject ('build\source-package-' + [Guid]::NewGuid().ToString('N'))
$taskDestination = Join-Path $taskProject 'source'
New-Item -ItemType Directory -Path $taskStage,$taskDestination -Force | Out-Null
foreach ($taskName in @('src','tests','scripts','assets','docs','third_party','README.md','LICENSE','CMakeLists.txt','.clang-format','.gitattributes','.gitignore')) {
    Copy-Item -LiteralPath (Join-Path $taskProject $taskName) -Destination $taskStage -Recurse -Force
}
New-Item -ItemType Directory -Path (Join-Path $taskStage 'generated') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $taskProject 'generated\payload.hpp') -Destination (Join-Path $taskStage 'generated')
Add-Type -AssemblyName System.IO.Compression.FileSystem
$taskArchive = Join-Path $taskDestination 'Squeeze-1.0-source.zip'
if (Test-Path -LiteralPath $taskArchive) { Remove-Item -LiteralPath $taskArchive }
[IO.Compression.ZipFile]::CreateFromDirectory($taskStage,$taskArchive,[IO.Compression.CompressionLevel]::Optimal,$false)
Get-Item -LiteralPath $taskArchive | Select-Object Name,Length
