# Step07B — rigetto GPU dei falsi vettori statici

Step07B mantiene il percorso Step07A residente in VRAM e aggiunge un controllo
locale nel pixel shader che espande il campo NVOF. Non esistono download di
motion vector, cost o frame video verso la RAM.

Per ogni pixel lo shader riceve, nella stessa tabella di descrittori D3D12:

- il vettore NVOF `SHORT2`;
- il relativo cost `R8_UINT` (un valore maggiore indica minore affidabilita);
- il frame BGRA corrente e quello precedente.

Confronta l'errore di luminanza per moto zero con l'errore dopo il warp del
frame precedente tramite il vettore. Un vantaggio chiaro del warp mantiene il
vettore. Se non c'e vantaggio, il vettore sopravvive solo quando ha modulo
significativo, cost basso e una prova locale: contrasto oppure coerenza con i
quattro vettori NVOF adiacenti. Questo evita che una zona nera uniforme riceva
un moto soltanto per una stima debole, senza introdurre una regola generica
"basso contrasto = fermo" che cancellerebbe gli interni degli oggetti.

Le soglie iniziali sono volutamente conservative e servono da punto di prova:
vantaggio fotometrico `0.012`, modulo `0.18 px`, cost normalizzato `< 0.25`,
contrasto locale `0.010`, coerenza entro `0.20 px`. Non sono ancora tarate sul
film dell'utente.

## Memoria e sincronizzazione

Il renderer prende in prestito le due texture input NVOF, output e cost; attende
il fence NVOF sulla sua coda GPU, legge le quattro risorse come SRV, poi le
riporta a `COMMON`. Il fence del renderer continua a proteggere il riuso dello
staging NVOF. Sono stati assegnati quattro descrittori per ciascuno dei tre slot
frame, per non sovrascrivere descrittori ancora in uso dalla GPU.

## Verifica

`tools/Build-Step07A.ps1 -GpuTests -StepName step07b` ha completato build e sei
test: confidence di riferimento, fallback Streamline, NVOF CPU, contratto
depth, GPU MV 1080p e GPU MV 4K.

Il runtime isolato e `build/step07b-player/runtime/DLSSVideoPlayer.exe`.
Il launcher supporta il runtime scelto: `tools/Start-Step07A.ps1 -StepName step07b`.
Con la clip locale `build/step07a-validation/test-pattern-4k60.mp4`, il log ha
confermato inizializzazione NVOF, `RAW NGX EvaluateFeature_C SUCCESS` e una
coppia GPU `pair=1`; non sono comparsi errori di compilazione shader o stati
D3D12. Il controllo visivo sul film Exorcist resta da fare quando l'unita `X:`
sara disponibile al processo desktop.
