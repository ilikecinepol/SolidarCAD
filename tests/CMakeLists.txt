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
solidar_add_test(product_document_tests
  LIBRARIES solidar_model LABELS unit regression)
solidar_add_test(assembly_definition_tests
  LIBRARIES solidar_model LABELS unit regression)
solidar_add_test(component_definition_tests
  LIBRARIES solidar_model LABELS unit regression)
solidar_add_test(component_extraction_tests
  LIBRARIES solidar_model LABELS unit regression topology)
solidar_add_test(component_placement_tests
  LIBRARIES solidar_model LABELS unit regression)
solidar_add_test(component_placement_drag_tests
  LIBRARIES solidar_viewport3d LABELS unit regression)
solidar_add_test(assembly_mate_solver_tests
  LIBRARIES solidar_model LABELS unit regression topology)
solidar_add_test(assembly_presentation_builder_tests
  LIBRARIES solidar_presentation LABELS unit regression)
solidar_add_test(component_presentation_builder_tests
  LIBRARIES solidar_presentation LABELS unit regression topology)
solidar_add_test(assembly_scene_publication_coordinator_tests
  LIBRARIES solidar_presentation LABELS unit regression)
solidar_add_test(component_scene_publication_coordinator_tests
  LIBRARIES solidar_presentation LABELS unit regression)
solidar_add_test(runtime_topology_diagnostics_tests
  LIBRARIES solidar_model LABELS unit regression topology)
solidar_add_test(model_command_history_tests
  LIBRARIES solidar_editor LABELS unit regression)
solidar_add_test(component_extraction_history_tests
  LIBRARIES solidar_editor LABELS unit regression topology)
solidar_add_test(component_placement_history_tests
  LIBRARIES solidar_editor LABELS unit regression)
solidar_add_test(model_workspace_projection_successor_tests
  LIBRARIES solidar_editor Qt6::Widgets LABELS unit regression topology ui-smoke)

# This target intentionally neither links solidar_model nor receives any Qt
# target/include directory. It compiles the public headers using only the
# repository and OCCT include roots, so a Qt include in the model API fails.
solidar_add_test(model_public_headers_compile_tests
  SOURCES model_public_headers_compile_tests.cpp
          shape_feature_public_header_compile.cpp
  LABELS compliance)
target_include_directories(model_public_headers_compile_tests
  PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(model_public_headers_compile_tests SYSTEM
  PRIVATE ${OpenCASCADE_INCLUDE_DIR})
target_compile_features(model_public_headers_compile_tests PRIVATE cxx_std_20)

# Presentation public headers are compiled without linking the implementation
# and without any Qt target/include directory.  The configure-time link check
# below also keeps the library's public dependency surface exactly model-only.
solidar_add_test(assembly_presentation_public_headers_compile_tests
  SOURCES assembly_presentation_public_headers_compile_tests.cpp
  LABELS compliance)
target_include_directories(assembly_presentation_public_headers_compile_tests
  PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(assembly_presentation_public_headers_compile_tests SYSTEM
  PRIVATE ${OpenCASCADE_INCLUDE_DIR})
target_compile_features(assembly_presentation_public_headers_compile_tests
  PRIVATE cxx_std_20)

solidar_add_test(component_presentation_public_headers_compile_tests
  SOURCES component_presentation_public_headers_compile_tests.cpp
  LABELS compliance)
target_include_directories(component_presentation_public_headers_compile_tests
  PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(component_presentation_public_headers_compile_tests SYSTEM
  PRIVATE ${OpenCASCADE_INCLUDE_DIR})
target_compile_features(component_presentation_public_headers_compile_tests
  PRIVATE cxx_std_20)

# The unified assembly history header is compiled without Qt include paths or
# implementation linkage. Its closed cold command variants must remain free of
# QObject/publication/runtime-geometry ownership.
solidar_add_test(assembly_command_history_public_headers_compile_tests
  SOURCES assembly_command_history_public_headers_compile_tests.cpp
  LABELS compliance)
target_include_directories(assembly_command_history_public_headers_compile_tests
  PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(assembly_command_history_public_headers_compile_tests
  PRIVATE cxx_std_20)
target_compile_definitions(assembly_command_history_public_headers_compile_tests
  PRIVATE
    "SOLIDAR_ASSEMBLY_COMMAND_HISTORY_HEADER=\"${CMAKE_SOURCE_DIR}/src/ui/application/AssemblyCommandHistory.h\""
    "SOLIDAR_OCCURRENCE_DELTA_HEADER=\"${CMAKE_SOURCE_DIR}/src/model/OccurrenceDelta.h\""
    "SOLIDAR_OCCURRENCE_DUPLICATE_HEADER=\"${CMAKE_SOURCE_DIR}/src/model/OccurrenceDuplicate.h\""
    "SOLIDAR_OCCURRENCE_REMOVE_HEADER=\"${CMAKE_SOURCE_DIR}/src/model/OccurrenceRemove.h\""
    "SOLIDAR_PRODUCT_DOCUMENT_HEADER=\"${CMAKE_SOURCE_DIR}/src/model/ProductDocument.h\"")

get_target_property(solidar_presentation_public_links
                    solidar_presentation INTERFACE_LINK_LIBRARIES)
if(NOT "${solidar_presentation_public_links}" STREQUAL "solidar_model")
  message(FATAL_ERROR
          "solidar_presentation public link interface must contain only solidar_model")
endif()

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
solidar_add_test(extrude_topology_lineage_tests
  LIBRARIES solidar_model LABELS unit regression topology)
solidar_add_test(production_edge_lineage_tests
  LIBRARIES solidar_model LABELS unit regression topology)
solidar_add_test(transformed_feature_topology_lineage_tests
  LIBRARIES solidar_model LABELS unit regression topology)
solidar_add_test(revolve_feature_tests
  LIBRARIES solidar_model LABELS regression)
solidar_add_test(revolve_topology_lineage_tests
  LIBRARIES solidar_model LABELS unit regression topology)
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
solidar_add_test(pocket_topology_lineage_tests
  LIBRARIES solidar_model LABELS unit regression topology)
solidar_add_test(project_file_tests
  LIBRARIES solidar_project Qt6::Core LABELS regression)
solidar_add_test(product_envelope_v5_decoder_tests
  LIBRARIES solidar_project Qt6::Core LABELS unit regression)
target_compile_definitions(product_envelope_v5_decoder_tests PRIVATE
  "SOLIDAR_MINIMAL_V5_FUZZ_SEED_PATH=\"${CMAKE_SOURCE_DIR}/fuzz/corpus/project_decoder/minimal-v5.solidar\"")
solidar_add_test(product_envelope_v5_encoder_tests
  LIBRARIES solidar_project Qt6::Core LABELS unit regression topology)
solidar_add_test(product_envelope_v5_conversion_tests
  LIBRARIES solidar_project Qt6::Core LABELS unit regression topology)
solidar_add_test(product_envelope_v5_atomic_writer_tests
  LIBRARIES solidar_project Qt6::Core LABELS unit regression)
solidar_add_test(product_envelope_v5_materializer_tests
  LIBRARIES solidar_project Qt6::Core LABELS unit regression topology)
solidar_add_test(linked_part_source_resolver_tests
  LIBRARIES solidar_project Qt6::Core LABELS unit regression)
solidar_add_test(linked_part_source_resolver_native_tests
  LIBRARIES solidar_project Qt6::Core LABELS regression)
solidar_add_test(linked_part_source_resolver_public_headers_compile_tests
  LIBRARIES solidar_project Qt6::Core LABELS compliance)
solidar_add_test(product_envelope_v6_decoder_tests
  LIBRARIES solidar_project Qt6::Core LABELS unit regression)
target_compile_definitions(product_envelope_v6_decoder_tests PRIVATE
  "SOLIDAR_MINIMAL_V6_FIXTURE_PATH=\"${CMAKE_SOURCE_DIR}/fuzz/corpus/project_decoder/minimal-v6-assembly.solidar\"")
solidar_add_test(product_envelope_v6_encoder_tests
  LIBRARIES solidar_project Qt6::Core LABELS unit regression topology)
target_compile_definitions(product_envelope_v6_encoder_tests PRIVATE
  "SOLIDAR_MINIMAL_V6_FIXTURE_PATH=\"${CMAKE_SOURCE_DIR}/fuzz/corpus/project_decoder/minimal-v6-assembly.solidar\"")
solidar_add_test(product_envelope_v6_materializer_tests
  LIBRARIES solidar_project Qt6::Core LABELS unit regression topology)
target_compile_definitions(product_envelope_v6_materializer_tests PRIVATE
  "SOLIDAR_MINIMAL_V6_FIXTURE_PATH=\"${CMAKE_SOURCE_DIR}/fuzz/corpus/project_decoder/minimal-v6-assembly.solidar\"")
solidar_add_test(product_envelope_v7_tests
  LIBRARIES solidar_project Qt6::Core LABELS unit regression topology)
solidar_add_test(assembly_graph_analysis_tests
  LIBRARIES solidar_model LABELS unit regression topology)
# A6c1a recursive presentation/UI identity extends existing focused targets.
# V8 editor routing and native nested GPU evidence deliberately stay A6c1b.
solidar_add_test(product_envelope_v8_tests
  LIBRARIES solidar_project Qt6::Core LABELS unit regression topology)
solidar_add_test(product_envelope_v8_public_headers_compile_tests
  LIBRARIES solidar_project Qt6::Core LABELS compliance)
solidar_add_test(product_envelope_v9_tests
  LIBRARIES solidar_project Qt6::Core LABELS unit regression topology)
solidar_add_test(product_envelope_v9_atomic_writer_tests
  LIBRARIES solidar_project Qt6::Core LABELS unit regression topology)
solidar_add_test(product_envelope_v9_public_headers_compile_tests
  LIBRARIES solidar_project Qt6::Core LABELS compliance)
solidar_add_test(product_envelope_v10_tests
  LIBRARIES solidar_project Qt6::Core LABELS unit regression topology)
solidar_add_test(product_envelope_v10_public_headers_compile_tests
  LIBRARIES solidar_project Qt6::Core LABELS compliance)
solidar_add_test(product_envelope_v11_tests
  LIBRARIES solidar_project Qt6::Core LABELS unit regression topology)
solidar_add_test(product_envelope_v11_public_headers_compile_tests
  LIBRARIES solidar_project Qt6::Core LABELS compliance)
solidar_add_test(product_envelope_v12_tests
  LIBRARIES solidar_project Qt6::Core LABELS unit regression topology)
solidar_add_test(product_envelope_v12_public_headers_compile_tests
  LIBRARIES solidar_project Qt6::Core LABELS compliance)
solidar_add_test(project_decoder_property_tests
  LIBRARIES solidar_project Qt6::Core LABELS unit regression)
solidar_add_test(project_format_v4_reference_decoder_tests
  LIBRARIES solidar_project Qt6::Core LABELS unit regression topology)
solidar_add_test(project_format_v4_reference_encoder_tests
  LIBRARIES solidar_project Qt6::Core LABELS unit regression topology)
solidar_add_test(project_format_v4_document_encoder_tests
  LIBRARIES solidar_project Qt6::Core LABELS unit regression topology)
solidar_add_test(project_format_v4_document_materializer_tests
  LIBRARIES solidar_project Qt6::Core LABELS unit regression topology)
solidar_add_test(project_format_v4_application_gate_tests
  LIBRARIES solidar_editor Qt6::Core LABELS unit regression topology)
solidar_add_test(project_atomic_writer_tests
  LIBRARIES solidar_project Qt6::Core LABELS unit regression)
solidar_add_test(project_format_v4_atomic_writer_tests
  LIBRARIES solidar_project Qt6::Core LABELS unit regression topology)
solidar_add_test(project_format_v4_envelope_decoder_tests
  LIBRARIES solidar_project Qt6::Core LABELS unit regression topology)
solidar_add_test(project_format_v4_payload_scanner_tests
  LIBRARIES solidar_project Qt6::Core LABELS unit regression topology)
solidar_add_test(project_format_v4_topology_aggregator_tests
  LIBRARIES solidar_project Qt6::Core LABELS unit regression topology)
solidar_add_test(project_format_v4_legacy_sketch_validator_tests
  LIBRARIES solidar_project Qt6::Core LABELS unit regression topology)
solidar_add_test(project_format_v4_model_validator_tests
  LIBRARIES solidar_project Qt6::Core LABELS unit regression topology)
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
solidar_add_test(project_v10_application_routing_tests
  LIBRARIES solidar_editor Qt6::Core LABELS unit regression topology)
target_compile_definitions(project_application_service_tests PRIVATE
  "SOLIDAR_MINIMAL_V6_FIXTURE_PATH=\"${CMAKE_SOURCE_DIR}/fuzz/corpus/project_decoder/minimal-v6-assembly.solidar\"")
solidar_add_test(product_conversion_application_service_tests
  LIBRARIES solidar_editor Qt6::Core LABELS unit regression topology)
solidar_add_test(product_clone_application_service_tests
  LIBRARIES solidar_editor Qt6::Core LABELS unit regression topology)
solidar_add_test(product_relocation_application_service_tests
  LIBRARIES solidar_editor Qt6::Core LABELS unit regression topology)
solidar_add_test(project_switch_ui_tests
  LIBRARIES solidar_editor solidar_project Qt6::Widgets
  LABELS regression ui-smoke)
solidar_add_test(main_window_v10_routing_tests
  LIBRARIES solidar_editor solidar_project Qt6::Widgets
  LABELS unit regression topology ui-smoke)
target_compile_definitions(project_switch_ui_tests PRIVATE
  "SOLIDAR_MINIMAL_V6_FIXTURE_PATH=\"${CMAKE_SOURCE_DIR}/fuzz/corpus/project_decoder/minimal-v6-assembly.solidar\""
  "SOLIDAR_FULL_V9_FIXTURE_PATH=\"${CMAKE_SOURCE_DIR}/fuzz/corpus/project_decoder/full-v9-v2-mate.solidar\"")
solidar_add_test(model_ribbon_tests
  LIBRARIES solidar_editor Qt6::Widgets LABELS ui-smoke)
solidar_add_test(tool_icon_tests
  LIBRARIES solidar_sketch_ui solidar_viewport3d solidar_theme Qt6::Widgets
  LABELS unit ui-smoke)
solidar_add_test(part_design_history_ui_tests
  LIBRARIES solidar_editor solidar_project Qt6::Widgets LABELS ui-smoke)
solidar_add_test(feature_removal_tests
  LIBRARIES solidar_model LABELS regression)
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
solidar_add_test(assembly_render_scene_tests
  LIBRARIES solidar_viewport3d LABELS unit regression)
solidar_add_test(component_viewport_scene_tests
  LIBRARIES solidar_viewport3d LABELS unit regression topology)
solidar_add_test(component_viewport_picking_tests
  LIBRARIES solidar_viewport3d LABELS unit regression topology)
solidar_add_test(component_viewport_session_tests
  LIBRARIES solidar_viewport3d LABELS unit regression topology)
solidar_add_test(component_viewport_public_headers_compile_tests
  LIBRARIES solidar_viewport3d LABELS unit compliance)
solidar_add_test(assembly_viewport_picking_tests
  LIBRARIES solidar_viewport3d LABELS unit regression topology)
solidar_add_test(assembly_viewport_spatial_tests
  LIBRARIES solidar_viewport3d LABELS unit regression topology)
solidar_add_test(assembly_viewport_session_tests
  LIBRARIES solidar_viewport3d LABELS unit regression topology)
solidar_add_test(assembly_insert_placement_session_tests
  LIBRARIES solidar_editor Qt6::Core LABELS unit regression ui-smoke)
solidar_add_test(assembly_mate_tool_session_tests
  LIBRARIES solidar_editor Qt6::Core LABELS unit regression ui-smoke topology)
solidar_add_test(assembly_mate_ui_tests
  LIBRARIES solidar_editor Qt6::Widgets LABELS regression ui-smoke topology)
solidar_add_test(assembly_command_history_tests
  LIBRARIES solidar_editor Qt6::Core LABELS unit regression topology)
solidar_add_test(linked_part_lifecycle_application_service_tests
  LIBRARIES solidar_editor Qt6::Core LABELS unit regression topology)
solidar_add_test(assembly_insert_application_service_tests
  LIBRARIES solidar_editor solidar_project Qt6::Core LABELS unit regression)
target_compile_definitions(assembly_insert_application_service_tests PRIVATE
  "SOLIDAR_MINIMAL_V6_FIXTURE_PATH=\"${CMAKE_SOURCE_DIR}/fuzz/corpus/project_decoder/minimal-v6-assembly.solidar\"")
solidar_add_test(assembly_tree_model_tests
  LIBRARIES solidar_editor Qt6::Core LABELS unit regression ui-smoke)
solidar_add_test(component_tree_model_tests
  LIBRARIES solidar_editor Qt6::Core LABELS unit regression ui-smoke)
solidar_add_test(component_workspace_tests
  LIBRARIES solidar_editor Qt6::Widgets LABELS unit regression ui-smoke)
solidar_add_test(assembly_visibility_ui_failure_tests
  LIBRARIES solidar_editor solidar_project Qt6::Widgets
  LABELS unit regression ui-smoke)
solidar_add_test(body_render_mesh_normals_tests
  LIBRARIES solidar_viewport3d LABELS regression)
solidar_add_test(viewport_renderer_tests
  LIBRARIES solidar_viewport3d LABELS regression)

# Visible, opt-in, real-driver harness. Deliberately not registered in CTest:
# CI may use an offscreen/software context and cannot satisfy the A4b/A4c1
# GPU gates.
solidar_add_test(assembly_gpu_smoke_classification_tests LABELS unit)
add_executable(assembly_renderer_gpu_smoke assembly_renderer_gpu_smoke.cpp)
target_link_libraries(assembly_renderer_gpu_smoke PRIVATE
  solidar_viewport3d Qt6::Gui Qt6::OpenGL)
add_executable(assembly_workspace_gpu_smoke assembly_workspace_gpu_smoke.cpp)
target_link_libraries(assembly_workspace_gpu_smoke PRIVATE
  solidar_editor solidar_project Qt6::Widgets Qt6::OpenGLWidgets)
target_compile_definitions(assembly_workspace_gpu_smoke PRIVATE
  "SOLIDAR_MINIMAL_V6_FIXTURE_PATH=\"${CMAKE_SOURCE_DIR}/fuzz/corpus/project_decoder/minimal-v6-assembly.solidar\"")
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
solidar_add_test(component_viewport_integration_tests
  LIBRARIES solidar_viewport3d Qt6::Widgets LABELS unit regression ui-smoke topology)
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
  LIBRARIES solidar_project Qt6::Core LABELS regression topology)
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
solidar_add_test(edge_reference_v2_consumer_tests
  LIBRARIES solidar_editor LABELS unit regression topology)
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
solidar_add_test(topology_lineage_tests
  LIBRARIES solidar_model LABELS unit regression topology)
solidar_add_test(topology_history_capture_tests
  LIBRARIES solidar_model LABELS unit regression topology)
solidar_add_test(boolean_topology_history_capture_tests
  LIBRARIES solidar_model LABELS unit regression topology)
solidar_add_test(face_boundary_edge_provenance_tests
  LIBRARIES solidar_model LABELS unit regression topology)
solidar_add_test(face_prism_topology_history_capture_tests
  LIBRARIES solidar_model LABELS unit regression topology)
solidar_add_test(revolve_topology_history_capture_tests
  LIBRARIES solidar_model LABELS unit regression topology)
solidar_add_test(face_extrude_topology_lineage_tests
  LIBRARIES solidar_model LABELS unit regression topology)
solidar_add_test(sketch_constraint_integrity_tests
  LIBRARIES solidar_model LABELS regression)
solidar_add_test(sketch_schema_tests
  LIBRARIES solidar_model LABELS unit regression)
solidar_add_test(sketch_equation_kernel_tests
  LIBRARIES solidar_model LABELS unit regression)
solidar_add_test(sketch_nonlinear_solver_tests
  LIBRARIES solidar_model LABELS unit regression)
solidar_add_test(sketch_solver_cutover_compliance_tests
  COMMAND ${CMAKE_COMMAND}
          -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/sketch_solver_cutover_compliance_tests.cmake
  LABELS compliance regression)

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
