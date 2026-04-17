// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOC_TEST_H
#define QSOC_TEST_H

#include "common/qsocconsole.h"

#include <QBuffer>
#include <QByteArray>
#include <QCoreApplication>
#include <QString>
#include <QStringList>
#include <QtTest>

/**
 * @brief Captures CLI output for tests.
 * @details Bridges three sinks into one inspectable surface:
 *          - Qt log messages (qDebug/qInfo/qWarning/qCritical) via qInstallMessageHandler.
 *          - QSocConsole::out()/err() direct writes via QBuffer redirection.
 *          messages() returns log entries (one per Qt log call).
 *          text() returns combined plain text from all three streams.
 *          Ctor installs, dtor restores. Non-reentrant: do not nest.
 */
class QSocTestCapture
{
public:
    QSocTestCapture()
    {
        outBuffer.open(QIODevice::ReadWrite);
        errBuffer.open(QIODevice::ReadWrite);
        QSocConsole::setOutputDevice(&outBuffer);
        QSocConsole::setErrorDevice(&errBuffer);
        s_target   = &m_messages;
        oldHandler = qInstallMessageHandler(&QSocTestCapture::handler);
    }

    ~QSocTestCapture()
    {
        qInstallMessageHandler(oldHandler);
        QSocConsole::setOutputDevice(nullptr);
        QSocConsole::setErrorDevice(nullptr);
        s_target = nullptr;
    }

    QSocTestCapture(const QSocTestCapture &)            = delete;
    QSocTestCapture &operator=(const QSocTestCapture &) = delete;

    /** Reset all captured content. */
    void clear()
    {
        m_messages.clear();
        outBuffer.close();
        errBuffer.close();
        outBuffer.setData(QByteArray());
        errBuffer.setData(QByteArray());
        outBuffer.open(QIODevice::ReadWrite);
        errBuffer.open(QIODevice::ReadWrite);
    }

    /** Captured Qt log messages, one entry per qDebug/qInfo/etc call. */
    QStringList &messages() { return m_messages; }

    /** Combined text: stdout + stderr + log messages joined by '\n'. */
    QString text()
    {
        QSocConsole::out().flush();
        QSocConsole::err().flush();
        QString combined;
        combined.append(QString::fromUtf8(outBuffer.data()));
        combined.append(QString::fromUtf8(errBuffer.data()));
        if (!m_messages.isEmpty()) {
            combined.append(m_messages.join(QChar::fromLatin1('\n')));
            combined.append(QChar::fromLatin1('\n'));
        }
        return combined;
    }

private:
    static void handler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
    {
        Q_UNUSED(type);
        Q_UNUSED(context);
        if (s_target != nullptr) {
            *s_target << msg;
        }
    }

    inline static QStringList *s_target = nullptr;

    QStringList      m_messages;
    QBuffer          outBuffer;
    QBuffer          errBuffer;
    QtMessageHandler oldHandler = nullptr;
};

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
        const QCoreApplication app(argc, argv); \
        /* Run tests */ \
        TestClass testCase; \
        const int result = QTest::qExec(&testCase, argc, argv); \
        /* Output test completion information */ \
        fprintf(stderr, "Tests completed with result: %d\n", result); \
        /* Exit immediately without waiting for event loop cleanup */ \
        _exit(result ? 1 : 0); \
        /* This line will never be reached */ \
        return result; \
    }

#endif // QSOC_TEST_H
