// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOC_TEST_PTY_H
#define QSOC_TEST_PTY_H

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

#ifdef Q_OS_UNIX
#include <cerrno>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

/* Drive the built qsoc agent in a pseudo-terminal against the mock endpoint. */
namespace QSocTestPty {

inline QString builtQsoc()
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

inline int pickFreePort()
{
    QTcpServer probe;
    if (!probe.listen(QHostAddress::LocalHost, 0)) {
        return 0;
    }
    const int port = probe.serverPort();
    probe.close();
    return port;
}

inline bool waitForPort(int port, int timeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < timeoutMs) {
        QTcpSocket probe;
        probe.connectToHost(QHostAddress::LocalHost, static_cast<quint16>(port));
        if (probe.waitForConnected(200)) {
            return true;
        }
        QTest::qWait(20);
    }
    return false;
}

inline bool waitForMockReady(QProcess &mock, int port, int timeoutMs)
{
    // The mock's MOCK_READY line only proves the interpreter reached main(),
    // not that the server bound, so a TCP connect is the readiness proof.
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < timeoutMs) {
        if (mock.state() == QProcess::NotRunning) {
            return false;
        }
        QTcpSocket probe;
        probe.connectToHost(QHostAddress::LocalHost, static_cast<quint16>(port));
        if (probe.waitForConnected(200)) {
            return true;
        }
        QTest::qWait(20);
    }
    return false;
}

inline QProcessEnvironment isolatedEnvironment(const QString &root)
{
    const QString home    = QDir(root).filePath(QStringLiteral("home"));
    const QString xdg     = QDir(root).filePath(QStringLiteral("xdg"));
    const QString config  = QDir(root).filePath(QStringLiteral("config"));
    const QString runtime = QDir(root).filePath(QStringLiteral("runtime"));
    const QString temp    = QDir(root).filePath(QStringLiteral("tmp"));
    QDir().mkpath(home);
    QDir().mkpath(xdg);
    QDir().mkpath(runtime);
    QDir().mkpath(temp);
    QFile::setPermissions(
        runtime, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    QProcessEnvironment environment;
    environment.insert(QStringLiteral("HOME"), home);
    environment.insert(QStringLiteral("XDG_CONFIG_HOME"), xdg);
    environment.insert(QStringLiteral("XDG_RUNTIME_DIR"), runtime);
    environment.insert(QStringLiteral("QSOC_HOME"), config);
    environment.insert(QStringLiteral("TEMP"), temp);
    environment.insert(QStringLiteral("TMP"), temp);
    environment.insert(QStringLiteral("TMPDIR"), temp);
    environment.insert(QStringLiteral("PATH"), qEnvironmentVariable("PATH"));
    environment.insert(QStringLiteral("LANG"), QStringLiteral("C.UTF-8"));
    environment.insert(QStringLiteral("TERM"), QStringLiteral("xterm-256color"));
    environment.insert(QStringLiteral("NO_PROXY"), QStringLiteral("*"));
    environment.insert(QStringLiteral("no_proxy"), QStringLiteral("*"));
    return environment;
}

class BoundedProcess final : public QProcess
{
public:
    ~BoundedProcess() override { stop(); }

    void stop()
    {
        if (state() == QProcess::NotRunning) {
            return;
        }
        terminate();
        if (!waitForFinished(2000)) {
            kill();
            waitForFinished(2000);
        }
    }
};

#ifdef Q_OS_UNIX

[[noreturn]] inline void failPtyChild()
{
    ::_exit(127);
}

inline void installPty(const char *slavePath, int inheritedSlaveFd)
{
    if (::setsid() < 0) {
        failPtyChild();
    }
    // The inherited fd was opened with O_NOCTTY, so reopen by name. After
    // setsid() the child is a session leader with no controlling terminal, so
    // this open() assigns the slave as its controlling terminal on Linux and
    // macOS alike.
    const int ttyFd = ::open(slavePath, O_RDWR);
    if (ttyFd < 0) {
        failPtyChild();
    }
    for (int fd : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) {
        if (::dup2(ttyFd, fd) != fd) {
            failPtyChild();
        }
    }
    if (ttyFd > STDERR_FILENO) {
        ::close(ttyFd);
    }
    if (inheritedSlaveFd > STDERR_FILENO) {
        ::close(inheritedSlaveFd);
    }

    // Put the slave into raw input before exec so the agent's first read does
    // not race the terminal's canonical default. A cooked pty would echo the
    // typed line itself and hold it in the canonical buffer, and the CR
    // terminator the test sends only commits a line when ICRNL maps it to NL,
    // which Linux does by default and macOS does not. With ECHO and ICANON off
    // from the start, the typed line is only echoed by the agent's compositor
    // (after its raw-mode input monitor is active), so the test's echo wait
    // doubles as a readiness gate and CR always arrives as CR.
    struct termios raw;
    if (::tcgetattr(STDIN_FILENO, &raw) == 0) {
        raw.c_iflag &= ~static_cast<tcflag_t>(ICRNL | INLCR | IXON);
        raw.c_oflag &= ~static_cast<tcflag_t>(OPOST);
        raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
        raw.c_cc[VMIN]  = 1;
        raw.c_cc[VTIME] = 0;
        (void) ::tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }
}

class PtyProcess final : public QProcess
{
public:
    ~PtyProcess() override
    {
        stop();
        closeDescriptors();
    }

    bool startInPty(
        const QString             &program,
        const QStringList         &arguments,
        const QString             &workingDirectory,
        const QProcessEnvironment &environment)
    {
        closeDescriptors();

        masterFd_ = ::posix_openpt(O_RDWR | O_NOCTTY);
        if (masterFd_ < 0 || ::grantpt(masterFd_) != 0 || ::unlockpt(masterFd_) != 0) {
            closeDescriptors();
            return false;
        }
        const char *slaveName = ::ptsname(masterFd_);
        if (slaveName == nullptr) {
            closeDescriptors();
            return false;
        }
        slavePath_ = slaveName;
        slaveFd_   = ::open(slaveName, O_RDWR | O_NOCTTY);
        if (slaveFd_ < 0) {
            closeDescriptors();
            return false;
        }

        struct winsize size = {};
        size.ws_col         = 120;
        size.ws_row         = 30;
        if (::ioctl(slaveFd_, TIOCSWINSZ, &size) != 0) {
            closeDescriptors();
            return false;
        }
        const int flags = ::fcntl(masterFd_, F_GETFL, 0);
        if (flags < 0 || ::fcntl(masterFd_, F_SETFL, flags | O_NONBLOCK) != 0) {
            closeDescriptors();
            return false;
        }
        (void) ::fcntl(masterFd_, F_SETFD, FD_CLOEXEC);
        (void) ::fcntl(slaveFd_, F_SETFD, FD_CLOEXEC);

        setWorkingDirectory(workingDirectory);
        setProcessEnvironment(environment);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const int   childSlave     = slaveFd_;
        const char *childSlavePath = slavePath_.constData();
        setChildProcessModifier(
            [childSlave, childSlavePath]() { installPty(childSlavePath, childSlave); });
#endif
        start(program, arguments);
        const bool started = waitForStarted(5000);
        if (started) {
            ::close(slaveFd_);
            slaveFd_ = -1;
        }
        return started;
    }

    /* Read the pty while waiting on any condition: a child whose output
     * nobody reads blocks in write() once the pty buffer fills, which takes
     * about 1 KiB on macOS. */
    template<typename Predicate>
    bool waitUntil(Predicate predicate, int timeoutMs)
    {
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < timeoutMs && state() != QProcess::NotRunning) {
            drainOutput();
            if (predicate()) {
                return true;
            }
            QTest::qWait(20);
        }
        drainOutput();
        return predicate();
    }

    bool waitForOutput(const QByteArray &needle, int timeoutMs)
    {
        return waitUntil([&]() { return output_.contains(needle); }, timeoutMs);
    }

    qsizetype markOutput()
    {
        drainOutput();
        return output_.size();
    }

    bool waitForOutputAfter(const QByteArray &needle, qsizetype offset, int timeoutMs)
    {
        return waitUntil([&]() { return output_.indexOf(needle, offset) >= 0; }, timeoutMs);
    }

    bool writeInput(const QByteArray &input, int timeoutMs = 2000)
    {
        QElapsedTimer clock;
        qsizetype     written = 0;
        clock.start();
        while (written < input.size() && clock.elapsed() < timeoutMs) {
            const ssize_t result = ::write(
                masterFd_, input.constData() + written, static_cast<size_t>(input.size() - written));
            if (result > 0) {
                written += result;
                continue;
            }
            if (result < 0 && errno == EINTR) {
                continue;
            }
            if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                QTest::qWait(10);
                continue;
            }
            return false;
        }
        return written == input.size();
    }

    bool submitLine(const QByteArray &line, int timeoutMs = 2000)
    {
        const qsizetype priorOutput = markOutput();
        return writeInput(line, timeoutMs) && waitForOutputAfter(line, priorOutput, timeoutMs)
               && writeInput("\r", timeoutMs);
    }

    bool waitForExit(int timeoutMs)
    {
        return waitUntil([this]() { return state() == QProcess::NotRunning; }, timeoutMs);
    }

    const QByteArray &output() const { return output_; }

    void stop()
    {
        if (state() == QProcess::NotRunning) {
            drainOutput();
            return;
        }
        terminate();
        if (!waitForExit(2000)) {
            kill();
            (void) waitForExit(2000);
        }
    }

protected:
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    void setupChildProcess() override { installPty(slavePath_.constData(), slaveFd_); }
#endif

public:
    void drainOutput()
    {
        if (masterFd_ < 0) {
            return;
        }
        char buffer[8192];
        for (;;) {
            const ssize_t size = ::read(masterFd_, buffer, sizeof(buffer));
            if (size > 0) {
                output_.append(buffer, size);
                constexpr qsizetype maxCapture = 4 * 1024 * 1024;
                if (output_.size() > maxCapture) {
                    output_.remove(0, output_.size() - maxCapture);
                }
                continue;
            }
            if (size < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
    }

private:
    void closeDescriptors()
    {
        if (slaveFd_ >= 0) {
            ::close(slaveFd_);
            slaveFd_ = -1;
        }
        if (masterFd_ >= 0) {
            ::close(masterFd_);
            masterFd_ = -1;
        }
    }

    QByteArray output_;
    QByteArray slavePath_;
    int        masterFd_ = -1;
    int        slaveFd_  = -1;
};

#endif

} // namespace QSocTestPty

#endif // QSOC_TEST_PTY_H
