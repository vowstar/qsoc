# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

# Every src header with a Q_OBJECT, Q_GADGET or Q_NAMESPACE class must have
# its moc output included by the .cpp of the same name. AUTOMOC otherwise
# bundles it into one mocs_compilation object per target, and any binary
# that references one meta-object then links the whole static library.
#
# Usage: cmake -DSOURCE_DIR=<repo> -P check_moc_includes.cmake

file(GLOB_RECURSE headers RELATIVE "${SOURCE_DIR}" "${SOURCE_DIR}/src/*.h")
set(missing "")
foreach(header IN LISTS headers)
    file(STRINGS "${SOURCE_DIR}/${header}" macros REGEX "^[ \t]*(Q_OBJECT|Q_GADGET|Q_NAMESPACE)")
    if(NOT macros)
        continue()
    endif()
    string(REGEX REPLACE "\\.h$" ".cpp" source "${header}")
    if(NOT EXISTS "${SOURCE_DIR}/${source}")
        continue()
    endif()
    get_filename_component(base "${header}" NAME_WE)
    file(STRINGS "${SOURCE_DIR}/${source}" includes REGEX "^#include \"moc_${base}\\.cpp\"")
    if(NOT includes)
        list(APPEND missing "${source}")
    endif()
endforeach()

if(missing)
    list(JOIN missing "\n  " lines)
    message(FATAL_ERROR "missing #include \"moc_<name>.cpp\" at the end of:\n  ${lines}")
endif()
