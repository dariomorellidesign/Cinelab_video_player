#requires -Version 7.0
[CmdletBinding()]
param([switch]$GpuTests)
& (Join-Path $PSScriptRoot 'Build-Step07A.ps1') -StepName step07d -GpuTests:$GpuTests
