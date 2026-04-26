// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/mcp/qsocmcptypes.h"
#include "qsoc_test.h"

#include <QtCore>
#include <QtTest>

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        static auto                   argc      = 1;
        static char                   appName[] = "qsoc_test";
        static std::array<char *, 1>  argv      = {{appName}};
        static const QCoreApplication app(argc, argv.data());
    }

    void normalizeKeepsAlphanumeric()
    {
        QCOMPARE(QSocMcp::normalizeName("read_file"), QStringLiteral("read_file"));
        QCOMPARE(QSocMcp::normalizeName("Tool-42"), QStringLiteral("Tool-42"));
    }

    void normalizeReplacesSpaces()
    {
        QCOMPARE(QSocMcp::normalizeName("my server"), QStringLiteral("my_server"));
        QCOMPARE(QSocMcp::normalizeName("a  b"), QStringLiteral("a_b"));
    }

    void normalizeReplacesPunctuation()
    {
        QCOMPARE(QSocMcp::normalizeName("hello!"), QStringLiteral("hello"));
        QCOMPARE(QSocMcp::normalizeName("foo.bar/baz"), QStringLiteral("foo_bar_baz"));
        QCOMPARE(QSocMcp::normalizeName("a:b@c"), QStringLiteral("a_b_c"));
    }

    void normalizeStripsEdgeUnderscores()
    {
        QCOMPARE(QSocMcp::normalizeName("__leading"), QStringLiteral("leading"));
        QCOMPARE(QSocMcp::normalizeName("trailing__"), QStringLiteral("trailing"));
        QCOMPARE(QSocMcp::normalizeName("__both__"), QStringLiteral("both"));
    }

    void normalizeHandlesNonAscii()
    {
        QCOMPARE(QSocMcp::normalizeName(QString::fromUtf8("配置")), QString());
        QCOMPARE(QSocMcp::normalizeName(QString::fromUtf8("read 文件")), QStringLiteral("read"));
    }

    void buildToolNameComposes()
    {
        QCOMPARE(QSocMcp::buildToolName("fs", "read_file"), QStringLiteral("mcp__fs__read_file"));
        QCOMPARE(
            QSocMcp::buildToolName("my server", "Create Issue"),
            QStringLiteral("mcp__my_server__Create_Issue"));
    }

    void buildToolNameAvoidsCollisions()
    {
        const QString a = QSocMcp::buildToolName("alpha", "echo");
        const QString b = QSocMcp::buildToolName("beta", "echo");
        QVERIFY(a != b);
        QCOMPARE(a, QStringLiteral("mcp__alpha__echo"));
        QCOMPARE(b, QStringLiteral("mcp__beta__echo"));
    }
};

QSOC_TEST_MAIN(Test)
#include "test_qsocmcpnaming.moc"
