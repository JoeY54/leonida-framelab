# SPDX-License-Identifier: MIT
# Runs the installed D3D12SR consumer in separate processes: standard checks, D3D12 debug layer,
# GPU-based validation, timing without validation or readback, and native device removal.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$RuntimeDirectory,
    [string]$LogRoot = (Join-Path (Get-Location) 'framelab-logs'),
    [int]$TimingFrames = 600,
    [switch]$SkipGpuValidation,
    [switch]$SkipDeviceRemoval
)
$ErrorActionPreference = 'Stop'
$exe = (Get-Item -LiteralPath $Executable).FullName
$runtime = (Get-Item -LiteralPath $RuntimeDirectory).FullName
$runId = 'd3d12-sr-{0:yyyyMMdd-HHmmss}-{1}' -f (Get-Date), ([guid]::NewGuid().ToString('N').Substring(0, 8))
$logs = Join-Path ($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($LogRoot)) $runId
New-Item -ItemType Directory -Force -Path $logs | Out-Null
Write-Output "D3D12 SR evidence directory: $logs"

$cases = @(
    @{ name = 'correctness'; args = @(); summary = 'correctness' },
    @{ name = 'validation'; args = @('--validation'); summary = 'validation' }
)
if (-not $SkipGpuValidation) { $cases += @{ name = 'gpu-validation'; args = @('--gpu-validation'); summary = 'gpu-validation' } }
$cases += @{ name = 'timing'; args = @('--timing', "$TimingFrames"); summary = 'timing' }
if (-not $SkipDeviceRemoval) { $cases += @{ name = 'device-removal'; args = @('--device-removal'); summary = 'device-removal' } }

$results = @()
$failed = $false
foreach ($case in $cases) {
    $caseLogs = Join-Path $logs $case.name
    New-Item -ItemType Directory -Path $caseLogs | Out-Null
    $stdout = Join-Path $caseLogs 'stdout.log'
    $stderr = Join-Path $caseLogs 'stderr.log'
    $arguments = @("`"$runtime`"", "`"$caseLogs`"") + $case.args
    $run = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory (Split-Path $exe) -WindowStyle Hidden `
        -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    # Windows PowerShell 5.1 reports a null ExitCode unless the process handle is opened before the process exits.
    $null = $run.Handle
    if (-not $run.WaitForExit(600000)) { $run.Kill(); $run.WaitForExit(); throw "Timeout: $($case.name)" }
    $run.WaitForExit()
    if ($null -eq $run.ExitCode) { throw "Exit code unavailable: $($case.name)" }
    $summaryPath = Join-Path $caseLogs "d3d12-sr-$($case.summary).json"
    $summary = if (Test-Path -LiteralPath $summaryPath) { Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json } else { $null }
    $row = [ordered]@{
        name = $case.name
        exit_code = $run.ExitCode
        stderr_bytes = (Get-Item -LiteralPath $stderr).Length
        summary = $summary
    }
    $readback = Join-Path $caseLogs 'sr-1280x720-rgba8.rgba'
    if (Test-Path -LiteralPath $readback) {
        $length = (Get-Item -LiteralPath $readback).Length
        if ($length -ne 1280 * 720 * 4) { throw "Unexpected readback size: $readback" }
        $row.readback_sha256 = (Get-FileHash -LiteralPath $readback -Algorithm SHA256).Hash
    }
    $ok = $run.ExitCode -eq 0 -and $row.stderr_bytes -eq 0 -and $summary -and $summary.result -eq 'pass'
    if ($case.name -in @('validation', 'gpu-validation') -and $summary) {
        $ok = $ok -and $summary.validation.errors -eq 0 -and $summary.validation.warnings -eq 0
    }
    $row.verdict = if ($ok) { 'pass' } else { 'fail' }
    if (-not $ok) { $failed = $true; Write-Warning "$($case.name) failed. See $stdout" }
    $results += [pscustomobject]$row
    Write-Output ("{0}: {1} (exit {2})" -f $case.name, $row.verdict, $run.ExitCode)
}

$gpu = Get-CimInstance Win32_VideoController | Where-Object { $_.AdapterCompatibility -match 'NVIDIA' } | Select-Object -First 1
$report = [ordered]@{
    run_id = $runId
    executable = $exe
    runtime_directory = $runtime
    gpu = if ($gpu) { $gpu.Name } else { $null }
    driver = if ($gpu) { $gpu.DriverVersion } else { $null }
    os = [Environment]::OSVersion.VersionString
    cases = $results
}
$report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $logs 'd3d12-sr-results.json') -Encoding utf8
if ($failed) { exit 1 }
