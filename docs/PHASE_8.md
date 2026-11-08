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
