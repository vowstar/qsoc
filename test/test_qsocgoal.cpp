// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocgoal.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

namespace {

QStringList readLogLines(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void loadEmpty()
    {
        QTemporaryDir   dir;
        QSocGoalCatalog catalog;
        catalog.load(dir.path());
        QVERIFY(!catalog.current().has_value());
    }

    void createPersists()
    {
        QTemporaryDir   dir;
        QSocGoalCatalog catalog;
        catalog.load(dir.path());
        QSignalSpy spy(&catalog, &QSocGoalCatalog::goalChanged);

        QString err;
        QVERIFY2(catalog.create(QStringLiteral("Build the top RTL"), 0, &err), qPrintable(err));
        QCOMPARE(spy.count(), 1);

        QSocGoalCatalog reload;
        reload.load(dir.path());
        QVERIFY(reload.current().has_value());
        QCOMPARE(reload.current()->objective, QStringLiteral("Build the top RTL"));
        QCOMPARE(reload.current()->status, QSocGoalStatus::Active);
    }

    void createRefusesWhenActive()
    {
        QTemporaryDir   dir;
        QSocGoalCatalog catalog;
        catalog.load(dir.path());
        QVERIFY(catalog.create(QStringLiteral("first"), 0, nullptr));
        QString err;
        QVERIFY(!catalog.create(QStringLiteral("second"), 0, &err));
        QVERIFY(err.contains(QStringLiteral("already active")));
    }

    void replaceDiscardsAndCreates()
    {
        QTemporaryDir   dir;
        QSocGoalCatalog catalog;
        catalog.load(dir.path());
        QVERIFY(catalog.create(QStringLiteral("first"), 0, nullptr));
        const QString firstId = catalog.current()->id;
        QVERIFY(catalog.replace(QStringLiteral("second"), 0, nullptr));
        QVERIFY(catalog.current().has_value());
        QCOMPARE(catalog.current()->objective, QStringLiteral("second"));
        QVERIFY(catalog.current()->id != firstId);

        const auto lines = readLogLines(catalog.logFilePath());
        QVERIFY(!lines.isEmpty());
        bool sawDiscarded = false;
        bool sawCreated   = false;
        for (const auto &line : lines) {
            if (line.contains(QStringLiteral("\"event\":\"discarded\""))) {
                sawDiscarded = true;
            }
            if (line.contains(QStringLiteral("\"event\":\"created\""))) {
                sawCreated = true;
            }
        }
        QVERIFY(sawDiscarded);
        QVERIFY(sawCreated);
    }

    void clearLogsAndDrops()
    {
        QTemporaryDir   dir;
        QSocGoalCatalog catalog;
        catalog.load(dir.path());
        QVERIFY(catalog.create(QStringLiteral("x"), 0, nullptr));
        QVERIFY(catalog.clear(nullptr));
        QVERIFY(!catalog.current().has_value());

        QSocGoalCatalog reload;
        reload.load(dir.path());
        QVERIFY(!reload.current().has_value());
    }

    void setStatusActiveToPaused()
    {
        QTemporaryDir   dir;
        QSocGoalCatalog catalog;
        catalog.load(dir.path());
        QVERIFY(catalog.create(QStringLiteral("x"), 0, nullptr));
        QVERIFY(catalog.setStatus(QSocGoalStatus::Paused, nullptr));
        QCOMPARE(catalog.current()->status, QSocGoalStatus::Paused);
        QVERIFY(catalog.setStatus(QSocGoalStatus::Active, nullptr));
        QCOMPARE(catalog.current()->status, QSocGoalStatus::Active);
    }

    void completeDropsGoal()
    {
        QTemporaryDir   dir;
        QSocGoalCatalog catalog;
        catalog.load(dir.path());
        QVERIFY(catalog.create(QStringLiteral("x"), 0, nullptr));
        QVERIFY(catalog.setStatus(QSocGoalStatus::Complete, nullptr));
        QVERIFY(!catalog.current().has_value());
    }

    void accountUsageTripsBudgetLimited()
    {
        QTemporaryDir   dir;
        QSocGoalCatalog catalog;
        catalog.load(dir.path());
        QVERIFY(catalog.create(QStringLiteral("x"), 100, nullptr));
        QVERIFY(catalog.accountUsage(40, 5, nullptr));
        QCOMPARE(catalog.current()->status, QSocGoalStatus::Active);
        QVERIFY(catalog.accountUsage(70, 5, nullptr));
        QCOMPARE(catalog.current()->status, QSocGoalStatus::BudgetLimited);
        QCOMPARE(catalog.current()->tokensUsed, 110);
    }

    void accountUsageNoOpWhenNoGoal()
    {
        QTemporaryDir   dir;
        QSocGoalCatalog catalog;
        catalog.load(dir.path());
        QVERIFY(catalog.accountUsage(100, 1, nullptr));
        QVERIFY(!catalog.current().has_value());
    }

    void updateObjectiveLogs()
    {
        QTemporaryDir   dir;
        QSocGoalCatalog catalog;
        catalog.load(dir.path());
        QVERIFY(catalog.create(QStringLiteral("first"), 0, nullptr));
        QVERIFY(catalog.updateObjective(QStringLiteral("second"), nullptr));
        QCOMPARE(catalog.current()->objective, QStringLiteral("second"));

        const auto lines = readLogLines(catalog.logFilePath());
        bool       found = false;
        for (const auto &line : lines) {
            if (line.contains(QStringLiteral("\"event\":\"objective_updated\""))) {
                found = true;
            }
        }
        QVERIFY(found);
    }

    void rejectsEmptyObjective()
    {
        QTemporaryDir   dir;
        QSocGoalCatalog catalog;
        catalog.load(dir.path());
        QString err;
        QVERIFY(!catalog.create(QStringLiteral("   "), 0, &err));
        QVERIFY(err.contains(QStringLiteral("empty")));
    }

    void setTokenBudgetUpdates()
    {
        QTemporaryDir   dir;
        QSocGoalCatalog catalog;
        catalog.load(dir.path());
        QVERIFY(catalog.create(QStringLiteral("x"), 0, nullptr));
        QVERIFY(catalog.setTokenBudget(5000, nullptr));
        QCOMPARE(catalog.current()->tokenBudget, 5000);
    }

    void noteContinuationAppendsLog()
    {
        QTemporaryDir   dir;
        QSocGoalCatalog catalog;
        catalog.load(dir.path());
        QVERIFY(catalog.create(QStringLiteral("x"), 0, nullptr));
        catalog.noteContinuation(QStringLiteral("auto"));
        const auto lines = readLogLines(catalog.logFilePath());
        bool       found = false;
        for (const auto &line : lines) {
            if (line.contains(QStringLiteral("\"event\":\"continued\""))) {
                found = true;
            }
        }
        QVERIFY(found);
    }

    void statusFromStringRoundTrip()
    {
        for (auto status :
             {QSocGoalStatus::Active,
              QSocGoalStatus::Paused,
              QSocGoalStatus::BudgetLimited,
              QSocGoalStatus::Complete}) {
            const auto raw   = qSocGoalStatusToString(status);
            const auto round = qSocGoalStatusFromString(raw);
            QVERIFY(round.has_value());
            QVERIFY(round.value() == status);
        }
        QVERIFY(!qSocGoalStatusFromString(QStringLiteral("garbage")).has_value());
    }
};

} // namespace

#include "test_qsocgoal.moc"

QSOC_TEST_MAIN(Test)
