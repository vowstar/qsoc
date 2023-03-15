#include "mainwindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QThread>
#include <QLocale>
#include <QTranslator>

#include "qsoccliworker.h"
#include "qstaticlog.h"

bool isGui(int &argc, char *argv[])
{
    for (int i = 1; i < argc; ++i)
    {
        if (!qstrcmp(argv[i], "--cli" ) || !qstrcmp(argv[i], "-c" ))
            return false;
    }
    return true;
}

void initTranslator(const QCoreApplication &app, QTranslator &translator, const QString &prefix = ":/i18n/")
{
    const QStringList uiLanguages = QLocale::system().uiLanguages();

    for (const QString &locale : uiLanguages)
    {
        const QString baseName = QLocale(locale).name();

        if (translator.load(prefix+ baseName + ".qm"))
        {
            break;
        }
    }
    app.installTranslator(&translator);
}

void initParser(const QCoreApplication &app, QCommandLineParser &parser)
{
    QCoreApplication::setApplicationName(QCoreApplication::translate("main", "SoC Studio"));
    QCoreApplication::setApplicationVersion("1.0.0");

    parser.setApplicationDescription(QCoreApplication::translate("main", "Generate SoC components via the command line."));
    parser.addHelpOption();
    parser.addVersionOption();

    parser.addOptions({
        {{"c", "cli"},
         QCoreApplication::translate("main", "Start the software in CLI mode, otherwise it will start in GUI mode.")},
        {{"l", "level"},
         QCoreApplication::translate("main", "Set log level. 0 is silent, 1 is error, 2 is warning, 3 is info, 4 is debug, 5 is verbose."),
         "level"},
    });

    parser.process(app);
}

int main(int argc, char *argv[])
{
    QStaticLog::setLevel(QStaticLog::levelS);

    QCommandLineParser parser;
    QTranslator translator;
    QTranslator translatorBase;

    int result = 0;

    if (isGui(argc, argv))
    {
        QApplication app(argc, argv);
        initTranslator(app, translator, ":/i18n/");
        initTranslator(app, translatorBase, ":/i18n/qtbase_");
        initParser(app, parser);

        MainWindow w;
        w.show();
        result = app.exec();
    }
    else
    {
        QCoreApplication app(argc, argv);
        initTranslator(app, translator, ":/i18n/");
        initTranslator(app, translatorBase, ":/i18n/qtbase_");
        initParser(app, parser);

        if (parser.isSet("level"))
        {
            QStaticLog::setLevel(parser.value("level").toInt());
        }

        QThread thread;
        QSocCliWorker socCliWorker;
        socCliWorker.setup(thread);
        socCliWorker.setParser(&parser);
        thread.start();
        result = app.exec();
        thread.wait();
    }
    return result;
}
