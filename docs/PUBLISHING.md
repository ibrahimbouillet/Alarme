# Publishing ALARM

ALARM is developed by **Ibrahim Bouillet** and distributed under the MIT License.
No account, service key or external server is required to run the alarm.

## Prepare a release

From the source checkout, with the UCRT64 build tools on PATH:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\package.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\export-source.ps1
```

The first command builds and tests ALARM, then creates the Windows x64 release
ZIP and a SHA-256 file in `out/releases`. The second creates a source ZIP from
the current working tree, including uncommitted public changes. It excludes Git
history, private archives, local settings, generated packages and build output.
Neither command commits, stages or publishes anything.

Extract the release ZIP, test `install.ps1`, open ALARM, enable an alarm and
verify the sound on your intended hardware. Use `uninstall.ps1` to remove the
installation. Keep the LICENSE and bundled runtime notices in the distribution.
The release executable and PowerShell scripts are unsigned.

## Personal reports and existing history

Machine-specific development reports are preserved locally under
`.local/publication-archive/`, which is ignored by Git. Do not upload `.local/`,
build output, user configuration or raw diagnostics as repository source.

Removing a file from the working tree does **not** remove it from earlier commits.
The public repository starts with an independent clean history. Earlier private
development history is retained separately and is not an ancestor of the public
release. Do not merge or push branches or tags from a private development archive
into the public repository.

For a similar migration, extract the clean source ZIP into a new directory and
create a fresh repository there. Keep the original repository private and update
its local remote before reusing its old GitHub name for the new repository.

GitHub's generated source ZIP follows the committed repository contents. The
local source export is intended for preparing that clean first commit; it is not
a replacement for reviewing the final files you choose to commit.

## Automated checks

The Windows workflow builds with GCC UCRT64 and runs the deterministic engine,
timing and widget-state tests. It uploads a downloadable ZIP artifact without
publishing a release. It does not establish physical wake, speaker audibility or
lock-screen presentation. Run the full local suite and the relevant manual checks
before describing those behaviors as tested on a particular device.

The optional liquid demo and widget provider are developer experiments and are
excluded from the ordinary alarm release.
