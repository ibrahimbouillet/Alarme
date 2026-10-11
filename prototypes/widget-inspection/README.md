# Widget instance inspector

This optional developer tool, written by **Ibrahim Bouillet**, records existing widget instances using `GetWidgetInfos`. It does not create, update, remove, or pin widgets and does not change the alarm engine. Its original source is covered by the repository's [MIT license](../../LICENSE).

It requires Windows 11 x64, .NET SDK 10 or newer, and the Windows App Runtime used by the [widget companion](../lock-widget/README.md). Build it from the repository root:

```powershell
dotnet publish .\prototypes\widget-inspection\WidgetInspection.csproj -c Release -o .\build\WidgetInspection
```

The executable accepts one argument: the path to a text receipt. It must run in the registered widget package's identity context to access that provider's instances. For example, after registering the candidate package:

```powershell
$package = Get-AppxPackage -Name 'Ibrahim.ALARM.Widgets'
if (!$package) { throw 'Register the widget candidate first.' }
New-Item -ItemType Directory -Force -Path .\.local\widget-inspection | Out-Null
$command = Join-Path $PWD 'build\WidgetInspection\WidgetInspection.exe'
$receipt = Join-Path $PWD '.local\widget-inspection\instances.txt'
Invoke-CommandInDesktopPackage -PackageFamilyName $package.PackageFamilyName -AppId 'App' -Command $command -Args ('"' + $receipt + '"') -PreventBreakaway
```

Receipts can contain local package and instance identifiers. Keep them under `.local` or another untracked directory. API results describe the instances known to Windows; they do not verify lock-screen appearance or behavior.
