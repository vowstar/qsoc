// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/mcp/qsocmcpclient.h"
#include "agent/mcp/qsocmcpmanager.h"
#include "agent/mcp/qsocmcpstdio.h"
#include "agent/mcp/qsocmcptypes.h"
#include "qsoc_test.h"

#include <yaml-cpp/yaml.h>

#include <QtCore>
#include <QtTest>

namespace {

YAML::Node loadYaml(const char *text)
{
    return YAML::Load(text);
}

} // namespace

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

    void parseUndefinedReturnsEmpty()
    {
        const YAML::Node node;
        QCOMPARE(McpServerConfig::parseList(node).size(), qsizetype(0));
    }

    void parseNonSequenceReturnsEmpty()
    {
        const YAML::Node node = loadYaml("not_a_sequence: true");
        QCOMPARE(McpServerConfig::parseList(node).size(), qsizetype(0));
    }

    void parseStdioEntry()
    {
        const YAML::Node node = loadYaml(
            "- name: fs\n"
            "  type: stdio\n"
            "  command: /usr/local/bin/mcp-fs\n"
            "  args: [--root, /tmp]\n"
            "  env:\n"
            "    LOG_LEVEL: info\n");

        const auto list = McpServerConfig::parseList(node);
        QCOMPARE(list.size(), qsizetype(1));

        const auto &cfg = list.at(0);
        QCOMPARE(cfg.name, QStringLiteral("fs"));
        QCOMPARE(cfg.type, QStringLiteral("stdio"));
        QCOMPARE(cfg.command, QStringLiteral("/usr/local/bin/mcp-fs"));
        QCOMPARE(cfg.args, QStringList({"--root", "/tmp"}));
        QCOMPARE(cfg.env.value("LOG_LEVEL"), QStringLiteral("info"));
        QVERIFY(cfg.enabled);
        QVERIFY(cfg.isValid());
    }

    void parseHttpEntry()
    {
        const YAML::Node node = loadYaml(
            "- name: search\n"
            "  type: http\n"
            "  url: http://127.0.0.1:8080/mcp\n"
            "  headers:\n"
            "    Authorization: Bearer placeholder\n"
            "  request_timeout_ms: 5000\n");

        const auto list = McpServerConfig::parseList(node);
        QCOMPARE(list.size(), qsizetype(1));

        const auto &cfg = list.at(0);
        QCOMPARE(cfg.type, QStringLiteral("http"));
        QCOMPARE(cfg.url, QStringLiteral("http://127.0.0.1:8080/mcp"));
        QCOMPARE(cfg.headers.value("Authorization"), QStringLiteral("Bearer placeholder"));
        QCOMPARE(cfg.requestTimeoutMs, 5000);
        QVERIFY(cfg.isValid());
    }

    void typeDefaultsToStdio()
    {
        const YAML::Node node = loadYaml(
            "- name: implicit\n"
            "  command: /bin/echo\n");

        const auto list = McpServerConfig::parseList(node);
        QCOMPARE(list.size(), qsizetype(1));
        QCOMPARE(list.at(0).type, QStringLiteral("stdio"));
    }

    void invalidEntriesAreSkipped()
    {
        const YAML::Node node = loadYaml(
            "- name: missing_command\n"
            "  type: stdio\n"
            "- type: stdio\n"
            "  command: /bin/echo\n"
            "- name: bad_type\n"
            "  type: websocket\n"
            "  url: ws://127.0.0.1\n"
            "- name: ok\n"
            "  type: stdio\n"
            "  command: /bin/echo\n");

        const auto list = McpServerConfig::parseList(node);
        QCOMPARE(list.size(), qsizetype(1));
        QCOMPARE(list.at(0).name, QStringLiteral("ok"));
    }

    void multipleServersPreserveOrder()
    {
        const YAML::Node node = loadYaml(
            "- name: a\n"
            "  command: /bin/true\n"
            "- name: b\n"
            "  command: /bin/false\n"
            "- name: c\n"
            "  type: http\n"
            "  url: http://127.0.0.1:1\n");

        const auto list = McpServerConfig::parseList(node);
        QCOMPARE(list.size(), qsizetype(3));
        QCOMPARE(list.at(0).name, QStringLiteral("a"));
        QCOMPARE(list.at(1).name, QStringLiteral("b"));
        QCOMPARE(list.at(2).name, QStringLiteral("c"));
    }

    void disabledFlagPreserved()
    {
        const YAML::Node node = loadYaml(
            "- name: off\n"
            "  command: /bin/true\n"
            "  enabled: false\n");

        const auto list = McpServerConfig::parseList(node);
        QCOMPARE(list.size(), qsizetype(1));
        QVERIFY(!list.at(0).enabled);
    }

    void managerSkipsDisabledAndInvalid()
    {
        QList<McpServerConfig> configs;

        McpServerConfig good;
        good.name    = "good";
        good.type    = "stdio";
        good.command = "/bin/true";
        configs << good;

        McpServerConfig disabled;
        disabled.name    = "disabled";
        disabled.type    = "stdio";
        disabled.command = "/bin/true";
        disabled.enabled = false;
        configs << disabled;

        McpServerConfig invalid;
        invalid.name = "missing_command";
        invalid.type = "stdio";
        configs << invalid;

        QSocMcpManager manager(configs);
        QCOMPARE(manager.clientCount(), qsizetype(1));
        QVERIFY(manager.findClient("good") != nullptr);
        QVERIFY(manager.findClient("disabled") == nullptr);
        QVERIFY(manager.findClient("missing_command") == nullptr);
    }

    void clientStartsDisconnected()
    {
        McpServerConfig cfg;
        cfg.name    = "x";
        cfg.type    = "stdio";
        cfg.command = "/bin/true";

        auto               *transport = new QSocMcpStdioTransport(cfg);
        const QSocMcpClient client(cfg, transport);
        QCOMPARE(client.name(), QStringLiteral("x"));
        QCOMPARE(client.state(), QSocMcpClient::State::Disconnected);
    }
};

QSOC_TEST_MAIN(Test)
#include "test_qsocmcpconfig.moc"
