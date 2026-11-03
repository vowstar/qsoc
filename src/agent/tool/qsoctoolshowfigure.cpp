// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolshowfigure.h"

#include "common/qsocimageattach.h"
#include "common/qsocsvg.h"

namespace {
constexpr int kSvgMaxLines = 100000;
}

QSocToolShowFigure::QSocToolShowFigure(QObject *parent, QSocTool *reader, QLLMService *llm)
    : QSocTool(parent)
    , reader_(reader)
    , llm_(llm)
{}

QString QSocToolShowFigure::getName() const
{
    return QStringLiteral("show_figure");
}

QString QSocToolShowFigure::getDescription() const
{
    return QStringLiteral(
        "Show an image file (PNG, JPG, GIF, WebP, SVG) or inline SVG source to the user in the "
        "terminal and keep it in the session history. The pixels are not sent to you unless "
        "inspect is true.");
}

json QSocToolShowFigure::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"file_path", {{"type", "string"}, {"description", "Path to the image file"}}},
          {"svg", {{"type", "string"}, {"description", "SVG source, instead of file_path"}}},
          {"inspect",
           {{"type", "boolean"},
            {"description", "Also send the drawn image to you, on a model that accepts images"}}},
          {"title", {{"type", "string"}, {"description", "Short label shown above the image"}}}}},
    };
}

namespace {

QString showSvg(const QByteArray &source, const QString &label, bool inspect, QLLMService *llm)
{
    QString error;
    if (QSocSvg::sanitize(source, false, &error).isEmpty())
        return QStringLiteral("Error: %1").arg(error);
    if (inspect) {
        const QByteArray png = QSocSvg::drawForModel(source);
        if (png.isEmpty())
            return QStringLiteral("Error: SVG could not be drawn");
        return QSocImageAttach::buildAttachmentResult(
            label, QStringLiteral("image/png"), png, llm, true);
    }
    return QStringLiteral("Sent to the user's terminal: %1. The pixels were not sent to you.\n")
               .arg(label)
           + QSocImageAttach::wrapAttachmentPayload(
               {{"mime", QSocSvg::mimeType()},
                {"data", source.toBase64().toStdString()},
                {"source_url", label.toStdString()},
                {"byte_size", source.size()},
                {"display_only", true}});
}

} // namespace

QString QSocToolShowFigure::execute(const json &arguments)
{
    const QString title   = arguments.contains("title") && arguments["title"].is_string()
                                ? QString::fromStdString(arguments["title"].get<std::string>())
                                : QString();
    const bool    inspect = arguments.contains("inspect") && arguments["inspect"].is_boolean()
                            && arguments["inspect"].get<bool>();
    if (arguments.contains("svg") && arguments["svg"].is_string()) {
        return showSvg(
            QByteArray::fromStdString(arguments["svg"].get<std::string>()),
            title.isEmpty() ? QStringLiteral("svg") : title,
            inspect,
            llm_);
    }
    if (!arguments.contains("file_path") || !arguments["file_path"].is_string())
        return QStringLiteral("Error: file_path or svg is required");
    if (reader_.isNull())
        return QStringLiteral("Error: file reading is not available");
    const QString filePath = QString::fromStdString(arguments["file_path"].get<std::string>());
    const bool    isSvg    = filePath.endsWith(QStringLiteral(".svg"), Qt::CaseInsensitive);
    const QString result   = reader_->execute(
        {{"file_path", arguments["file_path"]},
         {"display_only", isSvg || !inspect},
         {"max_lines", kSvgMaxLines}});
    if (result.startsWith(QStringLiteral("Error:")))
        return result;
    if (isSvg)
        return showSvg(result.toUtf8(), title.isEmpty() ? filePath : title, inspect, llm_);

    if (inspect)
        return result;
    auto payload = QSocImageAttach::attachmentPayload(result);
    if (!payload.is_object())
        return QStringLiteral("Error: not a displayable image: %1").arg(filePath);
    if (!title.isEmpty())
        payload["source_url"] = title.toStdString();
    return QStringLiteral(
               "Sent to the user's terminal: %1 (%2x%3). The pixels were not sent to you.\n")
               .arg(filePath)
               .arg(payload.value("width", 0))
               .arg(payload.value("height", 0))
           + QSocImageAttach::wrapAttachmentPayload(payload);
}

#include "moc_qsoctoolshowfigure.cpp"
