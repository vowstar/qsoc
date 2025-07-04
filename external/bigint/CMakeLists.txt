# This CMake file builds the Big Integer Library
# Original library by Matt McCutchen, placed in the public domain

cmake_minimum_required(VERSION 3.10)

project(bigint VERSION 2010.04.30 LANGUAGES CXX)

# Library source files
set(BIGINT_SOURCES
    BigInteger.cpp
    BigIntegerAlgorithms.cpp
    BigIntegerUtils.cpp
    BigUnsigned.cpp
    BigUnsignedInABase.cpp
)

# Library header files
set(BIGINT_HEADERS
    BigInteger.h
    BigIntegerAlgorithms.h
    BigIntegerLibrary.h
    BigIntegerUtils.h
    BigUnsigned.h
    BigUnsignedInABase.h
    NumberlikeArray.h
)

# Create static library
add_library(bigint STATIC ${BIGINT_SOURCES} ${BIGINT_HEADERS})

# Set C++ standard (match the main project)
set_target_properties(bigint PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

# Include directories
target_include_directories(bigint
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Create an alias for the library with a namespace
add_library(bigint::bigint ALIAS bigint)
