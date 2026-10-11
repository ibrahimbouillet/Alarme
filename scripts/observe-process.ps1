# Copyright (c) 2026 Ibrahim Bouillet. SPDX-License-Identifier: MIT
# Passive, bounded process observation. The default invocation returns as soon
# as its hidden observer starts; it never starts/stops ALARM or operates its UI.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateRange(1, 2147483647)]
    [int]$TargetProcessId,
    [ValidateRange(1, 3600)]
    [int]$DurationSeconds = 300,
    [ValidateRange(0.2, 60)]
    [double]$SampleSeconds = 1,
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\build\observations'),
    [switch]$Worker,
    [long]$ExpectedStartUtcTicks = 0,
    [string]$ReportBase
)
$ErrorActionPreference = 'Stop'
$invariant = [Globalization.CultureInfo]::InvariantCulture

function Write-ObservationJson([string]$Path, $Value) {
    $temporary = $Path + '.tmp'
    $json = ConvertTo-Json -InputObject $Value -Depth 5
    [IO.File]::WriteAllText($temporary, $json, [Text.UTF8Encoding]::new($false))
    Move-Item -LiteralPath $temporary -Destination $Path -Force
}

if (-not $Worker) {
    $observed = Get-Process -Id $TargetProcessId
    try { $startedTicks = $observed.StartTime.ToUniversalTime().Ticks }
    finally { $observed.Dispose() }
    $reportDirectory = [IO.Path]::GetFullPath($OutputDirectory)
    New-Item -ItemType Directory -Path $reportDirectory -Force | Out-Null
    $ReportBase = Join-Path $reportDirectory ('process-' + $TargetProcessId + '-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfff', $invariant))
    Write-ObservationJson ($ReportBase + '.status.json') ([ordered]@{
        Status = 'starting'; TargetProcessId = $TargetProcessId
        DurationSeconds = $DurationSeconds; SampleSeconds = $SampleSeconds
        Scope = 'Passive CPU/memory/handle counters; no UI input, capture, sound, sleep, or process control'
    })
    # An encoded command avoids shell-argument ambiguity for spaces/apostrophes
    # in paths. Only this script's fixed invocation and numeric options execute.
    $scriptLiteral = $PSCommandPath.Replace("'", "''")
    $reportLiteral = $ReportBase.Replace("'", "''")
    $workerCommand = "& '$scriptLiteral' -Worker -TargetProcessId $TargetProcessId -ExpectedStartUtcTicks $startedTicks -DurationSeconds $DurationSeconds -SampleSeconds " + $SampleSeconds.ToString($invariant) + " -ReportBase '$reportLiteral'"
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($workerCommand))
    $observerHost = (Get-Process -Id $PID).Path
    $observer = Start-Process -FilePath $observerHost -WindowStyle Hidden -PassThru -ArgumentList @(
        '-NoLogo', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-EncodedCommand', $encoded
    ) -RedirectStandardOutput ($ReportBase + '.stdout.log') -RedirectStandardError ($ReportBase + '.stderr.log')
    [pscustomobject]@{
        ObserverProcessId = $observer.Id; TargetProcessId = $TargetProcessId
        Samples = $ReportBase + '.samples.csv'
        Summary = $ReportBase + '.summary.json'
        Status = $ReportBase + '.status.json'
    }
    $observer.Dispose()
    return
}

if ([string]::IsNullOrWhiteSpace($ReportBase)) { throw 'Worker requires its report path.' }
$samples = $null
$observed = $null
$sampleCount = 0
$completion = 'error'
$failure = $null
$first = $null
$last = $null
$peak = @{ WorkingSetBytes = 0L; PrivateBytes = 0L; Handles = 0; Threads = 0; GdiObjects = 0; UserObjects = 0; CpuOneCorePercent = 0.0 }
try {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace AlarmProcessObservation {
    public static class NativeCounters {
        [DllImport("user32.dll", SetLastError=true)]
        public static extern uint GetGuiResources(IntPtr process, uint flags);
    }
}
'@
    $observed = Get-Process -Id $TargetProcessId
    if ($ExpectedStartUtcTicks -and $observed.StartTime.ToUniversalTime().Ticks -ne $ExpectedStartUtcTicks) {
        throw 'Target PID was reused; observation cancelled.'
    }
    $identity = [ordered]@{
        ProcessId = $TargetProcessId; ProcessName = $observed.ProcessName
        StartedUtc = $observed.StartTime.ToUniversalTime().ToString('o', $invariant)
        Executable = $observed.Path
    }
    $samples = [IO.StreamWriter]::new($ReportBase + '.samples.csv', $false, [Text.UTF8Encoding]::new($false))
    $samples.AutoFlush = $true
    $samples.WriteLine('Utc,ElapsedSeconds,CpuSeconds,CpuOneCorePercent,CpuMachinePercent,WorkingSetBytes,PrivateBytes,Handles,Threads,GdiObjects,UserObjects')
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $logicalProcessors = [Environment]::ProcessorCount
    Write-ObservationJson ($ReportBase + '.status.json') ([ordered]@{
        Status = 'running'; Target = $identity
        DurationSeconds = $DurationSeconds; SampleSeconds = $SampleSeconds
    })
    while ($true) {
        if ($observed.HasExited) { $completion = 'target_exited'; break }
        $observed.Refresh()
        $elapsed = $clock.Elapsed.TotalSeconds
        $cpu = $observed.TotalProcessorTime.TotalSeconds
        $oneCore = if ($last -and $elapsed -gt $last.ElapsedSeconds) {
            [Math]::Max(0, ($cpu - $last.CpuSeconds) / ($elapsed - $last.ElapsedSeconds) * 100)
        } else { 0.0 }
        $entry = [ordered]@{
            Utc = [DateTime]::UtcNow.ToString('o', $invariant)
            ElapsedSeconds = $elapsed; CpuSeconds = $cpu
            CpuOneCorePercent = $oneCore; CpuMachinePercent = $oneCore / $logicalProcessors
            WorkingSetBytes = $observed.WorkingSet64; PrivateBytes = $observed.PrivateMemorySize64
            Handles = $observed.HandleCount; Threads = $observed.Threads.Count
            GdiObjects = [AlarmProcessObservation.NativeCounters]::GetGuiResources($observed.Handle, 0)
            UserObjects = [AlarmProcessObservation.NativeCounters]::GetGuiResources($observed.Handle, 1)
        }
        if (-not $first) { $first = $entry }
        $values = foreach ($value in $entry.Values) {
            if ($value -is [IFormattable]) { $value.ToString($null, $invariant) } else { [string]$value }
        }
        $samples.WriteLine(($values -join ','))
        $sampleCount++
        foreach ($metric in @('WorkingSetBytes', 'PrivateBytes', 'Handles', 'Threads', 'GdiObjects', 'UserObjects', 'CpuOneCorePercent')) {
            if ($entry[$metric] -gt $peak[$metric]) { $peak[$metric] = $entry[$metric] }
        }
        $last = $entry
        $deadline = $first.ElapsedSeconds + $DurationSeconds
        if ($elapsed -ge $deadline) { $completion = 'completed'; break }
        $next = [Math]::Min($deadline, $first.ElapsedSeconds + $sampleCount * $SampleSeconds)
        $remainingMs = [Math]::Ceiling(($next - $clock.Elapsed.TotalSeconds) * 1000)
        if ($remainingMs -gt 0) { Start-Sleep -Milliseconds ([int]$remainingMs) }
    }
} catch {
    $targetExited = $false
    if ($observed) {
        try { $targetExited = $observed.HasExited } catch { }
    }
    if ($targetExited) { $completion = 'target_exited' }
    else { $failure = $_.Exception.Message }
} finally {
    if ($samples) { $samples.Dispose() }
    if ($observed) { $observed.Dispose() }
    $span = if ($first -and $last) { $last.ElapsedSeconds - $first.ElapsedSeconds } else { 0.0 }
    $cpuDelta = if ($first -and $last) { $last.CpuSeconds - $first.CpuSeconds } else { 0.0 }
    $summary = [ordered]@{
        Status = $completion; Error = $failure; Target = $identity
        RequestedDurationSeconds = $DurationSeconds; ObservedSpanSeconds = $span; Samples = $sampleCount
        LogicalProcessors = [Environment]::ProcessorCount
        CpuSecondsDuringObservation = $cpuDelta
        AverageCpuOneCorePercent = $(if ($span -gt 0) { $cpuDelta / $span * 100 } else { 0.0 })
        AverageCpuMachinePercent = $(if ($span -gt 0) { $cpuDelta / $span * 100 / [Environment]::ProcessorCount } else { 0.0 })
        First = $first; Last = $last; Peak = $peak
        Scope = 'Read-only process counters; this does not prove audibility, transparency, or absence of visible flicker'
    }
    Write-ObservationJson ($ReportBase + '.summary.json') $summary
    Write-ObservationJson ($ReportBase + '.status.json') ([ordered]@{
        Status = $completion; Error = $failure; Samples = $sampleCount
        Summary = $ReportBase + '.summary.json'
    })
}
if ($failure) { throw $failure }
