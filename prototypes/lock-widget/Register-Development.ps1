# Copyright (c) 2026 Ibrahim Bouillet. SPDX-License-Identifier: MIT
param([switch]$Candidate)
$ErrorActionPreference = 'Stop'
# Only use already-enabled Developer Mode. Never change it or install/trust certificates.
$developerMode = Get-ItemPropertyValue -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock' -Name AllowDevelopmentWithoutDevLicense -ErrorAction Stop
if ($developerMode -ne 1) { throw 'Developer Mode is not already enabled. Registration was not attempted.' }
$runtime = Get-AppxPackage -Name 'Microsoft.WindowsAppRuntime.2' | Where-Object { $_.Architecture -eq 'X64' -and [version]$_.Version -ge [version]'2.3.1.0' }
if (!$runtime) { throw 'Windows App Runtime 2.3.1 x64 or newer is missing. No runtime will be installed automatically.' }
$packageName = if ($Candidate) { 'Ibrahim.ALARM.Widgets' } else { 'Ibrahim.ALARM.WidgetPrototype' }
$publisher = if ($Candidate) { 'CN=Ibrahim ALARM' } else { 'CN=ALARM Widget Prototype' }
$packageRoot = if ($Candidate) { Join-Path (Split-Path (Split-Path $PSScriptRoot)) 'out\Widgets' } else { Join-Path $PSScriptRoot 'package' }
$manifest = Join-Path $packageRoot 'AppxManifest.xml'
if (!(Test-Path -LiteralPath $manifest)) { throw 'Build this widget package first.' }
$existing = Get-AppxPackage -Name $packageName
if ($existing -and $existing.Publisher -ne $publisher) { throw 'Another publisher owns this package name.' }
if ($existing -and ([IO.Path]::GetFullPath($existing.InstallLocation) -ne [IO.Path]::GetFullPath((Split-Path $manifest)) -or !$existing.IsDevelopmentMode)) {
    throw 'An existing package uses another path or is not a development registration. Nothing was changed.'
}
$identity = ([xml](Get-Content -LiteralPath $manifest -Raw)).Package.Identity
if ($identity.Name -ne $packageName -or $identity.Publisher -ne $publisher) { throw 'Unexpected manifest identity. Nothing was registered.' }
$manifestVersion = [version]$identity.Version
if ($existing -and [version]$existing.Version -gt $manifestVersion) { throw 'A newer package is registered. No downgrade was attempted.' }
if ($existing -and [version]$existing.Version -eq $manifestVersion) {
    $existing | Select-Object Name,Version,PackageFullName,PackageFamilyName,InstallLocation,IsDevelopmentMode,SignatureKind,Status
    return
}
Add-AppxPackage -Register $manifest
Get-AppxPackage -Name $packageName | Select-Object Name,Version,PackageFullName,PackageFamilyName,InstallLocation,IsDevelopmentMode,SignatureKind,Status
