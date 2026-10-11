# Third-party notices

Original ALARM code and assets are by Ibrahim Bouillet and licensed under the
[MIT License](LICENSE). The following components retain their own notices.

## Native Windows executable

The GCC UCRT64 release build links compiler and MinGW runtime components
statically. Their unmodified notices accompany the distribution:

- [GCC GPLv3 text](licenses/gcc/COPYING3) and
  [GCC Runtime Library Exception](licenses/gcc/COPYING.RUNTIME).
- [MinGW-w64 notices](licenses/mingw-w64/COPYING),
  [project notice](licenses/mingw-w64/COPYING.MinGW-w64.txt), and
  [runtime notice](licenses/mingw-w64/COPYING.MinGW-w64-runtime.txt).
- [winpthreads notice](licenses/winpthreads/COPYING).

Windows system libraries are supplied by Windows. The source build uses CMake,
Ninja and a C++ compiler; those development tools are not included in the release.

## Optional widget prototype

The COM registration implementation follows the Microsoft
[WindowsAppSDK Widgets sample](https://github.com/microsoft/WindowsAppSDK-Samples/tree/main/Samples/Widgets).
Its original [Microsoft MIT notice](licenses/microsoft/Widgets-Samples-LICENSE.txt)
is retained. The optional C# project restores its Windows App SDK and C++/WinRT
packages through NuGet; those packages are separate from the main ALARM release.
