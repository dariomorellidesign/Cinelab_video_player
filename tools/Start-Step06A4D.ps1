[CmdletBinding()]
param(
    [string]$VideoPath='X:\MOVIES\FILM\[1080p] The Exorcist\The.Exorcist.1973.1080p.BrRip.x264.bitloks.YIFY.mp4',
    [ValidateSet('dlaa','quality','balanced','performance','ultra-performance')][string]$Quality='dlaa',
    [ValidatePattern('^\d+x\d+$')][string]$OutputSize='1918x1080'
)
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$runtime=Join-Path $repo 'build\step06a4d-player\runtime'
$exe=Join-Path $runtime 'CineLabVideoPlayer.exe'
foreach($path in @($VideoPath,$exe)){if(-not(Test-Path -LiteralPath $path -PathType Leaf)){throw "File non trovato: $path"}}
if($VideoPath.Contains('"')){throw 'Il percorso contiene un carattere non valido.'}
$running=@(Get-Process -Name CineLabVideoPlayer -ErrorAction SilentlyContinue | Where-Object {$_.Path -eq $exe})
if($running.Count){throw 'La versione Step06A4D e gia aperta. Chiuderla prima di avviare un nuovo test.'}
# The player truncates its log at startup. Preserve the preceding session first.
$archive=Join-Path $repo ('build\step06a4d-validation\sessions\'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $archive -Force | Out-Null
foreach($name in @('CineLabVideoPlayer.log','ReShade.log','step06a4d-runtime.txt')) {
    $path=Join-Path $runtime $name
    if(Test-Path -LiteralPath $path){Copy-Item -LiteralPath $path -Destination (Join-Path $archive $name)}
}
# This is the user's interactive playback launcher; keep its window visible.
$player=Start-Process -FilePath $exe -ArgumentList @('--quality',$Quality,'--output',$OutputSize,('"'+$VideoPath+'"')) -WorkingDirectory $runtime -PassThru
Write-Host "Player avviato (PID $($player.Id)). Log precedenti: $archive"
