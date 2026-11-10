# Harry Potter and the Philosopher's Stone (PS2) on the Anbernic RG40XX H

A native port of the PlayStation 2 game to a small ARM handheld, made by static recompilation: the game's MIPS R5900 code is
translated to C++ ahead of time and built together with a runtime that stands in for the console. There is no emulator CPU
core in the loop.

- **Game:** NTSC-U `SLUS-20826` (primary), PAL `SLES-52055` (secondary).
- **Target:** Anbernic RG40XX H running Knulli: Allwinner H700, 4 x Cortex-A53 at 1.416 GHz, Mali-G31 MP2, 1 GB RAM, 640x480.
- **State:** work in progress. The game boots, plays its opening, loads and saves, and the first playable room runs on the
  device at about 15-18 frames a second. See [docs/PROGRESS.md](docs/PROGRESS.md).

## You need your own copy of the game

This repository contains **no game files**: no disc image, executable, assets, BIOS, save states, recordings or generated code.
All of that is produced on your machine from your own disc and stays in `private/`, which git ignores. The project does not
download game files and will not tell you where to find them.

## How it works

| Part | What it does |
|---|---|
| `eegen/` | Our R5900 to C++ generator (`eegen.py`): own decoder, control flow and register dataflow. `vu0gen.py` and `vu1gen.py` translate the vector unit microprograms the game uses. |
| Runtime | A fork of [PS2Recomp](https://github.com/ran-j/PS2Recomp)'s runtime (kernel calls, memory, DMA, VIF/GIF, an emulated IOP for the game's own sound and disc drivers). It is patched heavily; the checkout and our patch live in `private/third_party/`. |
| `gsgles/` | The Graphics Synthesizer on OpenGL ES 3 for the Mali-G31, plus `gsplay`, a player for GS dumps used to compare pictures and measure GPU cost. |
| `runner/` | The whole game: runtime, generated code, display, pad (SDL2), audio. Builds for x86 (PC, software renderer, for logic tests) and aarch64 (the device). |
| `harness/`, `vu1/` | Replay harnesses that check generated code byte for byte against recordings made with PCSX2. |
| `scripts/` | ELF analysis, device runs under a time and temperature guard, profiling, screenshot and dump tools. |
| `bench/` | Device microbenchmarks from the feasibility phase. |
| `docs/` | Phase reports with every measurement (`PHASE_0.md` to `PHASE_8.md`), the plan, provenance, the visual checklist. |

Threads on the device: the game (EE), two graphics stages (VIF1/VU1/GIF, then GS parsing), the display (GL), the IOP, audio.

## Building (outline)

The build is developed on Windows with WSL (Ubuntu) and has several manual first steps; it is not yet a one-command setup.

1. Extract your disc into `private/extract/SLUS-20826/files/` (the ELF is `SLUS_208.26`).
2. `sh scripts/ps2recomp_trial.sh setup`, then `b` and `runtime b`: fetches PS2Recomp at the pinned commit, applies our
   patches from `private/third_party/trial/` and runs it once (its output supplies the native stubs and a few instruction
   bodies eegen borrows).
3. `sh runner/gen.sh`: eegen and the vector unit generators write the game's code to `private/third_party/build-m2/gen/`.
4. `sh runner/build.sh x86` or `sh runner/build.sh aarch64` (cross compiler `aarch64-linux-gnu-g++`).
5. PC test: `sh runner/try.sh SECONDS PAD`. Device: `sh scripts/play.sh` from Git Bash (copies the binary over SSH and starts
   the game on the panel; quit with Menu + Start, screenshot with Menu + L1).

The patches to PS2Recomp are not in this repository yet (they sit next to the checkout in `private/`), so a fresh clone cannot
complete step 2 today. Publishing them is on the list.

`NOTES.md` is the working log: decisions, open questions and the commands that worked. `CLAUDE.md` holds the project rules.

## Licence

GPL-3.0, as PS2Recomp, whose runtime this port forks. Which of our files derive from PS2Recomp is listed in
[docs/PROVENANCE.md](docs/PROVENANCE.md). A `LICENSE` file with the GPL text still has to be added.

Harry Potter and all related names are trademarks of their owners. This is an unofficial fan project, not affiliated with or
endorsed by Warner Bros., Electronic Arts or Sony.
