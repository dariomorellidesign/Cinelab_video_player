# Step07C — moto lento della camera e tagli

Runtime isolato: `build/step07c-player/runtime/DLSSVideoPlayer.exe`.
Avvio: `tools/Start-Step07C.ps1`. Build: PowerShell 7,
`tools/Build-Step07C.ps1 -GpuTests`. Step07A, Step07B e Release restano
disponibili nei loro runtime precedenti; i sorgenti correnti sono Step07C.

## Correzione preliminare di Step07B

Step07B aggiornava `previousIndex` prima di esporre `previousColor`: i due
puntatori corrente/precedente coincidevano. Il suo test fotometrico confrontava
quindi il frame con se stesso. Il miglioramento riferito dall'utente rimane una
osservazione valida, ma non provava il confronto temporale descritto in STEP07B.
Ora i due input sono quelli usati da NVOF; un test hardware verifica identita
del precedente, alternanza e assenza di alias su tre coppie consecutive.

Separata inoltre la root signature MV da quella degli altri shader: il loro
table range torna a un solo SRV, evitando che una tabella da quattro iniziata
sull'ultimo descrittore colore oltrepassi l'heap. Il solo resolver usa quattro
texture piu un root SRV per il modello.

## Elaborazione

1. NVOF produce i vettori current-to-previous e cost come prima.
2. Un compute shader usa 256 punti distribuiti 16x16. I campioni con texture,
   cost accettabile e corrispondenza cromatica plausibile alimentano un fit di
   traslazione, scala e rotazione, con tre successive pesature robuste.
3. Il modello e valido con almeno 32 inlier, almeno 75% di consenso e almeno
   quattro inlier in ciascun quadrante. Non usa storia temporale.
4. Il resolver preserva il moto locale plausibile, anche inferiore a 0,18 px.
   In zone poco strutturate usa il moto della camera solo se il modello e valido
   e il confronto cromatico non lo contraddice. Altrimenti conserva la politica
   conservativa di zero. I vettori sono infine scalati ai pixel di input DLSS.

MotionResolveShader.h e condiviso dal player e dai test dei pixel: il test non
replica la formula su CPU. Il buffer modello contiene 32 byte e resta in VRAM.
Depth rimane costante, BiasCurrentColor zero; AI depth non viene avviata.

## Tagli e memoria

Il rilevatore di tagli campiona 32x18 punti, ognuno mediato su 3x3 pixel dal BGRA
gia presente in RAM. Conserva soltanto 576 luminanze. Una variazione ampia
dell'immagine e dell'istogramma genera un reset prima di NVOF: niente coppia
tra scene diverse, modello invalidato, MV zero e reset DLSS/FG nello stesso
frame. I normali frame scartati non invalidano la coppia presentata.

Questa e una scelta esplicita rispetto al primo piano interamente GPU: il reset
delle API NVOF/NGX/FG richiede una decisione host. Usare i pixel gia nel decoder
evita un readback sincrono o un reset ritardato di un frame. Con un futuro
decoder GPU il detector dovra essere riprogettato insieme al controllo reset.

Nessun download di immagini, MV, cost o modello nel player. Solo quattro
timestamp (32 byte) per frame sono scritti nella diagnostica readback, letti
occasionalmente dopo il fence gia previsto, senza una nuova attesa CPU. I log
`[GPU Motion Time]` distinguono cameraMs e filterExpandMs.

## Collaudo e limiti

La suite include coppie NVOF 1080p/4K, test della camera e dei pixel risultanti
1024x576/3840x2160, piu le regressioni precedenti. Pan da 0,125 px, zoom lento,
rotazione, movimento indipendente minoritario, supporto limitato a un quadrante,
nero uniforme e reset sono casi separati. Una zona nera con MV errati iniettati
deve acquisire il modello globale, oppure zero se non affidabile. Il test usa
il debug layer D3D12 quando disponibile e fallisce sui messaggi ERROR.

Il film Exorcist e stato riprodotto dall'inizio: circa 24 fps, log di reset sui
tagli rilevati e campioni GPU circa 0,008 ms per il fit, 0,04-0,05 ms per il
resolver/espansione a 1918x1080. Sono misure dei pass, non del throughput completo.
La qualita visiva su tutti i pan/zoom reali resta da valutare dall'utente.

Build finale: otto test passati, log in
`build/step07c-validation/tests-final.log`. Nei test pixel a 4K il fit costa
circa 0,010-0,012 ms e il resolver 0,13-0,15 ms GPU (il primo timestamp fit
del test pan e risultato zero, quindi non e usato nella stima). Queste sono
misure sintetiche isolate; il probe usa output RG32F per leggere i risultati,
mentre il player usa RG16F piu bias R8. Non equivalgono a un benchmark 4K60.

SHA256 runtime finale:
`CCE0C194611180903103748E1D9581776DC7A4B8C584DD6E3D62F3311AEDCDF7`.
Checkpoint sorgenti: `build/step07c-validation/source-checkpoint.zip`.

Il fit e un modello globale: parallasse, grandi soggetti che occupano tutti i
quadranti e oggetti uniformi con moto proprio restano ambigui. Il detector cut
e conservativo e puo perdere tagli con distribuzioni di luminosita simili;
flash o bruschi cambi di esposizione possono provocare reset. Nessuna promessa
di eliminare ogni artefatto. L'obiettivo completo 4K60 resta aperto: decode BGRA
CPU/pipe e catch-up sincrono non sono cambiati in questo step.

Modello consigliato per la prossima taratura circoscritta: GPT-5.6 Terra,
thinking Medio. Astra Medio per eventuali problemi di architettura o regressioni
non spiegate dai test.
