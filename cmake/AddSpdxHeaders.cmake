# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

# CMake module to add SPDX headers to source files
# Usage: include(AddSpdxHeaders)
#        add_spdx_headers(TARGET <target_name>)

# Get current year for copyright notice
string(TIMESTAMP CURRENT_YEAR "%Y")

# Try to find git executable
find_program(GIT_EXECUTABLE git)

# Function to get git user info
function(_get_git_user_info NAME_VAR EMAIL_VAR)
    # Check if git is available
    if(GIT_EXECUTABLE)
        # Try to get user name from git config
        execute_process(
            COMMAND ${GIT_EXECUTABLE} config --get user.name
            OUTPUT_VARIABLE GIT_USER_NAME
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        
        # Try to get user email from git config
        execute_process(
            COMMAND ${GIT_EXECUTABLE} config --get user.email
            OUTPUT_VARIABLE GIT_USER_EMAIL
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        
        # Check if both name and email were found
        if(GIT_USER_NAME AND GIT_USER_EMAIL)
            set(${NAME_VAR} "${GIT_USER_NAME}" PARENT_SCOPE)
            set(${EMAIL_VAR} "${GIT_USER_EMAIL}" PARENT_SCOPE)
            return()
        endif()
    endif()
    
    # If git is not available or user info not found, return empty values
    set(${NAME_VAR} "" PARENT_SCOPE)
    set(${EMAIL_VAR} "" PARENT_SCOPE)
endfunction()

# Function to get file's first commit year from git history
function(_get_git_file_first_year FILE_PATH YEAR_VAR)
    # Check if git is available
    if(GIT_EXECUTABLE)
        # Try to get first commit year for the file
        # %ad: author date, --date=format:%Y: format as year only
        execute_process(
            COMMAND ${GIT_EXECUTABLE} log --follow --format=%ad --date=format:%Y --reverse -- "${FILE_PATH}"
            OUTPUT_VARIABLE GIT_YEARS
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        
        # Get the first line (first year)
        if(GIT_YEARS)
            string(REGEX REPLACE "\n.*" "" FIRST_YEAR "${GIT_YEARS}")
            if(FIRST_YEAR MATCHES "^[0-9]+$")
                set(${YEAR_VAR} "${FIRST_YEAR}" PARENT_SCOPE)
                return()
            endif()
        endif()
    endif()
    
    # If git is not available or file history not found, return empty value
    set(${YEAR_VAR} "" PARENT_SCOPE)
endfunction()

# Function to add SPDX headers to source files
function(add_spdx_headers)
    # Parse arguments
    set(options "")
    set(oneValueArgs TARGET LICENSE COPYRIGHT_HOLDER COPYRIGHT_YEAR_START)
    set(multiValueArgs "")
    cmake_parse_arguments(SPDX "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    # Check if TARGET is provided
    if(NOT SPDX_TARGET)
        message(FATAL_ERROR "TARGET must be specified for add_spdx_headers")
        return()
    endif()

    # Set default values if not provided
    if(NOT SPDX_LICENSE)
        set(SPDX_LICENSE "Apache-2.0")
    endif()

    # Try to get git user info if copyright holder is not specified
    if(NOT SPDX_COPYRIGHT_HOLDER)
        _get_git_user_info(GIT_USER_NAME GIT_USER_EMAIL)
        if(GIT_USER_NAME AND GIT_USER_EMAIL)
            set(SPDX_COPYRIGHT_HOLDER "${GIT_USER_NAME} <${GIT_USER_EMAIL}>")
            message(STATUS "Using git user info for copyright: ${SPDX_COPYRIGHT_HOLDER}")
        else()
            set(SPDX_COPYRIGHT_HOLDER "Huang Rui <vowstar@gmail.com>")
            message(STATUS "Using default copyright holder: ${SPDX_COPYRIGHT_HOLDER}")
        endif()
    endif()

    if(NOT SPDX_COPYRIGHT_YEAR_START)
        set(SPDX_COPYRIGHT_YEAR_START "2023")
    endif()

    # Get all source files from the target
    get_target_property(target_sources ${SPDX_TARGET} SOURCES)
    
    # Filter for C++ source and header files
    set(cpp_files "")
    foreach(source ${target_sources})
        if(source MATCHES "\\.(cpp|h|hpp)$")
            list(APPEND cpp_files ${source})
        endif()
    endforeach()

    # Create custom target for adding SPDX headers
    add_custom_target(
        ${SPDX_TARGET}_add_spdx_headers
        COMMENT "Adding SPDX headers to ${SPDX_TARGET} source files"
    )

    # Process each source file
    foreach(source ${cpp_files})
        # Get absolute path
        get_filename_component(source_abs "${source}" ABSOLUTE)
        
        # Try to get first commit year for this file from git
        _get_git_file_first_year("${source_abs}" GIT_FIRST_YEAR)
        
        # Set copyright year range - use git first year if available, otherwise use default
        set(START_YEAR "${SPDX_COPYRIGHT_YEAR_START}")
        if(GIT_FIRST_YEAR)
            set(START_YEAR "${GIT_FIRST_YEAR}")
            message(STATUS "Using git first commit year for ${source}: ${START_YEAR}")
        endif()
        
        # Set copyright year range
        set(COPYRIGHT_YEAR "${START_YEAR}")
        if(NOT START_YEAR STREQUAL CURRENT_YEAR)
            set(COPYRIGHT_YEAR "${START_YEAR}-${CURRENT_YEAR}")
        endif()
        
        # Add custom command to add SPDX header if not already present
        add_custom_command(
            TARGET ${SPDX_TARGET}_add_spdx_headers
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E echo "Processing ${source}"
            COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/AddSpdxHeadersHelper.cmake
                "${source_abs}" "${SPDX_LICENSE}" "${SPDX_COPYRIGHT_HOLDER}" "${COPYRIGHT_YEAR}"
            VERBATIM
        )
    endforeach()
    
    # Make the main target depend on the SPDX headers target
    add_dependencies(${SPDX_TARGET} ${SPDX_TARGET}_add_spdx_headers)
endfunction()