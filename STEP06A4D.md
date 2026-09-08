# Step06A4D — checkpoint di sviluppo, 8 settembre 2026

Ripresa dallo Step06A4C presente sul PC. Questo documento va letto insieme a
`C:\PROGETTO_DLSS\RICOSTRUZIONE_2026-09-08\STATO_PROGETTO.md`, che fotografa
lo stato precedente alla ripresa dello sviluppo.

## Risultato e limiti

Nuovo runtime compilato in `build\step06a4d-player\runtime`. La correzione
dei vettori NVOF mantiene le decisioni dello Step06A3/A4C e riduce il costo CPU.
Il film di riferimento parte dall'inizio, con DLAA, output 1918x1080,
NVOF MEDIUM / griglia 2, stabilizzazione AUTO_FILM_GRAIN, AI depth asincrona,
DLSS-G OFF. Il log ReShade conferma creazione e valutazione della feature 18 NR.

I confronti seguenti sono campioni di telemetria della riproduzione, non misure
di qualità visiva né una garanzia di prestazioni in altre scene/configurazioni.
Il player resta sotto i 23,976 fps del film. La precedente eccezione Streamline
non è considerata risolta alla radice.

## Modifiche

- `src/NvofConfidenceRepair.h`: helper separato con texture e infill distribuiti
  per righe, classificazione su 16 gruppi con istogrammi indipendenti, mediana
  globale esatta su istogramma S10.5. Riutilizza i buffer e i vicini già raccolti
  per calcolare la deviazione locale. Soglie, copertura e regole di riempimento
  sono quelle precedenti.
- `src/OpticalFlowEngine.cpp`: integra l'helper prima di FilmMotionStabilizer;
  telemetria `repairVersion=4D histogramMedian=1`.
- `src/DLSSFrameGeneration.h`: quando `slSetD3DDevice` fallisce, chiude la sessione
  fallita e resta sul device nativo. Factory e swapchain vengono agganciate solo
  dopo un hook del device riuscito. È una protezione contro la prosecuzione con
  uno stato parziale; non identifica la causa dell'eccezione originale.
- `tests/`: riferimento A4C congelato, confronto dei risultati, test del fallback
  Streamline con errore iniettato e prova NVOF sulla GPU reale.
- `patches/nvof-step06a3-cost.patch`: conserva le modifiche SDK già esistenti,
  precedentemente nascoste in `external/` ignorata da Git. Abilita il cost buffer
  e le conversioni R8_UINT. Non è una nuova modifica della policy NVOF.
- `tools/`: build isolata, avvio con archivio dei log, confronto della telemetria.

L'helper richiede vettori NVOF grezzi in multipli esatti di 1/32 pixel. Non deve
essere spostato dopo la stabilizzazione, che produce valori non quantizzati.

I cambiamenti preesistenti in `src/main.cpp`, `src/OpticalFlowEngine.h` e nel
resto di `src/OpticalFlowEngine.cpp` sono stati mantenuti. Nessun commit creato.

## Verifiche completate

- Build Release del player con Visual Studio Build Tools 2022, MSVC 19.38.
- CTest: 3/3 PASS, inclusa la prova GPU.
- 376 casi: vettori identici bit per bit al riferimento A4C e statistiche della
  policy identiche. Dimensioni piccole/dispari, griglie 1/2/4, diversi contenuti,
  soglie limite, risoluzioni fino a 3840x2160 e reset/ridimensionamento.
- GPU RTX 5080, driver 616.56: traslazione sintetica nota di 8 pixel, errore
  mediano circa 0,0004 pixel e reset della storia verificato.
- Fallback Streamline: errore 24 iniettato, shutdown chiamato una volta,
  nessun hook DXGI successivo; test separato della condizione di abilitazione.
- Patch SDK: `git apply --check` sulla copia originale locale riuscito.
- Launcher PowerShell eseguito: apre il film e archivia i log precedenti.
- Apertura interattiva verificata sullo schermo; chiusura ordinata con
  `slShutdown result=0`. Questo non prova seek ripetuti, cambio risoluzione,
  Frame Generation o lunghe sessioni senza errori.

Microbenchmark finale, 12 esecuzioni alternate A4C/A4D per scenario: mediana
dell'helper da 3,0 a 3,9 volte più veloce. A 1918x1080 i tre casi passano da
13,78 / 14,33 / 20,45 ms a 4,47 / 4,72 / 6,29 ms. È un test isolato su dati
sintetici; il guadagno dell'intero player è molto inferiore.

## Confronto sul film in background

File: `X:\MOVIES\FILM\[1080p] The Exorcist\The.Exorcist.1973.1080p.BrRip.x264.bitloks.YIFY.mp4`.
Metadati: 1920x1072, decode 1918x1070, risorse NGX/output 1918x1080,
griglia NVOF 959x535. Audio inglese, sottotitoli OFF.

Finestra misurata: da 20 a 120 secondi dopo il primo campione Pipeline di ogni
avvio; processi avviati con finestra nascosta. Un A4D finale confrontato con
due avvii A4C separati. I campioni delle singole fasi non corrispondono agli
stessi identici fotogrammi, perché il player scarta frame per seguire l'audio.

| Metrica media | A4C, controllo ripetuto | A4D finale |
| --- | ---: | ---: |
| Correzione confidence | 11,709 ms | 7,801 ms |
| Classificazione | 4,843 ms | 1,902 ms |
| NVOF totale | 31,713 ms | 28,088 ms |
| Elaborazione fotogramma | 54,061 ms | 51,091 ms |
| Frame inviati al renderer (`submitFps`) | 15,294 | 16,347 |
| Incremento frame scartati nella finestra | 860 | 755 |

Confidence circa -33%; tempo del fotogramma circa -5,5%; submitFps circa +6,9%.
L'altro avvio A4C misura 55,30 ms e 15,10 submitFps sulla stessa finestra,
confermando la direzione del risultato. `submitFps` non è la frequenza del
monitor né include eventuali frame generati. Le prestazioni con finestra
visibile vanno misurate separatamente.

La prima variante A4D, con texture/infill divisi in sole 16 fasce, era veloce
nel microbenchmark ma peggiorava la riproduzione. È stata scartata dopo il
test reale; le sue evidenze sono conservate con prefisso `v1-regression-`.

## Controllo con finestra visibile

Seconda coppia di avvii sullo stesso film, stessa configurazione, finestra
portata in primo piano. Campione da 40 a 100 secondi dopo la prima Pipeline,
per escludere avvio e iniziale attivazione della finestra. Risultati in
`build\step06a4d-validation\exorcist-visible-comparison.json`.

| Metrica | A4C | A4D |
| --- | ---: | ---: |
| Confidence media | 9,410 ms | 6,719 ms |
| NVOF totale medio | 30,311 ms | 26,900 ms |
| Tempo fotogramma mediano | 52,048 ms | 50,813 ms |
| Tempo fotogramma medio | 54,943 ms | 61,912 ms |
| Tempo fotogramma p95 | 71,318 ms | 111,921 ms |
| Renderer medio | 8,008 ms | 17,544 ms |
| SubmitFps medio | 12,986 | 13,665 |
| Incremento frame scartati | 702 | 612 |

Il vantaggio della fase modificata resta presente (confidence circa -29%),
ma il tempo medio dell'intero fotogramma peggiora per picchi nella fase
renderer. La telemetria disponibile non basta ad attribuirne la causa.
Questa prova non certifica un miglioramento della regolarità della
presentazione a schermo: lo Step06A4D rimane sperimentale e non viene promosso
alla Release ufficiale. La media di `submitFps` è una media dei campioni del
contatore, non l'inverso della media dei tempi Pipeline.

Durante il controllo visivo il film, i comandi e lo stato NGX sono visibili;
questo è un controllo di avvio, non un confronto qualitativo dei fotogrammi.
Il player Step06A4D è stato lasciato aperto al termine delle misure.

## Riprodurre build e avvio

Da PowerShell 7:

```powershell
& 'C:\PROGETTO_DLSS\DLSS-Media-Processor\tools\Build-Step06A4D.ps1' -GpuTests
```

Lo script usa i percorsi locali verificati di CMake, CUDA 13.4 e TensorRT-RTX
1.6.1.120; richiede SDK/modelli/runtime già presenti. Ricostruisce in directory
separate, controlla le patch SDK, esegue CTest e copia solo il nuovo EXE nel
runtime A4C clonato. Le 18 DLL/addon vengono confrontate con la base tramite
SHA-256. Normalizza le variabili ambiente `Path`/`PATH`, che nell'ambiente
desktop causavano un errore MSBuild sulle chiavi duplicate.

Avvio interattivo (anche da Windows PowerShell):

```powershell
& 'C:\PROGETTO_DLSS\DLSS-Media-Processor\tools\Start-Step06A4D.ps1'
```

Il default è il film indicato dall'utente, dall'inizio, in DLAA 1918x1080.
Sono disponibili `-VideoPath`, `-Quality`, `-OutputSize`. Il launcher salva i
log esistenti in `build\step06a4d-validation\sessions\<data-ora>` prima
dell'avvio, perché il player tronca il proprio log a ogni sessione.
Chiudere il player normalmente prima di ricompilare o avviare un altro test.

Per ricostruire il confronto principale:

```powershell
$root='C:\PROGETTO_DLSS\DLSS-Media-Processor'
& "$root\tools\Compare-PlaybackLogs.ps1" `
  -Before "$root\build\step06a4d-validation\exorcist-dlaa-A4C-repeat.log" `
  -After "$root\build\step06a4d-validation\exorcist-dlaa-A4D.log" `
  -WarmupSeconds 20 -DurationSeconds 100
```

Se si ripristina un SDK pulito, la patch si applica dalla radice SDK con
`git apply <percorso-completo-della-patch>`. Nella copia `external\NvOFSDK`
attuale è già applicata: non riapplicarla. Il test GPU verifica anche che
il contratto del cost buffer funzioni realmente.

## Artefatti e punto di ripresa

- Runtime: `build\step06a4d-player\runtime\DLSSVideoPlayer.exe`.
- SHA-256 EXE finale: `19A76144F515C3EAE57E59DC85701D853CD2777C91C9FCF67CC8827E88F7CD2F`.
- Release ufficiale Step05C14 ancora presente, hash verificato invariato:
  `54C268691DABD5C6E3F0B095B25CE095C7E5A531A00C8A117FC4BD8EE42A72F8`.
- Dati: `build\step06a4d-validation\exorcist-repeat-comparison.json`,
  `exorcist-comparison-100s.json`, `confidence-benchmark-final.txt`, log A4C/A4D.
- Test: `build\step06a4d-tests\Testing\Temporary\LastTest.log`.
- Copia sorgenti del checkpoint: `build\step06a4d-validation\step06a4d-source-checkpoint.zip`;
  hash di sorgenti, EXE e prove in `build\step06a4d-validation\manifest-final.json`.

Il prossimo obiettivo tecnico è abbassare l'intera pipeline sotto il budget
di 41,708 ms e ridurre gli scarti. Rimangono da profilare meglio trasferimenti
NVOF, stabilizzazione film, costruzione guide e attese di presentazione; i tempi
isolati delle chiamate che aspettano la GPU non identificano da soli la causa.
La riproduzione del crash Streamline durante reinizializzazione richiede un
test dedicato, conservando log e dump per ogni tentativo.
