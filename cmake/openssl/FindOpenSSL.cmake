# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

# find_package(OpenSSL) for the bundled libssh2: the AWS-LC crypto target
# built by external/aws-lc, never a system OpenSSL.
if(NOT TARGET crypto)
    message(FATAL_ERROR "FindOpenSSL: add_subdirectory(external/aws-lc) first")
endif()
if(NOT TARGET OpenSSL::Crypto)
    add_library(OpenSSL::Crypto ALIAS crypto)
endif()
set(OPENSSL_INCLUDE_DIR    "${CMAKE_CURRENT_LIST_DIR}/../../external/aws-lc/include")
set(OPENSSL_CRYPTO_LIBRARY crypto)
set(OPENSSL_LIBRARIES      crypto)
set(OPENSSL_VERSION        "1.1.1")
set(OPENSSL_FOUND          TRUE)
set(OpenSSL_FOUND          TRUE)
