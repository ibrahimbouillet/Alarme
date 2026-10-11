# Copyright (c) 2026 Ibrahim Bouillet. SPDX-License-Identifier: MIT
[CmdletBinding()]
param(
    [string]$BuildDirectory = 'build',
    [string]$OutputDirectory = 'out\releases',
    [switch]$SkipBuild,
    [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'
$projectDir = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
function Resolve-ProjectPath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $projectDir $Path))
}
$buildDir = Resolve-ProjectPath $BuildDirectory
$outputDir = Resolve-ProjectPath $OutputDirectory
if (-not $SkipBuild) { & (Join-Path $PSScriptRoot 'build.ps1') -BuildDirectory $buildDir -SkipTests:$SkipTests }
elseif (-not $SkipTests) {
    ctest --test-dir $buildDir --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed; no package was created.' }
}
$executable = Join-Path $buildDir 'ALARM.exe'
if (-not (Test-Path -LiteralPath $executable)) { throw 'Build ALARM before packaging.' }
$version = (Get-Item -LiteralPath $executable).VersionInfo.ProductVersion
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'Missing or invalid ALARM version metadata.' }
$name = "ALARM-$version-windows-x64"
$stage = Join-Path $projectDir ('.local\packages\' + $name + '-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage -Force | Out-Null
Copy-Item -LiteralPath $executable -Destination (Join-Path $stage 'ALARM.exe')
Copy-Item -LiteralPath (Join-Path $projectDir 'INSTALL.md') -Destination (Join-Path $stage 'README.md')
foreach ($file in @('TECHNICAL_FEASIBILITY.md', 'LICENSE', 'THIRD_PARTY_NOTICES.md')) { Copy-Item -LiteralPath (Join-Path $projectDir $file) -Destination $stage }
foreach ($file in @('install.ps1', 'uninstall.ps1')) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination $stage }
New-Item -ItemType Directory -Path (Join-Path $stage 'docs') -Force | Out-Null
foreach ($file in @('PRODUCT.md', 'MANUAL_VALIDATION.md')) { Copy-Item -LiteralPath (Join-Path $projectDir ('docs\' + $file)) -Destination (Join-Path $stage 'docs') }
Copy-Item -LiteralPath (Join-Path $projectDir 'licenses') -Destination $stage -Recurse
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
$archive = Join-Path $outputDir ($name + '.zip')
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $archive -Force
$hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText(($archive + '.sha256'), "$hash  $name.zip`n", [Text.UTF8Encoding]::new($false))
Write-Output "Release ZIP: $archive"
Write-Output "SHA256: $hash"
