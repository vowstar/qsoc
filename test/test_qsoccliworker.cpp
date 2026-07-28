// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"
#include "common/config.h"
#include "common/qsocconsole.h"
#include "qsoc_test.h"

#include <QBuffer>
#include <QStringList>
#include <QtCore>
#include <QtTest>

#include <array>
#include <cstdio>

namespace {

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
    void cleanupTestCase()
    {
        QSocConsole::setOutputDevice(nullptr);
        QSocConsole::setErrorDevice(nullptr);
    }

    void explicitArgumentsIgnoreHostArguments()
    {
        resetCapture();
        QSocCliWorker socCliWorker;
        QSignalSpy    exitSpy(&socCliWorker, &QSocCliWorker::exit);
        socCliWorker.setup({"qsoc", "gui"}, true);

        QTRY_COMPARE(exitSpy.count(), 1);
        QCOMPARE(exitSpy.takeFirst().at(0).toInt(), 0);
        QVERIFY(!captured().contains("host-only"));
    }

    void invalidGuiOptionIsReported()
    {
        resetCapture();
        QSocCliWorker socCliWorker;
        QSignalSpy    exitSpy(&socCliWorker, &QSocCliWorker::exit);
        socCliWorker.setup({"qsoc", "gui", "--bogus"}, true);

        QTRY_COMPARE(exitSpy.count(), 1);
        QCOMPARE(exitSpy.takeFirst().at(0).toInt(), 1);
        QVERIFY(captured().contains("Unknown option 'bogus'"));
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
        const QString text = captured();
        QVERIFY2(text.contains("Usage: qsoc [options]"), qPrintable(text));
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

} // namespace

int main(int argc, char *argv[])
{
    int                    hostArgc     = 2;
    char                   hostName[]   = "qsoc";
    char                   hostOption[] = "--host-only";
    std::array<char *, 2>  hostArgv{{hostName, hostOption}};
    const QCoreApplication application(hostArgc, hostArgv.data());
    Test                   testCase;
    const int              result = QTest::qExec(&testCase, argc, argv);
    fprintf(stderr, "Tests completed with result: %d\n", result);
    _exit(result ? 1 : 0);
    return result;
}

#include "test_qsoccliworker.moc"
