# Optional liquid demo

The liquid demo is a separate graphics experiment. It does not initialize the alarm engine, audio, startup, or notification area. It is excluded from the standard alarm build and release archive.

## Build and launch

You need the main C++ build tools plus Windows SDK C++/WinRT projection headers. Use a separate build directory:

```powershell
cmake --preset windows-release -B build-demos -DALARM_BUILD_DEMOS=ON
cmake --build build-demos --config Release
ctest --test-dir build-demos -C Release --output-on-failure
.\build-demos\ALARM_Liquid_Demo.exe
```

If SDK discovery fails, pass `-DALARM_CPPWINRT_INCLUDE` with the SDK's `cppwinrt` header directory. The main alarm does not require that optional dependency.

The convenience launcher uses its own `build-demo` directory and configures/builds/tests the demo before opening it:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\liquid-demo.ps1
```

`-NoBuild` uses a previously built demo. `-Grid`, `-Desktop`, and `-Transparent` select mutually exclusive modes; desktop is the default.

## Interaction

In desktop mode, choose a capture source through the native Windows picker. No droplet is drawn until a source texture is available. Prefer the screen where the demo is open when checking the spatial effect; a differently sized source is fitted to the work area instead of matching the pixels directly behind the droplet.

Hold and drag a droplet, then release it to fall and spread into a puddle. It disappears four seconds after release. Clicking a free area creates another droplet, up to three. Escape, Alt+F4, or the Close button exits the demo.

Grid mode samples a generated internal grid and identifies itself as an optical test. Transparent mode checks light, shadow, and transparency without desktop refraction. These modes show an initial droplet without source selection.

## Capture and local metrics

The desktop source is chosen manually. Capture pixels stay in GPU textures; the demo does not save or transmit them. The choice is not persisted. Capture stops when the final droplet expires, after fifteen seconds without movement at rest, on hide/lock/suspend/display-off, or when the demo closes. Choosing another source requires another Windows selection.

The capture path uses BGRA8 SDR. HDR fidelity is not established. Internal texture rendering tests do not establish the visual accuracy of a selected desktop source.

`-MetricsPath` on the launcher enables optional JSON metrics for the demo's own frames, gestures, droplets, and capture lifecycle. F12 refreshes them immediately. These metrics do not contain screenshots or source names. Keep generated metrics and validation images out of published sources.

## Model and validation

The physics uses fixed 1/120-second steps, bounded gesture velocity, gravity, damped deformation, and a spreading puddle after impact. It is an ellipsoid-based visual model, not a general fluid solver. The optical shader uses air/water refraction, Fresnel reflection, bounded internal reflection, and a mouse-directed light source.

The optional test groups are `liquid_physics`, `desktop_capture_lifecycle`, and `liquid_renderer_hidden`. The hidden renderer uses private generated textures and writes only those test pixels. The capture lifecycle tests do not select a user source. Run them separately with:

```powershell
ctest --test-dir build-demos --output-on-failure -R '^(liquid_physics|desktop_capture_lifecycle|liquid_renderer_hidden)$'
```

Manual validation should check source selection/cancellation, hold/drag/release, puddle formation, disappearance, pause/close resource release, and the visible relationship between the chosen source and the water. Record source-based observations separately from tests using generated textures.
