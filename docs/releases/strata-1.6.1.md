# Strata 1.6.1

Candidate release. Publication requires the platform, Windows and updater acceptance gates.

- Fix managed chats stopping after successful web reads with remote_content_not_allowed.
- Resume interrupted AI responses while preserving completed tools and project changes.
- Separate hiding an error from restarting a turn; improve temporary-error and SSE handling.
- Add Updates to application and assistant settings: check, download, verify, install and restart.
- Verify update signatures, package integrity, architecture and application identity before installation.
- Preserve profiles, projects and chat history; retain the previous application for recovery.

Users upgrading from 1.6.0 must install 1.6.1 once using the existing download process.
The integrated updater is available from 1.6.1 onward for official Windows, macOS and AppImage distributions.

Backend compatibility and candidate evidence are recorded in the backend 1.6.1 gateway notes.
Native installation acceptance remains pending; this document is not a publication receipt.
