# Visual checklist

Visual checks are the user's (2026-11-10). The assistant logs numbers only (frame hashes, pixel differences against a reference,
counters, timings: `scripts/png_stats.py`) and does not open screenshots unless a numeric check flags something or the user asks.
When a step needs a visual comparison, the assistant lists the scenes here and waits for the user's notes.

## Taking and reading screenshots

- **Device:** hold **Menu, press L1** in the running game (not with Start: Menu + Start quits). The frame is saved as
  `/userdata/screenshots/ps2port-m<game mode>-<uptime>s-f<game frame>.png` (512x447, about 690 KB) and the log gets one line:
  `[shot] frame N mode M uptime S s -> FILE`. Game modes: 0 load, 2 play and menus, 3 pause menu, 4 Remembrall, 5 hint box,
  6 save screen, 9 cutscene.
- **Copy to Windows:** `sh scripts/shots_fetch.sh` (Git Bash, repo root) copies them to `private\screenshots\device\`. Knulli's
  network share was not running when checked (no `smbd`), so scp is the way.
- **PC runner:** it has no window and no keyboard. A screenshot is taken by pressing **Enter** in the terminal it was started from,
  by `kill -USR1 PID`, or at fixed times with `PS2PORT_SHOT_AT=20,45`; files go to `private/screenshots/`. For reference pictures
  PCSX2's own F8 is the better tool.
- **Knulli's own Function + L1:** it is bound per emulator through evmapy profiles, which run only for games started from the
  frontend; the runner is started over SSH with the frontend stopped, so the key does nothing there (read from the device's
  configuration, not pressed). The command behind it, `knulli-screenshot`, does work while the runner owns the display (640x480
  PNG of the panel, checked once: same share of lit pixels as the runner's own shot of that moment).
- Screenshots are game material: they stay under `private/` (never tracked).

## Template

| # | Scene | When (s after start, or how to get there) | Look at | Reference | Known issue | User's notes |
|---|---|---|---|---|---|---|
| n | name | time or steps | what should match | file under `D:\ps2port-data\` or "PCSX2, F8" | what is already known | filled in by the user |

## Chapter slice 1 (New Game through Diagon Alley and Ollivanders, then the next 5 area loads)

Times are from device runs on 2026-11-10 with nobody skipping; they move by a few seconds between boots.

| # | Scene | When | Look at | Reference | Known issue | User's notes |
|---|---|---|---|---|---|---|
| 1 | Language screen | 5-20 s; stays until Cross | flag, castle silhouette, the two languages, button hint | PCSX2, F8 | none known | |
| 2 | Opening narration ("There was nothing about the starry sky...") | from ~21 s (mode 9); Start skips it | sky, stars, trees, subtitle text and timing | PCSX2, F8 | plays without the movie parts (movie stub) | |
| 3 | Main menu (New Game, Continue, Options, Language) | after the narration or Start, ~24 s | logo, four entries, highlight, background | PCSX2, F8 | background is black here: not known whether PCSX2 shows more | |
| 4 | Load-game screen ("Select a Game to Load") | Main menu, Down, Cross | fog moving behind the list, save line, button hints | PCSX2, F8 | fog was slow (16 frames a second), now about 44; date shows 6 h off | |
| 5 | Loading screen (castle, progress bar) | after choosing a save or New Game | castle picture, bar filling smoothly | PCSX2, F8 | none known | |
| 6 | New Game cutscenes: Hagrid and the baby, Privet Drive | New Game, ~25-60 s in | faces, textures, subtitles, the car and houses | PCSX2, F8 | none known | |
| 7 | Leaky Cauldron: Quirrell | New Game, ~25 s after its load when skipping | characters close up, subtitles | PCSX2, F8 | none known | |
| 8 | Diagon Alley walk to Ollivanders | after the Leaky Cauldron | street textures, shop fronts, HUD (lightning bolt, button ring), people | `snapshots\N01`-`N03` if they show it, else PCSX2 | game speed about half | |
| 9 | Ollivanders: entering, hint boxes | Continue: ~58 s after start | shelves, carpet, light shafts, hint text, HUD | the user's three PCSX2 pictures of 2026-11-09 | frame about 60-67 ms | |
| 10 | Ollivanders: "Stand over there on that platform to try your wand", wand trial | walk to the platform | platform, wand effects, Ollivander | PCSX2, F8 | not yet played through on the port (heat stop at 85 C in the last session) | |
| 11 | Pause menu (Start in play) | any time in play outside a hint box | entries, fog or background, highlight | the user's PCSX2 picture | none known | |
| 12 | Save screen and its prompts | Pause menu, Save Game | three slots, "Are you sure" prompt, written line | PCSX2, F8 | time 6 h off | |
| 13-17 | The next 5 area loads after Ollivanders | in order of play | first view of each area, HUD, any new effect | PCSX2, F8 | not reached yet | |

For each row a screenshot with Menu + L1 at the moment that looks wrong is enough; the file name carries the game mode, time and
frame, which is what the assistant needs to find it in the logs.
