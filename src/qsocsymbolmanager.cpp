#include "qsocsymbolmanager.h"

#include "qslangdriver.h"

QSocSymbolManager::QSocSymbolManager(QObject *parent, QSocProjectManager *projectManager)
    : QObject{parent}
{
    /* Set project manager */
    if (projectManager) {
        this->projectManager = projectManager;
    }
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

    QSlangDriver driver(this, projectManager);
    if (driver.parseFileList(fileListPath, filePathList)) {
        /* Parse success */
        QStringList moduleList = driver.getModuleList();
        if (moduleList.isEmpty()) {
            qCritical() << "Error: no module found.";
            return false;
        }
        /* Pick first module if pattern is empty */
        if (symbolNameRegex.pattern().isEmpty()) {
            qDebug() << "Pick first module:" << moduleList.first();
            qDebug() << driver.getModuleAst(moduleList.first()).dump(4).c_str();
            return true;
        }
        /* Find module by pattern */
        bool hasMatch = false;
        for (const QString &moduleName : moduleList) {
            const QRegularExpressionMatch &match = symbolNameRegex.match(moduleName);
            if (match.hasMatch()) {
                qDebug() << "Found module:" << moduleName;
                qDebug() << driver.getModuleAst(moduleName).dump(4).c_str();
                hasMatch = true;
            }
        }
        if (hasMatch) {
            return true;
        }
    }
    qCritical() << "Error: no module found.";
    return false;
}
