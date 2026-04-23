// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocsftpclient.h"
#include "agent/remote/qsocsshsession.h"
#include "qsoc_test.h"

#include <QtCore>
#include <QtTest>

namespace {

struct TestApp
{
    static auto &instance()
    {
        static auto                   argc      = 1;
        static char                   appName[] = "qsoc";
        static std::array<char *, 1>  argv      = {{appName}};
        static const QCoreApplication app       = QCoreApplication(argc, argv.data());
        return app;
    }
};

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { TestApp::instance(); }

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
        QCOMPARE(sftp.exists(QStringLiteral("/tmp"), &err), false);
        QVERIFY(!err.isEmpty());
    }
};

} // namespace

QTEST_GUILESS_MAIN(Test)
#include "test_qsocsftpclient.moc"
