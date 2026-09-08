# Step07D — disocclusione GPU per DLSS

Runtime isolato: `build/step07d-player/runtime/DLSSVideoPlayer.exe`.
Avvio: `tools/Start-Step07D.ps1`. Build: `tools/Build-Step07D.ps1 -GpuTests`.

Step07C produceva MV locali e modello camera in VRAM, ma scriveva sempre zero
in `BiasCurrentColor`. Un MV descrive dove si trova il campione nel frame
precedente; non certifica che tale contenuto sia ancora visibile. In particolare
ai bordi di un oggetto che entra, esce o scopre uno sfondo, il campione
riproiettato puo appartenere all'altro lato del bordo.

Lo stesso pixel shader che espande e risolve gli MV ora scrive anche una
maschera R8. Usa soltanto texture gia residenti: luminanza corrente, precedente
riproiettata con l'MV, discontinuita locale del flow, struttura luminanza e costo
NVOF. Bianco significa che DLSS deve preferire il colore corrente. La risorsa e
gia collegata in `DLSSBackend` sia come `BiasCurrentColor` sia, quando presenti
nelle intestazioni NGX, come hint di disocclusione e responsivita.

Non introduce readback, RAM intermedia, storia CPU o un secondo pass GPU. La
maschera e intenzionalmente conservativa: il rumore di pellicola privo di moto o
di un confine del flow rimane nero. La vista **T-Mask** consente di verificarla:
deve illuminare bordi in movimento e contenuto non corrispondente, senza coprire
intere aree statiche.

Collaudo richiesto: stesso tratto con oggetto su fondo scuro in DLAA, poi in
Ultra Performance; controllare Input, MV, T-Mask e output finale. La taratura
dei quattro threshold shader dipendera da quel riscontro visivo. Il costo del
pass resta incluso nel timestamp `filterExpandMs`.
