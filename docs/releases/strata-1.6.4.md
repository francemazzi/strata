# Strata 1.6.4

The map-context chip above the assistant prompt now reads correctly: its text is struck through only while the map context is left out of the next message.

## Changes

- Desktop chat actions use accessible icons for copy, edit, new chat and history. Resume stays in the interruption notice with provider cooldown; restart and undo are explicit menu actions.
- Workspace and Cloud brings the local folder, account and three explicit content transfers into one page. Pending settings block transfers, previews retain operation results, and replies from an earlier workspace/account cannot update the current one. Import conflicts still keep local files by default. No file replication or public API changes.

- The chip ("<active layer> · 1:<scale>") showed its text struck through even while the map view, the active layer and its selection were being sent. The context itself was always sent as the tooltip said; only the display was wrong. Qt style sheets resolve font properties such as `text-decoration` once, without the `:checked` state, so the `:!checked` rule applied permanently. The strikeout is now set on the chip font and follows the chip state.

## Validation

A macOS development build (RelWithDebInfo, Qt 6.11.1) of the application and of `test_app_aichatdockwidget` compiled. The dock suite passed, including a new check that the chip font is struck out only after the chip is clicked off and restored after the next click.

The desktop UI update additionally passed focused Qt suites for chat, recovery, workspace transfers, index snapshots, context filtering and local rules/skills. Changed application objects were compiled and linked in an isolated external build; affected unity groups passed syntax checks. Offscreen widgets were inspected with light/dark palettes and elevated scaling. See [implementation evidence and acceptance limits](../implementation/desktop-ui-cloud.md).

No packaged build has been verified yet for this version.

## Downloads and remaining acceptance

Application source tag: `strata-v1.6.4`. This is a draft: the platform packages (macOS universal DMG, Windows x64 installer and portable ZIP, Linux x86_64 AppImage), their verification records and SHA-256 checksums are produced and sealed by the existing workflows from the tag.

Before publication the same gates as 1.6.3 apply: platform regression methods on the exact packages, Windows signing and runtime checks, macOS code-signature, Gatekeeper and notarization checks, plus the manual acceptance checks still pending from 1.6.3 (Windows 11 Smart App Control field acceptance, interactive GUI check of the final macOS DMG). The cloud backend is unchanged.
