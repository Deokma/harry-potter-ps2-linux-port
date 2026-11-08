# Phase 8: the whole game

User, 2026-11-08: "continue porting resources, scenes, gameplay and game".

**Resources need no porting:** the recompiled game reads its models, textures, levels, cutscenes and text from the original
disc files (`/userdata/ps2bench/game/` on the device) exactly as on a PS2. What remains is making every part of the game run
right on the device, and the parts the runtime only stubs.

## Plan (in order)

| # | Item | Why | State |
|---|---|---|---|
| 1 | **Saves** (memory card) | Sessions end at 85 C after about 13 minutes; without saves no one gets past the first chapter. Also the way to test later scenes. | Loading side checked; saving not yet (below) |
| 2 | **Later scenes** | Checked only up to Ollivanders (about 4 minutes of a long game). | Needs saves or the user's play sessions |
| 3 | **Game speed** | About 73 % in play: vsync follows guest cycles (Phase 7). Try vsync on real time. | Not started |
| 4 | **Sound** | `docs/PLAN_AUDIO.md`: SPU2 mixing on the IOP thread. | Plan only |
| 5 | **Heat** | 85 C after about 13 minutes at 1.416 GHz; the user keeps the 85 C stop. | Open |

## 1. Saves

- **This game's memory card calls are native stubs** (`sceMcInit` ... at `0x2F9238` onward, `trial/analyzer.toml`): the runtime's
  `Kernel/Stubs/MemoryCard.cpp`, which keeps the card as a host folder, `mc0/` next to the ELF (on the device
  `/userdata/ps2bench/game/mc0/`), one folder per save. The IOP's MCSERV stand-in (`ps2xIOP/src/modules/mcserv.cpp`, whose GetDir
  returns no entries) is not on this game's path; a fix tried there was taken back out, unused.
- The game's save folder is `BASLUS-20826-HPATPS`, three slots ("Save Game 1-3"), an `icon.sys` (strings in the ELF).
- **Measured (`panel-135947`):** with a dummy `mc0/BASLUS-20826-HPATPS/test.bin`, main menu, Continue: the game says the save data
  in memory card slot 1 "appears to be corrupt". So it finds and reads the folder: the listing and reading paths work. Dummy removed.
- **Not checked: writing a save.** Where the game saves is not known yet; Start in the Ollivanders gameplay (`panel-141136`, 240 s)
  showed no pause menu within 10 s. At boot the game asks the card for its info three times (0) and changes to its folder (-4,
  not there yet). `PS2PORT_MC_LOG=1` prints every card command and its result (runtime patch).
- The user's PCSX2 cards (`D:\ps2port-data\memcards`): `Mcd001.ps2` formatted and empty, `Mcd002.ps2` not formatted. No saves to
  borrow. `scripts/mc_extract.py` lists and copies saves from a PCSX2 card image into an `mc0` folder, for when there are some.

## Device

`panel-141136` (the fourth run in about 20 minutes) ended with **THERMAL STOP: 85.3 C**: device runs stopped there (stop rule c).

## 4. Sound (started 2026-11-08, the user's choice)

- **The mixer** (`ps2xIOP/src/emulator/core/iop_memory.cpp`, private IOP patch): on the IOP thread, in step with the IOP's clock
  (one 48 kHz frame every 768 IOP cycles; also after every IOP slice, `ps2portSpuTick`). ADPCM decode, pitch with linear
  interpolation, ADSR envelopes, voice volumes (fixed; a sweep keeps the last fixed level), VMIX, master volumes, core 0 into
  core 1 through core 1's input volume. The registers the game reads (NAX, ENDX, ENVX) behave exactly as in the model without
  sound; a keyed-off voice's release sounds apart from them. Out through a ring (0.68 s) to `runner/audio_sdl.cpp`: SDL2 audio
  (ALSA on the device, 48 kHz, 1,024-frame callbacks), resampled at the mixer's measured rate, kept near 80 ms waiting.
  `PS2PORT_NO_SOUND=1` (no mixing), `PS2PORT_AUDIO=0` (no output), `PS2PORT_SPU_PCM=FILE`, `PS2PORT_SPU_DEBUG=1`.
- **First run: almost silent** (peak 1-2 of 32,767). `[spu-mix]` showed core 0 playing a stereo stream (voices 0 and 1, pitch
  0x0EB3 = 44.1 kHz, envelope full, samples up to 15,257) and core 1's input volume (AVOL) at 0x7FFF, read as a sweep-format
  volume: -2/32,768. AVOL is a plain signed level. Fixed.
- **Measured after the fix (`panel-144549`, boot to the intro, 1.416 GHz):** RMS 2,000-4,200 per channel (-18 to -24 dBFS),
  peaks to 30,948, no clipping, left and right alike; silent only during boot and loading. The mixer made 29,000-33,000 frames
  a second of real time in the intro (the IOP's clock is that much behind real time, as the game is), so sound plays at about
  60-70 % speed and pitch until item 3 (game speed). The output ran dry for 81,628 frames by 60 s (boot and loading), none
  between 60 and 70 s.
- **Not done:** listening (the user, next session); Gaussian interpolation; reverb; noise and pitch modulation; ADMA streams;
  the cost on the IOP thread not measured yet (estimate in `PLAN_AUDIO.md`: a few % of a core).
