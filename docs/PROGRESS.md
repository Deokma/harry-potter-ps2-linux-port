# Progress

Last updated 2026-11-10. "Measured" means measured on the device (RG40XX H, 1.416 GHz) unless it says PC. Percentages are
estimates. Details and every number are in the phase reports (`PHASE_0.md` to `PHASE_8.md`).

## In one line

The engine side is far along: all of the game's code runs natively, graphics are right in every scene seen so far, controls,
loading and saving work. About 2-3 % of the game's content has been seen running on the device (boot to the first playable
room, Ollivanders). Left: speed, heat, sound quality, movies, and playing through the rest to fix what later chapters break.

## What works

| Area | State |
|---|---|
| Game code | 5,574 of 5,708 functions translated by eegen, the rest are native stubs (128), stubbed movie functions (5) and one PS2Recomp function. Checked byte for byte against PCSX2 recordings on replayed frames. |
| Vector units | VU0 and VU1 microprograms translated to native code and checked against recordings. |
| Graphics | GS on OpenGL ES 3. Menus, cutscenes, Diagon Alley and Ollivanders match the PCSX2 reference (user's check, 2026-11-09). |
| Controls | The device's pad through SDL2, PS2 layout by position; short taps are held for the game's next read. |
| Saves | Save from the pause menu, fresh boot, Continue loads back into the room (measured). |
| Boot to play | New Game reaches the first room; Continue reaches it about 58 s after start. |
| Screenshots | Menu + L1 on the device saves a PNG (`docs/VISUAL_CHECKLIST.md`). |

## Timeline

| Phase | Result |
|---|---|
| 0-2 | Feasibility: disc and ELF analysis, device benchmarks, decision to recompile statically rather than emulate. |
| 3 | PS2Recomp trial; our own generator (eegen) and a replay harness that reproduces a recorded frame byte for byte. |
| 4 | Title screen on the device through the Mali-G31. |
| 5-7 | Whole game in the runner; graphics on two threads of their own, IOP on its own thread; gameplay frame from 113 ms to about 44 ms; memory growth fixed; controls. |
| 8 (now) | Saves, the pause menu, the Ollivanders picture (a DMA chain was cut at 4,096 tags), input lag, the stick, first sound mixer, movie decoder wired in. |

## Fixed most recently (2026-11-09 and 10)

- **Input lag:** the game could run up to about 5 s of drawing ahead of the screen. It is now at most one frame ahead; the
  load-game screen went from one screen update per 5 s to 14-15 a second, then to about 44 with the shader fix below.
- **Stick left/right:** 16 of the game's 35 `sqrt.s` instructions read the wrong register under the semantics inherited from
  PS2Recomp (the R5900 takes the operand from `ft`). The runner's code is now generated with the R5900's own semantics.
  Scripted check: left and right pushes now change over 80 % of the picture; the player's own test is pending.
- **Palette textures with the linear filter:** expanded once instead of filtered in the shader (8 texture reads a pixel);
  the fog screen's GPU time went from 61 to 21.6 ms a frame.
- **GPU clock:** the device's governor stayed at 240 MHz under load; game runs now set 648 MHz.

## Where it stands in numbers

| Thing | Value |
|---|---|
| Frame in Ollivanders | about 55-67 ms (15-18 frames a second); the game wants 33 ms. It was 44 ms before the picture fix restored the missing geometry. |
| Game speed in play | about half: the game counts two vertical blanks a frame whatever the frame takes. |
| Heat | 85 C after 5 to 13 minutes of play at 1.416 GHz; a guard stops the session there. |
| Memory | flat in a 15-minute soak after the Phase 7 fix. |

## Open, in rough order

1. **Player's checks:** stick, input lag, the Menu + L1 screenshot, the visual checklist for chapter slice 1.
2. **Frame time** in play: back toward 33 ms. The game thread is the limit in Ollivanders, then VU1 and GS parsing.
3. **Game speed:** make game time follow real time; a first attempt broke the first loading screen.
4. **Heat:** runs at lower CPU clocks, less work a frame.
5. **Sound:** a first mixer plays the intro's music and voice; pitch follows the slow game; no reverb.
6. **Movies:** the decoder (a minimal FFmpeg) is built and wired in, but the runner stops in the first movie, so movies are
   stubbed (logos and other videos are skipped).
7. **Float semantics:** `cvt.w.s` now truncates as on the console, but arithmetic still rounds to nearest where the R5900
   rounds toward zero.
8. **Play through** the rest of the game, chapter by chapter.
9. **Start from the device** (a Knulli Ports entry) instead of over SSH from a PC; a one-command build; publishing the
   runtime patches and adding the `LICENSE` file.

## Known limits

- Only NTSC-U `SLUS-20826` is exercised; PAL has not been built.
- The PC build uses a software renderer and has no window or sound; it is for logic tests, not for playing.
- Device figures are from one unit; PC and qemu timings are never quoted as device timings.
