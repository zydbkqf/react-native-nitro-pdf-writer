cmake_minimum_required(VERSION 3.14)
project(NitroPdfWriterTests LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(MODULE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(LIBHARU_DIR ${MODULE_ROOT}/thirdparty/libharu)
set(LIBPNG_DIR ${MODULE_ROOT}/thirdparty/libpng)

include(FetchContent)
FetchContent_Declare(
  doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG v2.4.11
)
FetchContent_MakeAvailable(doctest)

file(GLOB LIBHARU_SOURCES ${LIBHARU_DIR}/src/*.c)
file(GLOB LIBPNG_SOURCES ${LIBPNG_DIR}/*.c)
list(FILTER LIBPNG_SOURCES EXCLUDE REGEX "(pngtest|example)\\.c$")

add_executable(pdfwriter_tests
  test_main.cpp
  test_handle_registry.cpp
  test_haru_lock.cpp
  test_media_cache.cpp
  test_media_limits.cpp
  test_font_prep.cpp
  test_load_attach.cpp
  test_libharu_smoke.cpp
  test_image_formats.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../MediaCache.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../MediaPrep.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../PlatformImageTranscoder.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../FontPrep.cpp
  ${LIBHARU_SOURCES}
  ${LIBPNG_SOURCES}
)

target_include_directories(pdfwriter_tests PRIVATE
  ..
  ${LIBHARU_DIR}/include
  ${LIBPNG_DIR}
)

target_compile_definitions(pdfwriter_tests PRIVATE PNG_ARM_NEON_OPT=0)

target_link_libraries(pdfwriter_tests PRIVATE doctest::doctest z)

enable_testing()
add_test(NAME pdfwriter_tests COMMAND pdfwriter_tests)
