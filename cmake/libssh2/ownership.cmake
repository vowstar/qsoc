# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(QSOC_SFTP_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/patched-libssh2/sftp.c")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/patch-sftp.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/external/libssh2/src/sftp.c"
        "${CMAKE_CURRENT_LIST_DIR}/sftp-dtor.patch" "${QSOC_SFTP_SOURCE}"
    COMMAND_ERROR_IS_FATAL ANY
)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/patch-sftp.py"
    "${CMAKE_CURRENT_LIST_DIR}/sftp-dtor.patch"
    "${CMAKE_CURRENT_SOURCE_DIR}/external/libssh2/src/sftp.c"
)
get_target_property(QSOC_SSH_SOURCES libssh2_static SOURCES)
list(FILTER QSOC_SSH_SOURCES EXCLUDE REGEX "(^|/)sftp[.]c$")
set_property(TARGET libssh2_static PROPERTY SOURCES "${QSOC_SSH_SOURCES}")
target_sources(libssh2_static PRIVATE "${QSOC_SFTP_SOURCE}")
target_include_directories(libssh2_static PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/external/libssh2/src")
