# Copyright (c) 2026 Ibrahim Bouillet. SPDX-License-Identifier: MIT
# Real per-user installer smoke test. Never removes an existing installation.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$InstallerPath,
    [string]$PayloadDirectory,
    [string]$ExpectedExecutable,
    [string]$OutputDirectory = '.local\installer-validation',
    [switch]$RunIntegration
)
$ErrorActionPreference = 'Stop'
if (-not [Environment]::Is64BitProcess -or $env:OS -ne 'Windows_NT') {
    throw 'Run this test in 64-bit Windows PowerShell or PowerShell.'
}
if ((-not $PayloadDirectory -and -not $ExpectedExecutable) -or ($PayloadDirectory -and $ExpectedExecutable)) {
    throw 'Supply either -PayloadDirectory or -ExpectedExecutable.'
}
$installer = (Resolve-Path -LiteralPath $InstallerPath).Path
if ($PayloadDirectory) { $ExpectedExecutable = Join-Path $PayloadDirectory 'ALARM.exe' }
$expectedExe = (Resolve-Path -LiteralPath $ExpectedExecutable).Path
$setupKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\ALARM.IbrahimBouillet.Setup_is1'
$startLink = Join-Path ([Environment]::GetFolderPath('Programs')) 'ALARM.lnk'
$desktopLink = Join-Path ([Environment]::GetFolderPath('Desktop')) 'ALARM.lnk'

# A clean Windows user is required. Do not overwrite another copy's registration
# or shortcuts, even if it has the same name and appears to be obsolete.
foreach ($root in @('HKCU:\Software', 'HKLM:\Software', 'HKCU:\Software\WOW6432Node', 'HKLM:\Software\WOW6432Node')) {
    $uninstallRoot = Join-Path $root 'Microsoft\Windows\CurrentVersion\Uninstall'
    if (-not (Test-Path -LiteralPath $uninstallRoot)) { continue }
    foreach ($key in Get-ChildItem -LiteralPath $uninstallRoot) {
        $entry = Get-ItemProperty -LiteralPath $key.PSPath
        if ($entry.DisplayName -eq 'ALARM' -or $key.PSChildName -in @('ALARM.IbrahimBouillet', 'ALARM.IbrahimBouillet.Setup_is1')) {
            throw 'An ALARM installation is already registered. Use a clean Windows user for installer tests.'
        }
    }
}
if ((Test-Path -LiteralPath $startLink) -or (Test-Path -LiteralPath $desktopLink)) {
    throw 'An ALARM shortcut already exists. Use a clean Windows user for installer tests.'
}
if (Get-Process -Name ALARM -ErrorAction SilentlyContinue) {
    throw 'Quit existing ALARM processes before running installer tests.'
}

function Get-PersonalFingerprint {
    $state = [Collections.Generic.List[string]]::new()
    foreach ($path in @((Join-Path $env:LOCALAPPDATA 'Ibrahim\ALARM'), (Join-Path $env:USERPROFILE '.alarm'))) {
        $state.Add($path + '|exists=' + (Test-Path -LiteralPath $path))
        if (Test-Path -LiteralPath $path) {
            foreach ($file in Get-ChildItem -LiteralPath $path -File -Force -Recurse | Sort-Object FullName) {
                $state.Add($file.FullName + '|' + (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash)
            }
        }
    }
    foreach ($path in @('HKCU:\Software\Classes\ibrahim-alarm', 'HKCU:\Software\Classes\AppUserModelId\Ibrahim.ALARM.Desktop')) {
        $state.Add($path + '|exists=' + (Test-Path -LiteralPath $path))
        if (Test-Path -LiteralPath $path) {
            $keys = @((Get-Item -LiteralPath $path)) + @(Get-ChildItem -LiteralPath $path -Recurse)
            foreach ($key in $keys | Sort-Object Name) {
                foreach ($name in $key.GetValueNames() | Sort-Object) {
                    $state.Add($key.Name + '|' + $name + '|' + ($key.GetValue($name) | ConvertTo-Json -Compress))
                }
            }
        }
    }
    $startupKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
    $startup = Get-ItemProperty -LiteralPath $startupKey -Name 'ALARM.Ibrahim' -ErrorAction SilentlyContinue
    $state.Add('startup|' + ($startup.'ALARM.Ibrahim' | ConvertTo-Json -Compress))
    return ($state -join "`n")
}
function Invoke-TestProcess([string]$File, [string[]]$Arguments) {
    $process = Start-Process -FilePath $File -ArgumentList $Arguments -WindowStyle Hidden -PassThru
    try {
        if (-not $process.WaitForExit(60000)) {
            $process.Kill()
            throw 'A test process exceeded 60 seconds; inspect the private test logs before retrying.'
        }
        $process.Refresh()
        return $process.ExitCode
    } finally { $process.Dispose() }
}
$checks = [Collections.Generic.List[object]]::new()
function Assert-Check([string]$Name, [bool]$Condition) {
    $script:phase = $Name
    if (-not $Condition) { throw ('Installer validation failed: ' + $Name) }
    $checks.Add([ordered]@{ name = $Name; passed = $true })
    Write-Host ('PASS ' + $Name)
}
function Test-Fixture([string]$Path, [string]$Hash) {
    return (Test-Path -LiteralPath $Path -PathType Leaf) -and ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -eq $Hash)
}
function Get-OwnedUninstaller {
    if (-not (Test-Path -LiteralPath $setupKey)) { return $null }
    $registered = Get-ItemProperty -LiteralPath $setupKey
    if ([IO.Path]::GetFullPath($registered.InstallLocation).TrimEnd('\') -ne $installDir) {
        throw 'The ALARM registration changed ownership during the test; no cleanup was attempted.'
    }
    $owner = Join-Path $installDir '.alarm-setup-owner'
    if (-not (Test-Path -LiteralPath $owner) -or (Get-Content -LiteralPath $owner -Raw).Trim() -ne 'ALARM-INNO-SETUP-v1') {
        throw 'The test installation ownership marker is missing; no cleanup was attempted.'
    }
    $path = Join-Path $installDir 'unins000.exe'
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw 'The test uninstaller is missing.' }
    return $path
}

$output = [IO.Path]::GetFullPath($OutputDirectory)
$ancestor = $output
while ($ancestor) {
    if ((Test-Path -LiteralPath $ancestor) -and ((Get-Item -LiteralPath $ancestor).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw 'The validation directory cannot be inside a symbolic link or junction.'
    }
    $ancestor = Split-Path -Parent $ancestor
}
$runRoot = Join-Path $output ('run-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $runRoot | Out-Null
$installDir = Join-Path $runRoot 'application'
$occupiedDir = Join-Path $runRoot 'occupied'
New-Item -ItemType Directory -Path $occupiedDir | Out-Null
$occupiedFixture = Join-Path $occupiedDir 'unrelated.txt'
[IO.File]::WriteAllText($occupiedFixture, 'Unrelated files must survive a refused installation.')
$occupiedHash = (Get-FileHash -LiteralPath $occupiedFixture -Algorithm SHA256).Hash
$expectedHash = (Get-FileHash -LiteralPath $expectedExe -Algorithm SHA256).Hash
$personalBefore = Get-PersonalFingerprint
$phase = 'start'
$failure = $null
$cleanupFailure = $false
$report = [ordered]@{
    schemaVersion = 1
    installerSha256 = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant()
    executableSha256 = $expectedHash.ToLowerInvariant()
    integrationRequested = [bool]$RunIntegration
    passed = $false
    failedCheck = $null
    cleanupPassed = $true
    checks = $checks
}
try {
    $phase = 'occupied directory refusal'
    $code = Invoke-TestProcess $installer @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', ('/DIR="' + $occupiedDir + '"'), ('/LOG="' + (Join-Path $runRoot 'refused.log') + '"'))
    Assert-Check 'occupied directory is refused' ($code -ne 0)
    Assert-Check 'refused setup preserves unrelated files' (Test-Fixture $occupiedFixture $occupiedHash)
    Assert-Check 'refused setup leaves no registration or shortcut' ((-not (Test-Path -LiteralPath $setupKey)) -and (-not (Test-Path -LiteralPath $startLink)) -and (-not (Test-Path -LiteralPath (Join-Path $occupiedDir 'ALARM.exe'))))

    $phase = 'silent installation'
    $code = Invoke-TestProcess $installer @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', ('/DIR="' + $installDir + '"'), ('/LOG="' + (Join-Path $runRoot 'install.log') + '"'))
    Assert-Check 'silent installation exits successfully' ($code -eq 0)
    $installedExe = Join-Path $installDir 'ALARM.exe'
    Assert-Check 'installed executable matches exact payload' (Test-Fixture $installedExe $expectedHash)
    Assert-Check 'uninstall registration exists' (Test-Path -LiteralPath $setupKey)
    $registered = Get-ItemProperty -LiteralPath $setupKey
    Assert-Check 'uninstall registration points to isolated target' (([IO.Path]::GetFullPath($registered.InstallLocation).TrimEnd('\') -eq $installDir) -and ($registered.UninstallString -eq ('"' + (Join-Path $installDir 'unins000.exe') + '"')))
    Assert-Check 'Start menu shortcut exists' (Test-Path -LiteralPath $startLink)
    $shell = New-Object -ComObject WScript.Shell
    try {
        $link = $shell.CreateShortcut($startLink)
        Assert-Check 'Start menu shortcut points to installed application' (($link.TargetPath -eq $installedExe) -and ($link.WorkingDirectory -eq $installDir))
    } finally { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shell) }
    Assert-Check 'desktop shortcut is unchecked by default' (-not (Test-Path -LiteralPath $desktopLink))
    Assert-Check 'setup does not launch ALARM' (-not (Get-Process -Name ALARM -ErrorAction SilentlyContinue))

    $settingsFixture = Join-Path $installDir 'config.ini'
    $unrelatedFixture = Join-Path $installDir 'user-files\unrelated.txt'
    New-Item -ItemType Directory -Path (Split-Path -Parent $unrelatedFixture) | Out-Null
    [IO.File]::WriteAllText($settingsFixture, "[alarm]`nhour=7`nminute=35`nenabled=false`n")
    [IO.File]::WriteAllText($unrelatedFixture, 'User-owned content must survive update and removal.')
    $settingsHash = (Get-FileHash -LiteralPath $settingsFixture -Algorithm SHA256).Hash
    $unrelatedHash = (Get-FileHash -LiteralPath $unrelatedFixture -Algorithm SHA256).Hash
    $phase = 'reinstallation with unrelated files'
    $code = Invoke-TestProcess $installer @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', ('/DIR="' + $installDir + '"'), ('/LOG="' + (Join-Path $runRoot 'reinstall.log') + '"'))
    Assert-Check 'reinstallation exits successfully' ($code -eq 0)
    Assert-Check 'reinstallation preserves settings fixture' (Test-Fixture $settingsFixture $settingsHash)
    Assert-Check 'reinstallation preserves unrelated file' (Test-Fixture $unrelatedFixture $unrelatedHash)
    Assert-Check 'reinstalled executable matches exact payload' (Test-Fixture $installedExe $expectedHash)

    if ($RunIntegration) {
        $phase = 'installed application integration'
        $integrationDir = Join-Path $runRoot 'integration-data'
        $code = Invoke-TestProcess $installedExe @('--integration-test', '--data-dir', ('"' + $integrationDir + '"'))
        Assert-Check 'installed application integration exits successfully' ($code -eq 0)
        $integrationReport = Join-Path $integrationDir 'integration.txt'
        Assert-Check 'installed application integration produces passing report' ((Test-Path -LiteralPath $integrationReport) -and ((Get-Content -LiteralPath $integrationReport -Raw) -match '^PASS real Windows timer'))
    }

    $phase = 'silent uninstallation'
    $uninstaller = Get-OwnedUninstaller
    Assert-Check 'owned uninstaller exists' ([bool]$uninstaller)
    $code = Invoke-TestProcess $uninstaller @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', ('/LOG="' + (Join-Path $runRoot 'uninstall.log') + '"'))
    Assert-Check 'silent uninstallation exits successfully' ($code -eq 0)
    Assert-Check 'uninstallation removes executable and registration' ((-not (Test-Path -LiteralPath $installedExe)) -and (-not (Test-Path -LiteralPath $setupKey)))
    Assert-Check 'uninstallation removes owned shortcuts' ((-not (Test-Path -LiteralPath $startLink)) -and (-not (Test-Path -LiteralPath $desktopLink)))
    Assert-Check 'uninstallation preserves settings fixture' (Test-Fixture $settingsFixture $settingsHash)
    Assert-Check 'uninstallation preserves unrelated file' (Test-Fixture $unrelatedFixture $unrelatedHash)
    Assert-Check 'personal settings and existing system integration remain unchanged' ((Get-PersonalFingerprint) -ceq $personalBefore)
    $report.passed = $true
} catch {
    $failure = $_
    $report.failedCheck = $phase
} finally {
    # Only invoke the embedded uninstaller when its exact registration and owner
    # marker prove that this run owns it. Never recursively delete test trees.
    try {
        if (Test-Path -LiteralPath $setupKey) {
            $uninstaller = Get-OwnedUninstaller
            $cleanupCode = Invoke-TestProcess $uninstaller @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', ('/LOG="' + (Join-Path $runRoot 'cleanup.log') + '"'))
            if ($cleanupCode -ne 0 -or (Test-Path -LiteralPath $setupKey)) { throw 'Test installation cleanup failed.' }
        }
    } catch { $cleanupFailure = $true; $report.cleanupPassed = $false; $report.passed = $false }
    # This report contains only fixed check names, booleans and hashes. Setup logs
    # contain local paths and stay in the ignored validation directory.
    [IO.File]::WriteAllText((Join-Path $runRoot 'report.json'), ($report | ConvertTo-Json -Depth 6), [Text.UTF8Encoding]::new($false))
}
if ($failure) { throw $failure }
if ($cleanupFailure) { throw 'Test installation cleanup failed; inspect the private validation directory.' }
Write-Host 'Installer lifecycle validation passed. The report and private logs are in the validation output directory.'
