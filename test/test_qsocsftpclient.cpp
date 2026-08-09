// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocagentremote.h"
#include "agent/remote/qsocsftpclient.h"
#include "agent/remote/qsocsshsession.h"
#include "qsoc_test.h"

#include <QtCore>
#include <QtTest>

namespace {

class Test : public QObject
{
    Q_OBJECT

private slots:
    /* With no underlying session, open() fails with a descriptive error and
     * isOpen() stays false. Destructor must not crash. */
    void testOpenWithoutSession()
    {
        QSocSshSession session;
        QSocSftpClient sftp(session);
        QVERIFY(!sftp.isOpen());
        QString err;
        QCOMPARE(sftp.open(&err), false);
        QVERIFY(!sftp.isOpen());
        QVERIFY(!err.isEmpty());
    }

    /* Operations should short-circuit to errors rather than segfault when
     * SFTP is not open. */
    void testOperationsWithoutOpen()
    {
        QSocSshSession session;
        QSocSftpClient sftp(session);

        QString err;
        QVERIFY(sftp.readFile(QStringLiteral("/tmp/nothing"), 0, &err).isEmpty());
        QVERIFY(!err.isEmpty());

        err.clear();
        QCOMPARE(sftp.writeFile(QStringLiteral("/tmp/nothing"), QByteArray("x"), &err), false);
        QVERIFY(!err.isEmpty());

        err.clear();
        QCOMPARE(sftp.mkdirP(QStringLiteral("/tmp/qsoc-test"), &err), false);
        QVERIFY(!err.isEmpty());

        err.clear();
        QCOMPARE(sftp.rename(QStringLiteral("/a"), QStringLiteral("/b"), &err), false);
        QVERIFY(!err.isEmpty());

        err.clear();
        QVERIFY(sftp.listDir(QStringLiteral("/tmp"), 0, &err).isEmpty());
        QVERIFY(!err.isEmpty());

        err.clear();
        QCOMPARE(sftp.presence(QStringLiteral("/tmp"), &err), QSocSftpClient::Presence::Unknown);
        QVERIFY(!err.isEmpty());
    }

    /* The shared liveness probe must fail closed: no client, no root, or a
     * host that cannot answer all mean "do not act on this workspace". */
    void testWorkspaceProbeFailsClosed()
    {
        QString err;
        QVERIFY(!remoteHostAnswers(nullptr, QStringLiteral("/tmp"), 500, &err));
        QVERIFY(!err.isEmpty());

        QSocSshSession session;
        QSocSftpClient sftp(session);
        err.clear();
        QVERIFY(!remoteHostAnswers(&sftp, QStringLiteral("/tmp"), 500, &err));
        QVERIFY(!err.isEmpty());

        err.clear();
        QVERIFY(!remoteHostAnswers(&sftp, QString(), 500, &err));
        QVERIFY(!err.isEmpty());
    }

    /* The probe must leave the client's own budget as it found it. */
    void testWorkspaceProbeRestoresTheOperationBudget()
    {
        QSocSshSession session;
        QSocSftpClient sftp(session);
        sftp.setOperationTimeoutMs(12345);
        (void) remoteHostAnswers(&sftp, QStringLiteral("/tmp"), 500, nullptr);
        QCOMPARE(sftp.operationTimeoutMs(), 12345);
    }

    /* A stat that never reached a server must report Unknown, never Absent:
     * write_file skips its read-before-overwrite guard on Absent, so the
     * two must stay distinguishable. */
    void testPresenceWithoutSessionIsUnknownNotAbsent()
    {
        QSocSshSession session;
        QSocSftpClient sftp(session);
        QString        err;
        const auto     result = sftp.presence(QStringLiteral("/definitely/not/here"), &err);
        QCOMPARE(result, QSocSftpClient::Presence::Unknown);
        QVERIFY(result != QSocSftpClient::Presence::Absent);
        QVERIFY(!err.isEmpty());
    }

    /* removeFile must not claim success when it could not learn whether the
     * file is there. */
    void testRemoveFileFailsWhenPresenceUnknown()
    {
        QSocSshSession session;
        QSocSftpClient sftp(session);
        QString        err;
        QCOMPARE(sftp.removeFile(QStringLiteral("/tmp/nothing"), &err), false);
        QVERIFY(!err.isEmpty());
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocsftpclient.moc"
