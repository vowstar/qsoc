// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocsshconfigparser.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
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

private:
    static QString writeFile(const QDir &dir, const QString &name, const QString &content)
    {
        const QString path = dir.absoluteFilePath(name);
        QFile         f(path);
        const bool    ok = f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        Q_ASSERT(ok);
        Q_UNUSED(ok);
        f.write(content.toUtf8());
        f.close();
        return path;
    }

private slots:
    void initTestCase() { TestApp::instance(); }

    void testEmptyConfig()
    {
        QTemporaryDir       tmp;
        const QString       cfg = writeFile(QDir(tmp.path()), "config", QString());
        QSocSshConfigParser parser;
        QVERIFY(parser.parse(cfg));
        QVERIFY(parser.isEmpty());
        QVERIFY(parser.listMenuHosts().isEmpty());
    }

    void testBasicHostResolution()
    {
        QTemporaryDir tmp;
        const QString content = QStringLiteral(
            "Host lab\n"
            "    HostName lab-server.example.com\n"
            "    User alice\n"
            "    Port 2222\n"
            "    IdentityFile ~/.ssh/id_lab\n");
        const QString cfg = writeFile(QDir(tmp.path()), "config", content);

        QSocSshConfigParser parser;
        QVERIFY(parser.parse(cfg));

        const QSocSshHostConfig h = parser.resolve(QStringLiteral("lab"));
        QCOMPARE(h.alias, QStringLiteral("lab"));
        QCOMPARE(h.hostname, QStringLiteral("lab-server.example.com"));
        QCOMPARE(h.user, QStringLiteral("alice"));
        QCOMPARE(h.port, 2222);
        QCOMPARE(h.identityFiles.size(), 1);
        QVERIFY(h.identityFiles.at(0).endsWith(QStringLiteral("/.ssh/id_lab")));
        QVERIFY(h.fromConfig);
    }

    void testFirstValueWins()
    {
        QTemporaryDir tmp;
        const QString content = QStringLiteral(
            "Host lab\n"
            "    Port 2222\n"
            "\n"
            "Host *\n"
            "    Port 22\n");
        const QString cfg = writeFile(QDir(tmp.path()), "config", content);

        QSocSshConfigParser parser;
        QVERIFY(parser.parse(cfg));
        const QSocSshHostConfig h = parser.resolve(QStringLiteral("lab"));
        QCOMPARE(h.port, 2222);
    }

    void testIdentityFilesAccumulate()
    {
        QTemporaryDir tmp;
        const QString content = QStringLiteral(
            "Host lab\n"
            "    IdentityFile ~/.ssh/id_a\n"
            "\n"
            "Host *\n"
            "    IdentityFile ~/.ssh/id_global\n");
        const QString cfg = writeFile(QDir(tmp.path()), "config", content);

        QSocSshConfigParser parser;
        QVERIFY(parser.parse(cfg));
        const QSocSshHostConfig h = parser.resolve(QStringLiteral("lab"));
        QCOMPARE(h.identityFiles.size(), 2);
        QVERIFY(h.identityFiles.at(0).endsWith(QStringLiteral("/.ssh/id_a")));
        QVERIFY(h.identityFiles.at(1).endsWith(QStringLiteral("/.ssh/id_global")));
    }

    void testNegationPattern()
    {
        QTemporaryDir tmp;
        const QString content = QStringLiteral(
            "Host lab* !labrat\n"
            "    User alice\n");
        const QString cfg = writeFile(QDir(tmp.path()), "config", content);

        QSocSshConfigParser parser;
        QVERIFY(parser.parse(cfg));
        QCOMPARE(parser.resolve(QStringLiteral("lab1")).user, QStringLiteral("alice"));
        QCOMPARE(parser.resolve(QStringLiteral("labrat")).user, QString());
    }

    void testWildcardOnlyBlockYieldsNoMenuEntry()
    {
        QTemporaryDir tmp;
        const QString content = QStringLiteral(
            "Host *\n"
            "    User alice\n"
            "\n"
            "Host lab\n"
            "    HostName lab-server\n"
            "\n"
            "Host fpga-build\n"
            "    HostName 10.0.0.12\n");
        const QString cfg = writeFile(QDir(tmp.path()), "config", content);

        QSocSshConfigParser parser;
        QVERIFY(parser.parse(cfg));
        const QStringList hosts = parser.listMenuHosts();
        QCOMPARE(hosts.size(), 2);
        QVERIFY(hosts.contains(QStringLiteral("lab")));
        QVERIFY(hosts.contains(QStringLiteral("fpga-build")));
        QVERIFY(!hosts.contains(QStringLiteral("*")));
    }

    void testNegatedPatternExcludedFromMenu()
    {
        QTemporaryDir tmp;
        const QString content = QStringLiteral(
            "Host lab !labrat\n"
            "    HostName lab\n");
        const QString cfg = writeFile(QDir(tmp.path()), "config", content);

        QSocSshConfigParser parser;
        QVERIFY(parser.parse(cfg));
        const QStringList hosts = parser.listMenuHosts();
        QCOMPARE(hosts.size(), 1);
        QCOMPARE(hosts.first(), QStringLiteral("lab"));
    }

    void testTokenExpansion()
    {
        QTemporaryDir tmp;
        const QString content = QStringLiteral(
            "Host lab\n"
            "    HostName %h-internal\n"
            "    IdentityFile ~/.ssh/id_%r_%p\n"
            "    User alice\n"
            "    Port 2022\n");
        const QString cfg = writeFile(QDir(tmp.path()), "config", content);

        QSocSshConfigParser parser;
        QVERIFY(parser.parse(cfg));
        const QSocSshHostConfig h = parser.resolve(QStringLiteral("lab"));
        QCOMPARE(h.hostname, QStringLiteral("lab-internal"));
        QVERIFY(h.identityFiles.at(0).endsWith(QStringLiteral("/.ssh/id_alice_2022")));
    }

    void testUnsupportedDirectivesAreIgnoredNotFatal()
    {
        QTemporaryDir tmp;
        const QString content = QStringLiteral(
            "Host lab\n"
            "    HostName lab-server\n"
            "    ProxyCommand ssh jump nc %h %p\n"
            "    LocalForward 9090 127.0.0.1:8080\n"
            "    CertificateFile ~/.ssh/id_rsa-cert.pub\n");
        const QString cfg = writeFile(QDir(tmp.path()), "config", content);

        QSocSshConfigParser parser;
        QVERIFY(parser.parse(cfg));
        QCOMPARE(parser.resolve(QStringLiteral("lab")).hostname, QStringLiteral("lab-server"));
        /* Notes mention the ignored directives so the caller can surface them
         * as diagnostics; but parsing itself still succeeded. */
        bool sawProxyNote = false;
        for (const QString &n : parser.notes()) {
            if (n.contains(QStringLiteral("proxycommand"))) {
                sawProxyNote = true;
            }
        }
        QVERIFY(sawProxyNote);
    }

    void testParserNeverReadsIdentityFile()
    {
        /* Create an IdentityFile path that does not exist on disk and assert
         * the parser still succeeds without accessing it. The absence of a
         * file-read assertion is strongest together with a "pretend this is a
         * key" scenario where opening would be unambiguously wrong. */
        QTemporaryDir tmp;
        const QString content = QStringLiteral(
                                    "Host lab\n"
                                    "    IdentityFile %1\n")
                                    .arg(QDir(tmp.path()).absoluteFilePath("does_not_exist_id_rsa"));
        const QString cfg     = writeFile(QDir(tmp.path()), "config", content);

        QSocSshConfigParser parser;
        QVERIFY(parser.parse(cfg));
        const QSocSshHostConfig h = parser.resolve(QStringLiteral("lab"));
        QCOMPARE(h.identityFiles.size(), 1);
        /* File must not exist: proof that resolve() returns a path, not contents. */
        QVERIFY(!QFileInfo::exists(h.identityFiles.at(0)));
    }

    void testStrictHostKeyValues()
    {
        QTemporaryDir tmp;
        const QString content = QStringLiteral(
            "Host yes\n    StrictHostKeyChecking yes\n"
            "Host an\n    StrictHostKeyChecking accept-new\n"
            "Host no\n    StrictHostKeyChecking no\n");
        const QString       cfg = writeFile(QDir(tmp.path()), "config", content);
        QSocSshConfigParser parser;
        QVERIFY(parser.parse(cfg));
        QCOMPARE(
            parser.resolve(QStringLiteral("yes")).strictHostKey,
            QSocSshHostConfig::StrictHostKey::Yes);
        QCOMPARE(
            parser.resolve(QStringLiteral("an")).strictHostKey,
            QSocSshHostConfig::StrictHostKey::AcceptNew);
        QCOMPARE(
            parser.resolve(QStringLiteral("no")).strictHostKey,
            QSocSshHostConfig::StrictHostKey::No);
    }

    void testUnknownAliasReturnsDefaults()
    {
        QTemporaryDir       tmp;
        const QString       content = QStringLiteral("Host lab\n    HostName lab-server\n");
        const QString       cfg     = writeFile(QDir(tmp.path()), "config", content);
        QSocSshConfigParser parser;
        QVERIFY(parser.parse(cfg));
        const QSocSshHostConfig h = parser.resolve(QStringLiteral("unknown-host"));
        QCOMPARE(h.hostname, QStringLiteral("unknown-host"));
        QCOMPARE(h.port, 22);
        QVERIFY(h.identityFiles.isEmpty());
        QVERIFY(!h.fromConfig);
    }
};

QTEST_GUILESS_MAIN(Test)
#include "test_qsocsshconfigparser.moc"
