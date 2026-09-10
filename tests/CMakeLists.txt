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

add_executable(part_design_tool_framework_tests
               part_design_tool_framework_tests.cpp)
target_link_libraries(part_design_tool_framework_tests PRIVATE solidar_editor)
add_test(NAME part_design_tool_framework_tests
         COMMAND part_design_tool_framework_tests)

add_executable(part_design_ui_state_regression_tests
               part_design_ui_state_regression_tests.cpp)
target_link_libraries(part_design_ui_state_regression_tests PRIVATE solidar_editor)
add_test(NAME part_design_ui_state_regression_tests
         COMMAND part_design_ui_state_regression_tests)

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

add_executable(project_lifecycle_tests project_lifecycle_tests.cpp)
target_link_libraries(project_lifecycle_tests PRIVATE solidar_project Qt6::Core)
add_test(NAME project_lifecycle_tests COMMAND project_lifecycle_tests)

add_executable(project_switch_ui_tests project_switch_ui_tests.cpp)
target_link_libraries(project_switch_ui_tests
  PRIVATE solidar_editor solidar_project Qt6::Widgets)
add_test(NAME project_switch_ui_tests COMMAND project_switch_ui_tests)

add_executable(model_ribbon_tests model_ribbon_tests.cpp)
target_link_libraries(model_ribbon_tests PRIVATE solidar_editor Qt6::Widgets)
add_test(NAME model_ribbon_tests COMMAND model_ribbon_tests)

add_executable(part_design_history_ui_tests part_design_history_ui_tests.cpp)
target_link_libraries(part_design_history_ui_tests
  PRIVATE solidar_editor solidar_project Qt6::Widgets)
add_test(NAME part_design_history_ui_tests COMMAND part_design_history_ui_tests)
add_executable(feature_removal_tests feature_removal_tests.cpp)
target_link_libraries(feature_removal_tests PRIVATE solidar_model)
add_test(NAME feature_removal_tests COMMAND feature_removal_tests)
add_executable(history_timeline_geometry_tests history_timeline_geometry_tests.cpp)
target_link_libraries(history_timeline_geometry_tests PRIVATE solidar_viewport3d)
add_test(NAME history_timeline_geometry_tests COMMAND history_timeline_geometry_tests)
add_executable(historical_preview_tests historical_preview_tests.cpp)
target_link_libraries(historical_preview_tests PRIVATE solidar_viewport3d)
add_test(NAME historical_preview_tests COMMAND historical_preview_tests)

add_executable(sketch_regression_tests sketch_regression_tests.cpp)
target_link_libraries(sketch_regression_tests PRIVATE solidar_model)
add_test(NAME sketch_regression_tests COMMAND sketch_regression_tests)

add_executable(body_render_mesh_tests body_render_mesh_tests.cpp)
target_link_libraries(body_render_mesh_tests PRIVATE solidar_viewport3d)
add_test(NAME body_render_mesh_tests COMMAND body_render_mesh_tests)

add_executable(body_render_mesh_normals_tests body_render_mesh_normals_tests.cpp)
target_link_libraries(body_render_mesh_normals_tests PRIVATE solidar_viewport3d)
add_test(NAME body_render_mesh_normals_tests COMMAND body_render_mesh_normals_tests)

add_executable(viewport_renderer_tests viewport_renderer_tests.cpp)
target_link_libraries(viewport_renderer_tests PRIVATE solidar_viewport3d)
add_test(NAME viewport_renderer_tests COMMAND viewport_renderer_tests)

add_executable(viewport_ux_polish_tests viewport_ux_polish_tests.cpp)
target_link_libraries(viewport_ux_polish_tests PRIVATE solidar_viewport3d)
add_test(NAME viewport_ux_polish_tests COMMAND viewport_ux_polish_tests)

add_executable(edge_selection_state_tests edge_selection_state_tests.cpp)
target_link_libraries(edge_selection_state_tests PRIVATE solidar_viewport3d)
add_test(NAME edge_selection_state_tests COMMAND edge_selection_state_tests)

add_executable(viewport_picking_tests viewport_picking_tests.cpp)
target_link_libraries(viewport_picking_tests PRIVATE solidar_viewport3d)
add_test(NAME viewport_picking_tests COMMAND viewport_picking_tests)

add_executable(numeric_parameter_state_tests numeric_parameter_state_tests.cpp)
target_link_libraries(numeric_parameter_state_tests PRIVATE solidar_model)
add_test(NAME numeric_parameter_state_tests COMMAND numeric_parameter_state_tests)

add_executable(viewport_manipulator_tests viewport_manipulator_tests.cpp)
target_link_libraries(viewport_manipulator_tests
                      PRIVATE solidar_viewport3d Qt6::Widgets)
add_test(NAME viewport_manipulator_tests COMMAND viewport_manipulator_tests)

add_executable(viewport_preview_selection_tests
               viewport_preview_selection_tests.cpp)
target_link_libraries(viewport_preview_selection_tests PRIVATE solidar_viewport3d)
add_test(NAME viewport_preview_selection_tests
         COMMAND viewport_preview_selection_tests)

add_executable(viewport_reset_tests viewport_reset_tests.cpp)
target_link_libraries(viewport_reset_tests
  PRIVATE solidar_viewport3d Qt6::Widgets)
add_test(NAME viewport_reset_tests COMMAND viewport_reset_tests)

add_executable(view_cube_tests view_cube_tests.cpp)
target_link_libraries(view_cube_tests PRIVATE solidar_viewport3d)
add_test(NAME view_cube_tests COMMAND view_cube_tests)
set_tests_properties(view_cube_tests PROPERTIES LABELS "regression;ui-smoke")

add_executable(world_grid_tests world_grid_tests.cpp)
target_link_libraries(world_grid_tests PRIVATE solidar_viewport3d)
add_test(NAME world_grid_tests COMMAND world_grid_tests)
set_tests_properties(world_grid_tests PROPERTIES LABELS "regression")

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

add_executable(downstream_feature_preservation_tests
               downstream_feature_preservation_tests.cpp)
target_link_libraries(downstream_feature_preservation_tests
  PRIVATE solidar_project Qt6::Core)
add_test(NAME downstream_feature_preservation_tests
         COMMAND downstream_feature_preservation_tests)

add_executable(shell_feature_tests shell_feature_tests.cpp)
target_link_libraries(shell_feature_tests PRIVATE solidar_project Qt6::Core)
add_test(NAME shell_feature_tests COMMAND shell_feature_tests)
add_executable(shell_complex_topology_tests shell_complex_topology_tests.cpp)
target_link_libraries(shell_complex_topology_tests PRIVATE solidar_model)
add_test(NAME shell_complex_topology_tests COMMAND shell_complex_topology_tests)

add_executable(draft_feature_tests draft_feature_tests.cpp)
target_link_libraries(draft_feature_tests PRIVATE solidar_project Qt6::Core)
add_test(NAME draft_feature_tests COMMAND draft_feature_tests)

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
add_executable(multi_solid_edge_feature_tests multi_solid_edge_feature_tests.cpp)
target_link_libraries(multi_solid_edge_feature_tests PRIVATE solidar_model)
add_test(NAME multi_solid_edge_feature_tests COMMAND multi_solid_edge_feature_tests)

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
  part_design_tool_framework_tests
  part_design_ui_state_regression_tests
  pocket_feature_tests
  body_render_mesh_tests
  body_render_mesh_normals_tests
  viewport_renderer_tests
  viewport_ux_polish_tests
  edge_selection_state_tests
  viewport_picking_tests
  numeric_parameter_state_tests
  viewport_manipulator_tests
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
  body_render_mesh_normals_tests
  viewport_renderer_tests
  viewport_ux_polish_tests
  fillet_feature_tests
  chamfer_feature_tests
  revolve_feature_tests
  parametric_feature_chain_tests
  part_design_regression_tests
  downstream_feature_preservation_tests
  shell_feature_tests
  shell_complex_topology_tests
  draft_feature_tests
  parametric_history_tests
  persistent_topology_tests
  project_lifecycle_tests
  mirror_feature_tests
  linear_pattern_feature_tests
  circular_pattern_feature_tests
  pattern_persistence_tests
  multi_solid_edge_feature_tests
  viewport_reset_tests
  PROPERTIES LABELS "regression"
)

set_property(TEST persistent_topology_tests
             PROPERTY LABELS "regression;topology")
set_tests_properties(
  editor_smoke_tests
  model_ribbon_tests
  part_design_history_ui_tests
  feature_removal_tests
  history_timeline_geometry_tests
  historical_preview_tests
  project_switch_ui_tests
  PROPERTIES LABELS "ui-smoke"
)

set_property(TEST historical_preview_tests
             PROPERTY LABELS "regression;ui-smoke")

set_property(TEST project_switch_ui_tests
             PROPERTY LABELS "regression;ui-smoke")

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
    part_design_tool_framework_tests
    part_design_ui_state_regression_tests
    extrude_geometry_regression_tests
    extrude_boolean_regression_tests
    extrude_history_regression_tests
    pocket_feature_tests
    project_file_tests
    editor_smoke_tests
    model_ribbon_tests
    part_design_history_ui_tests
    feature_removal_tests
    history_timeline_geometry_tests
    historical_preview_tests
    sketch_regression_tests
    body_render_mesh_tests
    body_render_mesh_normals_tests
    viewport_renderer_tests
    viewport_ux_polish_tests
    edge_selection_state_tests
    viewport_picking_tests
    viewport_manipulator_tests
    viewport_preview_selection_tests
    viewport_reset_tests
    view_cube_tests
    world_grid_tests
    macro_foundation_tests
    fillet_feature_tests
    chamfer_feature_tests
    parametric_feature_chain_tests
    part_design_regression_tests
    downstream_feature_preservation_tests
    shell_feature_tests
    shell_complex_topology_tests
    draft_feature_tests
    parametric_history_tests
    persistent_topology_tests
    mirror_feature_tests
    linear_pattern_feature_tests
    circular_pattern_feature_tests
    pattern_persistence_tests
    multi_solid_edge_feature_tests
    project_lifecycle_tests
    project_switch_ui_tests
    PROPERTIES
      ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${SOLIDAR_QT_BIN_DIRECTORY};PATH=path_list_prepend:${OpenCASCADE_BINARY_DIR}"
  )
endif()
