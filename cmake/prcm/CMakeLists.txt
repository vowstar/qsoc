# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

set(CMAKE_AUTOMOC OFF)
set(CMAKE_AUTOUIC OFF)
set(CMAKE_AUTORCC OFF)
set(Z3_BUILD_LIBZ3_SHARED OFF CACHE BOOL "Build the bundled solver statically" FORCE)
set(Z3_BUILD_EXECUTABLE OFF CACHE BOOL "Build the solver command line" FORCE)
set(Z3_BUILD_TEST_EXECUTABLES OFF CACHE BOOL "Build upstream solver tests" FORCE)
set(Z3_BUILD_PYTHON_BINDINGS OFF CACHE BOOL "Build Python bindings" FORCE)
set(Z3_BUILD_DOTNET_BINDINGS OFF CACHE BOOL "Build .NET bindings" FORCE)
set(Z3_BUILD_JAVA_BINDINGS OFF CACHE BOOL "Build Java bindings" FORCE)
set(Z3_INCLUDE_GIT_HASH OFF CACHE BOOL "Include solver Git metadata" FORCE)
set(Z3_INCLUDE_GIT_DESCRIBE OFF CACHE BOOL "Include solver Git description" FORCE)
add_subdirectory("${PROJECT_SOURCE_DIR}/external/z3"
                 "${PROJECT_BINARY_DIR}/external/z3" EXCLUDE_FROM_ALL)
target_include_directories(libz3 SYSTEM INTERFACE
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/external/z3/src/api/c++>")
