# Strata 1.6.1 delivery status

Scope: core; change: code; reuse: candidate. Roadmap phases 0 (distribution), 1 and 4 (assistant runtime).

## Implemented

- Failed provider rounds have a versioned checkpoint in existing encrypted chat metadata.
  Resume preserves completed tool results and mutations, validates scope/account/model/mode,
  rejects incomplete/undone/failed tool rounds, and reauthorizes managed execution.
- UI separates Resume, Hide notice and Restart this turn. Request-error messages are excluded
  from outgoing model context. Old remote_content_not_allowed histories can resume when complete.
- SSE application codes and numeric statuses survive HTTP 200. Router retains bounded retries;
  watchdog timeouts retry without replaying tools; long Retry-After intervals require manual resume.
- Shared Updates settings page and version-check dialog: background check at most daily,
  asynchronous download/cancel, signed manifest, SHA-256/length/platform checks, explicit install.
- Native helper stages Windows installer/portable, macOS bundle and Linux AppImage updates.
  It verifies the manifest/package again, waits for application exit, keeps backups and checks
  startup acknowledgement. Immediate replacement/startup failures restore the previous bundle.
  A still-running app without acknowledgement is not killed or replaced underneath the user.
- Linux holds a separate AppImage runtime open while the updater runs. Portable staging overlays
  the new package on a copy of the installation so local profile/project files are retained.
- Release pipeline signs update-manifest.json with a dedicated RSA-3072 key and seals it together
  with updater acceptance. Release 1.6.1+ cannot publish without the required updater receipts.

## Trust and test setup

The private update key is outside the repositories and configured as the repository secret
`STRATA_UPDATE_PRIVATE_KEY`. `update-public-key.h` contains only its public key.
The signed test fixture is version 0.0.0 with no packages and cannot authorize an installation.

Production binaries do not read test feed overrides. Test builds can explicitly configure
`STRATA_ENABLE_UPDATE_TEST_FEED=ON` with `ENABLE_TESTS=ON`; a build with
`IS_STRATA_RELEASE=true` rejects it. Those builds accept `STRATA_UPDATE_TEST_FEED` only at
HTTP 127.0.0.1. Test packages still need a valid manifest signature. Never publish such builds.

For each of Windows installed, Windows portable, macOS and Linux AppImage, upload a real
`updater-acceptance.json` to the draft before sealing. It identifies tag and exact sourceSha,
then `platforms[platform]` with tester, testedAt, result and scenarios:
upgrade, restart, preserve-data, invalid-signature, cancel, failed-install-recovery.
Every scenario must be passed; do not fabricate receipts. Exercise a strictly newer test
version without publishing it as Latest. Windows acceptance remains a separate existing gate.

## Verification and pending gates

- Local C++ build and targeted Qt tests: router, agent session, chat dock, updater manifest passed.
- Node release integrity and update signing tests: 13 passed.
- Backend PR: https://github.com/strata-gis/strata-be/pull/50.
- Backend candidate `strata-be-fix161`: authenticated three-round real web smoke passed;
  public traffic remains on `strata-be-00044-cfp`.
- Backend CI cannot start: account billing/spending restriction, not a test failure.
- Native end-to-end updater acceptance on installed release packages is still pending.
- No immutable 1.6.1 release tag or stable publication until CI and acceptance gates pass.

## Recovery and compatibility

No cloud/SQLite schema migration. Keep the 1.6.0 release and previous backend revision.
Clients on 1.6.0 benefit from the gateway fix after promotion; they need one manual install
of 1.6.1 to acquire the updater. Subsequent updates use the settings page.
Managed/read-only installations report a clear refusal instead of forcing a replacement.
Application backup directories are retained for explicit recovery; user data is not purged.
