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
# Circuit board import: Gerber layers and Excellon drill files.
target_sources(kerfkite_core PRIVATE core/src/excellon.cpp core/src/gerber.cpp core/src/pcb.cpp)
target_include_directories(kerfkite_core PUBLIC core/include third_party PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
if(MSVC)
  # Sources are UTF-8. Floating point stays strict so results match the reference on every platform.
  target_compile_options(kerfkite_core PUBLIC /utf-8 /fp:precise /W3)
else()
  target_compile_options(kerfkite_core PUBLIC -ffp-contract=off -Wall -Wextra)
endif()

add_executable(kerfkite-cam tools/kerfkite_cam.cpp)
target_link_libraries(kerfkite-cam PRIVATE kerfkite_core)

enable_testing()
add_executable(kerfkite_selftest tests/selftest.cpp)
target_link_libraries(kerfkite_selftest PRIVATE kerfkite_core)
add_test(NAME selftest COMMAND kerfkite_selftest)
