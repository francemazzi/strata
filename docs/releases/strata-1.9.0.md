# Strata 1.9.0

Strata 1.9.0 lets the assistant use a local Ollama server, fixes assistant tools that failed in real chats, checks coordinate transformations in every packaged release, and aligns Strata with QGIS master as of 10 November 2026. It uses the same Strata Cloud endpoint as 1.8.2, `https://api.getstrata.org`.

## AI assistant

- **Local models with Ollama (preview).** When an Ollama server is running on this machine, the model menu of the chat shows an **Ollama (local)** section with the installed models that support tool calling. Embedding models and chat models without tool support are left out. Prompts, project context and tool results stay on your machine, as long as the server address is a local one.
  - Strata only detects the server: it does not start Ollama, install it or download models. The server is checked at startup, when the model menu opens, when Strata comes back to the foreground and from the settings page. There is no background polling. When the server stops, its models leave the menu and the active provider is not replaced.
  - **Models & Providers → Ollama (local)** shows the server status, the model and the context window requested from the server (`num_ctx`, 32768 tokens by default and never more than the model supports). The server address is under **Advanced endpoints** (default `http://127.0.0.1:11434`, or `OLLAMA_HOST` when set), with a warning when it is not on this machine. No model is picked for you, and no API key is involved.
  - Requests wait up to 10 minutes, because Ollama sends nothing while it loads a large model. The conversation history is sized to the context window of the local model.
  - Local models are slower and less reliable at tool calling than cloud models. In our tests on a 64 GB Mac, `qwen3:8b` answered in about a minute but often stopped after one tool, and a 27B model gave correct answers in 1.5 to 4 minutes per reply. Prefer the largest tool-capable model your machine can run.
- **Category styles with your colors.** `style_layer_advanced` applies the classes and colors given in the request. Before, a class list in an unexpected shape was ignored without an error, and the layer got generated colors. A list in another shape, or a color that cannot be read, now returns an error that says what to send. Color ramps also accept ColorBrewer scheme names such as `Blues` or `RdYlGn`. The result lists every class with its feature count and the number of features that fall in no class, and categorized styles now count features correctly.
- **Processing options by name.** Choice parameters of Processing algorithms accept option names as well as numbers, for example `count` and `sum` for the summaries of a spatial join or `flat` for the end cap of a buffer, also as a list where several choices are allowed. Option names that contain a comma are not split. When a value is not recognized, the error lists the accepted options.
- **Read-only SQL never adds layers.** `query_sql` used to ignore `load_as_layer` and return a preview that read like a loaded layer. It now returns an error that points to `execute_sql` with `load_as_layer`, which asks for approval and is not offered in Ask mode.

## QGIS

Strata now includes QGIS master as of 10 November 2026, except its four most recent changes, which conflict with Strata build changes and will follow in a later release. Among the changes:

- the dialog that imports a layer into a database can also save its style there, and GeoPackage connections support saving styles;
- the browser shows field domains in field names and tooltips, with an icon on fields that have one;
- attribute forms: closing the project while the new feature dialog is open no longer crashes, and adding a child feature with geometry from a relation editor keeps the dialogs in the right order;
- the widgets of the 3D map configuration dialog no longer overlap.

## Build

- The Linux AppImage is built without native Wayland support. The QGIS code of 9 November enables it by default and needs Qt WaylandClient, which the Ubuntu 24.04 build does not install. The AppImage runs through X11 or XWayland, also in Wayland sessions, as before; the only native Wayland code is pointer locking for 3D navigation, which the AppImage could not use.
- Release checks now transform coordinates inside the packaged app on macOS, Linux and Windows. They reproject known points between WGS 84 and UTM 32N, run the Processing reprojection, and require the PROJ database, checked through the "Monte Mario to WGS 84" operations. Before, they only checked that coordinate reference systems were valid. On 5 November a reprojection kept longitude and latitude values in a UTM layer while both systems looked valid.

## Updating

From 1.7.1 or newer, install 1.9.0 over the existing installation. From 1.6.8 or older, follow the 1.7.1 notes: profiles that use the retired Google Cloud Run addresses switch to `https://api.getstrata.org` automatically after the update.

## Validation

- Pull request builds:
  - the Ollama provider and the assistant tool fixes each built on macOS (arm64 and x86_64, with the DMG) and Windows;
  - the QGIS synchronization of 10 November built on macOS (arm64 and x86_64, with the DMG) and Windows. It contains the synchronization of 9 November, which had built on Windows;
  - the AppImage change built the 9 November QGIS code on Ubuntu 24.04.
- Master with all the changes above (`aa325ee629a`) built on macOS (arm64 and x86_64) and Windows. The DMG job was cancelled because no runner was available.
- The new coordinate checks ran against the published 1.8.2 AppImage with the `strata-candidate-postgis.yml` workflow and passed:
  - WGS 84 to UTM 32N and back, and the Processing reprojection, within 0.03 mm of the values from PROJ's `cs2cs`;
  - WGS 84 to Monte Mario Italy zone 1 (EPSG:3003) within 75 m. Without a datum transformation chosen in the project, PROJ uses its approximate operation, so the check allows 150 m;
  - the PROJ database is found and lists the "Monte Mario to WGS 84" operations.
  The Windows version of these checks runs for the first time in this release.
- Local macOS development builds (2026-11-10):
  - assistant tool fixes: `test_app_aitoolregistry` and `test_app_aidatabasetools` pass;
  - Ollama: `test_app_ailocalservercatalog` (7 cases), `test_app_aimodelrouter` (80 passed, 5 live tests skipped), `test_app_aichatdockwidget` (40) and `test_app_aiagentsessionmanager` (78) pass. Against Ollama 0.40.14, the live round trip with `qwen3:8b` runs a tool call, sends the result and gets a final answer, and the settings list 13 of 19 installed models.

## Release verification

The release candidate is built from the immutable `strata-v1.9.0` tag. Windows, macOS, and Linux packages must be produced from this source revision. Checksums, platform verification records, signing, and the release seal are required before the candidate is published. A draft is not an installer release.
