// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>
#include "agent/qsocfilehistory.h"
#include "agent/runtime/qsocagentruntime_p.h"
#include "common/qsoclinediff.h"
#include <algorithm>
#include <QSet>

void QSocAgentRuntime::showHistoryDiff()
{
    if (!d->currentFileHistory || d->currentFileHistory->isEmpty()) {
        emitOutput("(no file history available yet — run an edit first)\n");
        return;
    }
    const auto  snapshots = d->currentFileHistory->listSnapshots();
    QList<int>  turns;
    QStringList labels;
    for (const auto &snapshot : snapshots)
        if (snapshot.turn > 0 && !turns.contains(snapshot.turn)) {
            turns.append(snapshot.turn);
            labels.append(QString("Turn #%1").arg(snapshot.turn));
        }
    if (turns.isEmpty()) {
        emitOutput("(no completed turns to diff yet)\n");
        return;
    }
    const int selected = d->menu ? d->menu("Diff: pick a turn", labels, {}, {}) : turns.size() - 1;
    if (selected < 0 || selected >= turns.size())
        return;
    const int turn = turns[selected];
    using Record   = QSocFileHistory::FileRecord;
    QMap<QString, Record> before, after;
    for (const auto &snapshot : snapshots) {
        if (snapshot.turn <= turn - 1)
            for (auto it = snapshot.files.begin(); it != snapshot.files.end(); ++it)
                before.insert(it.key(), it.value());
        if (snapshot.turn == turn)
            after = snapshot.files;
    }
    const auto record = [](const QMap<QString, Record> &map, const QString &path) {
        return map.contains(path) ? map.value(path) : Record::absent(QSocFileHistory::Epoch());
    };
    QSet<QString> all;
    for (auto it = before.begin(); it != before.end(); ++it)
        all.insert(it.key());
    for (auto it = after.begin(); it != after.end(); ++it)
        all.insert(it.key());
    QStringList paths(all.begin(), all.end());
    std::sort(paths.begin(), paths.end());
    QStringList changed, hints;
    for (const auto &path : paths) {
        const auto a = record(before, path), b = record(after, path);
        if (!a.isUnknown() && !b.isUnknown() && a.isAbsent() == b.isAbsent()
            && a.sha256() == b.sha256())
            continue;
        changed.append(path);
        hints.append(a.isUnknown() || b.isUnknown() ? "state unavailable" : "");
    }
    if (changed.isEmpty()) {
        emitOutput("(no files changed in this turn)\n");
        return;
    }
    const int file = d->menu ? d->menu("Diff: pick a file", changed, hints, {}) : 0;
    if (file < 0 || file >= changed.size())
        return;
    if (!hints[file].isEmpty()) {
        emitOutput("File content unavailable; no diff can be displayed.\n");
        return;
    }
    const auto            path    = changed[file];
    const QString         oldText = record(before, path).isPresent()
                                        ? d->currentFileHistory->contentAt(path, turn - 1)
                                        : QString();
    const QString         newText = record(after, path).isPresent()
                                        ? d->currentFileHistory->contentAt(path, turn)
                                        : QString();
    QSocAgentRuntimeEvent event;
    event.kind = QSocAgentRuntimeEvent::Kind::Diff;
    event.text = path;
    event.json = {{"before", oldText.toStdString()}, {"after", newText.toStdString()}};
    emit eventRaised(event);
}
