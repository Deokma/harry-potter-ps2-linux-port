# Progress

Last updated 2026-11-11. "Measured" means measured on the device (RG40XX H, 1.416 GHz) unless it says PC. Percentages are
estimates. Details and every number are in the phase reports (`PHASE_0.md` to `PHASE_8.md`).

## In one line

The engine side is far along: all of the game's code runs natively, graphics are right in every scene seen so far, controls,
loading and saving work, and the game world runs at its real speed. About 2-3 % of the game's content has been seen running on
the device (boot to the first playable room, Ollivanders). Left: sound speed, frame rate, heat, movies, starting from the
device's menu, and playing through the rest to fix what later chapters break.

## What works

| Area | State |
|---|---|
| Game code | 5,579 of 5,708 functions translated by eegen with the R5900's own semantics, the rest are native stubs (128) and one PS2Recomp function. Checked byte for byte against PCSX2 recordings on replayed frames. |
| Vector units | VU0 and VU1 microprograms translated to native code and checked against recordings. |
| Graphics | GS on OpenGL ES 3. Menus, cutscenes, Diagon Alley and Ollivanders match the PCSX2 reference (user's check, 2026-11-09). |
| Game speed | The world steps on the game's own 25 Hz clock, which follows real time: 25.0 ticks a second measured in play while 15-18 frames a second are drawn. |
| Controls | The device's pad through SDL2, PS2 layout by position; short taps are held for the game's next read; at most one frame between the game and the screen. |
| Saves | Save from the pause menu, fresh boot, Continue loads back into the room (measured). |
| Screenshots | Menu + L1 on the device saves a PNG (`docs/VISUAL_CHECKLIST.md`). |

## Timeline

| Phase | Result |
|---|---|
| 0-2 | Feasibility: disc and ELF analysis, device benchmarks, decision to recompile statically rather than emulate. |
| 3 | PS2Recomp trial; our own generator (eegen) and a replay harness that reproduces a recorded frame byte for byte. |
| 4 | Title screen on the device through the Mali-G31. |
| 5-7 | Whole game in the runner; graphics on two threads of their own, IOP on its own thread; gameplay frame from 113 ms to about 44 ms; memory growth fixed; controls. |
| 8 (now) | Saves, pause menu, the Ollivanders picture, input lag, the stick, palette textures on the GPU, screenshots, first sound mixer, movie decoder wired in. |

## Fixed most recently (2026-11-09 and 10)

- **Input lag:** the game could run up to about 5 s of drawing ahead of the screen; now at most one frame.
- **Stick left/right:** 16 of the game's 35 `sqrt.s` read the wrong register under the semantics inherited from PS2Recomp. The
  code is now generated with the R5900's own semantics. Scripted check: left and right pushes change over 80 % of the picture.
- **Palette textures with the linear filter:** expanded once instead of filtered in the shader; the load screen's fog went from
  61 to 21.6 ms of GPU a frame.
- **GPU clock** raised to 648 MHz for sessions (the governor stayed at 240 MHz).
- **Menu and loading blinking (2026-11-11, built, not yet run on the device):** the screen now shows each flip with that flip's
  display registers; it used the live ones, which in fast screens already named the next, half-drawn buffer.
- **Sound speed (2026-11-11, PC, not yet run on the device):** the sound driver's clock stood still while the game waited at
  the end of each frame; now it follows real time (`PS2PORT_REALTIME`, on).
- **Movies (2026-11-11, PC, not yet run on the device):** the logos, the main menu's moving background and the trailer now
  decode and reach the screen; movie sound is silent for now.
- **Game speed was misread:** the "about half speed" figure came from a debug counter. The game's logic clock reads COP0 Count,
  which the runtime serves from real time, and it ticks at 25.0 a second on the device.

## Where it stands in numbers

| Thing | Value |
|---|---|
| Frame in Ollivanders | about 55-67 ms (15-18 frames a second); the game draws at 30. Logic still at full speed. |
| Sound | 24-28 k of 48 k samples a second on the device before 2026-11-11; with the EE clock on real time, 47-48 k on the PC at the device's pace (not yet measured on the device). |
| Heat | 85 C after 5 to 13 minutes of play at 1.416 GHz; a guard stops the session there. |
| Memory | flat in a 15-minute soak after the Phase 7 fix. |

## Open, in rough order

1. **Player's checks:** stick, input lag, Menu + L1, the visual checklist for chapter slice 1, the Ports launcher's first start.
2. **Cutscenes that sometimes never end:** one cause fixed (a race in the IOP emulation that lost EA's stream go-on); a second
   stall remains at the device's pace. Then movie sound (silent).
3. **Frame time** in play: back toward 33 ms. The game thread is the limit in Ollivanders, then VU1 and GS parsing. A faster
   EE also fixes sound.
4. **Heat:** runs at lower CPU clocks, less work a frame.
5. **Movies:** pictures play on the PC since 2026-11-11 (EA logo, WB splash, the main menu's moving background, the trailer);
   the decoder reads the movie from the disc folder because EA's stream stops refilling. Left: the user's visual check, decode
   cost on the device, the movies' own sound.
6. **Float semantics:** `cvt.w.s` truncates as on the console, but arithmetic still rounds to nearest where the R5900 rounds
   toward zero.
7. **Play through** the rest of the game, chapter by chapter.
8. **Distribution:** the Ports launcher's first start on the device; a one-command build; publishing the runtime patches;
   the `LICENSE` file.

## Known limits

- Only NTSC-U `SLUS-20826` is exercised; PAL has not been built.
- The PC build uses a software renderer and has no window or sound output; it is for logic tests, not for playing.
- Device figures are from one unit; PC and qemu timings are never quoted as device timings.
- The Ports launcher (`runner/knulli/`) has not run on the device yet.
