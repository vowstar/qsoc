#include "mainwindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QHash>
#include <QLocale>
#include <QThread>
#include <QTranslator>

#include "qsoccliworker.h"
#include "qstaticlog.h"

bool isGui(int &argc, char *argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (0 == qstrcmp(argv[i], "gui"))
            return true;
    }
    return false;
}

void initTranslator(
    const QCoreApplication &app, QTranslator &translator, const QString &prefix = ":/i18n/")
{
    const QStringList uiLanguages = QLocale::system().uiLanguages();

    for (const QString &locale : uiLanguages) {
        const QString baseName = QLocale(locale).name();

        if (translator.load(prefix + baseName + ".qm")) {
            break;
        }
    }
    app.installTranslator(&translator);
}

void initParser(const QCoreApplication &app, QCommandLineParser &parser)
{
    QCoreApplication::setApplicationName("socstuido");
    QCoreApplication::setApplicationVersion("1.0.0");
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
    /* help and version options have higher priority */
    if ((!parser.isSet("help")) && (!parser.isSet("version"))) {
        const QStringList args = parser.positionalArguments();
        if (args.isEmpty()) {
            qCritical() << "Error: missing subcommand";
            parser.showHelp(1);
        }
        const QString &command = args.first();
        if (command == "gui") {
            QStaticLog::logV(Q_FUNC_INFO, "Starting GUI ...");
        } else if (command == "symbol") {
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
            if (subArgs.isEmpty()) {
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
                qCritical() << "Error: unknown subcommand" << subCommand;
                parser.showHelp(1);
            }
        } else if (command == "schematic") {
            qCritical() << "Error: not implemented schematic yet";
            parser.showHelp(1);
        } else if (command == "generate") {
            qCritical() << "Error: not implemented generate yet";
            parser.showHelp(1);
        } else {
            qCritical() << "Error: unknown subcommand" << command;
            parser.showHelp(1);
        }
    }
    parser.process(app);
}

int main(int argc, char *argv[])
{
    QStaticLog::setLevel(QStaticLog::levelS);

    QCommandLineParser parser;
    QTranslator        translator;
    QTranslator        translatorBase;

    int result = 0;

    if (isGui(argc, argv)) {
        const QApplication app(argc, argv);
        initTranslator(app, translator, ":/i18n/app_");
        initTranslator(app, translatorBase, ":/i18n/qtbase_");
        initParser(app, parser);

        MainWindow mainWindow;
        mainWindow.show();
        result = app.exec();
    } else {
        const QCoreApplication app(argc, argv);
        initTranslator(app, translator, ":/i18n/app_");
        initTranslator(app, translatorBase, ":/i18n/qtbase_");
        initParser(app, parser);

        QThread       thread;
        QSocCliWorker socCliWorker;
        socCliWorker.setup(thread);
        socCliWorker.setParser(&parser);
        thread.start();
        result = app.exec();
        thread.wait();
    }
    return result;
}
