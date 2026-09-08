# Step06A4E — sorgente depth unica; obiettivo GPU 4K60

Obiettivo concordato: 3840×2160 a 60 fotogrammi **sorgente** al secondo, poi misurare il margine. Budget nominale: 16,667 ms/frame. FG non conta come capacità di elaborare nuovi fotogrammi sorgente.

## Stato effettivo

Implementata e compilata la selezione depth comune; **la migrazione completa su GPU NON è ancora implementata**. Step06A4D e build/Release sono preservati. La guida e il diagramma Step06A4D descrivono la versione precedente e non questa modifica.

Nuovo runtime isolato: `build/step06a4e-player/runtime/DLSSVideoPlayer.exe`.
Avvio: `tools/Start-Step06A4E.ps1 -DepthSource flat|legacy|ai` (default Flat). Costruzione: PowerShell 7, `tools/Build-Step06A4E.ps1 -GpuTests`.

## Comportamento depth

- Legacy: proxy Legacy per maschera, DLSS e FG; nessuna inferenza AI né invio di immagini/movimento al worker AI.
- Flat: texture D32 costante 0,75 inizializzata una volta; niente proxy Legacy, niente AI, niente espansione per-frame Legacy. La maschera mantiene movimento/fotometria ma evita gradienti depth e pesi esponenziali depth. Il risultato di questo bypass è verificato uguale a quello della mappa costante.
- AI: un solo snapshot temporale immutabile alimenta la maschera e la texture D32 usata da DLSS e FG. La maschera ne usa il ricampionamento alla propria risoluzione; la polarità convenzionale è 1-nearness. Non viene stimato Legacy.
- AI non ancora disponibile o invalidata: fallback Flat esplicito, mai Legacy. Una mappa AI valida ma ritardata rimane AI; l'età è mostrata nella barra di stato. Questa è una limitazione qualitativa del worker CPU attuale, non una soluzione alla sua latenza.
- Cambio sorgente: reset storia guide/DLSS/FG, reset worker temporale e texture AI; uscita da AI arresta il sidecar. Salto/discontinuità e cambio scena rilevato invalidano la storia AI, compresi i risultati in volo del sidecar.
- Il vecchio toggle proxy Flat/Estimated ora passa dalla stessa selezione globale. `DMP_MASK_DEPTH` non può più sovrascrivere soltanto la maschera.
- Vista **Depth**: mostra la risorsa effettivamente selezionata anche durante PresentCurrent/pausa. AI Depth e HW Z rimangono viste diagnostiche del ramo AI, che non viene avviato semplicemente aprendo una vista.

Feedback: titolo e barra `Depth richiesta -> effettiva`; log `[Depth Signal]` min/max/media e `[Depth Contract]` sorgenti e puntatore alla risorsa D32. Le etichette DLSS/FG indicano gli ingressi predisposti: i rispettivi runtime li consumano soltanto quando attivi. Questo non prova un miglioramento visivo e non dimostra come l'addon NR usi internamente il dato.

## Verifica

Quattro CTest superati: equivalenza confidence, fallback Streamline, NVOF reale sulla GPU, contratto depth. L'ultimo verifica isolamento Legacy da AI, Flat costante, AI anche in presenza del vecchio override ambiente, rifiuto input invalidi, reset storia al cambio e equivalenza del bypass della maschera.

Prima prova manuale sul film: Flat costante 0,75 e nessun processo DMPAIDepthWorker; AI con intervallo 0..1 e risorsa diversa; Legacy con mappa visivamente diversa e arresto del sidecar. Tutte le viste usano il pulsante Depth. Nessuna misura comparabile di qualità finale o prestazioni 4K60 in questa prova.

La prima implementazione scartava AI oltre tre fotogrammi di ritardo. Il test ha evidenziato alternanze frequenti AI/Flat, quindi è stata corretta: tenere l'ultima AI valida con età visibile, invalidare su discontinuità. Log della prova iniziale conservato in `build/step06a4e-validation/depth-switch-initial-age-threshold.log`.

## Lavoro ancora necessario per GPU e margine 4K60

1. NVOF: mantenere output SHORT2 e cost R8 su GPU. Attualmente i due DownloadData introducono readback e attese separate. Il codice possiede già inputResources/outputResource/costResource D3D12; non serve passare dalla CPU per accedere ai risultati.
2. Portare confidence repair e FilmMotionStabilizer in compute shader, con test contro il riferimento CPU (il modello affine e la storia non possono essere semplicemente eliminati).
3. Portare analisi luma, scene-cut, proxy Legacy e maschera in compute. Riutilizzare direttamente le texture movimento/depth/mask come ingressi NGX/FG; eliminare costruzione RGBA32F CPU e upload guide.
4. Condividere il frame colore tra renderer e analisi NVOF; applicare contrasto/shadow filter in una texture GPU privata per non alterare il colore finale.
5. AI: preprocessing, inferenza e stabilizzazione residenti su GPU. L'attuale sidecar comunica via RAM e scarica l'output CUDA; richiede un contratto di texture condivise/fence e interoperabilità CUDA-D3D12 (o una diversa integrazione da validare). Flat/Legacy devono restare indipendenti da questo costo.
6. Decodifica hardware in superficie GPU e conversione colore GPU. FFmpeg attuale restituisce BGRA tramite pipe CPU; la fallback MF non espone ancora superfici GPU al player.
7. Misure GPU con timestamp per stadio e fence, contatori effettivi di byte upload/readback, benchmark ripetibile 1080p24 e 4K60 con DLSS/NR/FG separati. Verificare drop, tempi medi e percentili, VRAM e qualità su scene in movimento. La CPU conserva controllo, audio, UI e piccoli aggregati diagnostici; non matrici dense per-frame.

## Ordine di grandezza dei trasferimenti attuali

Conti teorici del payload, non misure PCIe, per vero input 3840×2160 e 60 fps (DLAA, senza riduzione decode):

| Passaggio | Byte/frame | GB/s decimali |
|---|---:|---:|
| Un upload BGRA | 33.177.600 | 1,99 |
| Due upload BGRA (renderer + NVOF) | 66.355.200 | 3,98 |
| Readback NVOF + cost, grid 2 | 10.368.000 | 0,622 |
| Upload guide RGBA32F, grid 2 | 33.177.600 | 1,99 |
| Readback NVOF + cost, grid 4 | 2.592.000 | 0,156 |
| Upload guide RGBA32F, grid 4 | 8.294.400 | 0,498 |

Auto NVOF può scegliere grid 4 a 4K; non va confuso con grid 2 forzato. Ulteriori copie IPC AI sono traffico RAM, non tutte attraversamenti PCIe. I sincronismi e il lavoro CPU possono essere limitanti prima della sola banda del bus. Impostare soltanto output 4K con il film 1080p non collauda una sorgente 4K60.
