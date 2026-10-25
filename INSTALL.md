# ALARM

ALARM is a native Windows alarm programmed by **Ibrahim Bouillet**.

## Install and start

Requirements: Windows 10 or 11, a 64-bit PC, an audio output and a Direct3D 11
compatible graphics device. This package needs no compiler, account or .NET runtime.

1. Download [ALARM-0.6.1-windows-x64-setup.exe](https://github.com/ibrahimbouillet/Alarme/releases/download/v0.6.1/ALARM-0.6.1-windows-x64-setup.exe).
2. Double-click it and follow the green setup wizard.
3. Open **ALARM** from the Start menu.

Installation is for the current user, without administrator rights. The default
location is `%LOCALAPPDATA%\Programs\ALARM`. A Start menu shortcut and Windows
installed-app entry are created. Installation works offline and never starts the
app or enables an alarm automatically. No terminal commands are required.

The installer and application are unsigned. Windows SmartScreen may show an
unknown-publisher warning; only continue if you trust the download from the
[official release page](https://github.com/ibrahimbouillet/Alarme/releases/tag/v0.6.1).

## Portable ZIP and scripted installation

Download [ALARM-0.6.1-windows-x64.zip](https://github.com/ibrahimbouillet/Alarme/releases/download/v0.6.1/ALARM-0.6.1-windows-x64.zip)
and extract it into a folder. GitHub's **Source code** downloads do not contain a
ready-to-run application.

For portable use, launch `ALARM.exe` directly from the extracted folder. Keep it
at a stable location if you enable startup with Windows. The executable and
PowerShell scripts are unsigned. To install from the ZIP instead, open PowerShell
in its extracted folder and run:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\install.ps1
```

Then open **ALARM** from the Start menu. Quit an existing instance before installing
or upgrading with either installer. Your saved alarm settings are preserved.

If you previously installed ALARM with `install.ps1`, uninstall that copy first
from Windows installed apps, then run the graphical installer. Your personal
settings and custom sounds are kept.

## Set your first alarm

Click the large time and enter `HH:MM`, then press Enter. Open Settings, choose
your audio output and sound, and save. Use **Listen to sound** to check the sound,
then **Enable alarm**. The first launch defaults to 06:00 with the alarm disabled.

At the scheduled time, the alarm repeats five minutes of sound and ten minutes
of silence until you press **STOP**. STOP ends the current cycle; an enabled daily
alarm remains scheduled for tomorrow. Disable the daily alarm separately to turn
future alarms off.

**A ringing alarm sets its selected Windows output to 100% and unmutes it.** The
previous level and mute are restored during silence, at STOP and on normal exit.
ALARM's own sound volume is separate. The sound preview preserves Windows volume.

Closing the window leaves ALARM running near the Windows clock. Use **Quit** from
Settings or the icon menu to exit. Alarms require the application to be running.

## Power and sound

Wake from sleep and locked-session sound depend on your hardware and Windows
settings. Modern Standby or a closed laptop lid can prevent audio. Test on your
own computer before relying on wake behavior. ALARM cannot ring after full shutdown.

Read the [user guide](docs/PRODUCT.md),
[hardware checks](docs/MANUAL_VALIDATION.md) and
[technical limits](TECHNICAL_FEASIBILITY.md).

## Uninstall

Quit ALARM first, then use **Settings > Apps > Installed apps > ALARM > Uninstall**
(**Apps & features** on Windows 10). For a scripted ZIP installation, you can also
run `uninstall.ps1` from its installation folder. Your settings, logs and custom
WAV files are preserved.
For portable use, disable Windows startup, quit ALARM and remove its folder.

## License

Copyright (c) 2026 **Ibrahim Bouillet**. Distributed under the [MIT License](LICENSE).
Bundled runtime notices are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
