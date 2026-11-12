// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocsftpclient.h"
#include "agent/remote/qsocsshexec.h"
#include "agent/remote/qsocsshsession.h"
#include "qsoc_test.h"
#include "qsoc_test_sftp_peer.h"

#include <QFile>
#include <QScopeGuard>
#include <QUuid>
#include <QtTest>

namespace {
class Test : public QObject
{
    Q_OBJECT
private slots:
    void access_data()
    {
        QTest::addColumn<bool>("readOnly");
        QTest::newRow("writable") << false;
        QTest::newRow("read-only") << true;
    }
    void access()
    {
        QFETCH(bool, readOnly);
        QSocTestSftpPeer fixture;
        if (!fixture.start(readOnly) && fixture.missing())
            QSOC_TEST_MISSING_DEPENDENCY(fixture.error());
        QVERIFY2(fixture.error().isEmpty(), qPrintable(fixture.error()));
        QTemporaryDir shadow;
        QVERIFY(shadow.isValid());
        QByteArray expected(65536, '\0');
        for (qsizetype index = 0; index < expected.size(); ++index)
            expected[index] = char(index % 251);
        QFile source(fixture.workDir() + QStringLiteral("/sample.bin"));
        QVERIFY(source.open(QIODevice::WriteOnly));
        QCOMPARE(source.write(expected), expected.size());
        source.close();
        QFile local(shadow.filePath(QStringLiteral("sample.bin")));
        QVERIFY(local.open(QIODevice::WriteOnly));
        QCOMPARE(local.write("local shadow"), qint64(12));
        local.close();
        const auto previous = QDir::currentPath();
        QVERIFY(QDir::setCurrent(shadow.path()));
        const auto restore = qScopeGuard([&] { QDir::setCurrent(previous); });
        {
            QString        error;
            QSocSshSession rejected;
            auto           wrong = fixture.hostConfig();
            wrong.user           = QStringLiteral("not-authorized");
            QCOMPARE(rejected.connectTo(wrong, &error), QSocSshSession::ConnectStatus::AuthFailed);
            QSocSshSession session;
            error.clear();
            QCOMPARE(
                session.connectTo(fixture.hostConfig(), &error), QSocSshSession::ConnectStatus::Ok);
            const auto exec
                = QSocSshExec(session).run(QStringLiteral("qsoc_exec_must_not_run"), 3000);
            QVERIFY(!exec.errorText.isEmpty());
            QVERIFY(exec.stdoutBytes.isEmpty());
            QVERIFY(!exec.timedOut);
            QCOMPARE(fixture.execRequests(), 1);
            QSocSftpClient sftp(session);
            QString        canonical;
            QCOMPARE(
                sftp.realPath(QStringLiteral("."), &canonical, &error),
                QSocSftpClient::Presence::Present);
            QCOMPARE(canonical, fixture.canonicalDir());
            error.clear();
            QCOMPARE(sftp.readFile(QStringLiteral("sample.bin"), expected.size(), &error), expected);
            QVERIFY2(error.isEmpty(), qPrintable(error));
            bool remoteStaging = false;
            bool localStaging  = false;
            sftp.setDataPhaseObserver([&] {
                const QStringList patterns{QStringLiteral(".qsoc-write-*")};
                remoteStaging = !QDir(fixture.workDir())
                                     .entryList(patterns, QDir::Files | QDir::Hidden)
                                     .isEmpty();
                localStaging
                    = !QDir(shadow.path()).entryList(patterns, QDir::Files | QDir::Hidden).isEmpty();
            });
            const bool wrote = sftp.writeFile(QStringLiteral("fresh.bin"), expected, &error);
            sftp.setDataPhaseObserver({});
            QVERIFY2(readOnly || wrote, qPrintable(error));
            QCOMPARE(wrote, !readOnly);
            QCOMPARE(QFileInfo::exists(fixture.workDir() + QStringLiteral("/fresh.bin")), !readOnly);
            if (readOnly) {
                QVERIFY(!error.isEmpty());
            } else {
                QVERIFY(remoteStaging);
                QVERIFY(!localStaging);
                QCOMPARE(
                    sftp.readFile(QStringLiteral("fresh.bin"), expected.size(), &error), expected);
                QVERIFY(sftp.removeFile(QStringLiteral("fresh.bin"), &error));
                const QString nested
                    = QStringLiteral("nested-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
                for (const QString &path :
                     {nested + QStringLiteral("/deep/fresh.bin"),
                      fixture.canonicalDir() + QStringLiteral("/absolute.bin")}) {
                    error.clear();
                    QVERIFY2(sftp.writeFile(path, expected, &error), qPrintable(error));
                    QCOMPARE(sftp.readFile(path, expected.size(), &error), expected);
                    QVERIFY2(error.isEmpty(), qPrintable(error));
                    QVERIFY(sftp.removeFile(path, &error));
                }
                QVERIFY(!QFileInfo::exists(shadow.filePath(nested)));
            }
            QCOMPARE(sftp.readFile(QStringLiteral("sample.bin"), expected.size(), &error), expected);
            for (bool closeFirst : {false, true}) {
                session.disconnectFromHost();
                QVERIFY(!sftp.isOpen());
                QCOMPARE(
                    session.connectTo(fixture.hostConfig(), &error),
                    QSocSshSession::ConnectStatus::Ok);
                QVERIFY(!sftp.isOpen());
                if (closeFirst)
                    sftp.close();
                error.clear();
                QCOMPARE(
                    sftp.readFile(QStringLiteral("sample.bin"), expected.size(), &error), expected);
                QVERIFY2(error.isEmpty(), qPrintable(error));
                QVERIFY(!sftp.listDir(fixture.canonicalDir(), 0, &error).isEmpty());
                QVERIFY2(error.isEmpty(), qPrintable(error));
            }
            QCOMPARE(fixture.execRequests(), 1);
            QVERIFY(fixture.stop());
        }
    }
};
} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocsftppeer.moc"
