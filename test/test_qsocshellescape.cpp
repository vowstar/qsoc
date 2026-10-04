// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/runtime/qsocagentruntime_p.h"
#include "common/qsocshellpath.h"
#include "qsoc_test.h"

#include <QDir>
#include <QTemporaryDir>
#include <QtTest>

class Test : public QObject
{
    Q_OBJECT

private slots:
    void cmdReceivesTheLineVerbatim()
    {
        const QString line = QStringLiteral(R"(echo "a b" & findstr /c:"x y" "my file.txt")");
        QCOMPARE(
            QSocShellPath::cmdExeNativeArguments(line),
            QStringLiteral(R"(/d /s /c "echo "a b" & findstr /c:"x y" "my file.txt"")"));
        QCOMPARE(QSocShellPath::cmdExeNativeArguments({}), QStringLiteral(R"(/d /s /c "")"));
    }

#ifdef Q_OS_UNIX
    void aLocalLineReportsItsExitAndShell()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString out = QSocAgentRuntimeInternal::runLocalShellEscape(
            QStringLiteral("printf 'x'; pwd >&2; exit 2"), dir.path(), {});
        QVERIFY2(out.startsWith(QStringLiteral("x")), qPrintable(out));
        QVERIFY2(out.contains(QDir(dir.path()).canonicalPath()), qPrintable(out));
        QVERIFY2(out.contains(QStringLiteral("\n(exit code: 2)\n")), qPrintable(out));
        QVERIFY2(out.endsWith(QStringLiteral("(shell: /bin/sh)\n")), qPrintable(out));
    }
#endif

#ifdef Q_OS_WIN
    void cmdKeepsQuotesAndDecodesNonAscii()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        /* Spelled as an escape: MSVC reads this file in the ANSI code page. */
        const QString word = QStringLiteral("caf") + QChar(0x00E9);
        const QString out  = QSocAgentRuntimeInternal::runLocalShellEscape(
            QStringLiteral("echo \"quoted words\" ") + word, dir.path(), {});
        QVERIFY2(out.contains(QStringLiteral("\"quoted words\" ") + word), qPrintable(out));
        QVERIFY2(out.endsWith(QStringLiteral("(shell: cmd.exe)\n")), qPrintable(out));
    }
#endif
};

QSOC_TEST_MAIN(Test)
#include "test_qsocshellescape.moc"
