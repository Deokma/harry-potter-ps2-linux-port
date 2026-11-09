# Study: what still runs interpreted, and build-time optimisations (2026-11-09, study only)

Profile: `panel-124408` (device, 1.416 GHz, scripted path to the shop, samples 230-290 s, 5,992 samples, the 2026-11-08
15:46 build; local data `private/therm/prof1/`). Frame 44.8 ms p50 in the shop (measured). Thread loads from the runner's
`[m5]` line: game thread 90 %, GsThread (stage A) 83 %, GsDraw (stage B) 36 %, main/GL 23 %, IopThread 18 % of a core.

## 1. What is not static native code at run time (measured unless marked)

| Part | How it runs | Share (measured) | Note |
|---|---|---|---|
| EE functions | 5,579 of 5,708 translated by eegen; 128 are PS2Recomp's native stand-ins (kernel, card, libmpeg, libc); **1 refused** (MMI) | eegen code 46.1 % of the game thread | `build-m2/gen/eg/eegen_report.txt` |
| PS2Recomp's generated code (`FUN_`) | statically compiled, but used whenever the game resumes at a call's return address (eegen enters only at function starts and 3,206 loop heads) | **9.7 % of the game thread**, `FUN_002c4fe0` alone 5.5 % | not interpreted, but the slower of the two translations |
| EE runtime around guest code | scheduler checks, address translation, IO registers, `hasFunction` | scheduler/runtime 11.3 %, PS2Memory 8.9 %, libc copies 9.7 % of the game thread | |
| IOP (7 IRX modules: SIO2MAN, PADMAN, MCMAN, MCSERV, LIBSD, SDRDRV, STREAM) | **interpreted** (`IopCpuCore::executeInstruction`, `IopEmulator::step`, import decode) | IopThread 18 % of a core: interpreter about 55-77 %, SPU2 mixing (`spuAdvance`) 15 %, hash-map lookups 10 %; plus about 1-2 % of the game thread (catching the IOP up) | |
| VU1 | translated (`egVu1Run`) | **60 % of stage A**, the limiting thread | VU interpreter 0.5 % of stage A |
| VU0 | translated, 13 of 18 entries (all that run); the other 5 would fall back to the interpreter | not visible in the profile | |
| VIF1 | runtime C++ (fast UNPACK path) | `processVIF1Data` 14.7 % + unpack 4.4 % of stage A | |
| Copies (`memcpy` in the device's glibc) | libc | 12.3 % of stage A, 9.7 % of the game thread | symbol shows as `__xpg_strerror_r` (stripped libc; nearest symbol) |

So almost nothing is interpreted on the frame's critical path: the IOP is, but it runs on its own thread at 18 %.
The frame is limited by stage A (VU1 and VIF) and the game thread (eegen code plus runtime overhead).

## 2. Build-time optimisations not used yet (estimates)

Today: GCC cross build, `-O2` runtime, `-O3` own code and VU1, `-march=armv8-a+... -mtune=cortex-a53`, `-ffp-contract=off`,
no LTO, no PGO, no function ordering, static libstdc++, the device's glibc for `memcpy`.

| Option | Where it acts | Expected (estimate) | Risk / cost |
|---|---|---|---|
| **A. PGO from recorded gameplay** (`-fprofile-generate` build run with scripted pads over menu, cutscenes, shop; `-fprofile-use`) | all of eegen's code, translated VU1, the runtime: branch layout, inlining, hot/cold splitting | **5-15 % on the game thread and stage A** (2-6 ms each) | no semantic change (byte-exact tests stay valid); needs a device run to collect `.gcda`; one extra build per release |
| B. Function and block ordering for the i-cache (`-ffunction-sections` + a link order from the sampler, or PGO's `-freorder-functions`) | 5,579 guest functions in an 88 MB binary on a 32 KB L1i | 0-5 % (unmeasured: needs PMU i-cache miss counts) | low; comes almost free with A |
| C. LTO (`-flto=auto`, partitioned) | runtime helpers into guest code (scheduler, `translateAddress`, IO) | 0-5 % (always_inline of PS2Recomp's helpers gave nothing, PHASE_4) | long links for ~5,600 objects; memory on the build host |
| D. A53-specific tuning (`-mcpu=cortex-a53`) | scheduling already tuned | ~0 % | none |
| **E. Static translation (or HLE) of the IOP modules** | IopThread's interpreter, the IOP lock, catch-up on the game thread | IopThread 18 % -> ~5 % of a core (heat), 1-2 % off the game thread | an R3000 -> C++ pass like eegen (the IRXs total ~200 KB); STREAM.IRX is custom; HLE needs its protocol |
| F. A faster `memcpy` linked in (arm-optimized-routines, tuned for A53) | the copies: 12.3 % of stage A, 9.7 % of the game thread | 1-3 ms on stage A if copies speed up 1.3-1.5x | low; measure the device glibc's version first |
| G. eegen resume entries at call return addresses (static translation, not a flag) | the 9.7 % PS2Recomp share on the game thread | 2-3 ms of the game thread | eegen change; harness checks exist |

**The two most likely to pay off: A (PGO) and E (IOP statically translated).** A acts on both limiting threads at once
with no change in semantics. E takes most of a core's 18 % away, which matters for heat (the session limit) more than for
the frame. F and G are cheap follow-ups with measured shares behind them.

## 3. One command: disc image in, device package out (sketch)

```
sh scripts/port.sh DISC.iso OUT/            # proposed; every step exists today as a separate script
 1. extract   scripts/extract_disc.py DISC -> private/extract/<id>/files (ELF, IRX, RESOURCE, MOVIES)
 2. map       PS2Recomp analyzer (trial/analyzer.toml) + export_function_map.py -> functions, native stubs
 3. ps2recomp ps2xRecomp -> trial/out-b; scripts/ps2recomp_gen_fixes.py (holes in the map); movie stub if wanted
 4. vu        micro images for VU0/VU1 -> vu0gen / vu1gen inputs
 5. gen       sh runner/gen.sh (eegen --borrow, vu0gen, vu1gen) -> build-m2/gen
 6. build     sh runner/build.sh aarch64 (FFmpeg if present) -> runner; strip -> runner-aarch64
 7. package   runner-aarch64 + the disc's files + a Knulli Ports launcher (/userdata/roms/ports/HP1.sh) + empty mc0/ -> OUT/hp1-port.tar
 8. check     x86 harness generator check (RDRAM byte-identical, exact instruction count) before packaging
```

Gaps before this is truly "disc in": step 4 takes the VU micro images from savestates today (`private/vu1/slot2/micro.bin`),
not from the disc (the MPG uploads would have to be found in the game's data); step 2's map still needs holes found at run
time (`0x28a140` was found today; a scan of function-pointer tables in data, `scripts/scan_code_in_data.py`, could list
them ahead of time); and the package holds the user's own disc files, so it must never leave their machine.
