# Savestates for Phase 1 measurements

You create these; I measure from them. Each savestate fixes one scene so
that every measurement (GS dumps, EE cycle-rate sweep, RAM checks) starts
from exactly the same point and can be repeated.

## Rules

1. **Use the clean profile only:** `D:\ps2port-data\pcsx2-clean\pcsx2-qt.exe`.
   Do not use the bundle's PCSX2 for these. Its "50 FPS" patch writes a
   value into game RAM (`0x3A4FD8`), and a savestate keeps that value even
   after patches are turned off. That would silently change the game's
   frame pacing in every measurement.
2. **Boot fresh**, then reach the scene by loading your game save from the
   memory card. Your memory cards were copied into
   `D:\ps2port-data\memcards\`, so your PAL progress is there.
3. Save **2–5 seconds before** the heavy moment, so that loading the state
   and waiting a moment brings you into it. For walking scenes, stand where
   the busiest view is on screen.
4. Keys (same as your bundle): **F2** = next slot (the OSD shows the slot
   number), **Shift+F2** = previous slot, **F1** = save to the selected slot,
   **F3** = load it.
5. If a scene does not exist or you cannot reach it, skip it and say so.
   S08 can stand in for anything that is missing.

Files land in `D:\ps2port-data\savestates\` as
`SLES-52055 (C71A6146).0N.p2s` (PAL) or `SLUS-20826 (C71A6146).0N.p2s` (NTSC),
where `N` is the slot. The two discs have separate slot numbering.

## Scene list

### PAL disc (SLES-52055, `private\pcsx2\Game\HarryPotter.chd`)

| Slot | Scene ID | Scene | What makes it useful |
|---|---|---|---|
| 1 | S01 | Main menu (after the intro, menu fully shown) | baseline; frontend rendering |
| 2 | S02 | The busiest Hogwarts interior you know: several characters on screen at once (e.g. a hall or crowded corridor) | character count, interior fill |
| 3 | S03 | The largest outdoor view: grounds / courtyard with the longest view distance | geometry and draw-call peak |
| 4 | S04 | A flying lesson (on the broom, in the air) | fast camera, large open area |
| 5 | S05 | A Quidditch match in progress (many players visible) | likely the heaviest scene overall |
| 6 | S06 | A fight / boss / spell-heavy moment (as many effects and enemies on screen as possible) | particles, alpha blending |
| 7 | S07 | Just before an **in-engine** cutscene starts (rendered by the game, not a movie) | cutscene camera, close-up characters |
| 8 | S08 | Your pick: wherever the game looks most complex or felt slowest to you | catch-all for the worst case |

### NTSC disc (SLUS-20826, `private\disc\SLUS-20826.iso`)

| Slot | Scene ID | Scene | What makes it useful |
|---|---|---|---|
| 1 | N01 | Main menu | PAL vs NTSC comparison, 60 Hz pacing |
| 2 | N02 | First playable area, a few seconds in, busiest view | NTSC gameplay baseline |
| 3 | N03 | *Optional:* the same place as one of S02–S08, if you get there | 1:1 PAL/NTSC check |

### No savestate needed

| ID | What |
|---|---|
| PILOT | NTSC main menu, booted fresh. A first capture so I can check the tools on a real dump before you do the rest |
| F01 | NTSC boot from power-on: the EA logo and intro movie (FMV) |

## When you are done

Tell me which slots exist (and which scene S08 is). The files are on this
machine, so there's nothing to send. The measurement steps for each scene
are in [MEASURE_RUNBOOK.md](MEASURE_RUNBOOK.md).
