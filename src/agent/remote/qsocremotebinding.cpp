// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocremotebinding.h"

#include <yaml-cpp/yaml.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace {

QString bindingRelativePath()
{
    return QStringLiteral(".qsoc/remote.yml");
}

} // namespace

QString QSocRemoteBinding::pathFor(const QString &projectRoot)
{
    if (projectRoot.isEmpty()) {
        return {};
    }
    return QDir(projectRoot).absoluteFilePath(bindingRelativePath());
}

QString QSocRemoteBinding::readTarget(const QString &projectRoot)
{
    const QString path = pathFor(projectRoot);
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        return {};
    }
    try {
        const YAML::Node node = YAML::LoadFile(path.toStdString());
        if (node && node.IsMap() && node["target"]) {
            return QString::fromStdString(node["target"].as<std::string>());
        }
    } catch (const YAML::Exception &) {
        /* Malformed file: treat as absent. Caller can re-write cleanly. */
    }
    return {};
}

bool QSocRemoteBinding::writeTarget(
    const QString &projectRoot, const QString &target, QString *errorMessage)
{
    const QString path = pathFor(projectRoot);
    if (path.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Project root is empty");
        }
        return false;
    }
    const QString dirPath = QFileInfo(path).absolutePath();
    QDir          dir;
    if (!dir.mkpath(dirPath)) {
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
    node["target"] = target.toStdString();

    YAML::Emitter emitter;
    emitter.SetIndent(2);
    emitter << node;
    const QByteArray payload = QByteArray(emitter.c_str(), emitter.size()) + "\n";

    /* Atomic write via QSaveFile (temp + rename). */
    QSaveFile saver(path);
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

bool QSocRemoteBinding::removeTarget(const QString &projectRoot, QString *errorMessage)
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

    YAML::Emitter emitter;
    emitter.SetIndent(2);
    emitter << node;
    const QByteArray payload = QByteArray(emitter.c_str(), emitter.size()) + "\n";

    QSaveFile saver(path);
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
