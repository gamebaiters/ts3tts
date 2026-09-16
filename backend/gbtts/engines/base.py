"""Engine interface. Every call happens on the backend's single worker thread."""

from __future__ import annotations

import threading
from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from typing import Callable, Iterator

import numpy as np

StatusFn = Callable[[str, str], None]   # (state, human message)


@dataclass
class SynthRequest:
    text: str                      # one normalised segment
    lang: str                      # ISO code ("it", "en", ...)
    voice: str                     # engine-specific voice id (prefix stripped)
    speed: float = 1.0
    instruct: str = ""
    cancel: threading.Event = field(default_factory=threading.Event)


class EngineError(Exception):
    pass


class Engine(ABC):
    key: str = "base"
    label: str = "Engine"
    native_speed: bool = False     # True: engine applies speed itself (no WSOLA)

    @abstractmethod
    def available(self) -> tuple[bool, str]:
        """Can this engine run on this machine at all? (ok, reason)"""

    @abstractmethod
    def load(self, status: StatusFn) -> None: ...

    @abstractmethod
    def unload(self) -> None: ...

    @abstractmethod
    def is_loaded(self) -> bool: ...

    @abstractmethod
    def voices(self) -> list[dict]:
        """[{id, name, kind, lang, engine, description}] with ids WITHOUT prefix."""

    @abstractmethod
    def stream(self, req: SynthRequest) -> Iterator[tuple[np.ndarray, int]]:
        """Yield (float32 mono chunk, sample_rate) as soon as audio exists."""

    def info(self) -> dict:
        return {"engine": self.key, "label": self.label}
