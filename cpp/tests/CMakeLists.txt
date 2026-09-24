cmake_minimum_required(VERSION 3.14)
project(NitroPdfWriterTests LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(MODULE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(LIBHARU_DIR ${MODULE_ROOT}/libharu)
set(LIBPNG_DIR ${MODULE_ROOT}/libpng)

# Ensure libharu and libpng are downloaded.
if(NOT EXISTS ${LIBHARU_DIR}/include/hpdf.h OR NOT EXISTS ${LIBHARU_DIR}/include/hpdf_config.h OR NOT EXISTS ${LIBPNG_DIR}/png.h)
  execute_process(
    COMMAND node ${MODULE_ROOT}/scripts/prepare-libharu.js
    WORKING_DIRECTORY ${MODULE_ROOT}
    RESULT_VARIABLE PREPARE_RESULT
  )
  if(NOT PREPARE_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to prepare native dependencies for tests. Run 'node scripts/prepare-libharu.js' manually.")
  endif()
endif()

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
  test_font_prep.cpp
  test_load_attach.cpp
  test_libharu_smoke.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../MediaCache.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../MediaPrep.cpp
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
