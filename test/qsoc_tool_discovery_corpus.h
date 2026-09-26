// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOC_TOOL_DISCOVERY_CORPUS_H
#define QSOC_TOOL_DISCOVERY_CORPUS_H

#include <nlohmann/json.hpp>
#include <QStringList>
#include <QVector>

namespace QSocToolDiscoveryCorpus {

struct Entry
{
    QString name;
    QString description;
    bool    allowed;
};

struct Query
{
    QString     text;
    QStringList expected;
};

inline QString name(int index)
{
    return QStringLiteral("test_tool_%1").arg(index, 3, 10, QLatin1Char('0'));
}

inline QStringList domains()
{
    return {
        "layout",
        "timing",
        "routing",
        "clock",
        "reset",
        "register",
        "memory",
        "bus",
        "power",
        "verification"};
}

inline QVector<Entry> entries()
{
    QVector<Entry> result;
    const auto     groups = domains();
    for (int i = 0; i < 200; ++i) {
        QString description = groups[i % groups.size()] + QStringLiteral(" analysis operation.");
        if (i == 10 || i == 20) {
            description += QStringLiteral(" Aliases: inspect audit.");
        }
        if (i == 7) {
            description += QStringLiteral(" 校验");
        }
        result.append({name(i), description, true});
    }
    for (int i = 200; i < 220; ++i) {
        result.append(
            {QStringLiteral("restricted_tool_%1").arg(i),
             QStringLiteral("restricted-marker inspection operation."),
             false});
    }
    return result;
}

inline nlohmann::json parameters()
{
    nlohmann::json properties = nlohmann::json::object();
    for (int i = 0; i < 12; ++i) {
        properties[QStringLiteral("option_%1").arg(i).toStdString()]
            = {{"type", "string"},
               {"description",
                "A textual input for this operation. Preserve its contents exactly."}};
    }
    return {
        {"type", "object"},
        {"properties", properties},
        {"required", nlohmann::json::array({"option_0"})},
        {"additionalProperties", false}};
}

inline QVector<Query> queries()
{
    QVector<Query> result;
    for (const int index : {0, 99, 199}) {
        result.append({name(index), {name(index)}});
    }
    const auto groups = domains();
    for (int group = 0; group < groups.size(); ++group) {
        QStringList expected;
        for (int i = group; i < 200; i += 10) {
            expected.append(name(i));
        }
        result.append({groups[group], expected});
    }
    result.append({QStringLiteral("inspect"), {name(10), name(20)}});
    result.append({QStringLiteral("audit"), {name(10), name(20)}});
    result.append({QStringLiteral("校验"), {name(7)}});
    result.append({QStringLiteral("missing_semantics_849"), {}});
    result.append({QStringLiteral("unmatched_query_957"), {}});
    result.append({QStringLiteral("restricted_tool_200"), {}});
    result.append({QStringLiteral("restricted-marker"), {}});
    return result;
}

} // namespace QSocToolDiscoveryCorpus

#endif
