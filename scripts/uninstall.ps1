# Copyright (c) 2026 Ibrahim Bouillet. SPDX-License-Identifier: MIT
# Personal alarm settings and custom WAV files are preserved.
[CmdletBinding()]
param(
    [string]$InstallDirectory = $PSScriptRoot,
    [switch]$NoShortcut,
    [switch]$NoRegistry
)
$ErrorActionPreference = 'Stop'
$installDir = [IO.Path]::GetFullPath($InstallDirectory).TrimEnd('\')
$markerPath = Join-Path $installDir '.alarm-install.json'
if (-not (Test-Path -LiteralPath $markerPath -PathType Leaf)) { throw 'No ALARM installation marker was found; no files were removed.' }
$metadata = Get-Content -LiteralPath $markerPath -Raw | ConvertFrom-Json
if ($metadata.Product -ne 'ALARM by Ibrahim Bouillet' -or $metadata.InstallDirectory -ne $installDir -or $installDir -eq [IO.Path]::GetPathRoot($installDir).TrimEnd('\')) { throw 'Invalid installation marker; no files were removed.' }
$exePath = Join-Path $installDir 'ALARM.exe'
foreach ($running in @(Get-Process ALARM -ErrorAction SilentlyContinue)) {
    if ($running.Path -eq $exePath) { throw 'Quit ALARM from the tray menu before uninstalling it.' }
}
$allowed = @('ALARM.exe', 'README.md', 'TECHNICAL_FEASIBILITY.md', 'LICENSE', 'THIRD_PARTY_NOTICES.md', 'install.ps1', 'uninstall.ps1', 'docs\PRODUCT.md', 'docs\MANUAL_VALIDATION.md')
$validated = @()
foreach ($relative in $metadata.Files) {
    if ([IO.Path]::IsPathRooted($relative) -or ($relative -notin $allowed -and $relative -notmatch '^licenses\\[a-zA-Z0-9._-]+\\[a-zA-Z0-9._-]+$')) { throw 'Unexpected file in installation marker; no files were removed.' }
    $path = [IO.Path]::GetFullPath((Join-Path $installDir $relative))
    if (-not $path.StartsWith($installDir + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'File escapes installation directory; no files were removed.' }
    $ancestor = $path
    while ($ancestor) {
        if ((Test-Path -LiteralPath $ancestor) -and ((Get-Item -LiteralPath $ancestor).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Uninstall through a junction or symbolic link is not supported.' }
        $ancestor = Split-Path -Parent $ancestor
    }
    $validated += $path
}
if (-not $NoShortcut) {
    $expectedShortcut = Join-Path ([Environment]::GetFolderPath('Programs')) 'ALARM.lnk'
    if (Test-Path -LiteralPath $expectedShortcut) {
        $shell = New-Object -ComObject WScript.Shell
        try { if ($shell.CreateShortcut($expectedShortcut).TargetPath -eq $exePath) { Remove-Item -LiteralPath $expectedShortcut } }
        finally { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shell) }
    }
}
if (-not $NoRegistry) {
    $registryPath = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\ALARM.IbrahimBouillet'
    if ((Test-Path -LiteralPath $registryPath) -and (Get-ItemProperty -LiteralPath $registryPath).InstallLocation -eq $installDir) { Remove-Item -LiteralPath $registryPath }
    $runPath = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
    $startup = (Get-ItemProperty -LiteralPath $runPath -Name 'ALARM.Ibrahim' -ErrorAction SilentlyContinue).'ALARM.Ibrahim'
    if ($startup -eq ('"' + $exePath + '" --background')) { Remove-ItemProperty -LiteralPath $runPath -Name 'ALARM.Ibrahim' }
    # Native notifications register a protocol on first launch. Only this copy's
    # exact command and application identity establish ownership for removal.
    $protocolPath = 'HKCU:\Software\Classes\ibrahim-alarm'
    $protocolCommandPath = $protocolPath + '\shell\open\command'
    if ((Test-Path -LiteralPath $protocolPath) -and (Test-Path -LiteralPath $protocolCommandPath)) {
        $owner = (Get-ItemProperty -LiteralPath $protocolPath -Name ALARMIdentity -ErrorAction SilentlyContinue).ALARMIdentity
        $command = (Get-Item -LiteralPath $protocolCommandPath).GetValue('')
        if ($owner -eq 'Ibrahim.ALARM.Desktop' -and $command -eq ('"' + $exePath + '" --open')) {
            Remove-Item -LiteralPath $protocolPath -Recurse
            $identityPath = 'HKCU:\Software\Classes\AppUserModelId\Ibrahim.ALARM.Desktop'
            if ((Test-Path -LiteralPath $identityPath) -and (Get-ItemProperty -LiteralPath $identityPath -Name ALARMIdentity -ErrorAction SilentlyContinue).ALARMIdentity -eq 'Ibrahim.ALARM.Desktop') {
                Remove-Item -LiteralPath $identityPath
            }
        }
    }
}
foreach ($path in $validated) { if (Test-Path -LiteralPath $path -PathType Leaf) { Remove-Item -LiteralPath $path } }
Remove-Item -LiteralPath $markerPath
# Remove only empty directories whose absolute paths remain inside the installation.
$directories = @($validated | ForEach-Object {
    $parent = Split-Path -Parent $_
    while ($parent -and $parent.StartsWith($installDir + '\', [StringComparison]::OrdinalIgnoreCase)) { $parent; $parent = Split-Path -Parent $parent }
}) | Sort-Object -Unique | Sort-Object Length -Descending
foreach ($directory in $directories) { if ((Test-Path -LiteralPath $directory) -and -not (Get-ChildItem -LiteralPath $directory -Force)) { Remove-Item -LiteralPath $directory } }
if (-not (Get-ChildItem -LiteralPath $installDir -Force)) { Remove-Item -LiteralPath $installDir }
Write-Output 'ALARM uninstalled. Personal settings and custom sound files were preserved.'
