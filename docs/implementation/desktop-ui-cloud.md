# Desktop UI e Workspace and Cloud — 9 ottobre 2026

Implementazione isolata da `origin/master` (`666656e6887`) sul branch `feat/desktop-ui-cloud`. Il checkout PostGIS originale non viene modificato. Backend `44e1f0f` e frontend `f1fc367` sono riferimenti di compatibilità, senza modifiche API, database o distribuzione.

## Comportamento

- Chat: azioni copia messaggio/codice, modifica, nuova chat e cronologia a icona, palette di sistema, etichette accessibili e tooltip traducibili. Focus da tastiera, area minima 28 px, icone 18 px; la copia mostra una spunta temporanea.
- Riprendi resta nell'avviso di interruzione, con conto alla rovescia per il cooldown. Riavvia questo turno e Annulla questo turno sono nel menu Altre azioni. Ripresa, streaming, Stop, approvazioni e verifiche mantengono la logica esistente.
- Workspace and Cloud conserva la chiave di navigazione `workspace` e riunisce cartella attiva/origine/autorizzazione, account/prerequisiti e contenuti condivisi. Updates conserva pagina e identificatore. Indexing & Docs e Rules & Skills conservano configurazione/editor e rimandano ai trasferimenti centrali.
- Invio contesto AI, invio regole/skill e importazione sono azioni distinte. Il consenso al contesto resta esplicito; i file GIS originali non sono replicati. Le cartelle web Tavole PDF→DXF non sono presentate come cartelle locali sincronizzate.
- Anteprima e risultato hanno etichette separate: aggiornare l'anteprima non cancella l'esito; accodato non significa indicizzato. L'importazione mantiene la preview dei conflitti e Mantieni locale come scelta iniziale.
- Modifiche non salvate a cartella, credenziali, trust, consenso o indicizzazione bloccano i trasferimenti: il prossimo passo spiega di salvare con OK e riaprire le impostazioni.
- Cambiare account/workspace/progetto/configurazione invalida anteprima e richieste. Le risposte tardive non aggiornano il nuovo contesto; un cambio durante la preview modale impedisce l'importazione. Gli elementi già inviati possono rimanere nel vecchio workspace e la UI lo spiega.
- La preview del contesto legge uno snapshot dell'indice senza attenderne il mutex; scansione delle istruzioni e filtri vengono eseguiti con QtConcurrent. Nessuna scrittura cloud avviene aprendo le impostazioni o aggiornando la preview. Fingerprint e client esistenti rimangono invariati.

## Verifiche locali

Compilazione C++20/Qt 6.11.1 su macOS arm64, fuori dal repository in `/Volumes/LLM_MODELS/strata-desktop-ui-check`. Sono stati ricompilati gli oggetti modificati, collegata una libreria app isolata e ricompilati i test modificati usando gli oggetti QGIS già disponibili in `/Volumes/LLM_MODELS/strata_core-build-pyqgis`. Non è una build pulita completa né un nuovo pacchetto distribuibile.

Sono stati controllati sintatticamente tutti i gruppi unity interessati dall'inserimento del nuovo sorgente (batch 8, gruppi 33, 34 e 37–45). La build originale non è stata riconfigurata o sovrascritta.

Suite Qt eseguite in offscreen: 39 esiti positivi chat, 15 recovery, 19 workspace/cloud, 4 indice (selezione), 9 client cloud e 20 store regole/skill; nessun fallimento. I conteggi Qt comprendono inizializzazione e chiusura della suite. Due controlli visivi aggiuntivi al 200% sono passati.

| Suite | Copertura |
| --- | --- |
| aichatdockwidget | copia e clipboard, tastiera, azioni/menu, streaming, stato runtime, contesto mappa, suggerimenti |
| aiincidentrecovery | ripresa, continuità tool, isolamento del contesto, attesa provider |
| aiworkspacecloud | navigazione, cartella/account, configurazione pendente, esito separato, risposte tardive, nessuna scrittura all'apertura, accodamento/errore/esito parziale, conflitti e cambio workspace durante import |
| aiworkspaceindex (selezione mirata) | snapshot locale, esclusione geometrie e rifiuto root diversa |
| aicloudindexclient | payload, deduplica e filtri sui dati |
| airulesskillsstore | lettura/scrittura locale e limiti del workspace |

Schermate dei widget reali Qt controllate con palette chiara/scura, dock largo 420 px e scala 100%/200%. Log e screenshot sono nella directory di verifica esterna. Le credenziali dei test sono sintetiche; nessun trasferimento verso il cloud di produzione è stato effettuato.

## Accettazione ancora necessaria prima della distribuzione

- Build completa e pacchetti della release, con i gate esistenti di firma e runtime.
- Controllo interattivo del pacchetto macOS e controllo Windows, chiaro/scuro e scaling elevato: il rendering offscreen su macOS non certifica questi ambienti.
- Smoke con account cloud reale e workspace di prova: invio/accodamento, esito parziale e importazione su rete reale. I test locali non certificano l'indicizzazione remota.
