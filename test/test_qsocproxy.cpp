// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/mcp/qsocmcphttp.h"
#include "agent/tool/qsoctoolweb.h"
#include "common/qsocconfig.h"
#include "common/qsocproxy.h"
#include "qsoc_test.h"

#include <memory>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QScopeGuard>
#include <QTcpServer>
#include <QtCore>
#include <QtTest>

namespace {

class DirectFactory : public QNetworkProxyFactory
{
public:
    explicit DirectFactory(int &queries)
        : queries_(queries)
    {}
    QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery &) override
    {
        ++queries_;
        return {QNetworkProxy(QNetworkProxy::NoProxy)};
    }

private:
    int &queries_;
};

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY(directory_.isValid());
        oldHome_ = qgetenv("QSOC_HOME");
        oldXdg_  = qgetenv("XDG_CONFIG_HOME");
        qputenv("QSOC_HOME", directory_.path().toUtf8());
        qputenv("XDG_CONFIG_HOME", directory_.path().toUtf8());
    }

    void cleanupTestCase()
    {
        if (oldHome_.isNull())
            qunsetenv("QSOC_HOME");
        else
            qputenv("QSOC_HOME", oldHome_);
        if (oldXdg_.isNull())
            qunsetenv("XDG_CONFIG_HOME");
        else
            qputenv("XDG_CONFIG_HOME", oldXdg_);
        QVERIFY(directory_.remove());
    }

    void parseEmptyAndSystem()
    {
        QCOMPARE(QSocProxy::parse({}).proxy.type(), QNetworkProxy::DefaultProxy);
        QCOMPARE(QSocProxy::parse("system").proxy.type(), QNetworkProxy::DefaultProxy);
        QCOMPARE(QSocProxy::parse("DEFAULT").proxy.type(), QNetworkProxy::DefaultProxy);
    }

    void parseNone()
    {
        QCOMPARE(QSocProxy::parse("none").proxy.type(), QNetworkProxy::NoProxy);
        QCOMPARE(QSocProxy::parse("off").proxy.type(), QNetworkProxy::NoProxy);
        QCOMPARE(QSocProxy::parse("Direct").proxy.type(), QNetworkProxy::NoProxy);
    }

    void parseHttpUrl()
    {
        const auto proxy = QSocProxy::parse("http://127.0.0.1:8080");
        QCOMPARE(proxy.proxy.type(), QNetworkProxy::HttpProxy);
        QCOMPARE(proxy.proxy.hostName(), QStringLiteral("127.0.0.1"));
        QCOMPARE(proxy.proxy.port(), quint16(8080));
    }

    void parseSocks5UrlWithUserInfo()
    {
        const QString user     = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString password = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto    proxy    = QSocProxy::parse(
            QStringLiteral("socks5://%1:%2@127.0.0.1:1080").arg(user, password));
        QVERIFY(proxy.error.isEmpty());
        QCOMPARE(proxy.proxy.type(), QNetworkProxy::Socks5Proxy);
        QCOMPARE(proxy.proxy.hostName(), QStringLiteral("127.0.0.1"));
        QCOMPARE(proxy.proxy.port(), quint16(1080));
        QCOMPARE(proxy.proxy.user(), user);
        QCOMPARE(proxy.proxy.password(), password);
    }

    void invalidSpecs_data()
    {
        QTest::addColumn<QString>("spec");
        for (const auto *spec :
             {"http://",
              "http://:8080",
              "http://127.0.0.1:0",
              "http://127.0.0.1:-1",
              "http://127.0.0.1:65536",
              "http://127.0.0.1:abc",
              "http://127.0.0.1:",
              "http://127.0.0.1/path",
              "http://127.0.0.1?query",
              "http://127.0.0.1#fragment",
              "http://bad host:80",
              "socks5://",
              "socks5://127.0.0.1:65536",
              "https://127.0.0.1:8080",
              "ftp://127.0.0.1",
              "127.0.0.1:8080",
              "sytem"})
            QTest::newRow(spec) << QString::fromLatin1(spec);
    }

    void invalidSpecs()
    {
        QFETCH(QString, spec);
        const auto result = QSocProxy::parse(spec);
        QVERIFY(result.error.startsWith("Invalid proxy configuration:"));
        const auto direct = QSocProxy::parse("none");
        QCOMPARE(QSocProxy::resolve(spec, direct), result);
        QCOMPARE(QSocProxy::resolve({}, result), result);
        QCOMPARE(QSocProxy::resolve("system", result), result);
        QCOMPARE(QSocProxy::resolve("none", result), result);
        QCOMPARE(QSocProxy::resolve("http://127.0.0.1:8080", result), result);
    }

    void errorsDoNotExposeCredentials()
    {
        const QString credential = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto result = QSocProxy::parse(QStringLiteral("http://%1:%1@/bad").arg(credential));
        QVERIFY(!result.error.isEmpty());
        QVERIFY(!result.error.contains(credential));
    }

    void legacyInvalid_data()
    {
        QTest::addColumn<QString>("key");
        QTest::addColumn<QString>("value");
        QTest::newRow("type") << QString("proxy.type") << QString("invalid");
        QTest::newRow("empty-host") << QString("proxy.host") << QString();
        QTest::newRow("host-url") << QString("proxy.host") << QString("http://127.0.0.1");
        QTest::newRow("unknown-field") << QString("proxy.hostname") << QString("127.0.0.1");
        QTest::newRow("scalar") << QString("proxy") << QString("http://127.0.0.1");
        for (const auto *port : {"", "0", "-1", "65536", "abc"})
            QTest::newRow(qPrintable(QString("port-%1").arg(port)))
                << QString("proxy.port") << QString::fromLatin1(port);
    }

    void legacyInvalid()
    {
        QFETCH(QString, key);
        QFETCH(QString, value);
        QSocConfig config;
        config.setValue("proxy.type", "http");
        config.setValue(key, value);
        QVERIFY(!QSocProxy::fromLegacyConfig(&config).error.isEmpty());
    }

    void invalidYamlFields_data()
    {
        QTest::addColumn<QByteArray>("yaml");
        QTest::newRow("sequence") << QByteArray("proxy: []\n");
        QTest::newRow("type-sequence") << QByteArray("proxy: {type: []}\n");
        QTest::newRow("port-null") << QByteArray("proxy: {type: http, port: null}\n");
        QTest::newRow("host-map") << QByteArray("proxy: {type: http, host: {}}\n");
        QTest::newRow("invalid-yaml") << QByteArray("proxy: [\n");
        QTest::newRow("invalid-root") << QByteArray("[proxy]\n");
    }

    void invalidYamlFields()
    {
        QFETCH(QByteArray, yaml);
        QFile file(directory_.filePath("qsoc.yml"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(yaml), yaml.size());
        file.close();
        QSocConfig config;
        QVERIFY(!QSocProxy::fromLegacyConfig(&config).error.isEmpty());
        QVERIFY(file.remove());
        config.loadConfig();
        QVERIFY(!config.hasLoadErrors());
    }

    void invalidConfigurationOpensNoConnection()
    {
        QTcpServer origin;
        QVERIFY(origin.listen(QHostAddress::LocalHost));
        QSignalSpy                      connections(&origin, &QTcpServer::newConnection);
        int                             queries = 0;
        QSocProxy::NetworkAccessManager manager;
        manager.setProxyFactory(new DirectFactory(queries));
        QSocProxy::apply(&manager, QSocProxy::parse("http://"));
        const QNetworkRequest request(
            QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(origin.serverPort())));
        for (bool post : {false, true}) {
            auto      *reply = post ? manager.post(request, QByteArray()) : manager.get(request);
            QSignalSpy finished(reply, &QNetworkReply::finished);
            QTRY_COMPARE(finished.size(), 1);
            QVERIFY(reply->errorString().startsWith("Invalid proxy configuration:"));
            QCOMPARE(reply->error(), QNetworkReply::ProxyNotFoundError);
            QVERIFY(reply->isFinished());
            reply->abort();
            QCOMPARE(finished.size(), 1);
            delete reply;
        }
        QCOMPARE(queries, 0);
        QCOMPARE(connections.size(), 0);
    }

    void errorCallbackCanDestroyManager()
    {
        auto manager = std::make_unique<QSocProxy::NetworkAccessManager>();
        QSocProxy::apply(manager.get(), QSocProxy::parse("http://"));
        QPointer<QNetworkReply> reply = manager->get(QNetworkRequest(QUrl("http://127.0.0.1/")));
        connect(reply, &QNetworkReply::errorOccurred, this, [&manager] { manager.reset(); });
        QTRY_VERIFY(!manager);
        QVERIFY(reply.isNull());
    }

    void invalidGlobalBlocksWebTools()
    {
        QTcpServer origin;
        QVERIFY(origin.listen(QHostAddress::LocalHost));
        QSignalSpy    connections(&origin, &QTcpServer::newConnection);
        const QString url = QStringLiteral("http://127.0.0.1:%1").arg(origin.serverPort());
        QSocConfig    config;
        config.setValue("proxy.type", "invalid");
        config.setValue("web.search_api_url", url);
        QSocToolWebSearch search(nullptr, &config);
        QSocToolWebFetch  fetch(nullptr, &config);
        QVERIFY(search.execute({{"query", "probe"}}).contains("Invalid proxy configuration:"));
        QVERIFY(
            fetch.execute({{"url", url.toStdString()}}).contains("Invalid proxy configuration:"));
        QCOMPARE(connections.size(), 0);
    }

    void invalidProxyBlocksMcp_data()
    {
        QTest::addColumn<bool>("globalInvalid");
        QTest::newRow("global") << true;
        QTest::newRow("server") << false;
    }

    void invalidProxyBlocksMcp()
    {
        QFETCH(bool, globalInvalid);
        QTcpServer origin;
        QVERIFY(origin.listen(QHostAddress::LocalHost));
        QSignalSpy connections(&origin, &QTcpServer::newConnection);
        const auto previous = QSocProxy::qsocWideDefault();
        const auto restore  = qScopeGuard([&] { QSocProxy::setQsocWideDefault(previous); });
        QSocProxy::setQsocWideDefault(QSocProxy::parse(globalInvalid ? "http://" : "none"));
        McpServerConfig config;
        config.url   = QStringLiteral("http://127.0.0.1:%1/mcp").arg(origin.serverPort());
        config.proxy = globalInvalid ? QString() : QStringLiteral("http://");
        QSocMcpHttpTransport transport(config);
        QSignalSpy           failed(&transport, &QSocMcpTransport::messageFailed);
        transport.start();
        transport.sendTrackedMessage({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}}, 1);
        QTRY_COMPARE(failed.size(), 1);
        QVERIFY(failed.first().last().toString().contains("Invalid proxy configuration:"));
        QCOMPARE(connections.size(), 0);
        transport.stop();
    }

    void resolvePerTargetWinsOverWide()
    {
        const auto wide   = QSocProxy::parse("http://127.0.0.1:8080");
        const auto target = QSocProxy::resolve("none", wide);
        QCOMPARE(target.proxy.type(), QNetworkProxy::NoProxy);
    }

    void resolveEmptyTargetUsesWide()
    {
        const auto wide   = QSocProxy::parse("http://127.0.0.1:8080");
        const auto target = QSocProxy::resolve(QString(), wide);
        QCOMPARE(target.proxy.type(), QNetworkProxy::HttpProxy);
        QCOMPARE(target.proxy.hostName(), QStringLiteral("127.0.0.1"));
    }

    void resolveSystemTargetUsesWide()
    {
        const auto wide = QSocProxy::parse("none");
        QCOMPARE(QSocProxy::resolve("system", wide).proxy.type(), QNetworkProxy::NoProxy);
        QCOMPARE(QSocProxy::resolve("default", wide).proxy.type(), QNetworkProxy::NoProxy);
    }

    void resolveTargetUrlBeatsWide()
    {
        const auto wide   = QSocProxy::parse("none");
        const auto target = QSocProxy::resolve("http://127.0.0.2:8081", wide);
        QCOMPARE(target.proxy.type(), QNetworkProxy::HttpProxy);
        QCOMPARE(target.proxy.hostName(), QStringLiteral("127.0.0.2"));
        QCOMPARE(target.proxy.port(), quint16(8081));
    }

private:
    QTemporaryDir directory_{QDir::tempPath() + "/test_qsoc_proxy_config_XXXXXX"};
    QByteArray    oldHome_;
    QByteArray    oldXdg_;
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocproxy.moc"
