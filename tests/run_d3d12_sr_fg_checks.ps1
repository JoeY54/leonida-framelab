# SPDX-License-Identifier: MIT
# FL-16: runs the installed D3D12SR + D3D12FG consumer (samples/d3d12_sr_fg) in separate processes.
# The four independent modes (native, SR only, FG only, SR chained into FG) run first. The combined
# mode then runs in both creation/shutdown orders, under the D3D12 debug layer, across output sizes
# and quality modes, and at staged source-frame counts. A stage runs only after every earlier case
# passed. Each case writes a JSON summary; the runner collects them with host details.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$RuntimeDirectory,
    [string]$LogRoot = (Join-Path (Get-Location) 'framelab-logs'),
    [int[]]$StageFrames = @(120, 500, 1000),
    [switch]$SkipDebug,
    [switch]$SkipMatrix
)
$ErrorActionPreference = 'Stop'
$exe = (Get-Item -LiteralPath $Executable).FullName
$runtime = (Get-Item -LiteralPath $RuntimeDirectory).FullName
$runId = 'd3d12-sr-fg-{0:yyyyMMdd-HHmmss}-{1}' -f (Get-Date), ([guid]::NewGuid().ToString('N').Substring(0, 8))
$logs = Join-Path ($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($LogRoot)) $runId
New-Item -ItemType Directory -Force -Path $logs | Out-Null
Write-Output "D3D12 SR+FG evidence directory: $logs"

$cases = @(
    @{ name = 'native'; args = @('--mode', 'native') },
    @{ name = 'sr'; args = @('--mode', 'sr') },
    @{ name = 'fg'; args = @('--mode', 'fg') },
    @{ name = 'sr-fg'; args = @('--mode', 'sr-fg') },
    @{ name = 'sr-fg-fg-first'; args = @('--mode', 'sr-fg', '--order', 'fg-first') }
)
if (-not $SkipDebug) {
    $cases += @{ name = 'sr-fg-debug'; args = @('--mode', 'sr-fg', '--debug') }
    $cases += @{ name = 'sr-fg-debug-fg-first'; args = @('--mode', 'sr-fg', '--order', 'fg-first', '--debug') }
}
if (-not $SkipMatrix) {
    foreach ($output in @('1920x1080', '2560x1440', '3840x2160')) {
        foreach ($quality in @('performance', 'balanced', 'quality')) {
            $cases += @{ name = "sr-fg-$output-$quality"; args = @('--mode', 'sr-fg', '--output', $output, '--quality', $quality) }
        }
    }
}
foreach ($frames in $StageFrames) {
    $cases += @{ name = "stage-$frames"; args = @('--mode', 'sr-fg', '--frames', "$frames") }
}

$results = @()
$failed = $false
foreach ($case in $cases) {
    if ($failed -and $case.name -like 'stage-*') {
        Write-Output "$($case.name): skipped (an earlier case failed)"
        $results += [pscustomobject]@{ name = $case.name; verdict = 'skipped'; reason = 'earlier failure' }
        continue
    }
    $caseLogs = Join-Path $logs $case.name
    New-Item -ItemType Directory -Path $caseLogs | Out-Null
    $stdout = Join-Path $caseLogs 'stdout.log'
    $stderr = Join-Path $caseLogs 'stderr.log'
    $arguments = @("`"$runtime`"", "`"$caseLogs`"") + $case.args
    $started = Get-Date
    $run = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory (Split-Path $exe) `
        -PassThru -NoNewWindow -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    # Windows PowerShell 5.1 reports a null ExitCode unless the process handle is opened before the process exits.
    $null = $run.Handle
    $peak = 0
    while (-not $run.HasExited) {
        try { $run.Refresh(); $peak = [Math]::Max($peak, $run.PeakWorkingSet64) } catch { }
        if (((Get-Date) - $started).TotalSeconds -gt 1800) { $run.Kill(); $run.WaitForExit(); throw "Timeout: $($case.name)" }
        Start-Sleep -Milliseconds 250
    }
    $run.WaitForExit()
    if ($null -eq $run.ExitCode) { throw "Exit code unavailable: $($case.name)" }
    $text = Get-Content -LiteralPath $stdout -Raw
    $summaryFile = Get-ChildItem -LiteralPath $caseLogs -Filter 'd3d12-sr-fg-*.json' | Select-Object -First 1
    $summary = if ($summaryFile) { Get-Content -LiteralPath $summaryFile.FullName -Raw | ConvertFrom-Json } else { $null }
    $ok = $run.ExitCode -eq 0 -and $summary -and $summary.result -eq 'pass' -and $text -match 'FL16_RESULT pass' -and
        (Get-Item -LiteralPath $stderr).Length -eq 0
    $results += [pscustomobject][ordered]@{
        name = $case.name
        arguments = ($case.args -join ' ')
        exit_code = $run.ExitCode
        seconds = [Math]::Round(((Get-Date) - $started).TotalSeconds, 1)
        peak_working_set_mb = [Math]::Round($peak / 1MB, 1)
        verdict = if ($ok) { 'pass' } else { 'fail' }
        summary = $summary
    }
    if (-not $ok) { $failed = $true; Write-Warning "$($case.name) failed. See $stdout" }
    Write-Output ("{0}: {1} (exit {2}, {3} s)" -f $case.name, $results[-1].verdict, $run.ExitCode, $results[-1].seconds)
}

$gpu = Get-CimInstance Win32_VideoController | Where-Object { $_.AdapterCompatibility -match 'NVIDIA' } | Select-Object -First 1
$dlls = foreach ($name in @('nvngx_dlss.dll', 'nvngx_dlssg.dll')) {
    $path = Join-Path $runtime $name
    if (Test-Path -LiteralPath $path) { [ordered]@{ name = $name; version = (Get-Item -LiteralPath $path).VersionInfo.FileVersion } }
}
$report = [ordered]@{
    run_id = $runId
    executable = $exe
    runtime_directory = $runtime
    runtime_files = @($dlls)
    gpu = if ($gpu) { $gpu.Name } else { $null }
    driver = if ($gpu) { $gpu.DriverVersion } else { $null }
    os = [Environment]::OSVersion.VersionString
    presentation = 'not exercised by this client; see tests/run_d3d12_fg_present_checks.ps1'
    cases = $results
}
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $logs 'd3d12-sr-fg-results.json') -Encoding utf8
if ($failed) { exit 1 }
exit 0
