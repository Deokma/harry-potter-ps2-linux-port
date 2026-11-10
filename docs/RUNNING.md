# Running the port

Three ways to start the game. All need your own disc's files; see the README.

## 1. From the PC over SSH (the way every session so far has run)

The device runs Knulli with SSH on port 2222 (key-based; `BENCH_HOST` and `BENCH_SSH_OPTS` in `.env`). From Git Bash in the
repository:

1. Build the device runner in WSL (see the README), or use the last one in `private/third_party/build-m2/aarch64/`.
2. Start a session (default 30 minutes; it stops the device's menu while it runs and restarts it after):
   ```bash
   sh scripts/play.sh
   ```
3. Quit on the device with Menu + Start held for a second. The log, frame times and memory figures are in
   `/userdata/ps2bench/run/play-HHMMSS/` on the device.

`scripts/play.sh 15 PS2PORT_FRAME_SYNC=0` passes switches (`docs/ENV_VARS.md`). The script refuses to start while another run is
going.

## 2. From the device's own menu (Knulli Ports): written, not yet tried

`runner/knulli/HarryPotterPS2.sh` is a Ports launcher: it raises the GPU clock for the session, stops the game at 85 C and logs
to `/userdata/roms/ports/HarryPotterPS2/last-run.log`. It was written on 2026-11-10 without access to the device, so it has
never run there. To try it:

1. Put it on the device (copies the runner and the launcher; the disc's files must already be in `/userdata/ps2bench/game`,
   or edit `GAME_DIR` at the top of the launcher):
   ```bash
   sh scripts/install_port.sh
   ```
2. On the device: Start -> Game settings -> Update gamelists, then Ports -> HarryPotterPS2.
3. If nothing happens, read `/userdata/roms/ports/HarryPotterPS2/last-run.log` (over SSH, or on the SD card).

Open questions for that first try: whether the runner gets the display while EmulationStation is in the background (test runs
stop the frontend first), and whether the frontend comes back cleanly after a quit.

## 3. On the PC (logic tests, no window)

The x86 runner in WSL uses the software GS and has no window and no sound output; it is for checking logic, loading, saves and
sound data, not for playing or timing.

```bash
sh runner/try.sh 120 "CROSS@5+0.3" PS2PORT_WATCH=0x38c194
```

From WSL in the repository. Frames land in `private/third_party/trial/boot-*/frames/` (PPM files named `.png`). Useful for
faster runs: `DRAW=0/1` (no rasterizing) and `PS2PORT_PRESENT_EVERY=30`. `PS2PORT_SLOWDOWN=50` runs the game at about the
device's pace.

## Controls

The device's pad maps to the PS2 pad by position: bottom Cross, right Circle, left Square, top Triangle; L1/R1/L2/R2, both
sticks, Select and Start. Menu + Start quits; Menu + L1 saves a screenshot (`docs/VISUAL_CHECKLIST.md`).

## Saves

The game's memory card is the folder `mc0/` next to `SLUS_208.26` on the device (`/userdata/ps2bench/game/mc0/`). Save from the
pause menu (Start in play, outside hint boxes); Continue on the title menu loads. `scripts/ps2mc_extract.py` lists or copies the
saves on a PCSX2 memory card image into that layout.

## Heat

Sessions at 1.416 GHz reach 85 C after 5 to 13 minutes, and every script stops the game there. When a session ends early, the
log's last lines say `THERMAL STOP`.
