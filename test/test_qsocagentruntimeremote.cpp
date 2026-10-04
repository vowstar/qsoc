// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/protocol/qsocagentruntimeevent.h"
#include "agent/runtime/qsocagentruntime.h"
#include "qsoc_test.h"
#include "qsoc_test_sshd.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QtTest>

namespace {

/* HOME and the config roots are read when the runtime is built, so they are
 * redirected before QCoreApplication exists. */
struct EnvBootstrap
{
    EnvBootstrap()
        : root(QDir(QDir::tempPath())
                   .filePath(
                       QStringLiteral("test_qsocagentruntimeremote_")
                       + QString::number(QCoreApplication::applicationPid())))
    {
        const QString qsocHome = root + QStringLiteral("/config/qsoc");
        QDir().mkpath(qsocHome);
        QDir().mkpath(root + QStringLiteral("/runtime"));
        qputenv("QSOC_HOME", qsocHome.toUtf8());
        qputenv("XDG_CONFIG_HOME", (root + QStringLiteral("/config")).toUtf8());
        qputenv("XDG_RUNTIME_DIR", (root + QStringLiteral("/runtime")).toUtf8());
        qputenv("HOME", root.toUtf8());
        QFile touch(QDir(qsocHome).filePath(QStringLiteral("qsoc.yml")));
        touch.open(QIODevice::WriteOnly | QIODevice::Truncate);
        touch.close();
    }
    QString root;
};

const EnvBootstrap g_env;

const QString kAlias = QStringLiteral("loopback");

QString outputText(const QSignalSpy &events)
{
    QString text;
    for (const QVariantList &args : events) {
        const auto event = args.first().value<QSocAgentRuntimeEvent>();
        if (event.kind == QSocAgentRuntimeEvent::Kind::Output) {
            text += event.text;
        }
    }
    return text;
}

} // namespace

class Test : public QObject
{
    Q_OBJECT

private:
    QSocTestSshd m_fixture;

    struct Workspace
    {
        QString project;
        QString work;
        QString outside;
    };

    Workspace makeWorkspace(const QString &caseName) const
    {
        const QString   base = m_fixture.root() + QLatin1Char('/') + caseName;
        const Workspace paths{
            base + QStringLiteral("/project"),
            base + QStringLiteral("/work"),
            base + QStringLiteral("/outside")};
        QDir().mkpath(paths.project);
        QDir().mkpath(paths.work);
        QDir().mkpath(paths.outside);
        return paths;
    }

    static QSocAgentRuntimeOptions optionsFor(const Workspace &paths)
    {
        QSocAgentRuntimeOptions options;
        options.projectDirectory = paths.project;
        options.workspace        = paths.work;
        return options;
    }

private slots:
    void initTestCase()
    {
        qRegisterMetaType<QSocAgentRuntimeEvent>("QSocAgentRuntimeEvent");
        if (m_fixture.start()) {
            QVERIFY(m_fixture.writeClientSshConfig(g_env.root, kAlias));
        }
    }

    void shellEscapeRefusesARetargetedCwd()
    {
        QSOC_REQUIRE_SSHD(m_fixture);

        const Workspace paths    = makeWorkspace(QStringLiteral("escape_retargeted_cwd"));
        const QString   inside   = paths.work + QStringLiteral("/inside");
        const QString   selected = paths.work + QStringLiteral("/selected");
        const QString   victim   = paths.outside + QStringLiteral("/victim.txt");
        QVERIFY(QDir().mkpath(inside));
        QVERIFY(QFile::link(inside, selected));

        QSocAgentRuntime runtime(optionsFor(paths));
        QVERIFY(runtime.openSession());
        QString err;
        QVERIFY2(runtime.connectRemote(kAlias, &err), qPrintable(err));
        QVERIFY2(runtime.setWorkingDirectory(QStringLiteral("selected"), &err), qPrintable(err));
        QVERIFY(QFile::remove(selected));
        QVERIFY(QFile::link(paths.outside, selected));

        QSignalSpy events(&runtime, &QSocAgentRuntime::eventRaised);
        QVERIFY(runtime.executeCommand(QStringLiteral("!printf wrong > victim.txt")));
        const QString output = outputText(events);
        QVERIFY2(!QFileInfo::exists(victim), "! ran from a cwd outside the workspace");
        QVERIFY2(output.contains(QStringLiteral("outside the workspace")), qPrintable(output));
        runtime.disconnectRemote();
    }

    void shellEscapeReportsStderrAndExitCode()
    {
        QSOC_REQUIRE_SSHD(m_fixture);

        const Workspace  paths = makeWorkspace(QStringLiteral("escape_status"));
        QSocAgentRuntime runtime(optionsFor(paths));
        QVERIFY(runtime.openSession());
        QString err;
        QVERIFY2(runtime.connectRemote(kAlias, &err), qPrintable(err));

        QSignalSpy events(&runtime, &QSocAgentRuntime::eventRaised);
        QVERIFY(runtime.executeCommand(QStringLiteral("!echo to-stderr >&2; exit 4")));
        const QString output = outputText(events);
        QVERIFY2(output.contains(QStringLiteral("to-stderr")), qPrintable(output));
        QVERIFY2(output.contains(QStringLiteral("(exit code: 4)")), qPrintable(output));
        QVERIFY2(output.contains(QStringLiteral("(shell: ")), qPrintable(output));
        runtime.disconnectRemote();
    }

    void cleanupTestCase()
    {
        m_fixture.stop();
        QDir(g_env.root).removeRecursively();
    }
};

QSOC_TEST_MAIN(Test)
#include "test_qsocagentruntimeremote.moc"
