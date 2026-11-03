// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocsvg.h"

#include "common/qsocsibling.h"

#include <QHash>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

namespace QSocSvg {

namespace {

const QHash<QString, QString> &palette(bool light)
{
    static const QHash<QString, QString> dark
        = {{"fg", "#ebdbb2"},
           {"muted", "#928374"},
           {"border", "#504945"},
           {"surface", "#3c3836"},
           {"accent", "#b16286"},
           {"success", "#98971a"},
           {"warning", "#d79921"},
           {"error", "#cc241d"},
           {"c1", "#458588"},
           {"c2", "#d65d0e"},
           {"c3", "#98971a"},
           {"c4", "#b16286"},
           {"c5", "#689d6a"},
           {"c6", "#d79921"}};
    static const QHash<QString, QString> bright
        = {{"fg", "#3c3836"},
           {"muted", "#7c6f64"},
           {"border", "#bdae93"},
           {"surface", "#ebdbb2"},
           {"accent", "#8f3f71"},
           {"success", "#79740e"},
           {"warning", "#b57614"},
           {"error", "#9d0006"},
           {"c1", "#076678"},
           {"c2", "#af3a03"},
           {"c3", "#79740e"},
           {"c4", "#8f3f71"},
           {"c5", "#427b58"},
           {"c6", "#b57614"}};
    return light ? bright : dark;
}

QString resolveTokens(QString text, bool light)
{
    static const QRegularExpression token(QStringLiteral(R"(var\(\s*--([\w-]+)\s*(?:,[^)]*)?\))"));
    const auto                     &colors = palette(light);
    QString                         out;
    qsizetype                       last = 0;
    auto                            it   = token.globalMatch(text);
    while (it.hasNext()) {
        const auto match = it.next();
        out += QStringView(text).mid(last, match.capturedStart() - last);
        out += colors.value(match.captured(1), colors.value(QStringLiteral("fg")));
        last = match.capturedEnd();
    }
    out += QStringView(text).mid(last);
    return out;
}

bool isAllowedElement(const QString &name)
{
    static const QSet<QString> allowed
        = {"svg",    "g",        "defs",    "title",          "desc",           "style",
           "rect",   "circle",   "ellipse", "line",           "polyline",       "polygon",
           "path",   "text",     "tspan",   "linearGradient", "radialGradient", "stop",
           "marker", "clipPath", "mask",    "pattern",        "symbol",         "use"};
    return allowed.contains(name);
}

/* url(#id) stays. Any other url() or @import reaches outside the document. */
bool leavesDocument(const QString &value)
{
    static const QRegularExpression external(
        QStringLiteral(R"(url\(\s*['"]?\s*(?!#)|@import)"),
        QRegularExpression::CaseInsensitiveOption);
    return external.match(value).hasMatch();
}

} // namespace

const char *mimeType()
{
    return "image/svg+xml";
}

QByteArray sanitize(const QByteArray &source, bool lightBackground, QString *error)
{
    const auto fail = [error](const QString &reason) {
        if (error != nullptr)
            *error = reason;
        return QByteArray();
    };
    if (source.size() > kMaxSourceBytes)
        return fail(QStringLiteral("SVG is larger than 256 KiB"));

    const QString    resolved = resolveTokens(QString::fromUtf8(source), lightBackground);
    QXmlStreamReader reader(resolved);
    QByteArray       output;
    QXmlStreamWriter writer(&output);
    int              depth = 0;
    QString          styleBody;
    bool             inStyle = false;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isDTD())
            return fail(QStringLiteral("SVG must not declare a DTD"));
        if (reader.isStartElement()) {
            const QString name = reader.name().toString();
            if (inStyle)
                return fail(QStringLiteral("style must hold only CSS"));
            if (depth == 0 && name != QStringLiteral("svg"))
                return fail(QStringLiteral("root element must be svg"));
            if (!isAllowedElement(name)) {
                reader.skipCurrentElement();
                continue;
            }
            inStyle = name == QStringLiteral("style");
            writer.writeStartElement(name);
            if (depth == 0) {
                writer.writeAttribute(
                    QStringLiteral("xmlns"), QStringLiteral("http://www.w3.org/2000/svg"));
                writer.writeAttribute(
                    QStringLiteral("xmlns:xlink"), QStringLiteral("http://www.w3.org/1999/xlink"));
                const auto &colors = palette(lightBackground);
                if (!reader.attributes().hasAttribute(QStringLiteral("color")))
                    writer
                        .writeAttribute(QStringLiteral("color"), colors.value(QStringLiteral("fg")));
                if (!reader.attributes().hasAttribute(QStringLiteral("font-family")))
                    writer
                        .writeAttribute(QStringLiteral("font-family"), QStringLiteral("sans-serif"));
            }
            for (const auto &attribute : reader.attributes()) {
                /* Editor namespaces lose their declaration, and drawing ignores them. */
                const auto prefix = attribute.prefix();
                if (!prefix.isEmpty() && prefix != QLatin1String("xlink")
                    && prefix != QLatin1String("xml"))
                    continue;
                const QString key   = attribute.name().toString();
                const QString value = attribute.value().toString();
                if (key.startsWith(QStringLiteral("on"), Qt::CaseInsensitive)
                    || leavesDocument(value))
                    continue;
                if (key == QStringLiteral("href")) {
                    if (value.startsWith(QLatin1Char('#')))
                        writer.writeAttribute(QStringLiteral("xlink:href"), value);
                    continue;
                }
                writer.writeAttribute(attribute.qualifiedName().toString(), value);
            }
            ++depth;
        } else if (reader.isEndElement()) {
            if (inStyle) {
                if (leavesDocument(styleBody))
                    return fail(QStringLiteral("SVG must not reference external resources"));
                writer.writeCharacters(styleBody);
                styleBody.clear();
                inStyle = false;
            }
            writer.writeEndElement();
            --depth;
        } else if (reader.isCharacters()) {
            if (inStyle)
                styleBody += reader.text();
            else
                writer.writeCharacters(reader.text().toString());
        }
    }
    if (reader.hasError())
        return fail(QStringLiteral("invalid SVG: %1").arg(reader.errorString()));
    if (output.isEmpty())
        return fail(QStringLiteral("empty SVG"));
    return output;
}

QByteArray drawForModel(const QByteArray &source)
{
    QProcess process;
    process.start(QSocSibling::path(QStringLiteral("qsoc")), {"--svg-raster", "opaque", "16"});
    process.write(source);
    process.closeWriteChannel();
    return process.waitForFinished(5000) && process.exitCode() == 0
               ? process.readAllStandardOutput()
               : QByteArray();
}

} // namespace QSocSvg
