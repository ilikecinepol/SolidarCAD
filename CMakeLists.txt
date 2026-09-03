cmake_minimum_required(VERSION 3.24)

# Qt Creator injects Visual Studio's vcpkg toolchain before project().  The
# minimal OCCT toolkits may already be available in the repository-local build
# tree even though the complete manifest package is not installed yet.  Avoid
# rebuilding the entire OCCT port on every Qt Creator configure in that case.
option(SOLIDAR_ALLOW_OCCT_BUILD_TREE_FALLBACK
       "Allow the Windows Qt Creator OCCT build-tree fallback" ON)
set(_solidar_bootstrap_occt_root
    "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/vcpkg/blds/opencascade")
# Never change manifest mode in an existing Qt Creator build tree. Its
# generated toolchain records the mode selected during the first configure.
if(SOLIDAR_ALLOW_OCCT_BUILD_TREE_FALLBACK
   AND DEFINED VCPKG_MANIFEST_INSTALL AND NOT VCPKG_MANIFEST_INSTALL)
  set(VCPKG_MANIFEST_MODE OFF CACHE BOOL
      "Keep the build directory's original vcpkg mode" FORCE)
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(_solidar_bootstrap_occt_lib
      "${_solidar_bootstrap_occt_root}/x64-windows-dbg/win64/vc14/libd")
else()
  set(_solidar_bootstrap_occt_lib
      "${_solidar_bootstrap_occt_root}/x64-windows-rel/win64/vc14/lib")
endif()
if(SOLIDAR_ALLOW_OCCT_BUILD_TREE_FALLBACK
   AND EXISTS "${_solidar_bootstrap_occt_lib}/TKPrim.lib"
   AND EXISTS "${_solidar_bootstrap_occt_lib}/TKBO.lib"
   AND EXISTS "${_solidar_bootstrap_occt_lib}/TKBool.lib"
   AND EXISTS "${_solidar_bootstrap_occt_lib}/TKFillet.lib"
   AND EXISTS "${_solidar_bootstrap_occt_lib}/TKMesh.lib")
  set(VCPKG_MANIFEST_MODE OFF CACHE BOOL
      "Use the already-built local OCCT toolkits" FORCE)
endif()

# Some Qt Creator maintenance-tool profiles pass only the name `jom`, while
# their configure environment does not contain Qt Creator's jom directory.
if(CMAKE_GENERATOR MATCHES "NMake Makefiles JOM"
   AND NOT CMAKE_MAKE_PROGRAM)
  find_program(_solidar_jom_executable NAMES jom
    HINTS
      "C:/Qt/Tools/QtCreator/bin/jom"
      "$ENV{QTC_INSTALL}/bin/jom"
    NO_DEFAULT_PATH)
  if(_solidar_jom_executable)
    set(CMAKE_MAKE_PROGRAM "${_solidar_jom_executable}" CACHE FILEPATH
        "Qt Creator jom executable" FORCE)
  endif()
endif()

project(SolidarCAD VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

option(SOLIDAR_BUILD_TESTS "Build unit tests" ON)
set(SOLIDAR_REQUIRED_QT_VERSION "" CACHE STRING
    "Require an exact Qt version (empty accepts any supported Qt)")

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets OpenGLWidgets PrintSupport)
if(SOLIDAR_REQUIRED_QT_VERSION
   AND NOT Qt6_VERSION VERSION_EQUAL SOLIDAR_REQUIRED_QT_VERSION)
  message(FATAL_ERROR
    "This build profile requires Qt ${SOLIDAR_REQUIRED_QT_VERSION}, "
    "but CMake found Qt ${Qt6_VERSION}.")
endif()

# Prefer a regular SDK/vcpkg package. During the first manifest bootstrap,
# Visual Studio's bundled vcpkg keeps completed OCCT toolkits in its local
# build tree until every optional toolkit has finished. Let Qt Creator use the
# already-built minimal Extrude toolkits instead of requiring a manual
# CMAKE_PREFIX_PATH entry.
find_package(OpenCASCADE CONFIG QUIET)
if(NOT OpenCASCADE_FOUND AND WIN32
   AND SOLIDAR_ALLOW_OCCT_BUILD_TREE_FALLBACK)
  set(_solidar_occt_build_root
      "${CMAKE_SOURCE_DIR}/vcpkg_installed/vcpkg/blds/opencascade")
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(_solidar_occt_build_dir "${_solidar_occt_build_root}/x64-windows-dbg")
    set(_solidar_occt_bin_dir "${_solidar_occt_build_dir}/win64/vc14/bind")
    set(_solidar_occt_lib_dir "${_solidar_occt_build_dir}/win64/vc14/libd")
  else()
    set(_solidar_occt_build_dir "${_solidar_occt_build_root}/x64-windows-rel")
    set(_solidar_occt_bin_dir "${_solidar_occt_build_dir}/win64/vc14/bin")
    set(_solidar_occt_lib_dir "${_solidar_occt_build_dir}/win64/vc14/lib")
  endif()

  set(_solidar_occt_toolkits
      TKernel TKMath TKG2d TKG3d TKGeomBase TKBRep TKGeomAlgo TKTopAlgo TKPrim
      TKBO TKBool TKFillet TKMesh)
  if(EXISTS "${_solidar_occt_build_dir}/include/opencascade/TopoDS_Shape.hxx"
     AND EXISTS "${_solidar_occt_lib_dir}/TKPrim.lib"
     AND EXISTS "${_solidar_occt_lib_dir}/TKBO.lib"
     AND EXISTS "${_solidar_occt_lib_dir}/TKBool.lib"
     AND EXISTS "${_solidar_occt_lib_dir}/TKFillet.lib"
     AND EXISTS "${_solidar_occt_lib_dir}/TKMesh.lib")
    set(OpenCASCADE_FOUND TRUE)
    set(OpenCASCADE_INCLUDE_DIR
        "${_solidar_occt_build_dir}/include/opencascade")
    set(OpenCASCADE_BINARY_DIR "${_solidar_occt_bin_dir}")
    foreach(_solidar_occt_toolkit IN LISTS _solidar_occt_toolkits)
      if(NOT TARGET ${_solidar_occt_toolkit})
        add_library(${_solidar_occt_toolkit} SHARED IMPORTED GLOBAL)
        set_target_properties(${_solidar_occt_toolkit} PROPERTIES
          IMPORTED_IMPLIB "${_solidar_occt_lib_dir}/${_solidar_occt_toolkit}.lib"
          IMPORTED_LOCATION "${_solidar_occt_bin_dir}/${_solidar_occt_toolkit}.dll"
        )
      endif()
    endforeach()
    message(STATUS "Using local OCCT Extrude toolkits: ${_solidar_occt_build_dir}")
  endif()
endif()

if(NOT OpenCASCADE_FOUND)
  message(FATAL_ERROR
    "OpenCASCADE was not found. Configure with the vcpkg toolchain so "
    "vcpkg.json can install opencascade, or set OpenCASCADE_DIR to the "
    "directory containing OpenCASCADEConfig.cmake.")
endif()

add_subdirectory(src)

if(SOLIDAR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
