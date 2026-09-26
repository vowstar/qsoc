// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctooloutputread.h"
#include "agent/qsocagent.h"

QString QSocToolOutputRead::getName() const
{
    return QStringLiteral("tool_output_read");
}
QString QSocToolOutputRead::getDescription() const
{
    return QStringLiteral(
        "Read saved tool return text by artifact_id. Offsets and limits are UTF-8 bytes. "
        "Continue at next_offset until eof. Only this session's artifacts are readable. "
        "Captured text can already have been truncated by its source tool.");
}
json QSocToolOutputRead::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"artifact_id", {{"type", "string"}}},
          {"offset", {{"type", "integer"}, {"minimum", 0}}},
          {"limit", {{"type", "integer"}, {"minimum", 4}, {"maximum", 32768}}}}},
        {"required", json::array({"artifact_id"})}};
}
QString QSocToolOutputRead::execute(const json &arguments)
{
    const auto context = currentCallContext();
    auto      *agent   = context ? qobject_cast<QSocAgent *>(context->executionScope()) : nullptr;
    if (!agent || !agent->toolResultStore())
        return QStringLiteral("Error: result storage is unavailable in this session.");
    QString id;
    qint64  offset = 0;
    qint64  limit  = 32768;
    for (const char *field : {"offset", "limit"}) {
        if (arguments.contains(field) && !arguments.at(field).is_number_integer())
            return QStringLiteral("Error: offset and limit must be integers.");
    }
    try {
        id = QString::fromStdString(arguments.at("artifact_id").get<std::string>());
        if (arguments.contains("offset"))
            offset = arguments.at("offset").get<qint64>();
        if (arguments.contains("limit"))
            limit = arguments.at("limit").get<qint64>();
    } catch (const json::exception &) {
        return QStringLiteral("Error: artifact_id must be a string and offsets must be integers.");
    }
    if (offset < 0 || limit < 4 || limit > 32768)
        return QStringLiteral(
            "Error: offset must be nonnegative and limit must be 4 to 32768 bytes.");
    QString error;
    auto    page = agent->toolResultStore()->read(id, offset, limit, &error);
    if (!page)
        return QStringLiteral("Error: %1").arg(error);
    const auto encoded = [&] {
        json value           = QSocAgent::artifactReferenceJson(page->reference);
        value["offset"]      = page->offset;
        value["next_offset"] = page->nextOffset;
        value["eof"]         = page->eof;
        value["text"]        = page->text.toStdString();
        return QString::fromStdString(value.dump());
    };
    QString      result = encoded();
    const qint64 budget = agent->toolResultBudgetTokens();
    while (QSocRequestUsage::estimateText(result) > budget && !page->text.isEmpty()) {
        const auto bytes = page->text.toUtf8();
        const auto end   = QSocToolResultStore::utf8End(bytes, 0, bytes.size() / 2);
        page->text       = QString::fromUtf8(bytes.first(end));
        page->nextOffset = page->offset + end;
        page->eof        = page->nextOffset == page->reference.capturedBytes;
        result           = encoded();
    }
    if (QSocRequestUsage::estimateText(result) > budget || (!page->eof && page->text.isEmpty())) {
        agent->stopForToolResultBudget();
        return QStringLiteral(
            "Error: remaining context cannot hold an artifact page. Stop this turn.");
    }
    return result;
}
