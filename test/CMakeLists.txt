cmake_minimum_required(VERSION 3.10)

# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

project(test LANGUAGES CXX)

# Add CMAKE_MODULE_PATH to find custom modules if not already set
if(NOT CMAKE_MODULE_PATH)
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../cmake")
endif()

enable_testing()

add_executable(qsoc_formatter_probe qsoc_formatter_probe.cpp)
target_link_libraries(qsoc_formatter_probe PRIVATE Qt${QT_VERSION_MAJOR}::Core)

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

# Per-test policy is declared at the call site. A misspelled keyword is a
# configure error, unlike a name missing from a regex or a property variable.
function(QT_ADD_TEST_TARGET TARGET_NAME)
    cmake_parse_arguments(T "" "LOCK" "LABELS" ${ARGN})
    if(T_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "${TARGET_NAME}: unknown option ${T_UNPARSED_ARGUMENTS}")
    endif()

    add_test(NAME ${TARGET_NAME} COMMAND ${TARGET_NAME})

    # GUI tests need the offscreen platform plugin.
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

    # The loopback sshd fixtures each bring up their own server and mock on
    # their own ports, but they share /tmp and the machine's socket backlog,
    # and running them together has produced a mock that never started. A
    # shared lock keeps `ctest -jN` from interleaving them without forcing the
    # whole suite serial.
    if(T_LOCK)
        set_tests_properties(${TARGET_NAME} PROPERTIES RESOURCE_LOCK "${T_LOCK}")
    endif()

    # Labels group a feature that spans several files. A test may carry more
    # than one, which a filename or a directory cannot express.
    if(T_LABELS)
        set_tests_properties(${TARGET_NAME} PROPERTIES LABELS "${T_LABELS}")
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

add_executable(qsoc_mock_llm "${CMAKE_CURRENT_LIST_DIR}/qsoc_mock_llm.cpp")
target_link_libraries(qsoc_mock_llm PRIVATE
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Network
)

add_executable(qsoc_mcp_stdio_peer "${CMAKE_CURRENT_LIST_DIR}/qsoc_mcp_stdio_peer.cpp")
target_link_libraries(qsoc_mcp_stdio_peer PRIVATE Qt${QT_VERSION_MAJOR}::Core)

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
qt_add_test_target("test_qsoccommonqsocyamlutils")
qt_add_test_target("test_qsoccliparsebus")
qt_add_test_target("test_qsocbusmanagerphase0")
qt_add_test_target("test_qsoccliparsegenerate")
qt_add_test_target("test_qsoccliparsegenerateelaborate")
qt_add_test_target("test_qsoccliparsegenerateclocklogic")
qt_add_test_target("test_qsoccliparsegeneratecomblogic")
qt_add_test_target("test_qsoccliparsegenerateconnectedtarget")
qt_add_test_target("test_qsoccliparsegeneratefsmlogic")
qt_add_test_target("test_qsoccliparsegeneratemerge")
qt_add_test_target("test_qsoccliparsegeneratemultidriverguard")
qt_add_test_target("test_qsoccliparsegeneratepowerlogic")
qt_add_test_target("test_qsoccliparsegeneratereportunconnected")
qt_add_test_target("test_qsocgenerateconnectedtargetmap")
qt_add_test_target("test_qsocgeneratereproducibility")
add_dependencies(test_qsocgeneratereproducibility qsoc_formatter_probe)
target_compile_definitions(
    test_qsocgeneratereproducibility
    PRIVATE QSOC_FORMATTER_PROBE_PATH="$<TARGET_FILE:qsoc_formatter_probe>"
)
qt_add_test_target("test_qsoccliparsegenerateresetlogic")
qt_add_test_target("test_qsoccliparsegenerateseqlogic")
qt_add_test_target("test_qsoccliparsegeneratestub")
qt_add_test_target("test_qsoccliparsegeneratetemplate")
qt_add_test_target("test_qsoccliparsegeneratetemplatercsv")
qt_add_test_target("test_qsoccliparsegeneratetemplaterdl")
qt_add_test_target("test_qsoccliparsegeneratetemplateregex")
qt_add_test_target("test_qsoccliparsegeneratetoplevelport")
qt_add_test_target("test_qsoccliparsemodule")
qt_add_test_target("test_qsoccliparsemmio" LABELS "mmio")
qt_add_test_target("test_qsocmmiogenerator" LABELS "mmio")
qt_add_test_target("test_qsociomuxgenerator" LABELS "iomux")
qt_add_test_target("test_qsoccliparseiomux" LABELS "iomux")
qt_add_test_target("test_qsoccliparsegenerateiomux" LABELS "iomux")
qt_add_test_target("test_qsociomuxformal" LABELS "iomux")
qt_add_test_target("test_qsocmmioformal" LABELS "mmio")
qt_add_test_target("test_qsocmmiouvm" LABELS "mmio;iomux")
# Each row drives two verilator elaborations of UVM, budgeted at 300s each,
# so a row can legitimately outlast QtTest's 300s default function watchdog.
# A shared CI runner is about six times slower than a developer machine.
set_tests_properties(
    test_qsocmmiouvm
    PROPERTIES TIMEOUT 3000 RUN_SERIAL TRUE ENVIRONMENT "QTEST_FUNCTION_TIMEOUT=900000")
qt_add_test_target("test_qsocmodulemanagerphase0")
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
qt_add_test_target("test_qsocguibuseditorwindow")
qt_add_test_target("test_qsocguischematicsnapshot")
qt_add_test_target("test_qsocguiprccontrollergrouping")
qt_add_test_target("test_qsocguiprcnetlist")
qt_add_test_target("test_qsocguimoduleeditorwindow")
qt_add_test_target("test_qsocguimoduleeditormodels")
qt_add_test_target("test_qsocguischematicwindow")
qt_add_test_target("test_qsocagenttool")
qt_add_test_target("test_qsocagenttoolshell")
qt_add_test_target("test_qsocmcpnaming")
qt_add_test_target("test_qsocmcpstdio")
add_dependencies(test_qsocmcpstdio qsoc_mcp_stdio_peer)
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
qt_add_test_target("test_qsocagentbackoffabort")
qt_add_test_target("test_qsocagentsubagentguards")
qt_add_test_target("test_qsocagentdefinitionregistry")
qt_add_test_target("test_qsochostprofile")
qt_add_test_target("test_qsoctoolhostcatalog")
qt_add_test_target("test_qsoctoolaskuser")
qt_add_test_target("test_qsocgoal")
qt_add_test_target("test_qsoctoolgoalcomplete")
qt_add_test_target("test_qsocgoalprompt")
qt_add_test_target("test_qsocsubagenttasksource")
qt_add_test_target("test_qsocsubagentabortstate")
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
qt_add_test_target("test_qsoctoolwebhtml")
qt_add_test_target("test_qsocagentattachment")
qt_add_test_target("test_qsocimageattach")
qt_add_test_target("test_qsoctoolfilereadimage")
qt_add_test_target("test_qsocmarkdownrenderer")
qt_add_test_target("test_qtuiscrollviewstyledruns")
qt_add_test_target("test_qtuiblock_text")
qt_add_test_target("test_qtuistatusbarcontext")
qt_add_test_target("test_qsocsessiontitle")
qt_add_test_target("test_qsocawaysummary")
qt_add_test_target("test_qsoccontextrestore")
qt_add_test_target("test_qsochistoryorder")
qt_add_test_target("test_qtuiscrollviewblocks")
qt_add_test_target("test_qtuicompositorstreaming")
qt_add_test_target("test_qtuiscrollviewfocuscopy")
qt_add_test_target("test_qtuiscrollviewgraphicslayer")
qt_add_test_target("test_qtuiblockfold")
qt_add_test_target("test_qtuipathpicker")
qt_add_test_target("test_qsoctoolplanmode")
qt_add_test_target("test_qsocagentreminders")
qt_add_test_target("test_qsoctoolregistrylifecycle")
qt_add_test_target("test_qsoccodehighlighter")
qt_add_test_target("test_qtuiblock_table_overflow")
qt_add_test_target("test_qtuiblock_logicalcopy")
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
qt_add_test_target("test_qsocshellpath")
qt_add_test_target("test_qsocstatusline")
qt_add_test_target("test_qtuiansi")
qt_add_test_target("test_qsocsession")
qt_add_test_target("test_qsocsessionrecovery")
qt_add_test_target("test_qsocsessiontranscript")
qt_add_test_target("test_qsocagentlazystorage")
add_dependencies(test_qsocagentlazystorage qsoc)
qt_add_test_target("test_qsocfilehistory")
qt_add_test_target("test_qsocfilehistoryfence")
qt_add_test_target("test_qsocrewind")
qt_add_test_target("test_qlspslangbackend")
qt_add_test_target("test_qlspservice")
qt_add_test_target("test_qsocsshconfigparser")
qt_add_test_target("test_qsocremotepathcontext")
qt_add_test_target("test_qsoctoolresult")
qt_add_test_target("test_qsocagentremoterecovery" LOCK qsoc_loopback_sshd)
qt_add_test_target("test_qsocremoteconnection")
qt_add_test_target("test_qsocreconnectbudget")
qt_add_test_target("test_qsocremotecwdowner" LOCK qsoc_loopback_sshd)
qt_add_test_target("test_qsocremotecwdpublish" LOCK qsoc_loopback_sshd)
qt_add_test_target("test_qsocremotedeadlinkcost" LOCK qsoc_loopback_sshd)
qt_add_test_target("test_qsocsshabortlatency")
add_dependencies(test_qsocsshabortlatency qsoc)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_definitions(
        test_qsocsshabortlatency PRIVATE
        QSOC_TEST_WRAP_SIGACTION=1
        QSOC_TEST_WRAP_TCSETATTR=1
        QSOC_TEST_WRAP_READ=1
    )
    target_link_options(
        test_qsocsshabortlatency PRIVATE
        "-Wl,--wrap=sigaction"
        "-Wl,--wrap=tcsetattr"
        "-Wl,--wrap=read"
    )
endif()
qt_add_test_target("test_qsocsshagentbudget" LOCK qsoc_loopback_sshd)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_definitions(
        test_qsocsshagentbudget PRIVATE QSOC_TEST_WRAP_AUTH_TRANSPORT=1
    )
    target_link_options(
        test_qsocsshagentbudget PRIVATE
        "-Wl,--wrap=libssh2_userauth_publickey_fromfile_ex"
    )
endif()
qt_add_test_target("test_qsocsshstrandedchannel" LOCK qsoc_loopback_sshd)
qt_add_test_target("test_qsocremoteguardafterreconnect" LOCK qsoc_loopback_sshd)
qt_add_test_target("test_qsoctoolagenthostdispatch" LOCK qsoc_loopback_sshd)
qt_add_test_target("test_qsoctoolagentbindinglifetime" LOCK qsoc_loopback_sshd)
qt_add_test_target("test_qsoctoolagenthostbindingrecovery" LOCK qsoc_loopback_sshd)
qt_add_test_target("test_qsocremotejobtoken")
qt_add_test_target("test_qsocremotejobscript")
qt_add_test_target("test_qsocsshsession" LOCK qsoc_loopback_sshd)
qt_add_test_target("test_qsocsshexec")
qt_add_test_target("test_qsocsshexecstatus" LOCK qsoc_loopback_sshd)
qt_add_test_target("test_qsocsftpclient")
qt_add_test_target("test_qsocsftp_loopback" LOCK qsoc_loopback_sshd)
qt_add_test_target("test_qsocsshpubderive")
qt_add_test_target("test_qlongtaskmonitor")
qt_add_test_target("test_qsocpredictioncontroller")
qt_add_test_target("test_qsocmemoryrecall")
qt_add_test_target("test_qsocmemoryextractor")
qt_add_test_target("test_qsocmemorydream")
qt_add_test_target("test_qsocsftpstranded" LOCK qsoc_loopback_sshd)
foreach(_mock_test
    test_qsocagentlazystorage
    test_qsocagentremoterecovery
    test_qsocremotecwdowner
    test_qsocremotedeadlinkcost
    test_qsocsshabortlatency)
    add_dependencies(${_mock_test} qsoc_mock_llm)
    target_compile_definitions(${_mock_test}
        PRIVATE QSOC_MOCK_LLM_PATH="$<TARGET_FILE:qsoc_mock_llm>")
endforeach()

# Registration is explicit above, so a new test file that nobody registered is
# silently absent. Glob here to assert, never to register: a stale glob can only
# make this check miss, it can never make a test disappear.
file(GLOB _QSOC_TEST_SOURCES "${CMAKE_CURRENT_LIST_DIR}/test_*.cpp")
foreach(_src IN LISTS _QSOC_TEST_SOURCES)
    get_filename_component(_name "${_src}" NAME_WE)
    if(NOT TARGET ${_name})
        message(FATAL_ERROR "${_name}.cpp exists but no qt_add_test_target registers it")
    endif()
endforeach()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_definitions(
        test_qsocsftpstranded PRIVATE QSOC_TEST_WRAP_LIBSSH2_TEARDOWN=1
    )
    target_link_options(
        test_qsocsftpstranded PRIVATE
        "-Wl,--wrap=libssh2_channel_free"
        "-Wl,--wrap=libssh2_session_disconnect_ex"
        "-Wl,--wrap=libssh2_session_free"
    )
endif()
