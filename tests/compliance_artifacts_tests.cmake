cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR is required")
endif()

set(required_files
  LICENSE
  NOTICE
  DEPENDENCIES.md
  THIRD_PARTY_NOTICES.md
  LICENSES/README.md
  LICENSES/Qt/README.md
  LICENSES/Qt/qtbase/LGPL-3.0-only.txt
  LICENSES/Qt/qtbase/GPL-3.0-only.txt
  LICENSES/Qt/SBOM/qtbase-6.8.3.spdx
  LICENSES/Qt/SBOM/qtsvg-6.8.3.spdx
  LICENSES/OCCT/README.md
  docs/dependency-policy.md
  sbom/README.md
  sbom/components.json
  scripts/generate_sbom.py
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

file(READ "${SOURCE_DIR}/LICENSE" project_license)
string(FIND "${project_license}" "Mozilla Public License Version 2.0" mpl_position)
if(mpl_position EQUAL -1)
  message(FATAL_ERROR "LICENSE is not the Mozilla Public License Version 2.0")
endif()

file(READ "${SOURCE_DIR}/NOTICE" project_notice)
foreach(required_notice_text
        "Молотков Михаил Алексеевич"
        "https://github.com/ilikecinepol/SolidarCAD")
  string(FIND "${project_notice}" "${required_notice_text}" notice_position)
  if(notice_position EQUAL -1)
    message(FATAL_ERROR "NOTICE is missing required text: ${required_notice_text}")
  endif()
endforeach()

file(READ "${SOURCE_DIR}/sbom/solidarcad.spdx.json" sbom)
string(JSON spdx_version GET "${sbom}" spdxVersion)
if(NOT spdx_version STREQUAL "SPDX-2.3")
  message(FATAL_ERROR "SBOM must use SPDX-2.3")
endif()
string(JSON package_count LENGTH "${sbom}" packages)
if(package_count LESS 3)
  message(FATAL_ERROR "SBOM must describe SolidarCAD, Qt, and OCCT")
endif()

if(WIN32)
  find_program(PYTHON_LAUNCHER NAMES python python3 py REQUIRED)
  get_filename_component(python_name "${PYTHON_LAUNCHER}" NAME)
  if(python_name STREQUAL "py.exe")
    set(python_command "${PYTHON_LAUNCHER}" -3)
  else()
    set(python_command "${PYTHON_LAUNCHER}")
  endif()
else()
  find_program(PYTHON_LAUNCHER NAMES python3 python REQUIRED)
  set(python_command "${PYTHON_LAUNCHER}")
endif()
execute_process(
  COMMAND ${python_command} "${SOURCE_DIR}/scripts/generate_sbom.py"
          --root "${SOURCE_DIR}" --check
  RESULT_VARIABLE sbom_check_result
  OUTPUT_VARIABLE sbom_check_output
  ERROR_VARIABLE sbom_check_error)
if(NOT sbom_check_result EQUAL 0)
  message(FATAL_ERROR
          "Checked-in SBOM is stale or could not be checked with "
          "${PYTHON_LAUNCHER}: ${sbom_check_output}${sbom_check_error}")
endif()

foreach(required_text "Qt" "Open CASCADE" "NOASSERTION")
  string(FIND "${sbom}" "${required_text}" text_position)
  if(text_position EQUAL -1)
    message(FATAL_ERROR "SBOM is missing required text: ${required_text}")
  endif()
endforeach()

foreach(required_project_text
        "MPL-2.0"
        "Copyright (c) 2026 Молотков Михаил Алексеевич"
        "https://github.com/ilikecinepol/SolidarCAD")
  string(FIND "${sbom}" "${required_project_text}" project_text_position)
  if(project_text_position EQUAL -1)
    message(FATAL_ERROR "SBOM is missing project license text: ${required_project_text}")
  endif()
endforeach()

message(STATUS "Compliance artifacts are present and internally consistent")
