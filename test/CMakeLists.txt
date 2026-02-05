cmake_minimum_required(VERSION 3.10)

# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

project(test LANGUAGES CXX)

# Add CMAKE_MODULE_PATH to find custom modules if not already set
if(NOT CMAKE_MODULE_PATH)
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../cmake")
endif()

enable_testing()

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(TRANSFORM PROJECT_CPP_FILES PREPEND "${CMAKE_CURRENT_LIST_DIR}/../")
list(TRANSFORM PROJECT_H_FILES PREPEND "${CMAKE_CURRENT_LIST_DIR}/../")
list(TRANSFORM PROJECT_QRC_FILES PREPEND "${CMAKE_CURRENT_LIST_DIR}/../")
list(TRANSFORM PROJECT_UI_FILES PREPEND "${CMAKE_CURRENT_LIST_DIR}/../")
list(TRANSFORM PROJECT_TS_FILES PREPEND "${CMAKE_CURRENT_LIST_DIR}/../")
list(TRANSFORM APPONLY_TS_FILES PREPEND "${CMAKE_CURRENT_LIST_DIR}/../")

list(FILTER PROJECT_CPP_FILES EXCLUDE REGEX "src/main\\.cpp$")

set(PROJECT_SOURCES
    "${PROJECT_CPP_FILES}"
    "${PROJECT_H_FILES}"
    "${PROJECT_UI_FILES}"
    "${PROJECT_TS_FILES}"
    "${PROJECT_QRC_FILES}"
)

set(CMAKE_AUTOUIC_SEARCH_PATHS
    "${CMAKE_CURRENT_LIST_DIR}/../ui"
    "${CMAKE_CURRENT_LIST_DIR}/../i18n"
    "${CMAKE_CURRENT_LIST_DIR}/../resource"
)

function(QT_ADD_TEST_TARGET TARGET_NAME)
    add_test(NAME ${TARGET_NAME} COMMAND ${TARGET_NAME})

    # GUI tests require offscreen platform plugin
    if(${TARGET_NAME} MATCHES "^test_qsocgui")
        set_tests_properties(${TARGET_NAME} PROPERTIES
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        )
    else()
        set_tests_properties(${TARGET_NAME} PROPERTIES
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        )
    endif()

    if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
        qt_add_resources(PROJECT_SOURCES "${PROJECT_QRC_FILES}")
        qt_add_executable(${TARGET_NAME} MANUAL_FINALIZATION "${PROJECT_SOURCES}" "${CMAKE_CURRENT_LIST_DIR}/${TARGET_NAME}.cpp")
        # Don't use lupdate in tests
        qt_add_lrelease(${TARGET_NAME} TS_FILES "${PROJECT_TS_FILES}" QM_FILES_OUTPUT_VARIABLE QM_FILES)
        qt_add_resources(${TARGET_NAME} "translations" PREFIX "/i18n" BASE "${CMAKE_CURRENT_BINARY_DIR}" FILES "${QM_FILES}")
    else()
        qt5_add_resources(PROJECT_SOURCES "${PROJECT_QRC_FILES}")
        add_executable(${TARGET_NAME} "${PROJECT_SOURCES}" "${CMAKE_CURRENT_LIST_DIR}/${TARGET_NAME}.cpp")
        # Don't use lupdate in tests
        qt5_add_lrelease(${TARGET_NAME} TS_FILES "${PROJECT_TS_FILES}" QM_FILES_OUTPUT_VARIABLE QM_FILES)
        qt5_add_resources(${TARGET_NAME} "translations" PREFIX "/i18n" BASE "${CMAKE_CURRENT_BINARY_DIR}" FILES "${QM_FILES}")
    endif()

    target_include_directories(${TARGET_NAME} PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/../src"
        "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    target_include_directories(${TARGET_NAME} SYSTEM PRIVATE
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

    target_link_libraries(${TARGET_NAME} PRIVATE
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Gui
        Qt${QT_VERSION_MAJOR}::Network
        Qt${QT_VERSION_MAJOR}::PrintSupport
        Qt${QT_VERSION_MAJOR}::Sql
        Qt${QT_VERSION_MAJOR}::Svg
        Qt${QT_VERSION_MAJOR}::Test
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
endfunction()

# Add SPDX headers to test files if enabled
if(ENABLE_SPDX_HEADERS)
    include(AddSpdxHeaders)

    # Get all test files
    file(GLOB TEST_CPP_FILES "${CMAKE_CURRENT_LIST_DIR}/*.cpp")
    file(GLOB TEST_H_FILES "${CMAKE_CURRENT_LIST_DIR}/*.h")

    # Create a list of all test files
    set(ALL_TEST_FILES ${TEST_CPP_FILES} ${TEST_H_FILES})

    # Create a custom target for all test files
    add_custom_target(test_all_files SOURCES ${ALL_TEST_FILES})

    # Add SPDX headers to all test files
    add_custom_target(
        test_add_spdx_headers
        COMMENT "Adding SPDX headers to all test files"
    )

    # Add dependency to make test_add_spdx_headers run after the test_all_files_add_spdx_headers
    add_spdx_headers(TARGET test_all_files)
    add_dependencies(test_add_spdx_headers test_all_files_add_spdx_headers)
endif()

qt_add_test_target("test_qslangdriver")
qt_add_test_target("test_qsocverilogsignalextractor")
qt_add_test_target("test_qsoccliparsebus")
qt_add_test_target("test_qsoccliparsegenerate")
qt_add_test_target("test_qsoccliparsegenerateclocklogic")
qt_add_test_target("test_qsoccliparsegeneratecomblogic")
qt_add_test_target("test_qsoccliparsegeneratefsmlogic")
qt_add_test_target("test_qsoccliparsegeneratemerge")
qt_add_test_target("test_qsoccliparsegeneratepowerlogic")
qt_add_test_target("test_qsoccliparsegeneratereportunconnected")
qt_add_test_target("test_qsoccliparsegenerateresetlogic")
qt_add_test_target("test_qsoccliparsegenerateseqlogic")
qt_add_test_target("test_qsoccliparsegeneratestub")
qt_add_test_target("test_qsoccliparsegeneratetemplate")
qt_add_test_target("test_qsoccliparsegeneratetemplatercsv")
qt_add_test_target("test_qsoccliparsegeneratetemplaterdl")
qt_add_test_target("test_qsoccliparsegeneratetemplateregex")
qt_add_test_target("test_qsoccliparsegeneratetoplevelport")
qt_add_test_target("test_qsoccliparsemodule")
qt_add_test_target("test_qsoccliparsemodulebus")
qt_add_test_target("test_qsoccliparseproject")
qt_add_test_target("test_qsoccliworker")
qt_add_test_target("test_qsoccommonqsocnumberinfo")
qt_add_test_target("test_qsoccommonqsocverilogutils")
qt_add_test_target("test_qsoccommonqstaticmarkdown")
qt_add_test_target("test_qsoccommonqstaticregex")
qt_add_test_target("test_qsoccommonqstringutils")
qt_add_test_target("test_qsocguischematicwindow")
qt_add_test_target("test_qsocagenttool")
