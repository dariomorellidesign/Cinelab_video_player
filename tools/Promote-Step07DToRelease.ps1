#requires -Version 7.0
[CmdletBinding()]
param()

$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$source=Join-Path $root 'build\step07d-player\runtime'
$release=Join-Path $root 'build\Release'
$required=@('DLSSVideoPlayer.exe','ffmpeg.exe','ffprobe.exe')
foreach($name in $required){if(!(Test-Path -LiteralPath (Join-Path $source $name))){throw "Validated Step07D runtime is missing $name."}}

# Recreate a clean canonical runtime from the validated Step07D executable.
if(Test-Path -LiteralPath $release){Remove-Item -LiteralPath $release -Recurse -Force}
New-Item -ItemType Directory -Path $release | Out-Null
foreach($name in $required){Copy-Item -LiteralPath (Join-Path $source $name) -Destination (Join-Path $release $name)}
$officialDlss=Join-Path $root 'external\DLSS\lib\Windows_x86_64\rel\nvngx_dlss.dll'
if(!(Test-Path -LiteralPath $officialDlss)){throw 'Official NVIDIA DLSS runtime is required at external\\DLSS before promotion.'}
Copy-Item -LiteralPath $officialDlss -Destination (Join-Path $release 'nvngx_dlss.dll')
Copy-Item -LiteralPath (Join-Path $root 'languages') -Destination (Join-Path $release 'languages') -Recurse
foreach($name in @('README.md','LICENSE','THIRD_PARTY.md','CHANGELOG.md')){Copy-Item -LiteralPath (Join-Path $root $name) -Destination $release}
Copy-Item -LiteralPath (Join-Path $root 'docs') -Destination (Join-Path $release 'docs') -Recurse
$hash=(Get-FileHash -LiteralPath (Join-Path $release 'DLSSVideoPlayer.exe') -Algorithm SHA256).Hash
@("CINELAB_RELEASE_RUNTIME=1","SOURCE_RUNTIME=step07d-player","EXE_SHA256=$hash") | Set-Content -LiteralPath (Join-Path $release 'RELEASE_RUNTIME.txt') -Encoding utf8
Write-Host "Promoted Step07D to $release"
Write-Host "EXE_SHA256=$hash"
