cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR is required")
endif()

set(required_files
  DEPENDENCIES.md
  THIRD_PARTY_NOTICES.md
  LICENSES/README.md
  LICENSES/Qt/README.md
  LICENSES/OCCT/README.md
  docs/dependency-policy.md
  sbom/README.md
  sbom/solidarcad.spdx.json)

foreach(relative_path IN LISTS required_files)
  if(NOT EXISTS "${SOURCE_DIR}/${relative_path}")
    message(FATAL_ERROR "Missing compliance artifact: ${relative_path}")
  endif()
endforeach()

file(READ "${SOURCE_DIR}/vcpkg.json" vcpkg_manifest)
string(JSON baseline GET "${vcpkg_manifest}" builtin-baseline)
string(JSON dependency_name GET "${vcpkg_manifest}" dependencies 0 name)
if(NOT dependency_name STREQUAL "opencascade")
  message(FATAL_ERROR "Expected the direct vcpkg dependency to be opencascade")
endif()

file(READ "${SOURCE_DIR}/DEPENDENCIES.md" dependencies)
string(FIND "${dependencies}" "${baseline}" baseline_position)
if(baseline_position EQUAL -1)
  message(FATAL_ERROR "DEPENDENCIES.md does not contain the vcpkg baseline")
endif()

file(READ "${SOURCE_DIR}/sbom/solidarcad.spdx.json" sbom)
string(JSON spdx_version GET "${sbom}" spdxVersion)
if(NOT spdx_version STREQUAL "SPDX-2.3")
  message(FATAL_ERROR "SBOM must use SPDX-2.3")
endif()
string(JSON package_count LENGTH "${sbom}" packages)
if(package_count LESS 3)
  message(FATAL_ERROR "SBOM must describe SolidarCAD, Qt, and OCCT")
endif()

foreach(required_text "Qt" "Open CASCADE" "NOASSERTION")
  string(FIND "${sbom}" "${required_text}" text_position)
  if(text_position EQUAL -1)
    message(FATAL_ERROR "SBOM is missing required text: ${required_text}")
  endif()
endforeach()

message(STATUS "Compliance artifacts are present and internally consistent")
