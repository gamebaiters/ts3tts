"""GameBaiters TTS backend.

Local neural text-to-speech server for the GameBaiters TTS TeamSpeak 3 plugin.
The plugin owns a loopback TCP listener; this process connects to it, loads the
selected engine once (kept warm in VRAM) and streams 48 kHz mono int16 PCM back
while the model is still generating. Nothing is ever written to disk except the
small voice prompts the user explicitly creates.
"""

__version__ = "1.0.0"
PROTOCOL_VERSION = 1
