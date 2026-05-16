// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocgoal.h"
#include "agent/tool/qsoctoolgoalcomplete.h"
#include "qsoc_test.h"

#include <QTemporaryDir>
#include <QtTest>

namespace {

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
        QVERIFY2(out.contains(QStringLiteral("sub-agent context")), qPrintable(out));
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
