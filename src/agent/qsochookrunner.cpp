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

QSocHookRunner::Result QSocHookRunner::run(
    const HookCommandConfig &cfg, const nlohmann::json &payload)
{
    Result result;

    if (!cfg.isValid()) {
        result.status       = Status::StartFailed;
        result.errorMessage = QStringLiteral("invalid hook command configuration");
        return result;
    }

    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(QStringLiteral("/bin/bash"), QStringList() << QStringLiteral("-c") << cfg.command);

    if (!process.waitForStarted(5000)) {
        result.status       = Status::StartFailed;
        result.errorMessage = process.errorString();
        return result;
    }

    /* Hand the payload to the child and close stdin so naive readers
     * (cat / read / jq) terminate cleanly. */
    const std::string serialized = payload.dump();
    process.write(serialized.data(), static_cast<qint64>(serialized.size()));
    process.write("\n", 1);
    process.closeWriteChannel();

    QEventLoop loop;
    bool       finished = false;

    QObject::connect(
        &process,
        QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
        &loop,
        [&finished, &loop](int, QProcess::ExitStatus) {
            finished = true;
            loop.quit();
        });

    const int timeoutMs = cfg.timeoutSec > 0 ? cfg.timeoutSec * 1000 : 10000;
    QTimer::singleShot(timeoutMs, &loop, [&loop]() { loop.quit(); });

    loop.exec();

    if (!finished) {
        process.kill();
        process.waitForFinished(1000);
        result.status       = Status::Timeout;
        result.errorMessage = QStringLiteral("hook timed out after %1ms").arg(timeoutMs);
        result.stdoutText   = QString::fromUtf8(process.readAllStandardOutput());
        result.stderrText   = QString::fromUtf8(process.readAllStandardError());
        return result;
    }

    result.exitCode   = process.exitCode();
    result.stdoutText = QString::fromUtf8(process.readAllStandardOutput());
    result.stderrText = QString::fromUtf8(process.readAllStandardError());

    /* Try to parse the first non-empty stdout line as JSON. Anything
     * else (plain text, empty stdout) leaves response untouched. */
    if (!result.stdoutText.isEmpty()) {
        const qsizetype newlineIdx = result.stdoutText.indexOf('\n');
        const QString   firstLine  = newlineIdx >= 0 ? result.stdoutText.left(newlineIdx)
                                                     : result.stdoutText;
        const QString   trimmed    = firstLine.trimmed();
        if (!trimmed.isEmpty()) {
            try {
                result.response    = nlohmann::json::parse(trimmed.toStdString());
                result.hasResponse = result.response.is_object();
            } catch (const nlohmann::json::parse_error &) {
                result.hasResponse = false;
            }
        }
    }

    if (process.exitStatus() != QProcess::NormalExit) {
        result.status = Status::NonBlockingError;
        return result;
    }

    switch (result.exitCode) {
    case 0:
        result.status = Status::Success;
        break;
    case 2:
        result.status = Status::Block;
        break;
    default:
        result.status = Status::NonBlockingError;
        break;
    }
    return result;
}
