# Native Claude transport checks

This target compiles the production Claude authentication, process runtime and
MCP server on Windows and Linux without building the complete QGIS desktop.
Only the QGIS profile/settings boundary is replaced by a small Qt adapter;
subprocesses, TCP, JSON, persistent settings, permissions and cancellation use
the native operating system. The exact same transport test sources also run
against the complete desktop library on macOS. Agent approvals and the chat UI
remain covered by the full desktop suites, not by the portable target.

```sh
cmake -S tests/claude-transport -B build/claude-transport
cmake --build build/claude-transport --config Release
ctest --test-dir build/claude-transport -C Release --output-on-failure
```

All automated fixtures are synthetic. No subscription credentials or live model
requests are used in CI. Real subscription sign-in and packaged desktop
acceptance are recorded separately in the release evidence.
