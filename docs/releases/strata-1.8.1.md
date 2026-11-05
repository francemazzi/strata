# Strata 1.8.1

Claude subscriptions can run the Strata GIS agent through the unmodified official
Claude Code application. Connect the subscription under **Models & Providers**,
choose a supported Claude model, then select **Use in this chat**. Installation
and update guidance is available in the same panel.

- The Strata account and selected AI provider remain separate. Connecting Claude
  does not select it automatically, and provider failures do not switch billing
  to another service.
- Claude Code handles subscription sign-in, renewal and logout in a dedicated
  Strata profile. The retired direct OAuth flow and its Strata-owned tokens are
  removed; existing Anthropic API keys and model preferences are preserved.
- GIS tools retain Strata approval, verification, undo, cancellation and tool
  budgets through an authenticated local MCP connection. Native CLI tools and
  unrelated MCP servers are disabled. Duplicate request ids do not repeat tools.
- Streaming, map captures with existing consent, model reporting, interrupted-turn
  context and rate-limit cooldown are integrated into the existing chat.
- CLI conversations are not persisted; Strata retains its own history handling.

Validation on 2026-11-05: the complete macOS desktop build and affected AI test
suites passed. A real subscription signed in through the native settings panel,
answered in the chat, executed `list_project_layers`, and correctly reported zero
layers in an empty project. Native Windows/Linux transport checks and signed
package verification are release gates; this candidate must remain unpublished
until their evidence is complete.
