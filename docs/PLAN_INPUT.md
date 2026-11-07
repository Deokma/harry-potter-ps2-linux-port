# Plan: real pad input (RG40XX H, SDL2)

Phase 7, item 4a. Implemented on 2026-11-08 in `runner/pad_sdl.cpp`; results in `docs/PHASE_7.md`, "Controls".

## What exists

- **The game reads the pad through libpad's `scePadRead`, which PS2Recomp runs natively** (`ps2xRuntime/src/lib/Kernel/Stubs/Pad.cpp`, one of the native stubs): no PADMAN or SIO2MAN code runs on the emulated IOP. The stub fills the game's 32-byte pad buffer from `PS2Runtime::padBackend().readState(port, slot, data, size)`.
- **The backend (`ps2_pad.cpp`, `PSPadBackend::readState`) already builds a DualShock 2 report:** analog mode (`0x73`, and `0x79` with pressures, fixed earlier), buttons active-low, sticks centred at `0x80`. It reads input through raylib's API (`IsGamepadAvailable`, `IsGamepadButtonDown`, `GetGamepadAxisMovement`, keyboard as a fallback) and through `PS2PORT_PAD` scripts.
- **In the runner raylib is a stub** (`runner/host/raylib.h`), so no real input reaches it today. SDL2 is already loaded at run time by the runner (`bench/src/sdl_dyn.c`, for the display), from the firmware's own libSDL2.

## What to build

1. **`runner/host/pad_sdl.cpp`: the raylib gamepad calls the backend uses, implemented over SDL2's GameController API** (through `sdl_dyn`, no link-time SDL): `IsGamepadAvailable`, `IsGamepadButtonDown`, `GetGamepadAxisMovement`, plus `IsKeyDown` returning false. The backend's code stays as it is. Events are pumped on the main thread, which already runs SDL; the state is published to the EE thread as one atomic 64-bit snapshot (buttons, four axes), read in `readState`.
2. **Controller selection:** the first SDL game controller; `SDL_GameControllerAddMappingsFromFile` on Knulli's `gamecontrollerdb.txt` (the firmware ships one with the RG40XX H's built-in pad); `PS2PORT_PAD` scripts still win when set (tests).
3. **Pressure-sensitive buttons:** the RG40XX H has none; report 0xFF when pressed and 0 when not, for the pressures the game asks for in `0x79` mode.

## Button mapping (positional, as SDL's GameController reports them)

| RG40XX H | SDL button | DualShock 2 |
|---|---|---|
| Bottom face (B) | A (south) | Cross |
| Right face (A) | B (east) | Circle |
| Left face (Y) | X (west) | Square |
| Top face (X) | Y (north) | Triangle |
| L1 / R1 | left / right shoulder | L1 / R1 |
| L2 / R2 (digital) | left / right trigger axis > 0.5 | L2 / R2 |
| Start / Select | start / back | Start / Select |
| Left / right stick, clicks | left / right stick, L3 / R3 | left / right stick, L3 / R3 |
| D-pad | D-pad | D-pad |
| Menu (Knulli's hotkey) | guide | not passed on: the firmware's |

Positional mapping keeps the PS2's layout under the thumb (Cross at the bottom). Sticks: SDL's -32768..32767 to 0..255 with 0x80 centre, a dead zone of about 10 % (the pad's sticks rest slightly off centre), Y not inverted (the PS2 reports up as 0).

## What padman must see

Only what the native `scePadRead` stub reports, as now: port 0 slot 0 connected, state "stable", mode analog (`0x73`) or pressure (`0x79`) as the game sets it with `scePadSetMainMode` and `scePadEnterPressMode`, actuators accepted and ignored (no rumble motor on the RG40XX H). A second port reports no pad.

## Checks

1. On the PC: an Xbox or DualShock pad through WSLg's SDL; buttons and sticks logged once a second (`PS2PORT_PAD_LOG=1`).
2. On the device, without screen control: the user presses a short sequence while a test run logs the pad state; then the menu (Cross on New Game) and walking in the shop.
3. Latency: the state is sampled once per frame on the EE thread; at 22 frames a second that is up to 45 ms of added lag (measured later, estimate now).
