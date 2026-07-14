// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsochookmanager.h"

#include "common/qsocconsole.h"

#include <QEventLoop>
#include <QPointer>
#include <QRegularExpression>

#include <optional>
#include <utility>

namespace {

bool isSimplePipePattern(const QString &pattern)
{
    if (pattern.isEmpty()) {
        return false;
    }
    for (QChar ch : pattern) {
        const bool simple = (ch >= QChar('a') && ch <= QChar('z'))
                            || (ch >= QChar('A') && ch <= QChar('Z'))
                            || (ch >= QChar('0') && ch <= QChar('9')) || ch == QChar('_')
                            || ch == QChar('|');
        if (!simple) {
            return false;
        }
    }
    return true;
}

QString blockReasonFor(const QSocHookRunner::Result &res)
{
    if (res.hasResponse && res.response.is_object()) {
        const auto it = res.response.find("reason");
        if (it != res.response.end() && it->is_string()) {
            return QString::fromStdString(it->get<std::string>());
        }
    }
    const QString trimmed = res.stderrText.trimmed();
    if (!trimmed.isEmpty()) {
        return trimmed;
    }
    return QStringLiteral("hook blocked execution");
}

void mergeResult(QSocHookManager::Outcome &out, const QSocHookRunner::Result &res)
{
    out.rawResults.append(res);
    if (res.status == QSocHookRunner::Status::Block) {
        out.blocked = true;
        if (out.blockReason.isEmpty()) {
            out.blockReason = blockReasonFor(res);
        }
    }
    if (res.hasResponse && res.response.is_object()) {
        out.mergedResponse    = res.response;
        out.hasMergedResponse = true;
    }
}

} // namespace

QSocHookManager::QSocHookManager(QObject *parent)
    : QObject(parent)
{}

void QSocHookManager::setConfig(const QSocHookConfig &config)
{
    m_config = config;
}

bool QSocHookManager::hasHooksFor(QSocHookEvent event) const
{
    return !m_config.matchersFor(event).isEmpty();
}

bool QSocHookManager::matches(const QString &pattern, const QString &subject)
{
    if (pattern.isEmpty() || pattern == QStringLiteral("*")) {
        return true;
    }
    if (isSimplePipePattern(pattern)) {
        const QStringList parts = pattern.split('|', Qt::SkipEmptyParts);
        for (const QString &part : parts) {
            if (part == subject) {
                return true;
            }
        }
        return false;
    }
    QRegularExpression re(QStringLiteral("\\A(?:%1)\\z").arg(pattern));
    if (!re.isValid()) {
        QSocConsole::warn() << "Invalid hook matcher regex:" << pattern;
        return false;
    }
    return re.match(subject).hasMatch();
}

QSocHookManager::Outcome QSocHookManager::fire(
    QSocHookEvent event, const QString &matchSubject, const nlohmann::json &payload)
{
    Outcome out;

    const auto matchers = m_config.matchersFor(event);
    if (matchers.isEmpty()) {
        return out;
    }

    QList<HookCommandConfig> matched;
    for (const auto &group : matchers) {
        if (matches(group.matcher, matchSubject)) {
            for (const auto &cmd : group.commands) {
                matched.append(cmd);
            }
        }
    }
    if (matched.isEmpty()) {
        return out;
    }

    QEventLoop                                   loop;
    const QPointer<QSocHookManager>              owner(this);
    QList<QPointer<QSocHookRunner>>              runners;
    QList<std::optional<QSocHookRunner::Result>> results(matched.size());
    int                                          pending     = 0;
    bool                                         interrupted = false;
    runners.reserve(matched.size());

    const auto settle = [&](qsizetype index, QSocHookRunner::Result result) {
        if (results.at(index).has_value()) {
            return;
        }
        results[index] = std::move(result);
        if (--pending == 0) {
            loop.quit();
        }
    };
    connect(owner.data(), &QObject::destroyed, &loop, [&]() {
        interrupted = true;
        loop.quit();
    });

    for (qsizetype index = 0; index < matched.size() && !owner.isNull(); ++index) {
        auto *runner = new QSocHookRunner(owner.data());
        runners.append(runner);
        ++pending;
        connect(
            runner,
            &QSocHookRunner::resultReady,
            &loop,
            [&, index](const QSocHookRunner::Result &result) { settle(index, result); });
        connect(runner, &QObject::destroyed, &loop, [&, index]() {
            if (results.at(index).has_value()) {
                return;
            }
            QSocHookRunner::Result result;
            result.status       = QSocHookRunner::Status::StartFailed;
            result.errorMessage = QStringLiteral("hook runner destroyed during dispatch");
            interrupted         = true;
            settle(index, std::move(result));
        });
        runner->start(matched.at(index), payload);
    }
    if (pending > 0) {
        loop.exec();
    }

    if (interrupted) {
        for (auto &result : results) {
            if (!result.has_value()) {
                QSocHookRunner::Result failure;
                failure.status       = QSocHookRunner::Status::StartFailed;
                failure.errorMessage = QStringLiteral("hook dispatch interrupted before start");
                result.emplace(std::move(failure));
            }
        }
    }
    for (const auto &result : results) {
        if (result.has_value()) {
            mergeResult(out, result.value());
        }
    }
    if (interrupted) {
        out.blocked = true;
        if (out.blockReason.isEmpty()) {
            out.blockReason = QStringLiteral("hook dispatch interrupted");
        }
    }
    for (const auto &runner : runners) {
        if (!runner.isNull()) {
            runner->deleteLater();
        }
    }
    return out;
}
