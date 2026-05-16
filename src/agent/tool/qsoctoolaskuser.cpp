// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolaskuser.h"

#include <utility>

namespace {

QString jsonString(const json &args, const char *key)
{
    if (!args.contains(key) || !args[key].is_string()) {
        return {};
    }
    return QString::fromStdString(args[key].get<std::string>());
}

} // namespace

QSocToolAskUser::QSocToolAskUser(QObject *parent, Callback callback)
    : QSocTool(parent)
    , callback_(std::move(callback))
{}

void QSocToolAskUser::setCallback(Callback callback)
{
    callback_ = std::move(callback);
}

QString QSocToolAskUser::getName() const
{
    return QStringLiteral("ask_user");
}

QString QSocToolAskUser::getDescription() const
{
    return QStringLiteral(
        "Ask the user a question with 2-4 mutually exclusive options. Use when the "
        "user's intent is ambiguous, when a tool call was denied without an obvious "
        "reason, or when a non-trivial design choice needs human approval. Prefer "
        "this over silently picking one interpretation. The tool automatically "
        "appends an 'Other...' entry that opens a free-form text input, so the "
        "caller never needs to include such an option. The result is returned as "
        "either the picked option label or, when 'Other...' is chosen, the typed "
        "text. This tool is not available inside sub-agent runs because a child "
        "must not block mid-LLM-turn waiting on the user; surface the ambiguity to "
        "the parent first.");
}

json QSocToolAskUser::getParametersSchema() const
{
    return json{
        {"type", "object"},
        {"properties",
         {{"question",
           {{"type", "string"}, {"description", "Complete question text shown to the user."}}},
          {"header",
           {{"type", "string"},
            {"description",
             "Optional very short label (<=12 chars) shown as a chip / tag "
             "above the menu."}}},
          {"options",
           {{"type", "array"},
            {"minItems", 2},
            {"maxItems", 4},
            {"description",
             "2-4 mutually exclusive options. Do NOT include an 'Other' "
             "entry; the tool appends one automatically that opens a "
             "free-form text input."},
            {"items",
             {{"type", "object"},
              {"properties",
               {{"label", {{"type", "string"}, {"description", "Short text shown on the menu row."}}},
                {"description",
                 {{"type", "string"},
                  {"description", "Optional one-line hint shown next to the label."}}}}},
              {"required", json::array({"label"})}}}}}}},
        {"required", json::array({"question", "options"})}};
}

QString QSocToolAskUser::execute(const json &arguments)
{
    if (!callback_) {
        return QStringLiteral(
            R"({"status":"error","error":"ask_user is unavailable in sub-agent context; resolve ambiguity in the parent before dispatching"})");
    }
    const QString question = jsonString(arguments, "question");
    if (question.isEmpty()) {
        return QStringLiteral(R"({"status":"error","error":"question is required"})");
    }
    if (!arguments.contains("options") || !arguments["options"].is_array()) {
        return QStringLiteral(R"({"status":"error","error":"options must be a 2-4 entry array"})");
    }
    const auto &optionsJson = arguments["options"];
    if (optionsJson.size() < 2 || optionsJson.size() > 4) {
        return QStringLiteral(
            R"({"status":"error","error":"options must contain between 2 and 4 entries"})");
    }
    QList<QSocAskUserOption> options;
    options.reserve(static_cast<int>(optionsJson.size()));
    for (const auto &entry : optionsJson) {
        if (!entry.is_object() || !entry.contains("label") || !entry["label"].is_string()) {
            return QStringLiteral(
                R"({"status":"error","error":"each option needs a label string"})");
        }
        QSocAskUserOption option;
        option.label = QString::fromStdString(entry["label"].get<std::string>());
        if (entry.contains("description") && entry["description"].is_string()) {
            option.description = QString::fromStdString(entry["description"].get<std::string>());
        }
        if (option.label.compare(QStringLiteral("Other"), Qt::CaseInsensitive) == 0
            || option.label.compare(QStringLiteral("Other..."), Qt::CaseInsensitive) == 0) {
            return QStringLiteral(
                R"({"status":"error","error":"'Other' is appended automatically; do not list it"})");
        }
        options.append(option);
    }

    const QString header = jsonString(arguments, "header");
    const auto    result = callback_(question, header, options);

    if (result.canceled) {
        return QStringLiteral(R"({"status":"canceled","reason":"user canceled the prompt"})");
    }
    if (result.choice == QStringLiteral("Other")) {
        const json payload
            = json{{"status", "ok"}, {"choice", "Other"}, {"text", result.text.toStdString()}};
        return QString::fromStdString(payload.dump());
    }
    const json payload = json{{"status", "ok"}, {"choice", result.choice.toStdString()}};
    return QString::fromStdString(payload.dump());
}
