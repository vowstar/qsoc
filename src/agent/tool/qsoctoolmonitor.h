// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLMONITOR_H
#define QSOCTOOLMONITOR_H

#include "agent/qsoctaskeventqueue.h"
#include "agent/qsoctool.h"
#include "common/qsocprojectmanager.h"
#include "common/qsoctasksource.h"

#include <QHash>
#include <QPointer>
#include <QProcess>
#include <QTimer>

#include <optional>

class QSocRemoteConnection;

class QSocMonitorTaskSource final : public QSocTaskSource
{
    Q_OBJECT

public:
    /** @brief The bound session a remote monitor runs over, as a remote job. */
    struct RemoteSpec
    {
        QSocRemoteConnection *conn = nullptr;
    };

    struct StartResult
    {
        bool    ok = false;
        QString taskId;
        QString outputFile;
        QString error;
    };

    explicit QSocMonitorTaskSource(
        QObject            *parent         = nullptr,
        QSocTaskEventQueue *eventQueue     = nullptr,
        QSocProjectManager *projectManager = nullptr);
    ~QSocMonitorTaskSource() override;

    QString              sourceTag() const override { return QStringLiteral("monitor"); }
    QList<QSocTask::Row> listTasks() const override;
    QString              tailFor(const QString &id, int maxBytes) const override;
    bool                 killTask(const QString &id) override;

    StartResult startLocal(
        const QString &command,
        const QString &description,
        int            timeoutMs,
        bool           persistent,
        const QString &name  = QString(),
        const QString &owner = QString());
    StartResult startRemote(
        const QString    &command,
        const QString    &description,
        int               timeoutMs,
        bool              persistent,
        const RemoteSpec &remote,
        const QString    &name  = QString(),
        const QString    &owner = QString());

    bool stop(const QString &taskId, const QString &reason = QStringLiteral("stopped"));
    void stopAll();
    /** @brief Stop every monitor running over @p conn, before it goes away. */
    void stopRemote(const QSocRemoteConnection *conn, const QString &reason);

private:
    struct Run
    {
        QString               id;
        QString               name;
        QString               owner; /* agent its notices go to; empty for main */
        QString               command;
        QString               description;
        QString               outputPath;
        QString               status      = QStringLiteral("running");
        qint64                startedAtMs = 0;
        bool                  persistent  = false;
        bool                  remote      = false;
        QProcess             *process     = nullptr;
        QString               remoteJob; /* job id on the remote host */
        QSocRemoteConnection *conn = nullptr;
        QByteArray            stdoutBuffer;
        QByteArray            stderrBuffer;
        qint64                bytesWritten  = 0;
        qint64                windowStartMs = 0;
        int                   windowCount   = 0;
        int                   overflowCount = 0;
        QString               overflowLast;
        QTimer               *overflowTimer = nullptr;
        QTimer               *flushTimer    = nullptr;
        bool                  stopping      = false;
    };

    Run *createRun(
        const QString &command,
        const QString &description,
        bool           persistent,
        const QString &name,
        const QString &owner,
        bool           remote,
        QString       *error);
    void        armRun(Run *run, int timeoutMs);
    Run        *remoteRun(const QString &jobId) const;
    void        onRemoteOutput(const QString &jobId, const QByteArray &bytes, bool stderrStream);
    void        onRemoteSettled(const QString &jobId, int exitCode, const QByteArray &tail);
    bool        signalRemote(Run *run);
    StartResult startProcess(
        const QString     &command,
        const QString     &description,
        int                timeoutMs,
        bool               persistent,
        const QString     &name,
        const QString     &owner,
        const QString     &program,
        const QStringList &args,
        const QString     &workingDir,
        bool               remote,
        const QByteArray  &input = {});
    void drainProcess(Run *run, bool stderrStream);
    void acceptBytes(Run *run, QByteArray &buffer, const QByteArray &data, const QString &stream);
    void acceptLine(Run *run, const QString &stream, const QString &line);
    void flushPartial(Run *run);
    void flushOverflow(Run *run);
    void finishRun(const QString &taskId, int exitCode, QProcess::ExitStatus exitStatus);
    void appendOutput(Run *run, const QString &line);
    void emitEvent(Run *run, const QString &kind, const QString &status, const QString &content);
    void cleanupProcess(Run *run);

    static QString readTail(const QString &path, int maxBytes);

    QSocTaskEventQueue   *eventQueue_     = nullptr;
    QSocProjectManager   *projectManager_ = nullptr;
    QHash<QString, Run *> runs_;
    int                   nextId_ = 1;
};

class QSocToolMonitor : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolMonitor(
        QObject                                         *parent,
        QSocMonitorTaskSource                           *source,
        std::optional<QSocMonitorTaskSource::RemoteSpec> remote = std::nullopt);

    QString getName() const override { return QStringLiteral("monitor"); }
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

private:
    QSocMonitorTaskSource                           *source_ = nullptr;
    std::optional<QSocMonitorTaskSource::RemoteSpec> remote_;
};

class QSocToolMonitorStop : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolMonitorStop(QObject *parent, QSocMonitorTaskSource *source);

    QString getName() const override { return QStringLiteral("monitor_stop"); }
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

private:
    QSocMonitorTaskSource *source_ = nullptr;
};

#endif /* QSOCTOOLMONITOR_H */
