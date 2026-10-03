// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>
#include "cli/qsocagentinputhistory.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

namespace {
const QRegularExpression chip(QStringLiteral(R"(\[Pasted text #(\d+)(?: \+\d+ lines)?\])"));
constexpr qint64         maxBytes = 2 * 1024 * 1024;
} // namespace
QString QSocAgentInputHistory::path()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation))
        .filePath("history.jsonl");
}
QString QSocAgentInputHistory::paste(const QString &text)
{
    const int lines = text.count('\n') + 1;
    if (text.size() < 1000 && lines <= 5)
        return text;
    const int id = nextPaste_++;
    pastes_.insert(id, text);
    return QStringLiteral("[Pasted text #%1 +%2 lines]").arg(id).arg(lines);
}
QString QSocAgentInputHistory::expand(const QString &display) const
{
    QString result;
    int     cursor  = 0;
    auto    matches = chip.globalMatch(display);
    while (matches.hasNext()) {
        const auto match = matches.next();
        result += display.mid(cursor, match.capturedStart() - cursor);
        result += pastes_.value(match.captured(1).toInt(), match.captured());
        cursor = match.capturedEnd();
    }
    return result + display.mid(cursor);
}
QStringList QSocAgentInputHistory::load(const QString &project)
{
    QStringList other, current;
    for (const auto &filePath : {path(), QDir(project).filePath(".qsoc/history.jsonl")}) {
        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly))
            continue;
        if (file.size() > maxBytes) {
            file.seek(file.size() - maxBytes);
            file.readLine();
        }
        while (!file.atEnd()) {
            const auto    entry   = QJsonDocument::fromJson(file.readLine()).object();
            const QString display = entry.value("display").toString();
            if (display.isEmpty())
                continue;
            const auto oldPastes = entry.value("pastes").toObject();
            QString    remapped;
            int        cursor  = 0;
            auto       matches = chip.globalMatch(display);
            while (matches.hasNext()) {
                const auto match = matches.next();
                remapped += display.mid(cursor, match.capturedStart() - cursor);
                remapped += oldPastes.contains(match.captured(1))
                                ? paste(oldPastes.value(match.captured(1)).toString())
                                : match.captured();
                cursor = match.capturedEnd();
            }
            remapped += display.mid(cursor);
            auto &destination = entry.value("project").toString(project) == project ? current
                                                                                    : other;
            destination.removeAll(remapped);
            destination.append(remapped);
        }
    }
    other.append(current);
    return other;
}
void QSocAgentInputHistory::append(
    const QString &display, const QString &project, const QString &host)
{
    const QString filePath = path();
    QDir().mkpath(QFileInfo(filePath).absolutePath());
    QLockFile lock(filePath + ".lock");
    if (!lock.tryLock(0))
        return;
    QJsonObject pastes;
    auto        matches = chip.globalMatch(display);
    while (matches.hasNext()) {
        const auto match = matches.next();
        const int  id    = match.captured(1).toInt();
        if (pastes_.contains(id))
            pastes.insert(match.captured(1), pastes_.value(id));
    }
    const auto bytes
        = QJsonDocument(
              QJsonObject{
                  {"display", display}, {"pastes", pastes}, {"project", project}, {"host", host}})
              .toJson(QJsonDocument::Compact)
          + '\n';
    QFile file(filePath);
    if (!file.open(QIODevice::Append))
        return;
    file.write(bytes);
    file.close();
    if (QFileInfo(filePath).size() <= maxBytes || !file.open(QIODevice::ReadOnly))
        return;
    file.seek(qMax<qint64>(0, file.size() - maxBytes * 3 / 4));
    file.readLine();
    const auto recent = file.readAll();
    file.close();
    QSaveFile trimmed(filePath);
    if (trimmed.open(QIODevice::WriteOnly) && trimmed.write(recent) == recent.size())
        trimmed.commit();
}
