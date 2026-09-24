cmake_minimum_required(VERSION 3.22.1)
project("NitroPdfWriter")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -----------------------------------------------------------------------------
# Prepare libharu before building. This matches the behavior of the iOS podspec
# and ensures the C sources are available.
# -----------------------------------------------------------------------------
set(MODULE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(LIBHARU_DIR ${MODULE_ROOT}/libharu)
set(LIBPNG_DIR ${MODULE_ROOT}/libpng)

if(NOT EXISTS ${LIBHARU_DIR}/include/hpdf.h OR NOT EXISTS ${LIBPNG_DIR}/png.h)
  execute_process(
    COMMAND node ${MODULE_ROOT}/scripts/prepare-libharu.js
    WORKING_DIRECTORY ${MODULE_ROOT}
    RESULT_VARIABLE PREPARE_RESULT
  )
  if(NOT PREPARE_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to prepare native dependencies. Run 'node scripts/prepare-libharu.js' manually.")
  endif()
endif()

# Build libpng as a static library for Android. iOS uses the system libpng.
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
  ../cpp/FontPrep.cpp
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
