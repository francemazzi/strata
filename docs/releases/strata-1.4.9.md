# Strata 1.4.9

Desktop release combining Claude Code subscription (Pro/Max) BYOK, Windows signing
pipeline fixes from 1.4.7–1.4.8, and a Unity Build fix for Plan DataHub/trees tools.

## Highlights

- **Claude subscription via Claude Code**: connect from AI settings or the chat model
  menu; Strata runs `claude setup-token`, stores the token locally, and routes chat
  through the Claude Code CLI with PTY session management.
- **Windows release CI**: BOM-free GitHub Actions env, explicit NSIS `/DNSISDIR`, scoped
  NSIS toolchain signing, NSIS log upload on CPack failure.
- **Build**: separate internal namespaces for DataHub and trees detect tools (fixes
  duplicate symbols under Unity Build on Linux/Windows).

Windows packages still require Public Trust Authenticode and the Windows 11 Smart App
Control acceptance procedure before publish.
