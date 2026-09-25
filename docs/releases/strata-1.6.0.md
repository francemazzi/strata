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
