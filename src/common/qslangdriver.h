// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#ifndef QSLANGDRIVER_H
#define QSLANGDRIVER_H

#include "common/qsocprojectmanager.h"

#include <memory>
#include <QDir>
#include <QObject>
#include <QString>
#include <QStringList>

#include <nlohmann/json.hpp>
#include <slang/ast/Compilation.h>
#include <slang/ast/symbols/InstanceSymbols.h>

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
    ~QSlangDriver() override;

public slots:
    /**
     * @brief Set the project manager.
     * @details Assigns a new project manager to this object. The project
     *          manager is used for managing various project-related
     *          functionalities.
     * @param projectManager Pointer to the new project manager.
     */
    void setProjectManager(QSocProjectManager *projectManager);

    /**
     * @brief Get the project manager.
     * @details Retrieves the currently assigned project manager. This manager
     *          is responsible for handling various aspects of the project.
     * @return QSocProjectManager * Pointer to the current project manager.
     */
    QSocProjectManager *getProjectManager();
    /**
     * @brief Parse command line arguments.
     * @details This function will parse command line arguments.
     * @param args command line arguments.
     * @param silent if true, suppress error logging (for probing/discovery).
     * @retval true Parse successfully.
     * @retval false Parse failed.
     */
    bool parseArgs(const QString &args, bool silent = false);

    /**
     * @brief Parse file list.
     * @details This function will parse file list.
     * @param fileListPath file list path.
     * @param filePathList file path list.
     * @param macroDefines macro definitions in KEY or KEY=VALUE format.
     * @param macroUndefines macro names to undefine.
     * @retval true Parse successfully.
     * @retval false Parse failed.
     */
    bool parseFileList(
        const QString     &fileListPath,
        const QStringList &filePathList,
        const QStringList &macroDefines   = QStringList(),
        const QStringList &macroUndefines = QStringList());

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
     * @brief Parse Verilog code snippet
     * @details This function parses a Verilog code snippet by wrapping it in a
     *          minimal module structure. Useful for syntax checking code fragments
     *          from comb/seq primitives.
     * @param verilogCode Verilog code snippet (can be incomplete module)
     * @param wrapInModule If true, wrap snippet in a temporary module
     * @return true if parsing succeeded, false otherwise
     */
    bool parseVerilogSnippet(const QString &verilogCode, bool wrapInModule = true);

    /**
     * @brief Extract signal references from parsed AST
     * @details Traverses the AST to find all signal references (NamedValueExpression nodes).
     *          Use after parseVerilogSnippet() or parseFileList() to extract referenced signals.
     * @param excludeSignals Signals to exclude from the result (e.g., known outputs)
     * @return Set of signal names referenced in the code
     * @note Returns empty set if no compilation has been performed yet
     */
    QSet<QString> extractSignalReferences(const QSet<QString> &excludeSignals = {});

private:
    /**
     * @brief Extract bit width requirements from Verilog code syntax
     * @details Analyzes syntax tree to find bit selections like signal[N:M] or signal[N]
     *          and determines minimum required width for each signal
     * @param verilogCode Verilog code to analyze
     * @return Map of signal names to minimum required bit width
     */
    QMap<QString, int> extractBitWidthRequirements(const QString &verilogCode);

    /**
     * @brief Extract all identifiers from Verilog code syntax
     * @details Traverses syntax tree to find all identifier references
     * @param verilogCode Verilog code to analyze
     * @return Set of all identifier names found
     */
    QSet<QString> extractAllIdentifiers(const QString &verilogCode);

public:
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
     * @brief Filters and validates file paths in content.
     * @details This function processes each line of the input content as a file
     *          path, verifies if it exists as a regular file or valid symbolic
     *          link (not a directory), and returns absolute paths of valid
     *          files. Paths are resolved relative to the specified base
     *          directory.
     * @param content String containing potential file paths (one per line)
     * @param baseDir Base directory used for resolving relative paths
     * @return QString containing newline-separated absolute paths of valid
     *         files. Returns empty string if no valid files found.
     */
    QString contentValidFile(const QString &content, const QDir &baseDir);

private:
    /* Pointer of project manager. */
    QSocProjectManager *projectManager = nullptr;
    /* Compilation object to store parsing results */
    std::unique_ptr<slang::ast::Compilation> compilation = nullptr;
    /* Abstract Syntax Tree JSON data. */
    json ast;
    /* Module list. */
    QStringList moduleList;
};

#endif // QSLANGDRIVER_H
