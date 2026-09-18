# Strata 1.4.7

This release fixes Windows NSIS packaging after Azure Authenticode signing. CI now
writes GitHub Actions environment lines without a UTF-8 BOM, passes an explicit
`/DNSISDIR` to makensis, signs the full copied NSIS toolchain (not only plugin
DLLs), and uploads `NSISOutput.log` when CPack fails.

Application behavior matches 1.4.6. Windows packages still require Public Trust
signatures and the Windows 11 Smart App Control acceptance procedure.
