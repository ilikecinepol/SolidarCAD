cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR is required")
endif()

set(corpus_manifest
    "${SOURCE_DIR}/tests/fixtures/sketch-solver-corpus/manifest.json")
if(NOT EXISTS "${corpus_manifest}")
  message(FATAL_ERROR "Versioned sketch-solver corpus manifest is missing")
endif()
file(READ "${corpus_manifest}" corpus)
string(JSON corpus_schema GET "${corpus}" schema)
if(NOT corpus_schema STREQUAL "solidarcad-sketch-solver-corpus-v1")
  message(FATAL_ERROR "Unexpected corpus schema: ${corpus_schema}")
endif()
string(JSON corpus_case_count LENGTH "${corpus}" cases)
if(corpus_case_count LESS 10)
  message(FATAL_ERROR "Corpus is too small: ${corpus_case_count} cases")
endif()
math(EXPR corpus_last_case "${corpus_case_count} - 1")
foreach(case_index RANGE 0 ${corpus_last_case})
  string(JSON case_test GET "${corpus}" cases ${case_index} test)
  string(JSON case_fixture GET "${corpus}" cases ${case_index} fixture)
  string(JSON case_expected GET "${corpus}" cases ${case_index} expected)
  if(case_test STREQUAL "stage6_sketch_solver_benchmark")
    set(case_source
        "${SOURCE_DIR}/benchmarks/stage6_sketch_solver_benchmark.cpp")
  else()
    set(case_source "${SOURCE_DIR}/tests/${case_test}.cpp")
  endif()
  if(NOT EXISTS "${case_source}")
    message(FATAL_ERROR "Corpus test source is missing: ${case_source}")
  endif()
  file(READ "${case_source}" case_source_content)
  string(FIND "${case_source_content}" "${case_fixture}" fixture_position)
  if(fixture_position EQUAL -1)
    message(FATAL_ERROR
            "Corpus fixture '${case_fixture}' is missing from ${case_source}")
  endif()
  if(case_expected STREQUAL "")
    message(FATAL_ERROR "Corpus case ${case_index} has no expected contract")
  endif()
endforeach()
foreach(required_case
        "all-constraint-types"
        "constraint-order-permutations"
        "under-full-over-status"
        "tangent-line-circle-arc-bezier"
        "rectangle-slot-polygon-chain"
        "mirror-trim-undo"
        "delete-undo-redo-identities"
        "persistence-v1-v2-v3"
        "explicit-topology-only"
        "large-independent-connected")
  string(FIND "${corpus}" "${required_case}" case_position)
  if(case_position EQUAL -1)
    message(FATAL_ERROR "Corpus case is missing: ${required_case}")
  endif()
endforeach()

file(GLOB_RECURSE solver_sources
     "${SOURCE_DIR}/src/sketch/*.cpp"
     "${SOURCE_DIR}/src/sketch/*.h")
list(APPEND solver_sources
     "${SOURCE_DIR}/benchmarks/stage6_sketch_solver_benchmark.cpp")

foreach(source IN LISTS solver_sources)
  file(READ "${source}" content)
  foreach(forbidden
          "BasicSketchSolver"
          "solveStableHistoricalForBenchmark"
          "solveStableLowLevel"
          "setLineLengthById"
          "setCircleDiameterById"
          "setLineHorizontalById"
          "setLineVerticalById"
          "setLinesParallelByIds"
          "setParallelLineDistanceByIds"
          "setLineAngleByIds"
          "setPointsCoincident"
          "setPointOnLine"
          "setPointOnCircle"
          "setPointOnArc"
          "setPointToMidpoint"
          "setPointOnXAxis"
          "setPointOnYAxis"
          "setCircleTangentToLine"
          "setArcTangentToLine"
          "setGeometriesTangentByIds"
          "setPointDistance"
          "setPointDistanceX"
          "setPointDistanceY")
    string(FIND "${content}" "${forbidden}" position)
    if(NOT position EQUAL -1)
      file(RELATIVE_PATH relative "${SOURCE_DIR}" "${source}")
      message(FATAL_ERROR
              "Removed Solver-v1 symbol '${forbidden}' remains in ${relative}")
    endif()
  endforeach()
endforeach()

file(READ "${SOURCE_DIR}/benchmarks/stage6_sketch_solver_benchmark.cpp"
     benchmark_source)
string(FIND "${benchmark_source}" "historical" historical_position)
if(NOT historical_position EQUAL -1)
  message(FATAL_ERROR "Stage-6 benchmark still has a Solver-v1 branch")
endif()

message(STATUS "Sketch solver cutover has one production implementation")
