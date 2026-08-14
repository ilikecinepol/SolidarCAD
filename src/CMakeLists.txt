function(solidar_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

add_library(solidar_model
  model/Document.cpp
  model/Document.h
  sketch/Sketch.cpp
  sketch/Sketch.h
)
target_include_directories(solidar_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(solidar_model PUBLIC cxx_std_20)
solidar_warnings(solidar_model)

add_library(solidar_project
  project/ProjectFile.cpp
  project/ProjectFile.h
)
target_include_directories(solidar_project PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(solidar_project PUBLIC Qt6::Core)
solidar_warnings(solidar_project)

add_library(solidar_sketch_ui
  sketch/SketchRibbon.cpp
  sketch/SketchRibbon.h
  ui/SketchCanvas.cpp
  ui/SketchCanvas.h
)
target_include_directories(solidar_sketch_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(solidar_sketch_ui PUBLIC solidar_model Qt6::Widgets)
solidar_warnings(solidar_sketch_ui)

add_library(solidar_drawing_ui
  drawing/EskdRenderer.cpp
  drawing/EskdRenderer.h
  ui/DrawingSheetView.cpp
  ui/DrawingSheetView.h
)
target_include_directories(solidar_drawing_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(solidar_drawing_ui PUBLIC solidar_model Qt6::Widgets)
solidar_warnings(solidar_drawing_ui)

add_library(solidar_viewport3d
  assets/icons.qrc
  ui/ModelRibbon.cpp
  ui/ModelRibbon.h
  ui/Viewport.cpp
  ui/Viewport.h
)
target_include_directories(solidar_viewport3d PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(solidar_viewport3d PUBLIC solidar_model Qt6::OpenGLWidgets)
solidar_warnings(solidar_viewport3d)

add_library(solidar_editor
  ui/MainWindow.cpp
  ui/MainWindow.h
)
target_include_directories(solidar_editor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(solidar_editor PUBLIC
  solidar_model solidar_sketch_ui solidar_drawing_ui solidar_viewport3d
  Qt6::PrintSupport
)
solidar_warnings(solidar_editor)

add_library(solidar_home
  home/HomeWindow.cpp
  home/HomeWindow.h
)
target_include_directories(solidar_home PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(solidar_home PUBLIC solidar_project Qt6::Widgets)
solidar_warnings(solidar_home)

add_executable(solidar app/main.cpp)
target_link_libraries(solidar PRIVATE solidar_home solidar_editor)
solidar_warnings(solidar)

install(TARGETS solidar RUNTIME DESTINATION bin)
