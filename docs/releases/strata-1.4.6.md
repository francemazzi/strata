# Strata 1.4.6

This release continues the 1.4.5 Windows signing hotfix. CI now refreshes the
Azure OIDC session immediately before CPack signing, because federated tokens
expire while the Windows compile step runs. Release uploads retry transient
GitHub 502 responses when attaching large macOS DMG assets.

Application behavior matches 1.4.5. Windows packages require Public Trust
Authenticode signatures and the same acceptance procedure as prior candidates.
