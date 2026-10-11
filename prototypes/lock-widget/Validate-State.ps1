# Copyright (c) 2026 Ibrahim Bouillet. SPDX-License-Identifier: MIT
param([switch]$Candidate,[string]$OutputRoot,[string]$PackageRoot,[ValidateRange(1,600)][int]$ProbeTimeoutSeconds=120)
$ErrorActionPreference = 'Stop'
if ($PackageRoot) { $packageRoot = [IO.Path]::GetFullPath($PackageRoot) }
else { $packageRoot = if ($Candidate) { Join-Path (Split-Path (Split-Path $PSScriptRoot)) 'out\Widgets' } else { Join-Path $PSScriptRoot 'package' } }
$name = if ($Candidate) { 'ALARM.WidgetProvider.exe' } else { 'ALARM.WidgetPrototype.exe' }
$executable = Join-Path $packageRoot $name
if (!(Test-Path -LiteralPath $executable -PathType Leaf)) { throw 'Build this widget package first.' }
$bridge = Join-Path $env:USERPROFILE '.alarm\widget-state.json'
if (!(Test-Path -LiteralPath $bridge -PathType Leaf)) { throw 'Open ALARM normally before validating the widget state reader.' }
$original = Get-Content -LiteralPath $bridge -Raw
$baseline = $original | ConvertFrom-Json
$process = Get-Process -Id $baseline.processId -ErrorAction Stop
if ($process.ProcessName -notin @('ALARM','ALARM_MVP')) { throw 'The baseline must describe a live ALARM process.' }
$variant = if ($Candidate) { 'candidate' } else { 'prototype' }
$validationRoot = Join-Path (Split-Path (Split-Path $PSScriptRoot)) ('.local\widget-validation\' + $variant)
if ($OutputRoot) { $validationRoot = [IO.Path]::GetFullPath($OutputRoot) }
[void](New-Item -ItemType Directory -Force -Path $validationRoot)
$utf8 = [Text.UTF8Encoding]::new($false)
$checks = @(
    @{ Name='live'; Edit={param($s)}; Phase=$baseline.phase; Status=$null },
    @{ Name='audio-error'; Edit={param($s) $s.phase='error'; $s.enabled=$true}; Phase='error'; Status='Sound requested: audio error' },
    @{ Name='pid-mismatch'; Edit={param($s) $s.processId=$PID; $s.enabled=$true}; Phase='closed'; Status='ALARM closed' },
    @{ Name='missing'; Missing=$true; Phase='closed'; Status='ALARM closed' },
    @{ Name='unknown-phase'; Edit={param($s) $s.phase='made-up'; $s.enabled=$true}; Phase='closed'; Status='ALARM closed' },
    @{ Name='invalid-json'; Invalid=$true; Phase='closed'; Status='ALARM closed' }
)
$receipt = [Collections.Generic.List[string]]::new()
$receipt.Add('UTC=' + [DateTime]::UtcNow.ToString('o'))
$receipt.Add('Read-only provider fixtures; live engine/bridge/configuration never modified.')
foreach ($check in $checks) {
    $inputPath = Join-Path $validationRoot ($check.Name + '.json')
    $outputPath = Join-Path $validationRoot ($check.Name + '-result.json')
    if ($check.Missing) {
        $inputPath = Join-Path $validationRoot ([guid]::NewGuid().ToString() + '-absent.json')
    } elseif ($check.Invalid) { [IO.File]::WriteAllText($inputPath,'{invalid',$utf8) }
    else {
        $state = $original | ConvertFrom-Json
        & $check.Edit $state
        [IO.File]::WriteAllText($inputPath,($state | ConvertTo-Json -Compress),$utf8)
    }
    $arguments = '--state-probe "' + $inputPath + '" "' + $outputPath + '"'
    $probe = Start-Process -FilePath $executable -ArgumentList $arguments -WindowStyle Hidden -PassThru
    try {
        $probeDeadline = [DateTime]::UtcNow.AddSeconds($ProbeTimeoutSeconds)
        while (!$probe.WaitForExit(1000)) {
            if ([DateTime]::UtcNow -ge $probeDeadline) {
                if (!$probe.HasExited) { $probe.Kill() }
                throw ($check.Name + ': state probe timed out; only the fixture process was stopped')
            }
        }
        if ($probe.ExitCode -ne 0) { throw ($check.Name + ': state probe failed') }
    } finally { $probe.Dispose() }
    $result = Get-Content -LiteralPath $outputPath -Raw | ConvertFrom-Json
    if ($result.phase -ne $check.Phase -or ($check.Status -and $result.status -ne $check.Status)) {
        throw ($check.Name + ': unexpected presentation ' + ($result | ConvertTo-Json -Compress))
    }
    $receipt.Add('PASS ' + $check.Name + ': phase=' + $result.phase + '; status=' + $result.status)
}
if ((Get-Content -LiteralPath $bridge -Raw) -ne $original) { throw 'Engine bridge changed during validation; fixture receipt was not finalized.' }
$receipt.Add('PASS engine bridge unchanged')
[IO.File]::WriteAllLines((Join-Path $validationRoot 'state-validation.txt'),$receipt,$utf8)
$receipt
