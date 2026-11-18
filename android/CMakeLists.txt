cmake_minimum_required(VERSION 3.22.1)
project("NitroPdfWriter")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -----------------------------------------------------------------------------
# Native dependencies are vendored under thirdparty/ and compiled from source.
# Nothing is downloaded at build time.
# -----------------------------------------------------------------------------
set(MODULE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(LIBHARU_DIR ${MODULE_ROOT}/thirdparty/libharu)
set(LIBPNG_DIR ${MODULE_ROOT}/thirdparty/libpng)

if(NOT EXISTS ${LIBHARU_DIR}/include/hpdf.h
   OR NOT EXISTS ${LIBHARU_DIR}/include/hpdf_config.h
   OR NOT EXISTS ${LIBPNG_DIR}/png.h
   OR NOT EXISTS ${LIBPNG_DIR}/pnglibconf.h)
  message(FATAL_ERROR "Vendored sources missing under thirdparty/. The package is incomplete; reinstall it instead of patching the working copy.")
endif()

# libpng is compiled from the vendored sources on every platform: Android here,
# iOS in the podspec and SwiftPM targets. No platform uses a system libpng.
add_library(png STATIC
  ${LIBPNG_DIR}/png.c
  ${LIBPNG_DIR}/pngerror.c
  ${LIBPNG_DIR}/pngget.c
  ${LIBPNG_DIR}/pngmem.c
  ${LIBPNG_DIR}/pngpread.c
  ${LIBPNG_DIR}/pngread.c
  ${LIBPNG_DIR}/pngrio.c
  ${LIBPNG_DIR}/pngrtran.c
  ${LIBPNG_DIR}/pngrutil.c
  ${LIBPNG_DIR}/pngset.c
  ${LIBPNG_DIR}/pngtrans.c
  ${LIBPNG_DIR}/pngwio.c
  ${LIBPNG_DIR}/pngwrite.c
  ${LIBPNG_DIR}/pngwtran.c
  ${LIBPNG_DIR}/pngwutil.c
)

target_include_directories(png PUBLIC ${LIBPNG_DIR})
target_compile_definitions(png PRIVATE PNG_ARM_NEON_OPT=0)

file(GLOB LIBHARU_SOURCES ${LIBHARU_DIR}/src/*.c)

add_library(
  NitroPdfWriter
  SHARED
  ../cpp/HaruWorker.cpp
  ../cpp/MediaWorker.cpp
  ../cpp/MediaCache.cpp
  ../cpp/MediaPrep.cpp
  ../cpp/PlatformImageTranscoder.cpp
  ../cpp/FontPrep.cpp
  AndroidImageTranscoder.cpp
  ../cpp/HybridNitroPdfWriter.cpp
  ../nitrogen/generated/shared/c++/HybridNitroPdfWriterSpec.cpp
  cpp-adapter.cpp
  ${LIBHARU_SOURCES}
)

target_include_directories(
  NitroPdfWriter
  PRIVATE
  ../cpp
  ../nitrogen/generated/shared/c++
  ${LIBHARU_DIR}/include
  ${LIBPNG_DIR}
)

target_link_libraries(NitroPdfWriter png z)

target_link_options(NitroPdfWriter PRIVATE -Wl,--unresolved-symbols=ignore-all)

include(${CMAKE_SOURCE_DIR}/../nitrogen/generated/android/NitroPdfWriter+autolinking.cmake)
