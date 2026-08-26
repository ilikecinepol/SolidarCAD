function(solidar_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

add_library(solidar_model
  model/Body.cpp
  model/Body.h
  model/Document.cpp
  model/Document.h
  model/ExtrudeFeature.cpp
  model/ExtrudeFeature.h
  model/PocketFeature.cpp
  model/PocketFeature.h
  model/SketchProfileBuilder.cpp
  model/SketchProfileBuilder.h
  model/Feature.cpp
  model/Feature.h
  model/ShapeFeature.cpp
  model/ShapeFeature.h
  model/SketchPlacement.cpp
  model/SketchPlacement.h
  model/SolidFeature.h
  sketch/Sketch.cpp
  sketch/Sketch.h
  sketch/SketchSolver.cpp
  sketch/SketchSolver.h
)
target_include_directories(solidar_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(solidar_model SYSTEM PUBLIC ${OpenCASCADE_INCLUDE_DIR})
target_link_libraries(solidar_model PUBLIC
  TKernel TKMath TKG2d TKG3d TKGeomBase TKBRep TKTopAlgo TKPrim TKBO TKBool TKMesh
)
target_compile_features(solidar_model PUBLIC cxx_std_20)
solidar_warnings(solidar_model)

add_library(solidar_project
  project/ProjectFile.cpp
  project/ProjectFile.h
  io/StlExporter.cpp
  io/StlExporter.h
)
target_include_directories(solidar_project PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(solidar_project PUBLIC solidar_model Qt6::Core)
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
  ui/BodyRenderMesh.cpp
  ui/BodyRenderMesh.h
  ui/ModelRibbon.cpp
  ui/ModelRibbon.h
  ui/Viewport.cpp
  ui/Viewport.h
)
target_include_directories(solidar_viewport3d PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(solidar_viewport3d PUBLIC solidar_model Qt6::Widgets)
solidar_warnings(solidar_viewport3d)

add_library(solidar_editor
  ui/EditorFactory.cpp
  ui/EditorFactory.h
  ui/MainWindow.cpp
  ui/MainWindow.h
)
target_include_directories(solidar_editor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(solidar_editor PUBLIC
  solidar_model solidar_sketch_ui solidar_drawing_ui solidar_viewport3d
  solidar_project Qt6::PrintSupport
)
solidar_warnings(solidar_editor)

add_library(solidar_home
  home/HomeWindow.cpp
  home/HomeWindow.h
)
target_include_directories(solidar_home PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(solidar_home PUBLIC solidar_project Qt6::Widgets)
solidar_warnings(solidar_home)

add_executable(solidar
  app/main.cpp
  assets/icons.qrc
)
target_link_libraries(solidar PRIVATE solidar_home solidar_editor)
solidar_warnings(solidar)

# Keep Windows development builds runnable outside a specially configured
# terminal. Qt Creator kits do not always propagate the Qt bin directory to
# the run environment, which otherwise results in Windows error 0xc0000135.
if(WIN32)
  get_target_property(QT_QMAKE_EXECUTABLE Qt6::qmake IMPORTED_LOCATION)
  get_filename_component(QT_BIN_DIRECTORY "${QT_QMAKE_EXECUTABLE}" DIRECTORY)
  find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${QT_BIN_DIRECTORY}")
  if(WINDEPLOYQT_EXECUTABLE)
    add_custom_command(TARGET solidar POST_BUILD
      COMMAND "${WINDEPLOYQT_EXECUTABLE}"
              --no-translations
              --no-system-d3d-compiler
              --no-compiler-runtime
              "$<TARGET_FILE:solidar>"
      COMMENT "Deploying Qt runtime libraries next to SolidarCAD"
      VERBATIM
    )
  endif()
  add_custom_command(TARGET solidar POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:solidar>
            $<TARGET_FILE_DIR:solidar>
    COMMAND_EXPAND_LISTS
    COMMENT "Deploying OCCT runtime libraries next to SolidarCAD"
  )
endif()

install(TARGETS solidar RUNTIME DESTINATION bin)
