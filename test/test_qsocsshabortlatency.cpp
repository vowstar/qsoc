// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocinterrupt.h"
#include "agent/remote/qsocsshhostconfig.h"
#include "agent/remote/qsocsshsession.h"
#include "cli/qagentinputmonitor.h"
#include "qsoc_test.h"

#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QTcpServer>
#include <QtCore>
#include <QtTest>

#ifndef Q_OS_WIN
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#endif

#ifdef QSOC_TEST_WRAP_READ
namespace {
std::atomic<bool> g_injectInterruptAfterEmptyPipe{false};
} // namespace

extern "C" ssize_t __real_read(int fd, void *buffer, size_t count);

extern "C" ssize_t __wrap_read(int fd, void *buffer, size_t count)
{
    const ssize_t result = __real_read(fd, buffer, count);
    if (result < 0 && errno == EAGAIN && fd == QSocInterrupt::signalReadFd()
        && g_injectInterruptAfterEmptyPipe.exchange(false)) {
        QSocInterrupt::request();
    }
    return result;
}
#endif

#ifdef QSOC_TEST_WRAP_SIGACTION
namespace {
std::atomic<bool> g_failSigaction{false};
} // namespace

extern "C" int __real_sigaction(
    int signalNumber, const struct sigaction *action, struct sigaction *oldAction);

extern "C" int __wrap_sigaction(
    int signalNumber, const struct sigaction *action, struct sigaction *oldAction)
{
    if (signalNumber == SIGINT && g_failSigaction.load()) {
        errno = EPERM;
        return -1;
    }
    return __real_sigaction(signalNumber, action, oldAction);
}
#endif

#ifdef QSOC_TEST_WRAP_TCSETATTR
namespace {
std::atomic<bool> g_failTcsetattr{false};
std::atomic<int>  g_tcsetattrCalls{0};
} // namespace

extern "C" int __real_tcsetattr(int fd, int optionalActions, const struct termios *termiosState);

extern "C" int __wrap_tcsetattr(int fd, int optionalActions, const struct termios *termiosState)
{
    g_tcsetattrCalls.fetch_add(1);
    if (g_failTcsetattr.load()) {
        errno = EIO;
        return -1;
    }
    return __real_tcsetattr(fd, optionalActions, termiosState);
}
#endif

/*
 * How long a user waits after asking a connect attempt to stop.
 *
 * Connecting is a sequence of EAGAIN loops, each around one poll bounded by the
 * operation timeout. A poll of that length is a wait nothing can get out of, so
 * what matters is not whether the stop is eventually noticed but how long after
 * it was asked for. The measurement here is exactly that interval: the probe
 * records when it first answered yes, and the case reports the gap between that
 * and the call returning.
 *
 * The peer is a listening socket that never speaks. The kernel completes the
 * TCP handshake from the listen backlog without the test accepting anything, so
 * libssh2 sends its banner and then waits for a reply that never comes. That is
 * the real blocking path, with no server to install and no thread to drive.
 */

namespace {

/* The session's own budget for this case. Long enough that a wait bounded by it
 * rather than by a slice is unmistakable in the measurement, short enough that
 * the revert-check does not take a minute. */
constexpr int kSessionTimeoutMs = 6000;

/* When the probe starts saying stop, measured from the call starting. Well
 * inside the budget above, so the return can only be the abort. */
constexpr int kAbortAfterMs = 400;

/* What a stop may cost the user. One slice is 200 ms; this allows a slice, the
 * poll that was already in flight, and scheduling noise on a loaded machine. */
constexpr qint64 kAcceptableLatencyMs = 1500;

int pickFreePort()
{
    QTcpServer probe;
    if (!probe.listen(QHostAddress::LocalHost, 0)) {
        return 0;
    }
    const int port = probe.serverPort();
    probe.close();
    return port;
}

QString builtQsoc()
{
    const QDir buildDir(QStringLiteral(QT_TESTCASE_BUILDDIR));
    for (const QString &candidate :
         {QStringLiteral("../qsoc"), QStringLiteral("../qsoc.app/Contents/MacOS/qsoc")}) {
        const QString path = buildDir.absoluteFilePath(candidate);
        if (QFile::exists(path)) {
            return path;
        }
    }
    return {};
}

#ifndef Q_OS_WIN
class ScopedPtyStdin
{
public:
    ~ScopedPtyStdin()
    {
        if (savedStdin_ >= 0) {
            (void) ::dup2(savedStdin_, STDIN_FILENO);
            ::close(savedStdin_);
        }
        if (master_ >= 0) {
            ::close(master_);
        }
    }

    bool openPty()
    {
        savedStdin_ = ::dup(STDIN_FILENO);
        if (savedStdin_ < 0) {
            return false;
        }
        master_ = ::posix_openpt(O_RDWR | O_NOCTTY);
        if (master_ < 0 || ::grantpt(master_) != 0 || ::unlockpt(master_) != 0) {
            return false;
        }
        const char *slaveName = ::ptsname(master_);
        if (slaveName == nullptr) {
            return false;
        }
        const int slave = ::open(slaveName, O_RDWR | O_NOCTTY);
        if (slave < 0) {
            return false;
        }
        const bool installed = ::dup2(slave, STDIN_FILENO) == STDIN_FILENO;
        ::close(slave);
        return installed;
    }

    qint64 writeInput(const QByteArray &bytes) const
    {
        return master_ < 0 ? -1 : ::write(master_, bytes.constData(), bytes.size());
    }

private:
    int savedStdin_ = -1;
    int master_     = -1;
};
#endif

class Test : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        QSocInterrupt::clearRequest();
        (void) QSocInterrupt::drainSignalPipe();
    }

    /*
     * A stop asked for inside an attempt must be honoured inside the attempt.
     * Checking it only between attempts leaves the user holding a frozen
     * interface for the rest of the one in flight, which on a host that
     * accepts and then goes quiet is the whole operation budget.
     */
    void aStopDuringAConnectIsHonouredWithinASlice()
    {
        /* A peer that completes the TCP handshake and then says nothing. */
        QTcpServer silent;
        QVERIFY2(silent.listen(QHostAddress::LocalHost, 0), qPrintable(silent.errorString()));

        QSocSshSession session;
        session.setTimeoutMs(kSessionTimeoutMs);

        QElapsedTimer clock;
        qint64        askedAtMs = -1;
        /* No thread: the probe decides from the clock, so the stop arrives
         * from inside the poll loop that has to notice it. */
        session.setAbortProbe([&clock, &askedAtMs] {
            if (clock.elapsed() < kAbortAfterMs) {
                return false;
            }
            if (askedAtMs < 0) {
                askedAtMs = clock.elapsed();
            }
            return true;
        });

        QSocSshHostConfig host;
        host.hostname = QStringLiteral("127.0.0.1");
        host.port     = silent.serverPort();
        host.user     = QStringLiteral("nobody");
        /* Nothing may be read from the real user's home while this runs. */
        host.identityFiles  = {};
        host.identitiesOnly = true;
        host.strictHostKey  = QSocSshHostConfig::StrictHostKey::AcceptNew;

        QString err;
        clock.start();
        const auto   status       = session.connectTo(host, &err);
        const qint64 returnedAtMs = clock.elapsed();

        QVERIFY2(
            askedAtMs >= 0,
            qPrintable(QStringLiteral(
                           "the stop was never consulted; the call returned after %1 ms "
                           "without ever asking")
                           .arg(returnedAtMs)));
        const qint64 latencyMs = returnedAtMs - askedAtMs;
        qInfo(
            "abort latency: %lld ms (asked at %lld ms, returned at %lld ms)",
            latencyMs,
            askedAtMs,
            returnedAtMs);

        QVERIFY2(
            latencyMs <= kAcceptableLatencyMs,
            qPrintable(QStringLiteral(
                           "the user waited %1 ms after asking the connect to stop; a "
                           "slice is %2 ms and the whole attempt is %3 ms")
                           .arg(latencyMs)
                           .arg(200)
                           .arg(kSessionTimeoutMs)));
        /* And it must name the stop for what it was: the user cancelled, the
         * clock did not run out. Reporting a timeout would tell the user the
         * host was slow when they are the one who stopped it. */
        QCOMPARE(status, QSocSshSession::ConnectStatus::Aborted);
    }

    /*
     * The same slicing must not reach teardown. A disconnect drains libssh2
     * calls that return EAGAIN with nothing done, and giving one of those up
     * leaks the session and its channels, so a stop the user asked for during a
     * connect must not also abandon the release of what that connect built.
     */
    void aStopDoesNotAbandonTeardown()
    {
        QTcpServer silent;
        QVERIFY2(silent.listen(QHostAddress::LocalHost, 0), qPrintable(silent.errorString()));

        QSocSshSession session;
        session.setTimeoutMs(kSessionTimeoutMs);
        /* Stopping from the very first question, so any wait that honours the
         * probe returns at once. */
        session.setAbortProbe([] { return true; });

        QSocSshHostConfig host;
        host.hostname       = QStringLiteral("127.0.0.1");
        host.port           = silent.serverPort();
        host.user           = QStringLiteral("nobody");
        host.identityFiles  = {};
        host.identitiesOnly = true;
        host.strictHostKey  = QSocSshHostConfig::StrictHostKey::AcceptNew;

        QString err;
        QVERIFY(session.connectTo(host, &err) != QSocSshSession::ConnectStatus::Ok);

        /* The teardown has to run to completion even with the probe still
         * saying stop, and it has to come back. */
        QElapsedTimer clock;
        clock.start();
        session.disconnectFromHost();
        QVERIFY2(
            clock.elapsed() < 30000,
            qPrintable(QStringLiteral("teardown did not return; %1 ms").arg(clock.elapsed())));
        QVERIFY(!session.isConnected());
    }

    void uiAcknowledgementCannotEraseTheRequestLatch()
    {
        QVERIFY(!QSocInterrupt::requested());

        QSocInterrupt::request();
        QSocInterrupt::request();
        QVERIFY(QSocInterrupt::requested());
        QSocInterrupt::acknowledge();
        QVERIFY(QSocInterrupt::requested());
        QSocInterrupt::acknowledge();
        QSocInterrupt::acknowledge();
        QVERIFY(QSocInterrupt::requested());
        QSocInterrupt::clearRequest();
        QVERIFY(!QSocInterrupt::requested());
    }

    void aFailedBridgeInstallationBlocksSigintBeforeReturning()
    {
#ifdef QSOC_TEST_WRAP_SIGACTION
        QVERIFY(!QSocInterrupt::handlerReady());

        sigset_t signalSet;
        sigset_t previousSet;
        QVERIFY(::sigemptyset(&signalSet) == 0);
        QVERIFY(::sigaddset(&signalSet, SIGINT) == 0);
        QVERIFY(::sigprocmask(SIG_UNBLOCK, &signalSet, &previousSet) == 0);
        const auto restoreMask = qScopeGuard(
            [&] { (void) ::sigprocmask(SIG_SETMASK, &previousSet, nullptr); });

        g_failSigaction.store(true);
        QVERIFY(!QSocInterrupt::installBridge());
        g_failSigaction.store(false);

        QVERIFY(QSocInterrupt::byteFallbackReady());
        sigset_t blockedSet;
        QVERIFY(::sigprocmask(SIG_SETMASK, nullptr, &blockedSet) == 0);
        QCOMPARE(::sigismember(&blockedSet, SIGINT), 1);

        QCOMPARE(::raise(SIGINT), 0);
        QVERIFY2(!QSocInterrupt::requested(), "a blocked signal ran the unavailable handler");

        QVERIFY(QSocInterrupt::installBridge());
        QVERIFY(QSocInterrupt::handlerReady());
        QVERIFY(!QSocInterrupt::byteFallbackReady());
        QVERIFY(QSocInterrupt::requested());
        QCOMPARE(QSocInterrupt::drainSignalPipe(), 1);
#else
        QSKIP("sigaction failure injection is available on Linux linkers");
#endif
    }

    void anInterruptBurstDoesNotLeaveAPendingInterrupt()
    {
#ifndef Q_OS_WIN
        QVERIFY(QSocInterrupt::installBridge());
#endif
        QCOMPARE(QSocInterrupt::drainSignalPipe(), 0);
        constexpr int requests = 100000;
        for (int i = 0; i < requests; ++i) {
            QSocInterrupt::request();
        }
        QVERIFY(QSocInterrupt::requested());
        QCOMPARE(QSocInterrupt::drainSignalPipe(), 2);
        QVERIFY(QSocInterrupt::requested());
        QSocInterrupt::clearRequest();
        QVERIFY(!QSocInterrupt::requested());

        QCOMPARE(QSocInterrupt::drainSignalPipe(), 0);
        QSocInterrupt::request();
        QVERIFY(QSocInterrupt::requested());
        QCOMPARE(QSocInterrupt::drainSignalPipe(), 1);
        QVERIFY(QSocInterrupt::requested());
        QSocInterrupt::clearRequest();
        QVERIFY(!QSocInterrupt::requested());
    }

    void aNestedUiDrainCannotHideAnInterruptFromTheBlockingProbe()
    {
#ifndef Q_OS_WIN
        QVERIFY(QSocInterrupt::installBridge());
#endif
        QSocInterrupt::request();

        int        drained = -1;
        QEventLoop nested;
        QTimer::singleShot(0, &nested, [&] {
            drained = QSocInterrupt::drainSignalPipe();
            nested.quit();
        });
        nested.exec();

        QCOMPARE(drained, 1);
        QVERIFY(QSocInterrupt::requested());
        const auto abortProbe = [] { return QSocInterrupt::requested(); };
        QVERIFY(abortProbe());

        QSocInterrupt::clearRequest();
        QVERIFY(!QSocInterrupt::requested());
    }

    void clearingARequestDoesNotDropRapidUiEdges()
    {
#ifndef Q_OS_WIN
        QVERIFY(QSocInterrupt::installBridge());
#endif
        QSocInterrupt::request();
        QSocInterrupt::request();
        QSocInterrupt::clearRequest();

        QVERIFY(!QSocInterrupt::requested());
        QCOMPARE(QSocInterrupt::drainSignalPipe(), 2);
    }

    void aForegroundHandoffDropsItsLatchAndEdgesTogether()
    {
#ifndef Q_OS_WIN
        QVERIFY(QSocInterrupt::installBridge());
#endif
        QSocInterrupt::request();
        QSocInterrupt::request();
        QVERIFY(QSocInterrupt::requested());

        QVERIFY(QSocInterrupt::finishForegroundHandoff());
        QVERIFY(!QSocInterrupt::requested());
        QCOMPARE(QSocInterrupt::drainSignalPipe(), 0);

        QSocInterrupt::request();
        QVERIFY(QSocInterrupt::requested());
        QCOMPARE(QSocInterrupt::drainSignalPipe(), 1);
        QSocInterrupt::clearRequest();
    }

    void anInterruptAfterAnEmptyDrainBelongsToTheResumedUi()
    {
#ifdef QSOC_TEST_WRAP_READ
        QVERIFY(QSocInterrupt::installBridge());
        QVERIFY(QSocInterrupt::finishForegroundHandoff());

        bool interrupted = true;
        g_injectInterruptAfterEmptyPipe.store(true);
        QVERIFY(QSocInterrupt::finishForegroundHandoff(&interrupted));
        QVERIFY(!interrupted);
        QVERIFY(!QSocInterrupt::requested());
        QCOMPARE(QSocInterrupt::drainSignalPipe(), 1);
#else
        QSKIP("read ordering injection is available on Linux linkers");
#endif
    }

    void aNonStreamingSingleQueryObservesCtrlC()
    {
#ifdef Q_OS_WIN
        QSKIP("sending Ctrl-C to a child console is POSIX-only in this test");
#else
        const QString qsoc = builtQsoc();
        if (qsoc.isEmpty()) {
            QSKIP("a built qsoc binary is required");
        }
        const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
        if (python.isEmpty()) {
            QSKIP("python3 is required");
        }
        const QString mockScript
            = QDir(QStringLiteral(QT_TESTCASE_SOURCEDIR)).absoluteFilePath("qsoc_mock_llm.py");
        if (!QFile::exists(mockScript)) {
            QFAIL("the tracked mock LLM script is missing");
        }

        QTemporaryDir dir(QDir::tempPath() + QStringLiteral("/test_qsoc_nostream-XXXXXX"));
        QVERIFY2(dir.isValid(), qPrintable(dir.errorString()));
        const QString home       = dir.path() + QStringLiteral("/home");
        const QString xdg        = dir.path() + QStringLiteral("/xdg");
        const QString requestLog = dir.path() + QStringLiteral("/requests.jsonl");
        QVERIFY(QDir().mkpath(home));
        QVERIFY(QDir().mkpath(xdg));

        const int port = pickFreePort();
        QVERIFY(port != 0);
        QFile config(dir.path() + QStringLiteral("/.qsoc.yml"));
        QVERIFY(config.open(QIODevice::WriteOnly));
        config.write(QStringLiteral(
                         "llm:\n"
                         "  model: mock\n"
                         "  models:\n"
                         "    mock:\n"
                         "      name: Mock\n"
                         "      url: \"http://127.0.0.1:%1/v1/chat/completions\"\n"
                         "      timeout: 20000\n"
                         "      context: 131072\n"
                         "      max_output_tokens: 4096\n"
                         "      reasoning: false\n"
                         "proxy:\n"
                         "  type: none\n")
                         .arg(port)
                         .toUtf8());
        config.close();

        QProcessEnvironment env;
        env.insert(QStringLiteral("HOME"), home);
        env.insert(QStringLiteral("XDG_CONFIG_HOME"), xdg);
        env.insert(QStringLiteral("QSOC_HOME"), home + QStringLiteral("/.qsoc"));
        env.insert(QStringLiteral("PATH"), qEnvironmentVariable("PATH"));
        env.insert(QStringLiteral("LANG"), QStringLiteral("C.UTF-8"));
        env.insert(QStringLiteral("TERM"), QStringLiteral("dumb"));
        env.insert(QStringLiteral("NO_PROXY"), QStringLiteral("*"));
        env.insert(QStringLiteral("no_proxy"), QStringLiteral("*"));

        QProcess   mock;
        QProcess   child;
        const auto stopProcesses = qScopeGuard([&] {
            for (QProcess *process : {&child, &mock}) {
                if (process->state() == QProcess::NotRunning) {
                    continue;
                }
                process->terminate();
                if (!process->waitForFinished(3000)) {
                    process->kill();
                    (void) process->waitForFinished(2000);
                }
            }
        });

        QProcessEnvironment mockEnv = env;
        mockEnv.insert(QStringLiteral("MOCK_TTL"), QStringLiteral("30"));
        mockEnv.insert(QStringLiteral("MOCK_DELAY"), QStringLiteral("15"));
        mockEnv.insert(QStringLiteral("MOCK_REPLY"), QStringLiteral("MOCKOK"));
        mockEnv.insert(QStringLiteral("MOCK_REQUEST_LOG"), requestLog);
        mock.setProcessEnvironment(mockEnv);
        mock.setWorkingDirectory(dir.path());
        mock.start(python, {mockScript, QString::number(port), QStringLiteral("none")});
        QVERIFY(mock.waitForStarted(5000));

        QElapsedTimer readyClock;
        readyClock.start();
        bool mockReady = false;
        while (readyClock.elapsed() < 8000 && !mockReady) {
            QTcpSocket probe;
            probe.connectToHost(QHostAddress::LocalHost, static_cast<quint16>(port));
            mockReady = probe.waitForConnected(200);
            if (!mockReady) {
                QTest::qWait(50);
            }
        }
        QVERIFY2(mockReady, qPrintable(mock.readAllStandardError()));

        child.setProcessEnvironment(env);
        child.setWorkingDirectory(dir.path());
        child.start(
            qsoc,
            {QStringLiteral("agent"),
             QStringLiteral("--no-stream"),
             QStringLiteral("-q"),
             QStringLiteral("wait for the local reply")});
        QVERIFY(child.waitForStarted(5000));

        QElapsedTimer requestClock;
        requestClock.start();
        QByteArray request;
        while (requestClock.elapsed() < 10000) {
            QFile wire(requestLog);
            if (wire.open(QIODevice::ReadOnly)) {
                request = wire.readAll();
                if (request.contains("\"stream\":false")) {
                    break;
                }
            }
            QTest::qWait(20);
        }
        QVERIFY2(request.contains("\"stream\":false"), request.constData());

        QElapsedTimer interruptClock;
        interruptClock.start();
        QVERIFY(::kill(static_cast<pid_t>(child.processId()), SIGINT) == 0);
        QVERIFY2(child.waitForFinished(5000), "the non-streaming query ignored Ctrl-C");
        QVERIFY2(interruptClock.elapsed() < 2000, "the abort exceeded the event-loop polling bound");
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QCOMPARE(child.exitCode(), 0);

        const QByteArray output = child.readAllStandardOutput();
        QCOMPARE(output.trimmed(), QByteArray("(interrupted)"));
        QVERIFY(!output.contains("MOCKOK"));
        QVERIFY(!output.contains('\x1b'));
#endif
    }

    void terminalInterruptOwnershipIsScoped()
    {
#ifdef Q_OS_WIN
        QSKIP("termios is POSIX-only");
#else
        ScopedPtyStdin pty;
        QVERIFY(pty.openPty());

        struct termios custom{};
        QVERIFY(::tcgetattr(STDIN_FILENO, &custom) == 0);
        custom.c_cc[VINTR] = 0x18;
        custom.c_lflag &= ~static_cast<tcflag_t>(ISIG);
        QVERIFY(::tcsetattr(STDIN_FILENO, TCSANOW, &custom) == 0);
        QVERIFY(QSocInterrupt::installBridge());

        {
            QSocInterrupt::TerminalInterruptGuard guard;
            QVERIFY(guard.isReady());
            struct termios during{};
            QVERIFY(::tcgetattr(STDIN_FILENO, &during) == 0);
            QCOMPARE(during.c_cc[VINTR], static_cast<cc_t>(0x03));
            QVERIFY((during.c_lflag & ISIG) != 0);
            QVERIFY(guard.restore());
        }

        struct termios restored{};
        QVERIFY(::tcgetattr(STDIN_FILENO, &restored) == 0);
        QCOMPARE(restored.c_cc[VINTR], static_cast<cc_t>(0x18));
        QVERIFY((restored.c_lflag & ISIG) == 0);

        QAgentInputMonitor monitor;
        monitor.start();
        struct termios raw{};
        QVERIFY(::tcgetattr(STDIN_FILENO, &raw) == 0);
        QCOMPARE(raw.c_cc[VINTR], static_cast<cc_t>(0x03));
        QVERIFY((raw.c_lflag & ISIG) != 0);
        monitor.stop();
        QVERIFY(::tcgetattr(STDIN_FILENO, &restored) == 0);
        QCOMPARE(restored.c_cc[VINTR], static_cast<cc_t>(0x18));
        QVERIFY((restored.c_lflag & ISIG) == 0);

#endif
    }

    void aFailedTerminalModeChangeDoesNotClaimInputOwnership()
    {
#ifdef QSOC_TEST_WRAP_TCSETATTR
        ScopedPtyStdin pty;
        QVERIFY(pty.openPty());
        QVERIFY(QSocInterrupt::installBridge());

        g_tcsetattrCalls.store(0);
        g_failTcsetattr.store(true);
        QAgentInputMonitor monitor;
        monitor.start();
        g_failTcsetattr.store(false);

        QVERIFY2(g_tcsetattrCalls.load() > 0, "the terminal setter was not exercised");
        QVERIFY2(!monitor.isActive(), "failed terminal setup was reported as active");
#else
        QSKIP("tcsetattr failure injection is available on Linux linkers");
#endif
    }

    void aFailedTerminalRestoreKeepsTheLiveInputPath()
    {
#ifdef QSOC_TEST_WRAP_TCSETATTR
        ScopedPtyStdin pty;
        QVERIFY(pty.openPty());
        QVERIFY(QSocInterrupt::installBridge());

        QAgentInputMonitor monitor;
        monitor.start();
        QVERIFY(monitor.isActive());

        g_tcsetattrCalls.store(0);
        g_failTcsetattr.store(true);
        QVERIFY(!monitor.stop());
        g_failTcsetattr.store(false);

        QVERIFY2(monitor.isActive(), "a failed restore discarded the active input state");
        QCOMPARE(pty.writeInput(QByteArrayLiteral("x")), 1);
        QTRY_COMPARE_WITH_TIMEOUT(monitor.getInputBuffer(), QStringLiteral("x"), 1000);
        QVERIFY2(g_tcsetattrCalls.load() > 0, "the terminal restore was not attempted");

        QVERIFY(monitor.stop());
        QVERIFY(!monitor.isActive());
#else
        QSKIP("tcsetattr failure injection is available on Linux linkers");
#endif
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocsshabortlatency.moc"
