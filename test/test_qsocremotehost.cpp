// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/remote/qsocagentremote.h"
#include "agent/remote/qsocremotehost.h"
#include "agent/remote/qsocremotejobs.h"
#include "agent/remote/qsocsftpclient.h"
#include "agent/remote/qsocsshexec.h"
#include "agent/remote/qsoctoolremote.h"
#include "agent/tool/qsoctoolmonitor.h"
#include "common/qsocshellexecutor.h"
#include "common/qsocshellpath.h"
#include "qsoc_test.h"

#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

namespace {

/* Probe output as a host would print it: a login banner, then the frame. */
QString framed(const QStringList &fields)
{
    return QStringLiteral("Welcome to the build host\nbash=/not/a/field\n__QSOC_PROBE_BEGIN__\n")
           + fields.join(QLatin1Char('\n')) + QStringLiteral("\n__QSOC_PROBE_END__\n");
}

const QStringList kBashHost{
    QStringLiteral("os=Linux"),
    QStringLiteral("arch=x86_64"),
    QStringLiteral("bash=/usr/bin/bash"),
    QStringLiteral("bash_version=GNU bash, version 5.2.37(1)-release (x86_64-pc-linux-gnu)"),
    QStringLiteral("sh=/bin/sh"),
    QStringLiteral("sh_login=1"),
    QStringLiteral("proc=1"),
};

const QStringList kShHost{
    QStringLiteral("os=FreeBSD"),
    QStringLiteral("arch=amd64"),
    QStringLiteral("bash="),
    QStringLiteral("sh=/bin/sh"),
    QStringLiteral("sh_login=1"),
};

QSocRemoteHost hostFrom(const QStringList &fields, const QString &preference = QString())
{
    return parseRemoteHostProbe(framed(fields), {}, preference);
}

QSocRemoteHost windowsHost()
{
    return parseRemoteHostProbe(
        QStringLiteral("'sh' is not recognized as an internal or external command"),
        QStringLiteral("\r\nMicrosoft Windows [Version 10.0.20348.2340]\r\n"),
        {});
}

QSocShellExecutor executor(QSocShellExecutor::Kind kind, const QString &path, const QString &ver)
{
    QSocShellExecutor shell;
    shell.kind    = kind;
    shell.path    = path;
    shell.version = ver;
    return shell;
}

/* A connection over a transport that is never dialled, with the host the
 * probe hook decides. */
struct FakeBinding
{
    QObject              scratch;
    QSocRemoteConnection conn;

    explicit FakeBinding(const QSocRemoteHost &host)
    {
        conn.setHostProbe([host](QSocSshSession *, const QString &) { return host; });
        AgentRemoteState state;
        state.session            = new QSocSshSession(&scratch);
        state.sftp               = new QSocSftpClient(*state.session);
        state.targetKey          = QStringLiteral("user@host.invalid:22");
        state.endpointIdentity   = state.targetKey;
        state.workspace          = QStringLiteral("/srv/work");
        state.canonicalWorkspace = state.workspace;
        state.workspaceTreeId    = QStringLiteral("tree-id");
        conn.adopt(std::move(state));
    }
};

} // namespace

class Test : public QObject
{
    Q_OBJECT

private slots:
    void cleanup() { setLocalShellResolver({}); }

    void aBashHostPicksBashByName()
    {
        const QSocRemoteHost host = hostFrom(kBashHost);
        QCOMPARE(host.kind, QSocRemoteHost::Kind::Posix);
        QCOMPARE(host.os, QStringLiteral("Linux"));
        QCOMPARE(host.arch, QStringLiteral("x86_64"));
        QVERIFY(host.hasProc);
        QCOMPARE(host.shell.kind, QSocShellExecutor::Kind::Bash);
        QCOMPARE(host.shell.path, QStringLiteral("/usr/bin/bash"));
        QCOMPARE(host.shell.version, QStringLiteral("5.2.37(1)-release"));
        QCOMPARE(
            remoteScriptExec(host, QStringLiteral("true"), true).command,
            QStringLiteral("bash -l -s"));
        QVERIFY(remoteHostOffersExecTools(host));
        QVERIFY(!remoteHostShellEscapePassthrough(host));
    }

    void aBannerOutsideTheFrameIsNotAField()
    {
        /* The banner line spells a bash path; only the frame counts. */
        QCOMPARE(hostFrom(kShHost).shell.kind, QSocShellExecutor::Kind::Sh);
        QVERIFY(!remoteProbeAnswered(QStringLiteral("__QSOC_PROBE_BEGIN__\nos=Linux\n")));
    }

    void aHostWithoutBashFallsBackToSh()
    {
        const QSocRemoteHost host = hostFrom(kShHost);
        QCOMPARE(host.shell.kind, QSocShellExecutor::Kind::Sh);
        QVERIFY(!host.hasProc);
        QCOMPARE(
            remoteScriptExec(host, QStringLiteral("true"), true).command,
            QStringLiteral("sh -l -s"));
        QVERIFY(host.shell.summary().contains(QStringLiteral("POSIX only")));
    }

    void anShWithoutLoginRunsWithoutTheFlag()
    {
        QStringList fields = kShHost;
        fields.removeAll(QStringLiteral("sh_login=1"));
        QCOMPARE(
            remoteScriptExec(hostFrom(fields), QStringLiteral("true"), true).command,
            QStringLiteral("sh -s"));
    }

    void aPosixHostWithNoShellOffersNoExec()
    {
        const QSocRemoteHost host = hostFrom(
            {QStringLiteral("os=Linux"), QStringLiteral("bash="), QStringLiteral("sh=")});
        QCOMPARE(host.kind, QSocRemoteHost::Kind::Posix);
        QVERIFY(!host.shell.available());
        QVERIFY(!host.shellError.isEmpty());
        QVERIFY(!remoteScriptExec(host, QStringLiteral("true"), true).isValid());
        QVERIFY(remoteHostShellEscapePassthrough(host));
    }

    void aWindowsHostIsClassifiedAndOffersNoExec()
    {
        const QSocRemoteHost host = windowsHost();
        QCOMPARE(host.kind, QSocRemoteHost::Kind::Windows);
        QCOMPARE(host.os, QStringLiteral("Windows 10.0.20348.2340"));
        QVERIFY(!remoteHostOffersExecTools(host));
        QVERIFY(remoteHostShellEscapePassthrough(host));
        QVERIFY(!remoteCommandExec(host, QStringLiteral("/"), QStringLiteral("dir")).isValid());
    }

    void aSilentHostIsUnknown()
    {
        const QSocRemoteHost host = parseRemoteHostProbe({}, {}, {});
        QCOMPARE(host.kind, QSocRemoteHost::Kind::Unknown);
        QVERIFY(!remoteHostOffersExecTools(host));
        QVERIFY(!host.shellError.isEmpty());
    }

    void aStalledOrGarbledProbeIsUnknownWithItsReason()
    {
        QSocSshExec::Result timedOut;
        timedOut.timedOut = true;
        QCOMPARE(remoteProbeFailure(timedOut), QStringLiteral("shell probe timed out"));
        QSocSshExec::Result dead;
        dead.transportDead = true;
        QVERIFY(!remoteProbeFailure(dead).isEmpty());
        QVERIFY(remoteProbeFailure(QSocSshExec::Result{}).isEmpty());

        const QSocRemoteHost stalled = unknownRemoteHost(remoteProbeFailure(timedOut));
        QCOMPARE(stalled.kind, QSocRemoteHost::Kind::Unknown);
        QVERIFY(!remoteHostOffersExecTools(stalled));

        const QSocRemoteHost garbled = parseRemoteHostProbe(
            QStringLiteral("\x1b[2Jmotd\nos=Linux\n"), QStringLiteral("??"), {});
        QCOMPARE(garbled.kind, QSocRemoteHost::Kind::Unknown);
        QVERIFY(garbled.shellError.contains(QStringLiteral("no recognizable answer")));

        FakeBinding     binding(stalled);
        QSocAgentConfig config;
        config.remoteMode = true;
        applyRemoteHostToConfig(&binding.conn, &config);
        const QString lines = QSocAgent::environmentShellLines(config);
        QVERIFY2(
            lines.contains(QStringLiteral("- Shell: unknown (shell probe timed out)")),
            qPrintable(lines));
        QVERIFY(remoteHostShellEscapePassthrough(binding.conn.host()));
    }

    void thePreferenceSelectsAmongWhatWasFound()
    {
        QCOMPARE(hostFrom(kBashHost, QStringLiteral("sh")).shell.kind, QSocShellExecutor::Kind::Sh);
        QCOMPARE(
            hostFrom(kBashHost, QStringLiteral("auto")).shell.kind, QSocShellExecutor::Kind::Bash);
        const QSocRemoteHost missing = hostFrom(kShHost, QStringLiteral("bash"));
        QVERIFY(!missing.shell.available());
        QVERIFY(missing.shellError.contains(QStringLiteral("bash")));
    }

    void onlyAutoBashOrShAreValidPreferences()
    {
        for (const char *ok : {"", "auto", "bash", "sh"}) {
            QVERIFY2(validateRemoteShellPreference(QString::fromLatin1(ok)).isEmpty(), ok);
        }
        for (const char *bad : {"/bin/bash", "zsh", "Bash", "powershell"}) {
            QVERIFY2(!validateRemoteShellPreference(QString::fromLatin1(bad)).isEmpty(), bad);
        }
        FakeBinding binding(hostFrom(kBashHost));
        QString     err;
        QVERIFY(!binding.conn.setShellPreference(QStringLiteral("fish"), &err));
        QVERIFY(err.contains(QStringLiteral("fish")));
        QVERIFY(binding.conn.setShellPreference(QStringLiteral("sh"), &err));
        QCOMPARE(binding.conn.shellPreference(), QStringLiteral("sh"));
    }

    void theProbeLineSurvivesAnyLoginShell()
    {
        /* csh-family shells expand ! even inside single quotes and refuse a
         * newline in one, so the probe carries neither. */
        const QString probe = remoteHostProbeCommand();
        QVERIFY(probe.startsWith(QStringLiteral("sh -c '")));
        QVERIFY(probe.endsWith(QLatin1Char('\'')));
        QCOMPARE(probe.count(QLatin1Char('\'')), 2);
        QVERIFY(!probe.contains(QLatin1Char('!')));
        QVERIFY(!probe.contains(QLatin1Char('\n')));
        QCOMPARE(remoteWindowsProbeCommand(), QStringLiteral("cmd /c ver"));
    }

    void theExecLineCarriesNoUserText()
    {
        const QSocRemoteHost host    = hostFrom(kBashHost);
        const QString        command = QStringLiteral("echo 'it''s' !! \"$HOME\" `id` %PATH%");
        const QSocRemoteExec request
            = remoteCommandExec(host, QStringLiteral("/srv/it's here"), command);
        QCOMPARE(request.command, QStringLiteral("bash -l -s"));
        const QString input = QString::fromUtf8(request.input);
        QVERIFY(input.startsWith(QStringLiteral("eval ")));
        QVERIFY(input.endsWith(QStringLiteral(" </dev/null; exit $?\n")));
        QCOMPARE(input.count(QLatin1Char('\n')), 1);
    }

#ifdef Q_OS_UNIX
    void theStdinScriptRunsInTheDirectoryAndCannotBeEaten()
    {
        const QString sh = QStandardPaths::findExecutable(QStringLiteral("sh"));
        if (sh.isEmpty()) {
            QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("sh"));
        }
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString cwd = dir.path() + QStringLiteral("/it's dir");
        QVERIFY(QDir().mkpath(cwd));

        /* `cat` reads stdin: it must see /dev/null, not the rest of the
         * script, and the line after it must still run. */
        const QSocRemoteExec request = remoteCommandExec(
            hostFrom(kShHost), cwd, QStringLiteral("cat\nprintf 'pwd=%s\\n' \"$(pwd)\"\nexit 3"));
        QProcess proc;
        proc.start(sh, {QStringLiteral("-s")});
        QVERIFY(proc.waitForStarted(5000));
        proc.write(request.input);
        proc.closeWriteChannel();
        QVERIFY(proc.waitForFinished(10000));
        const QString out = QString::fromUtf8(proc.readAllStandardOutput());
        QCOMPARE(out, QStringLiteral("pwd=%1\n").arg(cwd));
        QCOMPARE(proc.exitCode(), 3);
    }

    void aMissingDirectoryStopsTheCommand()
    {
        const QString sh = QStandardPaths::findExecutable(QStringLiteral("sh"));
        if (sh.isEmpty()) {
            QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("sh"));
        }
        const QSocRemoteExec request = remoteCommandExec(
            hostFrom(kShHost), QStringLiteral("/nonexistent/qsoc"), QStringLiteral("echo ran"));
        QProcess proc;
        proc.start(sh, {QStringLiteral("-s")});
        QVERIFY(proc.waitForStarted(5000));
        proc.write(request.input);
        proc.closeWriteChannel();
        QVERIFY(proc.waitForFinished(10000));
        QVERIFY(!proc.readAllStandardOutput().contains("ran"));
        QVERIFY(proc.exitCode() != 0);
    }
#endif

    void jobScriptsUseTheProbedShell()
    {
        const QSocRemoteHost host   = hostFrom(kShHost);
        const QString        script = jobLaunchScript(
            QStringLiteral("/srv/work/.qsoc-agent/jobs/j"),
            QStringLiteral("/srv/work"),
            QStringLiteral("j"),
            QStringLiteral("make"),
            host.shell);
        QVERIFY(script.contains(QStringLiteral("nohup sh -c")));
        QVERIFY(script.contains(QStringLiteral("nohup sh -l -c")));
        QVERIFY(!script.contains(QStringLiteral("bash")));
    }

    void aShellLessHostGetsNoExecTools()
    {
        QSocMonitorTaskSource source;
        for (const QSocRemoteHost &host : {windowsHost(), parseRemoteHostProbe({}, {}, {})}) {
            FakeBinding       binding(host);
            QObject           owner;
            QSocToolRegistry *registry
                = buildAgentRemoteRegistry(&owner, &binding.conn, nullptr, &source);
            for (const char *name : {"bash", "bash_manage", "monitor", "monitor_stop"}) {
                QVERIFY2(registry->getTool(QString::fromLatin1(name)) == nullptr, name);
            }
            for (const char *name : {"read_file", "write_file", "list_files", "edit_file"}) {
                QVERIFY2(registry->getTool(QString::fromLatin1(name)) != nullptr, name);
            }
        }
        FakeBinding       binding(hostFrom(kBashHost));
        QObject           owner;
        QSocToolRegistry *registry
            = buildAgentRemoteRegistry(&owner, &binding.conn, nullptr, &source);
        for (const char *name : {"bash", "bash_manage", "monitor"}) {
            QVERIFY2(registry->getTool(QString::fromLatin1(name)) != nullptr, name);
        }
    }

    void theShTextReachesTheToolDescription()
    {
        FakeBinding             binding(hostFrom(kShHost));
        QSocToolRemoteShellBash tool(nullptr, &binding.conn, binding.conn.path());
        QVERIFY(tool.getDescription().contains(QStringLiteral("Only POSIX sh")));
        QSocToolRemoteBashManage manage(nullptr, &binding.conn, binding.conn.path());
        QVERIFY(manage.getDescription().contains(QStringLiteral("/proc")));
    }

    void aMonitorRunsThroughTheExecutor()
    {
        QSocMonitorTaskSource::RemoteSpec spec;
        spec.targetKey = QStringLiteral("user@host.invalid:22");
        spec.workspace = QStringLiteral("/srv/work");
        QVERIFY(!QSocMonitorTaskSource::remoteLaunch(spec, QStringLiteral("tail -f log"))
                     .error.isEmpty());

        FakeBinding windows(windowsHost());
        spec.conn = &windows.conn;
        QVERIFY(
            QSocMonitorTaskSource::remoteLaunch(spec, QStringLiteral("tail -f log"))
                .error.contains(QStringLiteral("POSIX shell")));

        FakeBinding bash(hostFrom(kBashHost));
        spec.conn = &bash.conn;
        const auto launch = QSocMonitorTaskSource::remoteLaunch(spec, QStringLiteral("tail -f log"));
        QVERIFY(launch.error.isEmpty());
        QCOMPARE(launch.args.last(), QStringLiteral("bash -l -s"));
        QVERIFY(launch.input.contains("tail -f log"));
        QVERIFY(launch.input.contains("/srv/work"));
    }

    void theRemoteEnvironmentNamesTheRemoteHost()
    {
        FakeBinding     binding(hostFrom(kShHost));
        QSocAgentConfig config;
        config.remoteMode = true;
        applyRemoteHostToConfig(&binding.conn, &config);
        const QString lines = QSocAgent::environmentShellLines(config);
        QVERIFY2(lines.contains(QStringLiteral("- OS: FreeBSD\n")), qPrintable(lines));
        QVERIFY(lines.contains(QStringLiteral("- Arch: amd64\n")));
        QVERIFY(lines.contains(QStringLiteral("- Shell: sh (POSIX only")));
        QVERIFY(lines.contains(QStringLiteral("- Executor: remote\n")));
        QVERIFY(lines.contains(QStringLiteral("MCP")));

        FakeBinding windows(windowsHost());
        applyRemoteHostToConfig(&windows.conn, &config);
        const QString none = QSocAgent::environmentShellLines(config);
        QVERIFY2(none.contains(QStringLiteral("- Shell: none (")), qPrintable(none));
        QVERIFY(none.contains(QStringLiteral("not offered")));
    }

    void theLocalEnvironmentNamesEachShellKind()
    {
        struct Case
        {
            QSocShellExecutor shell;
            const char       *expected;
        };
        const Case cases[] = {
            {executor(
                 QSocShellExecutor::Kind::Bash, QStringLiteral("/bin/bash"), QStringLiteral("5.2")),
             "- Shell: bash 5.2\n"},
            {executor(QSocShellExecutor::Kind::Sh, QStringLiteral("/bin/sh"), {}),
             "- Shell: sh (POSIX only"},
            {executor(
                 QSocShellExecutor::Kind::GitBash,
                 QStringLiteral("C:/Program Files/Git/bin/bash.exe"),
                 QStringLiteral("5.2")),
             "/c/... paths"},
            {executor(QSocShellExecutor::Kind::None, {}, {}), "- Shell: unavailable ("},
        };
        for (const Case &item : cases) {
            setLocalShellResolver([shell = item.shell] { return shell; });
            const QString lines = QSocAgent::environmentShellLines(QSocAgentConfig{});
            QVERIFY2(lines.contains(QString::fromLatin1(item.expected)), qPrintable(lines));
            QVERIFY(lines.contains(QStringLiteral("- Executor: local\n")));
        }
    }

    void shellKindsFollowTheFileName()
    {
        QCOMPARE(
            classifyShellPath(QStringLiteral("/usr/bin/bash"), false),
            QSocShellExecutor::Kind::Bash);
        QCOMPARE(classifyShellPath(QStringLiteral("/bin/sh"), false), QSocShellExecutor::Kind::Sh);
        QCOMPARE(
            classifyShellPath(QStringLiteral("C:/Git/bin/bash.exe"), true),
            QSocShellExecutor::Kind::GitBash);
        QCOMPARE(classifyShellPath({}, false), QSocShellExecutor::Kind::None);
        QCOMPARE(
            parseBashVersion(QStringLiteral("GNU bash, version 3.2.57(1)-release")),
            QStringLiteral("3.2.57(1)-release"));
    }

    void gitBashIsLookedForInTheStandardInstallRoots()
    {
        const QHash<QString, QString> env{
            {QStringLiteral("ProgramFiles"), QStringLiteral("C:\\Program Files")},
            {QStringLiteral("ProgramFiles(x86)"), QStringLiteral("C:\\Program Files (x86)")},
            {QStringLiteral("LOCALAPPDATA"), QStringLiteral("C:\\Users\\u\\AppData\\Local")},
            {QStringLiteral("USERPROFILE"), QStringLiteral("C:\\Users\\u")},
        };
        const QStringList found = QSocShellPath::gitBashInstallCandidates(env);
        for (const char *path :
             {"C:/Program Files/Git/bin/bash.exe",
              "C:/Program Files (x86)/Git/bin/bash.exe",
              "C:/Users/u/AppData/Local/Programs/Git/bin/bash.exe",
              "C:/Users/u/scoop/apps/git/current/bin/bash.exe"}) {
            QVERIFY2(found.contains(QString::fromLatin1(path)), path);
        }
        for (const QString &candidate : found) {
            QVERIFY2(
                !candidate.contains(QStringLiteral("System32"), Qt::CaseInsensitive),
                qPrintable(candidate));
        }
        QHash<QString, QString> scoop{{QStringLiteral("SCOOP"), QStringLiteral("D:\\scoop")}};
        QVERIFY(
            QSocShellPath::gitBashInstallCandidates(scoop).contains(
                QStringLiteral("D:/scoop/apps/git/current/bin/bash.exe")));
        QVERIFY(QSocShellPath::gitBashInstallCandidates({}).isEmpty());
    }
};

QSOC_TEST_MAIN(Test)
#include "test_qsocremotehost.moc"
