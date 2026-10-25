# Copyright (c) 2026 Ibrahim Bouillet. SPDX-License-Identifier: MIT
# Wrap an existing release in an offline installer; never rebuild ALARM.exe.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PayloadDirectory,
    [string]$OutputDirectory = 'out\releases',
    [string]$IsccPath,
    [string]$ExpectedVersion,
    [string]$ExpectedExecutableSha256
)
$ErrorActionPreference = 'Stop'
$projectDir = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
function Resolve-ProjectPath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $projectDir $Path))
}
$payloadDir = Resolve-ProjectPath $PayloadDirectory
$outputDir = Resolve-ProjectPath $OutputDirectory
if (-not (Test-Path -LiteralPath $payloadDir -PathType Container)) { throw 'PayloadDirectory must be an extracted ALARM release folder.' }
$ancestor = Get-Item -LiteralPath $payloadDir
while ($ancestor) {
    if ($ancestor.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Release payload must not use symbolic links or junctions.' }
    $ancestor = $ancestor.Parent
}
$required = @('ALARM.exe', 'README.md', 'TECHNICAL_FEASIBILITY.md', 'LICENSE', 'THIRD_PARTY_NOTICES.md', 'docs\PRODUCT.md', 'docs\MANUAL_VALIDATION.md')
foreach ($relative in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $payloadDir $relative) -PathType Leaf)) { throw "Release payload is incomplete: $relative is missing." }
}
if (-not (Test-Path -LiteralPath (Join-Path $payloadDir 'licenses') -PathType Container)) { throw 'Release payload is incomplete: licenses is missing.' }
foreach ($item in Get-ChildItem -LiteralPath $payloadDir -Recurse -Force) {
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Release payload must not contain symbolic links or junctions.' }
    if ($item.PSIsContainer) { continue }
    $relative = $item.FullName.Substring($payloadDir.Length + 1)
    if ($relative -notin ($required + @('install.ps1', 'uninstall.ps1')) -and $relative -notmatch '^licenses\\[a-zA-Z0-9._-]+\\[a-zA-Z0-9._-]+$') {
        throw "Unexpected file in release payload: $relative. Review the packaging allowlist before including it."
    }
}
if (-not (Get-ChildItem -LiteralPath (Join-Path $payloadDir 'licenses') -Recurse -File)) { throw 'Release payload has no third-party license files.' }
$executable = Join-Path $payloadDir 'ALARM.exe'
$version = (Get-Item -LiteralPath $executable).VersionInfo.ProductVersion
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'Missing or invalid ALARM version metadata.' }
if ($ExpectedVersion -and $version -ne $ExpectedVersion.TrimStart('v')) { throw "Payload version $version does not match expected version $ExpectedVersion." }
$exeHash = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant()
if ($ExpectedExecutableSha256 -and $exeHash -ne $ExpectedExecutableSha256.ToLowerInvariant()) { throw 'ALARM.exe does not match the expected release SHA256.' }
$reader = [IO.BinaryReader]::new([IO.File]::OpenRead($executable))
try {
    if ($reader.ReadUInt16() -ne 0x5A4D) { throw 'ALARM.exe is not a Windows executable.' }
    $reader.BaseStream.Position = 0x3C
    $peOffset = $reader.ReadInt32()
    if ($peOffset -lt 64 -or $peOffset -gt ($reader.BaseStream.Length - 6)) { throw 'Invalid ALARM PE header.' }
    $reader.BaseStream.Position = $peOffset
    if ($reader.ReadUInt32() -ne 0x00004550 -or $reader.ReadUInt16() -ne 0x8664) { throw 'This installer requires an x64 Windows ALARM executable.' }
} finally { $reader.Dispose() }
if (-not $IsccPath) {
    $command = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($command) { $IsccPath = $command.Source }
    else {
        foreach ($candidate in @((Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'), (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'))) {
            if (Test-Path -LiteralPath $candidate -PathType Leaf) { $IsccPath = $candidate; break }
        }
    }
}
if (-not $IsccPath -or -not (Test-Path -LiteralPath $IsccPath -PathType Leaf)) { throw 'Install Inno Setup 6.7+ from https://jrsoftware.org/isdl.php or pass -IsccPath to ISCC.exe.' }
$IsccPath = [IO.Path]::GetFullPath($IsccPath)
# ISCC's Windows file version can be 0.0.0.0. ALARM.iss checks the compiler's
# own preprocessor version instead of relying on that resource.
$stage = Join-Path $projectDir ('.local\installer\' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage -Force | Out-Null
& (Join-Path $projectDir 'installer\New-Branding.ps1') -OutputDirectory $stage
[IO.File]::WriteAllText((Join-Path $stage '.alarm-setup-owner'), "ALARM-INNO-SETUP-v1`n", [Text.UTF8Encoding]::new($false))
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
$name = "ALARM-$version-windows-x64-setup.exe"
$installer = Join-Path $outputDir $name
if (Test-Path -LiteralPath $installer) { throw 'Installer output already exists. Choose a new OutputDirectory or remove the previous artifact explicitly.' }
& $IsccPath '/Qp' ("/DPayloadDir=$payloadDir") ("/DBrandingDir=$stage") ("/DAppVersion=$version") ("/DInstallerOutputDir=$outputDir") (Join-Path $projectDir 'installer\ALARM.iss')
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $installer -PathType Leaf)) { throw 'Inno Setup compilation failed; no release is ready for publication.' }
if ((Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant() -ne $exeHash) { throw 'The release executable changed during packaging; do not publish this installer.' }
$hash = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText(($installer + '.sha256'), "$hash  $name`n", [Text.UTF8Encoding]::new($false))
Write-Output "Installer: $installer"
Write-Output "SHA256: $hash"
Write-Output "Unchanged ALARM.exe SHA256: $exeHash"
