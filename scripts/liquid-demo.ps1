# Copyright (c) 2026 Ibrahim Bouillet. SPDX-License-Identifier: MIT
# Explicit user-invoked launcher for the separate visible liquid playground.
# It never changes or closes the alarm process, configuration or Windows setup.
[CmdletBinding()]
param(
    [switch]$Grid,
    [switch]$Desktop,
    [switch]$Transparent,
    [switch]$NoBuild,
    [string]$MetricsPath
)
$ErrorActionPreference = 'Stop'
if (([int]$Grid.IsPresent + [int]$Desktop.IsPresent + [int]$Transparent.IsPresent) -gt 1) { throw 'Choose one mode: Desktop (default), Grid, or Transparent.' }
$desktopMode = -not $Grid -and -not $Transparent
$projectDir = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Push-Location -LiteralPath $projectDir
try {
    if (-not $NoBuild) {
        cmake --preset windows-release -B build-demo -DALARM_BUILD_DEMOS=ON
        if ($LASTEXITCODE -ne 0) { throw 'Liquid demo configure failed.' }
        cmake --build build-demo --parallel 4 --target alarm_liquid_demo alarm_liquid_tests alarm_capture_tests
        if ($LASTEXITCODE -ne 0) { throw 'Liquid demo build failed.' }
        ctest --test-dir build-demo --output-on-failure -R '^(liquid_physics|liquid_renderer_hidden|desktop_capture_lifecycle)$'
        if ($LASTEXITCODE -ne 0) { throw 'Liquid demo checks failed; nothing was launched.' }
    }
    $demoBinary = Join-Path $projectDir 'build-demo\ALARM_Liquid_Demo.exe'
    if (-not (Test-Path -LiteralPath $demoBinary -PathType Leaf)) { throw 'Build the liquid demo first.' }
    Write-Output 'Separate transparent playground. Hold/drag a drop, release to fall; it fades in four seconds. Escape or Close exits.'
    if ($desktopMode) { Write-Output 'Desktop mode waits for Choose display and your source selection in the Windows picker. No water is drawn before a GPU source is available. Pixels are discarded on idle/pause/close, and no source choice is saved.' }
    elseif ($Grid) { Write-Output 'OPTICAL GRID (TEST): refraction samples only an internally generated grid, not the desktop.' }
    else { Write-Output 'Transparent drop with mouse-directed light and shadow; desktop optical refraction is not claimed by this mode.' }
    $launchParameters = @{ FilePath = $demoBinary; WorkingDirectory = $projectDir; WindowStyle = 'Normal'; PassThru = $true }
    $demoArguments = @()
    if ($Grid) { $demoArguments += '--grid' }
    if ($desktopMode) { $demoArguments += '--desktop' }
    if ($Transparent) { $demoArguments += '--transparent' }
    if ($MetricsPath) {
        $resolvedMetrics = [IO.Path]::GetFullPath($MetricsPath)
        if ($resolvedMetrics.Contains('"')) { throw 'The metrics path cannot contain quotation marks.' }
        $demoArguments += @('--metrics', ('"' + $resolvedMetrics + '"'))
        Write-Output "Optional own-drop metrics: $resolvedMetrics. F12 refreshes the JSON immediately."
    }
    if ($demoArguments.Count) { $launchParameters.ArgumentList = $demoArguments }
    $demoProcess = Start-Process @launchParameters
    [pscustomobject]@{ ProcessId = $demoProcess.Id; Executable = $demoBinary; Mode = $(if ($desktopMode) { 'Select a Windows source manually' } elseif ($Grid) { 'Optical test grid' } else { 'Transparent diagnostic without desktop refraction' }) }
    $demoProcess.Dispose()
} finally { Pop-Location }
