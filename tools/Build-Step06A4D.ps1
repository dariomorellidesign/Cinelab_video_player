#requires -Version 7.0
[CmdletBinding()]
param([switch]$GpuTests)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$repo=Split-Path -Parent $PSScriptRoot
$cmake='C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ctest=Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
$build=Join-Path $repo 'build\step06a4d-build'
$tests=Join-Path $repo 'build\step06a4d-tests'
$base=Join-Path $repo 'build\step06a4c-player\runtime'
$stage=Join-Path $repo 'build\step06a4d-player\runtime'
$release=Join-Path $repo 'build\Release\DLSSVideoPlayer.exe'
$protectedHash=(Get-FileHash -LiteralPath $release).Hash

function Invoke-Native([string]$Executable,[string[]]$Arguments) {
    # Some desktop-agent environments contain both Path and PATH. MSBuild's
    # .NET Framework child process rejects those duplicate environment keys.
    $start=[Diagnostics.ProcessStartInfo]::new()
    $start.FileName=$Executable
    $start.WorkingDirectory=$repo
    $start.UseShellExecute=$false
    $start.Environment.Clear()
    $environment=[Environment]::GetEnvironmentVariables()
    foreach($key in $environment.Keys){if($key -ine 'Path'){$start.Environment[$key]=[string]$environment[$key]}}
    $start.Environment['PATH']=$env:Path
    foreach($arg in $Arguments){$start.ArgumentList.Add($arg)}
    $process=[Diagnostics.Process]::Start($start)
    $process.WaitForExit()
    if($process.ExitCode -ne 0){throw "$Executable failed: $($process.ExitCode)"}
}

foreach($path in @($cmake,$base,$release)){if(-not(Test-Path -LiteralPath $path)){throw "Missing prerequisite: $path"}}
$sdk=Join-Path $repo 'external\NvOFSDK\Common\NvOFBase'
if(-not(Select-String -LiteralPath (Join-Path $sdk 'NvOF.cpp') -SimpleMatch 'enableOutputCost = NV_OF_TRUE' -Quiet)) {
    throw 'The Step06A3 NVOF cost-buffer dependency patch is missing. See patches/nvof-step06a3-cost.patch.'
}
foreach($mapping in @('ofBufFormat = NV_OF_BUFFER_FORMAT_UINT8','dxgiFormat = DXGI_FORMAT_R8_UINT')) {
    if(-not(Select-String -LiteralPath (Join-Path $sdk 'NvOFD3DCommon.cpp') -SimpleMatch $mapping -Quiet)) {
        throw 'The Step06A3 NVOF R8_UINT mapping patch is missing. See patches/nvof-step06a3-cost.patch.'
    }
}
$running=@(Get-Process -Name DLSSVideoPlayer -ErrorAction SilentlyContinue | Where-Object {$_.Path -eq (Join-Path $stage 'DLSSVideoPlayer.exe')})
if($running.Count){throw 'Close the Step06A4D test player before replacing its executable.'}
Invoke-Native $cmake @('-S',$repo,'-B',$build,'-G','Visual Studio 17 2022','-A','x64',
    '-DDMP_TRTRTX_ROOT=C:\PROGETTO_DLSS\TensorRT-RTX-SDK\TensorRT-RTX-1.6.1.120',
    '-DCUDAToolkit_ROOT=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.4')
Invoke-Native $cmake @('--build',$build,'--config','Release','--target','DLSSVideoPlayer','--parallel','4')
$gpu=if($GpuTests){'ON'}else{'OFF'}
Invoke-Native $cmake @('-S',(Join-Path $repo 'tests'),'-B',$tests,'-G','Visual Studio 17 2022','-A','x64',('-DDMP_BUILD_GPU_TESTS='+$gpu))
Invoke-Native $cmake @('--build',$tests,'--config','Release','--parallel','4')
Invoke-Native $ctest @('--test-dir',$tests,'-C','Release','--output-on-failure')
if(-not(Test-Path -LiteralPath $stage)) {
    New-Item -ItemType Directory -Path $stage -Force | Out-Null
    & robocopy $base $stage /E /COPY:DAT /R:1 /W:1 /XD '_1Click_DLSS5_Backup' 'DLSS5 Screenshots' 'ngx_logs' /XF '*.log' '*-launch.txt' /NFL /NDL /NJH /NJS /NP
    if($LASTEXITCODE -ge 8){throw 'Runtime staging failed'}
}
foreach($file in (Get-ChildItem -LiteralPath $base -File | Where-Object {$_.Extension -in '.dll','.addon64'})) {
    if((Get-FileHash -LiteralPath $file.FullName).Hash -ne (Get-FileHash -LiteralPath (Join-Path $stage $file.Name)).Hash){throw "Runtime dependency differs: $($file.Name)"}
}
$exe=Join-Path $stage 'DLSSVideoPlayer.exe'
Copy-Item -LiteralPath (Join-Path $build 'Release\DLSSVideoPlayer.exe') -Destination $exe -Force
@('STEP06A4D_RUNTIME=1','BASE=Step06A4C','CONFIDENCE_POLICY=Step06A3 exact S10.5','GLOBAL_MEDIAN=parallel_histogram','STREAMLINE_FAILED_DEVICE=native_fallback',('EXE_SHA256='+(Get-FileHash -LiteralPath $exe).Hash)) |
    Set-Content -LiteralPath (Join-Path $stage 'step06a4d-runtime.txt') -Encoding utf8
if((Get-FileHash -LiteralPath $release).Hash -ne $protectedHash){throw 'Official Release changed unexpectedly'}
Write-Host "STEP06A4D_BUILD=PASS runtime=$exe"
