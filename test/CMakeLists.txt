cmake_minimum_required(VERSION 3.5)

project(test LANGUAGES CXX)

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
        "${SQLITE3_INCLUDE_DIRS}"
        "${SVLANG_INCLUDE_DIRS}"
        "${JSON_INCLUDE_DIRS}"
        "${YAML_INCLUDE_DIRS}"
        "${GPDS_INCLUDE_DIRS}"
        "${QSCHEMATIC_INCLUDE_DIRS}"
    )

    target_link_libraries(${TARGET_NAME} PRIVATE
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Gui
        Qt${QT_VERSION_MAJOR}::PrintSupport
        Qt${QT_VERSION_MAJOR}::Sql
        Qt${QT_VERSION_MAJOR}::Svg
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Widgets
        ${QT_LIBRARIES}
        ${SQLITE3_LIBRARIES}
        ${SVLANG_LIBRARIES}
        ${JSON_LIBRARIES}
        ${YAML_LIBRARIES}
        ${GPDS_LIBRARIES}
        ${QSCHEMATIC_LIBRARIES}
    )
endfunction()

qt_add_test_target("test_qslangdriver")
qt_add_test_target("test_qsoccliworker")
