# Strata 1.6.6

Managed AI failures no longer share one “retry shortly” message. This patch is the 1.6.5 desktop work plus the error copy from `a37602e616b`.

## Changes

- `managed_provider_funding_required` is not a retry. The chat says the managed AI service is unavailable, Strata credits were not charged, and trying again will not help until the service is restored.
- `managed_provider_busy` shows the provider wait from `retry_after` and does not invite an immediate retry.
- A Plan Account transfer timeout says the reply did not arrive in time, with the Qt error kept as technical detail.
- Italian strings for these three cases are in `i18n/qgis_it.ts`.

The compatible cloud backend is already deployed (`strata-be-00057-4sv`). It forwards SSE keepalives, aborts a silent OpenRouter fetch, and stores the user prompt on error turns. This desktop release does not migrate customer databases.

## Validation

The new checks live in `tests/src/app/testqgsaiincidentrecovery.cpp` (funding is not retried, busy names the wait, transfer timeout explains the abort). They were not executed in a local QGIS build before this tag.

## Downloads

Application source tag: `strata-v1.6.6`. Platform packages (macOS universal DMG, Windows x64 installer and portable ZIP, Linux x86_64 AppImage), verification records and SHA-256 checksums are produced by the existing workflows from this tag. The release stays a draft until those assets are sealed.
