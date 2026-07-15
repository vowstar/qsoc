// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsochookrunner.h"

#include <QEventLoop>
#include <QPointer>
#include <QProcess>
#include <QStringList>
#include <QTimer>

QSocHookRunner::QSocHookRunner(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<Result>();
}

QSocHookRunner::~QSocHookRunner()
{
    if (m_timer != nullptr) {
        m_timer->stop();
    }
    if (m_process == nullptr || m_process->state() == QProcess::NotRunning) {
        return;
    }
    QObject::disconnect(m_process, nullptr, this, nullptr);
    m_process->kill();
    m_process->waitForFinished(1000);
}

void QSocHookRunner::reset()
{
    if (m_process != nullptr) {
        QObject::disconnect(m_process, nullptr, this, nullptr);
        m_process->deleteLater();
        m_process = nullptr;
    }
    if (m_timer != nullptr) {
        m_timer->deleteLater();
        m_timer = nullptr;
    }
    m_result    = Result{};
    m_phase     = Phase::Idle;
    m_timeoutMs = 0;
}

void QSocHookRunner::start(const HookCommandConfig &cfg, const nlohmann::json &payload)
{
    Q_ASSERT(canStartNow());
    if (!canStartNow()) {
        return;
    }
    reset();

    if (!cfg.isValid()) {
        m_result.status       = Status::StartFailed;
        m_result.errorMessage = QStringLiteral("invalid hook command configuration");
        m_phase               = Phase::Running;
        claimTerminal();
        QTimer::singleShot(0, this, &QSocHookRunner::publishResult);
        return;
    }

    m_phase     = Phase::Running;
    m_timeoutMs = cfg.timeoutSec > 0 ? cfg.timeoutSec * 1000 : 10000;

    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(
        m_process,
        QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
        this,
        &QSocHookRunner::handleProcessFinished);

    m_process
        ->start(QStringLiteral("/bin/bash"), QStringList() << QStringLiteral("-c") << cfg.command);

    QPointer<QSocHookRunner> guard(this);
    QProcess *const          process = m_process;
    const bool               started = process->waitForStarted(5000);
    if (!guard) {
        return;
    }
    if (process != m_process || m_phase != Phase::Running) {
        return;
    }
    if (!started) {
        m_result.status       = Status::StartFailed;
        m_result.errorMessage = m_process->errorString();
        claimTerminal();
        QTimer::singleShot(0, this, &QSocHookRunner::publishResult);
        return;
    }

    /* Hand the payload to the child and close stdin so naive readers
     * (cat / read / jq) terminate cleanly. */
    const std::string serialized = payload.dump();
    m_process->write(serialized.data(), static_cast<qint64>(serialized.size()));
    m_process->write("\n", 1);
    m_process->closeWriteChannel();

    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &QSocHookRunner::handleTimeout);
    m_timer->start(m_timeoutMs);
}

void QSocHookRunner::handleProcessFinished()
{
    if (sender() != m_process || !claimTerminal()) {
        return;
    }
    if (m_timer != nullptr) {
        m_timer->stop();
    }
    captureStreams();
    m_result          = interpretExit();
    m_result.exitCode = m_process->exitCode();
    publishResult();
}

void QSocHookRunner::handleTimeout()
{
    if (!claimTerminal()) {
        return;
    }
    if (m_process != nullptr) {
        QObject::disconnect(m_process, nullptr, this, nullptr);
        m_process->kill();
        const bool stopped = m_process->waitForFinished(1000);
        captureStreams();
        if (!stopped) {
            m_process->close();
        }
    }
    m_result.status       = Status::Timeout;
    m_result.errorMessage = QStringLiteral("hook timed out after %1ms").arg(m_timeoutMs);
    publishResult();
}

bool QSocHookRunner::claimTerminal()
{
    if (m_phase != Phase::Running) {
        return false;
    }
    m_phase = Phase::Settled;
    return true;
}

void QSocHookRunner::publishResult()
{
    if (m_phase != Phase::Settled) {
        return;
    }
    QPointer<QSocHookRunner> guard(this);
    m_phase = Phase::Publishing;
    emit resultPublished(m_result);
    if (!guard) {
        return;
    }
    emit resultReady(m_result);
    if (!guard) {
        return;
    }
    emit finished();
    if (!guard) {
        return;
    }
    m_phase = Phase::Idle;
}

void QSocHookRunner::captureStreams()
{
    if (m_process == nullptr) {
        return;
    }
    m_result.stdoutText = QString::fromUtf8(m_process->readAllStandardOutput());
    m_result.stderrText = QString::fromUtf8(m_process->readAllStandardError());
}

QSocHookRunner::Result QSocHookRunner::interpretExit() const
{
    Result out      = m_result;
    out.stdoutText  = m_result.stdoutText;
    out.stderrText  = m_result.stderrText;
    out.response    = nlohmann::json{};
    out.hasResponse = false;

    /* Try to parse the first non-empty stdout line as JSON. Anything
     * else (plain text, empty stdout) leaves response untouched. */
    if (!out.stdoutText.isEmpty()) {
        const qsizetype newlineIdx = out.stdoutText.indexOf('\n');
        const QString   firstLine  = newlineIdx >= 0 ? out.stdoutText.left(newlineIdx)
                                                     : out.stdoutText;
        const QString   trimmed    = firstLine.trimmed();
        if (!trimmed.isEmpty()) {
            try {
                out.response    = nlohmann::json::parse(trimmed.toStdString());
                out.hasResponse = out.response.is_object();
            } catch (const nlohmann::json::parse_error &) {
                out.hasResponse = false;
            }
        }
    }

    if (m_process != nullptr && m_process->exitStatus() != QProcess::NormalExit) {
        out.status = Status::NonBlockingError;
        return out;
    }

    const int code = m_process != nullptr ? m_process->exitCode() : 0;
    switch (code) {
    case 0:
        out.status = Status::Success;
        break;
    case 2:
        out.status = Status::Block;
        break;
    default:
        out.status = Status::NonBlockingError;
        break;
    }
    return out;
}

QSocHookRunner::Result QSocHookRunner::run(
    const HookCommandConfig &cfg, const nlohmann::json &payload)
{
    if (!canStartNow()) {
        Result result;
        result.status       = Status::StartFailed;
        result.errorMessage = QStringLiteral("hook runner is busy");
        return result;
    }
    QEventLoop                     loop;
    Result                         captured;
    bool                           published = false;
    const QPointer<QSocHookRunner> owner(this);
    connect(this, &QSocHookRunner::resultPublished, &loop, [&](const Result &result) {
        if (published) {
            return;
        }
        captured  = result;
        published = true;
        loop.quit();
    });
    connect(this, &QObject::destroyed, &loop, [&]() {
        if (published) {
            return;
        }
        captured.status       = Status::StartFailed;
        captured.errorMessage = QStringLiteral("hook runner destroyed during run");
        published             = true;
        loop.quit();
    });
    start(cfg, payload);
    if (!owner.isNull() && !published) {
        loop.exec();
    }
    return captured;
}
