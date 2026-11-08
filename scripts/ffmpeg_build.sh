#!/bin/sh
# A minimal FFmpeg 7.1 (MPEG-2 video decoder, its parser, swscale) as static libraries, for the runner's movies: PS2Recomp's
# stand-in for the game's libmpeg (Kernel/Stubs/MPEG.cpp, PS2X_HAS_FFMPEG=1). In WSL, from the repo:
#   sh scripts/ffmpeg_build.sh x86|aarch64
# Source: https://ffmpeg.org/releases/ffmpeg-7.1.tar.xz (LGPL 2.1+), its signature checked with FFmpeg's release key
# (fingerprint FCF9 86EA 15E6 E293 A564 4F10 B432 2F04 D676 58D8), kept in ~/ps2port-ffmpeg/dl. Built in ~/ps2port-ffmpeg
# (WSL's own disk: fast); headers and libraries installed to private/third_party/ffmpeg-7.1-ARCH/. Static, so the device's own
# FFmpeg is not needed; aarch64 objects take runner/host/glibc_compat.h like the runner (the device's glibc is older).
set -eu
arch=$1
repo=$(cd "$(dirname "$0")/.." && pwd)
work=$HOME/ps2port-ffmpeg
src=$work/ffmpeg-7.1
[ -d "$src" ] || tar -C "$work" -xJf "$work/dl/ffmpeg-7.1.tar.xz"
build=$work/build-$arch
prefix=$repo/private/third_party/ffmpeg-7.1-$arch
mkdir -p "$build"
cd "$build"
set -- --prefix="$prefix" --disable-everything --disable-programs --disable-doc --disable-network --disable-autodetect \
    --disable-avdevice --disable-avformat --disable-avfilter --disable-swresample --disable-postproc \
    --enable-avcodec --enable-avutil --enable-swscale --enable-decoder=mpeg2video --enable-parser=mpegvideo \
    --enable-static --disable-shared --enable-pic
case $arch in
x86) "$src/configure" "$@" --disable-x86asm > configure.log 2>&1 || { tail -5 configure.log; exit 1; } ;;
aarch64)
    "$src/configure" "$@" --enable-cross-compile --arch=aarch64 --target-os=linux --cross-prefix=aarch64-linux-gnu- \
        --cpu=cortex-a53 --extra-cflags="-include $repo/runner/host/glibc_compat.h" > configure.log 2>&1 ||
        { tail -5 configure.log; exit 1; }
    ;;
*) echo "x86 or aarch64"; exit 2 ;;
esac
make -j"$(nproc)" > make.log 2>&1 || { tail -5 make.log; exit 1; }
make install > install.log 2>&1
ls "$prefix/lib"
