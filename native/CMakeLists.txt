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
  core/src/leveling.cpp core/src/protocols.cpp core/src/relief.cpp core/src/rotary.cpp core/src/router_entry.cpp
  core/src/simple_line.cpp core/src/stock.cpp core/src/tabs.cpp core/src/tool_changes.cpp core/src/fixture_collision.cpp core/src/stl_heightmap.cpp
  core/src/artwork_relief.cpp core/src/program_import.cpp core/src/dxf.cpp
  core/src/pdf_import.cpp core/src/inflate.cpp core/src/bundle_import.cpp core/src/python_text.cpp
  core/src/surface.cpp)
# The core's own JSON reader and writer.
target_sources(kerfkite_core PRIVATE core/src/json.cpp)
# Circuit board import: Gerber layers and Excellon drill files.
target_sources(kerfkite_core PRIVATE core/src/excellon.cpp core/src/gerber.cpp core/src/pcb.cpp)
# Isolation toolpaths from a copper layer, on the polygon kernel below.
target_sources(kerfkite_core PRIVATE core/src/isolation.cpp)
# KerfKite's own solid modelling kernel (docs/SOLID-KERNEL.md): the C++ standard library only. solid_command.cpp is its JSON front.
target_sources(kerfkite_core PRIVATE core/src/solid/arrangement.cpp core/src/solid/boolean.cpp core/src/solid/exact.cpp core/src/solid/hull.cpp
  core/src/solid/mesh.cpp core/src/solid/planar.cpp core/src/solid/primitives.cpp core/src/solid/profile.cpp core/src/solid/query.cpp
  core/src/solid/section.cpp core/src/solid/simplify.cpp core/src/solid/stl.cpp core/src/solid_command.cpp core/src/solid_cad.cpp core/src/scad.cpp core/src/assembly.cpp)
target_include_directories(kerfkite_core PUBLIC core/include PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
if(MSVC)
  # Sources are UTF-8. Floating point stays strict so results match the reference on every platform.
  target_compile_options(kerfkite_core PUBLIC /utf-8 /fp:precise /W3)
else()
  target_compile_options(kerfkite_core PUBLIC -ffp-contract=off -Wall -Wextra)
endif()

# KerfKite's own polygon kernel, and the offset, pocket and fill operations, the helical pocket entry and
# V-carving built on it: standard library only. The core contains no third-party code and its build fetches
# nothing.
target_sources(kerfkite_core PRIVATE core/src/polygon/arrangement.cpp core/src/polygon/boolean.cpp core/src/polygon/clip.cpp
  core/src/polygon/command.cpp core/src/polygon/distance.cpp core/src/polygon/measure.cpp core/src/polygon/offset.cpp
  core/src/polygon/voronoi.cpp core/src/operations.cpp core/src/pocket_entry.cpp core/src/vcarve.cpp)

# Work shared out over threads the same way on every machine (core/include/kerfkite/parallel.hpp), and the
# optional use of a GPU through the system's OpenCL library, loaded when first needed (core/include/kerfkite/gpu.hpp).
# Nothing of OpenCL is compiled in or fetched; without the library everything runs on the CPU.
find_package(Threads REQUIRED)
target_sources(kerfkite_core PRIVATE core/src/gpu/opencl.cpp)
target_link_libraries(kerfkite_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

add_executable(kerfkite-cam tools/kerfkite_cam.cpp)
target_link_libraries(kerfkite-cam PRIVATE kerfkite_core)

# KerfKite Studio's workstation server (server/): the editor in studio/static served with every route of the
# Studio API over the core, KerfKite's own HTTP/1.1 server, the controller's serial link and the phone pendant.
# The C++ standard library and the operating system only; it fetches nothing.
set(KERFKITE_SERVER_SOURCES server/child_process.cpp server/design_extra.cpp server/design_routes.cpp server/device.cpp server/http.cpp
  server/key_order.cpp server/machine.cpp server/machine_routes.cpp server/machine_setup.cpp server/os_crypto.cpp server/os_random.cpp
  server/phone_jobs.cpp server/phone_monitor.cpp server/protocols.cpp server/serial_port.cpp server/sqlite_reader.cpp
  server/static_files.cpp server/step_exchange.cpp server/studio.cpp server/web_app.cpp server/web_store.cpp server/web_transfer.cpp
  server/browser.cpp server/mcp.cpp server/mcp_tools.cpp server/remote_tools.cpp server/workshop.cpp)
add_library(kerfkite_server STATIC ${KERFKITE_SERVER_SOURCES})
target_include_directories(kerfkite_server PUBLIC server PRIVATE core/src ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(kerfkite_server PUBLIC kerfkite_core)
# The hosted editor's hashes come from the system: CNG (bcrypt.dll) on Windows, CommonCrypto (in libSystem) on
# macOS, and elsewhere OpenSSL's libcrypto, opened when first needed (hence the dynamic loader).
if(WIN32)
  target_link_libraries(kerfkite_server PUBLIC ws2_32 bcrypt setupapi)
elseif(NOT APPLE)
  target_link_libraries(kerfkite_server PUBLIC ${CMAKE_DL_LIBS})
endif()
add_executable(kerfkite-studio server/main.cpp)
target_include_directories(kerfkite-studio PRIVATE core/src ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(kerfkite-studio PRIVATE kerfkite_server)
# The hosted editor and upload-only machine inbox (studio/web_server.py without Python); no controller.
add_executable(kerfkite-web server/web_main.cpp)
target_include_directories(kerfkite-web PRIVATE core/src ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(kerfkite-web PRIVATE kerfkite_server)
# The assistant server (studio/mcp_server.py without Python): the Model Context Protocol over standard input and
# output or HTTP. It reports Studio's version, read from the Python package until there is none.
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/../studio/__init__.py KERFKITE_VERSION_LINE REGEX "^__version__ = ")
string(REGEX REPLACE "^__version__ = [\"']([^\"']+)[\"'].*$" "\\1" KERFKITE_VERSION "${KERFKITE_VERSION_LINE}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../studio/__init__.py)
add_executable(kerfkite-mcp server/mcp_main.cpp)
target_include_directories(kerfkite-mcp PRIVATE core/src ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_definitions(kerfkite-mcp PRIVATE KERFKITE_VERSION="${KERFKITE_VERSION}")
target_link_libraries(kerfkite-mcp PRIVATE kerfkite_server)
# The phone pendant's TLS relay (../packaging/pendant-tls) is Go's standard library and nothing else: built
# beside kerfkite-studio when Go is installed, without fetching anything. Without it pairing is HTTP only.
find_program(KERFKITE_GO go)
if(KERFKITE_GO)
  add_custom_target(kerfkite-pendant-tls ALL
    COMMAND ${CMAKE_COMMAND} -E env CGO_ENABLED=0 GO111MODULE=off
      ${KERFKITE_GO} build -trimpath -buildvcs=false -o $<TARGET_FILE_DIR:kerfkite-studio>/kerfkite-pendant-tls${CMAKE_EXECUTABLE_SUFFIX} .
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../packaging/pendant-tls
    COMMENT "Building the phone pendant's TLS relay with Go"
    VERBATIM)
  add_dependencies(kerfkite-pendant-tls kerfkite-studio)
endif()
# STEP exchange through Open CASCADE (LGPL 2.1 with the OCCT exception): a program of its own, kerfkite-step, linked
# to Open CASCADE's shared libraries so that they stay files that can be replaced. Built when Open CASCADE is found
# (-DOpenCASCADE_DIR=<its lib/cmake/opencascade>); without it the server answers that STEP needs Open CASCADE.
find_package(OpenCASCADE CONFIG QUIET NO_CMAKE_PACKAGE_REGISTRY NO_CMAKE_SYSTEM_PACKAGE_REGISTRY)
if(OpenCASCADE_FOUND)
  if(TARGET TKDESTEP)
    set(KERFKITE_OCCT_STEP TKDESTEP)  # 7.8 and later
  else()
    set(KERFKITE_OCCT_STEP TKSTEP TKSTEPBase TKSTEPAttr TKSTEP209 TKXSBase)
  endif()
  add_executable(kerfkite-step step/kerfkite_step.cpp)
  target_include_directories(kerfkite-step PRIVATE core/src ${OpenCASCADE_INCLUDE_DIR})
  target_link_libraries(kerfkite-step PRIVATE kerfkite_core ${KERFKITE_OCCT_STEP} TKBO TKPrim TKMesh TKShHealing TKTopAlgo TKGeomAlgo
    TKBRep TKGeomBase TKG3d TKG2d TKMath TKernel)
  # Open CASCADE's libraries where they were built (CMake's build path), and once installed beside the add-on,
  # where a package puts them.
  if(APPLE)
    set_target_properties(kerfkite-step PROPERTIES INSTALL_RPATH "@loader_path")
  else()
    set_target_properties(kerfkite-step PROPERTIES INSTALL_RPATH "$ORIGIN")
  endif()
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # The search path also for the libraries' own dependencies on one another (DT_RPATH, not DT_RUNPATH).
    target_link_options(kerfkite-step PRIVATE "LINKER:--disable-new-dtags")
  endif()
  message(STATUS "kerfkite-step: STEP exchange with Open CASCADE ${OpenCASCADE_VERSION}")
endif()
# The programs without Python as a package takes them: cmake --install <build> --prefix <folder> puts the server,
# the pendant's relay and kerfkite-cam in the folder, and the STEP add-on with the Open CASCADE libraries it loads
# in step/ (packaging/desktop/build.py --native packages such folders).
install(TARGETS kerfkite-studio kerfkite-web kerfkite-mcp kerfkite-cam RUNTIME DESTINATION .)
if(KERFKITE_GO)
  install(PROGRAMS $<TARGET_FILE_DIR:kerfkite-studio>/kerfkite-pendant-tls${CMAKE_EXECUTABLE_SUFFIX} DESTINATION .)
endif()
if(TARGET kerfkite-step)
  if(CMAKE_VERSION VERSION_LESS 3.21)
    message(WARNING "CMake 3.21 or later installs the STEP add-on with its Open CASCADE libraries")
  else()
    install(TARGETS kerfkite-step
      RUNTIME_DEPENDENCIES PRE_INCLUDE_REGEXES "TK[A-Za-z0-9]+" PRE_EXCLUDE_REGEXES ".*"
        DIRECTORIES ${OpenCASCADE_BINARY_DIR} ${OpenCASCADE_LIBRARY_DIR}
      RUNTIME DESTINATION step LIBRARY DESTINATION step)
  endif()
endif()
add_executable(kerfkite_machine_selftest tests/machine_selftest.cpp)
target_link_libraries(kerfkite_machine_selftest PRIVATE kerfkite_server)
add_executable(kerfkite_server_selftest tests/server_selftest.cpp)
target_include_directories(kerfkite_server_selftest PRIVATE core/src ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(kerfkite_server_selftest PRIVATE kerfkite_server)

# Timings of the solid kernel on the models a Studio user builds, with a hash of each result. With
# -DKERFKITE_SOLID_PROFILE=ON the kernel also clocks its phases (core/src/solid/timing.hpp); the clocks
# only measure, and are left out of every other build.
option(KERFKITE_SOLID_PROFILE "Clock the phases of the solid kernel, for kerfkite_solid_bench" OFF)
if(KERFKITE_SOLID_PROFILE)
  target_compile_definitions(kerfkite_core PUBLIC KERFKITE_SOLID_PROFILE)
endif()
add_executable(kerfkite_solid_bench tools/solid_bench.cpp)
target_link_libraries(kerfkite_solid_bench PRIVATE kerfkite_core)
# Two builds of the solid kernel compared as solids: one records the operations of generated models, the
# other does them again, and either says whether the results are the same sets of points.
add_executable(kerfkite_solid_shapes tools/solid_shapes.cpp)
target_link_libraries(kerfkite_solid_shapes PRIVATE kerfkite_core)

enable_testing()
add_executable(kerfkite_selftest tests/selftest.cpp)
target_link_libraries(kerfkite_selftest PRIVATE kerfkite_core)
add_test(NAME selftest COMMAND kerfkite_selftest)
# The controller transports against scripted firmware on a virtual clock.
add_test(NAME machine COMMAND kerfkite_machine_selftest)
# The workstation server against the editor's files: a simulator and a scripted controller, the loopback only;
# with the pendant's TLS relay and the STEP add-on where they are built.
set(KERFKITE_SERVER_TEST ${CMAKE_CURRENT_SOURCE_DIR}/../studio/static)
if(KERFKITE_GO)
  list(APPEND KERFKITE_SERVER_TEST $<TARGET_FILE_DIR:kerfkite-studio>/kerfkite-pendant-tls${CMAKE_EXECUTABLE_SUFFIX})
else()
  list(APPEND KERFKITE_SERVER_TEST -)
endif()
if(TARGET kerfkite-step)
  list(APPEND KERFKITE_SERVER_TEST $<TARGET_FILE:kerfkite-step>)
endif()
add_test(NAME server COMMAND kerfkite_server_selftest ${KERFKITE_SERVER_TEST})
# The hosted editor: its store (and the move of the Python's web.sqlite3), its routes, and with the relay, a file
# sent to a receiver.
add_executable(kerfkite_web_selftest tests/web_selftest.cpp)
target_include_directories(kerfkite_web_selftest PRIVATE core/src ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(kerfkite_web_selftest PRIVATE kerfkite_server)
set(KERFKITE_WEB_TEST ${CMAKE_CURRENT_SOURCE_DIR}/../studio/static ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/web-store)
if(KERFKITE_GO)
  list(APPEND KERFKITE_WEB_TEST $<TARGET_FILE_DIR:kerfkite-studio>/kerfkite-pendant-tls${CMAKE_EXECUTABLE_SUFFIX})
endif()
add_test(NAME web COMMAND kerfkite_web_selftest ${KERFKITE_WEB_TEST})
add_executable(kerfkite_json_selftest tests/json_selftest.cpp)
target_link_libraries(kerfkite_json_selftest PRIVATE kerfkite_core)
add_test(NAME json COMMAND kerfkite_json_selftest)
add_executable(kerfkite_solid_selftest tests/solid_selftest.cpp)
target_link_libraries(kerfkite_solid_selftest PRIVATE kerfkite_core)
add_test(NAME solid COMMAND kerfkite_solid_selftest)
# Generated Boolean cases, with a digest of every result. The digest is recorded, so each platform and
# compiler the checks run on has to produce the same solids to the last vertex.
add_test(NAME solid_same_everywhere COMMAND kerfkite_solid_selftest --digest 150 2026)
set_tests_properties(solid_same_everywhere PROPERTIES PASS_REGULAR_EXPRESSION "0 failed.*digest e150789607e7145b")
add_executable(kerfkite_parallel_selftest tests/parallel_selftest.cpp)
target_link_libraries(kerfkite_parallel_selftest PRIVATE kerfkite_core)
add_test(NAME parallel COMMAND kerfkite_parallel_selftest)
add_test(NAME gpu_off COMMAND kerfkite_parallel_selftest --gpu-off)
set_tests_properties(gpu_off PROPERTIES ENVIRONMENT KERFKITE_GPU=off)
add_executable(kerfkite_polygon_selftest tests/polygon_selftest.cpp)
target_link_libraries(kerfkite_polygon_selftest PRIVATE kerfkite_core)
add_test(NAME polygon COMMAND kerfkite_polygon_selftest)
# The same check with the kernel's own 128-bit integer in place of the compiler's, which is what a compiler
# without one (MSVC) uses. Both must give the recorded digest.
add_executable(kerfkite_polygon_selftest_portable tests/polygon_selftest.cpp core/src/polygon/arrangement.cpp
  core/src/polygon/boolean.cpp core/src/polygon/clip.cpp core/src/polygon/distance.cpp core/src/polygon/measure.cpp
  core/src/polygon/offset.cpp core/src/polygon/voronoi.cpp)
target_compile_definitions(kerfkite_polygon_selftest_portable PRIVATE KERFKITE_POLYGON_PORTABLE_INT128)
target_include_directories(kerfkite_polygon_selftest_portable PRIVATE core/include)
target_compile_options(kerfkite_polygon_selftest_portable PRIVATE $<TARGET_PROPERTY:kerfkite_core,INTERFACE_COMPILE_OPTIONS>)
add_test(NAME polygon_portable_int128 COMMAND kerfkite_polygon_selftest_portable)
