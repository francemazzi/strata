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
