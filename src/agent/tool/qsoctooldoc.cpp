// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctooldoc.h"

#include <QFile>
#include <QRegularExpression>
#include <QTextStream>

QSocToolDocQuery::QSocToolDocQuery(QObject *parent)
    : QSocTool(parent)
    , topicMap_(
          {{"about", ":/docs/en/about.typ"},
           {"commands", ":/docs/en/command.typ"},
           {"config", ":/docs/en/config.typ"},
           {"datasheet", ":/docs/en/datasheet.typ"},
           {"bus", ":/docs/en/format_bus.typ"},
           {"clock", ":/docs/en/format_clock.typ"},
           {"fsm", ":/docs/en/format_fsm.typ"},
           {"logic", ":/docs/en/format_logic.typ"},
           {"netlist", ":/docs/en/format_netlist.typ"},
           {"format_overview", ":/docs/en/format_overview.typ"},
           {"power", ":/docs/en/format_power.typ"},
           {"reset", ":/docs/en/format_reset.typ"},
           {"template", ":/docs/en/format_template.typ"},
           {"validation", ":/docs/en/format_validation.typ"},
           {"overview", ":/docs/en/overview.typ"}})
{}

QSocToolDocQuery::~QSocToolDocQuery() = default;

QString QSocToolDocQuery::getName() const
{
    return "query_docs";
}

QString QSocToolDocQuery::getDescription() const
{
    return "Query QSoC documentation by topic. "
           "Available topics: about, commands, config, datasheet, bus, clock, fsm, logic, "
           "netlist, format_overview, power, reset, template, validation, overview.";
}

json QSocToolDocQuery::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"topic",
           {{"type", "string"},
            {"description",
             "Documentation topic to query (e.g., 'netlist', 'bus', 'clock', 'reset', "
             "'commands')"}}},
          {"search",
           {{"type", "string"},
            {"description", "Optional search term to filter content within the topic"}}}}},
        {"required", json::array({"topic"})}};
}

QString QSocToolDocQuery::execute(const json &arguments)
{
    if (!arguments.contains("topic") || !arguments["topic"].is_string()) {
        return QString("Error: topic is required. Available topics: %1")
            .arg(getAvailableTopics().join(", "));
    }

    QString topic = QString::fromStdString(arguments["topic"].get<std::string>()).toLower();

    if (!topicMap_.contains(topic)) {
        return QString("Error: Unknown topic '%1'. Available topics: %2")
            .arg(topic, getAvailableTopics().join(", "));
    }

    QString content = readDocumentation(topicMap_[topic]);
    if (content.isEmpty()) {
        return QString("Error: Failed to read documentation for topic '%1'").arg(topic);
    }

    /* Strip Typst markup */
    content = stripTypstMarkup(content);

    /* Apply search filter if provided */
    if (arguments.contains("search") && arguments["search"].is_string()) {
        QString searchTerm = QString::fromStdString(arguments["search"].get<std::string>());
        if (!searchTerm.isEmpty()) {
            QStringList         lines = content.split('\n');
            QStringList         matchLines;
            constexpr qsizetype contextLines = 3;

            for (qsizetype idx = 0; idx < lines.size(); ++idx) {
                if (lines[idx].contains(searchTerm, Qt::CaseInsensitive)) {
                    /* Add context lines */
                    qsizetype start = qMax(static_cast<qsizetype>(0), idx - contextLines);
                    qsizetype end   = qMin(lines.size() - 1, idx + contextLines);
                    for (qsizetype lineIdx = start; lineIdx <= end; ++lineIdx) {
                        QString line = lines[lineIdx];
                        if (lineIdx == idx) {
                            line = ">>> " + line;
                        }
                        if (!matchLines.contains(line)) {
                            matchLines.append(line);
                        }
                    }
                    matchLines.append("---");
                }
            }

            if (matchLines.isEmpty()) {
                return QString("No matches found for '%1' in topic '%2'").arg(searchTerm, topic);
            }

            content = QString("Search results for '%1' in topic '%2':\n\n%3")
                          .arg(searchTerm, topic, matchLines.join('\n'));
        }
    }

    return QString("Documentation for topic '%1':\n\n%2").arg(topic, content);
}

QStringList QSocToolDocQuery::getAvailableTopics() const
{
    return topicMap_.keys();
}

QString QSocToolDocQuery::readDocumentation(const QString &resourcePath) const
{
    QFile file(resourcePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }

    QTextStream stream(&file);
    QString     content = stream.readAll();
    file.close();

    return content;
}

QString QSocToolDocQuery::stripTypstMarkup(const QString &content) const
{
    QString result = content;

    /* Remove Typst function calls like #function(...) */
    static const QRegularExpression funcCallRegex(R"(#[a-zA-Z_][a-zA-Z0-9_]*\([^)]*\))");
    result.remove(funcCallRegex);

    /* Remove Typst set/show rules */
    static const QRegularExpression setRegex(R"(#set\s+[^\n]+)");
    static const QRegularExpression showRegex(R"(#show\s+[^\n]+)");
    result.remove(setRegex);
    result.remove(showRegex);

    /* Remove import statements */
    static const QRegularExpression importRegex(R"(#import\s+[^\n]+)");
    result.remove(importRegex);

    /* Remove Typst comments */
    static const QRegularExpression lineCommentRegex(R"(//[^\n]*)");
    static const QRegularExpression blockCommentRegex(R"(/\*.*?\*/)");
    result.remove(lineCommentRegex);
    result.remove(blockCommentRegex);

    /* Convert Typst headings to plain text */
    static const QRegularExpression headingRegex(R"(^=+\s*)", QRegularExpression::MultilineOption);
    result.replace(headingRegex, "# ");

    /* Remove emphasis markers */
    static const QRegularExpression boldRegex(R"(\*([^*]+)\*)");
    static const QRegularExpression italicRegex(R"(_([^_]+)_)");
    result.replace(boldRegex, "\\1");
    result.replace(italicRegex, "\\1");

    /* Remove code blocks markers but keep content */
    static const QRegularExpression codeBlockStartRegex(R"(```[a-z]*\n)");
    static const QRegularExpression codeBlockEndRegex(R"(```)");
    result.remove(codeBlockStartRegex);
    result.remove(codeBlockEndRegex);

    /* Clean up inline code */
    static const QRegularExpression inlineCodeRegex(R"(`([^`]+)`)");
    result.replace(inlineCodeRegex, "\\1");

    /* Remove raw blocks */
    static const QRegularExpression rawBlockRegex(R"(#raw\([^)]*\))");
    result.remove(rawBlockRegex);

    /* Remove multiple blank lines */
    static const QRegularExpression multiBlankRegex(R"(\n{3,})");
    result.replace(multiBlankRegex, "\n\n");

    /* Trim whitespace */
    result = result.trimmed();

    return result;
}
