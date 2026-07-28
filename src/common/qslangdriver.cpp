// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "common/qslangdriver.h"
#include "common/qsocconsole.h"
#include "common/qstaticstringweaver.h"

#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <QTextStream>

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <string>

#include <slang/analysis/AnalysisManager.h>
#include <slang/ast/ASTSerializer.h>
#include <slang/ast/Compilation.h>
#include <slang/ast/expressions/MiscExpressions.h>
#include <slang/ast/symbols/CompilationUnitSymbols.h>
#include <slang/ast/symbols/ValueSymbol.h>
#include <slang/diagnostics/TextDiagnosticClient.h>
#include <slang/driver/Driver.h>
#include <slang/syntax/AllSyntax.h>
#include <slang/syntax/SyntaxTree.h>
#include <slang/syntax/SyntaxVisitor.h>
#include <slang/text/Json.h>
#include <slang/util/String.h>
#include <slang/util/TimeTrace.h>
#include <slang/util/VersionInfo.h>

struct QSlangDriver::Session
{
    slang::driver::Driver                    driver;
    std::unique_ptr<slang::ast::Compilation> compilation;
};

namespace {

std::mutex slangMutex;

} // namespace

QSlangDriver::QSlangDriver(QObject *parent, QSocProjectManager *projectManager)
    : QObject(parent)
    , projectManager(projectManager)
{
    /* All private members set by constructor */
}

QSlangDriver::~QSlangDriver()
{
    const std::scoped_lock slangLock(slangMutex);
    clearParseState();
}

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

void QSlangDriver::clearParseState()
{
    session.reset();
    ast = json{};
    moduleList.clear();
}

bool QSlangDriver::parseArgs(const QString &args, bool silent)
{
    return parseArgsImpl(args, silent, nullptr);
}

bool QSlangDriver::parseArgsImpl(const QString &args, bool silent, QString *diagnosticOutput)
{
    const std::scoped_lock slangLock(slangMutex);
    clearParseState();

    slang::OS::setStderrColorsEnabled(false);
    slang::OS::setStdoutColorsEnabled(false);

    auto guard = slang::OS::captureOutput();

    auto  candidateSession = std::make_unique<Session>();
    auto &driver           = candidateSession->driver;
    driver.addStandardArgs();

    bool result = false;
    try {
        if (!silent) {
            QSocConsole::debug().noquote().nospace() << Q_FUNC_INFO << ":" << "Arguments:" + args;
        }
        slang::OS::capturedStdout.clear();
        slang::OS::capturedStderr.clear();
        if (!driver.parseCommandLine(std::string_view(args.toStdString()))) {
            if (!silent) {
                if (!slang::OS::capturedStdout.empty()) {
                    QSocConsole::errorPlain() << slang::OS::capturedStdout.c_str();
                }
                if (!slang::OS::capturedStderr.empty()) {
                    QSocConsole::errorPlain() << slang::OS::capturedStderr.c_str();
                }
            }
            throw std::runtime_error("Failed to parse command line");
        }
        slang::OS::capturedStdout.clear();
        slang::OS::capturedStderr.clear();
        if (!driver.processOptions()) {
            if (!silent) {
                if (!slang::OS::capturedStdout.empty()) {
                    QSocConsole::errorPlain() << slang::OS::capturedStdout.c_str();
                }
                if (!slang::OS::capturedStderr.empty()) {
                    QSocConsole::errorPlain() << slang::OS::capturedStderr.c_str();
                }
            }
            throw std::runtime_error("Failed to process options");
        }
        slang::OS::capturedStdout.clear();
        slang::OS::capturedStderr.clear();
        if (!driver.parseAllSources()) {
            if (!silent) {
                if (!slang::OS::capturedStdout.empty()) {
                    QSocConsole::errorPlain() << slang::OS::capturedStdout.c_str();
                }
                if (!slang::OS::capturedStderr.empty()) {
                    QSocConsole::errorPlain() << slang::OS::capturedStderr.c_str();
                }
            }
            throw std::runtime_error("Failed to parse sources");
        }
        slang::OS::capturedStdout.clear();
        slang::OS::capturedStderr.clear();
        driver.reportMacros();
        if (!silent) {
            QSocConsole::infoPlain() << slang::OS::capturedStdout.c_str();
        }
        slang::OS::capturedStdout.clear();
        slang::OS::capturedStderr.clear();
        candidateSession->compilation = driver.createCompilation();
        driver.reportCompilation(*candidateSession->compilation, false);
        driver.runAnalysis(*candidateSession->compilation);
        if (!driver.reportDiagnostics(false)) {
            if (!silent) {
                if (!slang::OS::capturedStdout.empty()) {
                    QSocConsole::errorPlain() << slang::OS::capturedStdout.c_str();
                }
                if (!slang::OS::capturedStderr.empty()) {
                    QSocConsole::errorPlain() << slang::OS::capturedStderr.c_str();
                }
            }
            throw std::runtime_error("Failed to report compilation");
        }
        if (!silent) {
            QSocConsole::infoPlain() << slang::OS::capturedStdout.c_str();
        }

        slang::JsonWriter         writer;
        slang::ast::ASTSerializer serializer(*candidateSession->compilation, writer);

        serializer.serialize(candidateSession->compilation->getRoot());

        const json::parser_callback_t callback =
            [](int depth, json::parse_event_t /*event*/, json & /*parsed*/) -> bool {
            return depth <= 6;
        };
        json candidateAst = json::parse(std::string(writer.view()), callback);
        if (!silent) {
            QSocConsole::debugPlain() << candidateAst.dump(4).c_str();
        }
        session = std::move(candidateSession);
        ast     = std::move(candidateAst);
        result  = true;
    } catch (const std::exception &e) {
        clearParseState();
        /* Handle error */
        if (!silent) {
            QSocConsole::error().noquote().nospace() << Q_FUNC_INFO << ":" << e.what();
        }
    }
    if (diagnosticOutput) {
        *diagnosticOutput = QString::fromStdString(slang::OS::capturedStderr);
    }
    return result;
}

bool QSlangDriver::parseFileList(
    const QString     &fileListPath,
    const QStringList &filePathList,
    const QStringList &macroDefines,
    const QStringList &macroUndefines)
{
    return parseFileList(
        fileListPath, filePathList, macroDefines, macroUndefines, UnknownModulePolicy::Allow);
}

bool QSlangDriver::parseFileList(
    const QString      &fileListPath,
    const QStringList  &filePathList,
    const QStringList  &macroDefines,
    const QStringList  &macroUndefines,
    UnknownModulePolicy unknownModulePolicy)
{
    {
        const std::scoped_lock slangLock(slangMutex);
        clearParseState();
    }

    bool    result  = false;
    QString content = "";
    if (!QFileInfo::exists(fileListPath) && filePathList.isEmpty()) {
        QSocConsole::error().noquote().nospace()
            << Q_FUNC_INFO
            << ":File path parameter is empty, also the file list path not exist:" << fileListPath;
    } else {
        /* Process read file list path */
        if (QFileInfo::exists(fileListPath)) {
            QSocConsole::debug().noquote().nospace()
                << Q_FUNC_INFO << ":" << "Use file list path:" + fileListPath;
            /* Read text from filelist */
            QFile inputFile(fileListPath);
            if (inputFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QTextStream inputStream(&inputFile);
                content = inputStream.readAll();
            } else {
                QSocConsole::error().noquote().nospace()
                    << Q_FUNC_INFO << ":" << "Failed to open file list:" + fileListPath;
            }
        }
        /* Process append of file path list */
        if (!filePathList.isEmpty()) {
            QSocConsole::debug().noquote().nospace()
                << Q_FUNC_INFO << ":" << "Use file path list:" + filePathList.join(",");
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
            if (unknownModulePolicy == UnknownModulePolicy::Allow) {
                baseArgs += " --ignore-unknown-modules";
            }
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
            const QString &args = baseArgs;
            /* clang-format on */

            QSocConsole::debug().noquote().nospace()
                << Q_FUNC_INFO << ":" << "TemporaryFile name:" + tempFile.fileName();
            QSocConsole::debug().noquote().nospace() << Q_FUNC_INFO << ":" << "Content list begin";
            QSocConsole::debug().noquote().nospace()
                << Q_FUNC_INFO << ":" << content.toStdString().c_str();
            QSocConsole::debug().noquote().nospace() << Q_FUNC_INFO << ":" << "Content list end";
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

QSet<QString> QSlangDriver::extractAllIdentifiers(const QString &verilogCode)
{
    const std::scoped_lock slangLock(slangMutex);
    QSet<QString>          identifiers;

    /* Create a temporary syntax tree */
    auto tree = slang::syntax::SyntaxTree::fromText(verilogCode.toStdString());

    /* Recursive function to collect all identifiers */
    std::function<void(const slang::syntax::SyntaxNode &)> traverse =
        [&](const slang::syntax::SyntaxNode &node) {
            if (node.kind == slang::syntax::SyntaxKind::IdentifierName) {
                auto   &idName     = node.as<slang::syntax::IdentifierNameSyntax>();
                QString signalName = QString::fromStdString(
                    std::string(idName.identifier.valueText()));
                identifiers.insert(signalName);
            }

            /* Recursively visit children */
            for (uint32_t i = 0; i < node.getChildCount(); i++) {
                auto child = node.childNode(i);
                if (child) {
                    traverse(*child);
                }
            }
        };

    traverse(tree->root());
    return identifiers;
}

QMap<QString, int> QSlangDriver::extractBitWidthRequirements(const QString &verilogCode)
{
    const std::scoped_lock slangLock(slangMutex);
    QMap<QString, int>     bitWidths;

    /* Create a temporary syntax tree to analyze bit selections */
    auto tree = slang::syntax::SyntaxTree::fromText(verilogCode.toStdString());

    /* Helper to extract integer value from literal expressions */
    std::function<int(slang::syntax::ExpressionSyntax *)> extractConstantValue =
        [](slang::syntax::ExpressionSyntax *expr) -> int {
        if (!expr)
            return -1;

        if (expr->kind == slang::syntax::SyntaxKind::IntegerLiteralExpression) {
            auto &literal = expr->as<slang::syntax::LiteralExpressionSyntax>();
            try {
                std::string valueStr(literal.literal.valueText());
                return std::stoi(valueStr);
            } catch (...) {
                return -1;
            }
        }

        return -1;
    };

    /* Recursive function to traverse syntax tree */
    std::function<void(const slang::syntax::SyntaxNode &)> traverse =
        [&](const slang::syntax::SyntaxNode &node) {
            /* Check if this is range or bit select */
            if (node.kind == slang::syntax::SyntaxKind::SimpleRangeSelect
                || node.kind == slang::syntax::SyntaxKind::AscendingRangeSelect
                || node.kind == slang::syntax::SyntaxKind::DescendingRangeSelect) {
                auto &rangeSelect = node.as<slang::syntax::RangeSelectSyntax>();

                int leftIdx  = extractConstantValue(rangeSelect.left);
                int rightIdx = extractConstantValue(rangeSelect.right);

                /* Find the identifier this range applies to */
                auto parent = node.parent;
                while (parent) {
                    if (parent->kind == slang::syntax::SyntaxKind::IdentifierSelectName) {
                        auto &selectName = parent->as<slang::syntax::IdentifierSelectNameSyntax>();
                        QString signalName = QString::fromStdString(
                            std::string(selectName.identifier.valueText()));

                        if (leftIdx >= 0 && rightIdx >= 0) {
                            int highBit       = std::max(leftIdx, rightIdx);
                            int requiredWidth = highBit + 1;
                            if (!bitWidths.contains(signalName)
                                || bitWidths[signalName] < requiredWidth) {
                                bitWidths[signalName] = requiredWidth;
                            }
                        }
                        break;
                    }
                    parent = parent->parent;
                }
            } else if (node.kind == slang::syntax::SyntaxKind::BitSelect) {
                auto &bitSelect = node.as<slang::syntax::BitSelectSyntax>();
                int   bitIndex  = extractConstantValue(bitSelect.expr);

                /* Find the identifier this bit select applies to */
                auto parent = node.parent;
                while (parent) {
                    if (parent->kind == slang::syntax::SyntaxKind::IdentifierSelectName) {
                        auto &selectName = parent->as<slang::syntax::IdentifierSelectNameSyntax>();
                        QString signalName = QString::fromStdString(
                            std::string(selectName.identifier.valueText()));

                        if (bitIndex >= 0) {
                            int requiredWidth = bitIndex + 1;
                            if (!bitWidths.contains(signalName)
                                || bitWidths[signalName] < requiredWidth) {
                                bitWidths[signalName] = requiredWidth;
                            }
                        }
                        break;
                    }
                    parent = parent->parent;
                }
            }

            /* Recursively visit children */
            for (uint32_t i = 0; i < node.getChildCount(); i++) {
                auto child = node.childNode(i);
                if (child) {
                    traverse(*child);
                }
            }
        };

    traverse(tree->root());

    return bitWidths;
}

bool QSlangDriver::parseVerilogSnippet(const QString &verilogCode, bool wrapInModule)
{
    {
        const std::scoped_lock slangLock(slangMutex);
        clearParseState();
    }

    /* If no wrapping needed, directly parse */
    if (!wrapInModule) {
        QTemporaryFile tempFile("qsoc_snippet_XXXXXX.v");
        tempFile.setAutoRemove(false);
        if (!tempFile.open()) {
            return false;
        }
        QTextStream(&tempFile) << verilogCode;
        tempFile.flush();
        tempFile.close();

        QString args
            = QString("slang --single-unit --ignore-unknown-modules %1").arg(tempFile.fileName());
        bool result = parseArgs(args);
        tempFile.remove();
        return result;
    }

    /* Two-pass approach for wrapped code */
    /* Pass 1: Try parsing to collect undeclared identifiers */
    QString wrappedCode = QString("module __qsoc_temp_parse__;\n%1\nendmodule\n").arg(verilogCode);

    QTemporaryFile tempFile1("qsoc_snippet_pass1_XXXXXX.v");
    tempFile1.setAutoRemove(false);
    if (!tempFile1.open()) {
        return false;
    }
    QTextStream(&tempFile1) << wrappedCode;
    tempFile1.flush();
    tempFile1.close();

    /* Try first parse - may fail, use silent mode to suppress expected errors during probing */
    QString args1
        = QString("slang --single-unit --ignore-unknown-modules %1").arg(tempFile1.fileName());
    QString stderrOutput;
    bool    firstPassResult = parseArgsImpl(args1, true /* silent */, &stderrOutput);

    if (firstPassResult) {
        /* Parsing succeeded, no need for second pass */
        tempFile1.remove();
        return true;
    }

    /* First pass failed, continue to extract undeclared identifiers */

    /* Extract undeclared identifiers from error messages */
    QSet<QString>                   undeclaredIds;
    static const QRegularExpression undefRe(R"(use of undeclared identifier '([^']+)')");
    QRegularExpressionMatchIterator it = undefRe.globalMatch(stderrOutput);
    while (it.hasNext()) {
        undeclaredIds.insert(it.next().captured(1));
    }

    tempFile1.remove();

    /* Analyze bit width requirements from syntax */
    QMap<QString, int> bitWidths = extractBitWidthRequirements(verilogCode);

    /* Add all signals with bit selections (they must be valid signal names, not keywords) */
    for (const QString &signal : bitWidths.keys()) {
        undeclaredIds.insert(signal);
    }

    /* Pass 2: Generate declarations with appropriate widths */
    QStringList declarationList;
    for (const QString &id : undeclaredIds) {
        int     width = bitWidths.value(id, 0);
        QString declaration;
        if (width > 1) {
            /* Multi-bit signal: use [N-1:0] format */
            declaration = QString("    logic [%1:0] %2;").arg(width - 1).arg(id);
        } else if (width == 1) {
            /* Single bit accessed with index [0] - must use [0:0] not scalar */
            declaration = QString("    logic [0:0] %1;").arg(id);
        } else {
            /* No bit selection: declare as scalar (1-bit) */
            declaration = QString("    logic %1;").arg(id);
        }
        declarationList.append(declaration);
    }
    declarationList.sort();
    QString declarations = declarationList.join("\n") + "\n";

    QString finalCode
        = QString("module __qsoc_temp_parse__;\n%1%2\nendmodule\n").arg(declarations, verilogCode);

    QTemporaryFile tempFile2("qsoc_snippet_pass2_XXXXXX.v");
    tempFile2.setAutoRemove(false);
    if (!tempFile2.open()) {
        return false;
    }
    QTextStream(&tempFile2) << finalCode;
    tempFile2.flush();
    tempFile2.close();

    QString args2
        = QString("slang --single-unit --ignore-unknown-modules %1").arg(tempFile2.fileName());
    bool result = parseArgs(args2);
    tempFile2.remove();
    return result;
}

QSet<QString> QSlangDriver::extractSignalReferences(const QSet<QString> &excludeSignals)
{
    const std::scoped_lock slangLock(slangMutex);
    QSet<QString>          signalSet;

    if (!session || !session->compilation) {
        QSocConsole::warn().noquote().nospace() << Q_FUNC_INFO << ":" << "No compilation available";
        return signalSet;
    }

    std::function<void(const json &)> extractFromJson = [&](const json &node) {
        if (node.is_object()) {
            if (node.contains("kind") && node.contains("name")) {
                const std::string kind = node["kind"];
                if (kind == "Variable" || kind == "Net" || kind == "NamedValue"
                    || kind == "NamedValueExpression") {
                    const QString signal = QString::fromStdString(node["name"].get<std::string>());
                    if (!signal.isEmpty() && !signal.startsWith("__")
                        && !excludeSignals.contains(signal)) {
                        signalSet.insert(signal);
                    }
                }
            }
            for (auto it = node.begin(); it != node.end(); ++it) {
                extractFromJson(it.value());
            }
        } else if (node.is_array()) {
            for (const auto &element : node) {
                extractFromJson(element);
            }
        }
    };
    extractFromJson(ast);

    return signalSet;
}
