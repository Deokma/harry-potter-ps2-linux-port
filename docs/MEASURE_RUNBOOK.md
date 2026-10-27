# Phase 1 measurement runbook (you do the clicks)

Everything happens in the **clean profile**
`D:\ps2port-data\pcsx2-clean\pcsx2-qt.exe`. It was built by
`scripts/make_pcsx2_profile.py` with:
- your controls and hotkeys;
- all patches off;
- EE cycle rate 100 %, MTVU off;
- native resolution;
- performance and GS statistics on the OSD;
- savestates, dumps and logs going to `D:\ps2port-data\`.

Your bundle in `private\pcsx2\` is untouched.

Terminal commands run from the repo folder
`D:\Projects\anbernic\PS2Recompiler` (PowerShell is fine).

Order: **0 → 1 (pilot) → wait for my OK → 2 → 3 for each scene → 4 → 5
(optional)**.

---

## 0. One-time setup (≈5 min)

1. Start `D:\ps2port-data\pcsx2-clean\pcsx2-qt.exe`. It opens windowed.
2. **Settings → BIOS**: point the BIOS folder at the folder holding a BIOS
   **dumped from your own console**, then select that BIOS in the list.
   Nothing is copied; the profile only stores the path.
3. Confirm these settings. They should already be like this; just look:
   - **Settings → Emulation**: EE Cycle Rate **100 % (Normal Speed)**,
     EE Cycle Skipping **Disabled**, Multi-Threaded VU1 (MTVU) **off**.
   - **Settings → Graphics**: Internal Resolution **Native**.
   - **Settings → Hotkeys**: *Save Single Frame GS Dump* = **Shift+F8**,
     *Save Multi Frame GS Dump* = **Ctrl+Shift+F8**, *Save State to Slot*
     = F1, *Load State from Slot* = F3, *Select Next Slot* = F2.
4. Do **not** create per-game settings (Game Properties) in this profile.
   Keep the frame limiter on: don't press F4 (frame limit) or F12 / `.`
   (turbo).

## 1. Pilot (≈10 min). Do this first, then tell me "pilot done"

This checks my GS dump parser on a real dump before you spend time on the
rest.

1. **System → Start File…** → `D:\Projects\anbernic\PS2Recompiler\private\disc\SLUS-20826.iso`.
2. Wait until the main menu is fully shown.
3. Read the OSD and write down: **FPS**, **VPS**, **Speed**, and the
   resolution line.
4. **Hold Ctrl+Shift+F8 for about 3 seconds**, then release. Recording runs
   while the keys are held and stops on release. The OSD reports the saved
   dump.
5. **Press and release Shift+F8 once** (single-frame dump, taken on
   release).
6. In the terminal: `python -I scripts/file_dumps.py PILOT`
7. Tell me "pilot done" plus the numbers from step 3.

I'll run `gsdump_stats.py` on it and either confirm the steps below or
adjust them.

## 2. Savestates

Create the slots listed in [SAVESTATES.md](SAVESTATES.md), in the clean
profile. You can do this before or after the pilot reply.

## 3. Per scene (≈10 min each): S01–S08, N01–N03

Replace `S0x` with the scene ID.

1. Start the right disc (PAL: `private\pcsx2\Game\HarryPotter.chd`, NTSC:
   `private\disc\SLUS-20826.iso`). Press **F2** until the OSD shows the
   scene's slot, then **F3**.
2. Let it run ~3 s. Note **FPS / VPS / Speed** at 100 % EE.
3. **Multi-frame dump:** when the heaviest part starts (busiest view, most
   characters or effects), **hold Ctrl+Shift+F8 for ~5 s** and release.
   Keep it near 5 s, because these files are big (expect 100–500 MB).
4. **Three single-frame dumps:** press F3 to reload, then at **three
   different heavy-looking moments** press and release **Shift+F8**.
5. *Optional:* at the heaviest moment, take a Windows screenshot of the
   window with the OSD (**Win+Shift+S**) and save it as a PNG into
   `D:\ps2port-data\snapshots\`.
6. **EE cycle-rate sweep** (coarse proxy only, see the note below). For
   each of **75 %, 60 %, 50 %**:
   1. Settings → Emulation → EE Cycle Rate → the value; close Settings.
   2. **F3** to reload the scene.
   3. Watch the same ~10 s and note the **lowest FPS** the OSD shows.

   Then set it back to **100 %**.
7. In the terminal: `python -I scripts/file_dumps.py S0x`. This moves this
   scene's dumps and screenshots into `D:\ps2port-data\snapshots\S0x\`.
8. Fill in the scene's row in `D:\ps2port-data\reports\observations.md`.

## 4. FMV (F01)

1. Boot the NTSC ISO fresh (System → Start File…).
2. During the EA logo / intro movie: **hold Ctrl+Shift+F8 ~3 s**, release,
   then **Shift+F8** once.
3. `python -I scripts/file_dumps.py F01`

## 5. Optional: audio streaming run (A01, ≈3 min)

This logs every disc read, so I can measure the streaming rate and see
which files stream.

1. **Close PCSX2.** Then run
   `python -I scripts/pcsx2_set.py EmuCore.CdvdVerboseReads=true`
2. Start PCSX2, load S02 (or any scene with music and speech), and play
   normally for **60 s** (walk, trigger dialogue). Close PCSX2.
3. `copy D:\ps2port-data\logs\emulog.txt D:\ps2port-data\reports\A01_emulog.txt`
4. `python -I scripts/pcsx2_set.py EmuCore.CdvdVerboseReads=false`

## What to tell me

Which scene IDs are done, the S08 description, and anything odd (crashes,
missing scenes). The numbers are in `observations.md` and the dumps are on
disk, so nothing needs sending.

## Why the steps are like this

- **Several dumps per scene:** one multi-frame dump (≈125–150 frames) gives the
  distribution for p95. Single-frame dumps add the moments you judge
  heaviest, which become the max.
- **The EE sweep is a coarse proxy.** It scales emulated EE cycles, and
  this game runs with `EETimingHack`, so timing-sensitive code may react in
  ways that aren't pure CPU load. It gives a loose upper bound on EE
  utilisation with wide error bars. Phase 3 replaces it with instruction
  counts from the recompiled prototype.
- **Disk:** a scene's dumps take roughly 0.5–2 GB. D: has ~85 GB free and C:
  is not used.
