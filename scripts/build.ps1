# Copyright (c) 2026 Ibrahim Bouillet. SPDX-License-Identifier: MIT
[CmdletBinding()]
param(
    [string]$BuildDirectory = 'build',
    [string]$OutputDirectory = 'out',
    [switch]$WithDemos,
    [ValidateRange(1, 64)][int]$Jobs = 4,
    [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'
$projectDir = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
function Resolve-ProjectPath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $projectDir $Path))
}
$buildDir = Resolve-ProjectPath $BuildDirectory
$deliveryDir = Resolve-ProjectPath $OutputDirectory
foreach ($tool in @('cmake', 'ninja', 'g++', 'ctest')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "Missing $tool. Install the MSYS2 UCRT64 GCC/CMake/Ninja packages and add your UCRT64 bin directory to PATH. See README.md."
    }
}
Push-Location -LiteralPath $projectDir
try {
    $demoOption = if ($WithDemos) { 'ON' } else { 'OFF' }
    cmake -S $projectDir -B $buildDir -G Ninja '-DCMAKE_BUILD_TYPE=Release' '-DCMAKE_CXX_COMPILER=g++' '-DBUILD_TESTING=ON' "-DALARM_BUILD_DEMOS=$demoOption"
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
    cmake --build $buildDir --parallel $Jobs
    if ($LASTEXITCODE -ne 0) { throw 'C++ build failed.' }
    if (-not $SkipTests) {
        ctest --test-dir $buildDir --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw 'Tests failed; delivery was not updated.' }
    }
    New-Item -ItemType Directory -Path $deliveryDir -Force | Out-Null
    foreach ($name in @('ALARM.exe', 'alarm_probe.exe', 'alarm_audio_diagnostic.exe')) {
        $source = Join-Path $buildDir $name
        $destination = Join-Path $deliveryDir $name
        if ((Test-Path -LiteralPath $destination) -and (Get-FileHash -LiteralPath $source).Hash -eq (Get-FileHash -LiteralPath $destination).Hash) { continue }
        try { Copy-Item -LiteralPath $source -Destination $destination -Force }
        catch { throw "Cannot update $destination. Quit ALARM normally, then run the build again. $($_.Exception.Message)" }
    }
    if ($WithDemos) { Copy-Item -LiteralPath (Join-Path $buildDir 'ALARM_Liquid_Demo.exe') -Destination $deliveryDir -Force }
    Write-Output "Portable executables: $deliveryDir"
} finally { Pop-Location }
