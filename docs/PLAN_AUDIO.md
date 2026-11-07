# Plan: audio

Plan only; nothing here is implemented. Phase 7, item 4b.

## What the game uses (from the boot logs and the IOP traces)

- **The real IRX modules run on the emulated IOP:** `LIBSD.IRX` (Sony's sound library), `SDRDRV.IRX` (its RPC driver) and `STREAM.IRX` (EA's MultiStream V7) for streamed audio: the cutscenes' dialogue and music, read from disc 16 sectors (32 KB) at a time and timed by the voices' positions.
- **Everything reaches the SPU2 as registers and DMA:** sample data by DMA channels 4 and 7 into SPU RAM at TSA, voices keyed on and off (KON/KOFF), their pitch, start and loop addresses, and the stream library watching NAX, ENVX and ENDX.
- **Today a silent model** (`iop_memory.cpp`, on the IOP's clock since step 2) keeps SPU RAM and moves each voice through its 16-byte ADPCM blocks at its pitch, following the loop flags, so that what the game reads is true. Nothing is decoded or mixed. No envelopes (a voice is full volume until it stops), no reverb, no SPU interrupts.

## Minimum mixing for sound that is right

1. **ADPCM decode** per voice (28 samples a block, two filter coefficients, shift, the standard PS1/PS2 table).
2. **Pitch:** step through the decoded samples at pitch/4096 of 48 kHz, with linear interpolation (the hardware's Gaussian filter later, if needed).
3. **ADSR envelopes** (attack, decay, sustain, release from ADSR1/ADSR2), and the per-voice volumes (VOLL/VOLR, fixed mode first; sweeps later).
4. **Mix** both cores' 24 voices to stereo, the master volume, core 0 into core 1 as the hardware chains them, out at 48 kHz.
5. **Later:** reverb (the SPU2's effects area: a large share of the cost and of the hardware's sound in a cave or a hall), noise, pitch modulation, IRQ at an address (only if the trace shows the game setting it).

## Cost estimate (estimate, not measured)

Per active voice and output sample: decode about 10 cycles (amortised over 28 samples), interpolation and envelope about 15, mix about 5: about 30 cycles. **At 48 kHz and 16 active voices, about 23 M cycles a second: about 1.6 % of one A53 at 1.416 GHz; with all 48 voices, about 5 %.** Reverb would add about as much again. The output path (SDL2 audio callback, 48 kHz, 1,024-sample buffers) is under 1 %.

## Where it runs

**Not on the EE thread nor on stage A** (the game thread is at 92 % and stage A at 95 % of its time in jobs). Two choices:
- **On the IOP thread** (about 15 % busy): the voice state is already there, kept in step with the IOP's clock, and the mixer can produce exactly the samples for each stretch of IOP time. The IOP thread then hands blocks of samples to the output through a lock-free ring.
- **On a thread of its own,** pinned with the IOP thread's core preferred. Cores are 66-73 % busy in the shop; a few percent fits either way.

**Recommended:** the IOP thread produces the samples (correct timing for free), the SDL2 callback only copies from the ring. The ring absorbs the IOP running slower than real time: the game runs at about 73 % speed in gameplay (Phase 7, item 3), so **sound would play slow and low unless the output rate follows the emulated clock**, which is the first thing to decide (resample to real time, or accept slow sound while the game is slow).

## Measurement plan before implementing

1. **Voice use:** `PS2PORT_SPU_TRACE` extended to count, per second, voices keyed on, the most at once, the pitches and the ADSR settings used, over the full path (menu, the three cutscenes, the shop). Decides the voice count to budget for.
2. **Reverb:** whether the game enables the effects area (register EEA/ESA and the reverb bits in ATTR); if it does, how large.
3. **A decoder prototype on the device:** decode and mix a recorded register-and-DMA trace of 60 s of the street cutscene, single-threaded, timed on one A53 core, against the estimate above.
4. **The IOP thread's headroom:** its CPU share in the street and the shop with the prototype mixing inside it, and that the game thread's frame does not move.
5. **Then the output:** SDL2's audio device on the RG40XX H (rate and buffer it grants), and the latency from a KON to sound.
