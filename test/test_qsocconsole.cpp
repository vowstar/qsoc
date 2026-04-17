// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocconsole.h"
#include "qsoc_test.h"

#include <QtTest>

#ifndef Q_OS_WIN
#include <pty.h>
#include <unistd.h>
#endif

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qunsetenv("NO_COLOR");
        qunsetenv("FORCE_COLOR");
    }

    void cleanupTestCase()
    {
        QSocConsole::setColorMode(QSocConsole::ColorMode::Auto);
        qunsetenv("NO_COLOR");
        qunsetenv("FORCE_COLOR");
    }

    /* dim(text) wraps in an ANSI escape pair when color is on, returns plain otherwise. */
    void dimRespectsAlways()
    {
        QSocConsole::setColorMode(QSocConsole::ColorMode::Always);
        const QString out = QSocConsole::dim(QStringLiteral("hello"));
        QVERIFY(out.startsWith(QStringLiteral("\033[")));
        QVERIFY(out.endsWith(QStringLiteral("\033[0m")));
        QVERIFY(out.contains(QStringLiteral("hello")));
    }

    void dimRespectsNever()
    {
        QSocConsole::setColorMode(QSocConsole::ColorMode::Never);
        QCOMPARE(QSocConsole::dim(QStringLiteral("hello")), QStringLiteral("hello"));
    }

    /* In Auto mode, NO_COLOR (any non-empty value) wins. */
    void autoHonorsNoColor()
    {
        QSocConsole::setColorMode(QSocConsole::ColorMode::Auto);
        qputenv("NO_COLOR", "1");
        QCOMPARE(QSocConsole::dim(QStringLiteral("x")), QStringLiteral("x"));
        qunsetenv("NO_COLOR");
    }

    /* In Auto mode, FORCE_COLOR overrides isatty (test runs without TTY). */
    void autoHonorsForceColor()
    {
        QSocConsole::setColorMode(QSocConsole::ColorMode::Auto);
        qputenv("FORCE_COLOR", "1");
        const QString out = QSocConsole::dim(QStringLiteral("x"));
        QVERIFY(out.startsWith(QStringLiteral("\033[")));
        qunsetenv("FORCE_COLOR");
    }

    /* Always overrides NO_COLOR (matches `ls --color=always` semantics). */
    void alwaysIgnoresNoColor()
    {
        qputenv("NO_COLOR", "1");
        QSocConsole::setColorMode(QSocConsole::ColorMode::Always);
        const QString out = QSocConsole::dim(QStringLiteral("x"));
        QVERIFY(out.startsWith(QStringLiteral("\033[")));
        qunsetenv("NO_COLOR");
    }

    /* Never wins over FORCE_COLOR. */
    void neverIgnoresForceColor()
    {
        qputenv("FORCE_COLOR", "1");
        QSocConsole::setColorMode(QSocConsole::ColorMode::Never);
        QCOMPARE(QSocConsole::dim(QStringLiteral("x")), QStringLiteral("x"));
        qunsetenv("FORCE_COLOR");
    }

#ifndef Q_OS_WIN
    /* Auto + no env + isatty(stdout) == 1 → emit color. Uses openpty() to
       replace stdout with a real pty fd, so isatty() returns true. */
    void autoEnablesColorOnTty()
    {
        QSocConsole::setColorMode(QSocConsole::ColorMode::Auto);
        qunsetenv("NO_COLOR");
        qunsetenv("FORCE_COLOR");

        int master = -1;
        int slave  = -1;
        QVERIFY(openpty(&master, &slave, nullptr, nullptr, nullptr) == 0);
        QVERIFY(isatty(slave) == 1);

        const int savedStdout = dup(STDOUT_FILENO);
        QVERIFY(savedStdout >= 0);
        QVERIFY(dup2(slave, STDOUT_FILENO) >= 0);
        QVERIFY(isatty(STDOUT_FILENO) == 1);

        const QString out = QSocConsole::dim(QStringLiteral("warm"));

        QVERIFY(dup2(savedStdout, STDOUT_FILENO) >= 0);
        ::close(savedStdout);
        ::close(slave);
        ::close(master);

        QVERIFY(out.startsWith(QStringLiteral("\033[")));
        QVERIFY(out.endsWith(QStringLiteral("\033[0m")));
    }
#endif

    /* setLevel/level round-trip. */
    void levelRoundTrip()
    {
        QSocConsole::setLevel(QSocConsole::Level::Debug);
        QCOMPARE(QSocConsole::level(), QSocConsole::Level::Debug);
        QSocConsole::setLevel(QSocConsole::Level::Error);
        QCOMPARE(QSocConsole::level(), QSocConsole::Level::Error);
    }
};

QSOC_TEST_MAIN(Test)

#include "test_qsocconsole.moc"
