# Strata 1.4.11

Fixes Windows NSIS packaging: CMake must build the `!uninstfinalize` line with
`string(APPEND ... "%1")` so CPack does not corrupt the placeholder (1.4.10 wrote
`;%1"` and makensis aborted on line 43).

Same application content as 1.4.10. Windows still requires acceptance before publish.
