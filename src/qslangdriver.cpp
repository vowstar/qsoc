#include "qslangdriver.h"

#include <QFile>
#include <QProcess>
#include <QTemporaryFile>
#include <QTextStream>
#include <QRegularExpression>

#include <fmt/core.h>
#include <slang/ast/ASTSerializer.h>
#include <slang/ast/symbols/CompilationUnitSymbols.h>
#include <slang/diagnostics/TextDiagnosticClient.h>
#include <slang/driver/Driver.h>
#include <slang/syntax/SyntaxTree.h>
#include <slang/text/Json.h>
#include <slang/util/TimeTrace.h>
#include <slang/util/Version.h>

#include "qstaticlog.h"

QSlangDriver::QSlangDriver(QObject *parent)
    : QObject(parent)
{
    /* Get system environments */
    QStringList envList = QProcess::systemEnvironment();

    /* Save system environments into QMap */
    foreach (QString str, envList) {
        QStringList keyAndValue = str.split('=');
        if (keyAndValue.size() == 2) {
            env[keyAndValue[0]] = keyAndValue[1];
        }
    }
}

QSlangDriver::~QSlangDriver()
{
}

void QSlangDriver::setEnv(const QString &key, const QString &value)
{
    env[key] = value;
}

void QSlangDriver::setEnv(const QMap<QString, QString>& env)
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

        result = driver.parseCommandLine(slang::string_view(args.toStdString()));
        result = driver.processOptions();
        result = driver.parseAllSources();
        driver.reportMacros();
        result = driver.reportParseDiags();

        QStaticLog::logI(Q_FUNC_INFO, slang::OS::capturedStdout.c_str());
        QStaticLog::logE(Q_FUNC_INFO, slang::OS::capturedStderr.c_str());

        auto compilation = driver.createCompilation();
        slang::OS::capturedStdout.clear();
        slang::OS::capturedStderr.clear();
        result = driver.reportCompilation(*compilation, false);
        QStaticLog::logI(Q_FUNC_INFO, slang::OS::capturedStdout.c_str());
        QStaticLog::logE(Q_FUNC_INFO, slang::OS::capturedStderr.c_str());
        slang::JsonWriter writer;
        writer.setPrettyPrint(true);

        std::vector<std::string> astJsonScopes;

        slang::ast::ASTSerializer serializer(*compilation, writer);
        if (astJsonScopes.empty()) {
            serializer.serialize(compilation->getRoot());
        }
        else {
            for (auto& scopeName : astJsonScopes) {
                auto sym = compilation->getRoot().lookupName(scopeName);
                if (sym)
                    serializer.serialize(*sym);
            }
        }

        QStaticLog::logI(Q_FUNC_INFO, std::string(writer.view()).c_str());
    }
    catch (const std::exception& e) {
        /* Handle error */
        QStaticLog::logE(Q_FUNC_INFO, e.what());
    }
    return result;
}

bool QSlangDriver::parseFileList(const QString &fileListName)
{
    bool result = false;
    /* Read text from filelist */ 
    QFile inputFile(fileListName);
    if (inputFile.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        QTextStream inputStream(&inputFile);
        QString content = inputStream.readAll();

        /* Remove single line comment */
        content.remove(QRegularExpression("\\s*//[^\\n]*\\s*"));
        /* Remove multiline comments */
        content.remove(QRegularExpression("\\s*/\\*.*?\\*/\\s*"));
        /* Remove empty lines */
        content.remove(QRegularExpression("\\n\\s*\\n"));

        /* Replace environment variables */
        QMapIterator<QString, QString> it(env);
        while (it.hasNext()) {
            it.next();
            QString pattern = QString("${%1}").arg(it.key());
            content.replace(pattern, it.value());
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

            QString args = "slang -f \"" + tempFile.fileName() + "\" --ignore-unknown-modules --single-unit --compat vcs --error-limit=0";

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