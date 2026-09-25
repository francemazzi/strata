# Strata 1.6.0 (draft)

Draft notes for the AI optimization roadmap (`feat/ai-ottimizzazione-indicizzazione-ux`).
Numbers come from `scripts/ai/run_scenarios.py` on the development machine unless noted;
the Windows reference machine measurements are still to be done.

## Changes

### Indexing never blocks the window

- Diagnostics: `STRATA_AI_GUI_STALL_MS` logs interface stalls and the AI work behind them
  under "AI/Perf"; `scripts/ai/` has an E5 benchmark, a test dataset generator and
  scenarios that drive a real Strata build with a throw-away profile and a loopback chat
  provider. With `STRATA_AI_NO_KEYCHAIN` set, AI credentials stay in memory for the session
  instead of the system keychain.
- The local E5 model is no longer loaded on the interface thread at startup or on every
  AI settings OK: checking availability only looks at the files, and the model loads on
  the first embedding, in the background. A damaged tokenizer file is reported instead of
  crashing Strata.
- Workspace scans skip version control, cache and virtual environment folders at any
  depth instead of walking them, stop after a time budget, skip network shares unless
  allowed (`strata/index/allow_network_workspace`) and OneDrive placeholders on Windows.
- Layers are prepared for indexing on the interface thread from metadata only; their
  features are read in the background. Geometries are only turned into WKT when the
  privacy setting allows them in the model context, and layers from remote services are
  indexed from their metadata unless `strata/index/include_remote_layers` is on.
- File and layer indexing, the index tools and the settings dialog no longer wait on each
  other: the embedding model is held one batch at a time, so a chat search waits at most
  one batch; the index cache loads in the background when a project opens; OK in the AI
  settings reloads the model and re-embeds layers only when the embedding provider really
  changed. Indexing stops within one batch when canceled, when an import starts or when
  Strata quits, and a request that arrives during a pass is run afterwards instead of
  being dropped.
- The project health check behind the chat's suggestion card samples layer geometries in
  the background. It used to read up to 200 features of every vector layer on the interface
  thread whenever layers changed (about 0.85 s with 100 layers) and again before every
  model round; the model context now reuses the last completed check.
