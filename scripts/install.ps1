# Copyright (c) 2026 Ibrahim Bouillet. SPDX-License-Identifier: MIT
# Run from an extracted release. Installation is per-user and never starts ALARM.
[CmdletBinding()]
param(
    [string]$InstallDirectory = (Join-Path $env:LOCALAPPDATA 'Programs\ALARM'),
    [switch]$NoShortcut,
    [switch]$NoRegistry
)
$ErrorActionPreference = 'Stop'
$installDir = [IO.Path]::GetFullPath($InstallDirectory).TrimEnd('\')
$payloadDir = [IO.Path]::GetFullPath($PSScriptRoot)
if (-not (Test-Path -LiteralPath (Join-Path $payloadDir 'ALARM.exe'))) {
    throw 'Run install.ps1 from the extracted ALARM release ZIP. Build a release with scripts\package.ps1 first.'
}
if ($installDir -eq $payloadDir -or $installDir -eq [IO.Path]::GetPathRoot($installDir).TrimEnd('\')) {
    throw 'Choose a separate application directory, not the release folder or a drive root.'
}
$markerPath = Join-Path $installDir '.alarm-install.json'
$previous = $null
if (Test-Path -LiteralPath $installDir) {
    if ((Get-Item -LiteralPath $installDir).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Installation through a junction or symbolic link is not supported.' }
    if (Test-Path -LiteralPath $markerPath) {
        $previous = Get-Content -LiteralPath $markerPath -Raw | ConvertFrom-Json
        if ($previous.Product -ne 'ALARM by Ibrahim Bouillet' -or $previous.InstallDirectory -ne $installDir) { throw 'The directory is not a recognized ALARM installation.' }
    } elseif (Get-ChildItem -LiteralPath $installDir -Force) { throw 'Choose an empty directory to preserve its existing files.' }
}
$registryPath = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\ALARM.IbrahimBouillet'
if (-not $NoRegistry -and (Test-Path -LiteralPath $registryPath)) {
    $registered = Get-ItemProperty -LiteralPath $registryPath
    if ($registered.InstallLocation -ne $installDir) { throw 'Another ALARM installation is registered. Uninstall it before choosing a different directory, or use -NoRegistry for an isolated copy.' }
}
$exePath = Join-Path $installDir 'ALARM.exe'
$shortcutPath = Join-Path ([Environment]::GetFolderPath('Programs')) 'ALARM.lnk'
if (-not $NoShortcut -and (Test-Path -LiteralPath $shortcutPath)) {
    $shortcutShell = New-Object -ComObject WScript.Shell
    try {
        if ($shortcutShell.CreateShortcut($shortcutPath).TargetPath -ne $exePath) {
            throw 'An ALARM shortcut points to another location. Uninstall that copy first, or use -NoShortcut to preserve it.'
        }
    } finally { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shortcutShell) }
}
foreach ($running in @(Get-Process ALARM -ErrorAction SilentlyContinue)) {
    if ($running.Path -eq $exePath) { throw 'Quit the installed ALARM from its tray menu before updating it.' }
}
$files = @('ALARM.exe', 'README.md', 'TECHNICAL_FEASIBILITY.md', 'LICENSE', 'THIRD_PARTY_NOTICES.md', 'install.ps1', 'uninstall.ps1', 'docs\PRODUCT.md', 'docs\MANUAL_VALIDATION.md')
foreach ($notice in Get-ChildItem -LiteralPath (Join-Path $payloadDir 'licenses') -Recurse -File) {
    $files += $notice.FullName.Substring($payloadDir.Length + 1)
}
foreach ($relative in $files) {
    if (-not (Test-Path -LiteralPath (Join-Path $payloadDir $relative) -PathType Leaf)) { throw "Release is incomplete: $relative is missing." }
    $destination = Join-Path $installDir $relative
    if (Test-Path -LiteralPath $destination) {
        if ((Get-Item -LiteralPath $destination).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Refusing to replace a symbolic link.' }
    }
    $ancestor = Split-Path -Parent $destination
    while ($ancestor) {
        if ((Test-Path -LiteralPath $ancestor) -and ((Get-Item -LiteralPath $ancestor).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Installation through a junction or symbolic link is not supported.' }
        $ancestor = Split-Path -Parent $ancestor
    }
}
foreach ($relative in $files) {
    $destination = Join-Path $installDir $relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $payloadDir $relative) -Destination $destination -Force
}
if (-not $NoShortcut) {
    $shell = New-Object -ComObject WScript.Shell
    try {
        $shortcut = $shell.CreateShortcut($shortcutPath)
        if ((Test-Path -LiteralPath $shortcutPath) -and $shortcut.TargetPath -ne $exePath) { throw 'An unrelated ALARM shortcut already exists; use -NoShortcut to preserve it.' }
        $shortcut.TargetPath = $exePath
        $shortcut.WorkingDirectory = $installDir
        $shortcut.Description = 'ALARM by Ibrahim Bouillet'
        $shortcut.IconLocation = "$exePath,0"
        $shortcut.Save()
    } finally { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shell) }
}
$version = (Get-Item -LiteralPath $exePath).VersionInfo.ProductVersion
$metadata = [ordered]@{
    Product = 'ALARM by Ibrahim Bouillet'; Version = $version
    InstallDirectory = $installDir; Files = $files
    Shortcut = $(if (-not $NoShortcut) { $shortcutPath } elseif ($previous) { $previous.Shortcut } else { '' })
    Registry = -not $NoRegistry
}
[IO.File]::WriteAllText($markerPath, ($metadata | ConvertTo-Json -Depth 4), [Text.UTF8Encoding]::new($false))
if (-not $NoRegistry) {
    New-Item -Path $registryPath -Force | Out-Null
    $properties = @{
        DisplayName = 'ALARM'; DisplayVersion = $version; Publisher = 'Ibrahim Bouillet'
        InstallLocation = $installDir; DisplayIcon = "$exePath,0"
        UninstallString = 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "' + (Join-Path $installDir 'uninstall.ps1') + '"'
    }
    foreach ($property in $properties.GetEnumerator()) { New-ItemProperty -LiteralPath $registryPath -Name $property.Key -Value $property.Value -PropertyType String -Force | Out-Null }
    foreach ($name in @('NoModify', 'NoRepair')) { New-ItemProperty -LiteralPath $registryPath -Name $name -Value 1 -PropertyType DWord -Force | Out-Null }
}
Write-Output "Installed ALARM $version in $installDir. Open ALARM from Start or launch ALARM.exe. Daily alarms remain disabled until you enable one."
