// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOC_TEST_H
#define QSOC_TEST_H

#include <QCoreApplication>
#include <QtTest>

/**
 * @brief QSOC_TEST_MAIN macro for QSOC test applications
 * @details This macro provides a custom main function for QSOC test applications
 *          that avoids segmentation faults during test exit by using _exit instead
 *          of waiting for the event loop to clean up.
 * @param TestClass The test class name
 */
#define QSOC_TEST_MAIN(TestClass) \
    int main(int argc, char *argv[]) \
    { \
        /* Create application instance */ \
        QCoreApplication app(argc, argv); \
        /* Run tests */ \
        TestClass tc; \
        int       result = QTest::qExec(&tc, argc, argv); \
        /* Output test completion information */ \
        fprintf(stderr, "Tests completed with result: %d\n", result); \
        /* Exit immediately without waiting for event loop cleanup */ \
        _exit(result ? 1 : 0); \
        /* This line will never be reached */ \
        return result; \
    }

#endif // QSOC_TEST_H
