# ALARM

ALARM is a native Windows alarm programmed by **Ibrahim Bouillet**.

## Install and start

Requirements: Windows 10 or 11, a 64-bit PC, an audio output and a Direct3D 11
compatible graphics device. This package needs no compiler, account or .NET runtime.

1. Extract the release ZIP into a folder.
2. Open PowerShell in that folder and run:

   ```powershell
   powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\install.ps1
   ```

3. Open **ALARM** from the Start menu.

Installation is for the current user, without administrator rights. The default
location is `%LOCALAPPDATA%\Programs\ALARM`. A Start menu shortcut and Windows
installed-app entry are created. Installation never starts the app automatically.

For portable use, launch `ALARM.exe` directly from the extracted folder. Keep it
at a stable location if you enable startup with Windows. The executable and
PowerShell scripts are unsigned.

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

Quit ALARM first, then use its Windows installed-app entry or run `uninstall.ps1`
from the installation folder. Your settings, logs and custom WAV files are preserved.
For portable use, disable Windows startup, quit ALARM and remove its folder.

## License

Copyright (c) 2026 **Ibrahim Bouillet**. Distributed under the [MIT License](LICENSE).
Bundled runtime notices are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
