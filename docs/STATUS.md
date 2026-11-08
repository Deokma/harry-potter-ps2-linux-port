# Where the port stands (2026-11-08)

Harry Potter and the Philosopher's Stone (PS2, NTSC SLUS-20826) on the Anbernic RG40XX H. A short answer first, then the
detail. Percentages are **estimates** (marked so); what was measured on the device is said so.

## In one line

**About 60 % of the technical work is done (estimate); about 2-3 % of the game's content has been seen working on the
device** (boot to the first playable room, Ollivanders, of a game of roughly 8-10 hours).

The engine work is the large part and is far along: the whole game's code runs as native ARM code, the graphics run on the
device's GPU, the controls work. What is left is mostly making the rest of the game right (sound, movies, saves, speed,
heat) and then playing through it to find and fix what breaks in later chapters.

## By part

| Part | Done (estimate) | State |
|---|---|---|
| Game code (EE: 5,708 functions) | 95 % | All of it runs natively (eegen and PS2Recomp's code); checked byte for byte against PCSX2 on recorded frames |
| Vector units (VU0, VU1) | 95 % | Translated to native code; checked against recordings |
| Graphics (GS on GLES) | 75 % | Right in every scene seen (menus, cutscenes, Diagon Alley, Ollivanders); later scenes not seen |
| IOP, disc reading, streaming | 85 % | The game's own sound and stream drivers run on an emulated IOP on its own thread |
| Controls | 95 % | The device's pad through SDL2, PS2 layout by position; played by the user |
| Saves (memory card) | 50 % | The game finds and reads saves (measured); writing a save not checked yet |
| **Sound** | 35 % | First mixer (ADPCM, pitch, envelopes, volumes, SDL2/ALSA output): the intro's music and voice mix at sane levels on the device (measured: RMS -18 to -24 dBFS, no clipping). Plays slow and low with the game (about 65-75 % speed); gaps while loading; no reverb; not yet listened to by the user |
| **Movies (FMV)** | 0 % | Stubbed: the logos, the main menu's moving background (`FRONTEND.PSS`) and other `.PSS` movies show black. Needs an MPEG-2 (IPU) decoder |
| Speed | 50 % | 43.5 ms a frame in play (23 fps; the game wants 30): was 113 ms. The game runs at about 73 % speed (slow motion), sound too |
| Heat | 40 % | 85 C after about 13 minutes at 1.416 GHz; the session then stops (the user's choice) |
| Launching | 30 % | Needs a PC: `sh scripts/play.sh` over SSH. No menu entry on the device yet |

## What is left, in order (for the next session)

1. **Sound: listen and fix what is wrong.** The user's next play session has sound (`play.sh`; `PS2PORT_AUDIO=0` turns it off).
   Then: gaps while loading, Gaussian interpolation, reverb, noise and pitch modulation if the game uses them.
   `PS2PORT_SPU_DEBUG=1` prints a `[spu-mix]` line per core every 5 s; `PS2PORT_SPU_PCM=FILE` records the mix. Code:
   `ps2xIOP/src/emulator/core/iop_memory.cpp` (private IOP patch), `runner/audio_sdl.cpp`. Plan: `docs/PLAN_AUDIO.md`.
2. **Game speed to 100 %.** The game counts vblanks that follow its own CPU cycles; at 23 fps it sees 30. Try vblanks on real
   time (the game reads elapsed vblanks, so it may step further per frame). This also brings sound to the right pitch.
3. **Saves.** Find where the game offers to save (not the Start button in Ollivanders), save, then Continue from the menu.
   `PS2PORT_MC_LOG=1` (already on in `play.sh`) logs every memory card command.
4. **Movies.** Decode the `.PSS` movies (MPEG-2 video through the IPU, ADPCM/PCM audio) or at least the main menu's background.
5. **Play through the game.** Each chapter: does it load, look right, sound right, not crash. The user's play sessions are the
   main way (the logs record what happens); saves let a session start where the last one stopped.
6. **Speed and heat.** 43.5 ms to 33 ms a frame: the GS thread's stage A is the limit (VU1's code about 22 ms of it); less
   work also means less heat and longer sessions.
7. **Start from the device.** A Knulli "Ports" entry (a script in `/userdata/roms/ports/`) so the game starts from the
   device's own menu, without a PC.

## How to pick up

Read `NOTES.md`, "Current state and handoff", then the phase files: `docs/PHASE_7.md` (speed, the out-of-memory fix,
controls), `docs/PHASE_8.md` (the whole game: saves, sound). Device rules are in `CLAUDE.md` (frame table, 85 C stop, no
screen control, no pushes). Play: `sh scripts/play.sh 15` from Git Bash on the PC; quit on the device with Menu + Start.
