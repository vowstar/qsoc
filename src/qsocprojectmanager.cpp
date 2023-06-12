#include "qsocprojectmanager.h"

#include <QDir>

QSocProjectManager::QSocProjectManager(QObject *parent)
    : QObject{parent}
{
    /* Set default paths */
    projectPath   = QDir::currentPath();
    busPath       = QDir::currentPath() + "/bus";
    symbolPath    = QDir::currentPath() + "/symbol";
    schematicPath = QDir::currentPath() + "/schematic";
    outputPath    = QDir::currentPath() + "/output";
}

bool QSocProjectManager::create(const QString &projectName)
{
    /* Create project directory */
    QDir dir = QDir(projectPath);
    if (!dir.mkpath(projectPath)) {
        qCritical() << "Error: failed to create bus directory.";
        return false;
    }
    if (!dir.mkpath(busPath)) {
        qCritical() << "Error: failed to create bus directory.";
        return false;
    }
    if (!dir.mkpath(symbolPath)) {
        qCritical() << "Error: failed to create symbol directory.";
        return false;
    }
    if (!dir.mkpath(schematicPath)) {
        qCritical() << "Error: failed to create schematic directory.";
        return false;
    }
    if (!dir.mkpath(outputPath)) {
        qCritical() << "Error: failed to create output directory.";
        return false;
    }

    return true;
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

void QSocProjectManager::setProjectPath(const QString &projectPath)
{
    this->projectPath = projectPath;
}

void QSocProjectManager::setBusPath(const QString &busPath)
{
    this->busPath = busPath;
}

void QSocProjectManager::setSymbolPath(const QString &symbolPath)
{
    this->symbolPath = symbolPath;
}

void QSocProjectManager::setSchematicPath(const QString &schematicPath)
{
    this->schematicPath = schematicPath;
}

void QSocProjectManager::setOutputPath(const QString &outputPath)
{
    this->outputPath = outputPath;
}
