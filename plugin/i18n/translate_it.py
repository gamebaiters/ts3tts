"""Fill plugin/i18n/gbtts_it.ts with the Italian catalogue.

Workflow after any UI string change (same loop as the Soundboard):
    lupdate -no-obsolete -locations none src -ts i18n/gbtts_it.ts
    python i18n/translate_it.py
    (the build runs lrelease)

Keeping the table here means re-running lupdate never loses translations.
Unknown sources are reported and left unfinished.
"""

from __future__ import annotations

import sys
import xml.etree.ElementTree as ET
from pathlib import Path

IT = {
    # ---- QObject ----
    "GameBaiters TTS - show / hide (plugin)": "GameBaiters TTS - mostra / nascondi (plugin)",
    "off": "spento",
    "GameBaiters TTS - voice effects": "GameBaiters TTS - effetti voce",
    # ---- Controller ----
    "The voice engine is not installed yet.": "Il motore vocale non è ancora installato.",
    "Voice engine stopped.": "Motore vocale fermo.",
    "Cannot open the local connection for the voice engine.": "Impossibile aprire la connessione locale con il motore vocale.",
    "Cannot start the voice engine: %1": "Impossibile avviare il motore vocale: %1",
    "Starting the voice engine…": "Avvio del motore vocale…",
    "voice engine disconnected": "motore vocale disconnesso",
    "The voice engine stopped repeatedly (exit code %1). Open the log from Settings.":
        "Il motore vocale si è fermato più volte (codice %1). Apri il log dalle Impostazioni.",
    "Voice engine stopped (exit code %1), restarting…": "Motore vocale fermato (codice %1), riavvio…",
    "Loading the voice model…": "Caricamento del modello vocale…",
    "Could not speak the message: %1": "Impossibile pronunciare il messaggio: %1",
    "Message shortened to %1 characters.": "Messaggio accorciato a %1 caratteri.",
    "The voice engine is not installed: open Settings › Engine to install it.":
        "Il motore vocale non è installato: aprilo da Impostazioni › Installazione motore.",
    "The voice engine is not running: press Start.": "Il motore vocale non è in esecuzione: premi Avvia.",
    "The voice engine is still starting, try again in a moment.": "Il motore vocale si sta ancora avviando, riprova tra un attimo.",
    "Choose a voice first.": "Scegli prima una voce.",
    "Not connected to a server: only you will hear it.": "Non sei connesso a un server: lo sentirai solo tu.",
    "voice preview": "anteprima voce",
    "Restarting the voice engine to free the memory used by Qwen…":
        "Riavvio del motore vocale per liberare la memoria usata da Qwen…",
    "No effect": "Nessun effetto",
    "Radio host": "Speaker radiofonico",
    "Small room": "Stanza piccola",
    "Robot": "Robot",
    "Chipmunk": "Scoiattolo",
    "Demon": "Demone",
    "Telephone": "Telefono",
    "Military radio": "Radio militare",
    "Megaphone": "Megafono",
    "Cave": "Caverna",
    "Underwater": "Subacqueo",
    "Alien": "Alieno",
    "Helium": "Elio",
    "Giant": "Gigante",
    "Ghost": "Fantasma",
    "Old AM radio": "Vecchia radio AM",
    "8D voice": "Voce 8D",
    # ---- SettingsDialog ----
    "GameBaiters TTS - settings": "GameBaiters TTS - impostazioni",
    "Voice engine": "Motore vocale",
    "Qwen3-TTS — most natural, needs an NVIDIA GPU": "Qwen3-TTS — il più naturale, richiede una GPU NVIDIA",
    "Kokoro — light: CPU only, little RAM, no GPU": "Kokoro — leggero: solo CPU, poca RAM, nessuna GPU",
    "fp32 — fast on CPU (~325 MB, recommended)": "fp32 — veloce su CPU (~325 MB, consigliato)",
    "int8 — smaller download (~115 MB) but much slower": "int8 — download più piccolo (~115 MB) ma molto più lento",
    "Kokoro model": "Modello Kokoro",
    "Kokoro (light, CPU)": "Kokoro (leggero, CPU)",
    "Kokoro, light CPU": "Kokoro, CPU leggero",
    "Supertonic 3 — CPU only, no GPU needed": "Supertonic 3 — solo CPU, nessuna GPU necessaria",
    "Engine": "Motore",
    "1.7B — best quality (~4.7 GB of VRAM)": "1.7B — qualità migliore (~4,7 GB di VRAM)",
    "0.6B — lighter and faster (~2.7 GB of VRAM)": "0.6B — più leggero e veloce (~2,7 GB di VRAM)",
    "Qwen model": "Modello Qwen",
    "Streaming step": "Passo di streaming",
    "Read the whole sentence before speaking (more natural intonation)":
        "Leggi tutta la frase prima di parlare (intonazione più naturale)",
    "Expressiveness": "Espressività",
    "Supertonic quality steps": "Passi di qualità Supertonic",
    "never": "mai",
    " min": " min",
    "Free the GPU after inactivity": "Libera la GPU dopo inattività",
    "Even out the loudness of every voice": "Uniforma il volume di tutte le voci",
    "Free GPU memory now": "Libera ora la memoria GPU",
    "Engine installation": "Installazione motore",
    "Browse…": "Sfoglia…",
    "Folder": "Cartella",
    "Start the engine together with TeamSpeak": "Avvia il motore insieme a TeamSpeak",
    "Start / restart": "Avvia / riavvia",
    "Stop": "Ferma",
    "Open log": "Apri log",
    "Open folder": "Apri cartella",
    "Install / repair the engine…": "Installa / ripara il motore…",
    "Audio": "Audio",
    "Voice level before effects": "Livello voce prima degli effetti",
    " ms": " ms",
    "Audio buffered before a message starts. Raise it if the voice stutters while a game uses the GPU; lower it for faster replies.":
        "Audio accumulato prima che un messaggio parta. Alzalo se la voce scatta mentre un gioco usa la GPU; abbassalo per risposte più rapide.",
    "Start buffer": "Buffer di partenza",
    "Text": "Testo",
    "Read numbers, times, dates, money and units in words": "Leggi a parole numeri, orari, date, importi e unità di misura",
    "Expand chat abbreviations (cmq, xké, nn, tvb…)": "Espandi le abbreviazioni da chat (cmq, xké, nn, tvb…)",
    "Also write the message in the channel chat, prefix:": "Scrivi il messaggio anche nella chat del canale, prefisso:",
    "Pronunciation dictionary — how to say names, nicknames and words the voice gets wrong:":
        "Dizionario di pronuncia — come dire nomi, nickname e parole che la voce sbaglia:",
    "Written": "Scritto",
    "Say it as": "Pronuncia come",
    "Add": "Aggiungi",
    "Remove": "Rimuovi",
    "Interface": "Interfaccia",
    "Show the TTS button in the TeamSpeak toolbar": "Mostra il pulsante TTS nella barra di TeamSpeak",
    "Clear the box after sending": "Svuota la casella dopo l'invio",
    "Keep the TTS window on top": "Tieni la finestra TTS sempre in primo piano",
    "Automatic": "Automatica",
    "Language (after restarting TeamSpeak)": "Lingua (dopo il riavvio di TeamSpeak)",
    "Hotkeys: TeamSpeak › Tools › Options › Hotkeys › Add › Plugins › GameBaiters - TTS.\nChat: /tts <text> speaks, /tts stop stops.":
        "Tasti rapidi: TeamSpeak › Strumenti › Opzioni › Tasti rapidi › Aggiungi › Plugins › GameBaiters - TTS.\nChat: /tts <testo> parla, /tts stop ferma.",
    "Close": "Chiudi",
    "%1 ms of audio per step: lower starts sooner, higher is steadier":
        "%1 ms di audio per passo: più basso parte prima, più alto è più stabile",
    "Engine folder": "Cartella del motore",
    "No log yet: start the engine first.": "Nessun log ancora: avvia prima il motore.",
    "Found in %1 — %2": "Trovato in %1 — %2",
    "Not installed (%1). Press “Install / repair the engine…”.": "Non installato (%1). Premi “Installa / ripara il motore…”.",
    "The installer script was not found. Reinstall the plugin package.":
        "Lo script di installazione non è stato trovato. Reinstalla il pacchetto del plugin.",
    "Install the voice engine": "Installa il motore vocale",
    "This opens a PowerShell window that installs the voice engine into:\n\n%1\n\nIt downloads from the internet (Python packages, PyTorch, Hugging Face, GitHub):\n • Python libraries and PyTorch: about 4 GB\n • Voice models: Qwen3-TTS 1.7B + voice design (about 9 GB, only with an NVIDIA GPU) and Kokoro (about 350 MB)\n\nPython 3.12 must already be installed. Nothing else on the system is changed. Continue?":
        "Si apre una finestra PowerShell che installa il motore vocale in:\n\n%1\n\nScarica da internet (pacchetti Python, PyTorch, Hugging Face, GitHub):\n • librerie Python e PyTorch: circa 4 GB\n • modelli vocali: Qwen3-TTS 1.7B + progettazione voci (circa 9 GB, solo con GPU NVIDIA) e Kokoro (circa 350 MB)\n\nPython 3.12 deve essere già installato. Nient'altro nel sistema viene modificato. Continuare?",
    "Could not open PowerShell.": "Impossibile aprire PowerShell.",
    "Follow the PowerShell window. When it says it is done, press “Start / restart”.":
        "Segui la finestra di PowerShell. Quando dice che ha finito, premi “Avvia / riavvia”.",
    # ---- TtsWindow ----
    "GameBaiters - TTS": "GameBaiters - TTS",
    "Turn the voice engine on or off": "Accendi o spegni il motore vocale",
    "Voices": "Voci",
    "Create, clone and manage voices": "Crea, clona e gestisci le voci",
    "Settings": "Impostazioni",
    "Voice": "Voce",
    "Style (optional): cheerful, whispering, angry, excited…": "Stile (facoltativo): allegro, sussurrato, arrabbiato, entusiasta…",
    "How the sentence should be said. Qwen voices only; short descriptions work best.":
        "Come dire la frase. Solo voci Qwen; le descrizioni brevi funzionano meglio.",
    "Language": "Lingua",
    "Speed": "Velocità",
    "Output and effects": "Uscita ed effetti",
    "Hear it": "Ascolta",
    "Play the voice in your own headphones too": "Riproduci la voce anche nelle tue cuffie",
    "Custom": "Personalizzato",
    "Effects…": "Effetti…",
    "Open the full GameBaiters Soundboard effects chain for the voice":
        "Apri la catena effetti completa di GameBaiters Soundboard per la voce",
    "Channel": "Canale",
    "Me": "Io",
    "Pitch": "Tono",
    "Effect": "Effetto",
    "Mute my microphone": "Silenzia il mio microfono",
    "Lower my microphone": "Abbassa il mio microfono",
    "Keep my microphone": "Lascia il mio microfono",
    "What happens to your real microphone while the voice is speaking":
        "Cosa succede al tuo microfono reale mentre la voce parla",
    "Only for me": "Solo per me",
    "Preview: the voice plays only in your headphones, the channel hears nothing":
        "Anteprima: la voce suona solo nelle tue cuffie, il canale non sente niente",
    "Speak even when muted": "Parla anche col microfono mutato",
    "If your microphone is muted in TeamSpeak, lift the mute just for the voice (your real microphone stays silent) and put it back afterwards":
        "Se il microfono è mutato in TeamSpeak, toglie il muto solo per la voce (il microfono reale resta in silenzio) e lo rimette subito dopo",
    "While speaking": "Mentre parla",
    "Double-click to say it again, right-click for more": "Doppio clic per ripeterlo, clic destro per altre opzioni",
    "Type here and press Enter to speak…": "Scrivi qui e premi Invio per parlare…",
    "Speak": "Parla",
    "Voice engine not installed": "Motore vocale non installato",
    "Voice engine stopped": "Motore vocale fermo",
    "Starting…": "Avvio…",
    "Loading…": "Caricamento…",
    "Ready": "Pronto",
    "Working…": "Al lavoro…",
    "Error": "Errore",
    "VRAM %1/%2 GB": "VRAM %1/%2 GB",
    "replies in %1 ms": "risponde in %1 ms",
    "server connected": "server connesso",
    "not connected: only you hear it": "non connesso: senti solo tu",
    "Install…": "Installa…",
    "Start": "Avvia",
    "Your voices": "Le tue voci",
    "Qwen stock voices": "Voci integrate Qwen",
    "Supertonic (CPU)": "Supertonic (CPU)",
    "(no voices yet)": "(ancora nessuna voce)",
    "waiting": "in attesa",
    "speaking": "sta parlando",
    "%1 s": "%1 s",
    "stopped": "fermato",
    "error": "errore",
    "only me": "solo io",
    "Enter to speak · Shift+Enter new line · Esc stops · ↑ previous messages":
        "Invio per parlare · Maiusc+Invio a capo · Esc ferma · ↑ messaggi precedenti",
    "Say it again": "Ripeti",
    "Listen only myself": "Ascolta solo io",
    "Edit in the box": "Modifica nella casella",
    "Copy text": "Copia testo",
    # ---- VoiceDialog ----
    "GameBaiters TTS - voices": "GameBaiters TTS - voci",
    "Preview ready (%1 s). Like it? Give it a name and save it; otherwise change the description and generate again.":
        "Anteprima pronta (%1 s). Ti piace? Dalle un nome e salvala; altrimenti cambia la descrizione e rigenera.",
    "Voice “%1” created and selected.": "Voce “%1” creata e selezionata.",
    "Failed: %1": "Non riuscito: %1",
    "Available voices": "Voci disponibili",
    "Use": "Usa",
    "Listen": "Ascolta",
    "Rename": "Rinomina",
    "Delete voice": "Elimina voce",
    "Describe the voice you want: gender, age, timbre, tone, pace, accent. The model invents a matching voice; listen to it and save it if you like it.":
        "Descrivi la voce che vuoi: genere, età, timbro, tono, ritmo, accento. Il modello inventa una voce corrispondente; ascoltala e salvala se ti piace.",
    "e.g. Narrator": "es. Narratore",
    "Name": "Nome",
    "Examples…": "Esempi…",
    "Describe the voice…": "Descrivi la voce…",
    "Description": "Descrizione",
    "Sample sentence": "Frase di prova",
    "Generate preview": "Genera anteprima",
    "Save voice": "Salva voce",
    "The first preview loads the voice-design model (a few seconds, and ~4 GB the first time if it is not downloaded yet).":
        "La prima anteprima carica il modello di progettazione voci (qualche secondo, e ~4 GB la prima volta se non è ancora scaricato).",
    "Design from a description": "Progetta da una descrizione",
    "Use 5-15 seconds of clean speech: one voice, no music, no background noise. Writing exactly what is said makes the copy much more faithful.":
        "Usa 5-15 secondi di parlato pulito: una sola voce, niente musica, niente rumore di fondo. Scrivere esattamente cosa viene detto rende la copia molto più fedele.",
    "e.g. My voice": "es. La mia voce",
    "WAV, FLAC, OGG or MP3 file": "File WAV, FLAC, OGG o MP3",
    "Choose file": "Scegli file",
    "Recording": "Registrazione",
    "Exactly what is said in the recording (recommended)": "Esattamente cosa viene detto nella registrazione (consigliato)",
    "Transcript": "Trascrizione",
    "This is my own voice, or I have explicit permission to use it":
        "È la mia voce, oppure ho il permesso esplicito di usarla",
    "Create voice": "Crea voce",
    "Clone from a recording": "Clona da una registrazione",
    "Rename voice": "Rinomina voce",
    "New name": "Nuovo nome",
    "Delete “%1”? This cannot be undone.": "Eliminare “%1”? L'operazione non si può annullare.",
    "Choose a recording": "Scegli una registrazione",
    "Audio (*.wav *.flac *.ogg *.mp3)": "Audio (*.wav *.flac *.ogg *.mp3)",
    "your voice": "tua voce",
    "Supertonic, CPU": "Supertonic, CPU",
    "Qwen stock": "integrata Qwen",
    " · reference %1 s": " · riferimento %1 s",
    "Write a description and a sample sentence first.": "Scrivi prima una descrizione e una frase di prova.",
    "Generating the preview… (you will hear it when it is ready)": "Generazione dell'anteprima… (la sentirai appena è pronta)",
    "Voice %1": "Voce %1",
    "Saving the voice…": "Salvataggio della voce…",
    "Analysing the recording and creating the voice…": "Analisi della registrazione e creazione della voce…",
}

# ---- v1.2: one engine at a time, per-engine voices --------------------------------
IT.update({
    # Controller
    "Loading %1…": "Caricamento di %1…",
    "%1 cannot run on this PC (%2): using %3. You can change it in Settings.":
        "%1 non può funzionare su questo PC (%2): uso %3. Puoi cambiarlo nelle Impostazioni.",
    "The %1 voices are still loading, try again in a moment.":
        "Le voci di %1 si stanno ancora caricando, riprova tra un attimo.",
    "That voice belongs to another engine: choose a %1 voice.":
        "Quella voce appartiene a un altro motore: scegli una voce di %1.",
    "Creating voices needs the Qwen3-TTS engine (Settings › Voice engine).":
        "Per creare voci serve il motore Qwen3-TTS (Impostazioni › Motore vocale).",
    # SettingsDialog
    "Model": "Modello",
    "Create and manage voices…": "Crea e gestisci le voci…",
    "The first clause of every message starts right away, the rest follows. Speaking style and voice creation are Qwen3-TTS features.":
        "La prima frase di ogni messaggio parte subito, il resto segue. Stile di lettura e creazione di voci sono funzioni di Qwen3-TTS.",
    "More steps: cleaner sound, slower generation": "Più passi: suono più pulito, generazione più lenta",
    "Quality steps": "Passi di qualità",
    "Only the chosen engine is loaded; the TTS window and the voice library show only its voices. Switching frees the memory of the previous one.":
        "Viene caricato solo il motore scelto: la finestra TTS e la libreria voci mostrano solo le sue voci. "
        "Cambiando motore si libera la memoria del precedente.",
    "Every change is saved immediately.": "Ogni modifica viene salvata subito.",
    "Not available on this PC.": "Non disponibile su questo PC.",
    "Not available on this PC: %1": "Non disponibile su questo PC: %1",
    "Active — %1": "Attivo — %1",
    "Active": "Attivo",
    "%1 options": "Opzioni di %1",
    # TtsWindow
    "Voice library of the active engine": "Libreria voci del motore attivo",
    "Change engine…": "Cambia motore…",
    "The engine is chosen in Settings; only its voices are listed here":
        "Il motore si sceglie nelle Impostazioni; qui compaiono solo le sue voci",
    "Listen to this voice (only you hear it)": "Ascolta questa voce (la senti solo tu)",
    "Language of the text. Only the languages this engine speaks are listed.":
        "Lingua del testo. Sono elencate solo le lingue che questo motore sa parlare.",
    "How the sentence should be said. Short descriptions work best.":
        "Come va detta la frase. Funzionano meglio descrizioni brevi.",
    # VoiceDialog
    "Choose the engine in Settings…": "Scegli il motore nelle Impostazioni…",
    "Search by name or language…": "Cerca per nome o lingua…",
    "<h3>%1</h3><p>%2</p><p>This engine has a fixed set of built-in voices: pick one on the left and press <b>Use</b>, or <b>Listen</b> to hear a sample.</p><p>Designing a voice from a description or cloning one from a recording needs the <b>Qwen3-TTS</b> engine (NVIDIA GPU).</p>":
        "<h3>%1</h3><p>%2</p><p>Questo motore ha un insieme fisso di voci integrate: scegline una a sinistra e "
        "premi <b>Usa</b>, oppure <b>Ascolta</b> per sentirne un esempio.</p><p>Per progettare una voce da una "
        "descrizione o clonarla da una registrazione serve il motore <b>Qwen3-TTS</b> (GPU NVIDIA).</p>",
    "Loading the %1 voices…": "Caricamento delle voci di %1…",
    "in use": "in uso",
    # VoiceText
    "Automatic (Italian/English)": "Automatica (italiano/inglese)",
    "Multilingual": "Multilingue",
    "Your voices (clone / design)": "Le tue voci (clonate / progettate)",
    "Stock voices (non-Italian accent)": "Voci integrate (accento non italiano)",
    "Women": "Donne",
    "Men": "Uomini",
    "Other": "Altre",
    "light, CPU only": "leggero, solo CPU",
    "CPU only, many languages": "solo CPU, molte lingue",
    "most natural, NVIDIA GPU": "il più naturale, GPU NVIDIA",
    "Light and fast, runs on the processor: ideal while gaming, the GPU stays free. Native Italian voices Sara and Nicola, plus English, Spanish, French and Portuguese voices.":
        "Leggero e veloce, gira sul processore: ideale mentre giochi, la GPU resta libera. Voci italiane native "
        "Sara e Nicola, più voci inglesi, spagnole, francesi e portoghesi.",
    "Runs on the processor, 30 languages with ten generic voices. Useful as a fallback when neither Qwen nor Kokoro can run.":
        "Gira sul processore, 30 lingue con dieci voci generiche. Utile come riserva quando né Qwen né Kokoro "
        "possono funzionare.",
    "The most natural voice: sentence-level intonation, speaking style instructions, your own cloned or designed voices. Needs an NVIDIA GPU.":
        "La voce più naturale: intonazione sull'intera frase, istruzioni di stile, voci tue clonate o progettate. "
        "Richiede una GPU NVIDIA.",
    "~520 MB RAM · no VRAM · max 8 threads, 0% CPU when idle · first word in ~0.3 s":
        "~520 MB di RAM · niente VRAM · max 8 thread, 0% CPU a riposo · prima parola in ~0,3 s",
    "~500 MB RAM · no VRAM · slower than Kokoro": "~500 MB di RAM · niente VRAM · più lento di Kokoro",
    "4.7 GB VRAM (1.7B) or 2.7 GB (0.6B) + ~2.2 GB RAM · first word in ~0.3 s":
        "4,7 GB di VRAM (1.7B) o 2,7 GB (0.6B) + ~2,2 GB di RAM · prima parola in ~0,3 s",
    "(no voices)": "(nessuna voce)",
    "The first message downloads this model (a few GB).": "Il primo messaggio scarica questo modello (qualche GB).",
})

# ---- v1.3: real-time voice changer -----------------------------------------------------------
IT.update({
    # Controller
    "Choose the voice you want to sound like first.": "Scegli prima la voce che vuoi avere.",
    "The voice engine is not running: the voice changer starts with it.":
        "Il motore vocale non è in esecuzione: il cambia voce partirà insieme a lui.",
    # TtsWindow
    "Voice “%1” added: the voice changer now uses it.": "Voce “%1” aggiunta: il cambia voce ora usa questa.",
    "Live voice (AI)": "Voce live (AI)",
    "Change my voice while I talk": "Cambia la mia voce mentre parlo",
    "When you talk (push-to-talk or voice activation) an AI model on this PC turns your voice into the chosen one, in about a quarter of a second. While this is on, your real voice is never sent.":
        "Quando parli (push-to-talk o attivazione vocale) un modello AI su questo PC trasforma la tua voce in "
        "quella scelta, in circa un quarto di secondo. Finché è attivo la tua voce vera non viene mai inviata.",
    "The voice you will sound like: any voice of your library": "La voce che avrai: qualsiasi voce della tua libreria",
    "Add a voice from a recording (5-15 s of clean speech)": "Aggiungi una voce da una registrazione (5-15 s di parlato pulito)",
    "Hear myself": "Sentimi",
    "Reactive (~0.25 s, about 3 CPU cores)": "Reattivo (~0,25 s, circa 3 core CPU)",
    "Light (~0.35 s, about 1 CPU core)": "Leggero (~0,35 s, circa 1 core CPU)",
    "Play your converted voice in your headphones (it is delayed: can be distracting)":
        "Riproduce la tua voce trasformata in cuffia (arriva in ritardo: può distrarre)",
    "Sound like": "Voce",
    "Latency": "Latenza",
    "Reference recording: %1 s": "Registrazione di riferimento: %1 s",
    "(no voices yet: add a recording)": "(nessuna voce: aggiungi una registrazione)",
    "Waiting for the voice engine…": "In attesa del motore vocale…",
    "Off: your normal voice is sent.": "Spento: viene inviata la tua voce normale.",
    "(your normal voice is sent meanwhile)": "(nel frattempo viene inviata la tua voce normale)",
    "On: you sound like “%1” · delay about %2 ms": "Attivo: hai la voce di “%1” · ritardo circa %2 ms",
    "Use this voice": "Usa questa voce",
    "Use only your own voice, or a voice you have explicit permission to use. Do not impersonate real people. Continue?":
        "Usa solo la tua voce o una voce per cui hai il permesso esplicito. Non impersonare persone reali. Continuare?",
    "Analysing the recording…": "Analisi della registrazione…",
    # Controller: messages queued while the engine starts
    "waiting for the voice engine": "in attesa del motore vocale",
    "no voice of the active engine": "nessuna voce del motore attivo",
    "The voice engine is starting: the message will be spoken as soon as it is ready.":
        "Il motore vocale si sta avviando: il messaggio verrà letto appena è pronto.",
    # SettingsDialog: read own channel chat aloud
    "Read aloud the messages I write in the channel chat (private chats are never read)":
        "Leggi ad alta voce i messaggi che scrivo nella chat del canale (le chat private non vengono mai lette)",
    "Everything you type in the channel chat is also spoken by the TTS voice, so whoever cannot read the chat hears it.":
        "Tutto ciò che scrivi nella chat del canale viene anche pronunciato dalla voce TTS: chi non può leggere la chat lo sente.",
})

IT.update({
    # v1.5: read channel chat switch next to the text box
    "Read aloud what I write in the channel chat": "Leggi ad alta voce ciò che scrivo nella chat del canale",
    "Private chats, the server chat and other people's messages are never read.":
        "Le chat private, la chat del server e i messaggi degli altri non vengono mai letti.",
    # v1.5: voice engine code refreshed after a plugin update
    "Voice engine updated to version %1 together with the plugin.":
        "Motore vocale aggiornato alla versione %1 insieme al plugin.",
    "Could not update the voice engine (%1): open Settings and press “Install / repair the engine…”.":
        "Impossibile aggiornare il motore vocale (%1): apri le Impostazioni e premi “Installa / ripara il motore…”.",
    "This version needs updated voice engine libraries: open Settings and press “Install / repair the engine…”.":
        "Questa versione richiede librerie del motore vocale aggiornate: apri le Impostazioni e premi “Installa / ripara il motore…”.",
    "The voice engine (%1) and the plugin (%2) have different versions: if something does not work, open Settings and press “Install / repair the engine…”.":
        "Il motore vocale (%1) e il plugin (%2) hanno versioni diverse: se qualcosa non funziona, apri le Impostazioni e premi “Installa / ripara il motore…”.",
    # v1.5: updates in Settings
    "Check for updates automatically (at most once a day)": "Controlla automaticamente gli aggiornamenti (al massimo una volta al giorno)",
    "Check for updates now": "Controlla aggiornamenti ora",
    "Installed version %1": "Versione installata %1",
    # v1.5: Updater
    "Check for updates": "Controllo aggiornamenti",
    "Could not check for updates: %1": "Impossibile controllare gli aggiornamenti: %1",
    "The update information is not valid: %1": "Le informazioni sull'aggiornamento non sono valide: %1",
    "GameBaiters TTS is up to date (version %1).": "GameBaiters TTS è aggiornato (versione %1).",
    "New version of GameBaiters TTS": "Nuova versione di GameBaiters TTS",
    "GameBaiters TTS %1 is available (you have %2).": "È disponibile GameBaiters TTS %1 (hai la %2).",
    "Download and install it now? TeamSpeak closes to install the update; your settings, your voices and the voice engine are kept.":
        "Scaricarla e installarla adesso? TeamSpeak si chiude per installare l'aggiornamento; impostazioni, voci e motore vocale restano come sono.",
    "Update now": "Aggiorna ora",
    "Later": "Più tardi",
    "Update failed": "Aggiornamento non riuscito",
    "The update to %1 failed: %2\n\nYou can download it manually from:\n%3":
        "L'aggiornamento alla %1 non è riuscito: %2\n\nPuoi scaricarlo a mano da:\n%3",
    # v1.5: UpdaterWindow
    "GameBaiters TTS update": "Aggiornamento di GameBaiters TTS",
    "Updating GameBaiters TTS to %1": "Aggiornamento di GameBaiters TTS alla %1",
    "Preparing…": "Preparazione…",
    "Cancel": "Annulla",
    "Installed %1, installing %2": "Installata %1, installo la %2",
    "Downloading %1 from %2": "Scarico %1 da %2",
    "cannot write %1": "impossibile scrivere %1",
    "Connecting…": "Connessione…",
    "Downloading… %1% (%2 of %3)": "Download… %1% (%2 di %3)",
    "Downloading… %1": "Download… %1",
    "Download cancelled.": "Download annullato.",
    "Downloaded %1.": "Scaricati %1.",
    "the downloaded file is not a TeamSpeak plugin package": "il file scaricato non è un pacchetto plugin di TeamSpeak",
    "checksum mismatch: the download is damaged (SHA-256 %1)": "checksum diverso: il download è danneggiato (SHA-256 %1)",
    "Checksum verified (SHA-256).": "Checksum verificato (SHA-256).",
    "No checksum in the update information: only the archive format was verified.":
        "Nessun checksum nelle informazioni di aggiornamento: verificato solo il formato dell'archivio.",
    "cannot write the update helper %1": "impossibile scrivere lo script di aggiornamento %1",
    "Update helper: %1": "Script di aggiornamento: %1",
    "Ready (test run: the helper is not started).": "Pronto (prova: lo script non viene avviato).",
    "could not start the update helper": "impossibile avviare lo script di aggiornamento",
    "Installing: TeamSpeak closes now, confirm the plugin installer, then start TeamSpeak again.":
        "Installazione: TeamSpeak ora si chiude, conferma l'installazione del plugin e poi riapri TeamSpeak.",
    "Update helper started. TeamSpeak closes, the new version is installed, then start TeamSpeak again.":
        "Script di aggiornamento avviato. TeamSpeak si chiude, la nuova versione viene installata, poi riapri TeamSpeak.",
    "Update failed.": "Aggiornamento non riuscito.",
    "ERROR: %1": "ERRORE: %1",
    "Cancelling…": "Annullamento…",
})

IT.update({
    # v1.5: engine installation window (EngineSetupDialog, EngineInstaller, Controller)
    "%1 GB": "%1 GB",
    "%1 MB": "%1 MB",
    "Checking the system": "Controllo del sistema",
    "Installation tools": "Strumenti di installazione",
    "Python 3.12": "Python 3.12",
    "Engine environment": "Ambiente del motore",
    "PyTorch for NVIDIA graphics cards": "PyTorch per schede video NVIDIA",
    "PyTorch for the processor": "PyTorch per il processore",
    "Voice engine libraries": "Librerie del motore vocale",
    "Voice engine code": "Codice del motore vocale",
    "Verification": "Verifica",
    "Voice models": "Modelli vocali",
    "Finishing": "Completamento",
    "%1% — %2": "%1% — %2",
    "cancelling…": "annullamento…",
    "Installation cancelled.": "Installazione annullata.",
    "Installation failed: %1": "Installazione non riuscita: %1",
    "Voice engine installed: starting it…": "Motore vocale installato: lo avvio…",
    "The voice engine is still being installed: try again when it is ready.":
        "Il motore vocale si sta ancora installando: riprova quando è pronto.",
    "The voice engine is not installed: press “Install…” at the top of this window.":
        "Il motore vocale non è installato: premi “Installa…” in alto in questa finestra.",
    "an installation is already running": "un'installazione è già in corso",
    "the installer is missing from the plugin package: reinstall the plugin":
        "l'installer manca dal pacchetto del plugin: reinstalla il plugin",
    "cannot create %1": "impossibile creare %1",
    "could not start PowerShell (error %1)": "impossibile avviare PowerShell (errore %1)",
    "installation cancelled": "installazione annullata",
    "the installer stopped unexpectedly (exit code %1)": "l'installer si è fermato inaspettatamente (codice di uscita %1)",
    "Free some space on the disk or choose another folder, then press Retry.":
        "Libera spazio sul disco o scegli un'altra cartella, poi premi Riprova.",
    "Check the internet connection and press Retry: what is already downloaded is kept.":
        "Controlla la connessione a internet e premi Riprova: quello che è già scaricato non si perde.",
    "Update the NVIDIA driver (nvidia.com or the NVIDIA app), or install the light engine that runs on the processor.":
        "Aggiorna il driver NVIDIA (nvidia.com o l'app NVIDIA), oppure installa il motore leggero che usa il processore.",
    "No NVIDIA graphics card was found: install the light engine.":
        "Nessuna scheda video NVIDIA trovata: installa il motore leggero.",
    "Another installation is running: wait for it to finish.": "È in corso un'altra installazione: aspetta che finisca.",
    "The plugin package is incomplete: install the plugin again.": "Il pacchetto del plugin è incompleto: reinstalla il plugin.",
    "The installation was stopped from outside (antivirus, Task Manager, shutdown). Press Retry: it resumes.":
        "L'installazione è stata fermata dall'esterno (antivirus, Gestione attività, spegnimento). Premi Riprova: riprende.",
    "Press Retry whenever you want: it resumes where it stopped.":
        "Premi Riprova quando vuoi: riprende da dove si era fermata.",
    "Press Retry: the installation resumes where it stopped. If it fails again, open the log.":
        "Premi Riprova: l'installazione riprende da dove si era fermata. Se fallisce di nuovo, apri il log.",
    "GameBaiters TTS - voice engine installation": "GameBaiters TTS - installazione del motore vocale",
    "GameBaiters TTS speaks with a neural voice that runs on this PC. It is installed once and everything is automatic: Python, the libraries and the voice models are downloaded into one folder, nothing else on the PC is changed.":
        "GameBaiters TTS parla con una voce neurale che gira su questo PC. Si installa una volta sola ed è tutto automatico: "
        "Python, le librerie e i modelli vocali vengono scaricati in un'unica cartella, il resto del PC non viene toccato.",
    "Complete — Qwen3-TTS on the NVIDIA graphics card, plus Kokoro": "Completo — Qwen3-TTS sulla scheda video NVIDIA, più Kokoro",
    "The most natural voice, ten Italian voices ready to use, speaking styles, voice design and cloning. About 8 GB to download, about 10 GB on disk.":
        "La voce più naturale, dieci voci italiane pronte, stili di lettura, creazione e clonazione delle voci. "
        "Circa 8 GB da scaricare, circa 10 GB su disco.",
    "Light — Kokoro on the processor": "Leggero — Kokoro sul processore",
    "Good Italian voices and no video memory used: the graphics card stays free for games. About 1 GB to download, about 1.5 GB on disk. The complete engine can be added later.":
        "Buone voci italiane e nessuna memoria video occupata: la scheda video resta libera per i giochi. Circa 1 GB da "
        "scaricare, circa 1,5 GB su disco. Il motore completo si può aggiungere dopo.",
    "Change…": "Cambia…",
    "The installation continues in the background if you close this window or TeamSpeak; the TTS window shows how far it is.":
        "L'installazione continua in background anche se chiudi questa finestra o TeamSpeak; la finestra TTS mostra a che punto è.",
    "Install": "Installa",
    "Folder of the voice engine": "Cartella del motore vocale",
    "No NVIDIA graphics card found: the light engine is the one for this PC.":
        "Nessuna scheda video NVIDIA trovata: per questo PC va bene il motore leggero.",
    "Graphics card: %1 (%2 of video memory).": "Scheda video: %1 (%2 di memoria video).",
    "Choose a folder.": "Scegli una cartella.",
    "Free space: %1 (needed about %2).": "Spazio libero: %1 (ne servono circa %2).",
    "Not enough space: %1 free, about %2 needed.": "Spazio insufficiente: liberi %1, ne servono circa %2.",
    "An incomplete or broken installation is in this folder (%1): it will be repaired, the models already downloaded are kept.":
        "In questa cartella c'è un'installazione incompleta o danneggiata (%1): verrà riparata, i modelli già scaricati restano.",
    "Repair": "Ripara",
    "The engine is already installed here: installing again updates and repairs it.":
        "Il motore è già installato qui: installarlo di nuovo lo aggiorna e lo ripara.",
    "The installation could not start: %1": "Impossibile avviare l'installazione: %1",
    "Installing the voice engine": "Installazione del motore vocale",
    "Show details": "Mostra dettagli",
    "You can close this window: the installation continues.": "Puoi chiudere questa finestra: l'installazione continua.",
    "Hide": "Nascondi",
    "Hide details": "Nascondi dettagli",
    "Click again to cancel": "Clicca di nuovo per annullare",
    "Step %1 of %2 — %3": "Passo %1 di %2 — %3",
    "%1 of %2": "%1 di %2",
    "%1/s": "%1/s",
    "checking that the libraries load (up to a minute)": "controllo che le librerie si carichino (fino a un minuto)",
    "Elapsed %1:%2": "Tempo trascorso %1:%2",
    "Retry": "Riprova",
    "Open GameBaiters TTS": "Apri GameBaiters TTS",
    "The voice engine is installed": "Il motore vocale è installato",
    "Installed in %1 (%2 on disk).": "Installato in %1 (%2 su disco).",
    "The engine is starting now: the first start prepares the graphics card for about a minute, then the Italian voices are ready.":
        "Il motore si sta avviando: al primo avvio prepara la scheda video per circa un minuto, poi le voci italiane sono pronte.",
    "The engine is starting now with the Kokoro voices: it is ready in a few seconds.":
        "Il motore si sta avviando con le voci Kokoro: è pronto in pochi secondi.",
    "Installation cancelled": "Installazione annullata",
    "The installation did not finish": "L'installazione non è stata completata",
    "Step “%1”: %2": "Passo “%1”: %2",
    "Show progress": "Mostra avanzamento",
    # v1.5: Italian stock voices
    "Italian voices (included)": "Voci italiane (incluse)",
    "Qwen built-in voices (non-Italian accent)": "Voci integrate Qwen (accento non italiano)",
    # BackendProcess layout problems
    "the Python environment is broken: %1 no longer exists": "l'ambiente Python è danneggiato: %1 non esiste più",
    "Python environment missing in %1": "ambiente Python mancante in %1",
    "engine code (gbtts/server.py) missing in %1": "codice del motore (gbtts/server.py) mancante in %1",
    "voice engine not installed": "motore vocale non installato",
})

NUMERUS = {
    "%n in queue": ["%n in coda", "%n in coda"],
    "%n voice(s) of %1": ["%n voce di %1", "%n voci di %1"],
}


def main() -> int:
    ts = Path(__file__).with_name("gbtts_it.ts")
    tree = ET.parse(ts)
    missing = []
    for msg in tree.getroot().iter("message"):
        src = msg.findtext("source") or ""
        tr = msg.find("translation")
        if tr is None:
            continue
        if msg.get("numerus") == "yes":
            forms = NUMERUS.get(src)
            if not forms:
                missing.append(src)
                continue
            for child in list(tr):
                tr.remove(child)
            for f in forms:
                ET.SubElement(tr, "numerusform").text = f
            tr.attrib.pop("type", None)
            continue
        if src in IT:
            tr.text = IT[src]
            tr.attrib.pop("type", None)
        else:
            missing.append(src)
    ET.indent(tree, space="    ")
    with open(ts, "wb") as fh:
        fh.write(b'<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE TS>\n')
        tree.write(fh, encoding="utf-8", xml_declaration=False)
    if missing:
        print(f"{len(missing)} untranslated:")
        for m in missing:
            print("  -", m.replace("\n", "\\n"))
        return 1
    print("all strings translated")
    return 0


if __name__ == "__main__":
    sys.exit(main())
