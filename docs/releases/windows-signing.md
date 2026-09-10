# Windows release signing and acceptance

Release builds require Azure Artifact Signing Public Trust. Sigstore bundles prove
artifact provenance but do not replace Authenticode or Smart App Control testing.
The known `0xc0e90002` incident is an Application Control policy block involving
`bin/OpenCL.dll`; it is not proof that the DLL bytes are corrupt.

## Azure prerequisites

Use the company's validated legal identity. The currently selected Azure CLI
subscription is named Microsoft Azure Sponsorship; resource access still requires
interactive login. Do not infer an existing signing account from cached login data.

1. Sign in with `az login --use-device-code` and inspect existing
   `Microsoft.CodeSigning/codeSigningAccounts` resources.
2. Reuse a valid Public Trust profile belonging to the company. Otherwise register
   `Microsoft.CodeSigning`, create a Basic signing account in West Europe, and
   complete organization identity validation in the Azure portal before creating
   the Public Trust profile. Do not use PublicTrustTest or PrivateTrust for users.
3. Create a GitHub environment named `windows-release`, allowing only tags matching
   `strata-v*`. Use a dedicated Entra application/service principal with federated
   issuer `https://token.actions.githubusercontent.com`, audience
   `api://AzureADTokenExchange`, subject
   `repo:francemazzi/strata:environment:windows-release`.
4. Assign `Artifact Signing Certificate Profile Signer` only on the selected
   certificate profile. The identity-verifier role belongs to the human performing
   company validation, not to CI. No client secret is required.
5. Set environment secrets `AZURE_CLIENT_ID`, `AZURE_TENANT_ID`,
   `AZURE_SUBSCRIPTION_ID`; set environment variables
   `AZURE_ARTIFACT_SIGNING_ENDPOINT`, `AZURE_ARTIFACT_SIGNING_ACCOUNT_NAME`,
   `AZURE_ARTIFACT_SIGNING_CERT_PROFILE_NAME`. For West Europe the endpoint is
   `https://weu.codesigning.azure.net`.

CI first checks configuration, authenticates, signs a real probe, and prepares a
private NSIS toolchain with signed plugin DLLs. A failure stops the build.
Dependencies with an existing invalid signature must be rebuilt from their
verified source; the pipeline will not silently replace that signature.

## Build and seal

The hotfix starts at released commit `e0bc105616a3b11277297fb5898d646431417fa3`.
Only release/signing changes belong in the candidate; keep ongoing feature work in
its existing checkout. The local `strata-v1.4.3` tag was divergent when investigated.
Merge the release-workflow corrections into `master` before tagging, so the sole
sealing workflow runs from the corrected default branch. Use an immutable new
`strata-v1.4.4` tag on the completed hotfix commit, without merging feature changes
from `master` into the release candidate.

Tag-triggered builds prepare a draft and upload platform receipts bound to the
source commit and SHA-256 hashes. Windows additionally uploads
`windows-verification.json`: signature inventories for ZIP and installed EXE,
matching payload checks, OpenCL x64 and bundled Python/GIS runtime checks, and
uninstallation. Windows Server CI does **not** count as a Windows 11 SAC test.

The `Sign release assets` workflow is the sole Sigstore writer. Once all three
platform receipts exist, it verifies them, signs the final binaries and
`release-manifest.json`, and leaves the release in draft. No `latest` lookup is
used. Download failures, changed source, missing platform files or failed checks
cannot produce a publishable candidate.

A retry with identical bytes is a no-op. Changed files are never overwritten.
For a failed draft rebuild, first inspect its state and explicitly remove only
the affected draft files, their platform receipt and any existing seal/signatures,
then rebuild and repeat acceptance. A published release always requires a new tag.
Manual Windows rebuilds must be dispatched **on the release tag**, with
`release_tag` and `ref_to_build` resolving to the same commit. Manual macOS
packaging also verifies the source workflow commit and requires notarization.

## Windows 11 acceptance

Use a clean Windows 11 x64 machine with Smart App Control **On**, not Evaluation,
and keep Defender enabled. Download the candidate EXE, ZIP and sealed manifest.
Record the start time before testing and retain package hashes. Test:

- Fresh installation, ordinary GUI launch and launch after reboot.
- Portable extraction into a separate directory and ordinary GUI launch.
- Loading vector/raster data, opening/saving a project, Processing and PyQGIS.
- OpenCL initialization both with a supported GPU and with no OpenCL platform;
  no available platform is acceptable, a DLL load failure or crash is not.
- Upgrade from the previous package, preserving user profiles and projects.
- Uninstallation and absence of CodeIntegrity enforcement blocks for the candidate.
- Reproduction on the affected colleague's PC using these exact package hashes.

The installer retains the old installation directory and registry identity while
showing the Strata release version. It must not delete user profiles or projects.
If the old uninstaller is blocked, install the verified new package over the
existing installation before testing its newly signed uninstaller. Do not disable
Windows security or replace DLLs from download sites.

After completing the scenarios, run `collect-windows-acceptance.ps1` on the test
machine with `-ManifestPath`, `-Installer`, `-Zip`, `-InstalledRoot`, `-PortableRoot`
and `-TestStartedAt` (ISO timestamp). It checks Windows/SAC state, package hashes
and CodeIntegrity logs and asks the tester to confirm each performed scenario.
The clean test session must contain no enforcement event 3077, including temporary
NSIS paths. If unrelated software creates a block, investigate and repeat the
session on a clean test machine; do not discard events by installation path.
It produces `windows-acceptance.json`; it does not manufacture test success.
Organization-managed policy exceptions remain an IT decision.

## Publish or stop

From the reviewed checkout, with `gh` and `cosign` installed:

```sh
node scripts/ci/publish-release.mjs strata-v1.4.4 windows-acceptance.json docs/releases/strata-1.4.4.md
```

This verifies the sealed manifest and all binary Sigstore bundles, the automatic
Windows report and manual acceptance, then rechecks remote hashes and the tag
before publishing, including signatures, checksums and unexpected assets. It cannot publish with absent or incomplete acceptance.
Publication does not trigger rebuilding any platform.

If a gate is missing, leave the draft unpublished. If a regression is found after
publication, withdraw the release and prepare a new hotfix rather than changing
its files. Record the incident and the affected hashes. Signing does not guarantee
immediate SmartScreen reputation or override a company's application-control policy.

Sources: [Azure setup](https://learn.microsoft.com/en-us/azure/artifact-signing/quickstart),
[OIDC](https://learn.microsoft.com/en-us/azure/developer/github/connect-from-azure-openid-connect),
[Smart App Control tests](https://learn.microsoft.com/en-us/windows/apps/develop/smart-app-control/test-your-app-with-smart-app-control).

## Implementation evidence (2026-09-10)

[PR 51](https://github.com/francemazzi/strata/pull/51) carries the isolated hotfix
and the corrections back to `master`. The candidate release is a draft with no
packages and no new release tag while Azure setup is blocked.

[Windows CI run 34464326361](https://github.com/francemazzi/strata/actions/runs/34464326361)
passed 10 Node integrity/CPack checks, 11 PowerShell failure scenarios, real
Authenticode DLL/PYD/ZIP verification, preservation of existing valid signatures,
rejection of modified binaries and native-process exit-code checks. The certificate
was a temporary test certificate trusted only on the disposable runner and removed
at the end. This is not an Azure Public Trust signature or a Windows 11 SAC test.

Local pre-commit and workflow syntax checks passed; legacy local-action metadata
and pre-existing shellcheck findings are outside the targeted actionlint pass.
Full application builds and release acceptance remain separate gates. No claim of
resolution on the affected PC is made until its exact candidate hashes pass.

The live draft lookup was additionally checked against GitHub release 386172503.
Pending draft tags are resolved through GitHub CLI's GraphQL lookup before reading
the REST release by ID; `releases/tags` alone cannot find such drafts. The Node
suite now contains 11 tests, including this lookup and authentication failures.
