cmake_minimum_required(VERSION 3.24)

project(SolidarCAD VERSION 0.1.0 LANGUAGES CXX)

# Qt's msvc2022_64 Debug binaries use the VS 2022 debug CRT.  The VS 2026
# toolset (MSVC 19.50+) is ABI-compatible for release applications, but its
# debug allocator is not compatible and eventually reports heap corruption
# (0xc0000374).  Qt Creator may silently restore CMAKE_BUILD_TYPE=Debug, so
# keep developer symbols while selecting the compatible release CRT.
if(WIN32 AND MSVC AND MSVC_VERSION GREATER_EQUAL 1950 AND
   CMAKE_BUILD_TYPE STREQUAL "Debug")
  message(WARNING
    "MSVC ${MSVC_VERSION} with Qt msvc2022_64 cannot safely use Debug CRT; "
    "using RelWithDebInfo instead.")
  set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

option(SOLIDAR_BUILD_TESTS "Build unit tests" ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets PrintSupport)

add_subdirectory(src)

if(SOLIDAR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
