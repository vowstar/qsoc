// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsochostprofile.h"

#include <yaml-cpp/yaml.h>

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

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
        entry.shell      = qstr(item["shell"].as<std::string>("")).trimmed();
        entry.scope      = scope;
        entry.sourcePath = sourcePath;
        if (entry.alias.isEmpty()) {
            qInfo() << "host catalog: skipping entry with empty alias in" << sourcePath;
            continue;
        }
        out.append(entry);
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
    if (!profile.shell.isEmpty()) {
        entry["shell"] = stds(profile.shell);
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
    projectNamesActive_ = false;

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
    projectNamesActive_ = projectRoot && projectRoot.IsMap() && projectRoot["active"];
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

namespace {

QString canonicalProject(const QString &projectPath)
{
    const QString canonical = QFileInfo(projectPath).canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(QFileInfo(projectPath).absoluteFilePath())
                               : canonical;
}

} // namespace

QString QSocHostBindingStore::defaultDir()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return base.isEmpty() ? QString() : QDir(base).filePath(QStringLiteral("host-bindings"));
}

QString QSocHostBindingStore::filePath(const QString &storeDir, const QString &projectPath)
{
    if (storeDir.isEmpty() || projectPath.isEmpty()) {
        return {};
    }
    const QByteArray digest
        = QCryptographicHash::hash(canonicalProject(projectPath).toUtf8(), QCryptographicHash::Sha256)
              .toHex()
              .left(32);
    return QDir(storeDir).filePath(QString::fromLatin1(digest) + QStringLiteral(".json"));
}

QSocHostBinding QSocHostBindingStore::load(const QString &storeDir, const QString &projectPath)
{
    QFile file(filePath(storeDir, projectPath));
    if (file.fileName().isEmpty() || !file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QJsonObject object = QJsonDocument::fromJson(file.read(64 * 1024)).object();
    if (object.value(QStringLiteral("project")).toString() != canonicalProject(projectPath)) {
        return {};
    }
    return {
        object.value(QStringLiteral("target")).toString(),
        object.value(QStringLiteral("workspace")).toString()};
}

bool QSocHostBindingStore::save(
    const QString         &storeDir,
    const QString         &projectPath,
    const QSocHostBinding &binding,
    QString               *errorMessage)
{
    const auto fail = [errorMessage](const QString &text) {
        if (errorMessage != nullptr) {
            *errorMessage = text;
        }
        return false;
    };
    const QString path = filePath(storeDir, projectPath);
    if (path.isEmpty()) {
        return fail(QStringLiteral("no project or no local data directory"));
    }
    if (!QDir().mkpath(storeDir)) {
        return fail(QStringLiteral("cannot create %1").arg(storeDir));
    }
    QFile::setPermissions(
        storeDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    const QJsonObject object{
        {QStringLiteral("project"), canonicalProject(projectPath)},
        {QStringLiteral("target"), binding.target},
        {QStringLiteral("workspace"), binding.workspace}};
    QSaveFile saver(path);
    if (!saver.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return fail(QStringLiteral("cannot open %1 for write").arg(path));
    }
    saver.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    saver.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
    if (!saver.commit()) {
        return fail(QStringLiteral("atomic commit failed for %1").arg(path));
    }
    return true;
}

#include "moc_qsochostprofile.cpp"
