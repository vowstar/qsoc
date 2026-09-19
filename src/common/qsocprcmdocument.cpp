// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocprcmdocument.h"
#include "common/qsocprcmreader.h"

#include <QFile>
#include <QScopeGuard>

namespace {

using QSocPrcmDetail::Context;
using QSocPrcmDetail::Reader;

void checkTree(
    const Reader &reader, QList<YAML::Node> &active, QMap<int, QList<YAML::Node>> &complete)
{
    const auto &node = reader.value();
    if (!node.IsMap() && !node.IsSequence()) {
        return;
    }
    for (const auto &parent : active) {
        if (node.is(parent)) {
            reader.fail("PRCM_ALIAS_CYCLE", "YAML aliases must not form a cycle.");
        }
    }
    for (const auto &previous : complete.value(node.Mark().pos)) {
        if (node.is(previous)) {
            return;
        }
    }
    active.append(node);
    const auto pop = qScopeGuard([&active] { active.removeLast(); });
    if (node.IsMap()) {
        for (const auto &key : reader.keys()) {
            checkTree(reader.member(key), active, complete);
        }
    } else {
        for (std::size_t i = 0; i < reader.size(); ++i) {
            checkTree(reader.item(i), active, complete);
        }
    }
    complete[node.Mark().pos].append(node);
}

[[noreturn]] void duplicate(const Reader &field, const QSocPrcmSource &previous)
{
    throw QSocPrcmDiagnostic{
        "PRCM_DUPLICATE",
        "Duplicate declaration: " + field.position().path,
        {previous, field.position()}};
}

class Merger
{
public:
    QSocPrcmDocument result;

    Merger() { result.node = YAML::Node(YAML::NodeType::Map); }

    void add(const Reader &root)
    {
        for (const auto &key : root.keys()) {
            const auto field = root.member(key);
            if (key == "clock" || key == "reset" || key == "power") {
                appendResource(key, field);
            } else if (key != "prcm" && field.value().IsMap()) {
                appendTable(key, field);
            } else {
                if (declaration.contains({key})) {
                    duplicate(field, declaration.value({key}));
                }
                result.node[key.toStdString()] = field.value();
                declaration.insert({key}, field.position());
                if (key == "prcm") {
                    result.origin.insert(key, field.position());
                }
            }
        }
    }

private:
    void appendResource(const QString &key, const Reader &field)
    {
        const auto count = field.size();
        if (!declaration.contains({key})) {
            result.node[key.toStdString()] = YAML::Node(YAML::NodeType::Sequence);
            declaration.insert({key}, field.position());
            result.origin.insert(key, field.position());
        }
        auto sequence = result.node[key.toStdString()];
        for (std::size_t i = 0; i < count; ++i) {
            const auto item = field.item(i);
            item.keys();
            const auto        name = item.member("name").name();
            const QStringList id{key, name};
            if (declaration.contains(id)) {
                duplicate(item.member("name"), declaration.value(id));
            }
            declaration.insert(id, item.member("name").position());
            const auto prefix = key + QString("[%1]").arg(sequence.size());
            result.origin.insert(prefix, item.position());
            sequence.push_back(item.value());
        }
    }

    void appendTable(const QString &key, const Reader &field)
    {
        if (!declaration.contains({key})) {
            result.node[key.toStdString()] = YAML::Node(YAML::NodeType::Map);
            declaration.insert({key}, field.position());
        } else if (!result.node[key.toStdString()].IsMap()) {
            duplicate(field, declaration.value({key}));
        }
        auto table = result.node[key.toStdString()];
        for (const auto &name : field.keys()) {
            const auto        item = field.member(name);
            const QStringList id{key, name};
            if (declaration.contains(id)) {
                duplicate(item, declaration.value(id));
            }
            table[name.toStdString()] = item.value();
            declaration.insert(id, item.position());
        }
    }

    QMap<QStringList, QSocPrcmSource> declaration;
};

} // namespace

QSocPrcmDocumentResult QSocPrcmDocumentLoader::load(const QStringList &files)
{
    QSocPrcmDocumentResult result;
    if (files.isEmpty()) {
        result.diagnostic.append({"PRCM_FILE", "Expected at least one input file.", {}});
        return result;
    }
    Merger merger;
    merger.result.file = files[0];
    for (const auto &path : files) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            result.diagnostic.append(
                {"PRCM_FILE", "Cannot read input: " + file.errorString(), {{path, {}, 0, 0}}});
            return result;
        }
        try {
            const auto                   node = YAML::Load(file.readAll().toStdString());
            Context                      context{path, {}, {}};
            const Reader                 root(node, {}, context);
            QList<YAML::Node>            active;
            QMap<int, QList<YAML::Node>> complete;
            checkTree(root, active, complete);
            merger.add(root);
        } catch (const QSocPrcmDiagnostic &diagnostic) {
            result.diagnostic.append(diagnostic);
            return result;
        } catch (const YAML::Exception &error) {
            result.diagnostic.append(
                {"PRCM_YAML",
                 QString::fromStdString(error.msg),
                 {{path,
                   {},
                   error.mark.is_null() ? 0 : error.mark.line + 1,
                   error.mark.is_null() ? 0 : error.mark.column + 1}}});
            return result;
        }
    }
    result.document = std::move(merger.result);
    return result;
}
