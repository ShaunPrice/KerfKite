# GEOS, the geometry library behind the polygon operations (offset, pocket and fill).
#
# The Python reference reaches GEOS through Shapely, and the core must give the same bytes, so the core
# builds the release that the pinned Shapely wheels contain: Shapely 2.2.0 carries GEOS 3.14.1. A GEOS of
# another version can place a vertex differently in the last bit, or order rings differently, so no other
# version is accepted, whether fetched or found on the system.
#
# Options
#   KERFKITE_WITH_GEOS     ON   Build the polygon operations. OFF builds the core without GEOS; the
#                               operations are then declined, as before.
#   KERFKITE_SYSTEM_GEOS   OFF  Use an installed GEOS instead of building one. It must be exactly the pinned
#                               version; point CMAKE_PREFIX_PATH at it.
#   KERFKITE_GEOS_SHARED   OFF  Build GEOS as shared libraries placed beside kerfkite-cam instead of linking
#                               it into the program. GEOS is LGPL-2.1; see docs/LICENSING.md.
#   KERFKITE_GEOS_URL           The archive to use instead of the two download addresses: a local path or
#                               a mirror of the release tarball. It must have the pinned SHA-256.
#
# Sets KERFKITE_GEOS_TARGET to the library to link, or leaves it empty when GEOS is off.

option(KERFKITE_WITH_GEOS "Build the polygon operations (offset, pocket, fill), which need GEOS" ON)
option(KERFKITE_SYSTEM_GEOS "Use an installed GEOS of exactly the pinned version instead of building it" OFF)
option(KERFKITE_GEOS_SHARED "Build GEOS as shared libraries beside the program instead of linking it in" OFF)
set(KERFKITE_GEOS_URL "" CACHE STRING "A local path or mirror for the pinned GEOS release tarball")

set(KERFKITE_GEOS_VERSION 3.14.1)
# The release tarball as published at download.osgeo.org and attached to the GitHub release: the two are
# the same file.
set(KERFKITE_GEOS_SHA256 3c20919cda9a505db07b5216baa980bacdaa0702da715b43f176fb07eff7e716)
set(KERFKITE_GEOS_TARGET "")

if(NOT KERFKITE_WITH_GEOS)
  message(STATUS "KerfKite: GEOS is off; offset, pocket and fill operations will be declined")
  return()
endif()

if(KERFKITE_SYSTEM_GEOS)
  find_package(GEOS ${KERFKITE_GEOS_VERSION} EXACT REQUIRED CONFIG)
  set(KERFKITE_GEOS_TARGET GEOS::geos_c)
  message(STATUS "KerfKite: using the installed GEOS ${GEOS_VERSION}")
  return()
endif()

if(KERFKITE_GEOS_SHARED)
  # The program looks for the libraries in its own directory, so the two can be moved together.
  set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
  if(APPLE)
    list(APPEND CMAKE_BUILD_RPATH "@loader_path")
  endif()
endif()

include(FetchContent)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)  # extracted files are dated when extracted, so a changed archive rebuilds
endif()
if(KERFKITE_GEOS_URL)
  set(kerfkite_geos_urls ${KERFKITE_GEOS_URL})
else()
  set(kerfkite_geos_urls
    https://github.com/libgeos/geos/releases/download/${KERFKITE_GEOS_VERSION}/geos-${KERFKITE_GEOS_VERSION}.tar.bz2
    https://download.osgeo.org/geos/geos-${KERFKITE_GEOS_VERSION}.tar.bz2)
endif()
FetchContent_Declare(kerfkite_geos URL ${kerfkite_geos_urls} URL_HASH SHA256=${KERFKITE_GEOS_SHA256})
FetchContent_GetProperties(kerfkite_geos)
if(NOT kerfkite_geos_POPULATED)
  if(NOT EXISTS ${FETCHCONTENT_BASE_DIR}/kerfkite_geos-src/CMakeLists.txt)
    message(STATUS "KerfKite: fetching GEOS ${KERFKITE_GEOS_VERSION}. Without a network, pass "
                   "-DKERFKITE_GEOS_URL=<path to geos-${KERFKITE_GEOS_VERSION}.tar.bz2> or -DKERFKITE_WITH_GEOS=OFF")
  endif()
  FetchContent_Populate(kerfkite_geos)
endif()

# GEOS is configured by its own build files, so it is compiled with the flags its authors chose, which are
# also the flags of the Shapely wheels (a plain Release build): no floating-point contraction on GCC and
# Clang, /fp:precise on MSVC. Nothing from the core's own targets reaches it. As a subdirectory GEOS builds
# none of its tests, benchmarks, tools or documentation, and EXCLUDE_FROM_ALL leaves out its install rules.
function(kerfkite_add_geos)
  set(BUILD_SHARED_LIBS ${KERFKITE_GEOS_SHARED})
  if(NOT MSVC AND NOT CMAKE_CONFIGURATION_TYPES)
    # Always the optimised build, as in the wheels, whatever the core itself is built as. (With MSVC the
    # two must use the same runtime library, so GEOS follows the core's configuration there.)
    set(CMAKE_BUILD_TYPE Release)
  endif()
  if(MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /MP")  # compile GEOS's files in parallel; no effect on the code
  endif()
  set(BUILD_TESTING OFF)
  set(BUILD_BENCHMARKS OFF)
  set(BUILD_DOCUMENTATION OFF)
  set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
  add_subdirectory(${kerfkite_geos_SOURCE_DIR} ${kerfkite_geos_BINARY_DIR} EXCLUDE_FROM_ALL)
  if(KERFKITE_GEOS_SHARED)
    # The libraries go beside the program so that it finds them without an install step. A generator
    # with several configurations adds the configuration's directory, as it does for the program.
    set_target_properties(geos geos_c PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
      LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
  endif()
endfunction()
kerfkite_add_geos()

# The checksum fixes the content; the geometry self-test also checks the version compiled in.
set(KERFKITE_GEOS_TARGET geos_c)
if(KERFKITE_GEOS_SHARED)
  message(STATUS "KerfKite: building GEOS ${KERFKITE_GEOS_VERSION} as shared libraries beside the program")
else()
  message(STATUS "KerfKite: building GEOS ${KERFKITE_GEOS_VERSION} into the program")
endif()
