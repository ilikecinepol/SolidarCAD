cmake_minimum_required(VERSION 3.24)

if(POLICY CMP0207)
  cmake_policy(SET CMP0207 NEW)
endif()

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
   AND EXISTS "${_solidar_bootstrap_occt_lib}/TKOffset.lib"
   AND EXISTS "${_solidar_bootstrap_occt_lib}/TKMesh.lib"
   AND EXISTS "${_solidar_bootstrap_occt_lib}/TKShHealing.lib")
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

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/stage" CACHE PATH
      "Default self-contained staging directory" FORCE)
endif()

if(MSVC)
  set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION bin)
  include(InstallRequiredSystemLibraries)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

option(SOLIDAR_BUILD_TESTS "Build unit tests" ON)
option(SOLIDAR_BUILD_BENCHMARKS
       "Build opt-in performance benchmarks (never registered with CTest)" OFF)
option(SOLIDAR_BUILD_FUZZERS
       "Build opt-in libFuzzer targets (Clang non-MSVC driver only)" OFF)
option(SOLIDAR_BUILD_LINUX_PACKAGE
       "Use the system-wide /opt layout and Debian package metadata on Linux"
       OFF)
option(SOLIDAR_ENABLE_SANITIZERS
       "Instrument SolidarCAD targets with supported runtime sanitizers" OFF)
option(SOLIDAR_ENABLE_CLANG_TIDY
       "Run the repository clang-tidy profile while compiling" OFF)
set(SOLIDAR_TEST_TIMEOUT_SECONDS "120" CACHE STRING
    "Common per-test CTest timeout for the selected build profile")
if(NOT SOLIDAR_TEST_TIMEOUT_SECONDS MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR "SOLIDAR_TEST_TIMEOUT_SECONDS must be a positive integer")
endif()
set(SOLIDAR_REQUIRED_QT_VERSION "" CACHE STRING
    "Require an exact Qt version (empty accepts any supported Qt)")

include(cmake/SolidarDeveloperProfiles.cmake)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets OpenGL OpenGLWidgets PrintSupport)
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
      TKBO TKBool TKFillet TKOffset TKMesh TKShHealing TKDE TKXSBase TKDESTEP)
  if(EXISTS "${_solidar_occt_build_dir}/include/opencascade/TopoDS_Shape.hxx"
     AND EXISTS "${_solidar_occt_lib_dir}/TKPrim.lib"
     AND EXISTS "${_solidar_occt_lib_dir}/TKBO.lib"
     AND EXISTS "${_solidar_occt_lib_dir}/TKBool.lib"
     AND EXISTS "${_solidar_occt_lib_dir}/TKFillet.lib"
     AND EXISTS "${_solidar_occt_lib_dir}/TKOffset.lib"
     AND EXISTS "${_solidar_occt_lib_dir}/TKMesh.lib"
     AND EXISTS "${_solidar_occt_lib_dir}/TKShHealing.lib"
     AND EXISTS "${_solidar_occt_lib_dir}/TKDE.lib"
     AND EXISTS "${_solidar_occt_lib_dir}/TKXSBase.lib"
     AND EXISTS "${_solidar_occt_lib_dir}/TKDESTEP.lib")
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

if(SOLIDAR_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

if(SOLIDAR_BUILD_FUZZERS)
  add_subdirectory(fuzz)
endif()

if(UNIX AND NOT APPLE AND SOLIDAR_BUILD_LINUX_PACKAGE)
  set(SOLIDAR_DOCUMENTATION_INSTALL_DIR "usr/share/doc/solidarcad")
else()
  set(SOLIDAR_DOCUMENTATION_INSTALL_DIR "share/SolidarCAD")
endif()

install(FILES
  LICENSE
  NOTICE
  README.md
  CHANGELOG.md
  DEPENDENCIES.md
  THIRD_PARTY_NOTICES.md
  DESTINATION "${SOLIDAR_DOCUMENTATION_INSTALL_DIR}"
)
install(DIRECTORY LICENSES/
        DESTINATION "${SOLIDAR_DOCUMENTATION_INSTALL_DIR}/LICENSES")
if(EXISTS "${OpenCASCADE_DIR}/copyright")
  install(FILES "${OpenCASCADE_DIR}/copyright"
          DESTINATION "${SOLIDAR_DOCUMENTATION_INSTALL_DIR}/LICENSES/OCCT")
endif()

if(UNIX AND NOT APPLE AND SOLIDAR_BUILD_LINUX_PACKAGE)
  install(PROGRAMS packaging/linux/solidar DESTINATION usr/bin)
  install(FILES packaging/linux/solidarcad.desktop
          DESTINATION usr/share/applications)
  install(FILES packaging/linux/application-x-solidarcad.xml
          DESTINATION usr/share/mime/packages)
  install(FILES src/assets/solidarity-3d-logo.png
          DESTINATION usr/share/pixmaps
          RENAME solidarcad.png)
endif()

set(CPACK_PACKAGE_NAME "SolidarCAD")
set(CPACK_PACKAGE_VENDOR "SolidarCAD")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Parametric CAD application for solid modelling")
set(CPACK_PACKAGE_HOMEPAGE_URL
    "https://github.com/ilikecinepol/SolidarCAD")
set(CPACK_PACKAGE_FILE_NAME
    "SolidarCAD-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}")
if(WIN32)
  set(CPACK_GENERATOR "ZIP")
elseif(UNIX AND NOT APPLE AND SOLIDAR_BUILD_LINUX_PACKAGE)
  set(CPACK_GENERATOR "DEB")
  set(CPACK_PACKAGING_INSTALL_PREFIX "/")
  set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
  set(CPACK_DEBIAN_PACKAGE_NAME "solidarcad")
  set(CPACK_DEBIAN_PACKAGE_RELEASE "1")
  set(CPACK_DEBIAN_PACKAGE_MAINTAINER
      "Mikhail Molotkov <ilikecinepol@users.noreply.github.com>")
  set(CPACK_DEBIAN_PACKAGE_SECTION "graphics")
  set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
  set(CPACK_DEBIAN_PACKAGE_DESCRIPTION
      "SolidarCAD is a parametric CAD application for constrained sketches,
 solid feature modelling, editable history, and STEP/STL exchange.")
  set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE_URL}")
  set(CPACK_DEBIAN_PACKAGE_DEPENDS
      "desktop-file-utils, shared-mime-info")
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  set(CPACK_DEBIAN_COMPRESSION_TYPE "xz")
  set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
      "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/postinst;${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/postrm")
  set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION TRUE)
else()
  set(CPACK_GENERATOR "TGZ")
endif()
include(CPack)
