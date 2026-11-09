# Where the port stands (2026-11-08, end of session)


**Update 2026-11-09:** movies hang at the first movie on the PC (cause known, `docs/PHASE_8.md`); a function-map hole at
`0x28a140` stops the game after ~4 minutes idle in the shop (found by the thermal run); saves wait for the user's play test;
studies in `docs/STUDY_VU1_GPU.md` and `docs/STUDY_BUILD_OPT.md`; plan in `docs/PLAN.md`. Handoff: `NOTES.md`.
Harry Potter and the Philosopher's Stone (PS2, NTSC SLUS-20826) on the Anbernic RG40XX H. Percentages are **estimates**;
what was measured is said so.

## Where we stopped (read this first)

**We were in the middle of movies** (the logos, the main menu's moving background and the other `.PSS` videos, black today).
The user stopped the session during the PC build. State, step by step:

| Step | State |
|---|---|
| 1. Minimal FFmpeg 7.1 built (MPEG-2 video decoder, its parser, swscale; static) | **Done** for PC and device: `private/third_party/ffmpeg-7.1-x86/`, `ffmpeg-7.1-aarch64/` (`scripts/ffmpeg_build.sh`; source in WSL `~/ps2port-ffmpeg`, signature checked against FFmpeg's release key; the user approved building it) |
| 2. Runner build uses it | **Done, not yet built**: `runner/build.sh` compiles `Kernel/Stubs/MPEG.cpp` with `PS2X_HAS_FFMPEG=1` and links the three libraries when they exist. `runner/host/glibc_compat.h` now skips assembly (FFmpeg's configure needed that) |
| 3. Movie stub removed | **Done**: the five movie functions (0x2dd8e0, 0x2dc868, 0x2dcc98, 0x2de1e8, 0x2dc738) restored from a fresh PS2Recomp run (`trial/out-m`, identical to `out-b` everywhere else sampled) into `trial/out-b` and the runtime's `src/runner`; `runner/gen.sh` no longer skips them (`PS2PORT_MOVIE_STUB=1` brings the stub back, then also run `scripts/ps2recomp_movie_stub.py` on those two folders); eegen's code regenerated (`sh runner/gen.sh` ran) |
| 4. PC build | **Interrupted** (the user stopped it): state unknown. Check no build is still running in WSL, then run it again: `sh runner/build.sh x86` |
| 5. PC test | Not done: a 60-80 s boot (`PS2PORT_RUNNER=private/third_party/build-m2/x86/runner sh scripts/ps2recomp_trial.sh boot 80 "CROSS@5+0.3"`); the WB and EA logos and the main menu's background should move. Screenshots are PPM files named `.png` in `trial/boot-*/frames` |
| 6. Device | Not done: `sh runner/build.sh aarch64`, strip to `runner-aarch64`, a short panel run; then the frame time in the menu and in play (decoding costs CPU) |

**Careful:** the device's `runner-aarch64` on disk is from before step 2, so play still works as before (no movies). The
next aarch64 build includes the movie changes, untested: test on the PC first.

## In one line

**About 60 % of the technical work is done (estimate); about 2-3 % of the game's content has been seen working on the
device** (boot to the first playable room, Ollivanders, of a game of roughly 8-10 hours). The engine (code, graphics,
controls) is far along; left are sound, movies, saves, speed and heat, then playing through to fix what later chapters break.

## By part

| Part | Done (estimate) | State |
|---|---|---|
| Game code (EE: 5,708 functions) | 95 % | All of it runs natively (eegen and PS2Recomp's code); checked byte for byte against PCSX2 on recorded frames |
| Vector units (VU0, VU1) | 95 % | Translated to native code; checked against recordings |
| Graphics (GS on GLES) | 75 % | Right in every scene seen (menus, cutscenes, Diagon Alley, Ollivanders); later scenes not seen |
| IOP, disc reading, streaming | 85 % | The game's own sound and stream drivers run on an emulated IOP on its own thread |
| Controls | 95 % | The device's pad through SDL2, PS2 layout by position; played by the user |
| Saves (memory card) | 50 % | The game finds and reads saves (measured); writing a save not checked yet |
| Sound | 35 % | First mixer: the intro's music and voice at sane levels on the device (measured); plays slow and low with the game; no reverb; not yet listened to by the user |
| Movies | 20 % | Decoder built and wired in, stub removed; not built into the runner yet, not seen (above) |
| Game speed | 50 % | About 73 % in play on the device. A fix makes game time follow real time (measured on the PC) but the first loading screen then never ends: off |
| Frame time | 50 % | 43.5 ms a frame in play (23 fps; the game wants 30): was 113 ms |
| Heat | 40 % | 85 C after about 13 minutes at 1.416 GHz; the session then stops (the user's choice) |
| Launching | 30 % | Needs a PC: `sh scripts/play.sh` over SSH. No menu entry on the device yet |

## What to do next, in order

1. **Finish movies** (steps 4-6 above), PC first, then the device.
2. **Sound: the user listens and says what is wrong** (`sh scripts/play.sh 15`; `PS2PORT_AUDIO=0` turns it off). Then gaps
   while loading, Gaussian interpolation, reverb. `PS2PORT_SPU_DEBUG=1`, `PS2PORT_SPU_PCM=FILE`. Code:
   `ps2xIOP/src/emulator/core/iop_memory.cpp` (private IOP patch), `runner/audio_sdl.cpp`. Plan: `docs/PLAN_AUDIO.md`.
3. **Game speed to 100 %.** `PS2PORT_REALTIME=1` (off by default) breaks the first load: find what the loader waits for, or
   give the game real elapsed time only where it steps its world (the frame end, 0x2e2268). Test on the PC with
   `PS2PORT_SLOWDOWN=70`; `docs/PHASE_8.md`, "Game speed".
4. **Saves.** Find where the game offers to save (not Start in Ollivanders), save, then Continue. `PS2PORT_MC_LOG=1` (on in
   `play.sh`) logs every memory card command.
5. **Play through the game,** chapter by chapter: loads, looks right, sounds right, no crash. The user's play sessions are the
   main way; saves let a session start where the last one stopped.
6. **Frame time and heat.** 43.5 to 33 ms a frame: the GS thread's stage A is the limit (VU1's code about 22 ms of it).
7. **Start from the device:** a Knulli "Ports" entry (`/userdata/roms/ports/`), no PC needed.

## Testing on the PC (the user's choice: PC first, then the device)

The PC runner (x86 in WSL, no SDL2 there) uses the software renderer. Good for logic, loading, saves, sound data and movies;
**not for timing**: it is far slower than the device in 3D scenes (about 900 ms a frame in the Hagrid street; the device 45 ms).

## How to pick up

Read `NOTES.md`, "Current state and handoff", then `docs/PHASE_8.md` (saves, sound, game speed, PC testing, movies) and
`docs/PHASE_7.md` (frame time, the out-of-memory fix, controls). Device rules: `CLAUDE.md` (frame table, 85 C stop, no screen
control, no pushes). Play: `sh scripts/play.sh 15` from Git Bash on the PC; quit on the device with Menu + Start.
