# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

# Helper script to add SPDX headers to source files
# Called by the add_spdx_headers function

# Check if file path is provided
if(CMAKE_ARGC LESS 5)
    message(FATAL_ERROR "Usage: cmake -P AddSpdxHeadersHelper.cmake <file_path> <license> <copyright_holder> <copyright_year>")
    return()
endif()

# Get arguments
set(FILE_PATH ${CMAKE_ARGV3})
set(LICENSE ${CMAKE_ARGV4})
set(COPYRIGHT_HOLDER ${CMAKE_ARGV5})
set(COPYRIGHT_YEAR ${CMAKE_ARGV6})

# Check if file exists
if(NOT EXISTS "${FILE_PATH}")
    message(WARNING "File does not exist: ${FILE_PATH}")
    return()
endif()

# Create a temporary file for the output
string(RANDOM LENGTH 8 TEMP_SUFFIX)
set(TEMP_FILE "${FILE_PATH}.${TEMP_SUFFIX}.tmp")

# Read the entire file content with proper encoding
file(READ "${FILE_PATH}" ORIGINAL_CONTENT)

# Check if file already has SPDX header
if(ORIGINAL_CONTENT MATCHES "SPDX-License-Identifier:")
    # SPDX header already exists, extract and update only the year
    
    # Extract the current license identifier
    string(REGEX MATCH "SPDX-License-Identifier: ([^\n\r]*)" LICENSE_MATCH "${ORIGINAL_CONTENT}")
    if(LICENSE_MATCH)
        string(REGEX REPLACE "SPDX-License-Identifier: ([^\n\r]*)" "\\1" EXISTING_LICENSE "${LICENSE_MATCH}")
    else()
        set(EXISTING_LICENSE "${LICENSE}")
    endif()
    
    # Extract the current copyright holder
    string(REGEX MATCH "SPDX-FileCopyrightText: [0-9-]+ ([^\n\r]*)" COPYRIGHT_MATCH "${ORIGINAL_CONTENT}")
    if(COPYRIGHT_MATCH)
        string(REGEX REPLACE "SPDX-FileCopyrightText: [0-9-]+ ([^\n\r]*)" "\\1" EXISTING_COPYRIGHT_HOLDER "${COPYRIGHT_MATCH}")
    else()
        set(EXISTING_COPYRIGHT_HOLDER "${COPYRIGHT_HOLDER}")
    endif()
    
    # Prepare updated SPDX headers
    set(SPDX_LINE1 "// SPDX-License-Identifier: ${EXISTING_LICENSE}")
    set(SPDX_LINE2 "// SPDX-FileCopyrightText: ${COPYRIGHT_YEAR} ${EXISTING_COPYRIGHT_HOLDER}")
    
    # Replace the existing SPDX headers
    string(REGEX REPLACE "(// SPDX-License-Identifier: [^\n\r]*\n// SPDX-FileCopyrightText: [^\n\r]*)" "${SPDX_LINE1}\n${SPDX_LINE2}" UPDATED_CONTENT "${ORIGINAL_CONTENT}")
    
    # Write the updated content to the temporary file
    file(WRITE "${TEMP_FILE}" "${UPDATED_CONTENT}")
    
    # Replace the original file with the temporary file
    file(RENAME "${TEMP_FILE}" "${FILE_PATH}")
    
    # Inform the user
    message(STATUS "Updated SPDX header year in ${FILE_PATH}")
else()
    # No SPDX header exists, add a new one
    
    # Prepare the SPDX headers with explicit newlines
    set(SPDX_LINE1 "// SPDX-License-Identifier: ${LICENSE}")
    set(SPDX_LINE2 "// SPDX-FileCopyrightText: ${COPYRIGHT_YEAR} ${COPYRIGHT_HOLDER}")
    
    # Write the headers and original content to the temporary file
    file(WRITE "${TEMP_FILE}" "${SPDX_LINE1}\n${SPDX_LINE2}\n\n${ORIGINAL_CONTENT}")
    
    # Replace the original file with the temporary file
    file(RENAME "${TEMP_FILE}" "${FILE_PATH}")
    
    # Inform the user
    message(STATUS "Added SPDX header to ${FILE_PATH}")
endif()