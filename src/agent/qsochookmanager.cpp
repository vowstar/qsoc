// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsochookmanager.h"

#include "common/qsocconsole.h"

#include <QEventLoop>
#include <QRegularExpression>

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

    QEventLoop              loop;
    QList<QSocHookRunner *> runners;
    runners.reserve(matched.size());
    int pending = static_cast<int>(matched.size());

    for (const auto &cmd : matched) {
        auto *runner = new QSocHookRunner(this);
        runners.append(runner);
        connect(runner, &QSocHookRunner::finished, &loop, [&pending, &loop]() {
            if (--pending == 0) {
                loop.quit();
            }
        });
        runner->start(cmd, payload);
    }
    if (pending > 0) {
        loop.exec();
    }

    for (auto *runner : runners) {
        const auto &res = runner->result();
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
        runner->deleteLater();
    }
    return out;
}
