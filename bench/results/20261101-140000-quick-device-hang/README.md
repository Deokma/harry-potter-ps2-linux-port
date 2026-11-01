# Quick run that took the device down (2026-11-01, RG40XX H, Knulli)

`BENCH_QUICK=1 scripts/run_remote.sh`, second quick pass. The log stops in
T1, during the `stream_*` variants, then the device stopped answering ping
and SSH (`run_remote_partial.log`).

What it shows, before the fix:

- T1 `stream_*` (one 64-byte `glBufferSubData` per draw into a 4 MB VBO
  ring the GPU was still reading): **~21 ms per draw**, 18.4 s per frame
  at 877 draws. Client arrays on the same run: 6–11 µs per draw.
- Likely cause: the Mali-G31 r20p0 driver protects in-flight buffer data
  by copying or reallocating the whole buffer on each sub-update, which on
  a 1 GB device also explains the device becoming unresponsive. Not
  confirmed from the device side (no kernel log after the reboot).

Consequence: T1's stream path now uploads the frame's vertices in one
orphaning `glBufferData` and draws at offsets. A renderer for this driver
must never sub-update a buffer per draw.

The numbers in this log come from quick mode and are not results.
