# Code signing policy

Last updated: October 25, 2026.

## Current status

ALARM is preparing an application to the
[SignPath Foundation](https://signpath.org/) program for free code signing of
open-source software. Acceptance and signing integration are not yet complete.
The current Windows application and installer are unsigned. This page does not
claim that SignPath has approved or signed a release.

The application is maintained by
[Ibrahim Bouillet](https://github.com/ibrahimbouillet), who is the project's
author and reviewer and the proposed signing approver. Maintainer accounts used
for source control and signing must use multi-factor authentication before
release signing is enabled.

## Planned release signing

If the application is accepted, signing will use artifacts built from this
public repository through its
[Windows GitHub Actions workflow](https://github.com/ibrahimbouillet/Alarme/actions/workflows/windows.yml).
Each signing request will require manual approval by the maintainer. Signing
credentials and private keys must not be committed to the repository.

The application, installer, and generated uninstaller must be covered by the
configured signing process. Signed artifacts must be verified before publication
and must not be modified afterward. Downloads will include SHA-256 checksums.
The release notes will identify the actual signing status of each release.

Once the service is active, signed releases will carry this acknowledgement:

> Free code signing provided by [SignPath.io](https://signpath.io/), certificate by [SignPath Foundation](https://signpath.org/).

SignPath Foundation requires approval of the project and compliance with its
[conditions](https://signpath.org/terms). Its certificates identify SignPath
Foundation as the certificate holder. This does not change ALARM's authorship
or MIT license. A valid signature alone does not guarantee that a new download
will immediately avoid a Windows reputation warning.

## Privacy

The standard ALARM app has no telemetry or automatic upload service. Future
release signing concerns build artifacts and does not add end-user tracking.
Local settings, logs, and optional widget state are described in the
[ALARM Privacy Policy](PRIVACY.md).
