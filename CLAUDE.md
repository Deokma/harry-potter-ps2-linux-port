# PS2 port: Harry Potter and the Philosopher's Stone → Anbernic H700

Native port by static recompilation of the R5900 code plus a runtime (forked
from PS2Recomp, GPL-3.0). Primary disc NTSC SLUS-20826, secondary PAL
SLES-52055. Target: RG40XX H (Knulli), Cortex-A53 at 1.416 GHz, Mali-G31.

**Start every session by reading `NOTES.md` → "Current state and handoff".**
Phase results are in `docs/PHASE_N.md`; the current work is `docs/PHASE_4.md`
(title-screen slice). Code derived from PS2Recomp is listed in
`docs/PROVENANCE.md`; update it when that changes.

## Hard rules
- Never download game files or BIOS images. Never commit ISO / ELF /
  extracted assets / BIOS / savestates / generated code / recordings: they
  live in `private/` (git-ignored) or `D:\ps2port-data\` (large data; C: is
  low on space).
- Don't assume an existing project works; check it.
- Be honest about feasibility. Never claim something runs on the device
  unless it ran there; PC (and qemu-user) timings are not device timings.
- Stop rule (user, 2026-11-07, Phase 7; replaces the 2026-11-06 rule (a)):
  judge the measured gameplay frame after a step by this table, not by the
  budget alone: <=50 ms continue; 50-55 ms report and ask the user; >55 ms stop
  and report options. Being over budget is not by itself a reason to stop. Also
  stop if (b) a decision from the user is needed, or (c) the device misbehaves
  (reboot, over 85 C). Otherwise continue to the next step without asking.
  Record results in `docs/PHASE_N.md`. Don't push.
- Keep `NOTES.md` updated (decisions, open questions, commands that worked).
  Small commits, clear messages; scripts in `scripts/` over manual steps.
- Never type passwords. Device SSH is key-based only (port 2222); after a
  device reboot the user restarts it (command in NOTES).
- No screen control (user, 2026-11-06): no computer use, no driving the PCSX2
  window, no SendKeys/SendInput. Use the saved states and GS dumps in
  `D:\ps2port-data\`; if a new capture is needed, ask the user.
- Token economy: short tool output (grep/head/summaries), never print large
  logs, commit + update NOTES before long steps, batch fixes to keep
  rebuilds few.
- User-facing steps: short numbered lists, one copy-paste command each.

## Layout
- `eegen/` — our R5900 → C++ generator (`eegen.py`, `eegen_rt.h`,
  `eegen_fp.h`). Defaults follow PS2Recomp's semantics so results can be
  checked byte for byte; `--hardware-semantics` switches.
- `scripts/` — ELF analysis, function map, replay states, device runs,
  profiling, RAM diffs.
- `bench/` — device microbenchmarks.
- `gsgles/` — GS dump player and the GLES backend (`gs_gles_backend.*`); `sh gsgles/build.sh cpu|gles|gles-aarch64`.
- `vu1/` — VU1 replay and comparison (`vu1play`, `sh vu1/build.sh x86|aarch64`); the generator is `eegen/vu1gen.py`.
- `runner/` — the whole game: PS2Recomp's runtime with eegen's code and translated VU0, VU1 (`sh runner/gen.sh`, `sh runner/build.sh x86|aarch64`, `sh runner/try.sh SECONDS PAD`).
- `harness/` — replay harness sources (`build.sh`, pipelines, `main.cpp`, `shim.cpp`).
- `private/third_party/` — PS2Recomp checkout + trial patch, its output
  (`trial/out-b/`), the harness's generated code and builds (`trial/harness/`), recordings
  (`trial/rec/`).

## Environment and traps
- Windows host; build in WSL (`MSYS_NO_PATHCONV=1 wsl -e bash -c '...'`),
  device runs from Git Bash (`scripts/harness_device_run.sh`, clock pinned
  to 1416000 kHz). WSL `/tmp` is cleared between calls.
- Avoid `\n` escapes in heredoc-embedded Python (they become real newlines);
  use the Edit tool. Run scripts from a copy if they may be edited mid-run.
- Generated code must be built with `-ffp-contract=off`.
- PS2Recomp's 127 native stubs must stay native (`eegen --native`); it emits
  nothing for a delay-slot `nop` (count convention).
- Validate every generator change on x86 with `HARNESS_SKIP=0x2cd6a0
  HARNESS_STOP_TIMER=2 HARNESS_VU0=3` (RDRAM byte-identical to
  `rec/frame-ntsc02r.out`, exactly 2,861,449 guest instructions for
  `frame-ntsc02r`) before timing on the device.
