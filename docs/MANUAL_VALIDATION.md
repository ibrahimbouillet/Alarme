# Manual validation

Use this guide to check ALARM on the computer where it will be used. Hardware wake, speaker audibility, notification visibility, and Windows startup need direct observation. Passing CTest or seeing a nonzero mixer level does not establish these results.

Before starting, save other work, confirm that you can sign back in after a normal Windows lock, and select a suitable alarm volume. A real alarm raises the chosen Windows output to 100% and unmutes it. Use a dedicated `--data-dir` for development tests so they do not replace your normal settings.

Record the ALARM version, Windows version, audio output, power source, and available sleep states. These commands read diagnostics without changing the power plan:

```powershell
powercfg /a
powercfg /query SCHEME_CURRENT SUB_SLEEP RTCWAKE
```

## Basic alarm and restoration

1. Note the output's Windows volume and mute state. Confirm that ALARM's own volume is above 0.
2. Use Listen to sound. Check that the sound starts, the preview finishes after eight seconds of playback, and the Windows output volume/mute remain unchanged.
3. In Diagnostics, choose the ten-second delayed alarm. Check the displayed deadline and audible start without touching the device at that moment.
4. Confirm that the output was raised to 100% and unmuted, then press STOP. Check that the original output volume/mute return.
5. Repeat with a custom supported WAV and, if relevant, a specific audio output.

For the full production cycle, trigger a delayed alarm, let it ring for five minutes, observe ten minutes of silence, and wait for the next ringing interval. Check endpoint restoration during silence and reapplication on the next ring, then press STOP. Use elapsed timing and the logs to distinguish a measured interval from an estimate.

## Daily scheduling and tray

1. Set a daily alarm a few minutes ahead and enable it.
2. Close the window. Confirm that the app remains available through its notification area icon and rings at the scheduled time.
3. Reopen it and press STOP. Confirm that the daily alarm stays enabled and its next occurrence is scheduled.
4. Quit through the icon menu or Settings. Confirm that the process exits, then start it again and check the saved time and enabled state.
5. Disable the daily alarm when the test is complete.

For startup, enable and save the Windows startup option, then sign out and back in when convenient. Check that ALARM starts near the clock. Disable and save the option afterward if you do not want it enabled. This checks startup after sign-in; it does not test execution before sign-in.

## Locked-session audio

Use the locked-session test in Diagnostics. It plays a three-second prelude, locks Windows normally, schedules an alarm ten seconds later, and stops twenty seconds after post-lock playback starts. The flow is bounded if audio preparation fails.

Listen while locked without moving the mouse, pressing a key, or using a remote input tool. Record whether the sound starts before any interaction, then unlock Windows normally and read the displayed status/log. Check output restoration. The lock-only test does not put the machine to sleep and does not establish wake behavior.

## Sleep and hibernation

1. Leave ALARM running and trigger a delayed alarm with enough time to enter the intended sleep state.
2. Put the PC to sleep using Windows. For hibernation or lid-close behavior, repeat with that specific transition.
3. Wait without input for wake and sound. If the machine does not resume as expected, wake it manually and record that manual intervention was needed.
4. After returning, inspect:

   ```powershell
   powercfg /lastwake
   ```

5. Check `alarm.log` for scheduling, suspension, resume, audio start/error, and STOP. Record audible sound separately from the wake owner and API success.
6. Press STOP, verify restoration, and disable any temporary daily schedule.

Repeat relevant scenarios on AC and battery. A machine can expose Modern Standby, traditional sleep, or hibernation with different behavior. A lid-close power action and an available wake timer do not establish that speakers remain usable. A fully shut-down PC cannot run ALARM.

## Opt-in developer helper

After building, `scripts/test-sleep.ps1` prepares isolated settings, starts a thirty-second wake test, and requests a physical sleep transition. It requires `-SleepNow`:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\test-sleep.ps1 -SleepNow
```

The additional `-EnableWakeTimers` switch changes the active plan's **AC wake-timer policy**, leaves it enabled after the test, and preserves the battery policy. Use it only when you want that change; inspect the recorded before/after values and restore the previous setting yourself if required. Normal ALARM installation/use does not make this change. A system policy may refuse it.

The script saves local evidence under `.local\wake-*`. Its return value and wake diagnostic do not certify audible audio or the full cycle. Observe the complete cycle manually and quit the test app normally afterward. Do not attach unreviewed machine logs to public reports.

## Notifications and accessibility

Check the tray menu, alarm notification, and app reopening. Windows Do not disturb and notification settings can suppress banners. While locked, an open action must use normal Windows authentication; ALARM does not provide STOP on the secure desktop.

Check keyboard navigation, Enter/Escape during time editing, DPI scaling, high contrast, and reduced animation settings. Verify the visible interface separately from hidden renderer tests. For the optional liquid demo and widget prototype, follow their own guides; they are separate from the alarm release.
