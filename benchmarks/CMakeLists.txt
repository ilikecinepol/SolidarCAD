add_executable(stage4_recompute_topology_benchmark
               stage4_recompute_topology_benchmark.cpp)
target_link_libraries(stage4_recompute_topology_benchmark
                      PRIVATE solidar_model)

add_executable(stage5_viewport_scalability_benchmark
               stage5_viewport_scalability_benchmark.cpp)
target_compile_features(stage5_viewport_scalability_benchmark PRIVATE cxx_std_20)
target_link_libraries(stage5_viewport_scalability_benchmark
                      PRIVATE solidar_viewport3d Qt6::Core)
set_target_properties(stage5_viewport_scalability_benchmark PROPERTIES
                      AUTOMOC OFF AUTORCC OFF)
if(WIN32)
  target_link_libraries(stage5_viewport_scalability_benchmark PRIVATE Psapi)
endif()
solidar_warnings(stage5_viewport_scalability_benchmark)

add_executable(stage6_sketch_solver_benchmark
               stage6_sketch_solver_benchmark.cpp)
target_compile_features(stage6_sketch_solver_benchmark PRIVATE cxx_std_20)
target_link_libraries(stage6_sketch_solver_benchmark PRIVATE solidar_model)
solidar_warnings(stage6_sketch_solver_benchmark)
