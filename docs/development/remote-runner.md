# STRATA remote runner

`strata-runner` is a separate, outbound-only executable shipped with STRATA Desktop. The first supported capability is `project.inspect`; it reads project CRS and layer metadata through the existing low-risk Desktop tool registry. It does not execute a payload received from the cloud and rejects every unknown capability.

## Local staging

Pair the runner with a one-time code created in the Remote Agents workspace console. Plain HTTP is accepted only for loopback hosts; every non-loopback endpoint must use HTTPS.

```sh
strata-runner --pair <one-time-code> --api-url http://127.0.0.1:3000
```

The machine credential is stored in the operating-system keychain. The installation id and API URL are non-secret Qt settings. Restart STRATA Desktop after the first pairing so its local bridge can read the credential and open the user-only local socket.

Start the long-running process:

```sh
strata-runner
```

Get the fingerprint required by a task:

```sh
strata-runner --fingerprint /absolute/path/project.qgz
```

The fingerprint binds the canonical path, file size and last modification time. A changed or different project is rejected before a tool runs.

## Security boundary

- The cloud connection is outbound HTTPS, except loopback HTTP in local staging.
- Desktop IPC uses `QLocalServer` with user-only access and authenticates every frame with the keychain credential.
- Frames larger than 1 MiB, malformed JSON, fingerprint mismatches and unknown capabilities fail closed.
- The bridge calls only an existing low-risk, no-approval tool and never interprets remote payload instructions.
- STRATA Desktop displays the active task and states which metadata leaves the machine.
- Backend leases, revocation, cancellation, idempotency and replay protection remain authoritative.

Mutation workflows are intentionally not enabled by this first implementation. They require local approval UI, diff/rollback and post-verification before their capabilities can be added to the allowlist.
