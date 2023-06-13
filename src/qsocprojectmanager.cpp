#include "qsocprojectmanager.h"

#include <fstream>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStringList>
#include <QVersionNumber>

#include "yaml-cpp/yaml.h"

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

bool QSocProjectManager::save(const QString &projectName)
{
    /* Check project name */
    if (projectName.isEmpty()) {
        qCritical() << "Error: project name is empty.";
        return false;
    }
    setProjectName(projectName);
    /* Create project directories */
    if (!QDir().mkpath(projectPath)) {
        qCritical() << "Error: failed to create bus directory.";
        return false;
    }
    if (!QDir().mkpath(busPath)) {
        qCritical() << "Error: failed to create bus directory.";
        return false;
    }
    if (!QDir().mkpath(symbolPath)) {
        qCritical() << "Error: failed to create symbol directory.";
        return false;
    }
    if (!QDir().mkpath(schematicPath)) {
        qCritical() << "Error: failed to create schematic directory.";
        return false;
    }
    if (!QDir().mkpath(outputPath)) {
        qCritical() << "Error: failed to create output directory.";
        return false;
    }
    /* Create YAML data */
    YAML::Node projectNode;
    projectNode["version"]   = QCoreApplication::applicationVersion().toStdString();
    projectNode["bus"]       = getSimplifyPath(busPath).toStdString();
    projectNode["symbol"]    = getSimplifyPath(symbolPath).toStdString();
    projectNode["schematic"] = getSimplifyPath(schematicPath).toStdString();
    projectNode["output"]    = getSimplifyPath(outputPath).toStdString();
    /* Save project file */
    const QString &projectFilePath = QString(projectPath + "/" + projectName + ".soc_pro");
    std::ofstream  outputFileStream(projectFilePath.toStdString());
    outputFileStream << projectNode;

    return true;
}

bool QSocProjectManager::load(const QString &projectName)
{
    /* Check project name */
    if (projectName.isEmpty()) {
        qCritical() << "Error: project name is empty.";
        return false;
    }
    setProjectName(projectName);
    /* Load project file */
    const QString &filePath = QString(projectPath + "/" + projectName + ".soc_pro");
    /* Check the existence of project files */
    if (!QFile::exists(filePath)) {
        qCritical() << "Error: project file not found.";
        return false;
    }
    /* Load project file */
    YAML::Node projectNode = YAML::LoadFile(filePath.toStdString());
    /* Check project file version */
    const QVersionNumber projectVersion = QVersionNumber::fromString(
        QString::fromStdString(projectNode["version"].as<std::string>()));
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
    setBusPath(QString::fromStdString(projectNode["bus"].as<std::string>()));
    setSymbolPath(QString::fromStdString(projectNode["symbol"].as<std::string>()));
    setSchematicPath(QString::fromStdString(projectNode["schematic"].as<std::string>()));
    setOutputPath(QString::fromStdString(projectNode["output"].as<std::string>()));

    return true;
}

bool QSocProjectManager::autoLoad()
{
    QString filePath;
    /* If path is a directory, search and pick a *.soc_pro file */
    if (QFileInfo(projectPath).isDir()) {
        QDir dir(projectPath);
        dir.setNameFilters(QStringList() << "*.soc_pro");
        dir.setFilter(QDir::Files | QDir::NoDotAndDotDot | QDir::NoSymLinks);
        if (dir.count() == 0) {
            qCritical() << "Error: project file not found.";
            return false;
        }
        filePath = dir.absoluteFilePath(dir[0]);
    }
    /* Check the existence of project files */
    if (!QFile::exists(filePath)) {
        qCritical() << "Error: project file not found.";
        return false;
    }
    /* Load project file */
    YAML::Node projectNode = YAML::LoadFile(filePath.toStdString());
    /* Check project file version */
    const QVersionNumber projectVersion = QVersionNumber::fromString(
        QString::fromStdString(projectNode["version"].as<std::string>()));
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
    setBusPath(QString::fromStdString(projectNode["bus"].as<std::string>()));
    setSymbolPath(QString::fromStdString(projectNode["symbol"].as<std::string>()));
    setSchematicPath(QString::fromStdString(projectNode["schematic"].as<std::string>()));
    setOutputPath(QString::fromStdString(projectNode["output"].as<std::string>()));

    return true;
}

bool QSocProjectManager::isValid()
{
    if (projectName.isEmpty()) {
        qCritical() << "Error: project name is empty.";
        return false;
    }
    if (projectPath.isEmpty()) {
        qCritical() << "Error: project path is empty.";
        return false;
    }
    if (busPath.isEmpty()) {
        qCritical() << "Error: bus path is empty.";
        return false;
    }
    if (symbolPath.isEmpty()) {
        qCritical() << "Error: symbol path is empty.";
        return false;
    }
    if (schematicPath.isEmpty()) {
        qCritical() << "Error: schematic path is empty.";
        return false;
    }
    if (outputPath.isEmpty()) {
        qCritical() << "Error: output path is empty.";
        return false;
    }
    if (!QDir(projectPath).exists()) {
        qCritical() << "Error: project path is not a directory.";
        return false;
    }
    if (!QDir(busPath).exists()) {
        qCritical() << "Error: bus path is not a directory.";
        return false;
    }
    if (!QDir(symbolPath).exists()) {
        qCritical() << "Error: symbol path is not a directory.";
        return false;
    }
    if (!QDir(schematicPath).exists()) {
        qCritical() << "Error: schematic path is not a directory.";
        return false;
    }
    if (!QDir(outputPath).exists()) {
        qCritical() << "Error: output path is not a directory.";
        return false;
    }
    return true;
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
