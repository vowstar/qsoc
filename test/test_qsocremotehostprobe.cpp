// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsoctool.h"
#include "agent/remote/qsocagentremote.h"
#include "agent/remote/qsocremotehost.h"
#include "agent/remote/qsocsftpclient.h"
#include "agent/remote/qsocsshexec.h"
#include "agent/remote/qsoctoolremote.h"
#include "agent/runtime/qsocagentruntime.h"
#include "qsoc_test.h"
#include "qsoc_test_sshd.h"

#include <QDir>
#include <QFile>
#include <QtTest>

namespace {

/* HOME and the config roots are read when the runtime is built, so they are
 * redirected before QCoreApplication exists. */
struct EnvBootstrap
{
    EnvBootstrap()
        : root(QDir(QDir::tempPath())
                   .filePath(
                       QStringLiteral("test_qsocremotehostprobe_")
                       + QString::number(QCoreApplication::applicationPid())))
    {
        const QString qsocHome = root + QStringLiteral("/config/qsoc");
        QDir().mkpath(qsocHome);
        QDir().mkpath(root + QStringLiteral("/runtime"));
        QFile::setPermissions(
            root + QStringLiteral("/runtime"),
            QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        qputenv("QSOC_HOME", qsocHome.toUtf8());
        qputenv("XDG_CONFIG_HOME", (root + QStringLiteral("/config")).toUtf8());
        qputenv("XDG_RUNTIME_DIR", (root + QStringLiteral("/runtime")).toUtf8());
        qputenv("HOME", root.toUtf8());
        QFile touch(QDir(qsocHome).filePath(QStringLiteral("qsoc.yml")));
        if (touch.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            touch.close();
        }
    }
    QString root;
};

const EnvBootstrap g_env;

const QString kAlias = QStringLiteral("loopback");

QString runTool(QSocAgentRuntime &runtime, const char *name, const json &args)
{
    QSocTool *tool = runtime.toolRegistry()->getTool(QString::fromLatin1(name));
    return tool == nullptr ? QStringLiteral("<no tool>") : tool->execute(args);
}

} // namespace

class Test : public QObject
{
    Q_OBJECT

private:
    QSocTestSshd m_fixture;

    struct Paths
    {
        QString project;
        QString work;
    };

    Paths makePaths(const QString &caseName, const QString &shell = QString()) const
    {
        const QString base = m_fixture.root() + QLatin1Char('/') + caseName;
        const Paths   paths{base + QStringLiteral("/project"), base + QStringLiteral("/work")};
        QDir().mkpath(paths.project + QStringLiteral("/.qsoc"));
        QDir().mkpath(paths.work);
        if (!shell.isEmpty()) {
            QFile file(paths.project + QStringLiteral("/.qsoc/host.yml"));
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                return paths;
            }
            file.write(QStringLiteral("hostList:\n  - alias: %1\n    workspace: %2\n    shell: %3\n")
                           .arg(kAlias, paths.work, shell)
                           .toUtf8());
        }
        return paths;
    }

    static QSocAgentRuntimeOptions optionsFor(const Paths &paths)
    {
        QSocAgentRuntimeOptions options;
        options.projectDirectory = paths.project;
        options.workspace        = paths.work;
        return options;
    }

    /* A binding over the loopback session, with the host the hook decides. */
    bool bind(
        QSocRemoteConnection                                            *conn,
        const QString                                                   &workspace,
        std::function<QSocRemoteHost(QSocSshSession *, const QString &)> probe,
        QString                                                         *error)
    {
        if (probe) {
            conn->setHostProbe(std::move(probe));
        }
        auto *session = new QSocSshSession();
        if (session->connectTo(m_fixture.hostConfig(), error) != QSocSshSession::ConnectStatus::Ok) {
            delete session;
            return false;
        }
        AgentRemoteState state;
        state.session          = session;
        state.sftp             = new QSocSftpClient(*session);
        state.targetKey        = kAlias;
        state.endpointIdentity = kAlias + QLatin1Char(':') + session->hostKeyIdentity();
        if (!prepareAgentRemoteWorkspace(workspace, &state, error)) {
            discardAgentRemoteState(&state);
            return false;
        }
        if (!conn->adopt(std::move(state))) {
            // cppcheck-suppress accessMoved
            discardAgentRemoteState(&state);
            return false;
        }
        return true;
    }

private slots:
    void initTestCase()
    {
        if (m_fixture.start()) {
            QVERIFY(m_fixture.writeClientSshConfig(g_env.root, kAlias));
        }
    }

    void theProbeDescribesThisHost()
    {
        QSOC_REQUIRE_SSHD(m_fixture);
        const Paths          paths = makePaths(QStringLiteral("probe"));
        QSocRemoteConnection conn;
        QString              err;
        QVERIFY2(bind(&conn, paths.work, {}, &err), qPrintable(err));

        const QSocRemoteHost &host = conn.host();
        QCOMPARE(host.kind, QSocRemoteHost::Kind::Posix);
        QCOMPARE(host.hasProc, QDir(QStringLiteral("/proc/1")).exists());
        QVERIFY(!host.os.isEmpty());
        QVERIFY(!host.arch.isEmpty());
        QVERIFY(host.shell.available());
        QVERIFY(host.shell.path.startsWith(QLatin1Char('/')));
        if (host.shell.kind == QSocShellExecutor::Kind::Bash) {
            QVERIFY(!host.shell.version.isEmpty());
        }
    }

    void execFeedsStdinAndSendsEof()
    {
        QSOC_REQUIRE_SSHD(m_fixture);
        const Paths          paths = makePaths(QStringLiteral("stdin"));
        QSocRemoteConnection conn;
        QString              err;
        QVERIFY2(bind(&conn, paths.work, {}, &err), qPrintable(err));

        QSocSshExec exec(*conn.session());
        const auto  piped = exec.run(QStringLiteral("cat"), 10000, QByteArray("over stdin\n"));
        QCOMPARE(piped.exitCode, 0);
        QCOMPARE(piped.stdoutBytes, QByteArray("over stdin\n"));
        /* No input still closes stdin, so a reader ends instead of waiting
         * out the budget. */
        const auto empty = exec.run(QStringLiteral("cat"), 10000);
        QVERIFY(!empty.timedOut);
        QCOMPARE(empty.exitCode, 0);
    }

    void theBashToolRunsALoginShellFromStdin()
    {
        QSOC_REQUIRE_SSHD(m_fixture);
        const Paths      paths = makePaths(QStringLiteral("bash_tool"));
        QSocAgentRuntime runtime(optionsFor(paths));
        QVERIFY(runtime.openSession());
        QString err;
        QVERIFY2(runtime.connectRemote(kAlias, &err), qPrintable(err));

        /* `cat` would wait for EOF forever under the old exec line; the
         * script line gives it /dev/null and runs on. */
        const QString out = runTool(
            runtime,
            "bash",
            json{
                {"command",
                 "cat; case $- in *s*) echo from-stdin;; esac; "
                 "shopt -q login_shell 2>/dev/null && echo login; pwd"},
                {"timeout_ms", 10000}});
        QVERIFY2(out.contains(QStringLiteral("exit_code: 0")), qPrintable(out));
        QVERIFY2(out.contains(QStringLiteral("from-stdin")), qPrintable(out));
        QVERIFY2(out.contains(paths.work), qPrintable(out));
        if (runtime.agent()->getConfig().remoteShell.startsWith(QStringLiteral("bash"))) {
            QVERIFY2(out.contains(QStringLiteral("login")), qPrintable(out));
        }
        runtime.disconnectRemote();
    }

    void aBackgroundJobRunsThroughTheExecutor()
    {
        QSOC_REQUIRE_SSHD(m_fixture);
        const Paths      paths = makePaths(QStringLiteral("job"));
        QSocAgentRuntime runtime(optionsFor(paths));
        QVERIFY(runtime.openSession());
        QString err;
        QVERIFY2(runtime.connectRemote(kAlias, &err), qPrintable(err));

        const QString launched
            = runTool(runtime, "bash", json{{"command", "echo job-ran; pwd"}, {"background", true}});
        const qsizetype at = launched.indexOf(QStringLiteral("job_id: "));
        QVERIFY2(at >= 0, qPrintable(launched));
        const QString jobId = launched.mid(at + 8).section(QLatin1Char('\n'), 0, 0).trimmed();
        QString       output;
        for (int i = 0; i < 50 && !output.contains(QStringLiteral("job-ran")); ++i) {
            QTest::qWait(100);
            output = runTool(
                runtime, "bash_manage", json{{"job_id", jobId.toStdString()}, {"action", "output"}});
        }
        QVERIFY2(output.contains(QStringLiteral("job-ran")), qPrintable(output));
        QVERIFY2(output.contains(paths.work), qPrintable(output));
        runtime.disconnectRemote();
    }

    void theEnvironmentReportsTheRemoteHost()
    {
        QSOC_REQUIRE_SSHD(m_fixture);
        const Paths      paths = makePaths(QStringLiteral("environment"));
        QSocAgentRuntime runtime(optionsFor(paths));
        QVERIFY(runtime.openSession());
        QString err;
        QVERIFY2(runtime.connectRemote(kAlias, &err), qPrintable(err));
        const QString prompt = runtime.agent()->buildSystemPromptWithMemory();
        QVERIFY2(prompt.contains(QStringLiteral("- Executor: remote\n")), qPrintable(prompt));
        QVERIFY(prompt.contains(QStringLiteral("- OS: ")));
        QVERIFY(prompt.contains(QStringLiteral("- Shell: ")));
        runtime.disconnectRemote();
    }

    void aCatalogShellSelectsSh()
    {
        QSOC_REQUIRE_SSHD(m_fixture);
        const Paths      paths = makePaths(QStringLiteral("override_sh"), QStringLiteral("sh"));
        QSocAgentRuntime runtime(optionsFor(paths));
        QVERIFY(runtime.openSession());
        QString err;
        QVERIFY2(runtime.connectRemote(kAlias, &err), qPrintable(err));
        QVERIFY2(
            runtime.agent()->getConfig().remoteShell.startsWith(QStringLiteral("sh (POSIX only")),
            qPrintable(runtime.agent()->getConfig().remoteShell));
        QSocTool *bash = runtime.toolRegistry()->getTool(QStringLiteral("bash"));
        QVERIFY(bash != nullptr);
        QVERIFY(bash->getDescription().contains(QStringLiteral("Only POSIX sh")));
        const QString out = runTool(runtime, "bash", json{{"command", "ps -o args= -p $$"}});
        QVERIFY2(out.contains(QStringLiteral("sh ")), qPrintable(out));
        QVERIFY2(!out.contains(QStringLiteral("bash")), qPrintable(out));
        runtime.disconnectRemote();
    }

    void anInvalidCatalogShellRefusesTheConnect()
    {
        QSOC_REQUIRE_SSHD(m_fixture);
        const Paths paths = makePaths(QStringLiteral("override_bad"), QStringLiteral("/bin/zsh"));
        QSocAgentRuntime runtime(optionsFor(paths));
        QVERIFY(runtime.openSession());
        QString err;
        QVERIFY(!runtime.connectRemote(kAlias, &err));
        QVERIFY2(err.contains(QStringLiteral("shell:")), qPrintable(err));
        QVERIFY(!runtime.isRemote());
    }

    void aShellLessHostRefusesExecAndPassesBangThrough()
    {
        QSOC_REQUIRE_SSHD(m_fixture);
        const Paths          paths = makePaths(QStringLiteral("windows"));
        QSocRemoteConnection conn;
        QString              err;
        const auto           windows = [](QSocSshSession *, const QString &) {
            return parseRemoteHostProbe({}, QStringLiteral("Microsoft Windows [Version 10.0]"), {});
        };
        QVERIFY2(bind(&conn, paths.work, windows, &err), qPrintable(err));

        QSocToolRemoteShellBash bash(nullptr, &conn, conn.path());
        const QString           refused = bash.execute(json{{"command", "touch nope"}});
        QVERIFY2(refused.contains(QStringLiteral("no POSIX shell")), qPrintable(refused));
        QVERIFY(!QFile::exists(paths.work + QStringLiteral("/nope")));

        /* The loopback login shell is POSIX, so the raw line runs; what is
         * checked is the notice and that no directory was applied. */
        const QString bang = runBoundRemoteShellEscape(&conn, QStringLiteral("echo raw-line"));
        QVERIFY2(bang.startsWith(remoteShellEscapePassthroughNotice()), qPrintable(bang));
        QVERIFY2(bang.contains(QStringLiteral("raw-line")), qPrintable(bang));
    }

    void aStalledProbeKeepsTheLinkAndReprobesOnReconnect()
    {
        QSOC_REQUIRE_SSHD(m_fixture);
        const Paths          paths = makePaths(QStringLiteral("stalled"));
        QSocRemoteConnection conn;
        QString              err;
        int                  probes = 0;
        /* The first probe is a real exec that outlives its budget, the way a
         * login that hangs would; the second answers normally. */
        const auto probe = [&probes](QSocSshSession *session, const QString &preference) {
            if (++probes > 1) {
                return probeRemoteHost(session, preference, 10000);
            }
            QSocSshExec exec(*session);
            return unknownRemoteHost(remoteProbeFailure(exec.run(QStringLiteral("sleep 5"), 300)));
        };
        QVERIFY2(bind(&conn, paths.work, probe, &err), qPrintable(err));
        QCOMPARE(conn.host().kind, QSocRemoteHost::Kind::Unknown);
        QCOMPARE(conn.host().shellError, QStringLiteral("shell probe timed out"));
        QVERIFY2(conn.isUsable(), qPrintable(conn.unusableText()));

        QFile seed(paths.work + QStringLiteral("/seen.txt"));
        QVERIFY(seed.open(QIODevice::WriteOnly));
        seed.write("still-readable\n");
        seed.close();
        QSocToolRemoteFileRead read(nullptr, &conn, conn.path());
        QVERIFY(read.execute(json{{"file_path", "seen.txt"}})
                    .contains(QStringLiteral("still-readable")));
        QSocToolRemoteShellBash bash(nullptr, &conn, conn.path());
        const QString           refused = bash.execute(json{{"command", "true"}});
        QVERIFY2(refused.contains(QStringLiteral("shell probe timed out")), qPrintable(refused));

        /* A reconnect is an adopt, and every adopt probes. */
        conn.setRebuilder([this](
                              const QString &,
                              const QString    &workspace,
                              AgentRemoteState *out,
                              QString          *error,
                              QDeadlineTimer) {
            auto *session = new QSocSshSession();
            if (session->connectTo(m_fixture.hostConfig(), error)
                != QSocSshSession::ConnectStatus::Ok) {
                delete session;
                return false;
            }
            out->session          = session;
            out->sftp             = new QSocSftpClient(*session);
            out->targetKey        = kAlias;
            out->endpointIdentity = kAlias + QLatin1Char(':') + session->hostKeyIdentity();
            return prepareAgentRemoteWorkspace(workspace, out, error);
        });
        conn.session()->markTransportDead();
        QCOMPARE(conn.reconnect(&err), QSocRemoteConnection::ReconnectOutcome::Reconnected);
        QCOMPARE(probes, 2);
        QCOMPARE(conn.host().kind, QSocRemoteHost::Kind::Posix);
        QVERIFY(bash.execute(json{{"command", "true"}}).contains(QStringLiteral("exit_code: 0")));
    }

    void cleanupTestCase()
    {
        m_fixture.stop();
        QDir(g_env.root).removeRecursively();
    }
};

QSOC_TEST_MAIN(Test)
#include "test_qsocremotehostprobe.moc"
