# GameBaiters TTS — guida rapida

Scrivi in una casella, premi **Invio**, e nel canale TeamSpeak parla una voce neurale che gira **sul tuo PC** (GPU NVIDIA). Niente microfono, niente cloud, nessun file audio salvato.

## 1. Installazione (una volta sola, tutto automatico)
1. Scarica `gb_tts_<versione>_win64.ts3_plugin` dall'[ultima release](https://github.com/gamebaiters/ts3tts/releases/latest), chiudi TeamSpeak e fai doppio clic sul file. L'interfaccia è in italiano (Impostazioni → Interfaccia per l'inglese).
2. Apri TeamSpeak: dopo pochi secondi si apre da sola la finestra **Installa il motore vocale** (oppure pulsante **Installa…** nella finestra TTS).
3. Scegli **Completo** (Qwen3-TTS sulla scheda video NVIDIA + Kokoro, circa 8 GB da scaricare) oppure **Leggero** (Kokoro sul processore, circa 1 GB) e premi **Installa**.
   Non serve installare niente prima, nemmeno Python: tutto finisce in `%LOCALAPPDATA%\GameBaitersTTS`, il resto del PC non viene toccato.
4. La barra mostra passo per passo cosa succede, quanto è scaricato e a che velocità. Puoi chiudere la finestra o anche TeamSpeak: l'installazione continua e la finestra TTS mostra la percentuale. **Annulla** va cliccato due volte; **Riprova** riprende da dove si era fermata.
5. Alla fine il motore parte da solo. Con il motore completo trovi subito **dieci voci italiane** nel gruppo *Voci italiane (incluse)*: Giulia, Marco, Sofia, Elena, Chiara, Rosa, Luca, Alessandro, Giorgio e Davide.

Se hai reinstallato Windows e il motore era in una cartella rimasta sul disco, la finestra lo riconosce come danneggiato e propone **Ripara** (i modelli già scaricati restano).

## 2. Uso
- Pulsante TTS nella barra di TeamSpeak (accanto a quello della Soundboard) oppure **Plugins → Open GameBaiters TTS**.
- Scrivi e premi **Invio**. **Maiusc+Invio** va a capo, **Esc** ferma, **↑/↓** richiama i messaggi già detti.
- Doppio clic su un messaggio della cronologia = ripetilo. Clic destro = "ascolta solo io", modifica, copia.
- Chat di TeamSpeak: `/tts ciao a tutti`, `/tts stop`, `/tts ripeti`.
- **Leggi ad alta voce ciò che scrivo nella chat del canale** (casella sotto il box di testo della finestra TTS): tutto quello che scrivi nella chat del **canale** viene anche detto dalla voce TTS, così chi non può leggere lo sente. Le chat private e quella del server non vengono mai lette; i messaggi degli altri nemmeno.
- Se premi Invio mentre il motore si sta ancora avviando, il messaggio non viene più rifiutato: va in coda ("in attesa del motore vocale") e viene detto appena il motore è pronto; il testo in chat col prefisso parte subito.
- Tasti rapidi: **Opzioni → Tasti rapidi → Aggiungi → Plugins → GameBaiters - TTS** (apri e scrivi, stop, ripeti, leggi appunti, solo per me).

## 3. Regolazioni
| Controllo | Cosa fa |
|---|---|
| Voce / Lingua | voce scelta; "Automatica" riconosce italiano/inglese |
| Velocità | 0,5×–2× senza cambiare il tono |
| Stile | es. "allegro", "sussurrato", "arrabbiato" (voci Qwen) |
| Canale / Io | volume per chi ti ascolta / nelle tue cuffie (+ "Ascolta") |
| Tono, Effetto, **Effetti…** | la stessa catena effetti della Soundboard (preset: Speaker radiofonico, Radio militare, Robot, Voce 8D…) |
| Mentre parla | cosa fare col tuo microfono vero: silenzia (default) / abbassa / lascia |
| Parla anche col microfono mutato | toglie il muto solo per la frase e lo rimette |
| Solo per me | anteprima: il canale non sente niente |

Funziona con push-to-talk e attivazione vocale: durante la frase il plugin trasmette da solo e poi ripristina esattamente la tua modalità.

## 4. Voci nuove (Voci…)
- **Progetta da una descrizione**: "voce maschile italiana sui quarant'anni, calda, ritmo lento…" → Genera anteprima → Salva.
- **Clona da una registrazione**: 5-15 s di parlato pulito + trascrizione. Solo la tua voce o con permesso esplicito.

## 5. Tre motori: si sceglie nelle Impostazioni
**Impostazioni → Motore vocale**: una scheda per motore con cosa offre, quanta memoria usa e se su questo PC può funzionare. Sotto compaiono solo le opzioni del motore scelto.
Viene caricato **un solo motore**: la finestra TTS e *Voci…* mostrano **solo le sue voci** (Kokoro raggruppate per lingua con l'italiano in cima, Supertonic per donne/uomini, Qwen le tue voci + quelle integrate). Ogni motore ricorda l'ultima voce che hai usato con lui. "Cambia motore…" nella finestra porta dritto alle Impostazioni.

| Motore | Voci italiane | Risorse | Velocità misurata | Quando usarlo |
|---|---|---|---|---|
| **Qwen3-TTS** (GPU) | Giulia, Marco + le tue voci clonate/progettate | 4,7 GB VRAM (1.7B) / 2,7 GB (0.6B) | risposta ~0,3 s, 1,4× tempo reale | voce più naturale, stile, clonazione |
| **Kokoro** (CPU) | Sara, Nicola | ~520 MB RAM, **0 VRAM**, max 8 thread e 0 CPU a riposo | risposta ~0,3 s, ~5,6× tempo reale (i5-13600K) | mentre giochi: GPU tutta al gioco |
| Supertonic (CPU) | voci M1–F5 | ~500 MB RAM, 0 VRAM | più lento di Kokoro | riserva |

Qwen invece occupa ~2,2 GB di RAM oltre alla VRAM: passando a Kokoro il motore si riavvia da solo appena è libero (avviso in basso) e restituisce tutta la memoria.

Kokoro: la prima proposizione (fino alla virgola) parte subito, il resto la segue. In *Impostazioni → Modello Kokoro* resta **fp32** (consigliato): l'int8 pesa meno da scaricare ma sulla CPU è ~7 volte più lento.

Se con Qwen un gioco satura la GPU e la voce scatta: *Buffer di partenza* più alto, modello **0.6B**, oppure passa a una voce Kokoro.

## 6. Voce live (AI): cambia la tua voce mentre parli
Nella finestra TTS, sezione **Voce live (AI)**:
1. scegli **Voce** (qualsiasi voce della tua libreria: Giulia, Marco, le tue clonate/progettate, oppure 📂 per aggiungerne una da una registrazione di 5-15 s — solo la tua voce o con permesso esplicito);
2. spunta **Cambia la mia voce mentre parli**. La prima volta scarica il modello (~1,9 GB) e analizza la voce: nel frattempo passa la tua voce normale.
3. parla come sempre (push-to-talk o attivazione vocale): nel canale arrivi con l'altra voce, **circa 0,3 s dopo**. Finché è attivo la tua voce vera non viene mai inviata; quando rilasci il tasto la coda della frase viene comunque trasmessa.

**Latenza**: *Reattivo* (~0,25 s, occupa ~3 core mentre parli) o *Leggero* (~0,35 s, ~1 core). In silenzio non consuma nulla. Gira sul processore: la GPU resta ai giochi e a Qwen. **Sentimi** ti fa ascoltare la voce trasformata (in ritardo, può distrarre).
Misurato su frasi italiane: il testo resta comprensibile quanto l'originale; il timbro diventa quello scelto. Con un microfono rumoroso il risultato può peggiorare.

## 7. Aggiornamenti automatici
Pochi secondi dopo l'avvio di TeamSpeak (al massimo una volta al giorno) il plugin controlla se c'è una versione nuova e **chiede** prima di fare qualsiasi cosa, mostrando le novità. **Aggiorna ora** scarica il pacchetto, ne verifica il checksum, chiude TeamSpeak in modo pulito e avvia l'installazione del plugin: conferma e riapri TeamSpeak. **Più tardi** richiede fra tre giorni.
Controllo manuale: **Plugins → GameBaiters - TTS → Check for TTS updates…** oppure *Impostazioni → Interfaccia → Controlla aggiornamenti ora* (lì si può anche disattivare il controllo automatico).
Il codice del motore vocale si aggiorna da solo al riavvio successivo; se una versione richiede librerie nuove, un avviso chiede di premere *Installa / ripara il motore…*.
Ogni modifica alle impostazioni viene salvata subito e, se TeamSpeak si chiude o crasha, il motore vocale si chiude con lui e restituisce subito RAM e VRAM.

## 8. Problemi
- **"Motore vocale non installato"** → pulsante **Installa…** nella finestra TTS (punto 1).
- **L'installazione non finisce** → la finestra dice a quale passo e perché (spazio, connessione, driver NVIDIA troppo vecchio…); **Riprova** riprende, **Apri log** mostra i dettagli (`%LOCALAPPDATA%\GameBaitersTTS\install\install.log`).
- **Si ferma / errore** → *Impostazioni → Apri log* (`logs\backend.log`).
- **Download bloccato da antivirus** (Kaspersky & co.) → già gestito (usa i certificati di Windows); premi Riprova, riprende da dove si era fermata.
- **Liberare la GPU** → *Impostazioni → Libera ora la memoria GPU* o "Libera la GPU dopo inattività".
- **Disinstallare tutto** → rimuovi il plugin da TeamSpeak, cancella `%LOCALAPPDATA%\GameBaitersTTS` e la chiave `HKCU\Software\GameBaiters\TTS`.
