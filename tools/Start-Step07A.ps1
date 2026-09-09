[CmdletBinding()]
param(
    [string]$VideoPath='X:\MOVIES\FILM\[1080p] The Exorcist\The.Exorcist.1973.1080p.BrRip.x264.bitloks.YIFY.mp4',
    [ValidateSet('dlaa','quality','balanced','performance','ultra-performance')][string]$Quality='dlaa',
    [ValidatePattern('^\d+x\d+$')][string]$OutputSize='1918x1080',
    [ValidateSet('gpu','reference')][string]$MotionPipeline='gpu',
    [ValidateSet('auto','native','third')][string]$DecodeResolution='auto',
    [ValidateSet('temporal','current')][string]$DLSSHistory='temporal',
    [ValidatePattern('^step[0-9a-z]+$')][string]$StepName='step07a'
)
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$runtime=Join-Path $repo ('build\'+$StepName+'-player\runtime')
$exe=Join-Path $runtime 'CineLabVideoPlayer.exe'
foreach($path in @($VideoPath,$exe)){if(-not(Test-Path -LiteralPath $path -PathType Leaf)){throw "File non trovato: $path"}}
if($VideoPath.Contains('"')){throw 'Il percorso contiene un carattere non valido.'}
$running=@(Get-Process -Name CineLabVideoPlayer -ErrorAction SilentlyContinue | Where-Object {$_.Path -eq $exe})
if($running.Count){throw "La versione $StepName e gia aperta. Chiuderla prima di avviare un nuovo test."}
# The player truncates its log at startup. Preserve the preceding session first.
$archive=Join-Path $repo ('build\'+$StepName+'-validation\sessions\'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $archive -Force | Out-Null
foreach($name in @('CineLabVideoPlayer.log','ReShade.log',($StepName+'-runtime.txt'))) {
    $path=Join-Path $runtime $name
    if(Test-Path -LiteralPath $path){Copy-Item -LiteralPath $path -Destination (Join-Path $archive $name)}
}
# This is the user's interactive playback launcher; keep its window visible.
$args=@('--motion-pipeline',$MotionPipeline,'--depth-source','flat','--quality',$Quality,'--output',$OutputSize)
if($DecodeResolution -ne 'auto'){$args+=@('--decode-resolution',$DecodeResolution)}
if($DLSSHistory -eq 'current'){$args+=@('--dlss-history','current')}
$args+=('"'+$VideoPath+'"')
$player=Start-Process -FilePath $exe -ArgumentList $args -WorkingDirectory $runtime -PassThru
Write-Host "Player avviato (PID $($player.Id)). Log precedenti: $archive"
