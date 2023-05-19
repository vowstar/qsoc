#include "qsoccliworker.h"

#include <QString>
#include <QTimer>
#include <QtGlobal>

#include "qslangdriver.h"
#include "qstaticlog.h"

QSocCliWorker::QSocCliWorker(QObject *parent)
    : QObject(parent)
{}

QSocCliWorker::~QSocCliWorker() {}

void QSocCliWorker::setup(bool isGui)
{
    /* Set up application name and version */
    QCoreApplication::setApplicationName("socstuido");
    QCoreApplication::setApplicationVersion("1.0.0");
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

void QSocCliWorker::processFileList(const QString &fileListName)
{
    QSlangDriver driver(this);
    if (driver.parseFileList(fileListName)) {
        /* Parse success */
    }
}

void QSocCliWorker::run()
{
    /* Set up command line parser */
    parser.setApplicationDescription(
        QCoreApplication::translate("main", "Generate SoC components via the command line"));
    parser.setOptionsAfterPositionalArgumentsMode(QCommandLineParser::ParseAsOptions);
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOptions({
        {{"c", "cli"},
         QCoreApplication::translate(
             "main", "Start the software in CLI mode, otherwise it will start in GUI mode")},
        {{"l", "level"},
         QCoreApplication::translate(
             "main",
             "Set log level. 0 is silent, 1 is error, 2 is warning, 3 is "
             "info, 4 is debug, 5 is verbose"),
         "level"},
    });
    parser.addPositionalArgument(
        "command",
        QCoreApplication::translate(
            "main",
            "gui         Start the software in GUI mode\n"
            "symbol      Import, update of symbol\n"
            "schematic   Processing of Schematic\n"
            "generate    Generate rtl, such as verilog, etc.\n"),
        "<command> [command options]");
    parser.parse(QCoreApplication::arguments());
    /* Set debug level as early as possible */
    if (parser.isSet("level")) {
        QStaticLog::setLevel(parser.value("level").toInt());
    }
    /* version options have higher priority */
    if (!parser.isSet("version")) {
        const QStringList args = parser.positionalArguments();
        if (args.isEmpty() && !parser.isSet("help")) {
            qCritical() << QCoreApplication::translate("main", "Error: missing subcommand");
            parser.showHelp(1);
        }
        const QString &command = args.first();
        if (command == "gui") {
            QStaticLog::logV(Q_FUNC_INFO, "Starting GUI ...");
        } else if (command == "symbol") {
            parseSymbol();
        } else if (command == "schematic") {
            qCritical() << "Error: not implemented schematic yet";
            parser.showHelp(1);
        } else if (command == "generate") {
            qCritical() << "Error: not implemented generate yet";
            parser.showHelp(1);
        } else {
            if (!parser.isSet("help")) {
                qCritical() << "Error: unknown subcommand" << command;
                parser.showHelp(1);
            }
        }
    }
    parser.process(*QCoreApplication::instance());

    emit quit();
}

void QSocCliWorker::parseSymbol()
{
    /* Setup subcommand */
    parser.clearPositionalArguments();
    parser.addPositionalArgument(
        "subcommand",
        QCoreApplication::translate(
            "main",
            "import   Import symbol from verilog\n"
            "update   Update symbol from verilog\n"
            "remove   Remove symbol by name"),
        "symbol <subcommand> [subcommand options]");
    parser.addPositionalArgument(
        "files",
        QCoreApplication::translate("main", "The verilog files to be processed"),
        "[<verilog files>]");
    parser.addOptions({
        {{"f", "filelist"},
            QCoreApplication::translate(
                "main",
                "The path where the file list is located, including a list of "
                "verilog files in order"),
            "filelist"},
    });
    parser.addOptions({
        {{"s", "symbol"},
            QCoreApplication::translate(
                "main", "The symbol name to be imported, updated or removed"),
            "symbol"},
    });

    QStringList subArguments = QCoreApplication::arguments();
    subArguments.removeOne("symbol");
    parser.parse(subArguments);
    const QStringList subArgs = parser.positionalArguments();
    if (subArgs.isEmpty() && !parser.isSet("help")) {
        qCritical() << "Error: missing subcommand";
        parser.showHelp(1);
    }
    const QString &subCommand = subArgs.first();
    if (subCommand == "import" || subCommand == "update") {
    } else if (subCommand == "remove") {
        parser.addPositionalArgument(
            "name",
            QCoreApplication::translate("main", "The name of the symbol to be removed"),
            "[<name>]");

    } else {
        if (!parser.isSet("help")) {
            qCritical() << "Error: unknown subcommand" << subCommand;
            parser.showHelp(1);
        }
    }
}
