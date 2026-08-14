cmake_minimum_required(VERSION 3.24)

project(SolidarCAD VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

option(SOLIDAR_BUILD_TESTS "Build unit tests" ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets OpenGLWidgets PrintSupport)

add_subdirectory(src)

if(SOLIDAR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
