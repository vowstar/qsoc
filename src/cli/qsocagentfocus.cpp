// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "cli/qsocagentfocus.h"

#include "tui/qtuicompositor.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QStringList>

#include <algorithm>
#include <utility>

QSocAgentFocus::QSocAgentFocus(
    QTuiCompositor &compositor, QSocTranscriptRenderer &mainRenderer, Request request, bool enabled)
    : compositor_(compositor)
    , mainRenderer_(mainRenderer)
    , renderer_(compositor)
    , request_(std::move(request))
    , enabled_(enabled)
{}

bool QSocAgentFocus::supports(const QJsonObject &greeting)
{
    return greeting.value(QStringLiteral("capabilities"))
        .toArray()
        .contains(QStringLiteral("agents"));
}

QString QSocAgentFocus::cycle(
    const QString &current, const QList<QSocTaskRegistry::TaggedRow> &rows, int direction)
{
    struct Agent
    {
        QString key;
        QString run;
        qint64  first  = 0;
        qint64  newest = 0;
        bool    held   = false;
    };
    QList<Agent> agents;
    for (const auto &item : rows) {
        const auto &row = item.row;
        if (row.kind != QSocTask::Kind::SubAgent || !row.live) {
            continue;
        }
        const QString key = row.agentId.isEmpty() ? row.id : row.agentId;
        auto          it  = std::find_if(agents.begin(), agents.end(), [&key](const Agent &agent) {
            return agent.key == key;
        });
        if (it == agents.end()) {
            agents.append({key, row.id, row.startedAtMs, row.startedAtMs, false});
            it = agents.end() - 1;
        } else if (row.startedAtMs >= it->newest) {
            it->run    = row.id;
            it->newest = row.startedAtMs;
        }
        it->first = qMin(it->first, row.startedAtMs);
        it->held  = it->held || row.id == current;
    }
    std::stable_sort(agents.begin(), agents.end(), [](const Agent &lhs, const Agent &rhs) {
        return lhs.first < rhs.first;
    });
    QStringList runs{QString()};
    int         position = 0;
    for (const auto &agent : std::as_const(agents)) {
        runs.append(agent.run);
        if (agent.held) {
            position = static_cast<int>(runs.size()) - 1;
        }
    }
    const int count = static_cast<int>(runs.size());
    return runs.at(((position + direction) % count + count) % count);
}

void QSocAgentFocus::setMainTitle(const QString &title)
{
    mainTitle_ = title;
    showTitle();
}

void QSocAgentFocus::showTitle()
{
    compositor_.setTitle(
        active() ? QStringLiteral("QSoC Agent · viewing %1 · Esc main · Ctrl+C stop").arg(target_)
                 : mainTitle_);
}

void QSocAgentFocus::enter(const QString &id)
{
    if (!enabled_ || id.isEmpty()) {
        return;
    }
    if (!active()) {
        mainTodos_       = compositor_.todoList().getItems();
        mainPlaceholder_ = compositor_.inputLine().getPlaceholder();
        compositor_.swapTranscript(mainTranscript_);
    }
    target_   = id;
    messages_ = nlohmann::json::array();
    poll();
    renderer_.replaceHistory(messages_);
    compositor_.inputLine().setPlaceholder(
        QStringLiteral("Message %1 · Esc returns to main").arg(id));
    showTitle();
    compositor_.invalidate();
}

void QSocAgentFocus::leave()
{
    if (!active()) {
        return;
    }
    target_.clear();
    messages_ = nlohmann::json::array();
    compositor_.clearTranscript();
    compositor_.swapTranscript(mainTranscript_);
    mainTranscript_.clear();
    compositor_.todoList().setItems(mainTodos_);
    const auto held = std::exchange(backlog_, {});
    for (const auto &event : held) {
        mainRenderer_.apply(event);
    }
    compositor_.inputLine().setPlaceholder(mainPlaceholder_);
    showTitle();
    compositor_.invalidate();
}

void QSocAgentFocus::route(const QSocAgentRuntimeEvent &event)
{
    if (active()) {
        backlog_.append(event);
    } else {
        mainRenderer_.apply(event);
    }
}

bool QSocAgentFocus::escape()
{
    if (!active()) {
        return false;
    }
    leave();
    return true;
}

bool QSocAgentFocus::interrupt(bool hadInput)
{
    if (!active()) {
        return false;
    }
    if (!hadInput) {
        request_(QStringLiteral("task_kill"), {{"source", "agent"}, {"id", target_}});
    }
    return true;
}

QString QSocAgentFocus::send(const QString &text)
{
    const auto reply  = request_(QStringLiteral("task_send"), {{"id", target_}, {"message", text}});
    const auto result = reply.value(QStringLiteral("result")).toObject();
    if (!result.value(QStringLiteral("ok")).toBool()) {
        return QStringLiteral("Not sent: %1")
            .arg(reply.value(QStringLiteral("error")).toString(result.value("error").toString()));
    }
    const QString run = result.value(QStringLiteral("task_id")).toString();
    if (!run.isEmpty()) {
        target_ = run;
        compositor_.inputLine().setPlaceholder(
            QStringLiteral("Message %1 · Esc returns to main").arg(run));
        showTitle();
    }
    return result.value(QStringLiteral("delivery")).toString() == QStringLiteral("queued")
               ? QStringLiteral("Queued for %1").arg(target_)
               : QStringLiteral("Sent to %1").arg(target_);
}

bool QSocAgentFocus::refresh()
{
    if (!active() || !poll()) {
        return false;
    }
    renderer_.replaceHistory(messages_);
    return true;
}

bool QSocAgentFocus::poll()
{
    bool changed = false;
    for (int page = 0; active() && page < 16; ++page) {
        const int  held  = static_cast<int>(messages_.size());
        const auto reply = request_(
            QStringLiteral("task_tail"),
            {{"source", "agent"},
             {"id", target_},
             {"format", "history"},
             {"offset", held},
             {"max_bytes", 1024 * 1024}});
        const auto result = reply.value(QStringLiteral("result")).toObject();
        if (!result.value(QStringLiteral("found")).toBool()) {
            return changed;
        }
        const auto batch = nlohmann::json::parse(
            QJsonDocument(result.value(QStringLiteral("messages")).toArray())
                .toJson(QJsonDocument::Compact)
                .toStdString(),
            nullptr,
            false);
        if (result.value(QStringLiteral("offset")).toInt() != held) {
            messages_ = nlohmann::json::array();
            changed   = true;
        }
        if (!batch.is_array() || batch.empty()) {
            break;
        }
        for (const auto &message : batch) {
            messages_.push_back(message);
        }
        changed = true;
        if (result.value(QStringLiteral("eof")).toBool(true)) {
            break;
        }
    }
    return changed;
}
