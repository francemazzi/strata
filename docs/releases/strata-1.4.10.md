# Strata 1.4.10

This release fixes NSIS uninstaller signing. CPack wrote a broken
`!uninstfinalize` line (nested quotes around `powershell.exe`), so makensis
aborted with `Usage: !uninstfinalize command_with_%1`. CI now calls a `.cmd`
wrapper so the NSI command is `<script> "%1"` and still signs Uninstall.exe
before it is packed.

Application behavior matches 1.4.9. Windows packages still require Public Trust
signatures and the Windows 11 Smart App Control acceptance procedure.
