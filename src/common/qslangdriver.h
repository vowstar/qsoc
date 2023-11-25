#ifndef QSLANGDRIVER_H
#define QSLANGDRIVER_H

#include "common/qsocprojectmanager.h"

#include <QDir>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

/**
 * @brief The QSlangDriver class.
 * @details This class is used to drive the slang verilog parser.
 */
class QSlangDriver : public QObject
{
    Q_OBJECT
public:
    /**
     * @brief Constructor for QSlangDriver.
     * @details This constructor will initialize the resources.
     * @param[in] parent parent object.
     * @param[in] projectManager project manager.
     */
    explicit QSlangDriver(QObject *parent = nullptr, QSocProjectManager *projectManager = nullptr);

    /**
     * @brief Destructor for QSlangDriver.
     * @details This destructor will free all the allocated resources.
     */
    ~QSlangDriver();

public slots:
    /**
     * @brief Parse command line arguments.
     * @details This function will parse command line arguments.
     * @param args command line arguments.
     * @retval true Parse successfully.
     * @retval false Parse failed.
     */
    bool parseArgs(const QString &args);

    /**
     * @brief Parse file list.
     * @details This function will parse file list.
     * @param fileListPath file list path.
     * @param filePathList file path list.
     * @retval true Parse successfully.
     * @retval false Parse failed.
     */
    bool parseFileList(const QString &fileListPath, const QStringList &filePathList);

    /**
     * @brief Get Abstract Syntax Tree.
     * @details This function will return the Abstract Syntax Tree
     *          of the parsed files.
     * @note The AST is in JSON format.
     * @return json & Abstract Syntax Tree.
     */
    const json &getAst();

    /**
     * @brief Get module Abstract Syntax Tree.
     * @details This function will return the Abstract Syntax Tree
     *          of the specified module.
     * @note The AST is in JSON format.
     * @param moduleName module name.
     * @return json & module Abstract Syntax Tree.
     */
    const json &getModuleAst(const QString &moduleName);

    /**
     * @brief Get module list.
     * @details This function will return the module list.
     * @return QStringList & The module list.
     */
    const QStringList &getModuleList();

    /**
     * @brief Removes comments from the content.
     * @details This function strips both single line and multiline comments
     *          from the input string. It normalizes line endings to Unix-style
     *          for cross-platform compatibility and removes empty lines.
     * @param content The string containing the content with comments.
     * @return QString The cleaned content string without comments.
     */
    QString contentCleanComment(const QString &content);

    /**
     * @brief Converts relative paths in content to absolute paths.
     * @details This function processes the given string and converts
     *          any relative file paths to absolute paths, using the
     *          specified base directory. Assumes path formats are
     *          appropriate for the operating system in use.
     * @param content String containing content with relative paths.
     * @param baseDir Base directory for resolving relative paths.
     * @return QString with modified content, featuring absolute paths.
     */
    QString contentRelativeToAbsolute(const QString &content, const QDir &baseDir);

private:
    /* Pointer of project manager. */
    QSocProjectManager *projectManager = nullptr;

    /* Abstract Syntax Tree JSON data. */
    json ast;

    /* Module list. */
    QStringList moduleList;
};

#endif // QSLANGDRIVER_H
