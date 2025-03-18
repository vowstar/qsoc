#include "cli/qsoccliworker.h"
#include "common/config.h"

#include <QDir>
#include <QFile>
#include <QStringList>
#include <QtCore>
#include <QtTest>

#include <iostream>

struct TestApp
{
    static auto &instance()
    {
        static auto                  argc      = 1;
        static char                  appName[] = "socstudio";
        static std::array<char *, 1> argv      = {{appName}};
        /* Use QCoreApplication for cli test */
        static const QCoreApplication app = QCoreApplication(argc, argv.data());
        return app;
    }
};

class Test : public QObject
{
    Q_OBJECT

private:
    static QStringList messageList;
    static void messageOutput(QtMsgType type, const QMessageLogContext &context, const QString &msg)
    {
        Q_UNUSED(type);
        Q_UNUSED(context);
        messageList << msg;
    }

private slots:
    void initTestCase()
    {
        TestApp::instance();
        qInstallMessageHandler(messageOutput);
    }

    void testProjectCreate()
    {
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments = {
            "socstudio",
            "project",
            "create",
            "test_project",
        };
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Check if the project file was created */
        QFile projectFile("test_project.soc_pro");
        QVERIFY(projectFile.exists());

        /* Read the file content */
        projectFile.open(QIODevice::ReadOnly | QIODevice::Text);
        QString content = projectFile.readAll();
        projectFile.close();

        /* Check for required strings */
        QVERIFY(content.contains("bus"));
        QVERIFY(content.contains("module"));
        QVERIFY(content.contains("schematic"));
        QVERIFY(content.contains("output"));
    }

    void testProjectList()
    {
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments = {
            "socstudio",
            "project",
            "list",
        };
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Check if test_project is listed in the output */
        bool found = false;
        for (const QString &msg : messageList) {
            if (msg.contains("test_project")) {
                found = true;
                break;
            }
        }
        QVERIFY(found);
    }

    void testProjectShow()
    {
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments = {
            "socstudio",
            "project",
            "show",
            "test_project",
        };
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Check for required strings in the output */
        bool hasBus = false, hasModule = false, hasSchematic = false, hasOutput = false;
        for (const QString &msg : messageList) {
            if (msg.contains("bus"))
                hasBus = true;
            if (msg.contains("module"))
                hasModule = true;
            if (msg.contains("schematic"))
                hasSchematic = true;
            if (msg.contains("output"))
                hasOutput = true;
        }
        QVERIFY(hasBus);
        QVERIFY(hasModule);
        QVERIFY(hasSchematic);
        QVERIFY(hasOutput);
    }

    void testProjectRemove()
    {
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments = {
            "socstudio",
            "project",
            "remove",
            "test_project",
        };
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Check if the project file was deleted */
        QFile projectFile("test_project.soc_pro");
        QVERIFY(!projectFile.exists());
    }
};

QStringList Test::messageList;

QTEST_APPLESS_MAIN(Test)

#include "test_qsoccliparseproject.moc"
