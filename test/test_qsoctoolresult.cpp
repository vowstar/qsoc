// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsoctool.h"
#include "qsoc_test.h"

#include <QtCore>
#include <QtTest>

namespace {

bool isOk(const QString &result)
{
    return QSocTool::classifyResult(result) == QSocTool::ResultStatus::Ok;
}

bool isFailed(const QString &result)
{
    return QSocTool::classifyResult(result) == QSocTool::ResultStatus::Failed;
}

bool isUncertain(const QString &result)
{
    return QSocTool::classifyResult(result) == QSocTool::ResultStatus::Uncertain;
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    /* The universal convention: a leading "Error:" is a failed call. */
    void errorPrefixIsFailure()
    {
        QVERIFY(isFailed(QStringLiteral("Error: file_path is required")));
        QVERIFY(isFailed(QStringLiteral("  Error: not connected\n")));
    }

    void plainResultIsSuccess()
    {
        QVERIFY(isOk(QStringLiteral("Wrote /tmp/a (3 bytes)")));
        QVERIFY(isOk(QString()));
    }

    /* An explicit status wins over everything else. */
    void explicitStatusIsAuthoritative()
    {
        QVERIFY(isUncertain(QStringLiteral("status: uncertain\nexit_code: -1\ntimed_out: true\n")));
        QVERIFY(isFailed(QStringLiteral("status: failed\nerror: exec did not start\n")));
        QVERIFY(isOk(QStringLiteral("status: ok\nexit_code: 1\nstdout:\nno match\n")));
    }

    /* The status line must be first. A body that quotes one cannot forge a
     * verdict, and a body that hides one cannot mask a real failure. */
    void statusIsOnlyReadFromTheFirstLine()
    {
        QVERIFY(isOk(QStringLiteral("exit_code: 0\nstdout:\nstatus: uncertain\n")));
        QVERIFY(isOk(QStringLiteral("module top;\nstatus: failed\nendmodule\n")));
    }

    /* Body text is never inspected, so a build log or a source file that
     * mentions an error is not a failed tool call. */
    void bodyTextIsNeverScanned()
    {
        QVERIFY(isOk(QStringLiteral("status: ok\nexit_code: 0\nstdout:\nfoo.c:1: error: x\n")));
        QVERIFY(isOk(QStringLiteral("module top;\n// error: just source text\nendmodule\n")));
    }

    /* Partial output followed by a disconnect: the metadata sits ahead of
     * the body precisely so this cannot read as a success. */
    void partialOutputThenDisconnectIsUncertain()
    {
        const QString result = QStringLiteral(
            "status: uncertain\n"
            "exit_code: -1\n"
            "timed_out: true\n"
            "transport_dead: true\n"
            "error: SSH transport is dead\n"
            "stdout:\n"
            "half of the build log\n");
        QVERIFY(isUncertain(result));
    }

    /* A non-zero exit is a real answer from a command that ran, not a
     * broken call: grep exits 1 on no match. */
    void nonZeroExitIsNotAFailedCall()
    {
        QVERIFY(isOk(QStringLiteral("status: ok\nexit_code: 1\nstdout:\n")));
    }

    /* A tool that answers in JSON declares its status there. Leaving those
     * unstyled painted a failed sub-agent dispatch with a green tick. */
    void jsonStatusIsHonoured()
    {
        QVERIFY(isFailed(
            QStringLiteral("{\"status\":\"error\",\"error\":\"host 'x' is not dispatchable\"}")));
        QVERIFY(isUncertain(QStringLiteral("{\"status\":\"uncertain\",\"detail\":\"n\"}")));
        QVERIFY(isOk(QStringLiteral("{\"status\":\"ok\",\"task_id\":\"7\"}")));
        /* No status member, or not an object: nothing is claimed. */
        QVERIFY(isOk(QStringLiteral("{\"task_id\":\"7\"}")));
        QVERIFY(isOk(QStringLiteral("[{\"status\":\"error\"}]")));
        /* A nested status must not be mistaken for the tool's own. */
        QVERIFY(isOk(QStringLiteral("{\"child\":{\"status\":\"error\"}}")));
        /* Malformed JSON is just text. */
        QVERIFY(isOk(QStringLiteral("{\"status\":")));
    }

    /* statusLine and classifyResult must agree, or the wire format drifts
     * from the parser. */
    void statusLineRoundTrips()
    {
        for (const auto status :
             {QSocTool::ResultStatus::Ok,
              QSocTool::ResultStatus::Failed,
              QSocTool::ResultStatus::Uncertain}) {
            const QString emitted = QSocTool::statusLine(status) + QStringLiteral("body\n");
            QCOMPARE(QSocTool::classifyResult(emitted), status);
        }
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsoctoolresult.moc"
