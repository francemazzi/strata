# Strata 1.4.8

This release fixes NSIS toolchain signing on CI. The 1.4.7 build signed the
entire NSIS tree recursively and failed on helper PE files such as
`RegTool-x86.bin`. CI now signs plugin DLLs, `makensis.exe`, and `Bin/*.exe`
only, while keeping BOM-free `GITHUB_ENV` and explicit `/DNSISDIR` for CPack.

Application behavior matches 1.4.7. Windows packages still require Public Trust
signatures and the Windows 11 Smart App Control acceptance procedure.
