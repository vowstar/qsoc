// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "cli/qsocexternaleditor.h"
#include "qsoc_test.h"

#include <QScopeGuard>
#include <QtCore>
#include <QtTest>

#ifdef Q_OS_UNIX
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

class Test : public QObject
{
    Q_OBJECT

private slots:
    void cleanup() { qunsetenv("EDITOR"); }

    void testResolveEditorDefault()
    {
        qunsetenv("EDITOR");
        QCOMPARE(QSocExternalEditor::resolveEditor(), QStringLiteral("vi"));
    }

    void testResolveEditorFromEnv()
    {
        qputenv("EDITOR", QByteArrayLiteral("nano"));
        QCOMPARE(QSocExternalEditor::resolveEditor(), QStringLiteral("nano"));
    }

    /* Build a temporary shell script that qsoc can invoke as "sh <script>".
     * This keeps the CI dependency surface at "POSIX sh" only — no vim,
     * nano, or ed required. */
    static QString makeShellScript(const QString &body)
    {
        QTemporaryFile wrapper(QStringLiteral("qsoc-fake-editor-XXXXXX.sh"));
        wrapper.setAutoRemove(false);
        [&] { QVERIFY(wrapper.open()); }();
        wrapper.write(body.toUtf8());
        wrapper.close();
        return wrapper.fileName();
    }

    void testEditTextRoundtrip()
    {
        /* Fake editor: append a marker line to the file the caller passes in. */
        const QString script = makeShellScript(
            QStringLiteral("#!/bin/sh\nprintf 'APPENDED\\n' >> \"$1\"\n"));

        /* Pass the script to `sh`; tests the same code path as EDITOR="code -w". */
        qputenv("EDITOR", (QStringLiteral("sh ") + script).toLocal8Bit());

        /* editText writes "hello world\n" to the tempfile (always trailing \n).
         * The fake script appends "APPENDED\n" after that. The result after
         * stripping one trailing \n is "hello world\nAPPENDED". */
        QString result;
        QString err;
        bool    success = QSocExternalEditor::editText(QStringLiteral("hello world"), result, err);
        QVERIFY2(success, qPrintable(err));
        QCOMPARE(result, QStringLiteral("hello world\nAPPENDED"));

        QFile::remove(script);
    }

    void testEditTextEmptyInput()
    {
        /* Fake editor that writes one line of content. */
        const QString script = makeShellScript(
            QStringLiteral("#!/bin/sh\nprintf 'written\\n' > \"$1\"\n"));
        qputenv("EDITOR", (QStringLiteral("sh ") + script).toLocal8Bit());

        QString result;
        QString err;
        bool    success = QSocExternalEditor::editText(QString(), result, err);
        QVERIFY2(success, qPrintable(err));
        QCOMPARE(result, QStringLiteral("written"));

        QFile::remove(script);
    }

    void testEditorFailureReturnsError()
    {
        /* Use 'false' — exits non-zero */
        qputenv("EDITOR", QByteArrayLiteral("false"));

        QString result;
        QString err;
        bool    success = QSocExternalEditor::editText(QStringLiteral("x"), result, err);
        QVERIFY(!success);
        QVERIFY(!err.isEmpty());
    }

#ifdef Q_OS_UNIX
    void testEditorReadsTerminalInput()
    {
        const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
        QVERIFY(master >= 0);
        const auto closeMaster = qScopeGuard([master] { ::close(master); });
        QCOMPARE(::grantpt(master), 0);
        QCOMPARE(::unlockpt(master), 0);
        const char *slaveName = ::ptsname(master);
        QVERIFY(slaveName != nullptr);
        const int slave = ::open(slaveName, O_RDWR | O_NOCTTY);
        QVERIFY(slave >= 0);
        const auto closeSlave = qScopeGuard([slave] { ::close(slave); });

        const int savedStdin = ::dup(STDIN_FILENO);
        QVERIFY(savedStdin >= 0);
        const auto restoreStdin = qScopeGuard([savedStdin] {
            ::dup2(savedStdin, STDIN_FILENO);
            ::close(savedStdin);
        });
        QVERIFY(::dup2(slave, STDIN_FILENO) >= 0);

        const QString script       = makeShellScript(QStringLiteral(
            "#!/bin/sh\n"
            "test -t 0 || exit 42\n"
            "IFS= read -r input || exit 43\n"
            "printf '%s\\n' \"$input\" > \"$1\"\n"));
        const auto    removeScript = qScopeGuard([script] { QFile::remove(script); });
        qputenv("EDITOR", (QStringLiteral("sh ") + script).toLocal8Bit());

        const QByteArray input("typed in terminal\n");
        QCOMPARE(::write(master, input.constData(), input.size()), ssize_t(input.size()));
        QString result;
        QString error;
        QVERIFY2(
            QSocExternalEditor::editText(QStringLiteral("before"), result, error),
            qPrintable(error));
        QCOMPARE(result, QStringLiteral("typed in terminal"));
        QVERIFY(error.isEmpty());
    }
#endif

    void testEditorMissingBinary()
    {
        qputenv("EDITOR", QByteArrayLiteral("/definitely/does/not/exist/editor"));

        QString result;
        QString err;
        bool    success = QSocExternalEditor::editText(QStringLiteral("x"), result, err);
        QVERIFY(!success);
        QVERIFY(!err.isEmpty());
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocexternaleditor.moc"
