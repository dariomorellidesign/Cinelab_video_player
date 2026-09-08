# Step07A — MV grezzi residenti su GPU

## Direzione richiesta e stato

Su richiesta dell'utente, il percorso normale elimina calcolo depth e maschera e usa direttamente NVOF. Obiettivo successivo invariato: sorgente 4K60 con margine. **Il 4K60 completo non è raggiunto**: il test sintetico ha evidenziato il limite della decodifica/conversione BGRA via pipe CPU e del recupero sincrono dei frame.

Step06A4E (selezione depth unica) è preservato come runtime e checkpoint sorgenti in `build/step06a4e-validation/source-checkpoint.zip`. La documentazione Step06A4D/E non descrive il nuovo percorso normale.

## Percorso effettivo

```
Video compresso su disco
  -> FFmpeg: decodifica e BGRA in RAM (ancora CPU/pipe)
  -> UN upload BGRA nella texture NVOF
  -> NVOF: coppia corrente / precedente elaborata, output SHORT2 in VRAM
  -> shader GPU: S10.5 / 32, ricampionamento bilineare e scala input DLSS
  -> MV RG16F + maschera R8 zero
  -> DLSS/NR e FG quando abilitati: MV GPU, depth D32 costante 0,75
  -> presentazione
```

Lo stesso input BGRA GPU è campionato dal renderer per la conversione colore lineare; niente secondo upload del colore e niente copia GPU-GPU per condividere l'input. La conversione in colore lineare e la conversione dei vettori producono necessariamente le texture nei formati richiesti.

Eliminati dal percorso normale:

- download MV e cost sulla CPU;
- conversione MV CPU, confidence repair e FilmMotionStabilizer;
- analisi luma/flow di TemporalGuides, stima depth Legacy/AI, stabilizzazione AI e maschera CPU;
- costruzione e upload delle guide RGBA32F;
- avvio del sidecar AI;
- soglia debug MV di 0,20 pixel: nel percorso GPU solo il vettore esattamente nullo è nero; il mapping colore resta non lineare e la vista non è una segmentazione di oggetti.

L'SDK calcola ancora il cost buffer in VRAM, che non viene scaricato né usato. Alcune risorse del percorso di riferimento sono ancora allocate all'inizializzazione; non c'è il loro costo per-frame nel percorso GPU. L'SDK UploadData contiene tuttora un'attesa CPU sul fence per riutilizzare il proprio staging/allocator. Il renderer aspetta il completamento NVOF sulla propria coda GPU e ripristina le risorse condivise in COMMON; prima di riutilizzarle l'engine aspetta il fence del renderer. Prima di distruggere/reinizializzare l'engine, il player completa i comandi del renderer.

Il puntatore depth è costante e comune a DLSS/FG; maschera zero. I filtri contrasto/ombre e la selezione depth sono disabilitati nella UI GPU. Le viste diagnostiche depth/maschera possono mostrare i loro valori costanti; AI non viene avviata per mostrarne la vista.

## Associazione frame-MV

NVOF è eseguito soltanto per i frame inviati al renderer. Un ordinario recupero di frame saltati conserva come riferimento l'ultimo frame elaborato, non l'ultimo decodificato e scartato. Il log `[MV Pair]` mostra i due timestamp, il delta e l'accettazione del renderer. Seek/discontinuità azzerano la coppia; il primo frame ha MV zero. FG può aggiungere frame sintetici: non costituiscono ulteriori frame sorgente calcolati da questa pipeline.

## Avvio e confronto

- `tools/Start-Step07A.ps1` avvia GPU raw sul film autorizzato, DLAA e output 1918×1080.
- `tools/Start-Step07A.ps1 -MotionPipeline reference` usa il percorso CPU filtrato con depth Flat. La maschera del riferimento resta quella precedente: questo confronto non isola da solo l'effetto dei filtri MV sul video finale. La vista MV permette di confrontare i campi; qualità finale richiede prova controllata sullo stesso segmento.
- Il titolo indica `GPU MV RAW` oppure `CPU MV FILTERED`.
- `tools/Build-Step07A.ps1 -GpuTests` richiede PowerShell 7 e crea build/runtime isolati.

Runtime: `build/step07a-player/runtime/DLSSVideoPlayer.exe`. Release originale invariata.

## Evidenza e limiti

Sei test passati: equivalenza confidence del riferimento, fallback Streamline, NVOF CPU di riferimento, contratto depth del riferimento, GPU MV 1080p e GPU MV 4K. I test GPU verificano tre traslazioni consecutive di 8 pixel, segno/scalatura e reset; a 3840×2160 le tre mediane sono esattamente (-8,0). Il readback usato esclusivamente nei test consente la misura; il player GPU non lo effettua.

Prova film 1080p: riproduzione osservata circa 23,98/24 fps, campioni del timer frame circa 4–6 ms nel tratto iniziale dopo l'avvio, senza crescita continua dei drop in quei tratti. La sessione contiene interazioni, seek e una generazione di clip di test: non è un benchmark A/B controllato. `frameMs` è tempo CPU di RenderVideoFrame, non GPU timestamp né tempo totale della decodifica. Non si deve ricavare un margine 4K dal suo inverso.

Non è provato che il difetto “solo contorni” sia interamente causato dai filtri: il codice confidence sopprime/sostituisce stime poco strutturate e la vecchia vista nascondeva i vettori minimi. Il nuovo percorso elimina entrambe le alterazioni del dato visualizzato, ma l'optical flow grezzo può stimare male aree senza texture, grana, occlusioni e cambi scena. Non è stata dimostrata l'assenza di ghosting/smearing né la superiorità visiva di raw.

Clip sintetica generata: `build/step07a-validation/test-pattern-4k60.mp4`, 3840×2160, 60/1 fps, 2700 frame, 45 s, H.264 NVENC, circa 117 Mb/s. È un carico sintetico, non un film 4K nativo. Test DLAA 3840×2160 e grid AUTO=4: fallito come riproduzione realtime. Il loop sincrono di catch-up legge e scarta centinaia di frame sul thread UI; lunghi blocchi e quasi tutti i frame scartati. Il file `4k60-initial-catchup-failure.log` conserva il fallimento, senza usarlo come benchmark valido dei soli shader.

Diagnostica separata: FFmpeg con decode 4 thread, filtro 4 thread e raw encoder 1 thread, output BGRA a NUL, 120 frame in 2,591 s (~46 fps includendo avvio). Con un consumer Python via pipe la stessa quantità richiede più tempo (14,6–16,3 fps in `decode-isolation.json`); il buffer pipe Python differisce da quello C++ del player, quindi questi numeri non sono misure equivalenti del decoder del player. Indicano che il percorso BGRA via RAM/pipe non è un buon fondamento per 4K60.

## Prossimo intervento necessario

Decodifica hardware e conversione colore su superfici GPU condivise con NVOF/renderer; gestione asincrona dei frame e lifetime/fence espliciti. Rimuovere il loop di recupero bloccante dal thread UI, con associazione stabile tra frame presentato e coppia MV. Misurare GPU time e latency completa, poi qualità/FG sulle persone in movimento e un vero film 4K HFR. Non introdurre GPU->RAM->GPU per accelerare soltanto il decoder: ricreerebbe il problema appena eliminato dai vettori.
