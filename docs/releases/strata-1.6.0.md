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

### Indexing is lighter

- The index database runs in write-ahead log mode, so searches never wait for a write and
  each write costs one disk sync. Layers removed together leave the database in one
  transaction, off the interface thread. Indexes of workspaces not opened for 30 days are
  deleted, and clearing the index also removes its log files.
- Reopening a project no longer embeds its layers again. Closing a project keeps its layers
  in the index (search only uses the layers of the open project); a layer whose files and
  settings did not change is not read again, and any chunk whose text was embedded before
  reuses that embedding. Edits are indexed once saved, not while they are in progress.
  Workspace files are only read again when their modification time changes, and only the
  changed files are rewritten. Layers not seen in any project for 30 days leave the index.
- Background indexing is gentle on the computer: the local model uses two threads by
  default instead of every core ("Indexing speed": low 1, normal 2, high up to 4), its
  threads run at low priority and wait without spinning, and indexing pauses between
  batches, while the map is being panned or zoomed, and while the computer runs on battery
  (`strata/index/pause_on_battery`). Indexing asked for explicitly runs at full speed.
- The local model gives its memory back (about 300 MB) after 3 minutes without indexing or
  search (`strata/index/model_idle_unload_s`), and loads again when needed. Texts are
  embedded in batches of similar length within a token budget, so short chunks are not
  padded to the longest one and the memory peak stays lower.
- Chunks fit the local model: they are sized with its own tokenizer (at most 450 of its 512
  tokens) instead of 1200 characters, which cut numeric CSV and project XML in half. CSV,
  TSV, GeoJSON, QGIS project and XML files are indexed from a summary (columns and types,
  row count and sample rows; features, geometry types and extent; project layers, CRS and
  layouts, without credentials) instead of their raw text; CSV files larger than 256 KB are
  summarized from their start instead of skipped. The index is rebuilt once after updating.
- Workspace content reaches a remote embedding service (OpenAI, OpenRouter, Strata Cloud)
  only after the user agrees, once per service, in a dialog that lists what is sent; until
  then indexing and search with that service do nothing. With the local model nothing
  leaves the computer and no question is asked (the layer indexing question used to appear
  on OK in the AI settings, after the layers were already processed). Each turn of chat
  adds at most 16 KB of retrieved context, and matches much weaker than the best are left
  out.
- The chat header shows what indexing is doing ("Indexing · files 42% · layer 8 of 20"),
  with Pause and Resume, a tooltip with the details, and a click to the indexing
  settings; it also says when indexing waits for mains power or cannot run (model not
  downloaded, consent missing, network folder skipped). The indexing settings add the
  speed, pausing on battery, reading features of remote layers, folders not to index, the
  number of files, the size of the index and a Clear index button. "Rebuild now" walks the
  workspace in its task instead of the dialog.

### Assistant tools never freeze the window

- `capture_map_canvas` draws in the background and returns after 20 seconds at most, with
  what was drawn so far and a warning naming the slow layers; Stop ends the drawing. It used
  to wait for every layer, so an unresponsive WMS froze Strata.
- `search_files` and `list_files` walk the workspace in the background with the same
  exclusions as indexing and say when the result was cut; `list_project_layers` counts the
  features of local layers only ("unknown" for remote ones, which would otherwise query the
  server). `install_python_package` runs pip as a separate process that Stop ends, checks
  the interpreter once per session, and makes new packages importable at once.
- `add_layer_from_file` and `add_layer_from_service` open the layer, check it and compute
  its extent in the background; a large file no longer freezes the window, and Stop adds
  nothing to the project.
- `calculate_field` and `batch_update_attributes` write their values a slice at a time, so
  the window keeps drawing; Stop while writing leaves the layer as it was. Saving a layer the
  tool put in edit mode is still one step, to stay atomic: about 0.25 s for 200 polygons of
  20,000 vertices in a GeoPackage.
- Database tools: `query_sql` and `execute_sql` run in the background and Stop cancels the
  query on the server. A read runs in a read-only transaction, so a function that writes
  is refused by PostgreSQL itself, and calls such as `pg_terminate_backend`, `nextval` or
  `dblink_exec` count as changes that need approval. Queries stop after 30 seconds on the
  server (`strata/ai/sql_timeout_s`, approved writes at least 5 minutes), and a SELECT only
  returns the rows the tool can show instead of the whole table. `describe_database_schema`
  reads the catalog in the background with estimated metadata. `export_layer_to_postgis`
  writes the rows in the background into a new table that replaces the old one only once
  the export succeeded, so a failed or stopped export leaves the existing table intact; it
  no longer runs `VACUUM FULL`, which locked and rewrote the table.
- Stop answers within half a second even when a tool is stuck in a call that cannot be
  interrupted (opening a huge file, a slow server): the work ends on its own afterwards.
- MCP tools that change data carry an idempotency key: when a call is stopped or times out,
  the model is told the outcome is uncertain and must check before retrying, and a retry
  with the same arguments returns the first outcome instead of running twice (needs the
  matching strata-be). Stopping a Data Hub extraction or a tree detection also cancels the
  job on the server, which stops spending quota.
- Chat replies made only of tool calls are kept in the chat history even when it is stored
  unencrypted; they were lost, leaving the tool results without their call.

### The assistant feels like Cursor

- The first prompt acts: new profiles have the assistant's tools on and start in Agent mode,
  which applies changes that can be undone directly and still asks before a database write
  or a remote change that Strata cannot take back (`execute_sql`, `export_layer_to_postgis`,
  MCP tools that change data). The mode picked in the chat is the one Strata starts with
  next time, and a long task pauses for Continue after 20 rounds of tool calls instead of 5.
  Profiles that turned tools off in the AI settings keep their settings and start in Plan.
- Each tool call shows up in the chat while it runs: what it does ("calculate_field ·
  Parcels · AREA = $area"), how long it has been running, its progress, and Stop. The result
  card says what changed in the tool's own words and how long it took; the raw result is
  under "Result details", closed, and rollback tokens no longer show.
- Undo from the chat: changes that can be undone have an Undo button on their card, and the
  user message has "Undo this turn", which undoes every change of that answer, newest first.
  Strata runs the tool's own rollback directly, without asking the model, and tells the
  model in the chat which changes were undone. Changes Strata cannot undo say so on their
  card.
- Messages: answers are formatted while they stream instead of only at the end, and the
  chat no longer jumps to the bottom while you read higher up. Every message can be copied,
  and so can code blocks without opening them. A question can be edited or asked again
  (also from the error banner, with Retry): the answer after it is dropped and its changes
  are undone first; an answer that changed something for good is kept, since asking again
  would change it twice.
