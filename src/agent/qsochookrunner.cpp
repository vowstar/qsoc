// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsochookrunner.h"

#include <QEventLoop>
#include <QProcess>
#include <QStringList>
#include <QTimer>

QSocHookRunner::QSocHookRunner(QObject *parent)
    : QObject(parent)
{}

QSocHookRunner::~QSocHookRunner() = default;

void QSocHookRunner::reset()
{
    if (m_process != nullptr) {
        m_process->deleteLater();
        m_process = nullptr;
    }
    if (m_timer != nullptr) {
        m_timer->deleteLater();
        m_timer = nullptr;
    }
    m_result    = Result{};
    m_running   = false;
    m_finalized = false;
    m_timeoutMs = 0;
}

void QSocHookRunner::start(const HookCommandConfig &cfg, const nlohmann::json &payload)
{
    Q_ASSERT(!m_running);
    reset();

    if (!cfg.isValid()) {
        m_result.status       = Status::StartFailed;
        m_result.errorMessage = QStringLiteral("invalid hook command configuration");
        m_running             = true;
        m_finalized           = true;
        QTimer::singleShot(0, this, &QSocHookRunner::finished);
        return;
    }

    m_running   = true;
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

    if (!m_process->waitForStarted(5000)) {
        m_result.status       = Status::StartFailed;
        m_result.errorMessage = m_process->errorString();
        m_finalized           = true;
        QTimer::singleShot(0, this, &QSocHookRunner::finished);
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
    if (m_finalized) {
        return;
    }
    if (m_timer != nullptr) {
        m_timer->stop();
    }
    captureStreams();
    m_result          = interpretExit();
    m_result.exitCode = m_process->exitCode();
    m_finalized       = true;
    emit finished();
}

void QSocHookRunner::handleTimeout()
{
    if (m_finalized) {
        return;
    }
    if (m_process != nullptr) {
        m_process->kill();
        m_process->waitForFinished(1000);
        captureStreams();
    }
    m_result.status       = Status::Timeout;
    m_result.errorMessage = QStringLiteral("hook timed out after %1ms").arg(m_timeoutMs);
    m_finalized           = true;
    emit finished();
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
    QEventLoop loop;
    connect(this, &QSocHookRunner::finished, &loop, &QEventLoop::quit);
    start(cfg, payload);
    if (m_running && !m_finalized) {
        loop.exec();
    }
    Result captured = m_result;
    m_running       = false;
    return captured;
}
