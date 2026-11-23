# The Linux desktop window and its smoke test. Building needs a C compiler, pkg-config and the GTK 3 headers
# (3.22 or later); WebKitGTK 4.1 or 4.0 is found when the window starts.
#
#   cmake -S packaging/desktop/linux -B build/window -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/window
cmake_minimum_required(VERSION 3.16)
project(kerfkite_linux_window C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0>=3.22)

add_library(kerfkite_workbench STATIC webkit_api.c workbench.c)
target_link_libraries(kerfkite_workbench PUBLIC PkgConfig::GTK3 ${CMAKE_DL_LIBS} m)
target_compile_options(kerfkite_workbench PUBLIC -Wall -Wextra)

add_executable(kerfkite-native main.c)
target_link_libraries(kerfkite-native PRIVATE kerfkite_workbench)

# Drives a workbench against a running Studio simulator: kerfkite-native-smoke http://127.0.0.1:PORT/
add_executable(kerfkite-native-smoke smoke.c)
target_link_libraries(kerfkite-native-smoke PRIVATE kerfkite_workbench)
