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

list(TRANSFORM PROJECT_QRC_FILES PREPEND "${CMAKE_CURRENT_LIST_DIR}/../")
list(TRANSFORM PROJECT_TS_FILES PREPEND "${CMAKE_CURRENT_LIST_DIR}/../")

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

    set(_TEST_SOURCES "${CMAKE_CURRENT_LIST_DIR}/${TARGET_NAME}.cpp")

    if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
        qt_add_resources(_TEST_SOURCES "${PROJECT_QRC_FILES}")
        qt_add_executable(${TARGET_NAME} MANUAL_FINALIZATION "${_TEST_SOURCES}")
        qt_add_lrelease(${TARGET_NAME} TS_FILES "${PROJECT_TS_FILES}" QM_FILES_OUTPUT_VARIABLE QM_FILES)
        qt_add_resources(${TARGET_NAME} "translations" PREFIX "/i18n" BASE "${CMAKE_CURRENT_BINARY_DIR}" FILES "${QM_FILES}")
    else()
        qt5_add_resources(_TEST_SOURCES "${PROJECT_QRC_FILES}")
        add_executable(${TARGET_NAME} "${_TEST_SOURCES}")
        qt5_add_lrelease(${TARGET_NAME} TS_FILES "${PROJECT_TS_FILES}" QM_FILES_OUTPUT_VARIABLE QM_FILES)
        qt5_add_resources(${TARGET_NAME} "translations" PREFIX "/i18n" BASE "${CMAKE_CURRENT_BINARY_DIR}" FILES "${QM_FILES}")
    endif()

    target_link_libraries(${TARGET_NAME} PRIVATE
        qsoc_core
        Qt${QT_VERSION_MAJOR}::Test
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
qt_add_test_target("test_qsocconsole")
qt_add_test_target("test_qsoccommonqsocnumberinfo")
qt_add_test_target("test_qsoccron")
qt_add_test_target("test_qsoctoolschedule")
qt_add_test_target("test_qsoctaskregistry")
qt_add_test_target("test_qsocmonitortasksource")
qt_add_test_target("test_qsoccommonqsocpaths")
qt_add_test_target("test_qsoccommonqsocverilogutils")
qt_add_test_target("test_qsoccommonqstaticmarkdown")
qt_add_test_target("test_qsoccommonqstaticregex")
qt_add_test_target("test_qsoccommonqstringutils")
qt_add_test_target("test_qsocguischematicwindow")
qt_add_test_target("test_qsocagenttool")
qt_add_test_target("test_qsocagenttoolshell")
qt_add_test_target("test_qsocmcpnaming")
qt_add_test_target("test_qsocmcpstdio")
qt_add_test_target("test_qsocmcphttp")
qt_add_test_target("test_qsocmcpclient")
qt_add_test_target("test_qsocmcpconfig")
qt_add_test_target("test_qsocmcpregister")
qt_add_test_target("test_qsocmcpreconnect")
qt_add_test_target("test_qsocmcpsystemprompt")
qt_add_test_target("test_qsochookconfig")
qt_add_test_target("test_qsochookrunner")
qt_add_test_target("test_qsochookmanager")
qt_add_test_target("test_qsocproxy")
qt_add_test_target("test_qsocagenttoolskill")
qt_add_test_target("test_qsocagentcompact")
qt_add_test_target("test_qsocagentsubagentguards")
qt_add_test_target("test_qsocagentdefinitionregistry")
qt_add_test_target("test_qsocsubagenttasksource")
qt_add_test_target("test_qsoctoolagent")
qt_add_test_target("test_qsoctoolagentstatus")
qt_add_test_target("test_qllmserviceclone")
qt_add_test_target("test_qsoctoolsendmessage")
qt_add_test_target("test_qsoctoolagentresume")
qt_add_test_target("test_qsocagentinputmonitor")
qt_add_test_target("test_qtuiinputlinewrap")
qt_add_test_target("test_qtuiscrollviewdiffwrap")
qt_add_test_target("test_qsocimagetokens")
qt_add_test_target("test_qllmservicemodalities")
qt_add_test_target("test_qsoctoolwebimage")
qt_add_test_target("test_qsocagentattachment")
qt_add_test_target("test_qsocimageattach")
qt_add_test_target("test_qsoctoolfilereadimage")
qt_add_test_target("test_qsocmarkdownrenderer")
qt_add_test_target("test_qtuiscrollviewstyledruns")
qt_add_test_target("test_qtuiblock_text")
qt_add_test_target("test_qtuiscrollviewblocks")
qt_add_test_target("test_qtuicompositorstreaming")
qt_add_test_target("test_qtuiscrollviewfocuscopy")
qt_add_test_target("test_qtuiblockfold")
qt_add_test_target("test_qtuiblock_hscroll")
qt_add_test_target("test_qsoccodehighlighter")
qt_add_test_target("test_qtuiblock_table_overflow")
qt_add_test_target("test_qtuitoolblock")
qt_add_test_target("test_qtuidiffblock")
qt_add_test_target("test_qtuiuserblock")
qt_add_test_target("test_qtuiosc8hyperlinks")
qt_add_test_target("test_qtuiblock_cookedansi")
qt_add_test_target("test_qtuitodoblock")
qt_add_test_target("test_qtuiimagepreviewblock")
qt_add_test_target("test_qtuicodeblock")
qt_add_test_target("test_qtuicompositorcodesplit")
qt_add_test_target("test_qtuiselectiondecorative")
qt_add_test_target("test_qsocloopscheduler")
qt_add_test_target("test_qsocagenttoolweb")
qt_add_test_target("test_qsocagentcompletion")
qt_add_test_target("test_qsocexternaleditor")
qt_add_test_target("test_qsocagenthistorysearch")
qt_add_test_target("test_qsoclinediff")
qt_add_test_target("test_qsocsession")
qt_add_test_target("test_qsocfilehistory")
qt_add_test_target("test_qlspslangbackend")
qt_add_test_target("test_qlspservice")
qt_add_test_target("test_qsocsshconfigparser")
qt_add_test_target("test_qsocremotepathcontext")
qt_add_test_target("test_qsocsshsession")
qt_add_test_target("test_qsocsshexec")
qt_add_test_target("test_qsocsftpclient")
qt_add_test_target("test_qsocremotebinding")
qt_add_test_target("test_qsocsshpubderive")
