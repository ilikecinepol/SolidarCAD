add_executable(document_tests document_tests.cpp)
target_link_libraries(document_tests PRIVATE solidar_model)
add_test(NAME document_tests COMMAND document_tests)

add_executable(extrude_feature_tests extrude_feature_tests.cpp)
target_link_libraries(extrude_feature_tests PRIVATE solidar_model)
add_test(NAME extrude_feature_tests COMMAND extrude_feature_tests)

add_executable(extrude_geometry_regression_tests
               extrude_geometry_regression_tests.cpp)
target_link_libraries(extrude_geometry_regression_tests PRIVATE solidar_model)
add_test(NAME extrude_geometry_regression_tests
         COMMAND extrude_geometry_regression_tests)

add_executable(extrude_boolean_regression_tests
               extrude_boolean_regression_tests.cpp)
target_link_libraries(extrude_boolean_regression_tests PRIVATE solidar_model)
add_test(NAME extrude_boolean_regression_tests
         COMMAND extrude_boolean_regression_tests)

add_executable(extrude_history_regression_tests
               extrude_history_regression_tests.cpp)
target_link_libraries(extrude_history_regression_tests PRIVATE solidar_model)
add_test(NAME extrude_history_regression_tests
         COMMAND extrude_history_regression_tests)

add_executable(pocket_feature_tests pocket_feature_tests.cpp)
target_link_libraries(pocket_feature_tests PRIVATE solidar_model)
add_test(NAME pocket_feature_tests COMMAND pocket_feature_tests)

add_executable(project_file_tests project_file_tests.cpp)
target_link_libraries(project_file_tests PRIVATE solidar_project Qt6::Core)
add_test(NAME project_file_tests COMMAND project_file_tests)

add_executable(editor_smoke_tests editor_smoke_tests.cpp)
target_link_libraries(editor_smoke_tests PRIVATE solidar_editor solidar_home Qt6::Widgets)
add_test(NAME editor_smoke_tests COMMAND editor_smoke_tests)

add_executable(sketch_regression_tests sketch_regression_tests.cpp)
target_link_libraries(sketch_regression_tests PRIVATE solidar_model)
add_test(NAME sketch_regression_tests COMMAND sketch_regression_tests)

add_executable(body_render_mesh_tests body_render_mesh_tests.cpp)
target_link_libraries(body_render_mesh_tests PRIVATE solidar_viewport3d)
add_test(NAME body_render_mesh_tests COMMAND body_render_mesh_tests)

add_executable(macro_foundation_tests macro_foundation_tests.cpp)
target_link_libraries(macro_foundation_tests PRIVATE solidar_model)
add_test(NAME macro_foundation_tests COMMAND macro_foundation_tests)

add_executable(fillet_feature_tests fillet_feature_tests.cpp)
target_link_libraries(fillet_feature_tests PRIVATE solidar_model)
add_test(NAME fillet_feature_tests COMMAND fillet_feature_tests)

# SOLIDAR TEST QT RUNTIME PATH
# On Windows the Qt-dependent test executables need Qt DLLs at runtime.
# Configure CTest itself so users do not need to edit PATH manually.
if(WIN32)
  get_target_property(SOLIDAR_QT_QMAKE_EXECUTABLE
                      Qt6::qmake IMPORTED_LOCATION)
  get_filename_component(SOLIDAR_QT_BIN_DIRECTORY
                         "${SOLIDAR_QT_QMAKE_EXECUTABLE}"
                         DIRECTORY)

  set_tests_properties(
    document_tests
    extrude_feature_tests
    extrude_geometry_regression_tests
    extrude_boolean_regression_tests
    extrude_history_regression_tests
    pocket_feature_tests
    project_file_tests
    editor_smoke_tests
    sketch_regression_tests
    body_render_mesh_tests
    macro_foundation_tests
    fillet_feature_tests
    PROPERTIES
      ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${SOLIDAR_QT_BIN_DIRECTORY};PATH=path_list_prepend:${OpenCASCADE_BINARY_DIR}"
  )
endif()
