// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOC_TEST_SFTP_PEER_H
#define QSOC_TEST_SFTP_PEER_H

#include "agent/remote/qsocsshhostconfig.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>

class QSocTestSftpPeer
{
public:
    ~QSocTestSftpPeer() { stop(); }
    bool start(bool readOnly)
    {
        QString executable = qEnvironmentVariable("QSOC_TEST_SFTP_PEER");
        if (executable.isEmpty()) {
            executable = QCoreApplication::applicationDirPath()
                         + QStringLiteral("/qsoc_test_sftp_peer");
#ifdef Q_OS_WIN
            executable += QStringLiteral(".exe");
#endif
        }
        if (!QFile::exists(executable)) {
            missing_ = true;
            error_   = QStringLiteral("test SFTP peer is unavailable");
            return false;
        }
        if (!directory_.isValid() || !QDir().mkpath(workDir()) || !QDir().mkpath(stateDir())) {
            error_ = QStringLiteral("Cannot create SFTP fixture directories");
            return false;
        }
        QProcessEnvironment environment;
        for (const char *name : {"SYSTEMROOT", "WINDIR"}) {
            if (qEnvironmentVariableIsSet(name))
                environment.insert(QString::fromLatin1(name), qEnvironmentVariable(name));
        }
        for (const char *name : {"HOME", "USERPROFILE", "TMPDIR", "TEMP", "TMP"})
            environment.insert(QString::fromLatin1(name), directory_.path());
        process_.setProcessEnvironment(environment);
        process_.setWorkingDirectory(directory_.path());
        QStringList
            arguments{QStringLiteral("--root"), workDir(), QStringLiteral("--state"), stateDir()};
        if (readOnly)
            arguments.append(QStringLiteral("--read-only"));
        process_.start(executable, arguments);
        if (!process_.waitForStarted(5000) || !process_.waitForReadyRead(5000)) {
            error_ = QStringLiteral("SFTP peer did not become ready");
            return false;
        }
        const auto ready = QJsonDocument::fromJson(process_.readLine(4096)).object();
        const int  port  = ready.value(QStringLiteral("port")).toInt();
        if (!ready.value(QStringLiteral("ready")).toBool() || port < 1 || port > 65535) {
            error_ = QStringLiteral("Invalid SFTP peer readiness");
            return false;
        }
        port_ = port;
        QFile publicKey(stateDir() + QStringLiteral("/host_key.pub"));
        QFile knownHosts(stateDir() + QStringLiteral("/known_hosts"));
        if (!publicKey.open(QIODevice::ReadOnly) || !knownHosts.open(QIODevice::WriteOnly)
            || knownHosts.write(
                   "[127.0.0.1]:" + QByteArray::number(port_) + ' ' + publicKey.readAll())
                   < 1) {
            error_ = QStringLiteral("Cannot pin the SFTP peer host key");
            return false;
        }
        return true;
    }
    bool stop()
    {
        process_.closeWriteChannel();
        if (process_.state() != QProcess::NotRunning && !process_.waitForFinished(2000)) {
            process_.kill();
            process_.waitForFinished(2000);
        }
        return process_.state() == QProcess::NotRunning
               && process_.exitStatus() == QProcess::NormalExit && process_.exitCode() == 0;
    }
    QString workDir() const { return directory_.path() + QStringLiteral("/work"); }
    QString stateDir() const { return directory_.path() + QStringLiteral("/state"); }
    QString canonicalDir() const
    {
        const QString native = QDir::fromNativeSeparators(workDir());
#ifdef Q_OS_WIN
        return QLatin1Char('/') + native;
#else
        return native;
#endif
    }
    bool    missing() const { return missing_; }
    QString error() const { return error_; }
    int     execRequests() const
    {
        QFile file(stateDir() + QStringLiteral("/exec_count"));
        return file.open(QIODevice::ReadOnly) ? file.readAll().toInt() : -1;
    }
    QSocSshHostConfig hostConfig() const
    {
        QSocSshHostConfig host;
        host.hostname           = QStringLiteral("127.0.0.1");
        host.port               = port_;
        host.user               = QStringLiteral("qsoc-test");
        host.identityFiles      = {stateDir() + QStringLiteral("/client_key")};
        host.identitiesOnly     = true;
        host.strictHostKey      = QSocSshHostConfig::StrictHostKey::Yes;
        host.userKnownHostsFile = stateDir() + QStringLiteral("/known_hosts");
        return host;
    }

private:
    QTemporaryDir directory_;
    QProcess      process_;
    QString       error_;
    int           port_    = 0;
    bool          missing_ = false;
};

#endif
