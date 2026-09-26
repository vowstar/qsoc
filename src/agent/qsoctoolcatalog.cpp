// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsoctoolcatalog.h"
#include "agent/qsocrequestusage.h"

#include <algorithm>
#include <QCryptographicHash>

namespace {

json catalogDefinition()
{
    return {
        {"type", "function"},
        {"function",
         {{"name", "tool_catalog"},
          {"description",
           "Find authorized specialized tools. Search by name or description, then describe an "
           "exact name to obtain its schema and schema_version."},
          {"parameters",
           {{"type", "object"},
            {"additionalProperties", false},
            {"properties",
             {{"operation", {{"type", "string"}, {"enum", json::array({"search", "describe"})}}},
              {"query", {{"type", "string"}, {"description", "Search words, required for search."}}},
              {"name",
               {{"type", "string"}, {"description", "Exact tool name, required for describe."}}}}},
            {"required", json::array({"operation"})}}}}}};
}

json invokeDefinition()
{
    return {
        {"type", "function"},
        {"function",
         {{"name", "tool_invoke"},
          {"description",
           "Call an authorized tool using its current catalog schema_version. arguments_json must "
           "encode an object matching that tool's described parameters."},
          {"parameters",
           {{"type", "object"},
            {"additionalProperties", false},
            {"properties",
             {{"name", {{"type", "string"}}},
              {"schema_version", {{"type", "string"}}},
              {"arguments_json", {{"type", "string"}}}}},
            {"required", json::array({"name", "schema_version", "arguments_json"})}}}}}};
}

bool hasOnly(const json &arguments, const QSet<QString> &fields)
{
    if (!arguments.is_object()) {
        return false;
    }
    for (auto item = arguments.begin(); item != arguments.end(); ++item) {
        if (!fields.contains(QString::fromStdString(item.key()))) {
            return false;
        }
    }
    return true;
}

QString stringField(const json &arguments, const char *name)
{
    const auto field = arguments.find(name);
    return field != arguments.end() && field->is_string()
               ? QString::fromStdString(field->get<std::string>())
               : QString();
}

QString checkDepth(const QByteArray &bytes)
{
    int  depth   = 0;
    bool quoted  = false;
    bool escaped = false;
    for (const char character : bytes) {
        if (quoted) {
            if (escaped) {
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if (character == '"') {
                quoted = false;
            }
            continue;
        }
        if (character == '"') {
            quoted = true;
        } else if (character == '{' || character == '[') {
            if (++depth > QSocToolCatalog::argumentDepth) {
                return QStringLiteral("Error: tool arguments exceed the JSON depth limit");
            }
        } else if (character == '}' || character == ']') {
            --depth;
        }
    }
    return {};
}

} // namespace

QSocToolCatalog::QSocToolCatalog(json allowedDefinitions, const QString &binding)
    : definitions_(std::move(allowedDefinitions))
{
    for (const auto &definition : definitions_) {
        const auto &function = definition.at("function");
        byName_.insert(QString::fromStdString(function.at("name").get<std::string>()), definition);
    }
    const auto data = binding.toUtf8() + '\0' + QByteArray::fromStdString(definitions_.dump());
    version_        = QString::fromLatin1(
        QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

bool QSocToolCatalog::alwaysDirect(const QString &name)
{
    static const QSet<QString> names
        = {"read_file",
           "write_file",
           "edit_file",
           "list_files",
           "bash",
           "remote_shell_bash",
           "bash_manage",
           "path_context",
           "ask_user",
           "goal_complete",
           "enter_plan_mode",
           "exit_plan_mode",
           "todo_list",
           "todo_add",
           "todo_update",
           "todo_delete",
           "tool_output_read"};
    return names.contains(name);
}

bool QSocToolCatalog::reserved(const QString &name)
{
    return name == "tool_catalog" || name == "tool_invoke";
}

bool QSocToolCatalog::contains(const QString &name) const
{
    return !reserved(name) && byName_.contains(name);
}

QString QSocToolCatalog::resolvedMode(const QString &policy) const
{
    if (policy != "auto") {
        return policy == "catalog" ? QStringLiteral("catalog") : QStringLiteral("direct");
    }
    json specialized = json::array();
    for (const auto &definition : definitions_) {
        const auto name = QString::fromStdString(
            definition.at("function").at("name").get<std::string>());
        if (!alwaysDirect(name)) {
            specialized.push_back(definition);
        }
    }
    return QSocRequestUsage::estimateText(QString::fromStdString(specialized.dump()))
                   > automaticThreshold
               ? QStringLiteral("catalog")
               : QStringLiteral("direct");
}

json QSocToolCatalog::wireDefinitions(const QString &policy) const
{
    if (resolvedMode(policy) == "direct") {
        return definitions_;
    }
    json result = json::array();
    for (const auto &definition : definitions_) {
        const auto name = QString::fromStdString(
            definition.at("function").at("name").get<std::string>());
        if (alwaysDirect(name)) {
            result.push_back(definition);
        }
    }
    result.push_back(catalogDefinition());
    result.push_back(invokeDefinition());
    return result;
}

QString QSocToolCatalog::query(const json &arguments) const
{
    if (!hasOnly(arguments, {"operation", "query", "name"})) {
        return QStringLiteral("Error: invalid catalog arguments");
    }
    const auto operation = stringField(arguments, "operation");
    if (operation == "describe") {
        const auto name = stringField(arguments, "name");
        if (arguments.contains("query") || !contains(name)) {
            return QStringLiteral("Error: requested tool is not available");
        }
        return QString::fromStdString(
            json({{"schema_version", version_.toStdString()}, {"definition", byName_.value(name)}})
                .dump());
    }
    const auto text = stringField(arguments, "query").trimmed();
    if (operation != "search" || arguments.contains("name") || text.isEmpty() || text.size() > 256) {
        return QStringLiteral("Error: search requires a query of 1 to 256 characters");
    }
    struct Match
    {
        int     score;
        QString name;
        QString description;
    };
    QVector<Match> matches;
    for (auto item = byName_.begin(); item != byName_.end(); ++item) {
        const auto description = QString::fromStdString(
            item.value().at("function").at("description").get<std::string>());
        const int score = item.key().compare(text, Qt::CaseInsensitive) == 0 ? 3
                          : item.key().contains(text, Qt::CaseInsensitive)   ? 2
                          : description.contains(text, Qt::CaseInsensitive)  ? 1
                                                                             : 0;
        if (score > 0 && !reserved(item.key())) {
            matches.append({score, item.key(), description});
        }
    }
    std::sort(matches.begin(), matches.end(), [](const Match &left, const Match &right) {
        return left.score != right.score ? left.score > right.score : left.name < right.name;
    });
    json results = json::array();
    for (qsizetype i = 0; i < qMin<qsizetype>(20, matches.size()); ++i) {
        const auto &match = matches[i];
        results.push_back(
            {{"name", match.name.toStdString()},
             {"description", match.description.left(160).toStdString()}});
    }
    return QString::fromStdString(json({{"schema_version", version_.toStdString()},
                                        {"tools", results},
                                        {"truncated", matches.size() > 20}})
                                      .dump());
}

QString QSocToolCatalog::parseArguments(const QString &encoded, json *arguments, qsizetype consumed)
{
    const auto bytes = encoded.toUtf8();
    if (consumed < 0 || bytes.size() > argumentBytes - consumed) {
        return QStringLiteral("Error: tool arguments exceed the combined byte limit");
    }
    const auto error = checkDepth(bytes);
    if (!error.isEmpty()) {
        return error;
    }
    *arguments = json::parse(bytes.constData(), bytes.constData() + bytes.size(), nullptr, false);
    return arguments->is_object() ? QString()
                                  : QStringLiteral("Error: tool arguments must be a JSON object");
}

QString QSocToolCatalog::validateArguments(const json &arguments, qsizetype consumed)
{
    if (!arguments.is_object()) {
        return QStringLiteral("Error: tool arguments must be a JSON object");
    }
    const auto encoded = QByteArray::fromStdString(arguments.dump());
    if (consumed < 0 || encoded.size() > argumentBytes - consumed) {
        return QStringLiteral("Error: tool arguments exceed the byte limit");
    }
    return checkDepth(encoded);
}

QString QSocToolCatalog::unwrap(
    const json &arguments, QSocToolDispatchView *view, qsizetype outerBytes) const
{
    if (!hasOnly(arguments, {"name", "schema_version", "arguments_json"})) {
        return QStringLiteral("Error: invalid dispatcher arguments");
    }
    const auto name = stringField(arguments, "name");
    if (!contains(name)) {
        return QStringLiteral("Error: requested tool is not available");
    }
    if (stringField(arguments, "schema_version") != version_) {
        return QStringLiteral("Error: tool schema changed; describe the tool again");
    }
    const auto encoded = arguments.find("arguments_json");
    if (encoded == arguments.end() || !encoded->is_string()) {
        return QStringLiteral("Error: arguments_json must encode a JSON object");
    }
    const auto error = parseArguments(
        QString::fromStdString(encoded->get<std::string>()), &view->finalArguments, outerBytes);
    if (!error.isEmpty()) {
        return error;
    }
    view->canonicalName = name;
    view->schemaVersion = version_;
    return {};
}
