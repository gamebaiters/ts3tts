"""Rotary position embedding used by the MeanVC2 DiT.

Plain-torch port of the two symbols MeanVC2 imports from x-transformers 2.2.11
(`RotaryEmbedding`, `apply_rotary_pos_emb`; MIT License, Phil Wang). Only the
code paths MeanVC2 uses are kept (no xpos, no interpolation, no base rescale),
so the backend does not need x-transformers and its einx/loguru dependencies.
Numerically identical: interleaved (d r) layout, freqs duplicated per pair.
"""

from __future__ import annotations

import torch
from torch import nn


class RotaryEmbedding(nn.Module):
    def __init__(self, dim: int, base: float = 10000.0):
        super().__init__()
        inv_freq = 1.0 / (base ** (torch.arange(0, dim, 2).float() / dim))
        self.register_buffer("inv_freq", inv_freq)

    def forward_from_seq_len(self, seq_len: int):
        return self.forward(torch.arange(seq_len, device=self.inv_freq.device))

    @torch.autocast("cuda", enabled=False)
    def forward(self, t: torch.Tensor):
        if t.ndim == 1:
            t = t.unsqueeze(0)                                            # n -> 1 n
        freqs = torch.einsum("b i , j -> b i j", t.type_as(self.inv_freq), self.inv_freq)
        freqs = torch.stack((freqs, freqs), dim=-1).flatten(-2)          # ... d r -> ... (d r)
        return freqs, 1.0


def rotate_half(x: torch.Tensor) -> torch.Tensor:
    x = x.unflatten(-1, (-1, 2))                                         # ... (d r) -> ... d r
    x1, x2 = x.unbind(dim=-1)
    return torch.stack((-x2, x1), dim=-1).flatten(-2)


@torch.autocast("cuda", enabled=False)
def apply_rotary_pos_emb(t: torch.Tensor, freqs: torch.Tensor, scale=1):
    rot_dim, seq_len, orig_dtype = freqs.shape[-1], t.shape[-2], t.dtype
    freqs = freqs[:, -seq_len:, :]
    scale = scale[:, -seq_len:, :] if isinstance(scale, torch.Tensor) else scale
    if t.ndim == 4 and freqs.ndim == 3:
        freqs = freqs.unsqueeze(1)                                       # b n d -> b 1 n d
    t, t_unrotated = t[..., :rot_dim], t[..., rot_dim:]
    t = (t * freqs.cos() * scale) + (rotate_half(t) * freqs.sin() * scale)
    return torch.cat((t, t_unrotated), dim=-1).type(orig_dtype)
