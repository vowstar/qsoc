// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocproxy.h"
#include "qsoc_test.h"

#include <QNetworkProxy>
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

    void parseEmptyAndSystem()
    {
        QCOMPARE(QSocProxy::parse({}).type(), QNetworkProxy::DefaultProxy);
        QCOMPARE(QSocProxy::parse("system").type(), QNetworkProxy::DefaultProxy);
        QCOMPARE(QSocProxy::parse("DEFAULT").type(), QNetworkProxy::DefaultProxy);
    }

    void parseNone()
    {
        QCOMPARE(QSocProxy::parse("none").type(), QNetworkProxy::NoProxy);
        QCOMPARE(QSocProxy::parse("off").type(), QNetworkProxy::NoProxy);
        QCOMPARE(QSocProxy::parse("Direct").type(), QNetworkProxy::NoProxy);
    }

    void parseHttpUrl()
    {
        const auto proxy = QSocProxy::parse("http://10.0.0.1:8080");
        QCOMPARE(proxy.type(), QNetworkProxy::HttpProxy);
        QCOMPARE(proxy.hostName(), QStringLiteral("10.0.0.1"));
        QCOMPARE(proxy.port(), quint16(8080));
    }

    void parseSocks5UrlWithUserInfo()
    {
        const auto proxy = QSocProxy::parse("socks5://alice:secret@127.0.0.1:1080");
        QCOMPARE(proxy.type(), QNetworkProxy::Socks5Proxy);
        QCOMPARE(proxy.hostName(), QStringLiteral("127.0.0.1"));
        QCOMPARE(proxy.port(), quint16(1080));
        QCOMPARE(proxy.user(), QStringLiteral("alice"));
        QCOMPARE(proxy.password(), QStringLiteral("secret"));
    }

    void parseUnrecognizedFallsBack()
    {
        const auto proxy = QSocProxy::parse("ftp://nope");
        QCOMPARE(proxy.type(), QNetworkProxy::DefaultProxy);
    }

    void resolvePerTargetWinsOverWide()
    {
        const auto wide   = QSocProxy::parse("http://10.0.0.1:8080");
        const auto target = QSocProxy::resolve("none", wide);
        QCOMPARE(target.type(), QNetworkProxy::NoProxy);
    }

    void resolveEmptyTargetUsesWide()
    {
        const auto wide   = QSocProxy::parse("http://10.0.0.1:8080");
        const auto target = QSocProxy::resolve(QString(), wide);
        QCOMPARE(target.type(), QNetworkProxy::HttpProxy);
        QCOMPARE(target.hostName(), QStringLiteral("10.0.0.1"));
    }

    void resolveSystemTargetUsesWide()
    {
        const auto wide = QSocProxy::parse("none");
        QCOMPARE(QSocProxy::resolve("system", wide).type(), QNetworkProxy::NoProxy);
        QCOMPARE(QSocProxy::resolve("default", wide).type(), QNetworkProxy::NoProxy);
    }

    void resolveTargetUrlBeatsWide()
    {
        const auto wide   = QSocProxy::parse("none");
        const auto target = QSocProxy::resolve("http://10.0.0.2:8081", wide);
        QCOMPARE(target.type(), QNetworkProxy::HttpProxy);
        QCOMPARE(target.hostName(), QStringLiteral("10.0.0.2"));
        QCOMPARE(target.port(), quint16(8081));
    }
};

QSOC_TEST_MAIN(Test)
#include "test_qsocproxy.moc"
