#include "cli/qsoccliworker.h"

#include "common/qslangdriver.h"
#include "common/qstaticlog.h"

#include <QString>
#include <QTimer>
#include <QtGlobal>

QSocCliWorker::QSocCliWorker(QObject *parent)
    : QObject(parent)
{
    /* Set up application name and version */
    QCoreApplication::setApplicationName("socstuido");
    QCoreApplication::setApplicationVersion("1.0.0");
    /* Set up command line parser */
    parser.setApplicationDescription(
        QCoreApplication::translate("main", "Generate SoC components via the command line."));
    parser.setOptionsAfterPositionalArgumentsMode(QCommandLineParser::ParseAsOptions);
}

QSocCliWorker::~QSocCliWorker() {}

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

bool QSocCliWorker::showError(int exitCode, const QString &message)
{
    qCritical().noquote() << message;
    qCritical().noquote() << QCoreApplication::applicationName()
                          << QCoreApplication::applicationVersion();
    qCritical().noquote() << parser.helpText();
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
        result = showError(exitCode, message);
    }
    return result;
}

bool QSocCliWorker::parseRoot(const QStringList &appArguments)
{
    /* Set up command line options */
    parser.addOptions({
        {{"h", "help"},
         QCoreApplication::translate("main", "Displays help on commandline options.")},
        {{"l", "level"},
         QCoreApplication::translate(
             "main",
             "Set log level. 0 is silent, 1 is error, 2 is warning, 3 is "
             "info, 4 is debug, 5 is verbose."),
         "level"},
        {{"v", "version"}, QCoreApplication::translate("main", "Displays version information.")},
    });
    parser.addPositionalArgument(
        "command",
        QCoreApplication::translate(
            "main",
            "gui         Start the software in GUI mode.\n"
            "project     Create, update of project.\n"
            "symbol      Import, update of symbol.\n"
            "schematic   Processing of Schematic.\n"
            "generate    Generate rtl, such as verilog, etc.\n"),
        "<command> [command options]");
    parser.parse(appArguments);
    /* Set debug level as early as possible */
    if (parser.isSet("level")) {
        QStaticLog::setLevel(parser.value("level").toInt());
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
    } else if (command == "symbol") {
        nextArguments.removeOne(command);
        if (!parseSymbol(nextArguments)) {
            return false;
        }
    } else if (command == "schematic") {
        return showError(
            1, QCoreApplication::translate("main", "Error: not implemented schematic yet."));
    } else if (command == "generate") {
        return showError(
            1, QCoreApplication::translate("main", "Error: not implemented generate yet."));
    } else {
        return showHelpOrError(
            1, QCoreApplication::translate("main", "Error: unknown subcommand: %1.").arg(command));
    }
    parser.process(*QCoreApplication::instance());
    return true;
}
