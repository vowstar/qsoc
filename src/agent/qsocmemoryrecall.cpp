// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocmemoryrecall.h"

#include <nlohmann/json.hpp>
#include <QRegularExpression>

using json = nlohmann::json;

QSocMemoryRecall::QSocMemoryRecall(Config config)
    : config_(config)
{}

bool QSocMemoryRecall::queryIsRecallable(const QString &query)
{
    const QStringList tokens = query.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    return tokens.size() >= 2;
}

QString QSocMemoryRecall::selectorSystemPrompt()
{
    return QStringLiteral(
        "You select persistent memories that will help process the user's query. "
        "You are given the query and a list of memory files with name, type, age, "
        "and description. Return JSON {\"selected_memories\":[name,...]} listing only "
        "memories clearly useful for this query. Be selective: when unsure, omit it. "
        "An empty list is fine when nothing clearly applies. Output JSON only.");
}

QString QSocMemoryRecall::buildSelectorPrompt(
    const QList<QSocMemoryManager::MemoryHeader> &headers, const QString &query) const
{
    QString manifest;
    for (const auto &header : headers) {
        const QString type = header.type.isEmpty() ? QStringLiteral("note") : header.type;
        const QString desc = header.description.isEmpty() ? header.name : header.description;
        manifest += QStringLiteral("- [%1] %2 (%3): %4\n")
                        .arg(type, header.name, freshnessPhrase(header.ageDays), desc);
    }

    /* Single multi-arg call so a literal %N in query/manifest is not
     * re-substituted by a following .arg. */
    return QStringLiteral("Query: %1\n\nAvailable memories:\n%2\nSelect up to %3 names.")
        .arg(query, manifest, QString::number(config_.maxFiles));
}

QStringList QSocMemoryRecall::parseSelection(const QString &responseContent)
{
    QStringList names;

    /* Isolate the JSON payload: tolerate code fences or surrounding prose
     * by taking the span between the first brace/bracket and its last
     * matching close. */
    const QString trimmed = responseContent.trimmed();
    qsizetype     start   = trimmed.indexOf('{');
    qsizetype     end     = trimmed.lastIndexOf('}');
    QString       payload;
    if (start >= 0 && end > start) {
        payload = trimmed.mid(start, end - start + 1);
    } else {
        /* Fall back to a bare array form: ["a","b"]. */
        start = trimmed.indexOf('[');
        end   = trimmed.lastIndexOf(']');
        if (start >= 0 && end > start) {
            payload = QStringLiteral("{\"selected_memories\":%1}")
                          .arg(trimmed.mid(start, end - start + 1));
        }
    }

    if (payload.isEmpty()) {
        return names;
    }

    try {
        const json parsed = json::parse(payload.toStdString());
        if (parsed.contains("selected_memories") && parsed["selected_memories"].is_array()) {
            for (const auto &item : parsed["selected_memories"]) {
                if (item.is_string()) {
                    names.append(QString::fromStdString(item.get<std::string>()));
                }
            }
        }
    } catch (const json::exception &) {
        /* Malformed selector reply: surface nothing rather than guess. */
    }

    return names;
}

QString QSocMemoryRecall::stripFrontmatter(const QString &content)
{
    if (!content.startsWith("---")) {
        return content;
    }
    const qsizetype endMarker = content.indexOf("\n---", 3);
    if (endMarker < 0) {
        return content;
    }
    /* Only strip when the block is real frontmatter (has a `key:` line), so
     * a body opening with a `---` rule is not truncated. */
    static const QRegularExpression keyLine(QStringLiteral("(^|\\n)\\s*[A-Za-z0-9_-]+\\s*:"));
    if (!content.left(endMarker).contains(keyLine)) {
        return content;
    }
    /* Skip past the closing "---" line and any following blank line. */
    const qsizetype bodyStart = content.indexOf('\n', endMarker + 1);
    if (bodyStart < 0) {
        return QString();
    }
    return content.mid(bodyStart + 1).trimmed();
}

QString QSocMemoryRecall::assembleBlock(
    const QList<QSocMemoryManager::MemoryHeader>                            &selected,
    const std::function<QString(const QString &scope, const QString &name)> &reader) const
{
    if (selected.isEmpty() || !reader) {
        return QString();
    }

    QString body;
    int     injected  = 0;
    int     usedBytes = 0;

    for (const auto &header : selected) {
        if (injected >= config_.maxFiles) {
            break;
        }

        QString fileBody = stripFrontmatter(reader(header.scope, header.name));
        if (fileBody.isEmpty()) {
            continue;
        }

        /* Per-file cap (bytes), trimmed to a line boundary when possible. */
        QByteArray utf8 = fileBody.toUtf8();
        if (utf8.size() > config_.perFileCap) {
            fileBody = QString::fromUtf8(utf8.left(config_.perFileCap));
            /* A byte-offset cut can split a multibyte char; Qt decodes each
             * dangling byte to a U+FFFD (up to 3), so drop all trailing
             * replacement chars. */
            while (fileBody.endsWith(QChar(QChar::ReplacementCharacter))) {
                fileBody.chop(1);
            }
            /* Compare in char units (fileBody is a QString): perFileCap is
             * bytes, so for multibyte content cap/2 bytes never matches a
             * char index and the line trim would never fire. */
            const qsizetype lastNewline = fileBody.lastIndexOf('\n');
            if (lastNewline > fileBody.size() / 2) {
                fileBody.truncate(lastNewline);
            }
            fileBody += QStringLiteral("\n(truncated)");
        }

        const QString entry = QStringLiteral("## %1 (%2)\n%3\n\n")
                                  .arg(header.name, freshnessPhrase(header.ageDays), fileBody);

        const int entryBytes = static_cast<int>(entry.toUtf8().size());
        if (usedBytes + entryBytes > config_.turnBudget) {
            /* Skip an oversized entry rather than aborting the loop, so
             * smaller later entries still get a chance under the budget. */
            continue;
        }

        body += entry;
        usedBytes += entryBytes;
        injected++;
    }

    if (body.isEmpty()) {
        return QString();
    }

    return QStringLiteral(
               "<recalled_memory>\n"
               "Persistent memory selected as relevant to your current task. These are "
               "point-in-time notes, not live state; verify stale claims against current "
               "code before relying on them.\n\n%1"
               "</recalled_memory>")
        .arg(body);
}

QString QSocMemoryRecall::freshnessPhrase(int ageDays)
{
    if (ageDays <= 0) {
        return QStringLiteral("updated today");
    }
    if (ageDays == 1) {
        return QStringLiteral("updated yesterday");
    }
    return QStringLiteral("updated %1 days ago").arg(ageDays);
}
