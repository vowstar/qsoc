// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"
#include "common/config.h"
#include "qsoc_test.h"

#include <QStringList>
#include <QThread>
#include <QtCore>
#include <QtTest>

#include <iostream>

struct TestApp
{
    static auto &instance()
    {
        static auto                  argc      = 1;
        static char                  appName[] = "qsoc";
        static std::array<char *, 1> argv      = {{appName}};
        /* Use QCoreApplication for cli test */
        static const QCoreApplication app = QCoreApplication(argc, argv.data());
        return app;
    }
};

class Test : public QObject
{
    Q_OBJECT

private:
    static QStringList messageList;
    static void messageOutput(QtMsgType type, const QMessageLogContext &context, const QString &msg)
    {
        Q_UNUSED(type);
        Q_UNUSED(context);
        messageList << msg;
    }

private slots:
    void initTestCase()
    {
        TestApp::instance();
        qInstallMessageHandler(messageOutput);
    }

    void cleanupTestCase() { /* Do nothing */ }

    void optionH()
    {
        messageList.clear();
        {
            QSocCliWorker     socCliWorker;
            const QStringList appArguments = {
                "qsoc",
                "-h",
            };
            socCliWorker.setup(appArguments, true);
            socCliWorker.run();
        }

        /* Check that the message list contains the expected message */
        QCOMPARE(messageList.count(), 1);
        QVERIFY(messageList.first().contains("Usage: qsoc [options]"));
    }

    void optionHelp()
    {
        messageList.clear();
        {
            QSocCliWorker     socCliWorker;
            const QStringList appArguments = {
                "qsoc",
                "--help",
            };
            socCliWorker.setup(appArguments, true);
            socCliWorker.run();
        }

        /* Check that the message list contains the expected message */
        QCOMPARE(messageList.count(), 1);
        QVERIFY(messageList.first().contains("Usage: qsoc [options]"));
    }

    void optionVerbose()
    {
        messageList.clear();
        {
            QSocCliWorker     socCliWorker;
            const QStringList appArguments = {
                "qsoc",
                "--verbose=10",
            };
            socCliWorker.setup(appArguments, true);
            socCliWorker.run();
        }

        /* Check that the message list contains the expected message */
        QCOMPARE(messageList.count(), 3);
        QVERIFY(messageList.first().contains("Error: invalid log level: 10"));
    }

    void optionV()
    {
        messageList.clear();
        {
            QSocCliWorker     socCliWorker;
            const QStringList appArguments = {
                "qsoc",
                "-v",
            };
            socCliWorker.setup(appArguments, true);
            socCliWorker.run();
        }

        /* Check that the message list contains the expected message */
        QCOMPARE(messageList.count(), 1);
        QVERIFY(messageList.first().contains("QSoC " QSOC_VERSION));
    }

    void optionVersion()
    {
        messageList.clear();
        {
            QSocCliWorker     socCliWorker;
            const QStringList appArguments = {
                "qsoc",
                "--version",
            };
            socCliWorker.setup(appArguments, true);
            socCliWorker.run();
        }

        /* Check that the message list contains the expected message */
        QCOMPARE(messageList.count(), 1);
        QVERIFY(messageList.first().contains("QSoC " QSOC_VERSION));
    }
};

QStringList Test::messageList;

QSOC_TEST_MAIN(Test)

#include "test_qsoccliworker.moc"
