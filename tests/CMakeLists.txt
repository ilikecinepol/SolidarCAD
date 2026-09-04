if(MSVC)
  add_compile_options(/UNDEBUG)
else()
  add_compile_options(-UNDEBUG)
endif()

add_executable(document_tests document_tests.cpp)
target_link_libraries(document_tests PRIVATE solidar_model)
add_test(NAME document_tests COMMAND document_tests)

add_executable(extrude_feature_tests extrude_feature_tests.cpp)
target_link_libraries(extrude_feature_tests PRIVATE solidar_model)
add_test(NAME extrude_feature_tests COMMAND extrude_feature_tests)

add_executable(revolve_feature_tests revolve_feature_tests.cpp)
target_link_libraries(revolve_feature_tests PRIVATE solidar_model)
add_test(NAME revolve_feature_tests COMMAND revolve_feature_tests)

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

add_executable(model_ribbon_tests model_ribbon_tests.cpp)
target_link_libraries(model_ribbon_tests PRIVATE solidar_editor Qt6::Widgets)
add_test(NAME model_ribbon_tests COMMAND model_ribbon_tests)

add_executable(sketch_regression_tests sketch_regression_tests.cpp)
target_link_libraries(sketch_regression_tests PRIVATE solidar_model)
add_test(NAME sketch_regression_tests COMMAND sketch_regression_tests)

add_executable(body_render_mesh_tests body_render_mesh_tests.cpp)
target_link_libraries(body_render_mesh_tests PRIVATE solidar_viewport3d)
add_test(NAME body_render_mesh_tests COMMAND body_render_mesh_tests)

add_executable(edge_selection_state_tests edge_selection_state_tests.cpp)
target_link_libraries(edge_selection_state_tests PRIVATE solidar_viewport3d)
add_test(NAME edge_selection_state_tests COMMAND edge_selection_state_tests)

add_executable(viewport_picking_tests viewport_picking_tests.cpp)
target_link_libraries(viewport_picking_tests PRIVATE solidar_viewport3d)
add_test(NAME viewport_picking_tests COMMAND viewport_picking_tests)

add_executable(viewport_preview_selection_tests
               viewport_preview_selection_tests.cpp)
target_link_libraries(viewport_preview_selection_tests PRIVATE solidar_viewport3d)
add_test(NAME viewport_preview_selection_tests
         COMMAND viewport_preview_selection_tests)

add_executable(macro_foundation_tests macro_foundation_tests.cpp)
target_link_libraries(macro_foundation_tests PRIVATE solidar_model)
add_test(NAME macro_foundation_tests COMMAND macro_foundation_tests)

add_executable(fillet_feature_tests fillet_feature_tests.cpp)
target_link_libraries(fillet_feature_tests PRIVATE solidar_model)
add_test(NAME fillet_feature_tests COMMAND fillet_feature_tests)

add_executable(chamfer_feature_tests chamfer_feature_tests.cpp)
target_link_libraries(chamfer_feature_tests PRIVATE solidar_project Qt6::Core)
add_test(NAME chamfer_feature_tests COMMAND chamfer_feature_tests)

add_executable(parametric_feature_chain_tests
               parametric_feature_chain_tests.cpp)
target_link_libraries(parametric_feature_chain_tests PRIVATE solidar_model)
add_test(NAME parametric_feature_chain_tests
         COMMAND parametric_feature_chain_tests)

add_executable(part_design_regression_tests
               part_design_regression_tests.cpp)
target_link_libraries(part_design_regression_tests
  PRIVATE solidar_project Qt6::Core)
add_test(NAME part_design_regression_tests
         COMMAND part_design_regression_tests)

add_executable(parametric_history_tests parametric_history_tests.cpp)
target_link_libraries(parametric_history_tests PRIVATE solidar_model)
add_test(NAME parametric_history_tests COMMAND parametric_history_tests)

foreach(pattern_test mirror_feature_tests linear_pattern_feature_tests circular_pattern_feature_tests)
  add_executable(${pattern_test} ${pattern_test}.cpp)
  target_link_libraries(${pattern_test} PRIVATE solidar_model)
  add_test(NAME ${pattern_test} COMMAND ${pattern_test})
endforeach()
add_executable(pattern_persistence_tests pattern_persistence_tests.cpp)
target_link_libraries(pattern_persistence_tests PRIVATE solidar_project Qt6::Core)
add_test(NAME pattern_persistence_tests COMMAND pattern_persistence_tests)

add_executable(persistent_topology_tests
               persistent_topology_tests.cpp)
target_link_libraries(persistent_topology_tests PRIVATE solidar_model)
add_test(NAME persistent_topology_tests COMMAND persistent_topology_tests)

add_test(
  NAME compliance_artifacts_tests
  COMMAND ${CMAKE_COMMAND}
    -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/compliance_artifacts_tests.cmake
)

set_tests_properties(
  document_tests
  extrude_feature_tests
  revolve_feature_tests
  pocket_feature_tests
  body_render_mesh_tests
  edge_selection_state_tests
  viewport_picking_tests
  viewport_preview_selection_tests
  macro_foundation_tests
  fillet_feature_tests
  chamfer_feature_tests
  PROPERTIES LABELS "unit"
)

set_tests_properties(
  extrude_geometry_regression_tests
  extrude_boolean_regression_tests
  extrude_history_regression_tests
  project_file_tests
  sketch_regression_tests
  fillet_feature_tests
  chamfer_feature_tests
  revolve_feature_tests
  parametric_feature_chain_tests
  part_design_regression_tests
  parametric_history_tests
  persistent_topology_tests
  mirror_feature_tests
  linear_pattern_feature_tests
  circular_pattern_feature_tests
  pattern_persistence_tests
  PROPERTIES LABELS "regression"
)

set_property(TEST persistent_topology_tests
             PROPERTY LABELS "regression;topology")

set_tests_properties(
  editor_smoke_tests
  model_ribbon_tests
  PROPERTIES LABELS "ui-smoke"
)

set_tests_properties(
  compliance_artifacts_tests
  PROPERTIES LABELS "compliance"
)

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
    revolve_feature_tests
    extrude_geometry_regression_tests
    extrude_boolean_regression_tests
    extrude_history_regression_tests
    pocket_feature_tests
    project_file_tests
    editor_smoke_tests
    model_ribbon_tests
    sketch_regression_tests
    body_render_mesh_tests
    edge_selection_state_tests
    viewport_picking_tests
    viewport_preview_selection_tests
    macro_foundation_tests
    fillet_feature_tests
    chamfer_feature_tests
    parametric_feature_chain_tests
    part_design_regression_tests
    parametric_history_tests
    persistent_topology_tests
    mirror_feature_tests
    linear_pattern_feature_tests
    circular_pattern_feature_tests
    pattern_persistence_tests
    PROPERTIES
      ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${SOLIDAR_QT_BIN_DIRECTORY};PATH=path_list_prepend:${OpenCASCADE_BINARY_DIR}"
  )
endif()
