# Cross toolchain for the RG35XX (H700, 4x Cortex-A53): Ubuntu's
# gcc-aarch64-linux-gnu. bench_cpu links statically; bench_gl links only
# against libc / libdl / libpthread and dlopens SDL2 on the device.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)

set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# ctest runs the self-test through qemu-user. Its timings mean nothing.
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-aarch64)
