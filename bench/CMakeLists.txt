# Phase 2 device benchmarks (docs/PHASE_2_DEVICE_BENCH.md).
#   native:   cmake -S bench -B bench/build/native -G Ninja
#   aarch64:  cmake -S bench -B bench/build/aarch64 -G Ninja \
#               -DCMAKE_TOOLCHAIN_FILE=bench/cmake/aarch64-linux-gnu.cmake
# scripts/build_bench.sh does both and runs the self-tests.
cmake_minimum_required(VERSION 3.16)
project(ps2bench C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_C_FLAGS_RELEASE "-O2")

option(BENCH_STATIC_CPU "Link bench_cpu statically (runs on any aarch64 Linux)" ON)
option(BENCH_BUILD_GL "Build bench_gl (needs Khronos GLES headers on the build host)" ON)
set(KHR_INCLUDE_DIR "/usr/include" CACHE PATH "Host directory holding GLES2/, GLES3/ and KHR/")

# No fused multiply-add anywhere: guest FPU / VU semantics, and T7's
# bit-exact NEON-vs-scalar check, both depend on it.
add_compile_options(-Wall -Wextra -ffp-contract=off)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  add_compile_options(-march=armv8-a -mtune=cortex-a53)
endif()

execute_process(COMMAND git describe --always --dirty
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                OUTPUT_VARIABLE BENCH_GIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT BENCH_GIT)
  set(BENCH_GIT unknown)
endif()
add_compile_definitions(BENCH_GIT="${BENCH_GIT}" BENCH_TARGET="${CMAKE_SYSTEM_PROCESSOR}")

find_package(Threads REQUIRED)

add_library(benchcore STATIC
  src/common.c src/linux_gnu.c src/t5_cpu.c src/t6_ee.c src/t7_vu.c src/t8_sys.c)
target_link_libraries(benchcore PUBLIC Threads::Threads m)
# T6 changes the FPU rounding mode at run time
set_source_files_properties(src/t6_ee.c PROPERTIES COMPILE_OPTIONS "-frounding-math")

add_executable(bench_cpu src/main_cpu.c)
target_link_libraries(bench_cpu PRIVATE benchcore)
if(BENCH_STATIC_CPU)
  target_link_options(bench_cpu PRIVATE -static)
endif()

enable_testing()
add_test(NAME selftest COMMAND bench_cpu --selftest)

if(BENCH_BUILD_GL)
  set(KHR_STAGE "${CMAKE_BINARY_DIR}/khr")
  foreach(d GLES2 GLES3 KHR)
    if(NOT EXISTS "${KHR_INCLUDE_DIR}/${d}")
      message(FATAL_ERROR "${KHR_INCLUDE_DIR}/${d} is missing: install libgles-dev "
                          "(Khronos headers), or configure with -DBENCH_BUILD_GL=OFF")
    endif()
    # staged copy, so a cross build sees these headers without the rest of /usr/include
    file(COPY "${KHR_INCLUDE_DIR}/${d}" DESTINATION "${KHR_STAGE}")
  endforeach()
  add_executable(bench_gl
    src/main_gl.c src/sdl_dyn.c src/gl_dyn.c src/gl_util.c
    src/t1_draw.c src/t2_fill.c src/t3_rt.c src/t4_upload.c src/t5_gpu.c src/t8_audio.c)
  target_include_directories(bench_gl PRIVATE "${KHR_STAGE}")
  target_link_libraries(bench_gl PRIVATE benchcore ${CMAKE_DL_LIBS})
endif()

# Native builds: check the hand-written SDL2 declarations against real headers.
if(NOT CMAKE_CROSSCOMPILING)
  find_path(SDL2_HEADER_DIR SDL.h PATH_SUFFIXES SDL2)
  if(SDL2_HEADER_DIR)
    add_library(sdl_abi_check OBJECT src/sdl_check.c)
    target_include_directories(sdl_abi_check PRIVATE "${SDL2_HEADER_DIR}")
    if(TARGET bench_gl)
      add_dependencies(bench_gl sdl_abi_check)
    endif()
  else()
    message(WARNING "SDL2 headers not found: sdl_min.h ABI check skipped")
  endif()
endif()
