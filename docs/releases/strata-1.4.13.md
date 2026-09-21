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

Candidate status: source implementation and local regression checks completed;
release CI and packaged desktop acceptance pending. No 1.4.13 binaries published.

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

These checks do not substitute for acceptance of the final signed artifacts.

Required evidence before publication:
- Focused credential/router/UI regressions and existing CI checks.
- Signed Windows EXE and ZIP, notarized macOS DMG and Linux AppImage receipts.
- Real Windows/macOS Cloud or API sign-in, first response, restart and disconnect.
- Linux behavior with and without a keychain service.

Windows 11 Smart App Control acceptance remains assigned to colleagues and must
be reported separately. No acceptance result is implied by a successful build.
The published 1.4.12 files must not be replaced.
