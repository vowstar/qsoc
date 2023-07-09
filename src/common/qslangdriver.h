#ifndef QSLANGDRIVER_H
#define QSLANGDRIVER_H

#include <QMap>
#include <QObject>

#include <nlohmann/json.hpp>

#include "qsocprojectmanager.h"

using json = nlohmann::json;

/**
 * @brief   The QSlangDriver class
 * @details This class is used to drive the slang verilog parser.
 */
class QSlangDriver : public QObject
{
    Q_OBJECT
public:
    /**
     * @brief Constructor for QSlangDriver
     * @param parent parent object
     * @param projectManager project manager
     * @details This constructor will initialize the resources
     */
    explicit QSlangDriver(QObject *parent = nullptr, QSocProjectManager *projectManager = nullptr);
    /**
     * @brief Destructor for QSlangDriver
     * @details This destructor will free all the allocated resources
     */
    ~QSlangDriver();

public slots:
    /**
     * @brief Parse command line arguments
     * @param args command line arguments
     * @return true if parse successfully, otherwise false
     * @details This function will parse command line arguments
     */
    bool parseArgs(const QString &args);
    /**
     * @brief Parse file list
     * @param fileListPath file list path
     * @param filePathList file path list
     * @return true if parse successfully, otherwise false
     * @details This function will parse file list
     */
    bool parseFileList(const QString &fileListPath, const QStringList &filePathList);
    /**
     * @brief Get Abstract Syntax Tree
     * @return Abstract Syntax Tree
     * @details This function will return the Abstract Syntax Tree
     *          of the parsed files
     * @note   The AST is in JSON format
     */
    const json &getAst();
    /**
     * @brief Get module Abstract Syntax Tree
     * @param moduleName module name
     * @return module Abstract Syntax Tree
     * @details This function will return the Abstract Syntax Tree
     *          of the specified module
     * @note   The AST is in JSON format
     */
    const json &getModuleAst(const QString &moduleName);
    /**
     * @brief Get module list
     * @return module list
     * @details This function will return the module list
     */
    const QStringList &getModuleList();

private:
    /* Pointer of project manager */
    QSocProjectManager *projectManager = nullptr;
    /* Abstract Syntax Tree JSON data */
    json ast;
    /* Module list */
    QStringList moduleList;
};

#endif // QSLANGDRIVER_H
