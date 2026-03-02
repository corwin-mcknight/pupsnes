# PupSNES 🐾

PupSNES is a Super Nintendo Entertainment System emulator focused on correctness, determinism, and a high-quality player experience. The goal is to emulate SNES hardware as faithfully as possible while being explicit about tradeoffs, avoiding hidden hacks, and building a foundation that supports modern features like rewind, debugging tools, and enhancements without compromising accuracy. PupSNES aims to be an emulator people can trust — both to play games and to understand how the SNES actually works.


## What language is the emulator written in?
Modern C++ (C++20).

## Why another SNES emulator?
To make the most accurate and feature-rich emulator.

## Architecture
* Hardware modeled as independent chips connected by explicit buses and signals.
* Cycle-accurate simulation with deterministic execution.

See `docs/architecture.md` for the scheduling model and timing details.
