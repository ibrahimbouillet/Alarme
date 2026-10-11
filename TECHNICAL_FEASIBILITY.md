# Technical capabilities and limits

ALARM is a C++20 Windows desktop application. Its timer, audio, tray, and configuration code is separate from the GPU interface. This document describes the implementation and the limits that users and contributors need to account for; it does not certify every PC's wake or audio behavior.

| Capability | Implementation | Practical limit |
| --- | --- | --- |
| Daily scheduling | Windows local calendar time converted to an absolute UTC waitable timer | The process must be running; Windows time zone and clock changes affect the next daily occurrence. |
| Repeating alarm | 300 seconds ringing and 600 seconds silent, based on Windows uptime | A suspended process cannot produce sound; reconciliation applies the appropriate phase when execution resumes. |
| Wake request | `SetWaitableTimer` with a resume request | Firmware, power policy, sleep state, and device drivers determine whether the machine wakes and audio runs. |
| Audio | Event-driven shared-mode WASAPI; built-in tone or supported WAV | A rendered audio stream is not proof of sound reaching a listener. Disconnected or unavailable outputs can cause errors. |
| Volume | Separate ALARM session gain; real alarms raise and unmute the selected Windows endpoint | Endpoint settings are shared with other applications. A crash, forced exit, or inaccessible endpoint can prevent restoration. |
| Notification area | Persistent icon, reopen/STOP/Quit menu, Explorer restart handling | If the icon cannot be created, closing minimizes the window so the app remains accessible. |
| Startup | Optional current-user Windows Run entry | Starts after sign-in; does not provide a background Windows service or pre-login alarm. |
| Locked session | Engine and audio can continue; notifications can open the app after normal sign-in | ALARM's controls stay on the normal user desktop. Windows owns the lock screen and authentication. |
| Glass interface | Direct3D 11, Direct2D, DirectWrite, and DirectComposition | GPU support and Windows accessibility settings affect rendering and effects. |
| Optional widget | A separately built Windows widget provider reads presentation state | Availability and lock-screen presentation depend on the Windows host; this prototype is excluded from the base release. |

## Timing

The daily alarm uses Windows local time. The scheduling function selects the next occurrence strictly after the current time. For a daylight-saving gap, it moves a missing time to the first valid minute of that day. For a repeated time, it schedules the earlier occurrence once. The application recalculates the daily schedule after a Windows time-change message.

The repeating cycle is anchored to `GetTickCount64` through `ElapsedCycle`, so a civil-clock change does not extend the ringing or silent interval. A delayed callback advances through elapsed transitions instead of adding another full interval from the callback time. If an overdue daily occurrence is within 24 hours, the running process starts a new cycle when it handles that occurrence; older occurrences are skipped. Restarting the application schedules the next daily occurrence rather than replaying an alarm from a previous process.

An absolute UTC timer can request resume, but Windows documents hardware-dependent accuracy and unsupported resume behavior in [`SetWaitableTimer`](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-setwaitabletimer). Timer acceptance, actual system resume, process execution, and audible sound are separate observations.

## Sleep and power

ALARM registers suspend/resume and display-state notifications, stops audio when suspension is reported, reconciles elapsed time on resume, and can reopen the audio stream after a device/display transition. During ringing it requests system and display activity before opening WASAPI.

These requests do not override every Windows sleep policy. [`SetThreadExecutionState`](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setthreadexecutionstate) cannot prevent a user-requested sleep transition, including the configured lid or power-button action. Modern Standby can restrict desktop application execution, and waking the system does not guarantee that an output device is ready. Test sleep, hibernation, locked-session audio, AC power, battery power, and lid behavior separately on the intended machine.

ALARM reads and reports the wake-timer policy. Normal installation and alarm use do not change it. The optional physical-test script can change the AC wake-timer setting when explicitly invoked with its switch; its effects are explained in the [manual validation guide](docs/MANUAL_VALIDATION.md).

## Audio and persistence

`AudioPurpose` distinguishes a real alarm, an eight-second preview, and a silent integration test. A real alarm forces the selected endpoint to 100% and unmutes it. A preview preserves endpoint volume and mute. The player snapshots the previous endpoint state and restores it as the stream stops, including silence, STOP, suspension, and normal shutdown; restoration failures are reported.

Supported WAV files are mono or stereo RIFF PCM (8/16/24/32-bit) or IEEE float (32/64-bit), at 8,000–192,000 Hz. Matching extensible PCM/float headers are accepted. The reader bounds file size, decoded samples, chunk count, and format validation. MP3 and compressed WAV are unsupported. The [user guide](docs/PRODUCT.md) gives the usable size limits.

Configuration is validated and replaced atomically through a temporary sibling file. Invalid configuration is preserved, and the application disables scheduling instead of overwriting it. Local logs are bounded and contain diagnostics rather than audio recordings. A named mutex prevents two engines using the same data directory.

## Validation boundaries

The base CTest suite covers scheduling/configuration, elapsed cycle behavior, widget state serialization, hidden rendering, native tray lifecycle, Windows API probes, WASAPI behavior, and an accelerated timer/window/audio integration test. Optional demos add physics, capture lifecycle, and hidden liquid rendering tests.

Software tests check the implemented logic and API behavior. They cannot establish that speakers are audible after a physical wake, that a closed lid works on a particular laptop, or that Windows displays a notification. Follow [MANUAL_VALIDATION.md](docs/MANUAL_VALIDATION.md) for those observations, and describe results with the exact Windows version, device, power source, and scenario tested.
