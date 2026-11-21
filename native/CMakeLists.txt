cmake_minimum_required(VERSION 3.16)
project(kerfkite_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# The controller table has one source, shared with the editor.
set(KERFKITE_PROTOCOLS ${CMAKE_CURRENT_SOURCE_DIR}/../studio/static/protocols.json)
file(READ ${KERFKITE_PROTOCOLS} KERFKITE_PROTOCOLS_JSON)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${KERFKITE_PROTOCOLS})
configure_file(core/src/protocols_data.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/protocols_data.hpp @ONLY)

add_library(kerfkite_core STATIC core/src/api.cpp core/src/cam.cpp core/src/drilling.cpp core/src/knife.cpp core/src/laser.cpp
  core/src/leveling.cpp core/src/protocols.cpp core/src/router_entry.cpp core/src/simple_line.cpp core/src/tabs.cpp)
# The core's own JSON reader and writer.
target_sources(kerfkite_core PRIVATE core/src/json.cpp)
# Circuit board import: Gerber layers and Excellon drill files.
target_sources(kerfkite_core PRIVATE core/src/excellon.cpp core/src/gerber.cpp core/src/pcb.cpp)
target_include_directories(kerfkite_core PUBLIC core/include PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
if(MSVC)
  # Sources are UTF-8. Floating point stays strict so results match the reference on every platform.
  target_compile_options(kerfkite_core PUBLIC /utf-8 /fp:precise /W3)
else()
  target_compile_options(kerfkite_core PUBLIC -ffp-contract=off -Wall -Wextra)
endif()

# KerfKite's own polygon kernel, and the offset, pocket and fill operations built on it: standard library
# only. The core contains no third-party code and its build fetches nothing.
target_sources(kerfkite_core PRIVATE core/src/polygon/arrangement.cpp core/src/polygon/boolean.cpp core/src/polygon/clip.cpp
  core/src/polygon/command.cpp core/src/polygon/measure.cpp core/src/polygon/offset.cpp core/src/operations.cpp)

add_executable(kerfkite-cam tools/kerfkite_cam.cpp)
target_link_libraries(kerfkite-cam PRIVATE kerfkite_core)

enable_testing()
add_executable(kerfkite_selftest tests/selftest.cpp)
target_link_libraries(kerfkite_selftest PRIVATE kerfkite_core)
add_test(NAME selftest COMMAND kerfkite_selftest)
add_executable(kerfkite_json_selftest tests/json_selftest.cpp)
target_link_libraries(kerfkite_json_selftest PRIVATE kerfkite_core)
add_test(NAME json COMMAND kerfkite_json_selftest)
add_executable(kerfkite_polygon_selftest tests/polygon_selftest.cpp)
target_link_libraries(kerfkite_polygon_selftest PRIVATE kerfkite_core)
add_test(NAME polygon COMMAND kerfkite_polygon_selftest)
# The same check with the kernel's own 128-bit integer in place of the compiler's, which is what a compiler
# without one (MSVC) uses. Both must give the recorded digest.
add_executable(kerfkite_polygon_selftest_portable tests/polygon_selftest.cpp core/src/polygon/arrangement.cpp
  core/src/polygon/boolean.cpp core/src/polygon/clip.cpp core/src/polygon/measure.cpp core/src/polygon/offset.cpp)
target_compile_definitions(kerfkite_polygon_selftest_portable PRIVATE KERFKITE_POLYGON_PORTABLE_INT128)
target_include_directories(kerfkite_polygon_selftest_portable PRIVATE core/include)
target_compile_options(kerfkite_polygon_selftest_portable PRIVATE $<TARGET_PROPERTY:kerfkite_core,INTERFACE_COMPILE_OPTIONS>)
add_test(NAME polygon_portable_int128 COMMAND kerfkite_polygon_selftest_portable)
