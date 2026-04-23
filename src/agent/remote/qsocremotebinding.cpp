// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocremotebinding.h"

#include <yaml-cpp/yaml.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace {

QString bindingRelativePath()
{
    return QStringLiteral(".qsoc/remote.yml");
}

QByteArray emitYaml(const YAML::Node &node)
{
    YAML::Emitter emitter;
    emitter.SetIndent(2);
    emitter << node;
    QByteArray payload(emitter.c_str(), static_cast<int>(emitter.size()));
    if (!payload.endsWith('\n')) {
        payload.append('\n');
    }
    return payload;
}

} // namespace

QString QSocRemoteBinding::pathFor(const QString &projectRoot)
{
    if (projectRoot.isEmpty()) {
        return {};
    }
    return QDir(projectRoot).absoluteFilePath(bindingRelativePath());
}

QSocRemoteBinding::Entry QSocRemoteBinding::read(const QString &projectRoot)
{
    Entry         entry;
    const QString path = pathFor(projectRoot);
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        return entry;
    }
    try {
        const YAML::Node node = YAML::LoadFile(path.toStdString());
        if (node && node.IsMap()) {
            if (node["target"]) {
                entry.target = QString::fromStdString(node["target"].as<std::string>());
            }
            if (node["workspace"]) {
                entry.workspace = QString::fromStdString(node["workspace"].as<std::string>());
            }
        }
    } catch (const YAML::Exception &) {
        /* Malformed file: treat as absent so the caller can rewrite cleanly. */
    }
    return entry;
}

bool QSocRemoteBinding::write(const QString &projectRoot, const Entry &entry, QString *errorMessage)
{
    const QString path = pathFor(projectRoot);
    if (path.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Project root is empty");
        }
        return false;
    }
    const QString dirPath = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(dirPath)) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Cannot create %1").arg(dirPath);
        }
        return false;
    }

    /* Load existing YAML (if any) so we preserve sibling keys. */
    YAML::Node node;
    if (QFileInfo::exists(path)) {
        try {
            node = YAML::LoadFile(path.toStdString());
            if (!node || !node.IsMap()) {
                node = YAML::Node(YAML::NodeType::Map);
            }
        } catch (const YAML::Exception &) {
            node = YAML::Node(YAML::NodeType::Map);
        }
    } else {
        node = YAML::Node(YAML::NodeType::Map);
    }

    if (entry.target.isEmpty()) {
        node.remove("target");
    } else {
        node["target"] = entry.target.toStdString();
    }
    if (entry.workspace.isEmpty()) {
        node.remove("workspace");
    } else {
        node["workspace"] = entry.workspace.toStdString();
    }

    const QByteArray payload = emitYaml(node);
    QSaveFile        saver(path);
    if (!saver.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Cannot open %1 for write").arg(path);
        }
        return false;
    }
    saver.write(payload);
    if (!saver.commit()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Atomic commit failed for %1").arg(path);
        }
        return false;
    }
    return true;
}

bool QSocRemoteBinding::clear(const QString &projectRoot, QString *errorMessage)
{
    const QString path = pathFor(projectRoot);
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        return true;
    }
    YAML::Node node;
    try {
        node = YAML::LoadFile(path.toStdString());
    } catch (const YAML::Exception &) {
        node = YAML::Node(YAML::NodeType::Map);
    }
    if (!node || !node.IsMap()) {
        node = YAML::Node(YAML::NodeType::Map);
    }
    node.remove("target");
    node.remove("workspace");

    /* If nothing else is left, delete the file to keep the project tree tidy. */
    if (node.size() == 0) {
        QFile file(path);
        if (!file.remove()) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("Cannot remove %1: %2").arg(path, file.errorString());
            }
            return false;
        }
        return true;
    }

    const QByteArray payload = emitYaml(node);
    QSaveFile        saver(path);
    if (!saver.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Cannot open %1 for write").arg(path);
        }
        return false;
    }
    saver.write(payload);
    if (!saver.commit()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Atomic commit failed for %1").arg(path);
        }
        return false;
    }
    return true;
}
