// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsoclocalpeer.h"
#include "qsoc_test.h"

#include <QLocalSocket>
#include <QProcess>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

namespace {
class Test : public QObject
{
    Q_OBJECT
private slots:
    void disconnectedPeerIsRejected()
    {
        QLocalSocket socket;
        QVERIFY(!QSocLocalPeer::sameUser(socket));
        QCOMPARE(QSocLocalPeer::processId(socket), qint64(-1));
    }

    void peerSecurityContext_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<bool>("accepted");
        QTest::newRow("same-context") << QStringLiteral("same") << true;
#ifdef Q_OS_WIN
        QTest::newRow("same-user-lower-integrity") << QStringLiteral("low") << false;
#endif
    }

    void peerSecurityContext()
    {
        QFETCH(QString, mode);
        QFETCH(bool, accepted);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
#ifdef Q_OS_WIN
        const auto endpoint = QStringLiteral("qsoc-peer-%1").arg(QUuid::createUuid().toString());
#else
        const auto endpoint = directory.filePath("peer.sock");
#endif
        QProcess server;
        server.setWorkingDirectory(directory.path());
        server.start(QStringLiteral(QSOC_PEER_PROBE_PATH), {endpoint, mode});
        QVERIFY(server.waitForStarted(5000));
        QVERIFY(server.waitForReadyRead(5000));
        QCOMPARE(server.readAllStandardOutput().trimmed(), QByteArray("ready"));
        QLocalSocket socket;
        socket.connectToServer(endpoint);
        QVERIFY(socket.waitForConnected(5000));
        QCOMPARE(QSocLocalPeer::processId(socket), server.processId());
        QCOMPARE(QSocLocalPeer::sameUser(socket), accepted);
        QVERIFY(socket.bytesAvailable() > 0 || socket.waitForReadyRead(5000));
        QCOMPARE(socket.readAll(), accepted ? QByteArray("accepted") : QByteArray("rejected"));
        socket.write("done");
        socket.flush();
        QVERIFY(server.waitForFinished(5000));
        QCOMPARE(server.exitStatus(), QProcess::NormalExit);
        QCOMPARE(server.exitCode(), 0);
    }
};
} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsoclocalpeer.moc"
