# Windows API notes

This is an implementation map for contributors. Consult Microsoft's current API contracts when changing platform code, and keep API success separate from observed hardware behavior.

## Timer and message loop

`WakeTimer` in `src/platform.cpp` owns a Windows waitable timer. Deadlines are positive absolute UTC FILETIME values, armed once with a resume request. [`SetWaitableTimer`](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-setwaitabletimer) documents UTC absolute deadlines and the case where a resume request succeeds but reports unsupported resume. Cancellation and handle lifetime belong to the application.

The main thread waits for messages and timer signals through `MsgWaitForMultipleObjectsEx`. Daily schedule calculation is local-calendar logic in `src/engine.cpp`; active cycle elapsed time is adapted from Windows uptime by `src/elapsed_cycle.h`. Do not use a civil-clock adjustment to reset a running cycle.

Suspend/resume, display power state, and session lock notifications trigger cancellation, reconciliation, and visual lifecycle changes. While ringing, ALARM requests system/display activity with [`SetThreadExecutionState`](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setthreadexecutionstate), then clears the request at silence/STOP/shutdown. This API does not prevent a user-requested sleep transition and is not a secure-desktop or authentication API.

## Audio

`src/audio.cpp` implements shared-mode, event-driven [WASAPI](https://learn.microsoft.com/en-us/windows/win32/coreaudio/wasapi). It opens PCM/float source data, uses the Windows audio conversion path, and assigns the Alerts category before initialization. The worker responds to buffer events, cancellation, and device changes.

Session gain controls ALARM separately from the endpoint level. `src/alarm_audio_policy.h` forces and unmutes the endpoint only for a real alarm; preview preserves endpoint state and integration uses silent session gain. The worker tracks endpoint snapshots and attempts restoration before discarding them. Endpoint access errors must be reported; stop completion, successful restoration, and physical audibility are distinct results.

## Tray, notifications, and startup

`src/tray.cpp` owns notification-area icon lifetime and the reopen/STOP/Quit menu. The app recreates its icon when Explorer broadcasts `TaskbarCreated`. The icon is independent of whether the main window is visible.

`src/notifications.cpp` registers the app's current-user identity, notification shortcut, and owned open protocol. Notification activation opens ALARM through normal Windows sign-in. Notification failure should leave timer/audio behavior available. Never infer banner visibility from a successful send call.

Optional startup uses a quoted executable path in the current user's Windows Run key. Microsoft's [Run and RunOnce documentation](https://learn.microsoft.com/en-us/windows/win32/setupapi/run-and-runonce-registry-keys) describes sign-in startup and possible execution delay. The setting must remain reversible and must not overwrite another owner's registry value. Moving a portable executable can invalidate its startup command.

## Configuration and presentation state

Configuration lives under `%LOCALAPPDATA%\Ibrahim\ALARM`; retain this path for compatibility. The serializer validates the entire file and performs a flushed temporary-file replacement. An invalid file is preserved for diagnosis instead of silently reset. Local logging is bounded.

The optional widget bridge is `%USERPROFILE%\.alarm\widget-state.json`. It is UTF-8 JSON with `enabled`, `hour`, `minute`, `phase`, `nextTransitionUtc`, `updatedUtc`, `processId`, and `soundLabel`. The sound label can include a file name but not the full source path. A consumer must check process liveness and valid state; it must not create an independent alarm schedule from stale presentation data. See [LOCK_SCREEN.md](LOCK_SCREEN.md).

## Graphics and capture

The alarm uses Direct3D 11, Direct2D, DirectWrite, DXGI, and DirectComposition. Refraction in the alarm window samples its own rendered content. It does not capture the desktop. Keyboard/input and lifecycle behavior must remain usable when visual effects change.

Desktop capture is confined to the optional liquid demo. It uses the Windows picker for a user-selected source, keeps pixels in GPU memory, and releases capture on inactivity/pause/close. Its additional C++/WinRT SDK dependency is excluded from the base alarm build. See [LIQUID_DEMO.md](LIQUID_DEMO.md).
