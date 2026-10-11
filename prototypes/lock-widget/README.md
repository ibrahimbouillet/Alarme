# ALARM Windows Widgets companion

This optional development component displays the public state of the native ALARM engine. It provides an Adaptive Card named **ALARM** and a local HTML card named **ALARM · glass**. Install and use the main alarm application first; a widget does not run the alarm engine or play audio.

Written by **Ibrahim Bouillet** and licensed under the repository's [MIT license](../../LICENSE). The COM factory follows the Microsoft WindowsAppSDK-Samples Widgets sample; its MIT notice is preserved in [Microsoft-Samples-LICENSE.txt](Microsoft-Samples-LICENSE.txt).

## Requirements

- Windows 11 on x64 hardware.
- .NET SDK 10 or newer, with `dotnet` available on `PATH`.
- Microsoft Windows App Runtime 2.3.1 x64 or newer, installed for the current user.
- Windows Developer Mode enabled before registering the development package.
- The native ALARM application built or installed and running normally.

NuGet downloads are required for the build. The project pins Microsoft.WindowsAppSDK 2.3.1 and publishes a self-contained .NET executable. The Windows App Runtime remains a separate package dependency. The registration scripts check prerequisites; they do not install runtimes or certificates, or enable Developer Mode.

## Build and register

Run these commands from the repository root in Windows PowerShell:

```powershell
& .\prototypes\lock-widget\Build.ps1 -Candidate
& .\prototypes\lock-widget\Register-Development.ps1 -Candidate
```

The candidate is built into `out\Widgets`. Its stable package name is `Ibrahim.ALARM.Widgets`, with publisher `CN=Ibrahim ALARM`. These identifiers are retained for compatibility. The two widget definitions are `AlarmSmall` and `AlarmGlassSmall`.

Development registration uses files directly from this directory. Keep the repository and `out\Widgets` at the same location while the package is registered. Close any running widget provider before replacing its files. This component is a development package, not a signed installer for general distribution.

Omit `-Candidate` to build and register the isolated prototype under `prototypes\lock-widget\package`. It has a separate package identity and COM class. Registration rejects a package with a different publisher, path, or registration mode, and rejects downgrades. An already registered version is left in place; increment the corresponding manifest version for a package update.

For an isolated build that leaves the default package directory intact:

```powershell
& .\prototypes\lock-widget\Build.ps1 -Candidate -OutputRoot .\build\Widgets-preview
```

The registration scripts use the default package directories; an isolated build is intended for inspection and testing.

## Add a widget

Add **ALARM** or **ALARM · glass** through the Windows widget catalog. Lock-screen availability depends on the Windows build, installed widget host, and feature rollout. See [Lock-screen integration](../../docs/LOCK_SCREEN.md) for supported behavior and limitations.

Check the time, enabled and disabled states, ringing and silent phases, audio-error state, and closed-engine state on your own computer. Opening the native app uses the existing `ibrahim-alarm://open` protocol after normal Windows sign-in. Windows owns lock-screen rendering and activation; no widget action changes the alarm above the lock screen.

The glass image used by the Adaptive Card is a static illustration. The HTML variant respects reduced motion and pauses timers and animations when hidden or inactive. API probes and generated previews cannot establish the final appearance or audibility of a locked computer.

## Validate the state reader

With ALARM running normally and the matching widget package built:

```powershell
& .\prototypes\lock-widget\Validate-State.ps1 -Candidate
```

This checks six isolated fixtures: live state, audio error, mismatched process ID, missing file, unknown phase, and invalid JSON. It verifies that the live bridge remains unchanged. Results are written to the ignored `.local\widget-validation\candidate` directory, or `.local\widget-validation\prototype` when `-Candidate` is omitted. Use `-OutputRoot` to choose another results directory. These checks do not ring an alarm or lock the computer.

Use `-PackageRoot .\build\Widgets-preview` to validate an isolated candidate build without replacing or invoking a registered package.

Each fixture probe has a 120-second timeout. Use `-ProbeTimeoutSeconds` to adjust this on a slower computer; a timeout stops only the fixture subprocess.

The provider also accepts `--state-probe <input.json> <output.json>`, `--template-probe <output.json>`, and `--api-probe <receipt.txt>` for developer diagnostics. API probes require the registered package identity context. Store diagnostic output outside the public source tree.

## Data and privacy

The provider reads only `%USERPROFILE%\.alarm\widget-state.json`, an atomic UTF-8 snapshot published by the normally running engine. It checks the phase, timestamps, time, live process ID, and process name. Missing, invalid, or stale process state is presented as unavailable. An audio-error phase is reported explicitly and is not proof that sound is audible.

HTML, CSS, JavaScript, the logo, and JSON state are served locally by `IWidgetResourceProvider`. The widget does not contact a content endpoint, read private alarm configuration, or control the engine. Its bounded diagnostic log is stored under `%LOCALAPPDATA%\Ibrahim\ALARM`.

Build output, package files, diagnostic receipts, instance IDs, and local machine snapshots are generated artifacts and should remain untracked. Do not remove files used by a registered package to clean the repository.

## Remove the development package

```powershell
& .\prototypes\lock-widget\Unregister-Development.ps1 -Candidate
```

The script removes only the package with the expected identity, publisher, path, and development-registration mode. Omit `-Candidate` for the isolated prototype.
