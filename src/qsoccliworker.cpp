#include "qsoccliworker.h"

#include <QString>
#include <QTimer>
#include <QtGlobal>

#include "qslangdriver.h"
#include "qstaticlog.h"

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
    parser.addVersionOption();
}

QSocCliWorker::~QSocCliWorker() {}

void QSocCliWorker::setup(bool isGui)
{
    /* Determine if we are in gui or cli mode */
    if (!isGui) {
        /* In cli, cause application to exit when finished */
        QObject::connect(
            this, SIGNAL(exit()), QCoreApplication::instance(), SLOT(exit()), Qt::QueuedConnection);
        QObject::connect(
            this, SIGNAL(quit()), QCoreApplication::instance(), SLOT(quit()), Qt::QueuedConnection);
    }
    /* This will run the task from the application event loop */
    QTimer::singleShot(0, this, SLOT(run()));
}

void QSocCliWorker::processFileList(const QString &fileListPath, const QStringList &filePathList)
{
    QSlangDriver driver(this);
    if (driver.parseFileList(fileListPath, filePathList)) {
        /* Parse success */
        QStringList moduleList = driver.getModuleList();
        if (moduleList.isEmpty()) {
            qCritical() << "Error: no module found.";
        } else {
            qDebug() << "Found modules:" << moduleList;
            qDebug() << driver.getModuleAst(moduleList.first()).dump(4).c_str();
        }
    }
}

void QSocCliWorker::run()
{
    parseRoot(QCoreApplication::arguments());
    emit quit();
}

void QSocCliWorker::parseRoot(const QStringList &appArguments)
{
    parser.addOptions({
        {{"h", "help"},
         QCoreApplication::translate("main", "Displays help on commandline options.")},
        {{"c", "cli"},
         QCoreApplication::translate(
             "main", "Start the software in CLI mode, otherwise it will start in GUI mode.")},
        {{"l", "level"},
         QCoreApplication::translate(
             "main",
             "Set log level. 0 is silent, 1 is error, 2 is warning, 3 is "
             "info, 4 is debug, 5 is verbose."),
         "level"},
    });
    parser.addPositionalArgument(
        "command",
        QCoreApplication::translate(
            "main",
            "gui         Start the software in GUI mode.\n"
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
    if (!parser.isSet("version")) {
        const QStringList cmdArguments = parser.positionalArguments();
        if (cmdArguments.isEmpty()) {
            if (!parser.isSet("help")) {
                qCritical() << QCoreApplication::translate("main", "Error: missing subcommand.");
                parser.showHelp(1);
            } else {
                parser.showHelp(0);
            }
        } else {
            /* Perform different operations according to different subcommands */
            const QString &command       = cmdArguments.first();
            QStringList    nextArguments = appArguments;
            if (command == "gui") {
                QStaticLog::logV(Q_FUNC_INFO, "Starting GUI ...");
            } else if (command == "symbol") {
                nextArguments.removeOne(command);
                parseSymbol(nextArguments);
            } else if (command == "schematic") {
                qCritical() << "Error: not implemented schematic yet.";
                parser.showHelp(1);
            } else if (command == "generate") {
                qCritical() << "Error: not implemented generate yet.";
                parser.showHelp(1);
            } else {
                if (!parser.isSet("help")) {
                    qCritical() << "Error: unknown subcommand." << command;
                    parser.showHelp(1);
                } else {
                    parser.showHelp(0);
                }
            }
        }
    }
    parser.process(*QCoreApplication::instance());
}

void QSocCliWorker::parseSymbol(const QStringList &appArguments)
{
    /* Clear upstream positional arguments and setup subcommand */
    parser.clearPositionalArguments();
    parser.addPositionalArgument(
        "subcommand",
        QCoreApplication::translate(
            "main",
            "import   Import symbol from verilog.\n"
            "update   Update symbol from verilog.\n"
            "remove   Remove symbol by name."),
        "symbol <subcommand> [subcommand options]");

    parser.parse(appArguments);
    const QStringList cmdArguments = parser.positionalArguments();
    if (cmdArguments.isEmpty()) {
        if (!parser.isSet("help")) {
            qCritical() << "Error: missing subcommand.";
            parser.showHelp(1);
        } else {
            parser.showHelp(0);
        }
    } else {
        const QString &command       = cmdArguments.first();
        QStringList    nextArguments = appArguments;
        if (command == "import") {
            nextArguments.removeOne(command);
            parseSymbolImport(nextArguments);
        } else if (command == "update") {
            nextArguments.removeOne(command);
            parseSymbolUpdate(nextArguments);
        } else if (command == "remove") {
            nextArguments.removeOne(command);
            parseSymbolRemove(nextArguments);
        } else {
            if (!parser.isSet("help")) {
                qCritical() << "Error: unknown subcommand." << command;
                parser.showHelp(1);
            } else {
                parser.showHelp(0);
            }
        }
    }
}

void QSocCliWorker::parseSymbolImport(const QStringList &appArguments)
{
    /* Clear upstream positional arguments and setup subcommand */
    parser.clearPositionalArguments();
    parser.addOptions({
        {{"f", "filelist"},
         QCoreApplication::translate(
             "main",
             "The path where the file list is located, including a list of "
             "verilog files in order."),
         "filelist"},
    });
    parser.addPositionalArgument(
        "name",
        QCoreApplication::translate("main", "The name of the symbol to be import."),
        "[<name>]");
    parser.addPositionalArgument(
        "files",
        QCoreApplication::translate("main", "The verilog files to be processed."),
        "[<verilog files>]");

    parser.parse(appArguments);
    const QStringList cmdArguments = parser.positionalArguments();
    if (cmdArguments.isEmpty()) {
        if (!parser.isSet("help")) {
            qCritical() << "Error: missing symbol name.";
            parser.showHelp(1);
        } else {
            parser.showHelp(0);
        }
    } else {
        const QString &symbolName   = cmdArguments.first();
        QStringList    filePathList = cmdArguments;
        filePathList.removeFirst();
        if (filePathList.isEmpty() && !parser.isSet("filelist")) {
            if (!parser.isSet("help")) {
                qCritical() << "Error: missing verilog files.";
                parser.showHelp(1);
            } else {
                parser.showHelp(0);
            }
        } else {
            if (!parser.isSet("filelist")) {
                processFileList("", filePathList);
            } else {
                processFileList(parser.value("filelist"), filePathList);
            }
        }
    }
}

void QSocCliWorker::parseSymbolUpdate(const QStringList &appArguments) {}

void QSocCliWorker::parseSymbolRemove(const QStringList &appArguments) {}
