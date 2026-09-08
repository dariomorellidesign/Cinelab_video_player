[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Before,
    [Parameter(Mandatory)][string]$After,
    [double]$WarmupSeconds=20,
    [double]$DurationSeconds=60,
    [string]$OutputPath
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$culture=[Globalization.CultureInfo]::InvariantCulture

function Get-PlaybackSummary([string]$Path) {
    $rows=@(foreach($line in [IO.File]::ReadLines((Resolve-Path -LiteralPath $Path).Path)) {
        if($line -notmatch '^\[(?<stamp>\d{2}:\d{2}:\d{2}\.\d{3})\]') { continue }
        $stamp=[TimeSpan]::ParseExact($Matches.stamp,'hh\:mm\:ss\.fff',$culture).TotalSeconds
        if($line -notmatch '\[(Pipeline|Perf|NVOF Confidence)\]') { continue }
        $kind=$Matches[1]
        $fields=@{}
        foreach($field in [regex]::Matches($line,'(?<key>\w+)=(?<value>-?[0-9]+(?:\.[0-9]+)?)(?:\s|$)')) {
            $fields[$field.Groups['key'].Value]=[double]::Parse($field.Groups['value'].Value,$culture)
        }
        [pscustomobject]@{Time=$stamp;Kind=$kind;Fields=$fields;Line=$line}
    })
    $first=$rows | Where-Object {$_.Kind -eq 'Pipeline'} | Select-Object -First 1
    if(-not $first){throw "No pipeline telemetry: $Path"}
    $start=$first.Time+$WarmupSeconds
    $end=$start+$DurationSeconds
    if(($rows | Select-Object -Last 1).Time -lt $end){throw "Incomplete sample window: $Path"}
    $window=@($rows | Where-Object {$_.Time -ge $start -and $_.Time -lt $end})
    $metrics=[ordered]@{}
    foreach($key in @('frameMs','nvofMs','guidesMs','rendererMs','otherMs','repairMs','textureMs','classifyMs','infillMs','submitFps')) {
        $kind=if($key -eq 'submitFps'){'Perf'}elseif($key -in @('repairMs','textureMs','classifyMs','infillMs')){'NVOF Confidence'}else{'Pipeline'}
        $values=@($window | Where-Object {$_.Kind -eq $kind -and $_.Fields.ContainsKey($key)} | ForEach-Object {$_.Fields[$key]} | Sort-Object)
        if($values.Count) {
            $metrics[$key]=[ordered]@{
                count=$values.Count
                mean=[math]::Round(($values | Measure-Object -Average).Average,4)
                median=[math]::Round($values[[int][math]::Floor($values.Count/2)],4)
                p95=[math]::Round($values[[int][math]::Ceiling($values.Count*0.95)-1],4)
            }
        }
    }
    $perf=@($window | Where-Object {$_.Kind -eq 'Perf'})
    $dropped=$null
    if($perf.Count -ge 2){$dropped=$perf[-1].Fields['dropped']-$perf[0].Fields['dropped']}
    $qualities=@($window | Where-Object {$_.Kind -eq 'Pipeline'} | ForEach-Object {if($_.Line -match 'quality=(\S+)'){$Matches[1]}} | Sort-Object -Unique)
    [ordered]@{log=$Path;warmupSeconds=$WarmupSeconds;durationSeconds=$DurationSeconds;quality=$qualities;droppedDelta=$dropped;metrics=$metrics}
}
$result=[ordered]@{before=(Get-PlaybackSummary $Before);after=(Get-PlaybackSummary $After)}
$json=$result | ConvertTo-Json -Depth 8
if($OutputPath){[IO.File]::WriteAllText($OutputPath,$json,[Text.UTF8Encoding]::new($false))}
$json
