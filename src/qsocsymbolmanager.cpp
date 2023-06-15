#include "qsocsymbolmanager.h"

#include "qslangdriver.h"

QSocSymbolManager::QSocSymbolManager(QObject *parent, QSocProjectManager *projectManager)
    : QObject{parent}
{
    this->projectManager = projectManager;
}

bool QSocSymbolManager::importFromFileList(
    const QRegularExpression &symbolNameRegex,
    const QString            &fileListPath,
    const QStringList        &filePathList)
{
    if (!projectManager) {
        qCritical() << "Error: project manager is null.";
        return false;
    }
    Q_UNUSED(symbolNameRegex)
    Q_UNUSED(fileListPath)
    Q_UNUSED(filePathList)

    QSlangDriver driver(this, projectManager);
    if (driver.parseFileList(fileListPath, filePathList)) {
        /* Parse success */
        QStringList moduleList = driver.getModuleList();
        if (moduleList.isEmpty()) {
            qCritical() << "Error: no module found.";
        } else {
            qDebug() << "Found modules:" << moduleList;
            qDebug() << driver.getModuleAst(moduleList.first()).dump(4).c_str();
        }
    }
    return false;
}