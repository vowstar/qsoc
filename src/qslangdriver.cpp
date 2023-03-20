#include "qslangdriver.h"

#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <QTextStream>

#include <fmt/core.h>
#include <slang/ast/ASTSerializer.h>
#include <slang/ast/symbols/CompilationUnitSymbols.h>
#include <slang/diagnostics/TextDiagnosticClient.h>
#include <slang/driver/Driver.h>
#include <slang/syntax/SyntaxTree.h>
#include <slang/text/Json.h>
#include <slang/util/TimeTrace.h>
#include <slang/util/Version.h>
#include <stdexcept>

#include "qstaticlog.h"

QSlangDriver::QSlangDriver(QObject *parent)
    : QObject(parent)
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
}

QSlangDriver::~QSlangDriver() {}

void QSlangDriver::setEnv(const QString &key, const QString &value)
{
    env[key] = value;
}

void QSlangDriver::setEnv(const QMap<QString, QString> &env)
{
    this->env = env;
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
        if (!driver.parseCommandLine(slang::string_view(args.toStdString()))) {
            QStaticLog::logE(Q_FUNC_INFO, slang::OS::capturedStdout.c_str());
            throw std::runtime_error("Failed to parse command line");
        }
        slang::OS::capturedStdout.clear();
        slang::OS::capturedStderr.clear();
        if (!driver.processOptions()) {
            QStaticLog::logE(Q_FUNC_INFO, slang::OS::capturedStdout.c_str());
            throw std::runtime_error("Failed to process options");
        }
        slang::OS::capturedStdout.clear();
        slang::OS::capturedStderr.clear();
        if (!driver.parseAllSources()) {
            QStaticLog::logE(Q_FUNC_INFO, slang::OS::capturedStdout.c_str());
            throw std::runtime_error("Failed to parse sources");
        }
        slang::OS::capturedStdout.clear();
        slang::OS::capturedStderr.clear();
        driver.reportMacros();
        QStaticLog::logI(Q_FUNC_INFO, slang::OS::capturedStdout.c_str());
        slang::OS::capturedStdout.clear();
        slang::OS::capturedStderr.clear();
        if (!driver.reportParseDiags()) {
            QStaticLog::logE(Q_FUNC_INFO, slang::OS::capturedStdout.c_str());
            throw std::runtime_error("Failed to report parse diagnostics");
        }
        slang::OS::capturedStdout.clear();
        slang::OS::capturedStderr.clear();
        auto compilation = driver.createCompilation();
        if (!driver.reportCompilation(*compilation, false)) {
            QStaticLog::logE(Q_FUNC_INFO, slang::OS::capturedStdout.c_str());
            throw std::runtime_error("Failed to report compilation");
        }
        result = true;
        QStaticLog::logI(Q_FUNC_INFO, slang::OS::capturedStdout.c_str());

        slang::JsonWriter writer;
        writer.setPrettyPrint(true);

        const std::vector<std::string> astJsonScopes;

        slang::ast::ASTSerializer serializer(*compilation, writer);
        if (astJsonScopes.empty()) {
            serializer.serialize(compilation->getRoot());
        } else {
            for (const auto &scopeName : astJsonScopes) {
                const auto *sym = compilation->getRoot().lookupName(scopeName);
                if (sym)
                    serializer.serialize(*sym);
            }
        }

        QStaticLog::logI(Q_FUNC_INFO, std::string(writer.view()).c_str());
    } catch (const std::exception &e) {
        /* Handle error */
        QStaticLog::logE(Q_FUNC_INFO, e.what());
    }
    return result;
}

bool QSlangDriver::parseFileList(const QString &fileListName)
{
    bool result = false;
    QStaticLog::logD(Q_FUNC_INFO, "Use file list:" + fileListName);
    /* Read text from filelist */
    QFile inputFile(fileListName);
    if (inputFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream inputStream(&inputFile);
        QString content = inputStream.readAll();

        /* Remove single line comment */
        content.remove(QRegularExpression(R"(\s*//[^\n]*\s*)"));
        /* Remove multiline comments */
        content.remove(QRegularExpression(R"(\s*/\*.*?\*/\s*)"));
        /* Remove empty lines */
        content.remove(QRegularExpression(R"(\n\s*\n)"));

        /* Replace environment variables */
        QMapIterator<QString, QString> iterator(env);
        while (iterator.hasNext()) {
            iterator.next();
            const QString pattern = QString("${%1}").arg(iterator.key());
            content.replace(pattern, iterator.value());
        }

        /* Create a temporary file */
        QTemporaryFile tempFile("socstudio");
        /* Do not remove file after close */
        tempFile.setAutoRemove(false);
        if (tempFile.open()) {
            /* Write new content to temporary file */
            QTextStream outputStream(&tempFile);
            outputStream << content;
            tempFile.flush();
            tempFile.close();

            const QString args
                = "slang -f \"" + tempFile.fileName()
                  + "\" --ignore-unknown-modules --single-unit --compat vcs --error-limit=0";

            QStaticLog::logV(Q_FUNC_INFO, "TemporaryFile name:" + tempFile.fileName());
            QStaticLog::logV(Q_FUNC_INFO, "Content list begin");
            QStaticLog::logV(Q_FUNC_INFO, content.toStdString().c_str());
            QStaticLog::logV(Q_FUNC_INFO, "Content list end");
            result = parseArgs(args);
            /* Delete temporary file */
            tempFile.remove();
        }
    } else {
        QStaticLog::logE(Q_FUNC_INFO, "Failed to open file list:" + fileListName);
    }

    return result;
}