// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "common/qslangdriver.h"
#include "common/qstaticlog.h"
#include "common/qstaticstringweaver.h"

#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <QTextStream>

#include <stdexcept>
#include <string>

#include <slang/ast/ASTSerializer.h>
#include <slang/ast/Compilation.h>
#include <slang/ast/symbols/CompilationUnitSymbols.h>
#include <slang/diagnostics/TextDiagnosticClient.h>
#include <slang/driver/Driver.h>
#include <slang/syntax/SyntaxTree.h>
#include <slang/text/Json.h>
#include <slang/util/String.h>
#include <slang/util/TimeTrace.h>
#include <slang/util/VersionInfo.h>

QSlangDriver::QSlangDriver(QObject *parent, QSocProjectManager *projectManager)
    : QObject(parent)
    , projectManager(projectManager)
{
    /* All private members set by constructor */
}

QSlangDriver::~QSlangDriver() = default;

void QSlangDriver::setProjectManager(QSocProjectManager *projectManager)
{
    /* Set projectManager */
    if (projectManager) {
        this->projectManager = projectManager;
    }
}

QSocProjectManager *QSlangDriver::getProjectManager()
{
    return projectManager;
}

bool QSlangDriver::parseArgs(const QString &args)
{
    slang::OS::setStderrColorsEnabled(false);
    slang::OS::setStdoutColorsEnabled(false);

    auto guard = slang::OS::captureOutput();

    slang::driver::Driver driver;
    driver.addStandardArgs();

    bool result = false;
    try {
        QStaticLog::logV(Q_FUNC_INFO, "Arguments:" + args);
        slang::OS::capturedStdout.clear();
        slang::OS::capturedStderr.clear();
        if (!driver.parseCommandLine(std::string_view(args.toStdString()))) {
            if (!slang::OS::capturedStdout.empty()) {
                QStaticLog::logE(Q_FUNC_INFO, slang::OS::capturedStdout.c_str());
            }
            if (!slang::OS::capturedStderr.empty()) {
                QStaticLog::logE(Q_FUNC_INFO, slang::OS::capturedStderr.c_str());
            }
            throw std::runtime_error("Failed to parse command line");
        }
        slang::OS::capturedStdout.clear();
        slang::OS::capturedStderr.clear();
        if (!driver.processOptions()) {
            if (!slang::OS::capturedStdout.empty()) {
                QStaticLog::logE(Q_FUNC_INFO, slang::OS::capturedStdout.c_str());
            }
            if (!slang::OS::capturedStderr.empty()) {
                QStaticLog::logE(Q_FUNC_INFO, slang::OS::capturedStderr.c_str());
            }
            throw std::runtime_error("Failed to process options");
        }
        slang::OS::capturedStdout.clear();
        slang::OS::capturedStderr.clear();
        if (!driver.parseAllSources()) {
            if (!slang::OS::capturedStdout.empty()) {
                QStaticLog::logE(Q_FUNC_INFO, slang::OS::capturedStdout.c_str());
            }
            if (!slang::OS::capturedStderr.empty()) {
                QStaticLog::logE(Q_FUNC_INFO, slang::OS::capturedStderr.c_str());
            }
            throw std::runtime_error("Failed to parse sources");
        }
        slang::OS::capturedStdout.clear();
        slang::OS::capturedStderr.clear();
        driver.reportMacros();
        QStaticLog::logI(Q_FUNC_INFO, slang::OS::capturedStdout.c_str());
        slang::OS::capturedStdout.clear();
        slang::OS::capturedStderr.clear();
        if (!driver.reportParseDiags()) {
            if (!slang::OS::capturedStdout.empty()) {
                QStaticLog::logE(Q_FUNC_INFO, slang::OS::capturedStdout.c_str());
            }
            if (!slang::OS::capturedStderr.empty()) {
                QStaticLog::logE(Q_FUNC_INFO, slang::OS::capturedStderr.c_str());
            }
            throw std::runtime_error("Failed to report parse diagnostics");
        }
        slang::OS::capturedStdout.clear();
        slang::OS::capturedStderr.clear();
        /* Move the compilation object to class member */
        compilation = std::move(driver.createCompilation());
        if (!driver.reportCompilation(*compilation, false)) {
            if (!slang::OS::capturedStdout.empty()) {
                QStaticLog::logE(Q_FUNC_INFO, slang::OS::capturedStdout.c_str());
            }
            if (!slang::OS::capturedStderr.empty()) {
                QStaticLog::logE(Q_FUNC_INFO, slang::OS::capturedStderr.c_str());
            }
            throw std::runtime_error("Failed to report compilation");
        }
        result = true;
        QStaticLog::logI(Q_FUNC_INFO, slang::OS::capturedStdout.c_str());

        slang::JsonWriter         writer;
        slang::ast::ASTSerializer serializer(*compilation, writer);

        serializer.serialize(compilation->getRoot());

        /* Define a SAX callback to limit parsing depth */
        const json::parser_callback_t callback =
            [](int depth, json::parse_event_t /*event*/, json & /*parsed*/) -> bool {
            /* Skip parsing when depth exceeds 4 levels */
            return depth <= 6;
        };

        /* Parse JSON with depth limitation using callback */
        auto jsonStr = std::string(writer.view());
        ast          = json::parse(jsonStr, callback);

        /* Print partial AST */
        QStaticLog::logV(Q_FUNC_INFO, ast.dump(4).c_str());
    } catch (const std::exception &e) {
        /* Handle error */
        QStaticLog::logE(Q_FUNC_INFO, e.what());
    }
    return result;
}

bool QSlangDriver::parseFileList(
    const QString     &fileListPath,
    const QStringList &filePathList,
    const QStringList &macroDefines,
    const QStringList &macroUndefines)
{
    bool    result  = false;
    QString content = "";
    if (!QFileInfo::exists(fileListPath) && filePathList.isEmpty()) {
        QStaticLog::logE(
            Q_FUNC_INFO,
            "File path parameter is empty, also the file list path not exist:" + fileListPath);
    } else {
        /* Process read file list path */
        if (QFileInfo::exists(fileListPath)) {
            QStaticLog::logD(Q_FUNC_INFO, "Use file list path:" + fileListPath);
            /* Read text from filelist */
            QFile inputFile(fileListPath);
            if (inputFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QTextStream inputStream(&inputFile);
                content = inputStream.readAll();
            } else {
                QStaticLog::logE(Q_FUNC_INFO, "Failed to open file list:" + fileListPath);
            }
        }
        /* Process append of file path list */
        if (!filePathList.isEmpty()) {
            QStaticLog::logD(Q_FUNC_INFO, "Use file path list:" + filePathList.join(","));
            /* Append file path list to the end of content */
            content.append("\n" + filePathList.join("\n"));
        }
        /* Removes comments from the content */
        content = contentCleanComment(content);
        /* Substitute environment variables */
        if (projectManager) {
            const QMap<QString, QString>   env = projectManager->getEnv();
            QMapIterator<QString, QString> iterator(env);
            while (iterator.hasNext()) {
                iterator.next();
                /* Create pattern */
                const QString pattern = QString("${%1}").arg(iterator.key());
                /* Replace environment variable */
                content = content.replace(pattern, iterator.value());
            }
        }
        /* Convert relative path to absolute path */
        if (QFileInfo::exists(fileListPath)) {
            content = contentValidFile(content, QFileInfo(fileListPath).absoluteDir());
        }
        /* Create a temporary file */
        QTemporaryFile tempFile("qsoc.fl");
        /* Do not remove file after close */
        tempFile.setAutoRemove(false);
        if (tempFile.open()) {
            /* Write new content to temporary file */
            QTextStream outputStream(&tempFile);
            outputStream << content;
            tempFile.flush();
            tempFile.close();
            /* clang-format off */
            QString baseArgs = QStaticStringWeaver::stripCommonLeadingWhitespace(R"(
                slang
                --ignore-unknown-modules
                --single-unit
                --compat vcs
                --timescale 1ns/10ps
                --error-limit=0
                -Wunknown-sys-name
                -Wbitwise-op-mismatch
                -Wcomparison-mismatch
                -Wunconnected-port
                -Wsign-compare
                --ignore-directive delay_mode_path
                --ignore-directive suppress_faults
                --ignore-directive enable_portfaults
                --ignore-directive disable_portfaults
                --ignore-directive nosuppress_faults
                --ignore-directive delay_mode_distributed
                --ignore-directive delay_mode_unit
            )");
            /* Add macro definitions */
            for (const QString &macro : macroDefines) {
                baseArgs += QString(" -D\"%1\"").arg(macro);
            }
            /* Add macro undefines */
            for (const QString &macro : macroUndefines) {
                baseArgs += QString(" -U\"%1\"").arg(macro);
            }
            /* Add file list */
            baseArgs += QString(" -f \"%1\"").arg(tempFile.fileName());
            const QString args = baseArgs;
            /* clang-format on */

            QStaticLog::logV(Q_FUNC_INFO, "TemporaryFile name:" + tempFile.fileName());
            QStaticLog::logV(Q_FUNC_INFO, "Content list begin");
            QStaticLog::logV(Q_FUNC_INFO, content.toStdString().c_str());
            QStaticLog::logV(Q_FUNC_INFO, "Content list end");
            result = parseArgs(args);
            /* Delete temporary file */
            tempFile.remove();
        }
    }

    return result;
}

const json &QSlangDriver::getAst()
{
    return ast;
}

const json &QSlangDriver::getModuleAst(const QString &moduleName)
{
    if (ast.contains("members")) {
        for (const json &member : ast["members"]) {
            if (member.contains("kind") && member["kind"] == "Instance") {
                if (member.contains("name") && member["name"] == moduleName.toStdString()) {
                    return member;
                }
            }
        }
    }
    return ast;
}

const QStringList &QSlangDriver::getModuleList()
{
    /* Clear the module list before populating */
    moduleList.clear();
    if (ast.contains("members")) {
        for (const json &member : ast["members"]) {
            if (member.contains("kind") && member["kind"] == "Instance") {
                if (member.contains("name")) {
                    moduleList.append(QString::fromStdString(member["name"]));
                }
            }
        }
    }
    return moduleList;
}

QString QSlangDriver::contentCleanComment(const QString &content)
{
    QString result = content;
    /* Normalize line endings to Unix-style */
    result.replace(QRegularExpression(R"(\r\n|\r)"), "\n");
    /* Remove single line comment */
    result.remove(QRegularExpression(R"(\s*//[^\n]*\s*)"));
    /* Remove multiline comments */
    result.remove(QRegularExpression(R"(\s*/\*.*?\*/\s*)"));
    /* Remove empty lines */
    result.remove(QRegularExpression(R"(\n\s*\n)"));
    return result;
}

QString QSlangDriver::contentValidFile(const QString &content, const QDir &baseDir)
{
    QStringList result;
    /* Splitting content into lines, considering different newline characters */
    const QStringList lines = content.split(QRegularExpression(R"(\r\n|\n|\r)"), Qt::KeepEmptyParts);

    for (const QString &line : lines) {
        QString absolutePath = line;
        /* Check for relative path and convert it to absolute */
        if (QDir::isRelativePath(line)) {
            /* Convert relative path to absolute path */
            absolutePath = baseDir.filePath(line);
        } else {
            /* Preserve absolute paths and non-path content as is */
            absolutePath = line;
        }
        const QFileInfo fileInfo(absolutePath);
        /* Check if path exists and is a regular file (including valid symlinks to files) */
        if (fileInfo.exists() && fileInfo.isFile()) {
            result.append(absolutePath);
        }
    }
    return result.join("\n");
}
