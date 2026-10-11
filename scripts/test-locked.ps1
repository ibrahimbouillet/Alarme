# Copyright (c) 2026 Ibrahim Bouillet. SPDX-License-Identifier: MIT
param([switch]$LockNow)
$ErrorActionPreference = 'Stop'
if (-not $LockNow) { throw 'Use -LockNow explicitly to authorize locking this session.' }
$projectDir = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$binary = Join-Path $projectDir 'out\ALARM.exe'
if (-not (Test-Path -LiteralPath $binary)) { throw 'Build ALARM first.' }
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class AlarmLockedTest {
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr window,int id);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr window,uint msg,IntPtr wParam,IntPtr lParam);
}
'@
$testDir = Join-Path $projectDir ('.local\locked-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $testDir -Force | Out-Null
$config = @'
ALARM_CONFIG_V1
enabled=0
hour=6
minute=0
device_id=
wav_path=
session_volume=0.5
override_endpoint=0
endpoint_volume=0.5
startup=0
'@
[IO.File]::WriteAllText((Join-Path $testDir 'config.ini'),$config,[Text.UTF8Encoding]::new($false))
$alarmProcess = Start-Process -FilePath $binary -ArgumentList @('--data-dir',('"' + $testDir + '"')) -WindowStyle Normal -PassThru
Start-Sleep -Milliseconds 750
if ($alarmProcess.HasExited) { throw 'ALARM exited before lock.' }
$logPath = Join-Path $testDir 'alarm.log'
if (-not (Test-Path -LiteralPath $logPath)) { throw 'Alarm engine unavailable; lock cancelled.' }
@("PID=$($alarmProcess.Id)","BeforeLock=$((Get-Date).ToString('o'))","Scope=lock-only, no sleep request","SessionGain=50%; preview preserves master; alarm requests100% and unmute with restoration") | Set-Content -LiteralPath (Join-Path $testDir 'session.txt') -Encoding UTF8
$alarmProcess.Refresh()
$testButton = [AlarmLockedTest]::GetDlgItem($alarmProcess.MainWindowHandle,118)
if ($testButton -eq [IntPtr]::Zero) { throw 'Native locked-test button unavailable.' }
Write-Output "Starting native flow: 3s audible prelude, lock, alarm in10s,20s sample. Test directory: $testDir"
[AlarmLockedTest]::SendMessage($testButton,0xF5,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null
Start-Sleep -Seconds 45
Get-Content -LiteralPath $logPath -Tail 16
Write-Output 'Native flow controls STOP20s after actual audio start. Inspect its log; unlock Windows normally.'
