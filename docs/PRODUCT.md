# User guide

ALARM is a native Windows alarm by **Ibrahim Bouillet**. Start with the [installation instructions](../README.md#install-and-start).

## Time and daily activation

Click the large time to edit it. Enter `HH:MM` between `00:00` and `23:59`, then press Enter or save the time. Escape cancels an unsaved edit. The clock uses your Windows time zone.

Enable the alarm from the main screen. The saved time and enabled state survive normal application restarts. The initial time is 06:00, with the daily alarm disabled. If today's time has already passed, ALARM schedules the next daily occurrence.

Once triggered, the cycle repeats five minutes of ringing and ten minutes of silence. Press STOP to end that cycle or cancel a pending diagnostic alarm. STOP keeps an enabled daily alarm scheduled for its next occurrence. Disable the daily alarm separately to turn future alarms off.

Changing saved settings or choosing another sound ends an active cycle and recalculates the daily schedule. Changes to the audio output, alarm volume, and startup checkbox take effect when you save Settings. Choosing a WAV or returning to the built-in sound saves that sound choice immediately.

## Sound and volume

In Settings, choose the Windows default output or a specific listed output. The Windows default follows the system's default output; a saved specific device must be available when the alarm rings. Use the built-in tone if you do not want to manage a sound file.

The alarm volume ranges from 0 to 100%. A value of 0 makes ALARM's stream silent. For a real alarm, ALARM raises the selected Windows output to 100% and unmutes it, while retaining your chosen ALARM volume. This also affects other applications using that output. The prior Windows volume and mute state are restored at silence, STOP, suspension, and normal exit. If Windows or the device refuses restoration, check the output volume manually.

Listen to sound starts a preview for eight seconds after audio playback begins. It preserves the Windows output volume and mute state, so a muted Windows output remains muted. Preparing the preview is cancellable and limited to 30 seconds. Audio errors appear in the interface; they do not count as a successful sound check.

Custom sounds must be WAV files with the following formats:

| Property | Supported values |
| --- | --- |
| Container | RIFF WAVE, including compatible extensible PCM/float formats |
| Channels | Mono or stereo |
| Sample rate | 8,000–192,000 Hz |
| PCM depth | 8, 16, 24, or 32 bits |
| Floating-point depth | 32 or 64 bits |
| Size | File up to 512 MiB; audio data and decoded sample buffer each up to 128 MiB |

MP3 and compressed WAV files are unsupported. ALARM loops the WAV during a ringing interval. It saves the file's path, so keep the file available at that location. Selection does not prove that a file's contents are playable; use the preview to check it.

## Close, reopen, and quit

The close button hides ALARM in the notification area near the Windows clock. Click its icon to reopen the window. The icon's menu also provides STOP and Quit. If Windows cannot create the icon, closing minimizes the window instead.

Quit in Settings or in the icon menu stops audio, cancels the timer, and exits the process. You must run ALARM for an alarm to ring. Opening ALARM again uses your saved settings. Launching it a second time with the same data directory reopens the existing app.

The optional startup checkbox launches ALARM near the clock after your Windows account signs in. It does not run before sign-in or while the PC is fully shut down. Save the checkbox change to apply it. For portable use, keep the executable at the same path after enabling this option.

## Diagnostics and hardware checks

Diagnostics contains delayed alarm buttons for 10 or 30 seconds. They start the normal five-minute/ten-minute alarm cycle and use the real alarm's Windows volume behavior. They do not enable the daily alarm. Press STOP when you have finished checking them.

The locked-session test plays an initial three-second sound, requests the normal Windows lock, then schedules an alarm ten seconds later. It stops automatically twenty seconds after the post-lock audio starts. Listen without moving the mouse or typing, then unlock Windows normally. This short test checks locked audio; it does not test sleep or the full repeating cycle.

Sleep, hibernation, Modern Standby, battery power, and a closed laptop lid need separate checks on your own hardware. See the [manual validation guide](MANUAL_VALIDATION.md). The main app requests wake but does not silently alter your power plan.

## Troubleshooting

| Problem | What to check |
| --- | --- |
| No sound in the preview | Windows output/mute, ALARM volume above 0, selected device availability, and the displayed audio error. |
| Custom sound no longer works | The WAV still exists at its saved path and uses a supported format; try the built-in sound. |
| No alarm after sleep | The app is running, the alarm is enabled, wake timers are allowed for the active power source, and the hardware sleep scenario has been tested. |
| No notification | Windows notification preferences and Do not disturb. Notification display is separate from audio. |
| The window closed but an alarm still rings | Close keeps the engine running. Reopen through its icon and use STOP or Quit. |
| Settings cannot be saved | Check the configuration error and folder permissions. Preserve the original file before resetting it. |

Settings and logs live in `%LOCALAPPDATA%\Ibrahim\ALARM`. If the configuration is invalid, ALARM preserves `config.ini` and disables scheduling. To reset it, quit the app, make a backup, and rename the invalid file; the next launch creates defaults with the daily alarm disabled. Logs can include private paths and device details; redact them before sharing publicly.

## Uninstall

Use the ALARM entry in Windows installed apps, or run the included `uninstall.ps1` from the installed directory:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\uninstall.ps1
```

For a custom installation location, supply `-InstallDirectory` with that location. Uninstallation preserves your settings, diagnostic logs, and custom sounds. For portable use, quit ALARM and disable its Windows startup option before removing its folder.
