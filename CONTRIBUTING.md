# Contributing to ALARM

ALARM is programmed by **Ibrahim Bouillet**. Contributions are welcome under the project's [MIT License](LICENSE). Read the [developer guide](docs/DEVELOPMENT_STRATEGY.md) before building or changing platform behavior.

## Reports

Describe the expected behavior, what happened, and concise steps to reproduce it. Include the ALARM version, Windows version, selected audio output, and relevant error message. For wake issues, include the actual sleep state, AC/battery source, whether the session was locked, and whether sound was heard before any input.

Review attachments before sharing: logs can contain local paths and audio device details. Keep account information, personal sound files, machine reports, and unrelated screenshots out of public issues.

## Changes

Use English for comments, documentation, and interface text. Credit the original programmer and preserve third-party license notices. Keep fixes focused and explain the user-visible result and how it was checked.

Preserve the daily schedule, five-minute/ten-minute cycle, STOP/close/Quit distinction, and endpoint restoration behavior. Keep rendering independent of scheduling and audio. Retain configuration compatibility or provide an explicit migration.

Run the relevant tests and full base build for behavior changes. New tests should exercise a meaningful failure or boundary case. Use isolated data directories for integration tests. Physical sleep, locking, audible audio, or endpoint-changing diagnostics need deliberate manual runs; do not make them implicit in the default build.

## Before proposing a change

- Check that the project builds using the documented commands.
- Run the required CTest groups and report their result.
- Validate visible interface changes with the native app.
- Explain any remaining hardware-specific uncertainty.
- Review the diff for generated files, private data, hard-coded local paths, and third-party notices.

Do not include binaries, build caches, local test logs, or machine-specific evidence in the source change. See [the publishing guide](docs/PUBLISHING.md) when preparing a distributable snapshot.
