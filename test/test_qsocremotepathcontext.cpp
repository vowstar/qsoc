// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocremotepathcontext.h"
#include "qsoc_test.h"

#include <QtCore>
#include <QtTest>

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

    void testConstructorNormalizesInputs()
    {
        QSocRemotePathContext
            ctx(QStringLiteral("/home/alice/work//chip/"),
                QStringLiteral("/home/alice/work/chip/src/./sub"),
                {QStringLiteral("/home/alice/work/chip"), QStringLiteral("/tmp/")});
        QCOMPARE(ctx.root(), QStringLiteral("/home/alice/work/chip"));
        QCOMPARE(ctx.cwd(), QStringLiteral("/home/alice/work/chip/src/sub"));
        QCOMPARE(
            ctx.writableDirs(),
            (QStringList{QStringLiteral("/home/alice/work/chip"), QStringLiteral("/tmp")}));
    }

    void testNormalizeDotAndDotDot()
    {
        QSocRemotePathContext ctx(QStringLiteral("/r"), QStringLiteral("/r/a/b"), {});
        QCOMPARE(ctx.normalize(QStringLiteral(".")), QStringLiteral("/r/a/b"));
        QCOMPARE(ctx.normalize(QStringLiteral("./x")), QStringLiteral("/r/a/b/x"));
        QCOMPARE(ctx.normalize(QStringLiteral("../c")), QStringLiteral("/r/a/c"));
        QCOMPARE(ctx.normalize(QStringLiteral("../../x")), QStringLiteral("/r/x"));
        QCOMPARE(ctx.normalize(QStringLiteral("../../../x")), QStringLiteral("/x"));
    }

    void testNormalizeAbsoluteClampsAboveRoot()
    {
        QSocRemotePathContext ctx(QStringLiteral("/r"), QStringLiteral("/r"), {});
        QCOMPARE(ctx.normalize(QStringLiteral("/a/../..")), QStringLiteral("/"));
        QCOMPARE(ctx.normalize(QStringLiteral("/a/b/..")), QStringLiteral("/a"));
        QCOMPARE(ctx.normalize(QStringLiteral("/home//alice///x")), QStringLiteral("/home/alice/x"));
    }

    void testEmptyPathResolvesToCwd()
    {
        QSocRemotePathContext ctx(QStringLiteral("/r"), QStringLiteral("/r/sub"), {});
        QCOMPARE(ctx.normalize(QString()), QStringLiteral("/r/sub"));
    }

    void testWritableExactAndUnder()
    {
        QSocRemotePathContext
            ctx(QStringLiteral("/r"),
                QStringLiteral("/r"),
                {QStringLiteral("/r"), QStringLiteral("/tmp")});

        QVERIFY(ctx.isWritable(QStringLiteral("/r")));
        QVERIFY(ctx.isWritable(QStringLiteral("/r/a")));
        QVERIFY(ctx.isWritable(QStringLiteral("/r/a/b/c")));
        QVERIFY(ctx.isWritable(QStringLiteral("/tmp/x")));
    }

    void testWritableRejectsPrefixImpostor()
    {
        /* /r/a writable must not allow /r/abc or /r/a-twin. */
        QSocRemotePathContext
            ctx(QStringLiteral("/r"), QStringLiteral("/r"), {QStringLiteral("/r/a")});
        QVERIFY(ctx.isWritable(QStringLiteral("/r/a")));
        QVERIFY(ctx.isWritable(QStringLiteral("/r/a/sub")));
        QVERIFY(!ctx.isWritable(QStringLiteral("/r/abc")));
        QVERIFY(!ctx.isWritable(QStringLiteral("/r/a-twin/x")));
    }

    void testWritableRejectsRelativeAndEmpty()
    {
        QSocRemotePathContext ctx(QStringLiteral("/r"), QStringLiteral("/r"), {QStringLiteral("/r")});
        QVERIFY(!ctx.isWritable(QString()));
        QVERIFY(!ctx.isWritable(QStringLiteral("r/a")));
    }

    void testResolveCwdRequestClampsAboveRoot()
    {
        QSocRemotePathContext
            ctx(QStringLiteral("/home/alice/work"), QStringLiteral("/home/alice/work"), {});
        QCOMPARE(ctx.resolveCwdRequest(QStringLiteral("sub")), QStringLiteral("/home/alice/work/sub"));
        QCOMPARE(
            ctx.resolveCwdRequest(QStringLiteral("../../etc")), QStringLiteral("/home/alice/work"));
        QCOMPARE(
            ctx.resolveCwdRequest(QStringLiteral("/other/dir")), QStringLiteral("/home/alice/work"));
        QCOMPARE(
            ctx.resolveCwdRequest(QStringLiteral("/home/alice/work/deep")),
            QStringLiteral("/home/alice/work/deep"));
    }

    void testSettersNormalize()
    {
        QSocRemotePathContext ctx;
        ctx.setRoot(QStringLiteral("/r//"));
        ctx.setCwd(QStringLiteral("/r/./sub/"));
        ctx.setWritableDirs({QStringLiteral("/tmp/"), QStringLiteral("/r/w")});
        QCOMPARE(ctx.root(), QStringLiteral("/r"));
        QCOMPARE(ctx.cwd(), QStringLiteral("/r/sub"));
        QCOMPARE(ctx.writableDirs(), (QStringList{QStringLiteral("/tmp"), QStringLiteral("/r/w")}));
    }
};

QSOC_TEST_MAIN(Test)
#include "test_qsocremotepathcontext.moc"
