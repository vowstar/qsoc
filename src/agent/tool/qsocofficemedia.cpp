// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>
#include "qsocofficemedia_p.h"

#include <QDir>
#include <QHash>
#include <QRegularExpression>
#include <QUrl>
#include <QXmlStreamReader>

namespace QSocOfficeMedia {
namespace {
using json = nlohmann::json;
struct Link
{
    QString type, target, part, issue;
    bool    external = false;
};
using Links = QHash<QString, Link>;

json attributes(const QXmlStreamAttributes &values)
{
    json result = json::object();
    for (const auto &value : values)
        result[value.qualifiedName().toString().toStdString()]
            = value.value().toString().toStdString();
    return result;
}

QString relationshipId(const QXmlStreamAttributes &values, const QString &name)
{
    for (const auto &value : values)
        if (value.name() == name && value.namespaceUri().endsWith(QStringLiteral("/relationships")))
            return value.value().toString();
    return {};
}

bool next(QXmlStreamReader &xml, const Package &package, QString *error)
{
    if (!error->isEmpty())
        return false;
    if (package.cancelled && package.cancelled()) {
        *error = QStringLiteral("Office media read cancelled or conversion deadline expired");
        return false;
    }
    xml.readNext();
    if (xml.isDTD() || xml.isEntityReference())
        xml.raiseError(QStringLiteral("DTD and entity declarations are unsupported"));
    if (xml.hasError())
        *error = QStringLiteral("Invalid Office media XML: %1").arg(xml.errorString());
    return !xml.atEnd();
}

QString targetPart(const QString &owner, const QString &raw)
{
    static const QRegularExpression
        encodedSeparator(QStringLiteral("%(?:2f|5c)"), QRegularExpression::CaseInsensitiveOption);
    const QUrl uri(raw, QUrl::StrictMode);
    if (!uri.isValid() || !uri.scheme().isEmpty() || uri.hasQuery() || uri.hasFragment()
        || raw.contains('\\') || raw.startsWith("//") || encodedSeparator.match(raw).hasMatch())
        return {};
    const QString decoded = QUrl::fromPercentEncoding(raw.toUtf8());
    for (const QChar ch : decoded)
        if (ch.unicode() < 32 || ch.unicode() == 127)
            return {};
    const QString directory = owner.left(owner.lastIndexOf('/') + 1);
    const QString part      = QDir::cleanPath(
        decoded.startsWith('/') ? decoded.mid(1) : directory + decoded);
    return part.isEmpty() || part == "." || part == ".." || part.startsWith("../")
                   || part.startsWith('/')
               ? QString()
               : part;
}

class Catalog
{
public:
    Catalog(const QString &format, const Package &package, QString *error)
        : format_(format)
        , package_(package)
        , error_(error)
    {}

    json run(const QList<Part> &parts)
    {
        contentTypes();
        json coverage = json::array();
        for (const auto &part : parts) {
            if (!error_->isEmpty())
                break;
            coverage.push_back(
                {{"page", part.page},
                 {"title", part.title.toStdString()},
                 {"archive_part", part.path.toStdString()}});
            if (format_ == "xlsx")
                worksheet(part);
            else
                scan(part, part.path, QString());
        }
        for (auto &media : media_) {
            media["reference_count_in_selected_parts"] = media["occurrences"].size();
            const QString path  = QString::fromStdString(media["archive_part"].get<std::string>());
            const auto    exact = overrides_.value(path);
            const auto    type  = exact.isEmpty() ? defaults_.value(path.section('.', -1).toLower())
                                                  : exact;
            media["declared_content_type"] = type.isEmpty() ? json(nullptr)
                                                            : json(type.toStdString());
        }
        return {
            {"media", media_},
            {"external_relationships", external_},
            {"unresolved_occurrences", unresolved_},
            {"coverage",
             {{"selected_parts", coverage},
              {"reference_scope", "selected_parts"},
              {"unvisited",
               "Headers, footers, notes, chart content, embedded packages and unselected parts are "
               "not traversed"}}},
            {"notice",
             "Media are original archive entries. Appearance crops and transforms are reported as "
             "raw metadata, never applied. Group transforms are not flattened. External targets "
             "are never fetched."}};
    }

private:
    void contentTypes()
    {
        if (!package_.contains(QStringLiteral("[Content_Types].xml")))
            return;
        QXmlStreamReader xml(package_.read(QStringLiteral("[Content_Types].xml"), error_));
        while (next(xml, package_, error_)) {
            if (!xml.isStartElement())
                continue;
            if (xml.name() == QStringLiteral("Default"))
                defaults_.insert(
                    xml.attributes().value("Extension").toString().toLower(),
                    xml.attributes().value("ContentType").toString());
            else if (xml.name() == QStringLiteral("Override")) {
                const QString part
                    = targetPart(QString(), xml.attributes().value("PartName").toString());
                if (!part.isEmpty())
                    overrides_.insert(part, xml.attributes().value("ContentType").toString());
            }
        }
    }

    Links links(const QString &owner)
    {
        if (links_.contains(owner))
            return links_.value(owner);
        Links         result;
        const int     slash = owner.lastIndexOf('/');
        const QString name = owner.left(slash + 1) + QStringLiteral("_rels/") + owner.mid(slash + 1)
                             + QStringLiteral(".rels");
        if (package_.contains(name)) {
            QXmlStreamReader xml(package_.read(name, error_));
            while (next(xml, package_, error_)) {
                if (!xml.isStartElement() || xml.name() != QStringLiteral("Relationship"))
                    continue;
                const auto    values = xml.attributes();
                const QString id     = values.value("Id").toString();
                if (id.isEmpty() || result.contains(id) || result.size() >= 4096) {
                    *error_ = QStringLiteral(
                        "Invalid, duplicate or excessive Office media relationships");
                    break;
                }
                Link link;
                link.type     = values.value("Type").toString();
                link.target   = values.value("Target").toString();
                link.external = values.value("TargetMode") == QStringLiteral("External")
                                || !QUrl(link.target).scheme().isEmpty();
                if (link.external)
                    external_.push_back(
                        {{"source_part", owner.toStdString()},
                         {"relationship_id", id.toStdString()},
                         {"type", link.type.toStdString()},
                         {"target", link.target.toStdString()},
                         {"fetched", false}});
                else {
                    link.part = targetPart(owner, link.target);
                    if (link.part.isEmpty())
                        link.issue = QStringLiteral("unsupported_internal_target");
                    else if (!package_.contains(link.part))
                        link.issue = QStringLiteral("missing_archive_part");
                }
                result.insert(id, link);
            }
        }
        links_.insert(owner, result);
        return result;
    }

    void occurrence(const QString &owner, const QString &id, json location)
    {
        if (++references_ > 1024) {
            *error_ = QStringLiteral("Office media exceeds 1024 occurrences. Select fewer parts");
            return;
        }
        location["source_part"]     = owner.toStdString();
        location["relationship_id"] = id.toStdString();
        const auto relationships    = links(owner);
        const auto found            = relationships.constFind(id);
        if (found == relationships.cend())
            location["status"] = "missing_relationship";
        else if (found->external) {
            location["status"] = "external_not_fetched";
            location["target"] = found->target.toStdString();
        } else if (!found->type.endsWith(QStringLiteral("/image")))
            location["status"] = "unsupported_relationship_type";
        else if (!found->issue.isEmpty()) {
            location["status"] = found->issue.toStdString();
            location["target"] = found->target.toStdString();
        } else {
            const auto path = found->part.toStdString();
            if (!indices_.contains(found->part)) {
                indices_.insert(found->part, media_.size());
                media_.push_back(
                    {{"media_id", path}, {"archive_part", path}, {"occurrences", json::array()}});
            }
            media_[indices_.value(found->part)]["occurrences"].push_back(std::move(location));
            return;
        }
        unresolved_.push_back(std::move(location));
    }

    QString cell(const json &position)
    {
        if (!position.contains("row") || !position.contains("col"))
            return {};
        bool         rowOk = false, colOk = false;
        const qint64 row
            = QString::fromStdString(position["row"].get<std::string>()).toLongLong(&rowOk);
        qint64 column
            = QString::fromStdString(position["col"].get<std::string>()).toLongLong(&colOk);
        if (!rowOk || !colOk || row < 0 || row >= 1048576 || column < 0 || column >= 16384)
            return {};
        QString letters;
        for (++column; column > 0; column = (column - 1) / 26)
            letters.prepend(QChar(static_cast<char16_t>('A' + (column - 1) % 26)));
        return letters + QString::number(row + 1);
    }

    void worksheet(const Part &part)
    {
        const auto       relationships = links(part.path);
        QXmlStreamReader xml(package_.read(part.path, error_));
        while (next(xml, package_, error_)) {
            if (!xml.isStartElement() || xml.name() != QStringLiteral("drawing"))
                continue;
            const QString id    = relationshipId(xml.attributes(), QStringLiteral("id"));
            const auto    found = relationships.constFind(id);
            if (found == relationships.cend() || found->external || !found->issue.isEmpty()
                || !found->type.endsWith(QStringLiteral("/drawing"))) {
                unresolved_.push_back(
                    {{"page", part.page},
                     {"sheet", part.title.toStdString()},
                     {"source_part", part.path.toStdString()},
                     {"relationship_id", id.toStdString()},
                     {"status", "unsupported_or_missing_worksheet_drawing"}});
                continue;
            }
            scan(part, found->part, id);
        }
    }

    void scan(const Part &part, const QString &owner, const QString &drawingId)
    {
        (void) links(owner);
        if (!error_->isEmpty())
            return;
        QXmlStreamReader xml(package_.read(owner, error_));
        QString          root;
        while (next(xml, package_, error_)) {
            if (xml.isStartElement()) {
                root = xml.name().toString();
                break;
            }
        }
        if (!error_->isEmpty())
            return;
        const QString expected = format_ == "docx"   ? QStringLiteral("document")
                                 : format_ == "pptx" ? QStringLiteral("sld")
                                                     : QStringLiteral("wsDr");
        if (root != expected) {
            unresolved_.push_back(
                {{"page", part.page},
                 {"source_part", owner.toStdString()},
                 {"relationship_id", drawingId.toStdString()},
                 {"status", "unsupported_media_source_part"}});
            return;
        }
        QStringList stack{root}, refs;
        json        anchor = json::object(), inherited = json::array(), properties = json::array(),
                    geometry = json::array(), crop = json::object();
        QString     kind;
        int         frameDepth = 0, paragraph = 0, frameParagraph = 0, groups = 0, frameGroups = 0;
        while (next(xml, package_, error_)) {
            const QString name = xml.name().toString();
            if (xml.isStartElement()) {
                stack.append(name);
                if (name == "p")
                    ++paragraph;
                if (name == "grpSp")
                    ++groups;
                if (format_ == "xlsx"
                    && (name == "oneCellAnchor" || name == "twoCellAnchor"
                        || name == "absoluteAnchor"))
                    anchor = {{"kind", name.toStdString()}};
                if (format_ == "xlsx"
                    && (name == "row" || name == "col" || name == "rowOff" || name == "colOff")) {
                    const QString parent = stack.value(stack.size() - 2);
                    if (parent == "from" || parent == "to")
                        anchor[parent.toStdString()][name.toStdString()]
                            = xml.readElementText().toStdString();
                    else
                        (void) xml.readElementText();
                    stack.removeLast();
                    continue;
                }
                if (format_ == "xlsx" && (name == "pos" || (name == "ext" && frameDepth == 0)))
                    anchor[name.toStdString()] = attributes(xml.attributes());
                if (format_ == "docx" && name == "docPr")
                    inherited.push_back(attributes(xml.attributes()));
                const bool picture = name == "pic" || name == "sp"
                                     || (format_ == "docx" && name == "shape");
                if (picture && frameDepth == 0) {
                    frameDepth = stack.size();
                    kind       = name;
                    refs.clear();
                    properties     = inherited;
                    geometry       = json::array();
                    crop           = json::object();
                    frameParagraph = paragraph;
                    frameGroups    = groups;
                    if (name == "shape")
                        properties.push_back(attributes(xml.attributes()));
                }
                if (!frameDepth)
                    continue;
                if (name == "cNvPr")
                    properties.push_back(attributes(xml.attributes()));
                if (name == "srcRect")
                    crop = attributes(xml.attributes());
                if (name == "xfrm" || name == "off" || name == "ext" || name == "chOff"
                    || name == "chExt")
                    geometry.push_back(
                        {{"element", name.toStdString()},
                         {"attributes", attributes(xml.attributes())}});
                if (name == "blip" || name == "svgBlip") {
                    const QString embedded
                        = relationshipId(xml.attributes(), QStringLiteral("embed"));
                    const QString linked = relationshipId(xml.attributes(), QStringLiteral("link"));
                    if (!embedded.isEmpty())
                        refs.append(embedded);
                    if (!linked.isEmpty())
                        refs.append(linked);
                } else if (name == "imagedata") {
                    const QString id = relationshipId(xml.attributes(), QStringLiteral("id"));
                    if (!id.isEmpty())
                        refs.append(id);
                }
            } else if (xml.isEndElement()) {
                if (frameDepth == stack.size() && name == kind) {
                    json location
                        = {{"page", part.page},
                           {"part_title", part.title.toStdString()},
                           {"object_properties", properties},
                           {"raw_crop", crop},
                           {"raw_geometry", geometry},
                           {"group_depth", frameGroups},
                           {"group_transform_applied", false},
                           {"appearance_transform_applied", false}};
                    QString alt;
                    for (const auto &property : properties)
                        for (const char *key : {"descr", "alt", "title"})
                            if (property.contains(key) && property[key].is_string()
                                && !property[key].get<std::string>().empty()) {
                                alt = QString::fromStdString(property[key].get<std::string>());
                                break;
                            }
                    location["alt_text"]   = alt.toStdString();
                    location["shape_id"]   = nullptr;
                    location["shape_name"] = nullptr;
                    for (const auto &property : properties) {
                        if (property.contains("id"))
                            location["shape_id"] = property["id"];
                        if (property.contains("name"))
                            location["shape_name"] = property["name"];
                    }
                    if (format_ == "docx")
                        location["paragraph"] = frameParagraph;
                    if (format_ == "pptx")
                        location["slide"] = part.page;
                    if (format_ == "xlsx") {
                        location["sheet"]                             = part.title.toStdString();
                        location["worksheet_part"]                    = part.path.toStdString();
                        location["worksheet_drawing_relationship_id"] = drawingId.toStdString();
                        location["raw_anchor"]                        = anchor;
                        const QString from    = cell(anchor.value("from", json::object()));
                        const QString to      = cell(anchor.value("to", json::object()));
                        location["from_cell"] = from.isEmpty() ? json(nullptr)
                                                               : json(from.toStdString());
                        location["to_cell"] = to.isEmpty() ? json(nullptr) : json(to.toStdString());
                        const auto anchorKind = anchor.value("kind", std::string());
                        location["anchor_status"]
                            = anchorKind == "absoluteAnchor" ? "absolute_no_cell_anchor"
                              : (from.isEmpty() || (anchorKind == "twoCellAnchor" && to.isEmpty()))
                                  ? "invalid_or_missing_cell_coordinates"
                                  : "cell_coordinates_only";
                    }
                    for (const QString &id : refs)
                        occurrence(owner, id, location);
                    frameDepth = 0;
                    kind.clear();
                }
                if (name == "grpSp")
                    --groups;
                if (format_ == "docx" && (name == "drawing" || name == "pict"))
                    inherited = json::array();
                if (!stack.isEmpty())
                    stack.removeLast();
            }
        }
    }

    QString                   format_;
    const Package            &package_;
    QString                  *error_;
    QHash<QString, Links>     links_;
    QHash<QString, QString>   defaults_, overrides_;
    QHash<QString, qsizetype> indices_;
    json media_ = json::array(), external_ = json::array(), unresolved_ = json::array();
    int  references_ = 0;
};
} // namespace

nlohmann::json discover(
    const QString &format, const QList<Part> &parts, const Package &package, QString *error)
{
    Catalog catalog(format, package, error);
    return catalog.run(parts);
}
} // namespace QSocOfficeMedia
