# Developer guide

ALARM is programmed by **Ibrahim Bouillet** and released under the [MIT License](../LICENSE). See [CONTRIBUTING.md](../CONTRIBUTING.md) for contribution expectations and [PUBLISHING.md](PUBLISHING.md) for preparing source and binary archives.

## Build requirements

The main application targets Windows 10/11 desktop APIs and x64 release packaging. The release workflow uses CMake 3.25 or newer with a C++20 GCC/MinGW toolchain and Ninja on `PATH`. An alternative Visual Studio 2022 preset is provided for the Desktop development with C++ workload and a Windows SDK; that preset still needs verification with an installed Visual Studio toolchain. The base application does not need C++/WinRT projection headers or the .NET SDK.

For GCC, use a consistent toolchain such as MSYS2 UCRT64; see [MSYS2's CMake guide](https://www.msys2.org/docs/cmake/) and [environment guide](https://www.msys2.org/docs/environments/). Make the compiler and Ninja visible in the shell where you run CMake. The presets do not assume a particular installation drive.

Install MSYS2 using [its official installer](https://www.msys2.org/), then open the **MSYS2 UCRT64** terminal and update/install the tools:

```sh
pacman -Syu
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
```

If the update asks you to close the terminal, reopen UCRT64 and rerun the update before installing the packages. In PowerShell, add that installation's UCRT64 tools for the current session:

```powershell
$msysRoot = Read-Host 'MSYS2 installation folder'
$env:PATH = (Join-Path $msysRoot 'ucrt64\bin') + ';' + $env:PATH
```

From the repository root:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1 -BuildDirectory build -OutputDirectory out
```

This configures, builds, runs CTest, and copies portable executables to `out`. `-SkipTests` skips validation only when you explicitly request it. Quit an application normally before replacing an executable that Windows has open.

The equivalent preset workflow for the default Ninja build is:

```powershell
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
```

For the alternative Visual Studio preset:

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-msvc
ctest --preset windows-msvc
```

Use different build directories when switching generators or compilers. Generated binaries, caches, logs, and local test evidence belong outside the tracked source tree.

## Project layout

| Location | Purpose |
| --- | --- |
| `src/engine.*`, `src/elapsed_cycle.h` | Daily local-time scheduling and repeating cycle logic |
| `src/config.*` | Validated, atomic configuration persistence |
| `src/audio.*`, `src/alarm_audio_policy.h` | Event-driven WASAPI playback and endpoint restoration policy |
| `src/platform.*` | Waitable timers, power policy diagnostics, paths, startup helpers, and logs |
| `src/main.cpp` | Windows message loop, commands, lifecycle, and integration test flow |
| `src/glass.*` | GPU interface and input feedback |
| `src/tray.*`, `src/notifications.*` | Notification area and native Windows notifications |
| `src/widget_state.*` | Presentation-only snapshot for optional consumers |
| `tests/` | Software tests |
| `scripts/` | Build, package, installation, and opt-in diagnostic helpers |
| `prototypes/lock-widget/` | Separate experimental Windows widget provider |

## Core behavior to preserve

Daily scheduling follows the Windows local calendar. A repeating cycle uses elapsed uptime and stays anchored to its start even after delayed callbacks. STOP cancels the current cycle and preserves an enabled daily schedule. Closing hides or minimizes the interface; only Quit exits the engine.

Real alarms raise and unmute the chosen Windows audio endpoint. Preview preserves endpoint settings; integration tests use zero ALARM session gain and do not override the endpoint. Audio cancellation must restore endpoint state where the device remains accessible. Changes to audio configuration stop an existing cycle before another sound/device starts.

The event loop waits for messages and timer events. Audio uses WASAPI buffer events. Visual timers pause while the window is hidden, minimized, locked, suspended, or the display is off; visual changes must not introduce polling into alarm scheduling or audio processing. Drawing errors must be reported without manufacturing a successful audio state.

Keep corrupt configuration available for diagnosis. Do not change compatibility identifiers or settings paths without an explicit migration. The public widget snapshot must omit full paths, device IDs, and diagnostic text.

## Validation

The base suite contains eight CTest groups: engine/configuration, elapsed timing, widget state, renderer, tray, Windows probe, WASAPI, and application integration. The integration scenario uses three-second ringing/two-second silence so a cold audio driver has time to initialize. It checks the actual Windows timer, hidden-window lifecycle, audio stop, schedule activation, and time editing. It does not replace the five-minute/ten-minute physical test.

When changing behavior, run the relevant groups and the build's full base suite before packaging. Renderer tests use private generated content. Audio tests use silent ALARM gain; explicit diagnostic tools can produce sound or change endpoint volume and must be run intentionally.

Examples:

```powershell
ctest --test-dir build --output-on-failure -R '^(alarm_engine|elapsed_cycle|mvp_integration)$'
.\out\alarm_probe.exe --no-audio --report .\build\probe.txt
```

Use `--data-dir` for isolated application tests. Normal app data is `%LOCALAPPDATA%\Ibrahim\ALARM`; isolated tests do not publish the user-profile widget bridge. `scripts/observe-process.ps1 -TargetProcessId <pid>` collects bounded CPU/memory/handle metrics without interacting with the app.

For sleep, lock, notification visibility, volume restoration, and full-cycle audibility, follow [MANUAL_VALIDATION.md](MANUAL_VALIDATION.md). Record software assertions, OS/API results, visible behavior, and physical audibility separately. Do not generalize one machine's wake result to all Windows computers.

## Optional components

The base CMake configuration sets `ALARM_BUILD_DEMOS=OFF`. Use `scripts/build.ps1 -WithDemos` for the optional demo targets and full eleven-test suite, or configure them in a separate directory:

```powershell
cmake --preset windows-release -B build-demos -DALARM_BUILD_DEMOS=ON
cmake --build build-demos --config Release
ctest --test-dir build-demos -C Release --output-on-failure
```

Desktop capture needs Windows SDK C++/WinRT projection headers. If automatic discovery fails, set `ALARM_CPPWINRT_INCLUDE` to the SDK's `cppwinrt` directory. See [LIQUID_DEMO.md](LIQUID_DEMO.md) for scope and interaction.

The widget provider is a separate .NET/Windows App SDK project with its own build and development registration scripts. Its dependencies and third-party notice are documented in [its README](../prototypes/lock-widget/README.md). The normal binary archive does not install the prototype. See [LOCK_SCREEN.md](LOCK_SCREEN.md).

## Packaging

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\package.ps1
```

This produces `out\releases\ALARM-0.6.1-windows-x64.zip` with the main app, installation/uninstallation scripts, user documentation, and license. Use `-SkipBuild` only with already validated output. Build a clean source archive with `scripts/export-source.ps1`; review it using the [publishing guide](PUBLISHING.md).

Keep distribution claims tied to the checks actually performed: a build proves compilation, CTest checks software behavior, and physical testing establishes a specific hardware scenario.
