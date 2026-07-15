// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocgoal.h"
#include "agent/tool/qsoctoolgoalcomplete.h"
#include "qsoc_test.h"

#include <QPointer>
#include <QTemporaryDir>
#include <QtTest>

namespace {

json parseResult(const QString &result)
{
    return json::parse(result.toStdString());
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void schemaShape()
    {
        QSocToolGoalComplete tool(nullptr, nullptr);
        QCOMPARE(tool.getName(), QStringLiteral("goal_complete"));
        const json schema = tool.getParametersSchema();
        QVERIFY(schema.is_object());
        QVERIFY(schema["properties"]["status"]["enum"].is_array());
        QCOMPARE(schema["properties"]["status"]["enum"].size(), size_t{1});
        QCOMPARE(
            QString::fromStdString(schema["properties"]["status"]["enum"][0].get<std::string>()),
            QStringLiteral("complete"));
    }

    void rejectsWhenNoCatalog()
    {
        QSocToolGoalComplete tool(nullptr, nullptr);
        const QString        out = tool.execute({{"status", "complete"}});
        QVERIFY2(out.contains(QStringLiteral("no project goal catalog")), qPrintable(out));
    }

    void rejectsAfterCatalogDestruction()
    {
        auto                *catalog = new QSocGoalCatalog;
        QSocToolGoalComplete tool(nullptr, catalog);
        delete catalog;

        const QString out = tool.execute({{"status", "complete"}});
        QVERIFY2(out.contains(QStringLiteral("no project goal catalog")), qPrintable(out));
    }

    void rejectsWhenNoActiveGoal()
    {
        QTemporaryDir   dir;
        QSocGoalCatalog catalog;
        catalog.load(dir.path());
        QSocToolGoalComplete tool(nullptr, &catalog);
        const QString        out = tool.execute({{"status", "complete"}});
        QVERIFY2(out.contains(QStringLiteral("no active goal")), qPrintable(out));
    }

    void rejectsWrongStatus()
    {
        QTemporaryDir   dir;
        QSocGoalCatalog catalog;
        catalog.load(dir.path());
        QVERIFY(catalog.create(QStringLiteral("x"), 0, nullptr));
        QSocToolGoalComplete tool(nullptr, &catalog);
        const QString        out = tool.execute({{"status", "paused"}});
        QVERIFY2(out.contains(QStringLiteral("only accepts status=complete")), qPrintable(out));
    }

    void completesAndClearsCatalog()
    {
        QTemporaryDir   dir;
        QSocGoalCatalog catalog;
        catalog.load(dir.path());
        QVERIFY(catalog.create(QStringLiteral("Synthesize top.v"), 0, nullptr));
        QSocToolGoalComplete tool(nullptr, &catalog);
        const QString        out = tool.execute({{"status", "complete"}});
        QVERIFY2(out.contains(QStringLiteral("\"status\":\"ok\"")), qPrintable(out));
        QVERIFY(out.contains(QStringLiteral("Synthesize top.v")));
        QVERIFY(!catalog.current().has_value());
    }

    void completionObserverMayDeleteCatalog()
    {
        QTemporaryDir dir;
        auto         *catalog = new QSocGoalCatalog;
        catalog->load(dir.path());
        QVERIFY(catalog->create(QStringLiteral("finish objective"), 0, nullptr));
        QSocToolGoalComplete      tool(nullptr, catalog);
        QPointer<QSocGoalCatalog> guard(catalog);
        connect(catalog, &QSocGoalCatalog::goalChanged, &tool, [catalog]() { delete catalog; });

        const QString out = tool.execute({{"status", "complete"}});

        QVERIFY(guard.isNull());
        QVERIFY2(out.contains(QStringLiteral("\"status\":\"ok\"")), qPrintable(out));
        QVERIFY(out.contains(QStringLiteral("finish objective")));
    }

    void truncatesLongObjective()
    {
        QTemporaryDir   dir;
        QSocGoalCatalog catalog;
        catalog.load(dir.path());
        const QString long_obj = QString(200, QLatin1Char('a'));
        QVERIFY(catalog.create(long_obj, 0, nullptr));
        QSocToolGoalComplete tool(nullptr, &catalog);
        const QString        out = tool.execute({{"status", "complete"}});
        QVERIFY(out.contains(QStringLiteral("...")));
        QVERIFY(out.size() < 250);
    }

    void resultEscapesControlCharacters()
    {
        QTemporaryDir   dir;
        QSocGoalCatalog catalog;
        catalog.load(dir.path());
        const QString objective = QStringLiteral(
            "quote \" slash \\ tab\t carriage\r newline\nfinished");
        QVERIFY(catalog.create(objective, 0, nullptr));
        QSocToolGoalComplete tool(nullptr, &catalog);

        const json result = parseResult(tool.execute({{"status", "complete"}}));

        QCOMPARE(QString::fromStdString(result.at("status").get<std::string>()), QStringLiteral("ok"));
        QCOMPARE(
            QString::fromStdString(result.at("completed_goal").get<std::string>()),
            QStringLiteral("quote \" slash \\ tab\t carriage\r newline finished"));
    }

    void errorResultIsAlwaysValidJson()
    {
        QSocToolGoalComplete tool(nullptr, nullptr);
        const json           result = parseResult(tool.execute({{"status", "complete"}}));
        QCOMPARE(
            QString::fromStdString(result.at("status").get<std::string>()), QStringLiteral("error"));
        QVERIFY(result.at("error").is_string());
    }

    void rejectsMissingStatus()
    {
        QTemporaryDir   dir;
        QSocGoalCatalog catalog;
        catalog.load(dir.path());
        QSocToolGoalComplete tool(nullptr, &catalog);
        const QString        out = tool.execute(json::object());
        QVERIFY2(out.contains(QStringLiteral("status is required")), qPrintable(out));
    }
};

} // namespace

#include "test_qsoctoolgoalcomplete.moc"

QSOC_TEST_MAIN(Test)
