miniaudio is pinned to **0.11.25** (2026-03-04), from
[mackron/miniaudio](https://github.com/mackron/miniaudio/tree/0.11.25).

`miniaudio.h` is unmodified. Its SHA-256 is
`ac7af4de748b7e26b777f37e01cee313a308a7296a3eb080e2906b320cc55c89`.
The upstream dual public-domain/MIT-0 license is included as `LICENSE`.

`pupsnes_miniaudio.h` disables unused high-level APIs, file codecs, generation,
and the silent fallback device. `miniaudio_impl.cpp` supplies its single
implementation translation unit. PupSNES opens playback devices only.
