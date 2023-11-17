#include "common/qsocprojectmanager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStringList>
#include <QVersionNumber>

#include <fstream>

QSocProjectManager::QSocProjectManager(QObject *parent)
    : QObject{parent}
{
    /* Get system environments */
    const QStringList envList = QProcess::systemEnvironment();
    /* Save system environments into QMap */
    foreach (const QString str, envList) {
        QStringList keyAndValue = str.split('=');
        if (keyAndValue.size() == 2) {
            env[keyAndValue[0]] = keyAndValue[1];
        }
    }
    /* Set project default name */
    setProjectName("");
    /* Set project default paths */
    setProjectPath(QDir::currentPath());
    setBusPath(QDir::currentPath() + "/bus");
    setSymbolPath(QDir::currentPath() + "/symbol");
    setSchematicPath(QDir::currentPath() + "/schematic");
    setOutputPath(QDir::currentPath() + "/output");
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
        if (iterator.key().contains("SOCSTUDIO_")) {
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
    bool result = false;
    /* Check project name */
    if (projectName.isEmpty()) {
        qCritical() << "Error: project name is empty.";
        return false;
    }
    /* Check project file */
    const QString &projectFilePath = QString(projectPath + "/" + projectName + ".soc_pro");
    if (QFile::exists(projectFilePath)) {
        result = true;
    }
    return result;
}

bool QSocProjectManager::mkpath()
{
    /* Check and create project directory */
    if (!QDir().mkpath(projectPath)) {
        qCritical() << "Error: Failed to create project directory.";
        return false;
    }
    /* Check and create bus directory */
    if (!QDir().mkpath(busPath)) {
        qCritical() << "Error: Failed to create bus directory.";
        return false;
    }
    /* Check and create symbol directory */
    if (!QDir().mkpath(symbolPath)) {
        qCritical() << "Error: Failed to create symbol directory.";
        return false;
    }
    /* Check and create schematic directory */
    if (!QDir().mkpath(schematicPath)) {
        qCritical() << "Error: Failed to create schematic directory.";
        return false;
    }
    /* Check and create output directory */
    if (!QDir().mkpath(outputPath)) {
        qCritical() << "Error: Failed to create output directory.";
        return false;
    }
    return true;
}

bool QSocProjectManager::save(const QString &projectName)
{
    /* Check project name */
    if (projectName.isEmpty()) {
        qCritical() << "Error: project name is empty.";
        return false;
    }
    setProjectName(projectName);
    /* Create project directories */
    if (!mkpath()) {
        qCritical() << "Error: failed to create project directories.";
        return false;
    }
    /* Save project file */
    const QString &projectFilePath = QString(projectPath + "/" + projectName + ".soc_pro");
    std::ofstream  outputFileStream(projectFilePath.toStdString());
    /* Serialize project yaml data */
    outputFileStream << getProjectNode();

    return true;
}

bool QSocProjectManager::load(const QString &projectName)
{
    /* Check project name */
    if (projectName.isEmpty()) {
        qCritical() << "Error: project name is empty.";
        return false;
    }
    /* Load project file */
    const QString &filePath = QString(projectPath + "/" + projectName + ".soc_pro");
    /* Check the existence of project files */
    if (!QFile::exists(filePath)) {
        qCritical() << "Error: project file not found.";
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
        qCritical() << "Error: project file version is newer than application version.";
        return false;
    }
    /* Set project name */
    setProjectName(filePath.split('/').last().split('.').first());
    /* Set project paths */
    setProjectPath(QFileInfo(filePath).absoluteDir().absolutePath());
    setProjectNode(localProjectNode);

    return true;
}

bool QSocProjectManager::loadFirst()
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
            qCritical() << "Error: project file not found.";
            return false;
        }
        /* Get the first path as filePath */
        filePath = projectDir.absoluteFilePath(projectDir[0]);
    }
    /* Check the existence of project files */
    if (!QFile::exists(filePath)) {
        qCritical() << "Error: project file not found.";
        return false;
    }
    const QString localProjectName = filePath.split('/').last().split('.').first();
    /* Load the project */
    load(localProjectName);

    return true;
}

bool QSocProjectManager::remove(const QString &projectName)
{
    /* Check project name */
    if (projectName.isEmpty()) {
        qCritical() << "Error: project name is empty.";
        return false;
    }
    /* Check the existence of project files */
    if (!isExist(projectName)) {
        qCritical() << "Error: project file not found.";
        return false;
    }
    /* Remove project file */
    const QString &filePath = QString(projectPath + "/" + projectName + ".soc_pro");
    if (!QFile::remove(filePath)) {
        qCritical() << "Error: failed to remove project file.";
        return false;
    }
    return true;
}

QStringList QSocProjectManager::list(const QRegularExpression &projectNameRegex)
{
    QStringList result;
    /* Check project path */
    if (!QDir(projectPath).exists()) {
        qCritical() << "Error: project path is not a directory.";
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
            result.append(filename.split('.').first());
        }
    }
    return result;
}

bool QSocProjectManager::isValid(bool writable)
{
    /* Validate project node */
    if (!isValidProjectNode()) {
        qCritical() << "Error: Invalid project node.";
        return false;
    }

    /* Validate project name */
    if (!isValidProjectName()) {
        qCritical() << "Error: Invalid project name.";
        return false;
    }

    /* Validate project path */
    if (!isValidProjectPath(writable)) {
        qCritical() << "Error: Invalid project path.";
        return false;
    }

    /* Validate bus path */
    if (!isValidBusPath(writable)) {
        qCritical() << "Error: Invalid bus path.";
        return false;
    }

    /* Validate symbol path */
    if (!isValidSymbolPath(writable)) {
        qCritical() << "Error: Invalid symbol path.";
        return false;
    }

    /* Validate schematic path */
    if (!isValidSchematicPath(writable)) {
        qCritical() << "Error: Invalid schematic path.";
        return false;
    }

    /* Validate output path */
    if (!isValidOutputPath(writable)) {
        qCritical() << "Error: Invalid output path.";
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
    const QString &projectName = getProjectName();
    /* Check if project name is empty */
    if (projectName.isEmpty()) {
        qCritical() << "Error: Project name is empty.";
        return false;
    }

    /* Define a list of invalid characters for file names */
    const QString invalidChars = "\\/:*?\"<>|";

    /* Check if project name contains any invalid characters */
    for (const QChar invalidChar : invalidChars) {
        if (projectName.contains(invalidChar)) {
            qCritical() << "Error: Project name contains invalid characters: " << invalidChar;
            return false;
        }
    }

    return true;
}

bool QSocProjectManager::isValidPath(const QString &path, bool writable)
{
    if (path.isEmpty()) {
        qCritical() << "Error: Path is empty";
        return false;
    }
    const QFileInfo pathInfo(path);
    if (!pathInfo.exists() || !pathInfo.isDir()) {
        qCritical() << "Error: Path does not exist or is not a directory: " << path;
        return false;
    }
    if (writable && !pathInfo.isWritable()) {
        qCritical() << "Error: Path is not writable: " << path;
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

bool QSocProjectManager::isValidSymbolPath(bool writable)
{
    return isValidPath(getSymbolPath(), writable);
}

bool QSocProjectManager::isValidSchematicPath(bool writable)
{
    return isValidPath(getSchematicPath(), writable);
}

bool QSocProjectManager::isValidOutputPath(bool writable)
{
    return isValidPath(getOutputPath(), writable);
}

const YAML::Node &QSocProjectManager::getProjectNode()
{
    projectNode["version"]   = QCoreApplication::applicationVersion().toStdString();
    projectNode["bus"]       = getSimplifyPath(busPath).toStdString();
    projectNode["symbol"]    = getSimplifyPath(symbolPath).toStdString();
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

const QString &QSocProjectManager::getSymbolPath()
{
    return symbolPath;
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
    setSymbolPath(QString::fromStdString(projectNode["symbol"].as<std::string>()));
    setSchematicPath(QString::fromStdString(projectNode["schematic"].as<std::string>()));
    setOutputPath(QString::fromStdString(projectNode["output"].as<std::string>()));
}

void QSocProjectManager::setProjectName(const QString &projectName)
{
    this->projectName = projectName;
}

void QSocProjectManager::setProjectPath(const QString &projectPath)
{
    this->projectPath            = getExpandPath(projectPath);
    env["SOCSTUDIO_PROJECT_DIR"] = this->projectPath;
}

void QSocProjectManager::setBusPath(const QString &busPath)
{
    this->busPath = getExpandPath(busPath);
}

void QSocProjectManager::setSymbolPath(const QString &symbolPath)
{
    this->symbolPath = getExpandPath(symbolPath);
}

void QSocProjectManager::setSchematicPath(const QString &schematicPath)
{
    this->schematicPath = getExpandPath(schematicPath);
}

void QSocProjectManager::setOutputPath(const QString &outputPath)
{
    this->outputPath = getExpandPath(outputPath);
}
