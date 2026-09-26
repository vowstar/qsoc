// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsoccontextrestore.h"
#include "agent/qsoctoolcatalog.h"

namespace {
QString filePathFromCall(const json &call)
{
    if (!call.is_object() || !call.contains("function") || !call["function"].is_object())
        return {};
    const auto &function = call["function"];
    if (!function.contains("name") || !function["name"].is_string()
        || !function.contains("arguments"))
        return {};
    QString name = QString::fromStdString(function["name"].get<std::string>());
    if (name != "read_file" && name != "write_file" && name != "edit_file" && name != "tool_invoke")
        return {};
    const auto   &raw     = function["arguments"];
    const QString encoded = QString::fromStdString(
        raw.is_string() ? raw.get<std::string>() : raw.dump());
    json arguments;
    if (!QSocToolCatalog::parseArguments(encoded, &arguments).isEmpty())
        return {};
    if (name == "tool_invoke") {
        if (arguments.size() != 3 || !arguments.contains("name") || !arguments["name"].is_string()
            || !arguments.contains("schema_version") || !arguments["schema_version"].is_string()
            || !arguments.contains("arguments_json") || !arguments["arguments_json"].is_string())
            return {};
        name             = QString::fromStdString(arguments["name"].get<std::string>());
        const auto inner = QString::fromStdString(arguments["arguments_json"].get<std::string>());
        if (!QSocToolCatalog::parseArguments(inner, &arguments, encoded.toUtf8().size()).isEmpty())
            return {};
    }
    if ((name == "read_file" || name == "write_file" || name == "edit_file")
        && arguments.contains("file_path") && arguments["file_path"].is_string())
        return QString::fromStdString(arguments["file_path"].get<std::string>());
    return {};
}
} // namespace

QSet<QString> QSocContextRestoreBuilder::recentFilePaths(const json &history)
{
    QSet<QString> paths;
    if (!history.is_array())
        return paths;
    for (const auto &message : history) {
        if (!message.is_object() || !message.contains("tool_calls")
            || !message["tool_calls"].is_array())
            continue;
        for (const auto &call : message["tool_calls"]) {
            const auto path = filePathFromCall(call);
            if (!path.isEmpty())
                paths.insert(path);
        }
    }
    return paths;
}

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
    int remaining  = qMax(0, inputs.totalBudget);
    int fileTokens = 0;
    int picked     = 0;
    for (const QString &path : inputs.candidatePaths) {
        if (picked >= inputs.maxFiles || remaining <= 10) {
            break;
        }
        if (inputs.excludedPaths.contains(path)) {
            continue;
        }
        FileRead read;
        if (inputs.readFileBounded) {
            read = inputs.readFileBounded(path);
        } else if (inputs.readFile) {
            const auto content = inputs.readFile(path);
            read.available     = content.has_value();
            read.content       = content.value_or(QString());
        }
        if (!read.available) {
            continue;
        }
        const QString *content = &read.content;
        ++picked;

        QSocContextRestore::FileItem item;
        item.displayPath = path;
        if (read.oversized || inputs.estimateTokens(*content) > inputs.maxTokensPerFile) {
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
        if (itemTokens > inputs.fileBudget - fileTokens || itemTokens > remaining - 10) {
            continue; /* over budget: drop this one, a later smaller one may fit */
        }
        remaining -= itemTokens + 10;
        fileTokens += itemTokens;
        restore.files.append(item);
    }

    /* Skills: most-recent first, body truncated per skill, within budget. */
    int skillTokens = 0;
    for (const QString &name : inputs.skillNames) {
        if (remaining <= 32) {
            break;
        }
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
        if (itemTokens > inputs.skillsBudget - skillTokens || itemTokens > remaining - 32) {
            continue;
        }
        remaining -= itemTokens + 32;
        skillTokens += itemTokens;
        restore.skills.append(item);
    }

    /* Running background agents. */
    for (const AgentRow &row : inputs.agents) {
        if (remaining <= 32) {
            break;
        }
        QSocContextRestore::AgentItem item;
        item.id              = row.id;
        item.label           = row.label;
        item.attachmentText  = QStringLiteral("- %1 (%2): %3").arg(row.label, row.id, row.summary);
        const int itemTokens = inputs.estimateTokens(item.attachmentText);
        if (itemTokens > remaining - 32) {
            continue;
        }
        remaining -= itemTokens + 32;
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
