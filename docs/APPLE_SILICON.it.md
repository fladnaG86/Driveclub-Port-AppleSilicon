# Driveclub su Apple Silicon (Rosetta 2)

*English: [APPLE_SILICON.md](APPLE_SILICON.md)*

Questo repository è un fork di [BlackLineInteractive/Driveclub-Port](https://github.com/BlackLineInteractive/Driveclub-Port).
Il backend macOS originale (`src/`) è pensato per i **Mac Intel**: il codice del
gioco PS4 è x86-64 e lì gira nativamente. Questo fork fa girare lo stesso
backend su **Apple Silicon**, eseguendo l'intero processo x86-64 sotto
**Rosetta 2**, e corregge i bug grafici e i problemi di prestazioni emersi strada facendo.

*Progetto non affiliato né approvato da Sony Interactive Entertainment. "Driveclub" e
"PlayStation" sono marchi dei rispettivi proprietari. Questo repository e le sue
release non contengono codice, dati del gioco né chiavi: serve una propria copia
legittima del gioco.*

Tutto è stato sviluppato e misurato su **una sola macchina: MacBook Pro M1 Max,
macOS 27.2, gioco v1.28 (CUSA00003)**. Altri chip Apple Silicon non sono stati provati.

[![Driveclub on Apple Silicon](../media/apple-silicon-race.jpg)](https://github.com/fladnaG86/Driveclub-Port-AppleSilicon/releases/download/apple-silicon-video/driveclub-apple-silicon.mp4)

*Gameplay su MacBook Pro M1 Max sotto Rosetta 2, con l'audio del gioco — clicca per vedere il video (MP4, 3:57). Il Metal Performance HUD (in alto a destra) mostra gli fps reali e il tempo GPU per frame.*

## Stato

| Area | Stato |
| --- | --- |
| Avvio, menu, schermata attract | Funziona |
| Avvio evento, giro completo in gara | Funziona (online e offline, `DC_OFFLINE=1`) |
| Audio | Funziona |
| Input | DualShock/DualSense via SDL3, oppure tastiera (vedi sotto) |
| Grafica | Le scene di gara sono corrette, per quanto confrontato con video PS4; vedi *Lacune note* |
| Prestazioni | ~26-28 fps in gara a 1920x1080 (M1 Max); erano ~12 fps all'inizio del lavoro |
| Stabilità | Gare di diversi minuti senza crash; un watchdog GPU chiude il processo in modo pulito se uno shader si blocca |

## Come giocare

### Avviare il gioco, passo per passo

1. **Controlla i requisiti.** Un Mac Apple Silicon con **macOS 15 o successivo** e **la tua copia di Driveclub
   v1.28 (CUSA00003)**, decrittata ed estratta in una cartella. Quella cartella
   contiene `eboot.bin` e `sce_module/`. Se hai gioco base e patch 1.28 in due
   cartelle, copia prima i file della patch sopra la cartella del gioco base
   (sostituendo `eboot.bin`). Altre versioni del gioco non partono: l'app ti
   avvisa se la tua è una di queste.
2. **Installa l'app.** Scarica `Driveclub-Native-0.2.4.dmg` dalle
   [Release](https://github.com/fladnaG86/Driveclub-Port-AppleSilicon/releases/latest),
   aprilo e trascina **Driveclub** su **Applicazioni**.
3. **Solo al primo avvio: autorizza l'app.** Non è notarizzata da Apple, quindi
   macOS la blocca la prima volta:
   1. Fai doppio clic su **Driveclub** in Applicazioni. macOS dice che non può
      verificarla: clicca **Fine** (non *Sposta nel Cestino*).
   2. Apri **Impostazioni di Sistema → Privacy e sicurezza**, scorri fino a
      *"Driveclub" è stato bloccato…* e clicca **Apri comunque**.
   3. Inserisci la password e clicca di nuovo **Apri comunque**.

   Se macOS chiede di installare **Rosetta**, accetta. Da lì in poi basta il doppio clic.
4. **Scegli la cartella del gioco.** La prima volta l'app la chiede: scegli la
   cartella con `eboot.bin` e clicca **Play**. Viene ricordata; tieni premuto
   **Opzione** mentre apri l'app per sceglierne un'altra.
5. **Aspetta circa 40 secondi** per il caricamento. Il titolo della finestra mostra gli fps.
6. **Nel gioco:** nella schermata *Start Event* premi **Croce** (`Invio`). Nella
   schermata *Controls* premi di nuovo **Croce**. La gara parte.
7. **Per uscire** premi `Cmd+Q`.

Ogni sessione scrive il suo log nella cartella **Driveclub Logs** sulla Scrivania, con
data e ora nel nome (`driveclub-2026-10-19_17-30-45.txt`); restano gli ultimi 20. Una
sessione finita in crash prende `-crash` nel nome (lo vedi alla riapertura dell'app),
una finita con un messaggio di errore prende `-error`. Ogni log inizia con modello del
Mac, chip e versione di macOS, e riporta gli fps ogni 10 secondi. Allega il log della
sessione andata male a una
[segnalazione](https://github.com/fladnaG86/Driveclub-Port-AppleSilicon/issues). La
prima volta macOS chiede di consentire a Driveclub l'accesso alla Scrivania; se rifiuti,
i log vanno in `~/Library/Logs/DriveclubNative`.

**Da Terminale** (variabili di debug, `DC_OFFLINE=1` e così via): il binario dentro
l'app accetta la cartella del gioco come prima e scrive il log nel terminale:
`/Applications/Driveclub.app/Contents/MacOS/driveclub /percorso/del/gioco/estratto`.

I salvataggi vanno in `~/Library/Application Support/DriveclubNative`.
I servizi online del gioco non esistono più; se un menu resta in attesa della
rete, avvia con `DC_OFFLINE=1 ./driveclub /percorso/del/gioco`.

### Comandi

Un controller PS4 (DualShock 4 o DualSense, USB o Bluetooth) funziona come su
console e si può collegare in qualsiasi momento. La tastiera può sostituirlo:

| Tasto PS4 | Tastiera | Nel gioco |
| --- | --- | --- |
| R2 | `W` | Accelera (tastiera: acceleratore a fondo) |
| L2 | `S` | Frena / retromarcia |
| Stick sinistro sinistra/destra | `A` / `D` | Sterza (tastiera: sterzata piena) |
| Croce ✕ | `Invio`, `Spazio` o `K` | Conferma nei menu |
| Cerchio ○ | `L` | Indietro nei menu |
| Quadrato □ | `J` | Azione di gioco (vedi la schermata *Controls* del gioco) |
| Triangolo △ | `I` | Azione di gioco (vedi la schermata *Controls* del gioco) |
| L1 / R1 | `Q` / `E` | Azione di gioco (vedi la schermata *Controls* del gioco) |
| Croce direzionale | Frecce | Muoversi nei menu |
| Options | `Esc` o `Tab` | Menu di pausa |
| Stick destro, L3, R3, touchpad | solo controller | Come su PS4 |

Tasti del port:

| Tasto | Funzione |
| --- | --- |
| `V` | Vsync sì/no |
| `F12` | Debug: salva il trace del frame successivo in `../bench/f12_<frame>` |
| `F11` | Debug: cattura GPU Xcode di due frame (serve `MTL_CAPTURE_ENABLED=1`; molto grande) |

## Compilare e avviare

Questa sezione non serve se usi l'app pronta (vedi *Come giocare*). Per compilarlo da sé:

Servono: Mac Apple Silicon con Rosetta 2, Xcode (command line tools),
CMake >= 3.24, Ninja e **SDL3 compilato per x86_64** (l'SDL3 di Homebrew su
Apple Silicon è arm64 e non si può collegare a un binario x86_64).

```bash
# SDL3 per x86_64 (provato con release-3.2.24)
git clone --branch release-3.2.24 https://github.com/libsdl-org/SDL SDL3-src
cmake -S SDL3-src -B SDL3-build -G Ninja -DCMAKE_OSX_ARCHITECTURES=x86_64 \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PWD/sdl3-x86_64
cmake --build SDL3-build && cmake --install SDL3-build

# Il port (CMakeLists.txt forza già x86_64 e -march=btver2)
cmake -S Driveclub-Port -B Driveclub-Port/build -G Ninja \
      -DSDL3_DIR=$PWD/sdl3-x86_64/lib/cmake/SDL3
cmake --build Driveclub-Port/build

# Avviare dalla radice del repository, indicando la propria copia estratta
cd Driveclub-Port && ./build/driveclub /percorso/del/gioco/estratto/CUSA00003/Image0
```

Serve **una propria copia del gioco**, come pacchetto decrittato ed estratto
(il fork è stato provato con la v1.28). Il repository non contiene file del gioco.

Per impacchettare la build come `Driveclub.app` in un `.dmg` (firma ad-hoc), passa
un SDL3 x86_64 compilato con `-ffile-prefix-map`, così non contiene percorsi locali:
`tools/app/make_app.sh 0.2.4 /percorso/libSDL3.0.dylib` → `dist/`.

## Cosa è stato fatto

I commit sono piccoli e con messaggi descrittivi (in inglese); questa è la mappa.

### 1. Funzionare sotto Rosetta 2

| Problema | Soluzione | Commit |
| --- | --- | --- |
| `-march=native` su host arm64 non produce codice che Rosetta esegue | Target `btver2` (la CPU Jaguar della PS4) | 587a6e5 |
| Rosetta riserva lo spazio x86 `0xFC0000000-0x7000000000`: mappare lì la memoria guest fa uccidere il processo | Lo spazio utente guest parte da `0x7000000000`; l'heap fisso del gioco a `0x1000000000` viene spostato riscrivendo i `movabs` nell'eboot (`DC_HEAP_SITES` per cambiarli) | 40e4166 |
| Crash all'avvio dei moduli: la libc saltava nei dati | Nessun proc param a `module_start` (l'init PRX la chiama in coda) | 2b899c7 |

### 2. Correttezza grafica

| Sintomo | Causa | Commit |
| --- | --- | --- |
| Griglia di sprite/bagliori spazzatura | Shader compute scritti per una wave GCN da 64 omettono `s_barrier` negli scambi LDS; Metal li esegue come due SIMD da 32. Ora i barrier vengono inseriti dove cambia il tipo di accesso LDS | f35ac47 |
| Uno shader compute girava per secondi, GPU bloccata, Mac da riavviare | Differenza dei contatori CE/DE confrontata senza segno (underflow): il constant engine si bloccava e le tabelle di descrittori vecchie davano a un loop di luci un conteggio enorme | 4d8cd93 |
| Shader infiniti potevano bloccare il Mac | Watchdog GPU (`DC_GPU_WATCHDOG`), guardia sui loop tradotti, limiti sulle dimensioni indirette | e419ed7 |
| Vetri/parabrezza dell'abitacolo mancanti | Dual-source blending | cbfc6e7 |
| Abitacolo invaso da colori, enormi aloni attorno alle altre auto | Le livree delle auto sono texture BC3 *codificate dalla GPU* in un render target uint e venivano campionate grezze. Ora sono copiate in una vera texture BC | a4f4a1f |
| Abitacolo sdoppiato / immagine fantasma con i fari | I volumi di luce usano baricentriche LINEAR (senza prospettiva); ora gli input usano `center_no_perspective` | e357073 |
| L'abitacolo spariva per singoli frame alla partenza | La chiave delle pipeline pixel includeva stato inutilizzato: ~1000 compilazioni inutili, e le draw venivano saltate durante la compilazione | b653264 |

### 3. HLE e input

| Sintomo | Causa | Commit |
| --- | --- | --- |
| "Start Event" ignorava il tasto Croce | `scePadRead` rispondeva per qualunque handle; la sonda del volante Logitech (handle -1) creava un volante fantasma che diventava il giocatore 0 | 3ef7c52 |
| Nessun controllo reale | Hot-plug controller SDL3, stick e grilletti analogici, guida da tastiera | 6be57f8 |
| Varie | Callback di stato NP, modalità offline, valori realistici per `sceSystemServiceParamGetInt`, timestamp del pad | 28ef31f, ec6b002, a92baaf |

### 4. Prestazioni: da ~12 a ~27 fps

Misure in gara, 1920x1080, M1 Max. Ogni passo è stato misurato A/B
(`tools/bench_race.sh`); i numeri sono nei messaggi di commit.

| Modifica | Effetto | Commit |
| --- | --- | --- |
| Dimensioni dei dispatch patchate lette dalla GPU (dispatch indiretto) invece di 18 attese CPU per frame | 12.4 -> 14.7 fps | 4f29dd9 |
| Cache delle texture per descrittore | CPU delle draw 42 -> 21 ms/frame | 480d81e |
| `WaitRegMem` controlla anche i fence già registrati da altre code | 15.0 -> 19.1 fps | 134630d |
| Matematica shader "relaxed" | GPU 51.3 -> 48.5 ms/frame | c339b9f |
| Un solo segnale evento per command buffer, dispatch raggruppati | GPU 49.0 -> 40.2 ms/frame | de0ff50 |
| Attributi dei vertici letti da un vertex descriptor Metal | stadio vertex 21 -> 12 ms/frame | 2b56ba0 |
| Meno overhead CPU per draw (cache dei binding, meno allocazioni, controllo shader una volta per frame) | thread CP ~25 -> ~21 ms/frame | 0c3b904, 37d021e, 6b38290 |
| **Basi di memoria tradotte dall'host** (sotto) | GPU 35.2 -> 27.2 ms/frame | d08a95f |
| Risultati di Resolve riusati per programma e descrittori; basi di memoria tradotte una volta per draw | thread CP 22.7 -> 21.1 ms/frame, ~+1-2 fps | 4d6df28 |

**Basi tradotte dall'host (d08a95f).** Gli shader accedono alla memoria guest
tramite una page table. La cattura Xcode dello shader più pesante (il TAA,
4.3 ms) mostrava 196 registri allocati e occupancy quasi nulla. Disassemblando
il suo codice macchina AGX (`tools/agx`) sono emerse ~10.000 istruzioni intere
spese a tradurre indirizzi, contro ~70 in virgola mobile. Ora l'analisi del
programma trova le basi di memoria i cui registri valgono lo stesso su ogni
percorso. Il renderer traduce sulla CPU solo quegli *indirizzi*, a ogni draw.
I dati li legge sempre la GPU, quindi i valori che la GPU scrive più avanti nel
frame restano corretti. La pipeline viene poi specializzata in modo che quegli
accessi indicizzino direttamente il buffer di memoria. TAA: 98 -> 80 registri,
12.8k -> 3.6k istruzioni, 4.3 -> 2.1 ms.

## Cosa resta da fare

**Ora il limite è la CPU.** In gara la GPU è occupata ~68% del tempo (~27 ms di
lavoro per frame), mentre il frame dura ~40 ms. I limiti sono:

1. **Il thread del command processor** (`GPU gfx`): ~22 ms di CPU per frame per
   ~2.200 draw (~10 µs ciascuna sotto Rosetta). Le parti più grosse: `Resolve`
   (descrittori texture, hashing `GetTexture`/`TextureChanged` di ~550 texture
   per frame), creazione dei render encoder (~100 pass, ~50 µs l'uno),
   contesa sul mutex con i 32 thread delle code compute, `LinearTable` (~1.7 ms).
2. **I thread del gioco sotto Rosetta.** Instruments mostra la GPU ferma in 2-3
   pause per frame mentre i worker TBB del gioco calcolano; il thread principale
   attende in spin l'etichetta di fine frame.
3. Lavoro GPU ancora costoso: `cs 0x7000102e00` (sonda di luce dell'abitacolo,
   ~4-5 ms), `ps 0x7068ce9900` (960x540, ~2.8 ms), `cs 0x7002fdf100` (~2 ms).
   `DC_PASS_TIMING` li elenca.

**Lacune note:**

- Formato texture `dfmt=49 nfmt=4` (R4G4) non supportato.
- L'operazione shader `IMAGE_ATOMIC_ADD` non è tradotta.
- Funzioni HLE ancora chiamate e non implementate, finora senza effetti
  visibili: VideoOut `DYhhWbJSeRg`/`pv9CI5VC+R0`, GnmDriver `0H2vBYbTLHI`,
  diverse funzioni NP / WebApi / Rudp / SharePlay (funzioni online), libkernel
  `FxVZqBAA7ks`/`ca7v6Cxulzs`/`4oXYe9Xmk0Q`. 227 import sono ancora irrisolti.
- Provato solo su M1 Max. Solo le scene di gara sono state confrontate con video PS4.
- I dump dei frame non sono riproducibili tra due run (i tempi cambiano): le
  modifiche vanno giudicate con contatori, non confrontando immagini.

**Provato e misurato senza guadagno** (per risparmiarvi il lavoro):
flusso di controllo strutturato nell'MSL al posto della catena piatta `tgt_`
(registri invariati); rimozione delle metà alte delle maschere; broadcast dei
load scalari con `simd_broadcast_first` (i registri uniformi AGX si riempiono
solo nel preambolo per dispatch); risolvere sulla CPU i *valori* dei load
scalari (sicuro solo per i descrittori, e non dava niente); unire i render pass
(solo ~10 su ~100 sono divisioni inutili); tracciamento scritture con
protezione di pagina per le texture (annullato: le scritture GPU non generano
fault, le texture resterebbero vecchie); traduttore di indirizzi a più
intervalli (più lento); commit anticipati quando la GPU è ferma.

## Strumenti di debug e profilazione

Sono variabili d'ambiente, salvo dove indicato. Spenti costano quasi nulla.

| Strumento | Cosa fa |
| --- | --- |
| `tools/bench_race.sh <nome> [VAR=..]` | Avvia un evento, tiene l'acceleratore per 150 s, stampa fps, % GPU occupata, errori, ms GPU/frame, ms CPU del CP/frame |
| `DC_GPU_STATS=1` | Ogni secondo: fps, % GPU occupata, pass e perché sono stati divisi, CPU del thread CP |
| `DC_PASS_TIMING=1` | Tempo GPU per render pass e dispatch, da timestamp ai confini degli stadi (senza divisioni extra) |
| `DC_CPU_PROFILE=1` | Tempo CPU per sezione di una draw |
| `F12` / `DC_TRACE_FRAME=n` | Trace di un frame: ogni draw/dispatch con le sue risorse; `DC_DUMP_PASSES=1` salva i target di ogni pass |
| `F11` / `DC_CAPTURE_AT=n` | Cattura GPU Xcode di due frame (enorme: ~70 GB). Richiede `MTL_CAPTURE_ENABLED=1` e `DC_GPU_WATCHDOG=120` |
| `DC_CAPTURE_CS=<addr>[:flip]` | Cattura Xcode di un solo dispatch (~6.5 GB): aprirla, Replay, Profile per vedere registri e limiter |
| `DC_SKIP_CS=<addr>` / `DC_SKIP_PS=<addr>` | Salta il lavoro di uno shader per misurarne il costo |
| `DC_MSL_OVERRIDE=<dir>` | Carica `<dir>/<stage>_<hash>.metal` al posto dell'MSL generato (esperimenti a mano) |
| `DC_DUMP_SHADERS=<dir>` | Salva i binari degli shader guest e l'MSL generato |
| `DC_SCAN_MAX=...`, `DC_PROBE=...` | Trova valori esplosi per pass/draw; legge i registri di un pixel/thread |
| `DC_NO_LINEAR`, `DC_NO_TEX_CACHE`, `DC_NO_SIGNAL_BATCH`, `DC_SW_FETCH`, `DC_MATH_MODE=safe` | Disattivano singole ottimizzazioni |
| `DC_TRACE_HLE=<sec>`, `DC_LOG_UNRESOLVED_ALL`, `DC_LOG_MISSING`, `DC_PROBE_FN=...` | Trace delle chiamate HLE e sonde su funzioni del gioco |
| `build/gcn_dis <bin> [vs/ps/cs] [--msl] [chiave=valore]` | Disassembla uno shader guest, genera e compila l'MSL, stampa thread massimi e basi lineari |
| `tools/agx/agxstat.sh <file.metal>` | Numero di registri e istruzioni AGX di un kernel (richiede `dougallj/applegpu` clonato in `tools/agx/applegpu`) |
| `tools/msl_check.py` | Test di regressione sull'MSL generato per piccoli shader di prova |

Note pratiche:

- Se un run blocca la GPU, il processo ucciso può restare nello stato `E` tenendo
  occupata la GPU: solo un riavvio lo libera. Controllare
  `ps -axo stat,command | grep driveclub` prima dei benchmark.
- `lldb` perde il processo sotto Rosetta; `sample <pid>` funziona (simboli con `nm`).
- `xctrace`/Instruments richiede una versione di Xcode allineata al sistema operativo.
