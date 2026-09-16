# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

set(UVM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/uvm-core")
if(NOT EXISTS "${UVM_ROOT}/src/uvm_pkg.sv")
    message(FATAL_ERROR "Missing external/uvm-core. Run git submodule update --init --recursive.")
endif()
file(GLOB_RECURSE UVM_FILES CONFIGURE_DEPENDS RELATIVE "${UVM_ROOT}"
    "${UVM_ROOT}/src/*" "${UVM_ROOT}/compat/*")
list(APPEND UVM_FILES LICENSE.txt NOTICE.txt README.md DEVIATIONS.md)
list(SORT UVM_FILES)
set(UVM_QRC "<RCC><qresource prefix=\"/uvm-core\">\n")
foreach(UVM_FILE IN LISTS UVM_FILES)
    string(REPLACE "&" "&amp;" UVM_SOURCE_PATH "${UVM_ROOT}/${UVM_FILE}")
    string(REPLACE "<" "&lt;" UVM_SOURCE_PATH "${UVM_SOURCE_PATH}")
    string(REPLACE ">" "&gt;" UVM_SOURCE_PATH "${UVM_SOURCE_PATH}")
    string(APPEND UVM_QRC "<file alias=\"${UVM_FILE}\">${UVM_SOURCE_PATH}</file>\n")
endforeach()
string(APPEND UVM_QRC "</qresource></RCC>\n")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/uvm_core.qrc" CONTENT "${UVM_QRC}" @ONLY)
target_sources(qsoc_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/uvm_core.qrc")
