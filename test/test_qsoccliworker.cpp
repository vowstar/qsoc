// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"
#include "common/config.h"
#include "common/qsocconsole.h"
#include "qsoc_test.h"

#include <QBuffer>
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
    QBuffer outBuffer;
    QBuffer errBuffer;

    void resetCapture()
    {
        outBuffer.close();
        errBuffer.close();
        outBuffer.setData(QByteArray());
        errBuffer.setData(QByteArray());
        outBuffer.open(QIODevice::ReadWrite);
        errBuffer.open(QIODevice::ReadWrite);
        QSocConsole::setOutputDevice(&outBuffer);
        QSocConsole::setErrorDevice(&errBuffer);
    }

    QString captured()
    {
        QSocConsole::out().flush();
        QSocConsole::err().flush();
        return QString::fromUtf8(outBuffer.data()) + QString::fromUtf8(errBuffer.data());
    }

private slots:
    void initTestCase() { TestApp::instance(); }

    void cleanupTestCase()
    {
        QSocConsole::setOutputDevice(nullptr);
        QSocConsole::setErrorDevice(nullptr);
    }

    void optionH()
    {
        resetCapture();
        {
            QSocCliWorker     socCliWorker;
            const QStringList appArguments = {
                "qsoc",
                "-h",
            };
            socCliWorker.setup(appArguments, true);
            socCliWorker.run();
        }
        QVERIFY(captured().contains("Usage: qsoc [options]"));
    }

    void optionHelp()
    {
        resetCapture();
        {
            QSocCliWorker     socCliWorker;
            const QStringList appArguments = {
                "qsoc",
                "--help",
            };
            socCliWorker.setup(appArguments, true);
            socCliWorker.run();
        }
        QVERIFY(captured().contains("Usage: qsoc [options]"));
    }

    void optionVerbose()
    {
        resetCapture();
        {
            QSocCliWorker     socCliWorker;
            const QStringList appArguments = {
                "qsoc",
                "--verbose=10",
            };
            socCliWorker.setup(appArguments, true);
            socCliWorker.run();
        }
        const QString text = captured();
        QVERIFY(text.contains("Error: invalid log level: 10"));
        QVERIFY(text.contains("QSoC " QSOC_VERSION));
        QVERIFY(text.contains("Usage: qsoc [options]"));
    }

    void optionV()
    {
        resetCapture();
        {
            QSocCliWorker     socCliWorker;
            const QStringList appArguments = {
                "qsoc",
                "-v",
            };
            socCliWorker.setup(appArguments, true);
            socCliWorker.run();
        }
        QVERIFY(captured().contains("QSoC " QSOC_VERSION));
    }

    void optionVersion()
    {
        resetCapture();
        {
            QSocCliWorker     socCliWorker;
            const QStringList appArguments = {
                "qsoc",
                "--version",
            };
            socCliWorker.setup(appArguments, true);
            socCliWorker.run();
        }
        QVERIFY(captured().contains("QSoC " QSOC_VERSION));
    }
};

QSOC_TEST_MAIN(Test)

#include "test_qsoccliworker.moc"
