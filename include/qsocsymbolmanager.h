#ifndef QSOCSYMBOLMANAGER_H
#define QSOCSYMBOLMANAGER_H

#include <QObject>
#include <QRegularExpression>

#include "qsocprojectmanager.h"

/**
 * @brief   The QSocSymbolManager class
 * @details This class is used to manage the symbol files.
 */
class QSocSymbolManager : public QObject
{
    Q_OBJECT
public:
    /**
     * @brief   Constructor
     * @param   parent parent object
     * @param   projectManager project manager
     * @details This constructor will create an instance of this object.
     */
    explicit QSocSymbolManager(
        QObject *parent = nullptr, QSocProjectManager *projectManager = nullptr);

public slots:
    /**
     * @brief   Import symbol files from file list
     * @param   symbolNameRegex regular expression to match the symbol name
     * @param   directoryPath directory path
     * @details This function will import symbol files from file list.
     */
    bool importFromFileList(
        const QRegularExpression &symbolNameRegex,
        const QString            &fileListPath,
        const QStringList        &filePathList);

private:
    QSocProjectManager *projectManager;

signals:
};

#endif // QSOCSYMBOLMANAGER_H
