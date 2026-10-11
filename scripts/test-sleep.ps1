# Copyright (c) 2026 Ibrahim Bouillet. SPDX-License-Identifier: MIT
param([switch]$EnableWakeTimers, [switch]$SleepNow)
$ErrorActionPreference = 'Stop'
$projectDir = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$binary = Join-Path $projectDir 'out\ALARM.exe'
if (-not (Test-Path -LiteralPath $binary)) { throw 'Build first: powershell -ExecutionPolicy Bypass -File scripts\build.ps1' }
if (-not $SleepNow) { throw 'Physical sleep requires explicit -SleepNow. Use -EnableWakeTimers only to authorize AC policy changes.' }
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class AlarmSleepTest {
    [DllImport("powrprof.dll", SetLastError=true)]
    [return: MarshalAs(UnmanagedType.U1)]
    public static extern bool SetSuspendState([MarshalAs(UnmanagedType.U1)] bool hibernate, [MarshalAs(UnmanagedType.U1)] bool forceCritical, [MarshalAs(UnmanagedType.U1)] bool disableWakeEvents);
    [DllImport("kernel32.dll")] public static extern ulong GetTickCount64();
}
'@
$testDir = Join-Path $projectDir ('.local\wake-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $testDir -Force | Out-Null
$powerBefore = powercfg /query SCHEME_CURRENT SUB_SLEEP RTCWAKE
if ($LASTEXITCODE -ne 0) { throw 'Cannot read wake-timer policy.' }
$powerBefore | Set-Content -LiteralPath (Join-Path $testDir 'power-before.txt') -Encoding UTF8
if ($EnableWakeTimers) {
    powercfg /setacvalueindex SCHEME_CURRENT SUB_SLEEP RTCWAKE 1
    if ($LASTEXITCODE -ne 0) { throw 'Windows rejected AC wake timer activation.' }
    powercfg /setactive SCHEME_CURRENT
    if ($LASTEXITCODE -ne 0) { throw 'Windows rejected applying the active power plan.' }
}
$powerAfter = powercfg /query SCHEME_CURRENT SUB_SLEEP RTCWAKE
$powerAfter | Set-Content -LiteralPath (Join-Path $testDir 'power-after.txt') -Encoding UTF8
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
[IO.File]::WriteAllText((Join-Path $testDir 'config.ini'), $config, [Text.UTF8Encoding]::new($false))
# This visible interactive window is intentional for the user-authorized physical test.
$alarmProcess = Start-Process -FilePath $binary -ArgumentList @('--wake-test','--data-dir',('"' + $testDir + '"')) -WindowStyle Normal -PassThru
Start-Sleep -Milliseconds 750
if ($alarmProcess.HasExited) { throw 'ALARM exited before sleep; inspect its test log.' }
$logPath = Join-Path $testDir 'alarm.log'
if (-not (Test-Path -LiteralPath $logPath) -or -not ((Get-Content -LiteralPath $logPath -Raw) -match 'TEST armed')) { throw 'Alarm not armed; physical sleep cancelled.' }
$metadata = @("PID=$($alarmProcess.Id)","TestDir=$testDir","BeforeSleep=$((Get-Date).ToString('o'))","Cycle=300s ringing / 600s silent","MasterVolume=100% and unmuted during the alarm; previous state restored afterwards")
$metadata | Set-Content -LiteralPath (Join-Path $testDir 'session.txt') -Encoding UTF8
Write-Output "Alarm armed for 30 seconds. Test directory: $testDir"
Write-Output 'Entering physical sleep. If no automatic return after a minute, wake the PC manually.'
$beforeTick = [AlarmSleepTest]::GetTickCount64()
$suspended = [AlarmSleepTest]::SetSuspendState($false,$false,$false)
$suspendError = if ($suspended) { 0 } else { [Runtime.InteropServices.Marshal]::GetLastWin32Error() }
$afterTick = [AlarmSleepTest]::GetTickCount64()
@("SetSuspendState=$suspended","Win32Error=$suspendError","ElapsedMilliseconds=$($afterTick-$beforeTick)","AfterCall=$((Get-Date).ToString('o'))") | Add-Content -LiteralPath (Join-Path $testDir 'session.txt') -Encoding UTF8
powercfg /lastwake | Set-Content -LiteralPath (Join-Path $testDir 'lastwake.txt') -Encoding UTF8
Write-Output "SetSuspendState returned $suspended; elapsed $($afterTick-$beforeTick) ms; evidence: $testDir"
if (-not $suspended) { throw "Physical sleep failed (Windows error $suspendError). ALARM remains available; press STOP." }
