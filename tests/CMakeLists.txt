if(MSVC)
  add_compile_options(/UNDEBUG)
else()
  add_compile_options(-UNDEBUG)
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Test)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Every test goes through this registration boundary. It keeps executable
# creation, CTest registration, labels, timeout and the Windows runtime search
# path together so a new target cannot silently miss one of them.
set(SOLIDAR_TEST_ENVIRONMENT_MODIFICATIONS)
if(WIN32)
  get_target_property(SOLIDAR_QT_QMAKE_EXECUTABLE
                      Qt6::qmake IMPORTED_LOCATION)
  get_filename_component(SOLIDAR_QT_BIN_DIRECTORY
                         "${SOLIDAR_QT_QMAKE_EXECUTABLE}" DIRECTORY)
  list(APPEND SOLIDAR_TEST_ENVIRONMENT_MODIFICATIONS
      "PATH=path_list_prepend:${SOLIDAR_QT_BIN_DIRECTORY};PATH=path_list_prepend:${OpenCASCADE_BINARY_DIR}")
endif()
if(SOLIDAR_SANITIZER_TEST_ENVIRONMENT)
  list(APPEND SOLIDAR_TEST_ENVIRONMENT_MODIFICATIONS
       ${SOLIDAR_SANITIZER_TEST_ENVIRONMENT})
endif()
if(SOLIDAR_SANITIZER_RUNTIME_DIRECTORY)
  list(APPEND SOLIDAR_TEST_ENVIRONMENT_MODIFICATIONS
       "PATH=path_list_prepend:${SOLIDAR_SANITIZER_RUNTIME_DIRECTORY}")
endif()

function(solidar_add_test test_name)
  if(TEST ${test_name})
    message(FATAL_ERROR "Duplicate test registration: ${test_name}")
  endif()
  set(one_value_args TIMEOUT)
  set(multi_value_args SOURCES LIBRARIES LABELS COMMAND)
  cmake_parse_arguments(TEST "" "${one_value_args}" "${multi_value_args}"
                        ${ARGN})
  if(TEST_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR
            "Unknown arguments for ${test_name}: ${TEST_UNPARSED_ARGUMENTS}")
  endif()
  if(TEST_COMMAND AND (TEST_SOURCES OR TEST_LIBRARIES))
    message(FATAL_ERROR
            "${test_name}: COMMAND cannot be combined with SOURCES/LIBRARIES")
  endif()
  if(NOT TEST_LABELS)
    message(FATAL_ERROR "${test_name}: LABELS are required")
  endif()
  if(NOT TEST_TIMEOUT)
    set(TEST_TIMEOUT ${SOLIDAR_TEST_TIMEOUT_SECONDS})
  endif()

  if(TEST_COMMAND)
    add_test(NAME ${test_name} COMMAND ${TEST_COMMAND})
  else()
    if(NOT TEST_SOURCES)
      set(TEST_SOURCES ${test_name}.cpp)
    endif()
    add_executable(${test_name} ${TEST_SOURCES})
    if(TEST_LIBRARIES)
      target_link_libraries(${test_name} PRIVATE ${TEST_LIBRARIES})
    endif()
    add_test(NAME ${test_name} COMMAND ${test_name})
  endif()

  set_tests_properties(${test_name} PROPERTIES
    LABELS "${TEST_LABELS}"
    TIMEOUT "${TEST_TIMEOUT}")
  if(SOLIDAR_TEST_ENVIRONMENT_MODIFICATIONS)
    set_tests_properties(${test_name} PROPERTIES
      ENVIRONMENT_MODIFICATION "${SOLIDAR_TEST_ENVIRONMENT_MODIFICATIONS}")
  endif()
  set_property(GLOBAL APPEND PROPERTY SOLIDAR_REGISTERED_TESTS ${test_name})
endfunction()

solidar_add_test(document_tests
  LIBRARIES solidar_model LABELS unit)
solidar_add_test(model_command_history_tests
  LIBRARIES solidar_editor LABELS unit regression)

# This target intentionally neither links solidar_model nor receives any Qt
# target/include directory. It compiles the public headers using only the
# repository and OCCT include roots, so a Qt include in the model API fails.
solidar_add_test(model_public_headers_compile_tests LABELS compliance)
target_include_directories(model_public_headers_compile_tests
  PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(model_public_headers_compile_tests SYSTEM
  PRIVATE ${OpenCASCADE_INCLUDE_DIR})
target_compile_features(model_public_headers_compile_tests PRIVATE cxx_std_20)

get_target_property(solidar_model_public_links
                    solidar_model INTERFACE_LINK_LIBRARIES)
if("${solidar_model_public_links}" MATCHES "Qt[0-9]*::")
  message(FATAL_ERROR
          "solidar_model public link interface must remain Qt-free")
endif()

solidar_add_test(app_settings_tests
  LIBRARIES solidar_theme Qt6::Core LABELS unit)
solidar_add_test(theme_manager_tests
  LIBRARIES solidar_theme LABELS unit)
solidar_add_test(settings_ui_tests
  LIBRARIES solidar_home solidar_theme Qt6::Widgets LABELS ui-smoke)
solidar_add_test(extrude_feature_tests
  LIBRARIES solidar_model LABELS unit)
solidar_add_test(revolve_feature_tests
  LIBRARIES solidar_model LABELS regression)
solidar_add_test(part_design_tool_framework_tests
  LIBRARIES solidar_editor LABELS unit)
solidar_add_test(part_design_ui_state_regression_tests
  LIBRARIES solidar_editor Qt6::Test LABELS unit)
solidar_add_test(tool_parameter_hud_tests
  LIBRARIES solidar_viewport3d Qt6::Widgets Qt6::Test LABELS unit regression)
solidar_add_test(extrude_geometry_regression_tests
  LIBRARIES solidar_model LABELS regression)
solidar_add_test(extrude_boolean_regression_tests
  LIBRARIES solidar_model LABELS regression)
solidar_add_test(extrude_history_regression_tests
  LIBRARIES solidar_model LABELS regression)
solidar_add_test(sketch_extrude_builder_tests
  LIBRARIES solidar_model LABELS unit)
solidar_add_test(face_extrude_tests
  LIBRARIES solidar_project Qt6::Core LABELS regression)
solidar_add_test(extrude_tool_session_tests
  LIBRARIES solidar_model LABELS regression)
solidar_add_test(pocket_feature_tests
  LIBRARIES solidar_model LABELS unit)
solidar_add_test(project_file_tests
  LIBRARIES solidar_project Qt6::Core LABELS regression)
solidar_add_test(project_decoder_property_tests
  LIBRARIES solidar_project Qt6::Core LABELS unit regression)
solidar_add_test(drawing_source_tests
  LIBRARIES solidar_drawing_ui solidar_project Qt6::Core LABELS regression)
solidar_add_test(stl_exporter_tests
  LIBRARIES solidar_project Qt6::Core LABELS regression)
solidar_add_test(step_exchange_tests
  LIBRARIES solidar_project Qt6::Core LABELS regression topology)
solidar_add_test(editor_smoke_tests
  LIBRARIES solidar_editor solidar_home Qt6::Widgets LABELS ui-smoke)
solidar_add_test(project_lifecycle_tests
  LIBRARIES solidar_project Qt6::Core LABELS regression)
solidar_add_test(project_application_service_tests
  LIBRARIES solidar_editor Qt6::Core LABELS regression)
solidar_add_test(project_switch_ui_tests
  LIBRARIES solidar_editor solidar_project Qt6::Widgets
  LABELS regression ui-smoke)
solidar_add_test(model_ribbon_tests
  LIBRARIES solidar_editor Qt6::Widgets LABELS ui-smoke)
solidar_add_test(tool_icon_tests
  LIBRARIES solidar_sketch_ui solidar_viewport3d solidar_theme Qt6::Widgets
  LABELS unit ui-smoke)
solidar_add_test(part_design_history_ui_tests
  LIBRARIES solidar_editor solidar_project Qt6::Widgets LABELS ui-smoke)
solidar_add_test(feature_removal_tests
  LIBRARIES solidar_model LABELS ui-smoke)
solidar_add_test(history_timeline_geometry_tests
  LIBRARIES solidar_viewport3d LABELS ui-smoke)
solidar_add_test(historical_preview_tests
  LIBRARIES solidar_editor LABELS regression ui-smoke)
solidar_add_test(sketch_regression_tests
  LIBRARIES solidar_model LABELS regression)
solidar_add_test(sketch_lock_constraint_tests
  LIBRARIES solidar_model LABELS unit regression)
solidar_add_test(sketch_canvas_axis_mapping_tests
  LIBRARIES solidar_sketch_ui solidar_viewport3d LABELS unit regression)
solidar_add_test(sketch_interaction_controller_tests
  LIBRARIES solidar_sketch_ui LABELS unit regression)
solidar_add_test(sketch_command_controller_tests
  LIBRARIES solidar_sketch_ui LABELS unit regression)
solidar_add_test(sketch_hit_tester_tests
  LIBRARIES solidar_sketch_ui LABELS unit regression)
solidar_add_test(sketch_renderer_tests
  LIBRARIES solidar_sketch_ui solidar_viewport3d Qt6::Widgets
  LABELS unit regression ui-smoke)
solidar_add_test(sketch_render_cache_ui_tests
  LIBRARIES solidar_sketch_ui solidar_viewport3d Qt6::Widgets
  LABELS regression ui-smoke)
solidar_add_test(sketch_rectangle_dimension_ui_tests
  LIBRARIES solidar_sketch_ui solidar_viewport3d solidar_project Qt6::Widgets
  LABELS regression ui-smoke)
solidar_add_test(body_render_mesh_tests
  LIBRARIES solidar_viewport3d LABELS unit)
solidar_add_test(body_render_mesh_normals_tests
  LIBRARIES solidar_viewport3d LABELS regression)
solidar_add_test(viewport_renderer_tests
  LIBRARIES solidar_viewport3d LABELS regression)
solidar_add_test(viewport_ux_polish_tests
  LIBRARIES solidar_viewport3d LABELS regression)
solidar_add_test(edge_selection_state_tests
  LIBRARIES solidar_viewport3d LABELS unit)
solidar_add_test(viewport_picking_tests
  LIBRARIES solidar_viewport3d LABELS unit)
solidar_add_test(preview_update_coordinator_tests
  LIBRARIES solidar_viewport3d Qt6::Test LABELS unit)
solidar_add_test(viewport_ruler_tests
  LIBRARIES solidar_viewport3d Qt6::Widgets LABELS unit)
solidar_add_test(numeric_parameter_state_tests
  LIBRARIES solidar_model LABELS unit)
solidar_add_test(viewport_manipulator_tests
  LIBRARIES solidar_viewport3d Qt6::Widgets LABELS unit)
solidar_add_test(viewport_preview_selection_tests
  LIBRARIES solidar_viewport3d LABELS unit)
solidar_add_test(viewport_direct_profile_tests
  LIBRARIES solidar_viewport3d Qt6::Widgets LABELS unit)
solidar_add_test(interaction_layer_tests
  LIBRARIES solidar_viewport3d Qt6::Widgets LABELS unit)
solidar_add_test(viewport_reset_tests
  LIBRARIES solidar_viewport3d Qt6::Widgets LABELS regression)
solidar_add_test(view_cube_tests
  LIBRARIES solidar_viewport3d LABELS regression ui-smoke)
solidar_add_test(world_grid_tests
  LIBRARIES solidar_viewport3d LABELS regression)
solidar_add_test(macro_foundation_tests
  LIBRARIES solidar_model LABELS unit)
solidar_add_test(fillet_feature_tests
  LIBRARIES solidar_model LABELS regression)
solidar_add_test(chamfer_feature_tests
  LIBRARIES solidar_project Qt6::Core LABELS regression)
solidar_add_test(join_bodies_feature_tests
  LIBRARIES solidar_project Qt6::Core LABELS regression)
solidar_add_test(parametric_feature_chain_tests
  LIBRARIES solidar_model LABELS regression)
solidar_add_test(part_design_regression_tests
  LIBRARIES solidar_project Qt6::Core LABELS regression)
solidar_add_test(downstream_feature_preservation_tests
  LIBRARIES solidar_project Qt6::Core LABELS regression)
solidar_add_test(shell_feature_tests
  LIBRARIES solidar_project Qt6::Core LABELS regression)
solidar_add_test(shell_complex_topology_tests
  LIBRARIES solidar_model LABELS regression)
solidar_add_test(draft_feature_tests
  LIBRARIES solidar_project Qt6::Core LABELS regression)
solidar_add_test(parametric_history_tests
  LIBRARIES solidar_model LABELS regression)

foreach(pattern_test move_feature_tests mirror_feature_tests
                     linear_pattern_feature_tests circular_pattern_feature_tests)
  solidar_add_test(${pattern_test}
    LIBRARIES solidar_model LABELS regression)
endforeach()

solidar_add_test(pattern_persistence_tests
  LIBRARIES solidar_project Qt6::Core LABELS regression)
solidar_add_test(multi_solid_edge_feature_tests
  LIBRARIES solidar_model LABELS regression)
solidar_add_test(persistent_topology_tests
  LIBRARIES solidar_model LABELS regression topology)
solidar_add_test(sketch_constraint_integrity_tests
  LIBRARIES solidar_model LABELS regression)

solidar_add_test(compliance_artifacts_tests
  COMMAND ${CMAKE_COMMAND}
          -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/compliance_artifacts_tests.cmake
  LABELS compliance)
solidar_add_test(release_artifact_script_tests
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/release_artifact_script_tests.py
  LABELS compliance)

# Fail configuration if a future direct add_test() bypasses the common
# registration contract anywhere in this directory. The deferred call runs
# after the complete CMakeLists.txt has been processed, so appending a direct
# add_test() below this block cannot evade the comparison.
function(solidar_verify_test_registration)
  get_property(registered_tests GLOBAL PROPERTY SOLIDAR_REGISTERED_TESTS)
  get_property(directory_tests DIRECTORY PROPERTY TESTS)
  list(SORT registered_tests)
  list(SORT directory_tests)
  if(NOT registered_tests STREQUAL directory_tests)
    message(FATAL_ERROR
            "Every test must be registered through solidar_add_test()")
  endif()
endfunction()
cmake_language(DEFER CALL solidar_verify_test_registration)
