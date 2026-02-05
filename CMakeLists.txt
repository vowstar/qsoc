# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

cmake_minimum_required(VERSION 3.10)

# Add CMAKE_MODULE_PATH to find custom modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Options
option(ENABLE_CLANG_TIDY   "Enable static analysis with clang-tidy"       OFF)
option(ENABLE_UNIT_TEST    "Enable unit test"                              ON)
option(ENABLE_DOXYGEN      "Enable documentation generation with Doxygen" OFF)
option(ENABLE_SPDX_HEADERS "Enable adding SPDX headers to source files"   OFF)
option(ENABLE_TEST_CLEANUP "Automatically cleanup test files after test"  OFF)
# CMake Settings
set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for tools like cppcheck and clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Project settings
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/common/config.h" CONFIG_H_CONTENT)
string(REGEX MATCH "#define QSOC_VERSION \"([0-9]+\\.[0-9]+\\.[0-9]+)\"" _ ${CONFIG_H_CONTENT})
set(QSOC_VERSION ${CMAKE_MATCH_1})
message(STATUS "Project version: qsoc ${QSOC_VERSION}")

project(qsoc VERSION ${QSOC_VERSION} LANGUAGES CXX)

# QT
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS
    Core
    Gui
    LinguistTools
    Network
    PrintSupport
    Sql
    Svg
    Test
    Widgets
)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS
    Core
    Gui
    LinguistTools
    Network
    PrintSupport
    Sql
    Svg
    Test
    Widgets
)

if (Qt6_FOUND)
    find_package(Qt6 COMPONENTS Core5Compat REQUIRED)
    set(QT_LIBRARIES Qt6::Core5Compat)
endif()

# SQLite3
find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3>=3.26.0)
if(SQLITE3_FOUND)
    message(STATUS "Use system SQLite3 library: ${SQLITE3_LIBRARIES} ${SQLITE3_VERSION}")
endif()

# Slang(sv-lang)
pkg_check_modules(SVLANG IMPORTED_TARGET sv-lang>=3.0.0)
if (SVLANG_FOUND)
    message(STATUS "Use system sv-lang library: ${SVLANG_LIBRARIES} ${SVLANG_VERSION}")
else()
    add_subdirectory(external/slang)
    set(SVLANG_INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/external/slang/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/external/slang/external"
    )
    set(SVLANG_LIBRARIES slang::slang)
    message(STATUS "Use local sv-lang library: ${SVLANG_LIBRARIES}")
endif()

# JSON
pkg_check_modules(JSON IMPORTED_TARGET nlohmann_json>=3.11.0)
if (JSON_FOUND)
    message(STATUS "Use system nlohmann_json library: ${JSON_LIBRARIES} ${JSON_VERSION}")
else()
    add_subdirectory(external/json)
    set(JSON_INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/external/json/single_include"
    )
    set(JSON_LIBRARIES nlohmann_json::nlohmann_json)
    message(STATUS "Use local nlohmann_json library: ${JSON_LIBRARIES} ${JSON_INCLUDE_DIRS}")
endif()

# INJA
pkg_check_modules(INJA IMPORTED_TARGET inja>=3.0.0)
if (INJA_FOUND)
    message(STATUS "Use system inja library: ${INJA_LIBRARIES} ${INJA_VERSION}")
else()
    set(INJA_USE_EMBEDDED_JSON OFF)
    set(INJA_INSTALL OFF)
    set(INJA_INSTALL_SINGLE_HEADER OFF)
    set(INJA_EXPORT OFF)
    set(INJA_BUILD_TESTS OFF)
    add_subdirectory(external/inja)
    set(INJA_INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/external/inja/include"
    )
    set(INJA_LIBRARIES inja)
    message(STATUS "Use local inja library: ${INJA_LIBRARIES} ${INJA_INCLUDE_DIRS}")
endif()

# YAML
pkg_check_modules(YAML IMPORTED_TARGET yaml-cpp>=0.7.0)
if (YAML_FOUND)
    message(STATUS "Use system yaml-cpp library: ${YAML_LIBRARIES} ${YAML_VERSION}")
else()
    set(YAML_CPP_BUILD_TESTS   OFF CACHE BOOL "disable yaml tests")
    set(YAML_CPP_BUILD_TOOLS   OFF CACHE BOOL "disable yaml tools")
    set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "disable yaml contrib")
    add_subdirectory(external/yaml)
    set(YAML_INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/external/yaml/include"
    )
    set(YAML_LIBRARIES yaml-cpp)
    message(STATUS "Use local yaml-cpp library: ${YAML_LIBRARIES} ${YAML_INCLUDE_DIRS}")
endif()

# CSV
pkg_check_modules(CSV IMPORTED_TARGET rapidcsv>=8.80)
if (CSV_FOUND)
    message(STATUS "Use system rapidcsv library: ${CSV_LIBRARIES} ${CSV_VERSION}")
else()
    add_subdirectory(external/csv)
    set(CSV_INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/external/csv/src"
    )
    set(CSV_LIBRARIES rapidcsv)
    message(STATUS "Use local rapidcsv library: ${CSV_LIBRARIES} ${CSV_INCLUDE_DIRS}")
endif()

# GPDS
pkg_check_modules(GPDS IMPORTED_TARGET gpds>=1.5.1)
if (GPDS_FOUND)
    message(STATUS "Use system gpds library: ${GPDS_LIBRARIES} ${GPDS_VERSION}")
else()
    set(GPDS_BUILD_TESTS OFF CACHE BOOL "disable gpds tests")
    set(GPDS_BUILD_EXAMPLES OFF CACHE BOOL "disable gpds examples build")
    set(GPDS_BUILD_STATIC ON CACHE BOOL "Build static library.")
    set(GPDS_BUILD_SHARED OFF CACHE BOOL "Build shared library.")
    add_subdirectory(external/gpds)
    add_library(gpds::gpds-static ALIAS gpds-static)
    set(GPDS_INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/external/gpds/gpds"
    )
    set(GPDS_LIBRARIES gpds::gpds-static)
    message(STATUS "Use local gpds library: ${GPDS_LIBRARIES} ${GPDS_INCLUDE_DIRS}")
endif()

# QSCHEMATIC
pkg_check_modules(QSCHEMATIC IMPORTED_TARGET qschematic>=1.4.0)
if (QSCHEMATIC_FOUND)
    message(STATUS "Use system qschematic library: ${QSCHEMATIC_LIBRARIES} ${QSCHEMATIC_VERSION}")
else()
    set(QSCHEMATIC_BUILD_DEMO               OFF CACHE BOOL "disable qschematic demo build")
    set(QSCHEMATIC_DEPENDENCY_GPDS_DOWNLOAD OFF CACHE BOOL "disable qschematic gpds sources download")
    set(QSCHEMATIC_DEPENDENCY_GPDS_TARGET   "gpds::gpds-static" CACHE STRING "The CMake target of the GPDS library to use")
    # Generate gpds-config.cmake to provide gpds::gpds-static target
    set(gpds_DIR "${CMAKE_CURRENT_BINARY_DIR}/external/gpds/gpds")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/external/gpds/gpds")

    # Create gpds-config.cmake with proper target definitions
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/external/gpds/gpds/gpds-config.cmake" "
# GPDS Configuration File
set(PACKAGE_VERSION \"1.10.0\")
set(GPDS_VERSION \"1.10.0\")

# Define the gpds::gpds-static target if it doesn't exist
if(NOT TARGET gpds::gpds-static)
    add_library(gpds::gpds-static INTERFACE IMPORTED)
    set_target_properties(gpds::gpds-static PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES \"${CMAKE_CURRENT_SOURCE_DIR}/external/gpds\"
        INTERFACE_LINK_LIBRARIES \"gpds-static\"
    )
endif()

# Set variables for compatibility
set(GPDS_FOUND TRUE)
set(GPDS_INCLUDE_DIRS \"${CMAKE_CURRENT_SOURCE_DIR}/external/gpds\")
set(GPDS_LIBRARIES gpds::gpds-static)
")

    # Create gpds-config-version.cmake for version checking
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/external/gpds/gpds/gpds-config-version.cmake" "
# GPDS Version Configuration File
set(PACKAGE_VERSION \"1.10.0\")

# Version compatibility check
if(\"1.10.0\" VERSION_LESS \"\${PACKAGE_FIND_VERSION}\")
    set(PACKAGE_VERSION_COMPATIBLE FALSE)
else()
    set(PACKAGE_VERSION_COMPATIBLE TRUE)
    if(\"\${PACKAGE_FIND_VERSION}\" STREQUAL \"1.10.0\")
        set(PACKAGE_VERSION_EXACT TRUE)
    endif()
endif()
")
    add_subdirectory(external/qschematic)
    set(QSCHEMATIC_INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/external/qschematic/qschematic"
    )
    set(QSCHEMATIC_LIBRARIES qschematic-static)
    message(STATUS "Use local qschematic library: ${QSCHEMATIC_LIBRARIES} ${QSCHEMATIC_INCLUDE_DIRS}")
endif()

# BIGINT
pkg_check_modules(BIGINT IMPORTED_TARGET bigint>=2010.04.30)
if (BIGINT_FOUND)
    message(STATUS "Use system bigint library: ${BIGINT_LIBRARIES} ${BIGINT_VERSION}")
else()
    add_subdirectory(external/bigint)
    set(BIGINT_INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/external/bigint"
    )
    set(BIGINT_LIBRARIES bigint::bigint)
    message(STATUS "Use local bigint library: ${BIGINT_LIBRARIES} ${BIGINT_INCLUDE_DIRS}")
endif()

# ANTLR4
pkg_check_modules(ANTLR4 IMPORTED_TARGET antlr4-runtime>=4.13.0)
if (ANTLR4_FOUND)
    message(STATUS "Use system antlr4-runtime library: ${ANTLR4_LIBRARIES} ${ANTLR4_VERSION}")
else()
    # Configure ANTLR4 C++ runtime build options
    set(ANTLR4_BUILD_CPP_TESTS OFF CACHE BOOL "disable antlr4 cpp tests")
    set(ANTLR4_INSTALL OFF CACHE BOOL "disable antlr4 install")
    set(WITH_DEMO OFF CACHE BOOL "disable antlr4 demo")
    set(WITH_LIBCXX OFF CACHE BOOL "disable antlr4 libcxx")
    set(ANTLR_BUILD_CPP_TESTS OFF CACHE BOOL "disable antlr4 build cpp tests")
    add_subdirectory(external/antlr4/runtime/Cpp)
    set(ANTLR4_INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/external/antlr4/runtime/Cpp/runtime/src"
    )
    set(ANTLR4_LIBRARIES antlr4_static)
    message(STATUS "Use local antlr4-runtime library: ${ANTLR4_LIBRARIES} ${ANTLR4_INCLUDE_DIRS}")
endif()

# SYSTEMRDL
pkg_check_modules(SYSTEMRDL IMPORTED_TARGET systemrdl>=0.1.0)
if (SYSTEMRDL_FOUND)
    message(STATUS "Use system systemrdl library: ${SYSTEMRDL_LIBRARIES} ${SYSTEMRDL_VERSION}")
else()
    # Configure SystemRDL to use local dependencies and disable unnecessary components
    set(USE_SYSTEM_NLOHMANN_JSON ON CACHE BOOL "enable systemrdl to use local json as system")
    set(USE_SYSTEM_INJA ON CACHE BOOL "enable systemrdl to use local inja as system")
    set(USE_SYSTEM_ANTLR4 ON CACHE BOOL "enable systemrdl to use local antlr4 as system")
    set(SYSTEMRDL_BUILD_TOOLS OFF CACHE BOOL "disable systemrdl tools")
    set(SYSTEMRDL_BUILD_TESTS OFF CACHE BOOL "disable systemrdl tests")
    set(SYSTEMRDL_BUILD_SHARED OFF CACHE BOOL "disable systemrdl shared library")
    set(SYSTEMRDL_BUILD_STATIC ON CACHE BOOL "enable systemrdl static library")
    # Set up variables for SystemRDL to treat our local libraries as "system" libraries
    set(NLOHMANN_JSON_INCLUDE_DIR "${JSON_INCLUDE_DIRS}" CACHE PATH "systemrdl nlohmann_json include dir")
    set(NLOHMANN_JSON_INCLUDE_DIRS "${JSON_INCLUDE_DIRS}" CACHE PATH "systemrdl nlohmann_json include dirs")
    set(NLOHMANN_JSON_FOUND TRUE CACHE BOOL "systemrdl nlohmann_json found")
    set(INJA_INCLUDE_DIR "${INJA_INCLUDE_DIRS}" CACHE PATH "systemrdl inja include dir")
    set(INJA_INCLUDE_DIRS "${INJA_INCLUDE_DIRS}" CACHE PATH "systemrdl inja include dirs")
    set(INJA_FOUND TRUE CACHE BOOL "systemrdl inja found")
    # Set ANTLR4 variables to bypass pkg-config and manual search
    set(ANTLR4_INCLUDE_DIR "${ANTLR4_INCLUDE_DIRS}" CACHE PATH "systemrdl antlr4 include dir")
    set(ANTLR4_INCLUDE_DIRS "${ANTLR4_INCLUDE_DIRS}" CACHE PATH "systemrdl antlr4 include dirs")
    set(ANTLR4_LIBRARY "${ANTLR4_LIBRARIES}" CACHE STRING "systemrdl antlr4 library")
    set(ANTLR4_LIBRARIES "${ANTLR4_LIBRARIES}" CACHE STRING "systemrdl antlr4 libraries")
    set(ANTLR4_FOUND TRUE CACHE BOOL "systemrdl antlr4 found")
    # Also set the variables that pkg_check_modules would set
    set(ANTLR4_CFLAGS "" CACHE STRING "systemrdl antlr4 cflags")
    set(ANTLR4_CFLAGS_OTHER "" CACHE STRING "systemrdl antlr4 cflags other")
    set(ANTLR4_LDFLAGS "" CACHE STRING "systemrdl antlr4 ldflags")
    set(ANTLR4_LDFLAGS_OTHER "" CACHE STRING "systemrdl antlr4 ldflags other")
    add_subdirectory(external/systemrdl)
    set(SYSTEMRDL_INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/external/systemrdl"
    )
    set(SYSTEMRDL_LIBRARIES SystemRDL::systemrdl_static)
    message(STATUS "Use local systemrdl library: ${SYSTEMRDL_LIBRARIES} ${SYSTEMRDL_INCLUDE_DIRS}")
endif()

# REPLXX
pkg_check_modules(REPLXX IMPORTED_TARGET replxx>=0.0.4)
if (REPLXX_FOUND)
    message(STATUS "Use system replxx library: ${REPLXX_LIBRARIES} ${REPLXX_VERSION}")
else()
    set(REPLXX_BUILD_EXAMPLES OFF CACHE BOOL "disable replxx examples")
    set(REPLXX_BUILD_PACKAGE OFF CACHE BOOL "disable replxx package")
    add_subdirectory(external/replxx)
    set(REPLXX_INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/external/replxx/include"
    )
    set(REPLXX_LIBRARIES replxx::replxx)
    message(STATUS "Use local replxx library: ${REPLXX_LIBRARIES} ${REPLXX_INCLUDE_DIRS}")
endif()

# FMT
# Check if fmt target already exists (to avoid conflicts with FetchContent or other mechanisms)
if(NOT TARGET fmt::fmt)
    pkg_check_modules(FMT IMPORTED_TARGET fmt>=10.0.0)
    if (FMT_FOUND)
        message(STATUS "Use system fmt library: ${FMT_LIBRARIES} ${FMT_VERSION}")
    else()
        set(FMT_INSTALL OFF CACHE BOOL "disable fmt install")
        set(FMT_TEST OFF CACHE BOOL "disable fmt tests")
        add_subdirectory(external/fmt)
        set(FMT_INCLUDE_DIRS
            "${CMAKE_CURRENT_SOURCE_DIR}/external/fmt/include"
        )
        set(FMT_LIBRARIES fmt::fmt)
        message(STATUS "Use local fmt library: ${FMT_LIBRARIES} ${FMT_INCLUDE_DIRS}")
    endif()
else()
    # fmt target already exists, just set the variables
    set(FMT_LIBRARIES fmt::fmt)
    get_target_property(FMT_INCLUDE_DIRS fmt::fmt INTERFACE_INCLUDE_DIRECTORIES)
    if(NOT FMT_INCLUDE_DIRS)
        set(FMT_INCLUDE_DIRS "")
    endif()
    message(STATUS "Use existing fmt target: ${FMT_LIBRARIES}")
endif()

file(GLOB_RECURSE PROJECT_CPP_FILES RELATIVE "${CMAKE_CURRENT_LIST_DIR}" "src/*.cpp")
file(GLOB_RECURSE PROJECT_H_FILES   RELATIVE "${CMAKE_CURRENT_LIST_DIR}" "src/*.h")
file(GLOB_RECURSE PROJECT_QRC_FILES RELATIVE "${CMAKE_CURRENT_LIST_DIR}" "resource/*.qrc")
file(GLOB_RECURSE PROJECT_DOC_QRC_FILES RELATIVE "${CMAKE_CURRENT_LIST_DIR}" "doc/*.qrc")
file(GLOB_RECURSE PROJECT_UI_FILES  RELATIVE "${CMAKE_CURRENT_LIST_DIR}" "ui/*.ui")
file(GLOB_RECURSE PROJECT_TS_FILES  RELATIVE "${CMAKE_CURRENT_LIST_DIR}" "i18n/*.ts")
file(GLOB_RECURSE APPONLY_TS_FILES  RELATIVE "${CMAKE_CURRENT_LIST_DIR}" "i18n/app_*.ts")

set(PROJECT_SOURCES
    "${PROJECT_CPP_FILES}"
    "${PROJECT_H_FILES}"
    "${PROJECT_UI_FILES}"
    "${PROJECT_TS_FILES}"
    "${PROJECT_QRC_FILES}"
    "${PROJECT_DOC_QRC_FILES}"
)

set(CMAKE_AUTOUIC_SEARCH_PATHS
    "${CMAKE_CURRENT_LIST_DIR}/ui"
    "${CMAKE_CURRENT_LIST_DIR}/i18n"
    "${CMAKE_CURRENT_LIST_DIR}/resource"
)

if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_resources(PROJECT_SOURCES "${PROJECT_QRC_FILES}" "${PROJECT_DOC_QRC_FILES}")
    qt_add_executable(${PROJECT_NAME}
        MANUAL_FINALIZATION
        "${PROJECT_SOURCES}"
    )
    qt_add_lupdate(${PROJECT_NAME} TS_FILES "${APPONLY_TS_FILES}" SOURCES "${PROJECT_CPP_FILES}" "${PROJECT_H_FILES}" "${PROJECT_UI_FILES}")
    qt_add_lrelease(${PROJECT_NAME} TS_FILES "${PROJECT_TS_FILES}" QM_FILES_OUTPUT_VARIABLE QM_FILES)
    qt_add_resources(${PROJECT_NAME} "translations" PREFIX "/i18n" BASE "${CMAKE_CURRENT_BINARY_DIR}" FILES "${QM_FILES}")
else()
    qt5_add_resources(PROJECT_SOURCES "${PROJECT_QRC_FILES}" "${PROJECT_DOC_QRC_FILES}")
    if(ANDROID)
        add_library(${PROJECT_NAME} SHARED
            "${PROJECT_SOURCES}"
        )

    else()
        add_executable(${PROJECT_NAME}
            "${PROJECT_SOURCES}"
        )
    endif()
    qt5_add_lupdate(${PROJECT_NAME} TS_FILES "${APPONLY_TS_FILES}" SOURCES "${PROJECT_CPP_FILES}" "${PROJECT_H_FILES}" "${PROJECT_UI_FILES}")
    qt5_add_lrelease(${PROJECT_NAME} TS_FILES "${PROJECT_TS_FILES}" QM_FILES_OUTPUT_VARIABLE QM_FILES)
    qt5_add_resources(${PROJECT_NAME} "translations" PREFIX "/i18n" BASE "${CMAKE_CURRENT_BINARY_DIR}" FILES "${QM_FILES}")
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE
    "${SQLITE3_INCLUDE_DIRS}"
    "${SVLANG_INCLUDE_DIRS}"
    "${JSON_INCLUDE_DIRS}"
    "${INJA_INCLUDE_DIRS}"
    "${YAML_INCLUDE_DIRS}"
    "${CSV_INCLUDE_DIRS}"
    "${GPDS_INCLUDE_DIRS}"
    "${QSCHEMATIC_INCLUDE_DIRS}"
    "${BIGINT_INCLUDE_DIRS}"
    "${ANTLR4_INCLUDE_DIRS}"
    "${SYSTEMRDL_INCLUDE_DIRS}"
    "${FMT_INCLUDE_DIRS}"
    "${REPLXX_INCLUDE_DIRS}"
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::Network
    Qt${QT_VERSION_MAJOR}::PrintSupport
    Qt${QT_VERSION_MAJOR}::Sql
    Qt${QT_VERSION_MAJOR}::Svg
    Qt${QT_VERSION_MAJOR}::Widgets
    ${QT_LIBRARIES}
    ${SQLITE3_LIBRARIES}
    ${SVLANG_LIBRARIES}
    ${JSON_LIBRARIES}
    ${INJA_LIBRARIES}
    ${YAML_LIBRARIES}
    ${CSV_LIBRARIES}
    ${GPDS_LIBRARIES}
    ${QSCHEMATIC_LIBRARIES}
    ${BIGINT_LIBRARIES}
    ${ANTLR4_LIBRARIES}
    ${SYSTEMRDL_LIBRARIES}
    ${FMT_LIBRARIES}
    ${REPLXX_LIBRARIES}
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER com.vowstar.${PROJECT_NAME}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

install(TARGETS ${PROJECT_NAME}
    BUNDLE DESTINATION .
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(${PROJECT_NAME})
endif()

# Clang-format
find_program(CLANG_FORMAT NAMES clang-format)
if(CLANG_FORMAT)
    message(STATUS "Found clang-format: ${CLANG_FORMAT}")
    add_custom_target(
        clang-format
        COMMAND ${CLANG_FORMAT} -i -style=file ${PROJECT_CPP_FILES} ${PROJECT_H_FILES} ${CMAKE_CURRENT_LIST_DIR}/test/*.cpp ${CMAKE_CURRENT_LIST_DIR}/test/*.h
        WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
    )
endif()

# Clang-tidy
if (ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_COMMAND NAMES clang-tidy)
    if(NOT CLANG_TIDY_COMMAND)
        message(WARNING "clang-tidy is not found!")
        set(CMAKE_CXX_CLANG_TIDY "" CACHE STRING "" FORCE)
    else()
        message(STATUS "Found clang-tidy: ${CLANG_TIDY_COMMAND}")
        set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
    endif()
    # Files generated by MOC, RCC, and UIC may produce clang-tidy warnings.
    # We generate a dummy .clang-tidy file in the binary directory that
    # disables all clang-tidy checks except one that will never match.
    # This one check is necessary; clang-tidy reports an error when no checks
    # are enabled.
    # Since the Qt code generators will generate source files in the binary
    # tree, clang-tidy will load the configuration from this dummy file when
    # the sources are built.
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/.clang-tidy"
        "---\n"
        "Checks: '-*,llvm-twine-local'\n"
        "...\n")
endif()

# Cppcheck (faster alternative to clang-tidy)
find_program(CPPCHECK_COMMAND NAMES cppcheck)
if(CPPCHECK_COMMAND)
    message(STATUS "Found cppcheck: ${CPPCHECK_COMMAND}")
    add_custom_target(
        cppcheck
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/cppcheck
        COMMAND ${CPPCHECK_COMMAND}
            --enable=all
            --inconclusive
            --max-ctu-depth=0
            --suppressions-list=${CMAKE_CURRENT_SOURCE_DIR}/.cppcheck-suppressions
            --cppcheck-build-dir=${CMAKE_BINARY_DIR}/cppcheck
            -I${CMAKE_CURRENT_SOURCE_DIR}/src
            -I${CMAKE_CURRENT_SOURCE_DIR}/test
            -I${CMAKE_BINARY_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/test
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Running cppcheck static analysis on src and test directories"
    )
else()
    message(STATUS "cppcheck not found, install it for faster static analysis")
endif()

# Doxygen
if (ENABLE_DOXYGEN)
    # check if Doxygen is installed
    find_package(Doxygen)
    if (DOXYGEN_FOUND)
        # doxygen_add_docs required cmake version minimum 3.9.
        cmake_minimum_required(VERSION 3.9)
        set(DOXYGEN_RECURSIVE YES)
        set(DOXYGEN_GENERATE_LATEX YES)
        set(DOXYGEN_USE_MDFILE_AS_MAINPAGE README.md)
        set(DOXYGEN_FULL_PATH_NAMES NO)
        doxygen_add_docs(doc_doxygen
            ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/README.md
            ALL
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            COMMENT "Generating API documentation with Doxygen")
    else()
        message("Doxygen need to be installed to generate the doxygen documentation")
    endif()
    if (NOT DOXYGEN_DOT_FOUND)
        message(FATAL_ERROR "Command `dot` not found.  Please install graphviz.")
    endif()
endif()

# Testing
if (ENABLE_UNIT_TEST)
    enable_testing()
    add_test(NAME test_cmd_version COMMAND ${PROJECT_NAME} -v)
    add_subdirectory(test)
endif()

# Add SPDX headers to source files if enabled
if(ENABLE_SPDX_HEADERS)
    include(AddSpdxHeaders)
    add_spdx_headers(TARGET ${PROJECT_NAME})
endif()

if(ENABLE_TEST_CLEANUP)
    add_definitions(-DENABLE_TEST_CLEANUP)
endif()
