if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR MSVC)
  message(FATAL_ERROR
          "Project decoder fuzzing requires Clang with its non-MSVC driver")
endif()
if(NOT SOLIDAR_ENABLE_SANITIZERS)
  message(FATAL_ERROR
          "Project decoder fuzzing requires SOLIDAR_ENABLE_SANITIZERS=ON")
endif()

add_executable(project_decoder_fuzz project_decoder_fuzz.cpp)
target_compile_features(project_decoder_fuzz PRIVATE cxx_std_20)
target_compile_options(project_decoder_fuzz PRIVATE -fsanitize=fuzzer)
target_link_options(project_decoder_fuzz PRIVATE -fsanitize=fuzzer)
target_link_libraries(project_decoder_fuzz PRIVATE solidar_project Qt6::Core)
set_target_properties(project_decoder_fuzz PROPERTIES AUTOMOC OFF AUTORCC OFF)
solidar_warnings(project_decoder_fuzz)
