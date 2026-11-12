// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocsftpclient.h"
#include "agent/remote/qsocsshsession.h"
#include "qsoc_test.h"
#include "qsoc_test_relay.h"
#ifdef Q_OS_WIN
#include "qsoc_test_sftp_peer.h"
#else
#include "qsoc_test_sshd.h"
#endif

#include <limits>
#include <memory>
#include <vector>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QtTest>

#ifdef Q_OS_WIN
#include <qt_windows.h>

#include <io.h>
#include <winioctl.h>
#endif

namespace {

QByteArray data()
{
    QByteArray bytes(90000, '\0');
    quint32    value = 17;
    for (char &byte : bytes) {
        value = value * 1664525U + 1013904223U;
        byte  = static_cast<char>(value >> 24);
    }
    return bytes;
}

void save(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        qFatal("Cannot save generated source");
}

class Test : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
#ifdef Q_OS_WIN
        if (!fixture_.start(true) && fixture_.missing())
            QSOC_TEST_MISSING_DEPENDENCY(fixture_.error());
        QVERIFY2(fixture_.error().isEmpty(), qPrintable(fixture_.error()));
#else
        fixture_.setExtraConfig({QStringLiteral("ForceCommand internal-sftp -R")});
        fixture_.start();
        QSOC_REQUIRE_SSHD(fixture_);
#endif
        QString error;
        QCOMPARE(session_.connectTo(hostConfig(), &error), QSocSshSession::ConnectStatus::Ok);
        client_ = std::make_unique<QSocSftpClient>(session_);
    }

    void cleanupTestCase()
    {
        client_.reset();
        session_.disconnectFromHost();
        const bool released = session_.lastTeardownReleasedState();
#ifdef Q_OS_WIN
        const bool stopped = fixture_.stop();
        QDir       tempRoot(QFileInfo(fixture_.workDir()).absolutePath());
        QVERIFY(!tempRoot.isRoot());
        const bool removed = tempRoot.removeRecursively();
        QVERIFY(stopped);
        QVERIFY(removed);
#else
        fixture_.stop();
        QVERIFY(fixture_.removeRoot());
#endif
        QVERIFY(released);
    }

    void rangesUseThePinnedHandleAndLosslessOffsets()
    {
        const auto path  = fixture_.workDir() + QStringLiteral("/ranges.fst");
        const auto bytes = data();
        save(path, bytes);
        QString error;
        auto    source = client_->openReadFile(remotePath(path), &error);
        QVERIFY2(source, qPrintable(error));
        QCOMPARE(source->size(), quint64(bytes.size()));
        QVERIFY(source->hasModificationTime());
        auto result = source->readAt(123, 2048, &error);
        QCOMPARE(result.end, QSocSftpClient::ReadEnd::Stopped);
        QCOMPARE(result.data, bytes.mid(123, 2048));
        for (quint64 offset :
             {quint64(2171),
              quint64(6267),
              quint64(17),
              quint64(4113),
              quint64(50000),
              quint64(54096),
              quint64(123),
              quint64(4219)}) {
            result = source->readAt(offset, 4096, &error);
            QCOMPARE(result.end, QSocSftpClient::ReadEnd::Stopped);
            QCOMPARE(result.data, bytes.mid(static_cast<qsizetype>(offset), 4096));
            QVERIFY2(error.isEmpty(), qPrintable(error));
            QVERIFY(source->isOpen());
        }
        result = source->readAt(bytes.size() - 17, 100, &error);
        QCOMPARE(result.end, QSocSftpClient::ReadEnd::Eof);
        QCOMPARE(result.data, bytes.right(17));
        result = source->readAt(std::numeric_limits<quint64>::max(), 16, &error);
        QCOMPARE(result.end, QSocSftpClient::ReadEnd::Eof);
        QVERIFY(result.data.isEmpty());
        result = source->readAt(0, 1024 * 1024 + 1, &error);
        QCOMPARE(result.end, QSocSftpClient::ReadEnd::Error);
        QVERIFY(source->isOpen());
#ifndef Q_OS_WIN
        QVERIFY(QFile::rename(path, path + QStringLiteral(".old")));
        save(path, QByteArray(bytes.size(), 'z'));
        result = source->readAt(123, 2048, &error);
        QCOMPARE(result.data, bytes.mid(123, 2048));
        auto replacement = client_->openReadFile(remotePath(path), &error);
        QVERIFY2(replacement, qPrintable(error));
        QCOMPARE(replacement->readAt(123, 2048, &error).data, QByteArray(2048, 'z'));
#endif
    }

    void changedSourcesAndCancellationCloseOnlyTheirHandle()
    {
        const auto path = fixture_.workDir() + QStringLiteral("/changed.fst");
        save(path, data());
        QString error;
        auto    source = client_->openReadFile(remotePath(path), &error);
        QVERIFY2(source, qPrintable(error));
        auto prefetched = client_->openReadFile(remotePath(path), &error);
        QVERIFY2(prefetched, qPrintable(error));
        QCOMPARE(prefetched->readAt(0, 4096, &error).data, data().left(4096));
        save(path, QByteArray(12, 'x'));
        const auto changed = source->readAt(0, 10, &error);
        QCOMPARE(changed.end, QSocSftpClient::ReadEnd::Error);
        QVERIFY(changed.data.isEmpty());
        QVERIFY(error.contains(QStringLiteral("source_changed")));
        QVERIFY(!source->isOpen());
        const auto stale = prefetched->readAt(4096, 10, &error);
        QCOMPARE(stale.end, QSocSftpClient::ReadEnd::Error);
        QVERIFY(stale.data.isEmpty());
        QVERIFY(error.contains(QStringLiteral("source_changed")));
        QVERIFY(!prefetched->isOpen());
        source = client_->openReadFile(remotePath(path), &error);
        QVERIFY2(source, qPrintable(error));
        QCOMPARE(source->readAt(0, 4, &error).data, QByteArray(4, 'x'));
        const auto cancelled = source->readAt(0, 10, &error, [] { return true; });
        QCOMPARE(cancelled.end, QSocSftpClient::ReadEnd::Cancelled);
        QVERIFY(!source->isOpen());
        QCOMPARE(client_->readFile(remotePath(path)), QByteArray(12, 'x'));
        source = client_->openReadFile(remotePath(path), &error);
        QVERIFY(source);
        QCOMPARE(source->readAt(0, 4, &error).data, QByteArray(4, 'x'));
        QSocSftpClient::RangeResult timed;
        client_->runWithin(QDeadlineTimer(1), [&] {
            QTest::qSleep(10);
            timed = source->readAt(0, 10, &error);
            return true;
        });
        QCOMPARE(timed.end, QSocSftpClient::ReadEnd::TimedOut);
        QVERIFY(!source->isOpen());
        QCOMPARE(client_->readFile(remotePath(path)), QByteArray(12, 'x'));
    }

    void handleLimitAndSubsystemCloseInvalidateEverySource()
    {
        const auto path = fixture_.workDir() + QStringLiteral("/limit.fst");
        save(path, data());
        std::vector<std::unique_ptr<QSocSftpClient::ReadFile>> files;
        QString                                                error;
        for (int index = 0; index < 8; ++index) {
            files.push_back(client_->openReadFile(remotePath(path), &error));
            QVERIFY2(files.back(), qPrintable(error));
            QCOMPARE(files.back()->readAt(0, 4096, &error).data, data().left(4096));
        }
        QVERIFY(!client_->openReadFile(remotePath(path), &error));
        QVERIFY(error.contains(QStringLiteral("handle limit")));
        files.pop_back();
        files.push_back(client_->openReadFile(remotePath(path), &error));
        QVERIFY(files.back());
        client_->close();
        for (const auto &file : files) {
            QVERIFY(!file->isOpen());
            const auto result = file->readAt(0, 10, &error);
            QCOMPARE(result.end, QSocSftpClient::ReadEnd::Error);
            QVERIFY(error.contains(QStringLiteral("source_expired")));
        }
        QVERIFY2(client_->open(&error), qPrintable(error));
        QCOMPARE(client_->readFile(remotePath(path), 32), data().left(32));
    }

    void sparseRangesAboveFourGiB()
    {
        const auto path = fixture_.workDir() + QStringLiteral("/sparse.fst");
        QFile      file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
#ifdef Q_OS_WIN
        DWORD returned = 0;
        if (!DeviceIoControl(
                reinterpret_cast<HANDLE>(_get_osfhandle(file.handle())),
                FSCTL_SET_SPARSE,
                nullptr,
                0,
                nullptr,
                0,
                &returned,
                nullptr))
            QSKIP("The fixture filesystem does not support sparse files");
#endif
        const quint64 offset = (quint64(1) << 32) + 12345;
        const auto    tail   = data().left(1024);
        QVERIFY(file.seek(static_cast<qint64>(offset)));
        QCOMPARE(file.write(tail), tail.size());
        file.close();
        QString error;
        auto    source = client_->openReadFile(remotePath(path), &error);
        QVERIFY2(source, qPrintable(error));
        QCOMPARE(source->size(), offset + tail.size());
        QCOMPARE(source->readAt(offset, tail.size(), &error).data, tail);
        QCOMPARE(source->readAt(offset + 917, 107, &error).data, tail.right(107));
    }

    void foreignOperationsDeferRetirement()
    {
        const auto path = fixture_.workDir() + QStringLiteral("/foreign.fst");
        save(path, data());
        QString error;
        auto    source  = client_->openReadFile(remotePath(path), &error);
        auto    sibling = client_->openReadFile(remotePath(path), &error);
        QVERIFY(source && sibling);
        QCOMPARE(source->readAt(0, 4096, &error).data, data().left(4096));
        QCOMPARE(sibling->readAt(0, 4096, &error).data, data().left(4096));
        {
            QSocSshSession::Operation foreign(session_);
            const auto                result = sibling->readAt(0, 16, &error);
            QCOMPARE(result.end, QSocSftpClient::ReadEnd::Error);
            QVERIFY(error.contains(QStringLiteral("busy")));
            QVERIFY(sibling->isOpen());
            QVERIFY(!client_->openReadFile(remotePath(path), &error));
            source.reset();
        }
        QCOMPARE(sibling->readAt(4096, 1024, &error).data, data().mid(4096, 1024));
        QCOMPARE(sibling->readAt(17, 1024, &error).data, data().mid(17, 1024));
        QVERIFY(session_.isConnected());
    }

    void reconnectNeverReusesAnOldHandle()
    {
        const auto path = fixture_.workDir() + QStringLiteral("/reconnect.fst");
        save(path, data());
        QString error;
        auto    source = client_->openReadFile(remotePath(path), &error);
        QVERIFY(source);
        QCOMPARE(source->readAt(0, 4096, &error).data, data().left(4096));
        const auto generation = session_.generation();
        session_.disconnectFromHost();
        QVERIFY(!source->isOpen());
        QVERIFY(session_.generation() != generation);
        QCOMPARE(session_.connectTo(hostConfig(), &error), QSocSshSession::ConnectStatus::Ok);
        QVERIFY(!source->isOpen());
        QCOMPARE(source->readAt(0, 16, &error).end, QSocSftpClient::ReadEnd::Error);
        QVERIFY(!source->isOpen());
        auto replacement = client_->openReadFile(remotePath(path), &error);
        QVERIFY2(replacement, qPrintable(error));
        QCOMPARE(replacement->readAt(0, 16, &error).data, data().left(16));
    }

    void abandonedRangeInvalidatesEverySibling()
    {
        const auto path = fixture_.workDir() + QStringLiteral("/abandoned.fst");
        save(path, data());
        QSocTestRelay relay(static_cast<quint16>(hostConfig().port));
        relay.start();
        QVERIFY(relay.waitUntilListening(5000));
        const auto     relayGuard = qScopeGuard([&relay] { relay.stop(); });
        QSocSshSession session;
        QString        error;
        QCOMPARE(
            session.connectTo(hostConfig(relay.port()), &error), QSocSshSession::ConnectStatus::Ok);
        QSocSftpClient                                         client(session);
        std::vector<std::unique_ptr<QSocSftpClient::ReadFile>> files;
        for (int index = 0; index < 8; ++index) {
            files.push_back(client.openReadFile(remotePath(path), &error));
            QVERIFY2(files.back(), qPrintable(error));
            QCOMPARE(files.back()->readAt(0, 4096, &error).data, data().left(4096));
        }
        client.setOperationTimeoutMs(200);
        client.setDataPhaseObserver([&relay] { relay.blackhole(); });
        QElapsedTimer elapsed;
        elapsed.start();
        const auto result = files.front()->readAt(100, 32000, &error);
        QVERIFY(
            result.end == QSocSftpClient::ReadEnd::TimedOut
            || result.end == QSocSftpClient::ReadEnd::Error);
        QVERIFY(result.data.isEmpty());
        QVERIFY(!error.isEmpty());
        QVERIFY2(elapsed.elapsed() < 3500, qPrintable(QString::number(elapsed.elapsed())));
        for (const auto &source : files)
            QVERIFY(!source->isOpen());
        QVERIFY(!session.isConnected());
        relay.stop();
        session.disconnectFromHost();
        QVERIFY(session.lastTeardownReleasedState());
    }

    void retirementCleanupSharesOneDeadline()
    {
        const auto path = fixture_.workDir() + QStringLiteral("/retired.fst");
        save(path, data());
        QSocTestRelay relay(static_cast<quint16>(hostConfig().port));
        relay.start();
        QVERIFY(relay.waitUntilListening(5000));
        const auto     relayGuard = qScopeGuard([&relay] { relay.stop(); });
        QSocSshSession session;
        QString        error;
        QCOMPARE(
            session.connectTo(hostConfig(relay.port()), &error), QSocSshSession::ConnectStatus::Ok);
        QSocSftpClient                                         client(session);
        std::vector<std::unique_ptr<QSocSftpClient::ReadFile>> files;
        for (int index = 0; index < 8; ++index) {
            files.push_back(client.openReadFile(remotePath(path), &error));
            QVERIFY2(files.back(), qPrintable(error));
            QCOMPARE(files.back()->readAt(0, 4096, &error).data, data().left(4096));
        }
        {
            QSocSshSession::Operation foreign(session);
            files.clear();
        }
        relay.blackhole();
        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY(client.readFile(remotePath(path), 32, &error).isEmpty());
        const auto milliseconds = elapsed.elapsed();
        qInfo("Retired-reader cleanup took %lld ms", static_cast<long long>(milliseconds));
        relay.stop();
        session.disconnectFromHost();
        QVERIFY(session.lastTeardownReleasedState());
        QVERIFY2(milliseconds < 3500, qPrintable(QString::number(milliseconds)));
    }

    void aSourceCanOutliveItsClient()
    {
        const auto path = fixture_.workDir() + QStringLiteral("/lifetime.fst");
        save(path, data());
        std::unique_ptr<QSocSftpClient::ReadFile> source;
        QSocSshSession                            session;
        QString                                   error;
        QCOMPARE(session.connectTo(hostConfig(), &error), QSocSshSession::ConnectStatus::Ok);
        {
            QSocSftpClient client(session);
            source = client.openReadFile(remotePath(path), &error);
            QVERIFY2(source, qPrintable(error));
            QCOMPARE(source->readAt(0, 4096, &error).data, data().left(4096));
        }
        QVERIFY(!source->isOpen());
        QCOMPARE(source->readAt(0, 10, &error).end, QSocSftpClient::ReadEnd::Error);
        QVERIFY(error.contains(QStringLiteral("source_expired")));
        session.disconnectFromHost();
        QVERIFY(session.lastTeardownReleasedState());
    }

private:
    QString remotePath(const QString &localPath) const
    {
#ifdef Q_OS_WIN
        return fixture_.canonicalDir() + localPath.mid(fixture_.workDir().size());
#else
        return localPath;
#endif
    }

    QSocSshHostConfig hostConfig(quint16 port = 0) const
    {
        auto host = fixture_.hostConfig();
        if (port == 0 || port == host.port)
            return host;
        host.port = port;
#ifdef Q_OS_WIN
        QFile publicKey(fixture_.stateDir() + QStringLiteral("/host_key.pub"));
        QFile knownHosts(host.userKnownHostsFile);
        if (!publicKey.open(QIODevice::ReadOnly)
            || !knownHosts.open(QIODevice::WriteOnly | QIODevice::Append))
            qFatal("Cannot pin the relay host key");
        const QByteArray entry = "[127.0.0.1]:" + QByteArray::number(port) + ' '
                                 + publicKey.readAll();
        if (knownHosts.write(entry) != entry.size())
            qFatal("Cannot pin the relay host key");
#endif
        return host;
    }

#ifdef Q_OS_WIN
    QSocTestSftpPeer fixture_;
#else
    QSocTestSshd fixture_;
#endif
    QSocSshSession                  session_;
    std::unique_ptr<QSocSftpClient> client_;
};
} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocsftpreadfile.moc"
