# Builds the Open CASCADE Technology that kerfkite-step uses, from its unmodified source: the modules STEP exchange
# needs, as shared libraries (LGPL 2.1 with the OCCT exception, kept as files that can be replaced), with none of
# Open CASCADE's optional third-party libraries. The releases use OCCT 7.9.3, the tag V7_9_3 at commit
# a016080bf6738d6aeae020badee4e888ad1540a5, the release the Python reference's OCP carries.
#
#   cmake -DSOURCE=<checkout> -DBUILD=<build folder> -DPREFIX=<install folder> [-DJOBS=4] -P native/step/open-cascade.cmake
#
# Then build native/ with -DOpenCASCADE_DIR=<the folder of OpenCASCADEConfig.cmake under PREFIX>.
cmake_minimum_required(VERSION 3.16)
foreach(name SOURCE BUILD PREFIX)
  if(NOT ${name})
    message(FATAL_ERROR "Pass -D${name}=<folder>")
  endif()
endforeach()
if(NOT JOBS)
  set(JOBS 4)
endif()
set(options
  -DCMAKE_BUILD_TYPE=Release -DBUILD_LIBRARY_TYPE=Shared "-DINSTALL_DIR=${PREFIX}"
  # The toolkits STEP needs: OCCT adds the ones they depend on.
  -DBUILD_MODULE_FoundationClasses=ON -DBUILD_MODULE_ModelingData=ON -DBUILD_MODULE_ModelingAlgorithms=ON
  -DBUILD_MODULE_Visualization=OFF -DBUILD_MODULE_ApplicationFramework=OFF -DBUILD_MODULE_DataExchange=OFF
  -DBUILD_MODULE_Draw=OFF -DBUILD_MODULE_DETools=OFF -DBUILD_ADDITIONAL_TOOLKITS=TKDESTEP
  -DBUILD_DOC_Overview=OFF -DBUILD_Inspector=OFF -DBUILD_SAMPLES_QT=OFF
  # No third-party libraries.
  -DUSE_FREETYPE=OFF -DUSE_OPENGL=OFF -DUSE_GLES2=OFF -DUSE_XLIB=OFF -DUSE_TK=OFF -DUSE_TCL=OFF -DUSE_FREEIMAGE=OFF
  -DUSE_FFMPEG=OFF -DUSE_OPENVR=OFF -DUSE_RAPIDJSON=OFF -DUSE_DRACO=OFF -DUSE_TBB=OFF -DUSE_VTK=OFF)
if(DEFINED ENV{MACOSX_DEPLOYMENT_TARGET})
  list(APPEND options "-DCMAKE_OSX_DEPLOYMENT_TARGET=$ENV{MACOSX_DEPLOYMENT_TARGET}")
endif()
foreach(step configure build install)
  if(step STREQUAL "configure")
    set(command "${CMAKE_COMMAND}" -S "${SOURCE}" -B "${BUILD}" ${options})
  elseif(step STREQUAL "build")
    set(command "${CMAKE_COMMAND}" --build "${BUILD}" --config Release --parallel ${JOBS})
  else()
    set(command "${CMAKE_COMMAND}" --install "${BUILD}" --config Release)
  endif()
  execute_process(COMMAND ${command} RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Open CASCADE ${step} failed (${result})")
  endif()
endforeach()
