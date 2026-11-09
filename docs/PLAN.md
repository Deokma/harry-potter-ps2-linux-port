# Plan from 2026-11-09 (one page)

Source: `docs/STATUS.md` "What to do next" and the user's brief of 2026-11-09. Budgets are tool calls; confidence is
how likely the item finishes inside its budget (estimate). Stop rules: CLAUDE.md frame table (<=50 ms go, 50-55 ask,
>55 stop, gameplay stretches only; street and cutscenes report-only), device misbehaviour (reboot, >85 C), a decision
from the user, or the item's budget used up. Items 3-5 end with a stop for the user's architecture decision.

| # | Item | Exit criteria | Budget (confidence) | Stop rule | Depends on |
|---|---|---|---|---|---|
| 1 | Movies, PC then device | x86 build links; 80 s PC boot shows the WB/EA logos and the menu background changing between frames; aarch64 build; on the device: frame time in the menu and in one cutscene, decode cost per core (`freq.txt`, sampler) | 15 (60 %: two long builds, decoder untested) | Table; build failure not fixable in 3 calls -> report, keep `PS2PORT_MOVIE_STUB=1` | none |
| 2 | Saves | Location of the game's save offer known; a save written to `mc0/` (`PS2PORT_MC_LOG=1` shows the write); after a restart Continue/Load reaches the saved place | 25 (50 %: may need the user to play to the save point) | (b) when the user has to play: instructions, then wait | 1 not required |
| 3 | Thermal study | Trip points (passive/critical) and cooling devices read; sustained runs at 1.416, 1.2, 1.0 GHz with temperature/clock/frame time every 30 s; a proposal with data and the SoC critical threshold (85 C stop unchanged) | 20 (70 %) | 85 C guard; **stop and wait for the user afterwards** | none |
| 4 | VU1 on the GPU (study only) | Map of the 8 VU1 programs to GLES vertex shaders; data formats (VIF unpack in, GIF out); estimated savings in stage A (~22 ms VU1) and GS parsing (~18 ms), risks, test coverage to redo | 20 (75 %) | **Stop and wait** | none |
| 5 | Build-time optimisation (study only) | Measured per-thread share of what still runs interpreted/translated at runtime (IOP, VU0 fallbacks, ~130 untranslated functions, PS2Recomp `FUN_`); unused build-time options (PGO, i-cache layout, LTO, A53 tuning, static IOP/HLE) with the two likeliest marked; one-page "disc image in, device package out" pipeline | 15 (70 %) | **Stop and wait** | none |
| 6 | Sound | User's listening notes recorded; options to decouple audio from game speed (with risks, and why `PS2PORT_REALTIME=1` breaks the first loading screen) proposed **before** coding | open, by cycles of ~8 calls | (b) each cycle waits for the user's notes | user time |
| 7 | Game speed to 100 % | Game time follows real time in play; first loading screen ends; measured on the PC with `PS2PORT_SLOWDOWN=70`, then the device | 25 (40 %) | Only after the user reviewed 3-5 | 3, 4, 5 reviewed |
| 8 | Play through, first bounded goal | **"Chapter slice 1":** from New Game through Diagon Alley and Ollivanders to the next 5 area loads after Ollivanders (or the first save point, whichever is later): each area loads, looks right (user's notes), no crash, 15-minute session flat in memory | 30 + user sessions (50 %) | Table on measured gameplay frames; (b) for user play | 2 (saves), 7 preferred |
| 9 | Frame time and heat | Gameplay frame <= 33 ms at 100 % game speed (target); SoC < 85 C for >= 60 min (target, value to be set from item 3's data); no memory growth over 60 min | set after 3-5 | Table | 3-5 decisions |
| 10 | Start from the device | Knulli Ports entry under `/userdata/roms/ports/` launches the game without a PC | 10 (80 %) | (b) for the user to try it | none |

**Targets, first values (estimates, to be replaced by measurements):** frame <= 33 ms at 100 % speed (today 43.5 ms at
~73 % speed, measured); thermal: today 85 C after ~13 min at 1.416 GHz (measured); memory: flat over 13 min (measured).

**Order now:** 0 -> 1 -> 2 -> 3 -> 4 -> 5 (stop) -> 6 -> after the user's decision 7 -> 8 -> 9 -> 10.
Context rule: past about 300k tokens in one session, write the handoff into `NOTES.md` and ask for a fresh session.
