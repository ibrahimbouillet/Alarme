# Locked sessions and the optional widget

ALARM runs on the normal Windows user desktop. Locking the session does not turn its window into a lock-screen control. The engine can remain active and request audio while locked, but audio availability must be checked on the intended hardware. Unlock Windows normally to open the app and press STOP.

The app can send Windows notifications about its schedule and ringing state. Windows controls whether and where these notifications appear, including lock-screen visibility. Activating a notification opens ALARM after normal sign-in. Notification acceptance does not prove visible presentation, and a suppressed notification does not imply a stopped audio engine.

See [MANUAL_VALIDATION.md](MANUAL_VALIDATION.md) for the built-in locked-session audio test and separate sleep tests.

## Experimental widget provider

`prototypes/lock-widget` contains a separate Windows widget provider, with an Adaptive Card presentation and a local HTML glass presentation. This developer prototype is not included in the standard alarm installer and is not required to run the alarm. Its build and development registration instructions are in [the prototype README](../prototypes/lock-widget/README.md).

The provider displays the alarm engine's presentation snapshot from `%USERPROFILE%\.alarm\widget-state.json`. It does not own scheduling or audio. The snapshot contains the enabled state, local time, phase, next transition, update timestamp, process ID, and sound label. Full WAV paths, audio device IDs, and diagnostic errors are excluded. Missing, invalid, or stale process state must display unavailable rather than fabricate a running alarm.

The provider is read-only with respect to the engine; an open action can launch ALARM. Windows owns widget placement, supported sizes, rendering, execution limits, and any lock-screen availability. A compiled or registered provider does not establish that the current Windows host offers or displays it on the lock screen. Verify the actual host and visible result separately.

The HTML presentation serves packaged/local resources and snapshot data without an external content service. Development dependency restore can use NuGet. See the prototype's retained third-party notice before redistributing its sources.

## Scope for contributors

Keep the normal alarm independent of the prototype. Preserve the presentation-only contract, validate process liveness, and pause presentation work when the host is inactive. Changes to registration should act only on identifiers owned by this project. Do not alter Windows sign-in, authentication, or secure-desktop components to add alarm controls.
