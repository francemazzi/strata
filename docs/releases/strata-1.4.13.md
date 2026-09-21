# Strata 1.4.13

This hotfix temporarily suspends Claude subscription connections. Use Strata
Cloud or an Anthropic API key (billed separately from a Claude subscription).

- Strata no longer starts Claude Code, collects setup tokens, or uses subscription
  tokens for direct Anthropic requests. It does not change Claude Code itself or
  revoke the user's account sessions. Legacy Strata-owned subscription secrets
  and temporary login files are removed.
- If a Claude subscription was selected, sending pauses until the user chooses
  another provider. Sign-in alone does not silently change the active provider.
  Requests no longer fail over to a different billed service automatically.
- New credentials use the OS keychain with no unencrypted persistence fallback.
  If unavailable, users can retry or explicitly choose session-only storage.
- Existing API keys, Cloud sessions and Codex refresh tokens are migrated only
  after a successful write/read-back. Unavailable legacy vaults remain intact
  with a visible migration-pending notice.
- Saving errors keep settings open. Provider state distinguishes configured,
  session-only and verified by a successful response.

Projects, profiles and conversation history are preserved. The encryption key
for existing chat/index data remains in the QGIS authentication vault.

## Validation and publication

Merged into `master` in [PR #53](https://github.com/francemazzi/strata/pull/53),
commit `bc84b5c7e60a51a715b61b3ea6b6155a0855e3ec`. The immutable release tag
`strata-v1.4.13` points to `d4e1b79c4b561aa1ab84202490e8eccc15a7b620`;
the merge and tag have identical source trees. Released on 2026-09-21 after
successful automatic checks, with field acceptance still pending as listed
below. Published artifacts are the same signed packages verified in CI;
publication does not rebuild them.

Evidence collected locally on macOS:
- All 27 AI CTest executables passed (36.68 seconds), including credential failure,
  read-back/rollback, interrupted migration, pending-dialog and provider-selection cases.
- Native macOS Keychain: write, read after clearing the process cache, and verified
  deletion passed with a disposable credential in an isolated test profile.
- Release integrity/packaging checks: 11 passed, one Windows-only check skipped;
  macOS packager: 14 passed.
- OpenRouter live API smoke passed through the router after native Keychain
  persistence and cache reload, with a non-empty response and verified connection
  status. The cache reload is not a full desktop-process restart.
- An existing OpenAI test key was rejected by
  the service; this is not recorded as a successful OpenAI acceptance test.

Final package evidence collected on 2026-09-21:
- [Windows release CI](https://github.com/francemazzi/strata/actions/runs/35590898298)
  passed. Authenticode verification covers all 926 portable and 927 installed
  binaries, including `OpenCL.dll`, `qgis_app.dll`, `qt6keychain.dll` and
  `Uninstall.exe`. Installer/ZIP payload hashes match. Installation, portable and
  installed PyQGIS/runtime checks, project/raster/vector/Processing operations,
  OpenCL loading and uninstallation passed on the CI runner.
- [macOS release CI](https://github.com/francemazzi/strata/actions/runs/35590898227)
  passed for Intel and Apple Silicon, including bundled PyQGIS, signing,
  notarization and stapling. The downloaded DMG hash matches its build receipt.
  Local deep bundle verification and Gatekeeper assessment passed. The packaged
  app launched with an isolated profile; AI panel, account settings, cancellation
  and application quit passed.
- [Linux release CI](https://github.com/francemazzi/strata/actions/runs/35590898229)
  passed, including bundled PyQGIS and AppImage artifact checks. The downloaded
  AppImage hash matches its build receipt.
- Source CI passed: Ubuntu/Fedora QGIS test matrices, database provider suites,
  Windows/macOS builds, WASM, clang-tidy, code layout and pre-commit checks.
- [Artifact sealing](https://github.com/francemazzi/strata/actions/runs/35611378423)
  verified all platform receipts against the immutable source commit and actual
  package hashes, then signed and verified four binaries plus the release
  manifest with Sigstore. All 19 release assets were checked against the sealed
  inventory before publication. Checksums, bundles, receipts and the Windows
  signature report are available with the [release](https://github.com/francemazzi/strata/releases/tag/strata-v1.4.13).

Field acceptance still pending:
- Complete Cloud/API sign-in, first response, desktop restart and disconnect on
  the final Windows/macOS packages. Local native Keychain/API checks and the
  basic packaged macOS launch are separate evidence, not full field acceptance.
- Real Linux credential behavior with and without a keychain service. Simulated
  unavailable/denied/failing keychain regressions passed locally.
- Windows 11 Smart App Control On, upgrade/profile preservation, GPU/no-GPU
  scenarios and confirmation on the affected colleagues' computers.

`windows-verification.json` explicitly reports `smartAppControlTested: false`.
No manual acceptance or resolution on the affected PCs is implied by signing or
successful CI. New signed files may still trigger SmartScreen reputation
warnings or organization-specific policies. The published 1.4.12 files remain
unchanged.
