// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/runtime/qsocagentruntime_p.h"
#include "common/qsocboundedcapture.h"
#include "common/qsocshellexecutor.h"
#include "common/qsocshellpath.h"
#include "qsoc_test.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
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
    void cleanup() { setLocalShellResolver({}); }

    void aLocalLineReportsItsExitAndShell()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString out = QSocAgentRuntimeInternal::runLocalShellEscape(
            QStringLiteral("printf 'x'; pwd >&2; exit 2"), dir.path(), {});
        QVERIFY2(out.startsWith(QStringLiteral("x")), qPrintable(out));
        QVERIFY2(out.contains(QDir(dir.path()).canonicalPath()), qPrintable(out));
        QVERIFY2(out.contains(QStringLiteral("\n(exit code: 2)\n")), qPrintable(out));
        QVERIFY2(
            out.endsWith(QStringLiteral("(shell: %1)\n").arg(localShellExecutor().summary())),
            qPrintable(out));
    }

    /* Counterexample: a `!` line read all its output into memory, and a line
     * reading stdin waited on a pipe nobody closed. */
    void aLocalLineKeepsBoundedOutputAndNoStdin()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QElapsedTimer clock;
        clock.start();
        const QString out = QSocAgentRuntimeInternal::runLocalShellEscape(
            QStringLiteral("cat; yes | head -c 20000000; echo LAST"), dir.path(), {});
        QVERIFY2(clock.elapsed() < 20000, qPrintable(QString::number(clock.elapsed())));
        QVERIFY(QSocBoundedCapture::isElided(out));
        QVERIFY2(
            out.toUtf8().size() <= QSocBoundedCapture::kDefaultLimit + 256,
            qPrintable(out.right(80)));
        QVERIFY2(out.contains(QStringLiteral("LAST\n")), qPrintable(out.right(80)));
    }

    void aLocalLineRunsUnderTheResolvedExecutor()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString wrapper = dir.filePath(QStringLiteral("resolved-sh"));
        QFile         file(wrapper);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("#!/bin/sh\necho via-resolved\nexec /bin/sh \"$@\"\n");
        file.close();
        QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QSocShellExecutor shell;
        shell.kind = QSocShellExecutor::Kind::Sh;
        shell.path = wrapper;
        setLocalShellResolver([shell] { return shell; });
        const QString out = QSocAgentRuntimeInternal::runLocalShellEscape(
            QStringLiteral("echo ran"), dir.path(), {});
        QVERIFY2(out.startsWith(QStringLiteral("via-resolved\nran\n")), qPrintable(out));
        QVERIFY2(out.contains(QStringLiteral("(shell: sh (POSIX only")), qPrintable(out));

        setLocalShellResolver([] { return QSocShellExecutor{}; });
        const QString none = QSocAgentRuntimeInternal::runLocalShellEscape(
            QStringLiteral("echo ran"), dir.path(), {});
        QVERIFY2(none.startsWith(QStringLiteral("Error: no shell")), qPrintable(none));
        QVERIFY(!none.contains(QStringLiteral("ran\n")));
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
        QVERIFY2(out.endsWith(QStringLiteral("(shell: cmd)\n")), qPrintable(out));
    }
#endif
};

QSOC_TEST_MAIN(Test)
#include "test_qsocshellescape.moc"
