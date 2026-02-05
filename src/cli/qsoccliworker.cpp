// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"

#include "common/config.h"
#include "common/qslangdriver.h"
#include "common/qstaticlog.h"

#include <QString>
#include <QTimer>
#include <QtGlobal>

QSocCliWorker::QSocCliWorker(QObject *parent)
    : QObject(parent)
    , projectManager(new QSocProjectManager(this))
    , socConfig(new QSocConfig(this, projectManager))
    , llmService(new QLLMService(this, socConfig))
    , busManager(new QSocBusManager(this, projectManager))
    , moduleManager(new QSocModuleManager(this, projectManager, busManager, llmService))
    , generateManager(new QSocGenerateManager(this, projectManager, moduleManager, busManager))
{
    /* Set up application name and version */
    QCoreApplication::setApplicationName("QSoC");
    QCoreApplication::setApplicationVersion(QSOC_VERSION);
    /* Set up command line parser */
    parser.setApplicationDescription(
        QCoreApplication::translate("main", "Generate SoC components via the command line."));
    parser.setOptionsAfterPositionalArgumentsMode(QCommandLineParser::ParseAsOptions);
}

QSocCliWorker::~QSocCliWorker() = default;

void QSocCliWorker::setup(const QStringList &appArguments, bool isGui)
{
    /* Set up exit code */
    exitCode = 0;
    /* Set up command line arguments */
    this->cmdArguments = appArguments;
    /* Determine if we are in gui or cli mode */
    if (!isGui && QCoreApplication::instance()) {
        /* In cli, cause application to exit when finished */
        connect(
            this,
            &QSocCliWorker::exit,
            QCoreApplication::instance(),
            &QCoreApplication::exit,
            Qt::QueuedConnection);
        connect(
            this,
            &QSocCliWorker::quit,
            QCoreApplication::instance(),
            &QCoreApplication::quit,
            Qt::QueuedConnection);
    }
    /* This will run the task from the application event loop */
    QTimer::singleShot(0, this, SLOT(run()));
}

void QSocCliWorker::run()
{
    parseRoot(cmdArguments);
    emit exit(exitCode);
}

bool QSocCliWorker::showVersion(int exitCode)
{
    qInfo().noquote() << QCoreApplication::applicationName()
                      << QCoreApplication::applicationVersion();
    this->exitCode = exitCode;
    return true;
}

bool QSocCliWorker::showHelp(int exitCode)
{
    qInfo().noquote() << parser.helpText();
    this->exitCode = exitCode;
    return true;
}

bool QSocCliWorker::showErrorWithHelp(int exitCode, const QString &message)
{
    qCritical().noquote() << message;
    qCritical().noquote() << QCoreApplication::applicationName()
                          << QCoreApplication::applicationVersion();
    qCritical().noquote() << parser.helpText();
    this->exitCode = exitCode;
    return false;
}

bool QSocCliWorker::showError(int exitCode, const QString &message)
{
    qCritical().noquote() << message;
    this->exitCode = exitCode;
    return false;
}

bool QSocCliWorker::showInfo(int exitCode, const QString &message)
{
    qInfo().noquote() << message;
    this->exitCode = exitCode;
    return true;
}

bool QSocCliWorker::showHelpOrError(int exitCode, const QString &message)
{
    bool result = false;
    if (parser.isSet("help")) {
        result = showHelp(0);
    } else {
        result = showErrorWithHelp(exitCode, message);
    }
    return result;
}

bool QSocCliWorker::parseRoot(const QStringList &appArguments)
{
    /* Set up command line options */
    parser.addOptions({
        {{"h", "help"},
         QCoreApplication::translate("main", "Displays help on commandline options.")},
        {"verbose",
         QCoreApplication::translate(
             "main",
             "Verbosity level (0-5).\n"
             "Higher values increase output detail.\n"
             "0=silent, 1=error, 2=warning, 3=info, 4=debug, 5=verbose"),
         "level"},
        {{"v", "version"}, QCoreApplication::translate("main", "Displays version information.")},
    });
    parser.addPositionalArgument(
        "command",
        QCoreApplication::translate(
            "main",
            "gui         Start the software in GUI mode.\n"
            "project     Create, update of project.\n"
            "module      Import, update of module.\n"
            "bus         Import, update of bus.\n"
            "schematic   Processing of Schematic.\n"
            "generate    Generate rtl, such as verilog, etc.\n"
            "agent       Run interactive AI agent mode.\n"),
        "<command> [command options]");
    parser.parse(appArguments);
    /* Set verbosity level as early as possible */
    if (parser.isSet("verbose")) {
        const int level = parser.value("verbose").toInt();
        /* Check if the level is valid */
        if (level < QStaticLog::Level::Silent || level > QStaticLog::Level::Verbose) {
            return showErrorWithHelp(
                1,
                QCoreApplication::translate("main", "Error: invalid log level: %1.")
                    .arg(parser.value("verbose")));
        }
        /* Set log level */
        QStaticLog::setLevel(static_cast<QStaticLog::Level>(level));
    }
    /* version options have higher priority */
    if (parser.isSet("version")) {
        return showVersion(0);
    }
    const QStringList cmdArguments = parser.positionalArguments();
    if (cmdArguments.isEmpty()) {
        return showHelpOrError(1, QCoreApplication::translate("main", "Error: missing subcommand."));
    }
    /* Perform different operations according to different subcommands */
    const QString &command       = cmdArguments.first();
    QStringList    nextArguments = appArguments;
    if (command == "gui") {
        QStaticLog::logV(Q_FUNC_INFO, "Starting GUI ...");
    } else if (command == "project") {
        nextArguments.removeOne(command);
        if (!parseProject(nextArguments)) {
            return false;
        }
    } else if (command == "module") {
        nextArguments.removeOne(command);
        if (!parseModule(nextArguments)) {
            return false;
        }
    } else if (command == "bus") {
        nextArguments.removeOne(command);
        if (!parseBus(nextArguments)) {
            return false;
        }
    } else if (command == "schematic") {
        return showErrorWithHelp(
            1, QCoreApplication::translate("main", "Error: not implemented schematic yet."));
    } else if (command == "generate") {
        nextArguments.removeOne(command);
        if (!parseGenerate(nextArguments)) {
            return false;
        }
    } else if (command == "agent") {
        nextArguments.removeOne(command);
        if (!parseAgent(nextArguments)) {
            return false;
        }
    } else {
        return showHelpOrError(
            1, QCoreApplication::translate("main", "Error: unknown subcommand: %1.").arg(command));
    }
    parser.process(*QCoreApplication::instance());
    return true;
}
