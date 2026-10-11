# Copyright (c) 2026 Ibrahim Bouillet. SPDX-License-Identifier: MIT
# Export the current public working tree without commit history or local artifacts.
[CmdletBinding()]
param([string]$OutputDirectory = 'out\releases')
$ErrorActionPreference = 'Stop'
$projectDir = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw 'Git is required to select the public working-tree files.' }
$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName = 'git'
$start.WorkingDirectory = $projectDir
$start.Arguments = 'ls-files --cached --others --exclude-standard -z'
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$process = [Diagnostics.Process]::Start($start)
$inventory = $process.StandardOutput.ReadToEnd()
$failure = $process.StandardError.ReadToEnd()
$process.WaitForExit()
if ($process.ExitCode -ne 0) { throw "Cannot inventory the source checkout: $failure" }
$process.Dispose()
$cmake = Get-Content -LiteralPath (Join-Path $projectDir 'CMakeLists.txt') -Raw
if ($cmake -notmatch 'project\(Alarm VERSION ([0-9.]+)') { throw 'Cannot read the source version.' }
$name = 'ALARM-' + $Matches[1] + '-source'
$stage = Join-Path $projectDir ('.local\source-exports\' + $name + '-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage -Force | Out-Null
$count = 0
foreach ($relative in @($inventory.Split([char]0) | Sort-Object -Unique)) {
    if (-not $relative) { continue }
    if ($relative -match '(^|/)(\.git|\.local|out|build[^/]*|bin|obj|package|evidence)/' -or $relative -match '(\.log|\.exe|\.msix|\.pfx|\.p12|\.pem|\.key)$' -or $relative -match '(^|/)\.env($|\.)' -or $relative -eq 'CMakeUserPresets.json') { continue }
    $source = [IO.Path]::GetFullPath((Join-Path $projectDir $relative))
    if (-not $source.StartsWith($projectDir + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Source path escapes the project.' }
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { continue }
    if ((Get-Item -LiteralPath $source).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Source export does not include symbolic links.' }
    $destination = Join-Path $stage $relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination
    $count++
}
if ([IO.Path]::IsPathRooted($OutputDirectory)) { $outputDir = [IO.Path]::GetFullPath($OutputDirectory) }
else { $outputDir = [IO.Path]::GetFullPath((Join-Path $projectDir $OutputDirectory)) }
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
$archive = Join-Path $outputDir ($name + '.zip')
Add-Type -AssemblyName System.IO.Compression.FileSystem
# Create a new ZIP before replacing a previous export, preserving it on failure.
$temporary = $archive + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
[IO.Compression.ZipFile]::CreateFromDirectory($stage, $temporary)
Move-Item -LiteralPath $temporary -Destination $archive -Force
$hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText(($archive + '.sha256'), "$hash  $name.zip`n", [Text.UTF8Encoding]::new($false))
Write-Output "Clean source snapshot ($count files): $archive"
Write-Output "SHA256: $hash"
