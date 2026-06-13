// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsoccontextrestore.h"

QStringList QSocContextRestore::readPaths() const
{
    QStringList out;
    for (const FileItem &item : files) {
        if (item.mode == Mode::Read) {
            out.append(item.displayPath);
        }
    }
    return out;
}

QStringList QSocContextRestore::referencedPaths() const
{
    QStringList out;
    for (const FileItem &item : files) {
        if (item.mode == Mode::Referenced) {
            out.append(item.displayPath);
        }
    }
    return out;
}

QStringList QSocContextRestore::skillNames() const
{
    QStringList out;
    for (const SkillItem &item : skills) {
        out.append(item.name);
    }
    return out;
}

QStringList QSocContextRestore::agentLabels() const
{
    QStringList out;
    for (const AgentItem &item : agents) {
        out.append(item.label);
    }
    return out;
}

QString QSocContextRestoreBuilder::truncateToTokens(
    const QString &text, int maxTokens, const std::function<int(const QString &)> &estimate)
{
    if (maxTokens <= 0 || !estimate || estimate(text) <= maxTokens) {
        return text;
    }
    static const QString marker = QStringLiteral("\n...(truncated)");
    /* Proportional first cut, then shrink until the marked text fits. */
    const int total = qMax(1, estimate(text));
    int     chars = qMax(1, static_cast<int>(static_cast<qint64>(text.size()) * maxTokens / total));
    QString out   = text.left(chars);
    while (out.size() > 0 && estimate(out + marker) > maxTokens) {
        out.chop(qMax(1, out.size() / 10));
    }
    return out + marker;
}

QSocContextRestore QSocContextRestoreBuilder::build(const Inputs &inputs)
{
    QSocContextRestore restore;
    if (!inputs.enabled || !inputs.estimateTokens) {
        return restore;
    }

    /* Files: most-recent first, skip excluded, cap at maxFiles, re-read
     * each, classify Read vs Referenced by the per-file token cap, and
     * keep within the total file budget. */
    int fileTokens = 0;
    int picked     = 0;
    for (const QString &path : inputs.candidatePaths) {
        if (picked >= inputs.maxFiles) {
            break;
        }
        if (inputs.excludedPaths.contains(path)) {
            continue;
        }
        std::optional<QString> content = inputs.readFile ? inputs.readFile(path) : std::nullopt;
        if (!content.has_value()) {
            continue; /* unreadable: skip, do not consume a slot */
        }
        ++picked;

        QSocContextRestore::FileItem item;
        item.displayPath = path;
        if (inputs.estimateTokens(*content) > inputs.maxTokensPerFile) {
            item.mode = QSocContextRestore::Mode::Referenced;
            item.attachmentText
                = QStringLiteral(
                      "[Referenced file after compaction: %1 (too large to re-inline; "
                      "use read_file if its content is needed)]")
                      .arg(path);
        } else {
            item.mode  = QSocContextRestore::Mode::Read;
            item.lines = static_cast<int>(content->count(QLatin1Char('\n')));
            if (!content->isEmpty() && !content->endsWith(QLatin1Char('\n'))) {
                ++item.lines;
            }
            item.attachmentText = QStringLiteral(
                                      "[Restored file after compaction: %1 (%2 lines)]\n%3")
                                      .arg(path)
                                      .arg(item.lines)
                                      .arg(*content);
        }
        const int itemTokens = inputs.estimateTokens(item.attachmentText);
        if (fileTokens + itemTokens > inputs.fileBudget) {
            continue; /* over budget: drop this one, a later smaller one may fit */
        }
        fileTokens += itemTokens;
        restore.files.append(item);
    }

    /* Skills: most-recent first, body truncated per skill, within budget. */
    int skillTokens = 0;
    for (const QString &name : inputs.skillNames) {
        std::optional<QString> body = inputs.readSkill ? inputs.readSkill(name) : std::nullopt;
        if (!body.has_value()) {
            continue;
        }
        const QString truncated
            = truncateToTokens(*body, inputs.maxTokensPerSkill, inputs.estimateTokens);
        QSocContextRestore::SkillItem item;
        item.name            = name;
        item.attachmentText  = QStringLiteral("## %1\n%2").arg(name, truncated);
        const int itemTokens = inputs.estimateTokens(item.attachmentText);
        if (skillTokens + itemTokens > inputs.skillsBudget) {
            continue;
        }
        skillTokens += itemTokens;
        restore.skills.append(item);
    }

    /* Running background agents. */
    for (const AgentRow &row : inputs.agents) {
        QSocContextRestore::AgentItem item;
        item.id             = row.id;
        item.label          = row.label;
        item.attachmentText = QStringLiteral("- %1 (%2): %3").arg(row.label, row.id, row.summary);
        restore.agents.append(item);
    }

    return restore;
}

json QSocContextRestoreBuilder::toMessages(const QSocContextRestore &restore)
{
    json messages = json::array();
    if (restore.isEmpty()) {
        return messages;
    }

    for (const QSocContextRestore::FileItem &item : restore.files) {
        messages.push_back({{"role", "user"}, {"content", item.attachmentText.toStdString()}});
    }

    if (!restore.skills.isEmpty()) {
        QString block = QStringLiteral("[Skills restored after compaction]\n");
        for (const QSocContextRestore::SkillItem &item : restore.skills) {
            block += item.attachmentText + QLatin1Char('\n');
        }
        messages.push_back({{"role", "user"}, {"content", block.toStdString()}});
    }

    if (!restore.agents.isEmpty()) {
        QString block = QStringLiteral("[Background agents still running after compaction]\n");
        for (const QSocContextRestore::AgentItem &item : restore.agents) {
            block += item.attachmentText + QLatin1Char('\n');
        }
        messages.push_back({{"role", "user"}, {"content", block.toStdString()}});
    }

    return messages;
}
