# Strata 1.6.1

Patch release prepared from `master`. The release tag and packages are not part of this commit.

## Changes

- Replace the Strata mark across macOS, Windows, Linux, the in-app assistant, splash screen and documentation.
- Report Claude OAuth success only after token exchange and secure credential storage complete.
- Present rate limits and other OAuth failures as typed, actionable states in Strata and in secure branded callback pages.

## Verification

- `test_app_aiclaudeoauthclient`
- `test_app_aiclaudeoauthhelpers`
- `test_app_aiclaudeconnectwidget`
- `test_app_aimodelrouter`
- `test_app_aisecretstore`

Live Anthropic login and final signed package acceptance remain release gates.
