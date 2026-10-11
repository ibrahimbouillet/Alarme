# Copyright (c) 2026 Ibrahim Bouillet. SPDX-License-Identifier: MIT
param([switch]$Candidate)
$ErrorActionPreference = 'Stop'
$packageName = if ($Candidate) { 'Ibrahim.ALARM.Widgets' } else { 'Ibrahim.ALARM.WidgetPrototype' }
$publisher = if ($Candidate) { 'CN=Ibrahim ALARM' } else { 'CN=ALARM Widget Prototype' }
$package = Get-AppxPackage -Name $packageName
if (!$package) { Write-Output 'Widget package is already absent.'; return }
$packageRoot = if ($Candidate) { Join-Path (Split-Path (Split-Path $PSScriptRoot)) 'out\Widgets' } else { Join-Path $PSScriptRoot 'package' }
$expected = [IO.Path]::GetFullPath($packageRoot)
if ($package.Publisher -ne $publisher -or [IO.Path]::GetFullPath($package.InstallLocation) -ne $expected -or !$package.IsDevelopmentMode) {
    throw 'Package ownership/path/development-mode check failed. Nothing was removed.'
}
Remove-AppxPackage -Package $package.PackageFullName
