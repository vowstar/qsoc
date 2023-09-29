cmake_minimum_required(VERSION 3.5)

project(socstudio VERSION 0.1 LANGUAGES CXX)

option(ENABLE_CLANG_TIDY "Enable static analysis with clang-tidy" ON)
option(ENABLE_UNIT_TEST  "Enable unit test"                       ON)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

## QT
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS
    Core
    Gui
    LinguistTools
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

## SQLite3
find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3>=3.30.0)
if(SQLITE3_FOUND)
    message(STATUS "Use system SQLite3 library: ${SQLITE3_LIBRARIES} ${SQLITE3_VERSION}")
endif()

## Slang(sv-lang)
pkg_check_modules(SVLANG IMPORTED_TARGET sv-lang>=3.0.0)
if (SVLANG_FOUND)
    message(STATUS "Use system sv-lang library: ${SVLANG_LIBRARIES} ${SVLANG_VERSION}")
else()
    add_subdirectory(external/slang)
    set(SVLANG_INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/external/slang/include
        ${CMAKE_CURRENT_SOURCE_DIR}/external/slang/external
    )
    set(SVLANG_LIBRARIES slang::slang)
    message(STATUS "Use local sv-lang library: ${SVLANG_LIBRARIES}")
endif()

## JSON
pkg_check_modules(JSON IMPORTED_TARGET nlohmann_json>=3.11.0)
if (JSON_FOUND)
    message(STATUS "Use system nlohmann_json library: ${JSON_LIBRARIES} ${JSON_VERSION}")
else()
    add_subdirectory(external/json)
    set(JSON_INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/external/json/include
    )
    set(JSON_LIBRARIES nlohmann_json::nlohmann_json)
    message(STATUS "Use local nlohmann_json library: ${JSON_LIBRARIES} ${JSON_INCLUDE_DIRS}")
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
        ${CMAKE_CURRENT_SOURCE_DIR}/external/yaml/include
    )
    set(YAML_LIBRARIES yaml-cpp)
    message(STATUS "Use local yaml-cpp library: ${YAML_LIBRARIES} ${YAML_INCLUDE_DIRS}")
endif()

# GPDS
pkg_check_modules(GPDS IMPORTED_TARGET gpds>=1.5.1)
if (GPDS_FOUND)
    message(STATUS "Use system gpds library: ${GPDS_LIBRARIES} ${GPDS_VERSION}")
else()
    set(GPDS_BUILD_TESTS OFF CACHE BOOL "disable gpds tests")
    set(GPDS_BUILD_EXAMPLES OFF CACHE BOOL "disable gpds examples build")
    set(GPDS_FEATURE_SPDLOG OFF CACHE BOOL "disable gpds spdlog build")
    set(QSCHEMATIC_DEPENDENCY_GPDS_TARGET   gpds-static CACHE STRING "qschematic gpds local target name")
    add_subdirectory(external/gpds)
    add_library(gpds::gpds-static ALIAS gpds-static)
    set(GPDS_INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/external/gpds/lib/include
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
    set(QSCHEMATIC_DEPENDENCY_GPDS_LOCAL    ON  CACHE BOOL "enable qschematic gpds local sources")
    # Generate dummy gpds gpds-targets.cmake to avoid error
    set(gpds_DIR "${CMAKE_CURRENT_BINARY_DIR}/external/gpds/lib/gpds")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/external/gpds/lib/gpds/gpds-targets.cmake" "\n")
    add_subdirectory(external/qschematic)
    set(QSCHEMATIC_INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/external/qschematic/qschematic"
    )
    set(QSCHEMATIC_LIBRARIES qschematic-static)
    message(STATUS "Use local qschematic library: ${QSCHEMATIC_LIBRARIES} ${QSCHEMATIC_INCLUDE_DIRS}")
endif()

file(GLOB_RECURSE PROJECT_CPP_FILES RELATIVE ${CMAKE_CURRENT_LIST_DIR} "src/*.cpp")
file(GLOB_RECURSE PROJECT_H_FILES   RELATIVE ${CMAKE_CURRENT_LIST_DIR} "src/*.h")
file(GLOB_RECURSE PROJECT_QRC_FILES RELATIVE ${CMAKE_CURRENT_LIST_DIR} "resource/*.qrc")
file(GLOB_RECURSE PROJECT_UI_FILES  RELATIVE ${CMAKE_CURRENT_LIST_DIR} "ui/*.ui")
file(GLOB_RECURSE PROJECT_TS_FILES  RELATIVE ${CMAKE_CURRENT_LIST_DIR} "i18n/*.ts")
file(GLOB_RECURSE APPONLY_TS_FILES  RELATIVE ${CMAKE_CURRENT_LIST_DIR} "i18n/app_*.ts")

set(PROJECT_SOURCES
    ${PROJECT_CPP_FILES}
    ${PROJECT_H_FILES}
    ${PROJECT_UI_FILES}
    ${PROJECT_TS_FILES}
    ${PROJECT_QRC_FILES}
)

set(CMAKE_AUTOUIC_SEARCH_PATHS
    ${CMAKE_CURRENT_LIST_DIR}/ui
    ${CMAKE_CURRENT_LIST_DIR}/i18n
    ${CMAKE_CURRENT_LIST_DIR}/resource
)

if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_resources(PROJECT_SOURCES ${PROJECT_QRC_FILES})
    qt_add_executable(${PROJECT_NAME}
        MANUAL_FINALIZATION
        ${PROJECT_SOURCES}
    )
    qt_add_lupdate(${PROJECT_NAME} TS_FILES ${APPONLY_TS_FILES} SOURCES ${PROJECT_CPP_FILES} ${PROJECT_H_FILES} ${PROJECT_UI_FILES})
    qt_add_lrelease(${PROJECT_NAME} TS_FILES ${PROJECT_TS_FILES} QM_FILES_OUTPUT_VARIABLE QM_FILES)
    qt_add_resources(${PROJECT_NAME} "translations" PREFIX "/i18n" BASE "${CMAKE_CURRENT_BINARY_DIR}" FILES "${QM_FILES}")
else()
    qt5_add_resources(PROJECT_SOURCES ${PROJECT_QRC_FILES})
    if(ANDROID)
        add_library(${PROJECT_NAME} SHARED
            ${PROJECT_SOURCES}
        )

    else()
        add_executable(${PROJECT_NAME}
            ${PROJECT_SOURCES}
        )
    endif()
    qt5_add_lupdate(${PROJECT_NAME} TS_FILES ${APPONLY_TS_FILES} SOURCES ${PROJECT_CPP_FILES} ${PROJECT_H_FILES} ${PROJECT_UI_FILES})
    qt5_add_lrelease(${PROJECT_NAME} TS_FILES ${PROJECT_TS_FILES} QM_FILES_OUTPUT_VARIABLE QM_FILES)
    qt5_add_resources(${PROJECT_NAME} "translations" PREFIX "/i18n" BASE "${CMAKE_CURRENT_BINARY_DIR}" FILES "${QM_FILES}")
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${SQLITE3_INCLUDE_DIRS}
    ${SVLANG_INCLUDE_DIRS}
    ${JSON_INCLUDE_DIRS}
    ${YAML_INCLUDE_DIRS}
    ${GPDS_INCLUDE_DIRS}
    ${QSCHEMATIC_INCLUDE_DIRS}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::PrintSupport
    Qt${QT_VERSION_MAJOR}::Sql
    Qt${QT_VERSION_MAJOR}::Svg
    Qt${QT_VERSION_MAJOR}::Widgets
    ${QT_LIBRARIES}
    ${SQLITE3_LIBRARIES}
    ${SVLANG_LIBRARIES}
    ${JSON_LIBRARIES}
    ${YAML_LIBRARIES}
    ${GPDS_LIBRARIES}
    ${QSCHEMATIC_LIBRARIES}
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
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(${PROJECT_NAME})
endif()

## Clang-format
find_program(CLANG_FORMAT NAMES clang-format)
if(CLANG_FORMAT)
    message(STATUS "Found clang-format: ${CLANG_FORMAT}")
    add_custom_target(
        clang-format
        COMMAND ${CLANG_FORMAT} -i -style=file ${PROJECT_CPP_FILES} ${PROJECT_H_FILES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    )
endif()

## Clang-tidy
if (ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_COMMAND NAMES clang-tidy)
    if(NOT CLANG_TIDY_COMMAND)
        message(WARNING "clang-tidy is not found!")
        set(CMAKE_CXX_CLANG_TIDY "" CACHE STRING "" FORCE)
    else()
        message(STATUS "Found clang-tidy: ${CLANG_TIDY_COMMAND}")
        set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
    endif()
    ## Files generated by MOC, RCC, and UIC may produce clang-tidy warnings.
    ## We generate a dummy .clang-tidy file in the binary directory that
    ## disables all clang-tidy checks except one that will never match.
    ## This one check is necessary; clang-tidy reports an error when no checks
    ## are enabled.
    ## Since the Qt code generators will generate source files in the binary
    ## tree, clang-tidy will load the configuration from this dummy file when
    ## the sources are built.
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/.clang-tidy"
        "---\n"
        "Checks: '-*,llvm-twine-local'\n"
        "...\n")
endif()

## Testing
if (ENABLE_UNIT_TEST)
    enable_testing()
    add_test(NAME test_cmd_version COMMAND ${PROJECT_NAME} -v)
    add_subdirectory(test)
endif()
