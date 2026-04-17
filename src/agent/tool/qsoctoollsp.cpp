// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoollsp.h"
#include "common/qlspservice.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QUrl>

QSocToolLsp::QSocToolLsp(QObject *parent)
    : QSocTool(parent)
{}

QSocToolLsp::~QSocToolLsp() = default;

QString QSocToolLsp::getName() const
{
    return "lsp";
}

QString QSocToolLsp::getDescription() const
{
    return "Code intelligence via Language Server Protocol. "
           "Provides diagnostics (errors/warnings), go-to-definition, hover info, "
           "find references, and document symbols for HDL files. "
           "Operations: diagnostics, definition, hover, references, symbols. "
           "The diagnostics operation only requires file_path. "
           "All other operations require file_path, line (1-based), and character (1-based).";
}

json QSocToolLsp::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"operation",
           {{"type", "string"},
            {"enum", json::array({"diagnostics", "definition", "hover", "references", "symbols"})},
            {"description", "The LSP operation to perform"}}},
          {"file_path",
           {{"type", "string"}, {"description", "Path to the file (absolute or relative)"}}},
          {"line", {{"type", "integer"}, {"description", "Line number, 1-based"}}},
          {"character", {{"type", "integer"}, {"description", "Column number, 1-based"}}}}},
        {"required", json::array({"operation", "file_path"})}};
}

QString QSocToolLsp::execute(const json &arguments)
{
    if (!arguments.contains("operation") || !arguments["operation"].is_string())
        return "Error: operation is required";
    if (!arguments.contains("file_path") || !arguments["file_path"].is_string())
        return "Error: file_path is required";

    QString operation = QString::fromStdString(arguments["operation"].get<std::string>());
    QString filePath  = QString::fromStdString(arguments["file_path"].get<std::string>());

    /* Make path absolute. */
    QFileInfo fileInfo(filePath);
    if (!fileInfo.isAbsolute())
        filePath = QDir::currentPath() + "/" + filePath;
    filePath = QFileInfo(filePath).absoluteFilePath();

    if (!QFileInfo::exists(filePath))
        return QString("Error: file does not exist: %1").arg(filePath);

    QLspService *service = QLspService::instance();
    if (!service->isAvailable())
        return "Error: no LSP backend available for this file type";

    if (operation == "diagnostics")
        return formatDiagnostics(filePath);

    /* All other operations need line and character. */
    if (!arguments.contains("line") || !arguments["line"].is_number_integer())
        return "Error: line is required for this operation";
    if (!arguments.contains("character") || !arguments["character"].is_number_integer())
        return "Error: character is required for this operation";

    int line      = arguments["line"].get<int>();
    int character = arguments["character"].get<int>();

    if (line < 1 || character < 1)
        return "Error: line and character must be positive (1-based)";

    if (operation == "definition")
        return formatDefinition(filePath, line, character);
    if (operation == "hover")
        return formatHover(filePath, line, character);
    if (operation == "references")
        return formatReferences(filePath, line, character);
    if (operation == "symbols")
        return formatDocumentSymbol(filePath);

    return QString("Error: unknown operation: %1").arg(operation);
}

/* Formatting helpers follow the patterns from Claude Code LSPTool formatters. */

static QString severityLabel(int severity)
{
    switch (severity) {
    case 1:
        return "Error";
    case 2:
        return "Warning";
    case 3:
        return "Info";
    case 4:
        return "Hint";
    default:
        return "Unknown";
    }
}

static QString relativePath(const QString &filePath)
{
    QString cwd = QDir::currentPath();
    QString rel = QDir(cwd).relativeFilePath(filePath);
    if (rel.length() < filePath.length() && !rel.startsWith("../../"))
        return rel;
    return filePath;
}

static QString locationString(const QString &uri, const QJsonObject &range)
{
    QString path = QUrl(uri).toLocalFile();
    if (path.isEmpty())
        path = uri;
    path = relativePath(path);

    QJsonObject start = range["start"].toObject();
    int         line  = start["line"].toInt() + 1;
    int         col   = start["character"].toInt() + 1;
    return QString("%1:%2:%3").arg(path).arg(line).arg(col);
}

QString QSocToolLsp::formatDiagnostics(const QString &filePath)
{
    QLspService *service = QLspService::instance();

    /* Trigger file open/reparse if not already tracked. */
    service->didSave(filePath);

    QJsonArray diags = service->diagnostics(filePath);
    if (diags.isEmpty())
        return "No diagnostics found.";

    QStringList lines;
    int         count = 0;
    for (const QJsonValue &val : diags) {
        if (count >= 30)
            break;
        QJsonObject diag     = val.toObject();
        int         severity = diag["severity"].toInt(1);
        QString     message  = diag["message"].toString();
        QJsonObject range    = diag["range"].toObject();
        QJsonObject start    = range["start"].toObject();
        int         line     = start["line"].toInt() + 1;
        int         col      = start["character"].toInt() + 1;
        QString     source   = diag["source"].toString();

        QString prefix = severityLabel(severity);
        if (!source.isEmpty())
            prefix += QString(" [%1]").arg(source);

        lines.append(QString("  %1:%2: %3: %4").arg(line).arg(col).arg(prefix, message));
        count++;
    }

    QString header
        = QString("Found %1 diagnostic(s) in %2:").arg(diags.size()).arg(relativePath(filePath));
    if (diags.size() > 30)
        header += QString(" (showing first 30 of %1)").arg(diags.size());

    return header + "\n" + lines.join("\n");
}

QString QSocToolLsp::formatDefinition(const QString &filePath, int line, int character)
{
    QLspService *service = QLspService::instance();
    QJsonValue   result  = service->definition(filePath, line, character);

    if (result.isNull())
        return "No definition found. The symbol may not be resolved by the LSP backend.";

    QJsonArray locations;
    if (result.isArray()) {
        locations = result.toArray();
    } else if (result.isObject()) {
        locations.append(result);
    }

    if (locations.isEmpty())
        return "No definition found.";

    if (locations.size() == 1) {
        QJsonObject loc = locations[0].toObject();
        /* Handle both Location and LocationLink formats. */
        QString     uri   = loc.contains("targetUri") ? loc["targetUri"].toString()
                                                      : loc["uri"].toString();
        QJsonObject range = loc.contains("targetSelectionRange")
                                ? loc["targetSelectionRange"].toObject()
                                : loc["range"].toObject();
        return QString("Defined in %1").arg(locationString(uri, range));
    }

    QStringList lines;
    lines.append(QString("Found %1 definitions:").arg(locations.size()));
    for (const QJsonValue &val : locations) {
        QJsonObject loc   = val.toObject();
        QString     uri   = loc.contains("targetUri") ? loc["targetUri"].toString()
                                                      : loc["uri"].toString();
        QJsonObject range = loc.contains("targetSelectionRange")
                                ? loc["targetSelectionRange"].toObject()
                                : loc["range"].toObject();
        lines.append(QString("  %1").arg(locationString(uri, range)));
    }
    return lines.join("\n");
}

QString QSocToolLsp::formatHover(const QString &filePath, int line, int character)
{
    QLspService *service = QLspService::instance();
    QJsonValue   result  = service->hover(filePath, line, character);

    if (result.isNull() || !result.isObject())
        return "No hover information available.";

    QJsonObject hover    = result.toObject();
    QJsonValue  contents = hover["contents"];

    QString text;
    if (contents.isObject()) {
        /* MarkupContent: { kind, value } */
        text = contents.toObject()["value"].toString();
    } else if (contents.isString()) {
        text = contents.toString();
    } else if (contents.isArray()) {
        /* MarkedString[] */
        QStringList parts;
        for (const QJsonValue &item : contents.toArray()) {
            if (item.isString())
                parts.append(item.toString());
            else if (item.isObject())
                parts.append(item.toObject()["value"].toString());
        }
        text = parts.join("\n\n");
    }

    if (text.isEmpty())
        return "No hover information available.";

    return QString("Hover info at %1:%2:\n\n%3").arg(line).arg(character).arg(text);
}

QString QSocToolLsp::formatReferences(const QString &filePath, int line, int character)
{
    QLspService *service   = QLspService::instance();
    QJsonArray   locations = service->references(filePath, line, character);

    if (locations.isEmpty())
        return "No references found.";

    /* Group references by file. */
    QMap<QString, QStringList> grouped;
    for (const QJsonValue &val : locations) {
        QJsonObject loc     = val.toObject();
        QString     uri     = loc["uri"].toString();
        QJsonObject range   = loc["range"].toObject();
        QJsonObject start   = range["start"].toObject();
        int         refLine = start["line"].toInt() + 1;
        int         refCol  = start["character"].toInt() + 1;

        QString path = QUrl(uri).toLocalFile();
        if (path.isEmpty())
            path = uri;
        path = relativePath(path);

        grouped[path].append(QString("  Line %1:%2").arg(refLine).arg(refCol));
    }

    int         fileCount = grouped.size();
    QStringList lines;
    lines.append(
        QString("Found %1 reference(s) across %2 file(s):").arg(locations.size()).arg(fileCount));

    for (auto it = grouped.constBegin(); it != grouped.constEnd(); ++it) {
        lines.append(QString("\n%1:").arg(it.key()));
        lines.append(it.value());
    }

    return lines.join("\n");
}

static QString symbolKindLabel(int kind)
{
    /* LSP SymbolKind enum values. */
    static const char *names[] = {
        "",         "File",     "Module",        "Namespace",   "Package",    "Class",
        "Method",   "Property", "Field",         "Constructor", "Enum",       "Interface",
        "Function", "Variable", "Constant",      "String",      "Number",     "Boolean",
        "Array",    "Object",   "Key",           "Null",        "EnumMember", "Struct",
        "Event",    "Operator", "TypeParameter",
    };
    if (kind >= 1 && kind <= 26)
        return names[kind];
    return "Unknown";
}

static void formatSymbolNode(
    QStringList &lines, const QJsonObject &symbol, int indent, int &count, int limit)
{
    if (count >= limit)
        return;

    QString name   = symbol["name"].toString();
    int     kind   = symbol["kind"].toInt();
    QString detail = symbol["detail"].toString();

    QJsonObject range = symbol["range"].toObject();
    QJsonObject start = range["start"].toObject();
    int         line  = start["line"].toInt() + 1;

    QString prefix = QString(indent * 2, ' ');
    QString entry  = QString("%1%2 (%3)").arg(prefix, name, symbolKindLabel(kind));
    if (!detail.isEmpty())
        entry += " " + detail;
    entry += QString(" - Line %1").arg(line);

    lines.append(entry);
    count++;

    /* Recurse into children (DocumentSymbol format). */
    if (symbol.contains("children")) {
        QJsonArray children = symbol["children"].toArray();
        for (const QJsonValue &child : children) {
            formatSymbolNode(lines, child.toObject(), indent + 1, count, limit);
        }
    }
}

QString QSocToolLsp::formatDocumentSymbol(const QString &filePath)
{
    QLspService *service = QLspService::instance();
    QJsonArray   symbols = service->documentSymbol(filePath);

    if (symbols.isEmpty())
        return "No symbols found in document.";

    QStringList lines;
    lines.append("Document symbols:");
    int count = 0;
    for (const QJsonValue &val : symbols) {
        QJsonObject sym = val.toObject();
        if (sym.contains("range")) {
            /* DocumentSymbol format (hierarchical). */
            formatSymbolNode(lines, sym, 0, count, 200);
        } else if (sym.contains("location")) {
            /* SymbolInformation format (flat). */
            if (count >= 200)
                break;
            QString     name      = sym["name"].toString();
            int         kind      = sym["kind"].toInt();
            QJsonObject loc       = sym["location"].toObject();
            QJsonObject range     = loc["range"].toObject();
            QJsonObject start     = range["start"].toObject();
            int         line      = start["line"].toInt() + 1;
            QString     container = sym["containerName"].toString();

            QString entry = QString("%1 (%2) - Line %3").arg(name, symbolKindLabel(kind)).arg(line);
            if (!container.isEmpty())
                entry += QString(" in %1").arg(container);
            lines.append(entry);
            count++;
        }
    }

    return lines.join("\n");
}
