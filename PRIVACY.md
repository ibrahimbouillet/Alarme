# ALARM Privacy Policy

Last updated: October 25, 2026.

This policy describes the standard ALARM Windows desktop application and its
Windows installer, maintained by Ibrahim Bouillet. It covers the behavior of
ALARM 0.6.1. Optional developer experiments are identified separately below.

## Data stays on your computer

ALARM works offline and does not require an account. The standard application
has no advertising, analytics, telemetry, automatic diagnostic upload, or
application update service. It does not send your settings, logs, audio device
information, or sound files to the maintainer or to SignPath.

ALARM processes local information to schedule and play your alarm:

- The daily alarm time and enabled state.
- Your selected audio output identifier, volume preferences, and startup choice.
- The full path of a WAV file you choose. ALARM reads that file for playback;
  it does not copy it into the installation or upload it.
- Windows time, audio output availability, power and session events, and error
  information needed to operate and diagnose the alarm.

The standard application does not record microphone audio or capture your
screen. A sound selected from a network share or a cloud-backed folder may cause
Windows or that storage provider to access the network when opening the file.
That is distinct from an ALARM telemetry or upload service.

## Local files and Windows integration

The default data folder is `%LOCALAPPDATA%\Ibrahim\ALARM`. It contains
`config.ini`, local diagnostic logs such as `alarm.log` and
`alarm.previous.log`, and `widget-state.json`. Running ALARM with `--data-dir`
uses the directory explicitly selected by the person launching it.

The normal app also publishes a local presentation state file at
`%USERPROFILE%\.alarm\widget-state.json` for the optional widget prototype.
Presentation state includes the alarm time, enabled state, current phase,
transition and update timestamps, process identifier, and the selected sound's
file name. It excludes the sound's full path and the audio device identifier.

Settings and logs are not encrypted by ALARM. File paths can identify your
Windows account or reveal personal file names. Diagnostic information may also
contain audio device names and identifiers. Local files remain subject to your
Windows file permissions and any backup or synchronization software you use.

ALARM uses Windows notifications and creates local shortcuts and application
registration entries so that a notification can reopen the app. If you enable
startup in Settings, it records a per-user Windows startup entry. The installer
registers an uninstaller and may write installation logs in the Windows temporary
folder. These local registrations and logs are not uploaded by ALARM.

## Retention, removal, and sharing

Your settings, local logs, widget state, and custom sounds are preserved when
you uninstall ALARM. There is no remote ALARM account or server-side user record
for the maintainer to delete. Logs rotate locally as they grow; other personal
files remain until you remove them or another program changes them.

To remove the default application data, quit ALARM, uninstall it if installed,
then delete `%LOCALAPPDATA%\Ibrahim\ALARM` and `%USERPROFILE%\.alarm` if you no
longer need their contents or the widget prototype. If you used `--data-dir`,
review and remove your chosen data folder separately. Your WAV files remain at
their original locations. Installer logs may remain in the Windows temporary
folder.

Diagnostic reports are shared only if you choose to send or publish them.
Review and redact logs, reports, and screenshots before posting them publicly.
Do not include credentials, personal paths, or unrelated personal information in
a GitHub issue.

## External services and optional experiments

Visiting the repository, downloading a release, or opening an issue uses GitHub
and is subject to the [GitHub Privacy Statement](https://docs.github.com/en/site-policy/privacy-policies/github-general-privacy-statement).
Windows, audio drivers, storage providers, and backup tools may process data
according to their own settings and policies; this policy describes ALARM's
behavior.

The repository includes optional graphics and widget development experiments.
The desktop-capture demo can capture a screen or window selected through Windows
when deliberately built and run. It is not included in the standard ALARM
installer or portable release. The optional widget consumes the local
presentation state described above.

ALARM is preparing an application to SignPath Foundation for free open-source
code signing. Signing is not yet active. A future signing service would process
software build artifacts, rather than collect data from running copies of
ALARM. See the [code signing policy](CODE_SIGNING.md) for the current status.

## Questions and changes

The maintainer is [Ibrahim Bouillet](https://github.com/ibrahimbouillet).
Questions about this policy can be raised in the
[public issue tracker](https://github.com/ibrahimbouillet/Alarme/issues).
Keep the initial question free of private data.

Changes to ALARM's data handling will be documented here with an updated date.
The repository history records revisions to this policy.
