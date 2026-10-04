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
#include "common/qsocmachine.h"
#include "common/qsocshellexecutor.h"
#include "common/qsocshellpath.h"
#include "qsoc_test.h"

#include <QProcess>
#include <QRegularExpression>
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

QSocMachine hostFrom(const QStringList &fields, const QString &preference = QString())
{
    return parseRemoteHostProbe(framed(fields), {}, preference);
}

/* The Windows probe as cmd runs it: defined %NAME% expand, the rest stays,
 * and echo keeps the quotes. */
QString cmdEcho(const QHash<QString, QString> &env)
{
    static const QRegularExpression varRe(QStringLiteral("%([^%$|]+)%"));
    QString                         line = remoteWindowsProbeCommand().mid(5);
    QString                         out;
    qsizetype                       last = 0;
    for (auto it = varRe.globalMatch(line); it.hasNext();) {
        const auto match = it.next();
        out += line.mid(last, match.capturedStart() - last);
        out += env.contains(match.captured(1)) ? env.value(match.captured(1)) : match.captured(0);
        last = match.capturedEnd();
    }
    return out + line.mid(last) + QStringLiteral("\r\n");
}

/* The Windows probe as PowerShell runs it: ${env:NAME} expand, quotes go. */
QString powerShellEcho(const QHash<QString, QString> &env)
{
    static const QRegularExpression varRe(QStringLiteral(R"(\$\{env:([^}]+)\})"));
    QString                         line = remoteWindowsProbeCommand().mid(5);
    line                                 = line.mid(1, line.size() - 2);
    QString   out;
    qsizetype last = 0;
    for (auto it = varRe.globalMatch(line); it.hasNext();) {
        const auto match = it.next();
        out += line.mid(last, match.capturedStart() - last) + env.value(match.captured(1));
        last = match.capturedEnd();
    }
    return out + line.mid(last) + QStringLiteral("\r\n");
}

const QHash<QString, QString> kWindowsEnv{
    {QStringLiteral("OS"), QStringLiteral("Windows_NT")},
    {QStringLiteral("PROCESSOR_ARCHITECTURE"), QStringLiteral("AMD64")},
    {QStringLiteral("ProgramFiles"), QStringLiteral("C:\\Program Files")},
    {QStringLiteral("ProgramFiles(x86)"), QStringLiteral("C:\\Program Files (x86)")},
    {QStringLiteral("LOCALAPPDATA"), QStringLiteral("C:\\Users\\u\\AppData\\Local")},
    {QStringLiteral("USERPROFILE"), QStringLiteral("C:\\Users\\u")},
};

const QString kGitBash = QStringLiteral("C:/Program Files/Git/bin/bash.exe");

/* A host that has Git Bash in the default install root only. */
bool hasGitBash(const QString &path)
{
    return QSocShellPath::toWindowsPath(path) == QSocShellPath::toWindowsPath(kGitBash);
}

QString notRecognized()
{
    return QStringLiteral("'sh' is not recognized as an internal or external command");
}

QSocMachine windowsHost()
{
    return parseRemoteHostProbe(notRecognized(), cmdEcho(kWindowsEnv), {});
}

QSocMachine gitBashHost(const QString &windowsOut, const QString &preference = {})
{
    return parseRemoteHostProbe(notRecognized(), windowsOut, preference, hasGitBash);
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

    explicit FakeBinding(const QSocMachine &host)
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
        const QSocMachine host = hostFrom(kBashHost);
        QCOMPARE(host.kind, QSocMachine::Kind::Posix);
        QCOMPARE(host.os, QStringLiteral("Linux"));
        QCOMPARE(host.arch, QStringLiteral("x86_64"));
        QVERIFY(host.hasProc);
        QCOMPARE(host.shell.kind, QSocShellExecutor::Kind::Bash);
        QCOMPARE(host.shell.path, QStringLiteral("/usr/bin/bash"));
        QCOMPARE(host.shell.version, QStringLiteral("5.2.37(1)-release"));
        QCOMPARE(
            remoteScriptExec(host, QStringLiteral("true"), true).command,
            QStringLiteral("bash -l -s"));
        QVERIFY(machineOffersExecTools(host));
        QCOMPARE(machineShellEscapeMode(host), QSocShellEscapeMode::Executor);
    }

    void aBannerOutsideTheFrameIsNotAField()
    {
        /* The banner line spells a bash path; only the frame counts. */
        QCOMPARE(hostFrom(kShHost).shell.kind, QSocShellExecutor::Kind::Sh);
        QVERIFY(!remoteProbeAnswered(QStringLiteral("__QSOC_PROBE_BEGIN__\nos=Linux\n")));
    }

    void aHostWithoutBashFallsBackToSh()
    {
        const QSocMachine host = hostFrom(kShHost);
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
        const QSocMachine host = hostFrom(
            {QStringLiteral("os=Linux"), QStringLiteral("bash="), QStringLiteral("sh=")});
        QCOMPARE(host.kind, QSocMachine::Kind::Posix);
        QVERIFY(!host.shell.available());
        QVERIFY(!host.shellError.isEmpty());
        QVERIFY(!remoteScriptExec(host, QStringLiteral("true"), true).isValid());
        QVERIFY(machineShellEscapeMode(host) == QSocShellEscapeMode::Passthrough);
    }

    void aWindowsHostIsClassifiedAndOffersNoExec()
    {
        const QSocMachine host = windowsHost();
        QCOMPARE(host.kind, QSocMachine::Kind::Windows);
        QCOMPARE(host.os, QStringLiteral("Windows"));
        QCOMPARE(host.arch, QStringLiteral("AMD64"));
        QCOMPARE(host.loginShell, QSocMachine::LoginShell::Cmd);
        QVERIFY(host.shellError.contains(QStringLiteral("no Git Bash found")));
        QVERIFY(!machineOffersExecTools(host));
        QCOMPARE(machineShellEscapeMode(host), QSocShellEscapeMode::Cmd);
        QVERIFY(!remoteCommandExec(host, QStringLiteral("/"), QStringLiteral("dir")).isValid());
    }

    void aSilentHostIsUnknown()
    {
        const QSocMachine host = parseRemoteHostProbe({}, {}, {});
        QCOMPARE(host.kind, QSocMachine::Kind::Unknown);
        QVERIFY(!machineOffersExecTools(host));
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

        const QSocMachine stalled = unknownRemoteHost(remoteProbeFailure(timedOut));
        QCOMPARE(stalled.kind, QSocMachine::Kind::Unknown);
        QVERIFY(!machineOffersExecTools(stalled));

        const QSocMachine garbled = parseRemoteHostProbe(
            QStringLiteral("\x1b[2Jmotd\nos=Linux\n"), QStringLiteral("??"), {});
        QCOMPARE(garbled.kind, QSocMachine::Kind::Unknown);
        QVERIFY(garbled.shellError.contains(QStringLiteral("no recognizable answer")));

        FakeBinding     binding(stalled);
        QSocAgentConfig config;
        config.remoteMode = true;
        applyRemoteHostToConfig(&binding.conn, &config);
        const QString lines = QSocAgent::environmentShellLines(config);
        QVERIFY2(
            lines.contains(QStringLiteral("- Shell: unavailable (shell probe timed out)")),
            qPrintable(lines));
        QVERIFY(machineShellEscapeMode(binding.conn.host()) == QSocShellEscapeMode::Passthrough);
    }

    void thePreferenceSelectsAmongWhatWasFound()
    {
        QCOMPARE(hostFrom(kBashHost, QStringLiteral("sh")).shell.kind, QSocShellExecutor::Kind::Sh);
        QCOMPARE(
            hostFrom(kBashHost, QStringLiteral("auto")).shell.kind, QSocShellExecutor::Kind::Bash);
        const QSocMachine missing = hostFrom(kShHost, QStringLiteral("bash"));
        QVERIFY(!missing.shell.available());
        QVERIFY(missing.shellError.contains(QStringLiteral("bash")));
    }

    void onlyAutoBashShOrAWindowsBashAreValidPreferences()
    {
        for (const char *ok :
             {"", "auto", "bash", "sh", "C:\\Git\\bin\\bash.exe", "D:/x/bash.exe", "/C:/g/bash.exe"}) {
            QVERIFY2(validateRemoteShellPreference(QString::fromLatin1(ok)).isEmpty(), ok);
        }
        for (const char *bad :
             {"/bin/bash",
              "zsh",
              "Bash",
              "powershell",
              "C:\\Windows\\System32\\cmd.exe",
              "bash.exe",
              "C:\\git\\bin\\mybash.exe"}) {
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
        /* The Windows probe is one echo: no operator that cmd, PowerShell
         * 5.1 and 7 would read differently. */
        const QString windows = remoteWindowsProbeCommand();
        QVERIFY(windows.startsWith(QStringLiteral("echo \"")));
        QVERIFY(windows.endsWith(QLatin1Char('"')));
        QCOMPARE(windows.count(QLatin1Char('"')), 2);
        for (const char *op : {"&", ";", "||", "\n", ">", "<"}) {
            QVERIFY2(!windows.contains(QLatin1String(op)), op);
        }
    }

    void theExecLineCarriesNoUserText()
    {
        const QSocMachine    host    = hostFrom(kBashHost);
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
        const QSocMachine host   = hostFrom(kShHost);
        const QString     script = jobLaunchScript(
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
        for (const QSocMachine &host : {windowsHost(), parseRemoteHostProbe({}, {}, {})}) {
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
        QVERIFY2(none.contains(QStringLiteral("- Shell: unavailable (")), qPrintable(none));
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

    void aMingwUnameIsAWindowsHost()
    {
        /* Git for Windows puts sh on PATH, so the POSIX probe can answer from
         * a Windows host. Its uname names the MSYS runtime. */
        for (const char *uname : {"MINGW64_NT-10.0-22631", "MSYS_NT-10.0", "CYGWIN_NT-10.0"}) {
            const QString posixOut = framed(
                {QStringLiteral("os=") + QLatin1String(uname),
                 QStringLiteral("arch=x86_64"),
                 QStringLiteral("bash=/usr/bin/bash"),
                 QStringLiteral("sh=/usr/bin/sh")});
            QCOMPARE(parseRemoteHostProbe(posixOut, {}, {}).kind, QSocMachine::Kind::Windows);
            const QSocMachine host
                = parseRemoteHostProbe(posixOut, cmdEcho(kWindowsEnv), {}, hasGitBash);
            QCOMPARE(host.kind, QSocMachine::Kind::Windows);
            QCOMPARE(host.shell.kind, QSocShellExecutor::Kind::GitBash);
            QVERIFY(!host.hasProc);
        }
        /* A MINGW answer with no Windows marker leaves the login shell
         * unknown, so nothing can be launched. */
        const QSocMachine silent = parseRemoteHostProbe(
            framed({QStringLiteral("os=MINGW64_NT-10.0"), QStringLiteral("arch=x86_64")}),
            {},
            {},
            hasGitBash);
        QCOMPARE(silent.kind, QSocMachine::Kind::Windows);
        QCOMPARE(silent.arch, QStringLiteral("x86_64"));
        QVERIFY(!machineOffersExecTools(silent));
        QCOMPARE(machineShellEscapeMode(silent), QSocShellEscapeMode::Passthrough);
    }

    void theWindowsAnswerNamesTheLoginShell()
    {
        const QString banner = QStringLiteral("Microsoft Windows [Version 10.0.1]\r\nmotd OS=x\r\n");
        const QSocMachine cmd = gitBashHost(banner + cmdEcho(kWindowsEnv));
        QCOMPARE(cmd.kind, QSocMachine::Kind::Windows);
        QCOMPARE(cmd.loginShell, QSocMachine::LoginShell::Cmd);
        QCOMPARE(cmd.shell.path, QStringLiteral("C:\\Program Files\\Git\\bin\\bash.exe"));
        QVERIFY(machineOffersExecTools(cmd));

        /* PowerShell leaves %OS% as typed and expands ${env:OS}. */
        const QSocMachine ps = gitBashHost(banner + powerShellEcho(kWindowsEnv));
        QCOMPARE(ps.loginShell, QSocMachine::LoginShell::PowerShell);
        QCOMPARE(ps.arch, QStringLiteral("AMD64"));
        QVERIFY(machineOffersExecTools(ps));

        /* Neither shell expanded anything: not a Windows answer. */
        QCOMPARE(
            parseRemoteHostProbe({}, remoteWindowsProbeCommand().mid(5), {}).kind,
            QSocMachine::Kind::Unknown);
    }

    void remoteCandidatesAreTheSharedList()
    {
        QStringList asked;
        const auto  record = [&asked](const QString &path) {
            asked << path;
            return false;
        };
        QHash<QString, QString> env = kWindowsEnv;
        env.insert(QStringLiteral("GIT_INSTALL_ROOT"), QStringLiteral("D:\\Git"));
        env.insert(QStringLiteral("SCOOP"), QStringLiteral("D:\\scoop"));
        parseRemoteHostProbe(notRecognized(), cmdEcho(env), {}, record);
        QCOMPARE(asked, QSocShellPath::windowsGitBashCandidates({}, {}, env));
        QCOMPARE(asked.first(), QStringLiteral("D:/Git/bin/bash.exe"));
        QVERIFY(asked.contains(QStringLiteral("D:/scoop/apps/git/current/bin/bash.exe")));
        QVERIFY(asked.contains(QStringLiteral("C:/Program Files (x86)/Git/bin/bash.exe")));

        /* An undefined variable that cmd leaves as typed is not a root. */
        for (const QString &path : asked) {
            QVERIFY2(!path.contains(QLatin1Char('%')), qPrintable(path));
        }

        /* host.yml shell: with a path is the only candidate, fail closed. */
        asked.clear();
        const QString     pinned = QStringLiteral("E:\\tools\\bash.exe");
        const QSocMachine host
            = parseRemoteHostProbe(notRecognized(), cmdEcho(kWindowsEnv), pinned, record);
        QCOMPARE(asked, QStringList{pinned});
        QVERIFY(!host.shell.available());
        QVERIFY(host.shellError.contains(pinned));

        /* The local list is the same function: the override alone wins. */
        QCOMPARE(
            QSocShellPath::windowsGitBashCandidates(pinned, QStringLiteral("C:/G/cmd/git.exe"), env),
            QStringList{pinned});
        const QStringList local
            = QSocShellPath::windowsGitBashCandidates({}, QStringLiteral("C:/G/cmd/git.exe"), env);
        QCOMPARE(local.first(), QStringLiteral("C:/G/bin/bash.exe"));
        QVERIFY(local.contains(QStringLiteral("D:/Git/bin/bash.exe")));
        for (const QString &path : local) {
            QVERIFY2(
                !path.contains(QStringLiteral("System32"), Qt::CaseInsensitive), qPrintable(path));
        }
    }

    void aPreferenceAppliesOnlyWhereItCan()
    {
        const QSocMachine posix = hostFrom(kBashHost, QStringLiteral("C:\\Git\\bin\\bash.exe"));
        QCOMPARE(posix.kind, QSocMachine::Kind::Posix);
        QVERIFY(!posix.shell.available());
        QVERIFY(posix.shellError.contains(QStringLiteral("Windows hosts only")));

        const QSocMachine sh = gitBashHost(cmdEcho(kWindowsEnv), QStringLiteral("sh"));
        QVERIFY(!sh.shell.available());
        QVERIFY(sh.shellError.contains(QStringLiteral("sh")));

        QCOMPARE(
            gitBashHost(cmdEcho(kWindowsEnv), QStringLiteral("bash")).shell.kind,
            QSocShellExecutor::Kind::GitBash);
        QCOMPARE(
            gitBashHost(cmdEcho(kWindowsEnv), QStringLiteral("/C:/Program Files/Git/bin/bash.exe"))
                .shell.kind,
            QSocShellExecutor::Kind::GitBash);
    }

    void pathsMapBothWays()
    {
        using namespace QSocShellPath;
        QCOMPARE(toPosixPath(QStringLiteral("/C:/Users/u/w")), QStringLiteral("/c/Users/u/w"));
        QCOMPARE(toPosixPath(QStringLiteral("C:\\Users\\u")), QStringLiteral("/c/Users/u"));
        QCOMPARE(toPosixPath(QStringLiteral("D:")), QStringLiteral("/d"));
        QCOMPARE(toPosixPath(QStringLiteral("/srv/work")), QStringLiteral("/srv/work"));
        QCOMPARE(toWindowsPath(QStringLiteral("/c/Users/u")), QStringLiteral("C:\\Users\\u"));
        QCOMPARE(toWindowsPath(QStringLiteral("/C:/Users/u")), QStringLiteral("C:\\Users\\u"));
        QCOMPARE(toWindowsPath(QStringLiteral("d:/x")), QStringLiteral("D:\\x"));
        QCOMPARE(toWindowsPath(QStringLiteral("/C:")), QStringLiteral("C:\\"));
        QCOMPARE(toSftpPath(QStringLiteral("C:\\Users\\u")), QStringLiteral("/C:/Users/u"));
        QCOMPARE(
            toSftpPath(QStringLiteral("C:/Program Files/Git")),
            QStringLiteral("/C:/Program Files/Git"));
        for (const char *path : {"/C:/Users/u/w", "C:\\a b\\c"}) {
            const QString text = QString::fromLatin1(path);
            QCOMPARE(toWindowsPath(toPosixPath(text)), toWindowsPath(text));
        }

        /* A cygpath answer that differs from the string mapping wins for paths
         * under the root; others keep the string mapping. */
        QSocMachine host = gitBashHost(cmdEcho(kWindowsEnv));
        QCOMPARE(machineShellPath(host, QStringLiteral("/C:/w/src")), QStringLiteral("/c/w/src"));
        applyRemoteShellRoot(&host, QStringLiteral("/C:/w"), QStringLiteral("/c/w\n"));
        QVERIFY(host.rootTo.isEmpty());
        applyRemoteShellRoot(&host, QStringLiteral("/C:/w"), QStringLiteral("/work\n"));
        QCOMPARE(machineShellPath(host, QStringLiteral("/C:/w/src")), QStringLiteral("/work/src"));
        QCOMPARE(machineShellPath(host, QStringLiteral("/c:/W")), QStringLiteral("/work"));
        QCOMPARE(machineShellPath(host, QStringLiteral("/C:/wx")), QStringLiteral("/c/wx"));
        QVERIFY(
            QSocAgent::environmentShellLines([&host] {
                QSocAgentConfig config;
                config.remoteMode    = true;
                config.remoteMachine = host;
                return config;
            }())
                .contains(QStringLiteral("- Workspace in bash: /work")));
        QCOMPARE(
            machineShellPath(hostFrom(kBashHost), QStringLiteral("/C:/w")), QStringLiteral("/C:/w"));
    }

    void gitBashStartsFromAFixedLauncher()
    {
        const QSocMachine    cmd     = gitBashHost(cmdEcho(kWindowsEnv));
        const QSocMachine    ps      = gitBashHost(powerShellEcho(kWindowsEnv));
        const QString        command = QStringLiteral("echo \"$HOME\" | wc -c & %PATH% 'x'");
        const QSocRemoteExec viaCmd  = remoteCommandExec(cmd, QStringLiteral("/C:/my w"), command);
        QCOMPARE(viaCmd.command, QStringLiteral(R"(""C:\Program Files\Git\bin\bash.exe" -l -s")"));
        const QSocRemoteExec viaPs = remoteCommandExec(ps, QStringLiteral("/C:/my w"), command);
        QCOMPARE(viaPs.command, QStringLiteral(R"(& 'C:\Program Files\Git\bin\bash.exe' -l -s)"));
        QCOMPARE(viaCmd.input, viaPs.input);
        const QString input = QString::fromUtf8(viaCmd.input);
        QVERIFY2(input.contains(QStringLiteral("/c/my w")), qPrintable(input));
        QCOMPARE(input.count(QLatin1Char('\n')), 1);
        QCOMPARE(
            remoteScriptExec(cmd, QStringLiteral("true"), false).command,
            QStringLiteral(R"(""C:\Program Files\Git\bin\bash.exe" -s")"));
        QCOMPARE(
            windowsLauncher(
                QSocMachine::LoginShell::PowerShell, QStringLiteral("C:/it's/bash.exe"), "-s"),
            QStringLiteral("& 'C:\\it''s\\bash.exe' -s"));
        QVERIFY(windowsLauncher(QSocMachine::LoginShell::Unknown, kGitBash, "-s").isEmpty());

        /* Jobs and monitors take the same path form. */
        const QString job = jobLaunchScript(
            machineShellPath(cmd, QStringLiteral("/C:/w/.qsoc-agent/jobs/j")),
            machineShellPath(cmd, QStringLiteral("/C:/w")),
            QStringLiteral("j"),
            QStringLiteral("make"),
            cmd.shell);
        QVERIFY(job.contains(QStringLiteral("'/c/w/.qsoc-agent/jobs/j'")));
        QVERIFY(job.contains(QStringLiteral("nohup bash -l -c")));
        FakeBinding                       binding(cmd);
        QSocMonitorTaskSource::RemoteSpec spec;
        spec.targetKey = QStringLiteral("user@host.invalid:22");
        spec.workspace = QStringLiteral("/C:/w");
        spec.conn      = &binding.conn;
        const auto launch = QSocMonitorTaskSource::remoteLaunch(spec, QStringLiteral("tail -f log"));
        QVERIFY(launch.error.isEmpty());
        QCOMPARE(launch.args.last(), viaCmd.command);
        QVERIFY(launch.input.contains("/c/w"));
    }

    void aWindowsBangRunsCmdVerbatim()
    {
        const QString     line = QStringLiteral(R"(echo "a|b" & dir | findstr x)");
        const QSocMachine cmd  = gitBashHost(cmdEcho(kWindowsEnv));
        const QSocMachine ps   = gitBashHost(powerShellEcho(kWindowsEnv));
        QCOMPARE(machineShellEscapeMode(cmd), QSocShellEscapeMode::Cmd);
        QCOMPARE(machineShellEscapeMode(windowsHost()), QSocShellEscapeMode::Cmd);

        /* The inner cmd sees `cd /d "C:\w x" && <line>`; the login cmd sees
         * every operator outside its own quotes escaped. */
        QCOMPARE(
            remoteShellEscapeExec(cmd, QStringLiteral("/C:/w x"), line).command,
            QStringLiteral(R"(cmd /d /s /c "cd /d "C:\w x" && echo "a^|b" & dir | findstr x")"));
        QCOMPARE(
            remoteShellEscapeExec(ps, QStringLiteral("/C:/w x"), line).command,
            QStringLiteral(R"(cmd --% /d /s /c "cd /d "C:\w x" && echo "a|b" & dir | findstr x")"));
        QVERIFY(remoteShellEscapeExec(cmd, QStringLiteral("/C:/w"), line).input.isEmpty());
        QCOMPARE(
            cmdCaretEscape(QStringLiteral(R"(a&b "c&d" (e)^)")),
            QStringLiteral(R"(a^&b "c&d" ^(e^)^^)"));
        QCOMPARE(shellEscapeShellName(cmd), QStringLiteral("cmd"));

        /* The same line locally, where the cwd is the process directory. */
        QCOMPARE(
            QSocShellPath::cmdExeNativeArguments(line),
            QStringLiteral(R"(/d /s /c "echo "a|b" & dir | findstr x")"));

        /* POSIX hosts keep the executor; an unknown host passes through. */
        QCOMPARE(
            remoteShellEscapeExec(hostFrom(kBashHost), QStringLiteral("/w"), line).command,
            QStringLiteral("bash -l -s"));
        QVERIFY(!remoteShellEscapeExec(parseRemoteHostProbe({}, {}, {}), {}, line).isValid());
    }

    void outputDecodingIsOneRule()
    {
        QCOMPARE(QSocShellPath::decodeConsoleOutput("caf\xc3\xa9"), QStringLiteral("caf\u00e9"));
        QVERIFY(!QSocShellPath::decodeConsoleOutput("caf\xe9").isEmpty());
    }

    void gitBashExposesTheToolsAndTheRules()
    {
        QSocMonitorTaskSource source;
        FakeBinding           binding(gitBashHost(cmdEcho(kWindowsEnv)));
        QObject               owner;
        QSocToolRegistry     *registry
            = buildAgentRemoteRegistry(&owner, &binding.conn, nullptr, &source);
        for (const char *name : {"bash", "bash_manage", "monitor"}) {
            QVERIFY2(registry->getTool(QString::fromLatin1(name)) != nullptr, name);
        }
        const QString description = registry->getTool(QStringLiteral("bash"))->getDescription();
        QVERIFY2(description.contains(gitBashGuidance()), qPrintable(description));
        QVERIFY(registry->getTool(QStringLiteral("bash_manage"))
                    ->getDescription()
                    .contains(QStringLiteral("/proc")));
    }

    void theEnvironmentIsTheSameForLocalAndRemote()
    {
        /* One machine, described once as this machine and once as a host:
         * every line but the executor matches. */
        for (const QSocShellExecutor &shell :
             {executor(QSocShellExecutor::Kind::GitBash, kGitBash, QStringLiteral("5.2")),
              executor(QSocShellExecutor::Kind::Bash, QStringLiteral("/bin/bash"), {}),
              executor(QSocShellExecutor::Kind::None, {}, {})}) {
            setLocalShellResolver([shell] { return shell; });
            QSocMachine     remote = localMachine();
            const QString   local  = QSocAgent::environmentShellLines(QSocAgentConfig{});
            FakeBinding     binding(remote);
            QSocAgentConfig config;
            config.remoteMode = true;
            applyRemoteHostToConfig(&binding.conn, &config);
            const QString viaHost  = QSocAgent::environmentShellLines(config);
            QString       expected = local;
            expected.replace(
                QStringLiteral("- Executor: local\n"), QStringLiteral("- Executor: remote\n"));
            QVERIFY2(
                viaHost.startsWith(expected),
                qPrintable(viaHost + QStringLiteral("\n--\n") + expected));
        }
        setLocalShellResolver(
            [] { return executor(QSocShellExecutor::Kind::GitBash, kGitBash, {}); });
        const QString local = QSocAgent::environmentShellLines(QSocAgentConfig{});
        QVERIFY2(local.contains(gitBashGuidance()), qPrintable(local));

        /* A shell-less Windows host reads like a shell-less machine. */
        QSocAgentConfig config;
        config.remoteMode    = true;
        config.remoteMachine = windowsHost();
        QVERIFY(
            QSocAgent::environmentShellLines(config).contains(QStringLiteral(
                "- Shell: unavailable (no Git Bash found; install Git for Windows on "
                "the host or set shell: in host.yml); bash, bash_manage and monitor "
                "are not offered\n")));
    }

#ifdef Q_OS_WIN
    void localGitBashIsDiscoveredWithoutWsl()
    {
        QSocShellPath::resetCache();
        const QString found = QSocShellPath::bashPath();
        if (!found.isEmpty()) {
            QVERIFY2(
                found.endsWith(QStringLiteral("bash.exe"), Qt::CaseInsensitive), qPrintable(found));
            QVERIFY2(
                !found.contains(QStringLiteral("System32"), Qt::CaseInsensitive), qPrintable(found));
            QCOMPARE(localShellExecutor().kind, QSocShellExecutor::Kind::GitBash);
        }
        qputenv("QSOC_GIT_BASH_PATH", "C:\\nonexistent\\bash.exe");
        QSocShellPath::resetCache();
        QVERIFY(QSocShellPath::bashPath().isEmpty());
        QCOMPARE(machineShellEscapeMode(localMachine()), QSocShellEscapeMode::Cmd);
        qunsetenv("QSOC_GIT_BASH_PATH");
        QSocShellPath::resetCache();
    }
#endif
};

QSOC_TEST_MAIN(Test)
#include "test_qsocremotehost.moc"
