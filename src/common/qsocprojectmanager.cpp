// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "common/qsocprojectmanager.h"
#include "common/qsocconsole.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QProcess>
#include <QSaveFile>
#include <QScopeGuard>
#include <QStringList>
#include <QTextStream>
#include <QVersionNumber>

#include <optional>

namespace {

bool validateProjectName(const QString &projectName, bool reportError = true)
{
    if (projectName.isEmpty()) {
        if (reportError) {
            QSocConsole::error() << "Project name is empty.";
        }
        return false;
    }

    const QString invalidChars = "\\/:*?\"<>|";
    for (const QChar invalidChar : invalidChars) {
        if (projectName.contains(invalidChar)) {
            if (reportError) {
                QSocConsole::error() << "Project name contains invalid characters: " << invalidChar;
            }
            return false;
        }
    }
    return true;
}

bool pathEntryExists(const QString &path)
{
    const QFileInfo pathInfo(path);
    return pathInfo.exists() || pathInfo.isSymLink();
}

void createMarkerFile(const QString &path)
{
    if (pathEntryExists(path)) {
        return;
    }
    QFile markerFile(path);
    if (!markerFile.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        QSocConsole::warn() << "Failed to create directory marker file.";
    }
}

std::optional<QByteArray> serializeProjectFile(const YAML::Node &projectNode)
{
    try {
        YAML::Emitter emitter;
        emitter << projectNode;
        if (!emitter.good()) {
            return std::nullopt;
        }
        return QByteArray::fromStdString(std::string(emitter.c_str(), emitter.size()));
    } catch (const YAML::Exception &) {
        return std::nullopt;
    }
}

} // namespace

QSocProjectManager::QSocProjectManager(QObject *parent)
    : QObject{parent}
{
    /* Get system environments */
    const QStringList envList = QProcess::systemEnvironment();
    /* Save system environments into QMap */
    for (const QString &str : envList) {
        QStringList keyAndValue = str.split('=');
        if (keyAndValue.size() == 2) {
            env[keyAndValue[0]] = keyAndValue[1];
        }
    }
    /* Set project default name */
    setProjectName("");
    /* Initialize current path */
    currentPath = QDir::currentPath();
    /* Set project default paths */
    setProjectPath(currentPath);
    setBusPath(QDir(currentPath).filePath("bus"));
    setModulePath(QDir(currentPath).filePath("module"));
    setSchematicPath(QDir(currentPath).filePath("schematic"));
    setOutputPath(QDir(currentPath).filePath("output"));
}

QSocProjectManager::~QSocProjectManager() = default;

QSocProjectManager::State QSocProjectManager::captureState() const
{
    State state;
    state.env           = env;
    state.projectNode   = YAML::Clone(projectNode);
    state.projectName   = projectName;
    state.projectPath   = projectPath;
    state.busPath       = busPath;
    state.modulePath    = modulePath;
    state.schematicPath = schematicPath;
    state.outputPath    = outputPath;
    state.currentPath   = currentPath;
    return state;
}

void QSocProjectManager::restoreState(const State &state)
{
    env           = state.env;
    projectNode   = YAML::Clone(state.projectNode);
    projectName   = state.projectName;
    projectPath   = state.projectPath;
    busPath       = state.busPath;
    modulePath    = state.modulePath;
    schematicPath = state.schematicPath;
    outputPath    = state.outputPath;
    currentPath   = state.currentPath;
}

void QSocProjectManager::setEnv(const QString &key, const QString &value)
{
    env[key] = value;
}

void QSocProjectManager::setEnv(const QMap<QString, QString> &env)
{
    this->env = env;
}

const QMap<QString, QString> &QSocProjectManager::getEnv()
{
    return env;
}

QString QSocProjectManager::getSimplifyPath(const QString &path)
{
    QString result = path;
    /* Substitute path to environment variables */
    QMapIterator<QString, QString> iterator(env);
    while (iterator.hasNext()) {
        iterator.next();
        if (iterator.key().contains("QSOC_")) {
            const QString pattern = QString("${%1}").arg(iterator.key());
            result                = result.replace(iterator.value(), pattern);
        }
    }
    return result;
}

QString QSocProjectManager::getExpandPath(const QString &path)
{
    QString result = path;
    /* Substitute environment variables */
    QMapIterator<QString, QString> iterator(env);
    while (iterator.hasNext()) {
        iterator.next();
        const QString pattern = QString("${%1}").arg(iterator.key());
        result                = result.replace(pattern, iterator.value());
    }
    return result;
}

bool QSocProjectManager::isExist(const QString &projectName)
{
    if (!validateProjectName(projectName, false)) {
        return false;
    }
    /* Check project file */
    const QString &projectFilePath
        = QDir(projectPath).filePath(QString("%1.soc_pro").arg(projectName));
    return pathEntryExists(projectFilePath);
}

bool QSocProjectManager::mkpath()
{
    /* Check and create project directory */
    if (!QDir().mkpath(projectPath)) {
        QSocConsole::error() << "Failed to create project directory.";
        return false;
    }

    /* Create .gitignore file */
    const QString gitignorePath = QDir(projectPath).filePath(".gitignore");
    if (!pathEntryExists(gitignorePath)) {
        QFile gitignoreFile(gitignorePath);
        if (gitignoreFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::NewOnly)) {
            QTextStream out(&gitignoreFile);
            out << "qsoc.fl.*" << Qt::endl;
            gitignoreFile.close();
        } else {
            QSocConsole::warn() << "Failed to create .gitignore file in project directory.";
        }
    }

    /* Check and create bus directory */
    if (!QDir().mkpath(busPath)) {
        QSocConsole::error() << "Failed to create bus directory.";
        return false;
    }
    createMarkerFile(QDir(busPath).filePath(".gitkeep"));

    /* Check and create module directory */
    if (!QDir().mkpath(modulePath)) {
        QSocConsole::error() << "Failed to create module directory.";
        return false;
    }
    createMarkerFile(QDir(modulePath).filePath(".gitkeep"));

    /* Check and create schematic directory */
    if (!QDir().mkpath(schematicPath)) {
        QSocConsole::error() << "Failed to create schematic directory.";
        return false;
    }
    createMarkerFile(QDir(schematicPath).filePath(".gitkeep"));

    /* Check and create output directory */
    if (!QDir().mkpath(outputPath)) {
        QSocConsole::error() << "Failed to create output directory.";
        return false;
    }
    createMarkerFile(QDir(outputPath).filePath(".gitkeep"));
    return true;
}

bool QSocProjectManager::create(const QString &projectName)
{
    if (!validateProjectName(projectName)) {
        return false;
    }
    const State previousState = captureState();
    auto        rollback      = qScopeGuard([&]() { this->restoreState(previousState); });

    const QString projectFilePath
        = QDir(projectPath).filePath(QString("%1.soc_pro").arg(projectName));
    if (!QDir().mkpath(projectPath)) {
        QSocConsole::error() << "failed to create project directory.";
        return false;
    }
    QLockFile projectLock(projectFilePath + ".lock");
    if (!projectLock.tryLock()) {
        QSocConsole::error() << "project file is locked.";
        return false;
    }
    if (pathEntryExists(projectFilePath)) {
        QSocConsole::error() << "project already exists.";
        return false;
    }
    if (!mkpath()) {
        QSocConsole::error() << "failed to create project directories.";
        return false;
    }

    const std::optional<QByteArray> projectBytes = serializeProjectFile(getProjectYaml());
    if (!projectBytes) {
        QSocConsole::error() << "failed to serialize project file.";
        return false;
    }

    QFile projectFile(projectFilePath);
    if (!projectFile.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        if (pathEntryExists(projectFilePath)) {
            QSocConsole::error() << "project already exists.";
        } else {
            QSocConsole::error() << "failed to create project file.";
        }
        return false;
    }
    if (projectFile.write(*projectBytes) != projectBytes->size() || !projectFile.flush()) {
        QSocConsole::error() << "failed to write project file.";
        if (!projectFile.remove()) {
            QSocConsole::warn() << "failed to remove incomplete project file.";
        }
        return false;
    }
    projectFile.close();
    setProjectName(projectName);
    rollback.dismiss();
    return true;
}

bool QSocProjectManager::save(const QString &projectName)
{
    /* Check project name */
    if (!validateProjectName(projectName)) {
        return false;
    }
    const State previousState = captureState();
    auto        rollback      = qScopeGuard([&]() { this->restoreState(previousState); });
    /* Create project directories */
    if (!mkpath()) {
        QSocConsole::error() << "failed to create project directories.";
        return false;
    }

    const std::optional<QByteArray> projectBytes = serializeProjectFile(getProjectYaml());
    if (!projectBytes) {
        QSocConsole::error() << "failed to serialize project file.";
        return false;
    }

    const QString projectFilePath
        = QDir(projectPath).filePath(QString("%1.soc_pro").arg(projectName));
    QSaveFile projectFile(projectFilePath);
    projectFile.setDirectWriteFallback(false);
    if (!projectFile.open(QIODevice::WriteOnly)) {
        QSocConsole::error() << "failed to open project file.";
        return false;
    }
    if (projectFile.write(*projectBytes) != projectBytes->size()) {
        projectFile.cancelWriting();
        QSocConsole::error() << "failed to write project file.";
        return false;
    }
    if (!projectFile.commit()) {
        QSocConsole::error() << "failed to commit project file.";
        return false;
    }
    setProjectName(projectName);
    rollback.dismiss();
    return true;
}

bool QSocProjectManager::load(const QString &projectName)
{
    /* Check project name */
    if (!validateProjectName(projectName)) {
        return false;
    }
    /* Load project file */
    const QString &filePath = QDir(projectPath).filePath(QString("%1.soc_pro").arg(projectName));
    /* Check the existence of project files */
    if (!QFile::exists(filePath)) {
        QSocConsole::error() << "project file not found.";
        return false;
    }
    /* Load project file */
    YAML::Node localProjectNode = YAML::LoadFile(filePath.toStdString());
    /* Check project file version */
    const QVersionNumber projectVersion = QVersionNumber::fromString(
        QString::fromStdString(localProjectNode["version"].as<std::string>()));
    const QVersionNumber appVersion = QVersionNumber::fromString(
        QCoreApplication::applicationVersion());
    if (projectVersion > appVersion) {
        QSocConsole::error() << "project file version is newer than application version.";
        return false;
    }
    /* Set project name */
    setProjectName(QFileInfo(filePath).completeBaseName());
    /* Set project paths */
    setProjectPath(QFileInfo(filePath).absoluteDir().absolutePath());
    setProjectNode(localProjectNode);

    return true;
}

bool QSocProjectManager::loadFirst(bool silent)
{
    QString filePath;
    /* If path is a directory, search and pick a *.soc_pro file */
    if (QFileInfo(projectPath).isDir()) {
        /* QDir object for '.soc_pro' files in 'projectPath', sorted by name. */
        const QDir projectDir(
            projectPath,
            "*.soc_pro",
            QDir::SortFlag::Name | QDir::SortFlag::IgnoreCase,
            QDir::Files | QDir::NoDotAndDotDot);
        if (projectDir.count() == 0) {
            if (!silent) {
                QSocConsole::error() << "project file not found.";
            }
            return false;
        }
        /* Get the first path as filePath */
        filePath = projectDir.absoluteFilePath(projectDir[0]);
    }
    /* Check the existence of project files */
    if (!QFile::exists(filePath)) {
        if (!silent) {
            QSocConsole::error() << "project file not found.";
        }
        return false;
    }
    const QString localProjectName = QFileInfo(filePath).completeBaseName();
    /* Load the project */
    load(localProjectName);

    return true;
}

bool QSocProjectManager::remove(const QString &projectName)
{
    /* Check project name */
    if (!validateProjectName(projectName)) {
        return false;
    }
    /* Check the existence of project files */
    if (!isExist(projectName)) {
        QSocConsole::error() << "project file not found.";
        return false;
    }
    /* Remove project file */
    const QString &filePath = QDir(projectPath).filePath(QString("%1.soc_pro").arg(projectName));
    if (!QFile::remove(filePath)) {
        QSocConsole::error() << "failed to remove project file.";
        return false;
    }
    return true;
}

QStringList QSocProjectManager::list(const QRegularExpression &projectNameRegex)
{
    QStringList result;
    /* Check project path */
    if (!QDir(projectPath).exists()) {
        QSocConsole::error() << "project path is not a directory.";
        return result;
    }
    /* QDir object for '.soc_pro' files in 'projectPath', sorted by name. */
    const QDir projectDir(
        projectPath,
        "*.soc_pro",
        QDir::SortFlag::Name | QDir::SortFlag::IgnoreCase,
        QDir::Files | QDir::NoDotAndDotDot);
    /* Add matching file basenames from projectDir to result list. */
    foreach (const QString &filename, projectDir.entryList()) {
        if (projectNameRegex.match(filename).hasMatch()) {
            result.append(QFileInfo(filename).completeBaseName());
        }
    }
    return result;
}

bool QSocProjectManager::isValid(bool writable)
{
    /* Validate project node */
    if (!isValidProjectNode()) {
        QSocConsole::error() << "Invalid project node.";
        return false;
    }

    /* Validate project name */
    if (!isValidProjectName()) {
        QSocConsole::error() << "Invalid project name.";
        return false;
    }

    /* Validate project path */
    if (!isValidProjectPath(writable)) {
        QSocConsole::error() << "Invalid project path.";
        return false;
    }

    /* Validate bus path */
    if (!isValidBusPath(writable)) {
        QSocConsole::error() << "Invalid bus path.";
        return false;
    }

    /* Validate module path */
    if (!isValidModulePath(writable)) {
        QSocConsole::error() << "Invalid module path.";
        return false;
    }

    /* Validate schematic path */
    if (!isValidSchematicPath(writable)) {
        QSocConsole::error() << "Invalid schematic path.";
        return false;
    }

    /* Validate output path */
    if (!isValidOutputPath(writable)) {
        QSocConsole::error() << "Invalid output path.";
        return false;
    }

    return true;
}

bool QSocProjectManager::isValidProjectNode()
{
    return projectNode.IsDefined() && !projectNode.IsNull();
}

bool QSocProjectManager::isValidProjectName()
{
    return validateProjectName(projectName);
}

bool QSocProjectManager::isValidPath(const QString &path, bool writable)
{
    if (path.isEmpty()) {
        QSocConsole::error() << "Path is empty";
        return false;
    }
    const QFileInfo pathInfo(path);
    if (!pathInfo.exists() || !pathInfo.isDir()) {
        QSocConsole::error() << "Path does not exist or is not a directory: " << path;
        return false;
    }
    if (writable && !pathInfo.isWritable()) {
        QSocConsole::error() << "Path is not writable: " << path;
        return false;
    }
    return true;
}

bool QSocProjectManager::isValidProjectPath(bool writable)
{
    return isValidPath(getProjectPath(), writable);
}

bool QSocProjectManager::isValidBusPath(bool writable)
{
    return isValidPath(getBusPath(), writable);
}

bool QSocProjectManager::isValidModulePath(bool writable)
{
    return isValidPath(getModulePath(), writable);
}

bool QSocProjectManager::isValidSchematicPath(bool writable)
{
    return isValidPath(getSchematicPath(), writable);
}

bool QSocProjectManager::isValidOutputPath(bool writable)
{
    return isValidPath(getOutputPath(), writable);
}

const YAML::Node &QSocProjectManager::getProjectYaml()
{
    projectNode["version"]   = QCoreApplication::applicationVersion().toStdString();
    projectNode["bus"]       = getSimplifyPath(busPath).toStdString();
    projectNode["module"]    = getSimplifyPath(modulePath).toStdString();
    projectNode["schematic"] = getSimplifyPath(schematicPath).toStdString();
    projectNode["output"]    = getSimplifyPath(outputPath).toStdString();
    return projectNode;
}

const QString &QSocProjectManager::getProjectName()
{
    return projectName;
}

const QString &QSocProjectManager::getProjectPath()
{
    return projectPath;
}

const QString &QSocProjectManager::getBusPath()
{
    return busPath;
}

const QString &QSocProjectManager::getModulePath()
{
    return modulePath;
}

const QString &QSocProjectManager::getSchematicPath()
{
    return schematicPath;
}

const QString &QSocProjectManager::getOutputPath()
{
    return outputPath;
}

void QSocProjectManager::setProjectNode(const YAML::Node &projectNode)
{
    this->projectNode = projectNode;
    setBusPath(QString::fromStdString(projectNode["bus"].as<std::string>()));
    setModulePath(QString::fromStdString(projectNode["module"].as<std::string>()));
    setSchematicPath(QString::fromStdString(projectNode["schematic"].as<std::string>()));
    setOutputPath(QString::fromStdString(projectNode["output"].as<std::string>()));
}

void QSocProjectManager::setProjectName(const QString &projectName)
{
    this->projectName = projectName;
}

void QSocProjectManager::setProjectPath(const QString &projectPath)
{
    this->projectPath       = getExpandPath(projectPath);
    env["QSOC_PROJECT_DIR"] = this->projectPath;
}

void QSocProjectManager::setBusPath(const QString &busPath)
{
    this->busPath = getExpandPath(busPath);
}

void QSocProjectManager::setModulePath(const QString &modulePath)
{
    this->modulePath = getExpandPath(modulePath);
}

void QSocProjectManager::setSchematicPath(const QString &schematicPath)
{
    this->schematicPath = getExpandPath(schematicPath);
}

void QSocProjectManager::setOutputPath(const QString &outputPath)
{
    this->outputPath = getExpandPath(outputPath);
}

void QSocProjectManager::setCurrentPath(const QString &currentPath)
{
    this->currentPath = getExpandPath(currentPath);

    /* Set project current paths */
    setProjectPath(this->currentPath);
    setBusPath(QDir(this->currentPath).filePath("bus"));
    setModulePath(QDir(this->currentPath).filePath("module"));
    setSchematicPath(QDir(this->currentPath).filePath("schematic"));
    setOutputPath(QDir(this->currentPath).filePath("output"));
}

const QString &QSocProjectManager::getCurrentPath()
{
    return currentPath;
}
