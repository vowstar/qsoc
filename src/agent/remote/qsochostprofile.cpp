// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsochostprofile.h"

#include <yaml-cpp/yaml.h>

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace {

constexpr auto kProjectRelativePath = ".qsoc/host.yml";
constexpr auto kUserRelativePath    = "host.yml";

QString qstr(const std::string &raw)
{
    return QString::fromStdString(raw);
}

std::string stds(const QString &str)
{
    return str.toStdString();
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

void parseHostList(
    const YAML::Node       &node,
    const QString          &scope,
    const QString          &sourcePath,
    QList<QSocHostProfile> &out)
{
    if (!node || !node.IsSequence()) {
        return;
    }
    for (const auto &item : node) {
        if (!item.IsMap() || !item["alias"]) {
            qInfo() << "host catalog: skipping malformed entry in" << sourcePath;
            continue;
        }
        QSocHostProfile entry;
        entry.alias      = qstr(item["alias"].as<std::string>("")).trimmed();
        entry.workspace  = qstr(item["workspace"].as<std::string>(""));
        entry.capability = qstr(item["capability"].as<std::string>(""));
        entry.target     = qstr(item["target"].as<std::string>(""));
        entry.scope      = scope;
        entry.sourcePath = sourcePath;
        if (entry.alias.isEmpty()) {
            qInfo() << "host catalog: skipping entry with empty alias in" << sourcePath;
            continue;
        }
        out.append(entry);
    }
}

void parseActive(const YAML::Node &node, QSocHostActiveBinding &binding)
{
    if (!node) {
        return;
    }
    if (node.IsScalar()) {
        binding.alias = qstr(node.as<std::string>("")).trimmed();
        return;
    }
    if (node.IsMap()) {
        binding.adHocTarget    = qstr(node["target"].as<std::string>(""));
        binding.adHocWorkspace = qstr(node["workspace"].as<std::string>(""));
    }
}

YAML::Node toYamlEntry(const QSocHostProfile &profile)
{
    YAML::Node entry(YAML::NodeType::Map);
    entry["alias"]     = stds(profile.alias);
    entry["workspace"] = stds(profile.workspace);
    if (!profile.capability.isEmpty()) {
        entry["capability"] = stds(profile.capability);
    }
    if (!profile.target.isEmpty()) {
        entry["target"] = stds(profile.target);
    }
    return entry;
}

} // namespace

QSocHostCatalog::QSocHostCatalog(QObject *parent)
    : QObject(parent)
{}

QString QSocHostCatalog::projectFilePath() const
{
    if (projectDir_.isEmpty()) {
        return {};
    }
    return QDir(projectDir_).absoluteFilePath(QString::fromLatin1(kProjectRelativePath));
}

QString QSocHostCatalog::userFilePath() const
{
    if (userDir_.isEmpty()) {
        return {};
    }
    return QDir(userDir_).absoluteFilePath(QString::fromLatin1(kUserRelativePath));
}

void QSocHostCatalog::load(const QString &userDir, const QString &projectDir)
{
    userDir_    = userDir;
    projectDir_ = projectDir;
    userList_.clear();
    projectList_.clear();
    activeBinding_ = {};

    const auto readFile =
        [](const QString &path, const QString &scope, QList<QSocHostProfile> &out) {
            if (path.isEmpty() || !QFileInfo::exists(path)) {
                return YAML::Node();
            }
            try {
                YAML::Node node = YAML::LoadFile(path.toStdString());
                if (node && node.IsMap()) {
                    parseHostList(node["hostList"], scope, path, out);
                    return node;
                }
            } catch (const YAML::Exception &e) {
                qInfo() << "host catalog: malformed YAML in" << path << ":" << e.what();
            }
            return YAML::Node();
        };

    readFile(userFilePath(), QStringLiteral("user"), userList_);
    const YAML::Node projectRoot
        = readFile(projectFilePath(), QStringLiteral("project"), projectList_);
    if (projectRoot && projectRoot.IsMap()) {
        parseActive(projectRoot["active"], activeBinding_);
    }
}

QList<QSocHostProfile> QSocHostCatalog::allList() const
{
    QList<QSocHostProfile> result = projectList_;
    for (const auto &userEntry : userList_) {
        bool shadowed = false;
        for (const auto &projEntry : projectList_) {
            if (projEntry.alias == userEntry.alias) {
                shadowed = true;
                break;
            }
        }
        if (!shadowed) {
            result.append(userEntry);
        }
    }
    return result;
}

const QSocHostProfile *QSocHostCatalog::find(const QString &alias) const
{
    for (const auto &projEntry : projectList_) {
        if (projEntry.alias == alias) {
            return &projEntry;
        }
    }
    for (const auto &userEntry : userList_) {
        if (userEntry.alias == alias) {
            return &userEntry;
        }
    }
    return nullptr;
}

QSocHostActiveBinding QSocHostCatalog::active() const
{
    return activeBinding_;
}

bool QSocHostCatalog::setActiveAlias(const QString &alias, QString *errorMessage)
{
    if (alias.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("alias is empty");
        }
        return false;
    }
    if (!find(alias)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("alias %1 not found in catalog").arg(alias);
        }
        return false;
    }
    activeBinding_       = {};
    activeBinding_.alias = alias;
    if (!writeProject(errorMessage)) {
        return false;
    }
    emit catalogChanged();
    return true;
}

bool QSocHostCatalog::setActiveAdHoc(
    const QString &target, const QString &workspace, QString *errorMessage)
{
    if (target.isEmpty() || workspace.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("ad-hoc binding requires target and workspace");
        }
        return false;
    }
    activeBinding_                = {};
    activeBinding_.adHocTarget    = target;
    activeBinding_.adHocWorkspace = workspace;
    if (!writeProject(errorMessage)) {
        return false;
    }
    emit catalogChanged();
    return true;
}

bool QSocHostCatalog::clearActive(QString *errorMessage)
{
    activeBinding_ = {};
    if (!writeProject(errorMessage)) {
        return false;
    }
    emit catalogChanged();
    return true;
}

bool QSocHostCatalog::upsert(
    const QSocHostProfile &profile, bool allowOverwrite, QString *errorMessage)
{
    if (profile.alias.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("alias is empty");
        }
        return false;
    }
    if (profile.workspace.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("workspace is empty");
        }
        return false;
    }
    for (auto it = projectList_.begin(); it != projectList_.end(); ++it) {
        if (it->alias == profile.alias) {
            if (!allowOverwrite) {
                if (errorMessage) {
                    *errorMessage = QStringLiteral("alias %1 already exists").arg(profile.alias);
                }
                return false;
            }
            QSocHostProfile updated = profile;
            updated.scope           = QStringLiteral("project");
            updated.sourcePath      = projectFilePath();
            *it                     = updated;
            if (!writeProject(errorMessage)) {
                return false;
            }
            emit catalogChanged();
            return true;
        }
    }
    QSocHostProfile fresh = profile;
    fresh.scope           = QStringLiteral("project");
    fresh.sourcePath      = projectFilePath();
    projectList_.append(fresh);
    if (!writeProject(errorMessage)) {
        projectList_.removeLast();
        return false;
    }
    emit catalogChanged();
    return true;
}

bool QSocHostCatalog::applyOps(
    const QString &alias, const QList<QSocHostCatalogOp> &opList, QString *errorMessage)
{
    if (alias.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("alias is empty");
        }
        return false;
    }
    int projectIndex = -1;
    for (int i = 0; i < projectList_.size(); ++i) {
        if (projectList_[i].alias == alias) {
            projectIndex = i;
            break;
        }
    }
    QSocHostProfile working;
    if (projectIndex >= 0) {
        working = projectList_[projectIndex];
    } else {
        const QSocHostProfile *userMatch = nullptr;
        for (const auto &userEntry : userList_) {
            if (userEntry.alias == alias) {
                userMatch = &userEntry;
                break;
            }
        }
        if (!userMatch) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("alias %1 not found").arg(alias);
            }
            return false;
        }
        /* Materialize the user-scope entry into project scope so the
         * caller can mutate it without touching the shared user file. */
        working            = *userMatch;
        working.scope      = QStringLiteral("project");
        working.sourcePath = projectFilePath();
    }
    for (const auto &operation : opList) {
        switch (operation.kind) {
        case QSocHostCatalogOp::Kind::CapabilityAppend: {
            QString cap = working.capability;
            if (!cap.isEmpty() && !cap.endsWith('\n')) {
                cap.append('\n');
            }
            cap.append(operation.value);
            working.capability = cap;
            break;
        }
        case QSocHostCatalogOp::Kind::CapabilityRemove: {
            if (operation.value.isEmpty()) {
                if (errorMessage) {
                    *errorMessage = QStringLiteral("capability_remove value is empty");
                }
                return false;
            }
            if (!working.capability.contains(operation.value)) {
                if (errorMessage) {
                    *errorMessage
                        = QStringLiteral("capability does not contain %1").arg(operation.value);
                }
                return false;
            }
            working.capability = working.capability.replace(operation.value, QString());
            /* Collapse the leftover blank lines a remove leaves behind. */
            working.capability
                = working.capability.replace(QStringLiteral("\n\n"), QStringLiteral("\n"));
            working.capability = working.capability.trimmed();
            break;
        }
        case QSocHostCatalogOp::Kind::CapabilityReplace:
            working.capability = operation.value;
            break;
        case QSocHostCatalogOp::Kind::SetWorkspace:
            if (operation.value.isEmpty()) {
                if (errorMessage) {
                    *errorMessage = QStringLiteral("set_workspace value is empty");
                }
                return false;
            }
            working.workspace = operation.value;
            break;
        case QSocHostCatalogOp::Kind::SetTarget:
            working.target = operation.value;
            break;
        }
    }
    if (projectIndex >= 0) {
        projectList_[projectIndex] = working;
    } else {
        projectList_.append(working);
    }
    if (!writeProject(errorMessage)) {
        /* Reload from disk to revert in-memory state on commit failure. */
        load(userDir_, projectDir_);
        return false;
    }
    emit catalogChanged();
    return true;
}

bool QSocHostCatalog::remove(const QString &alias, QString *errorMessage)
{
    int projectIndex = -1;
    for (int i = 0; i < projectList_.size(); ++i) {
        if (projectList_[i].alias == alias) {
            projectIndex = i;
            break;
        }
    }
    if (projectIndex < 0) {
        for (const auto &userEntry : userList_) {
            if (userEntry.alias == alias) {
                if (errorMessage) {
                    *errorMessage = QStringLiteral(
                                        "alias %1 is in user scope (%2); edit that file directly")
                                        .arg(alias, userFilePath());
                }
                return false;
            }
        }
        if (errorMessage) {
            *errorMessage = QStringLiteral("alias %1 not found").arg(alias);
        }
        return false;
    }
    projectList_.removeAt(projectIndex);
    if (activeBinding_.isAlias() && activeBinding_.alias == alias) {
        activeBinding_ = {};
    }
    if (!writeProject(errorMessage)) {
        load(userDir_, projectDir_);
        return false;
    }
    emit catalogChanged();
    return true;
}

bool QSocHostCatalog::writeProject(QString *errorMessage)
{
    const QString path = projectFilePath();
    if (path.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("project scope is not set");
        }
        return false;
    }
    const QString dirPath = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(dirPath)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("cannot create %1").arg(dirPath);
        }
        return false;
    }

    YAML::Node root;
    if (QFileInfo::exists(path)) {
        try {
            root = YAML::LoadFile(path.toStdString());
            if (!root || !root.IsMap()) {
                root = YAML::Node(YAML::NodeType::Map);
            }
        } catch (const YAML::Exception &) {
            root = YAML::Node(YAML::NodeType::Map);
        }
    } else {
        root = YAML::Node(YAML::NodeType::Map);
    }

    if (activeBinding_.isLocal()) {
        root.remove("active");
    } else if (activeBinding_.isAlias()) {
        root["active"] = stds(activeBinding_.alias);
    } else {
        YAML::Node adHoc(YAML::NodeType::Map);
        adHoc["target"]    = stds(activeBinding_.adHocTarget);
        adHoc["workspace"] = stds(activeBinding_.adHocWorkspace);
        root["active"]     = adHoc;
    }

    if (projectList_.isEmpty()) {
        root.remove("hostList");
    } else {
        YAML::Node list(YAML::NodeType::Sequence);
        for (const auto &projEntry : projectList_) {
            list.push_back(toYamlEntry(projEntry));
        }
        root["hostList"] = list;
    }

    if (root.size() == 0) {
        QFile file(path);
        if (file.exists() && !file.remove()) {
            if (errorMessage) {
                *errorMessage
                    = QStringLiteral("cannot remove empty %1: %2").arg(path, file.errorString());
            }
            return false;
        }
        return true;
    }

    const QByteArray payload = emitYaml(root);
    QSaveFile        saver(path);
    if (!saver.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("cannot open %1 for write").arg(path);
        }
        return false;
    }
    saver.write(payload);
    if (!saver.commit()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("atomic commit failed for %1").arg(path);
        }
        return false;
    }
    return true;
}
