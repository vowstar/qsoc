// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocinputsnapshot.h"
#include "agent/remote/qsocsftpclient.h"
#include "agent/remote/qsocsshsession.h"
#include "qsoc_test.h"
#include "qsoc_test_sshd.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>

namespace {

void save(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        qFatal("Cannot write generated input");
}

QByteArray load(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        qFatal("Cannot read generated snapshot");
    return file.readAll();
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void snapshotSurvivesSourceReplacementAndCleansUp()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path  = directory.filePath(QStringLiteral("input.pdf"));
        const auto bytes = QByteArray("source document");
        save(path, bytes);
        QString error;
        auto    snapshot = QSocInputSnapshot::fromLocal(path, bytes.size(), &error);
        QVERIFY2(snapshot, qPrintable(error));
        QCOMPARE(snapshot->source(), path);
        QCOMPARE(snapshot->size(), bytes.size());
        QCOMPARE(
            snapshot->sha256(), QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        const auto temporaryPath = snapshot->path();
        save(path, QByteArray("replacement"));
        QCOMPARE(load(temporaryPath), bytes);
        snapshot.reset();
        QVERIFY(!QFile::exists(temporaryPath));
        QVERIFY(!QFileInfo(temporaryPath).dir().exists());
    }

    void limitsCancellationAndDeadlineRejectIncompleteInput()
    {
        QTemporaryDir directory;
        const auto    path = directory.filePath(QStringLiteral("input.bin"));
        save(path, QByteArray(40000, 'x'));
        QString error;
        QVERIFY(!QSocInputSnapshot::fromLocal(path, 39999, &error));
        QVERIFY(error.contains(QStringLiteral("limit")));
        int checks = 0;
        QVERIFY(!QSocInputSnapshot::fromLocal(path, 40000, &error, [&] { return ++checks >= 3; }));
        QVERIFY(error.contains(QStringLiteral("cancelled")));
        QVERIFY(!QSocInputSnapshot::fromLocal(path, 40000, &error, {}, QDeadlineTimer(0)));
        QVERIFY(error.contains(QStringLiteral("deadline")));
        QVERIFY(!QSocInputSnapshot::fromLocal(directory.path(), 40000, &error));
        QVERIFY(!error.isEmpty());
    }

    void readonlySftpUsesNoShellAndDistinguishesReadEndings()
    {
        QSocTestSshd fixture;
        fixture.setExtraConfig({QStringLiteral("ForceCommand internal-sftp -R")});
        fixture.start();
        QSOC_REQUIRE_SSHD(fixture);
        const auto       path = fixture.workDir() + QStringLiteral("/input.pdf");
        const QByteArray bytes(70000, 'd');
        save(path, bytes);
        QSocSshSession session;
        QString        error;
        QCOMPARE(
            session.connectTo(fixture.hostConfig(fixture.port()), &error),
            QSocSshSession::ConnectStatus::Ok);
        QSocSftpClient sftp(session);
        auto           snapshot = QSocInputSnapshot::fromRemote(&sftp, path, bytes.size(), &error);
        QVERIFY2(snapshot, qPrintable(error));
        QCOMPARE(load(snapshot->path()), bytes);
        QVERIFY(!sftp.writeFile(path, QByteArray("forbidden"), &error));
        QCOMPARE(load(path), bytes);
        const auto stopped
            = sftp.readStreamResult(path, [](const QByteArray &) { return false; }, &error);
        QCOMPARE(stopped.end, QSocSftpClient::ReadEnd::Stopped);
        QVERIFY(stopped.bytes > 0);
        const auto eof
            = sftp.readStreamResult(path, [](const QByteArray &) { return true; }, &error);
        QCOMPARE(eof.end, QSocSftpClient::ReadEnd::Eof);
        QCOMPARE(eof.bytes, bytes.size());
        bool       cancel    = false;
        const auto cancelled = sftp.readStreamResult(
            path,
            [&](const QByteArray &) {
                cancel = true;
                return true;
            },
            &error,
            [&] { return cancel; });
        QCOMPARE(cancelled.end, QSocSftpClient::ReadEnd::Cancelled);
        QVERIFY(error.contains(QStringLiteral("cancelled")));
        bool legacyStop = false;
        session.setAbortProbe([&] { return legacyStop; });
        const auto resetProbe = qScopeGuard([&] { session.setAbortProbe({}); });
        QVERIFY(!sftp.readStream(
            path,
            [&](const QByteArray &) {
                legacyStop = true;
                return true;
            },
            &error));
        QVERIFY(error.contains(QStringLiteral("Interrupted")));
        legacyStop = false;
        QVERIFY(!QSocInputSnapshot::fromRemote(&sftp, path, bytes.size() - 1, &error));
        QVERIFY(error.contains(QStringLiteral("limit")));
        error.clear();
        QVERIFY2(!sftp.readFile(path, 16, &error).isEmpty(), qPrintable(error));
        QCOMPARE(sftp.readFile(path, 16).size(), 16);
        bool called = false;
        QVERIFY(!sftp.runWithin(
            QDeadlineTimer(1000),
            [&] {
                called = true;
                return true;
            },
            [] { return true; }));
        QVERIFY(!called);
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocinputsnapshot.moc"
