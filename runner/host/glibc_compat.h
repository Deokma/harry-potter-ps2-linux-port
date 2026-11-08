// The cross toolchain's glibc is newer than the device's (2.40). These two moved to a new symbol version in 2.43 with no change that
// matters here, and a binary asking for that version won't start on the device. Force-included in aarch64 builds (the runner, and
// FFmpeg's C files: scripts/ffmpeg_build.sh); skipped in assembly, which FFmpeg's configure also preprocesses with these flags.
#ifndef __ASSEMBLER__
__asm__(".symver atan2f,atan2f@GLIBC_2.17");
__asm__(".symver log10f,log10f@GLIBC_2.17");
#endif
