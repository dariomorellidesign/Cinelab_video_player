[CmdletBinding()]
param(
    [string]$VideoPath='X:\MOVIES\FILM\[1080p] The Exorcist\The.Exorcist.1973.1080p.BrRip.x264.bitloks.YIFY.mp4',
    [ValidateSet('dlaa','quality','balanced','performance','ultra-performance')][string]$Quality='dlaa',
    [ValidatePattern('^\d+x\d+$')][string]$OutputSize='1918x1080',
    [ValidateSet('gpu','reference')][string]$MotionPipeline='gpu',
    [ValidateSet('auto','native','third')][string]$DecodeResolution='auto',
    [ValidateSet('temporal','current')][string]$DLSSHistory='temporal'
)
& (Join-Path $PSScriptRoot 'Start-Step07A.ps1') -StepName step07d -VideoPath $VideoPath -Quality $Quality -OutputSize $OutputSize -MotionPipeline $MotionPipeline -DecodeResolution $DecodeResolution -DLSSHistory $DLSSHistory
