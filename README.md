# ALARM

ALARM is a native Windows desktop alarm programmed by **Ibrahim Bouillet**. Set a daily time, choose your sound, and keep the app running in the notification area. The interface uses Direct3D, Direct2D, and DirectWrite for its glass effects; the alarm engine runs independently of the drawing loop.

## Install and start

Requirements: Windows 10 or 11, a 64-bit PC, an audio output, and a Direct3D 11 compatible graphics device.

1. Download [ALARM 0.6.1 for Windows x64](https://github.com/ibrahimbouillet/Alarme/releases/download/v0.6.1/ALARM-0.6.1-windows-x64-setup.exe).
2. Double-click `ALARM-0.6.1-windows-x64-setup.exe` and follow the green setup wizard.
3. Open **ALARM** from the Start menu.

The installer works offline for the current Windows user without administrator rights. It installs into `%LOCALAPPDATA%\Programs\ALARM`, creates a Start menu shortcut, and adds ALARM to Windows installed apps. Installation does not launch the app or enable an alarm automatically. You do not need PowerShell commands, developer tools, an account, or a separate .NET runtime.

The installer and application are unsigned, so Windows SmartScreen may display an unknown-publisher warning. Only continue if you trust the download from this repository's [release page](https://github.com/ibrahimbouillet/Alarme/releases/tag/v0.6.1).

If ALARM was previously installed using `install.ps1`, uninstall that copy from Windows installed apps before running the graphical installer. Your saved settings are preserved.

For portable use, download [the ZIP package](https://github.com/ibrahimbouillet/Alarme/releases/download/v0.6.1/ALARM-0.6.1-windows-x64.zip), extract it, and open `ALARM.exe`. Keep that folder in a stable location if you enable startup with Windows. The ZIP also retains the optional `install.ps1` script; see the [installation guide](INSTALL.md). GitHub's **Source code** downloads are for developers.

## Set your first alarm

1. Click the large time, enter a time in `HH:MM` format, and press Enter to save it. Times use the Windows time zone and a 24-hour clock.
2. Open **Settings** to choose an audio output, adjust the alarm volume, or select a WAV file. Save your settings.
3. Use **Listen to sound** to check the sound at the current Windows volume.
4. Enable the alarm on the main screen. ALARM remembers the time and enabled state for future launches.

The initial time is **06:00**, and the alarm starts disabled. At the scheduled time it repeats **5 minutes of sound, then 10 minutes of silence**, until you press **STOP**. STOP ends the current cycle; an enabled daily alarm stays scheduled for its next occurrence. Disable the daily alarm separately when you no longer want it.

**During an alarm, ALARM sets the selected Windows audio output to 100% and unmutes it.** Your alarm volume setting still controls ALARM's own sound level. The previous output volume and mute state are restored during silence, on STOP, and on normal exit. The sound preview preserves the Windows output volume and mute state. A forced process termination or unavailable audio device can prevent restoration.

Closing the window keeps ALARM running near the Windows clock. Click its icon to reopen it. Choose **Quit** in Settings or the notification area menu to stop the app completely; alarms cannot ring after you quit.

See the [user guide](docs/PRODUCT.md) for sound formats, startup, diagnostics, and troubleshooting.

## Sleep, lock, and shutdown

ALARM requests a Windows wake timer and can play audio while the session is locked. Wake behavior depends on your hardware, firmware, Windows power plan, and audio drivers. Modern Standby and a closed laptop lid can prevent the process or audio device from running as expected. A successful timer request does not prove that an alarm will be audible on your PC.

**Test wake and locked-session audio on your own computer before relying on them.** ALARM cannot run when the PC is fully shut down, or before the app has started after sign-in. Windows notifications may be suppressed by system settings; the alarm sound is separate from notifications. See the [hardware test guide](docs/MANUAL_VALIDATION.md) and [technical limits](TECHNICAL_FEASIBILITY.md).

## Build from source

You need Windows, CMake 3.25 or later, and a C++20 compiler. The default build uses Ninja and GCC available on `PATH`; [MSYS2 UCRT64](https://www.msys2.org/docs/environments/) is one way to provide these tools. Put the UCRT64 `bin` directory on the PowerShell session's `PATH`, then run from the repository root:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1 -BuildDirectory build -OutputDirectory out
.\out\ALARM.exe
```

The script builds the application and runs the default test suite. An alternative `windows-msvc` preset is provided for Visual Studio 2022 with the Desktop development with C++ workload and a Windows SDK:

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-msvc
ctest --preset windows-msvc
```

The release workflow is validated with GCC UCRT64. The Visual Studio preset requires verification with your installed toolchain; see the [developer guide](docs/DEVELOPMENT_STRATEGY.md#build-requirements) for tool installation.

Create the portable archive with:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\package.ps1
```

The archive is written to `out\releases\ALARM-0.6.1-windows-x64.zip`. To create the graphical installer from that exact payload, install [Inno Setup 6](https://jrsoftware.org/isdl.php) and follow the [packaging instructions](docs/PUBLISHING.md#build-the-windows-installer). Optional graphics demos and the experimental Windows widget provider have separate dependencies and are not part of the standard alarm installation.

Read the [developer guide](docs/DEVELOPMENT_STRATEGY.md) and [contribution guide](CONTRIBUTING.md) for the project layout, optional builds, and validation. The [publishing guide](docs/PUBLISHING.md) explains how to prepare a clean public source snapshot.

## Files and privacy

Settings and local diagnostic logs are stored in `%LOCALAPPDATA%\Ibrahim\ALARM`. A selected WAV stays at its original location; ALARM saves its path rather than copying the file. The normal app also writes a small presentation state file to `%USERPROFILE%\.alarm\widget-state.json` for the optional widget prototype. That state includes the chosen sound's file name, without its full path or audio device ID.

The alarm works offline and has no telemetry or account requirement. Logs can contain local paths and audio device details; review them before attaching them to a public issue. Uninstallation preserves your personal settings and logs.

## Author and license

Programmer: **Ibrahim Bouillet**. ALARM is distributed under the [MIT License](LICENSE). [Third-party notices](THIRD_PARTY_NOTICES.md) are retained for the bundled runtimes and optional widget prototype.
