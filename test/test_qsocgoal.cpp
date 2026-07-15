// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocgoal.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>

#include <limits>

#include <QDir>
#include <QFile>
#include <QPointer>
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

bool blockGoalFile(const QSocGoalCatalog &catalog)
{
    const QString path = catalog.projectFilePath();
    return QFile::remove(path) && QDir().mkdir(path);
}

void compareGoals(const QSocGoal &actual, const QSocGoal &expected)
{
    QCOMPARE(actual.id, expected.id);
    QCOMPARE(actual.objective, expected.objective);
    QCOMPARE(actual.status, expected.status);
    QCOMPARE(actual.tokenBudget, expected.tokenBudget);
    QCOMPARE(actual.tokensUsed, expected.tokensUsed);
    QCOMPARE(actual.secondsUsed, expected.secondsUsed);
    QCOMPARE(actual.createdAt, expected.createdAt);
    QCOMPARE(actual.updatedAt, expected.updatedAt);
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

    void replaceRejectsEmptyWithoutDiscarding()
    {
        QTemporaryDir   dir;
        QSocGoalCatalog catalog;
        catalog.load(dir.path());
        QVERIFY(catalog.create(QStringLiteral("first"), 0, nullptr));
        const QSocGoal  before   = *catalog.current();
        const qsizetype logCount = readLogLines(catalog.logFilePath()).size();

        QString error;
        QVERIFY(!catalog.replace(QStringLiteral("   "), 0, &error));
        QVERIFY(error.contains(QStringLiteral("empty")));
        QVERIFY(catalog.current().has_value());
        QCOMPARE(catalog.current()->id, before.id);
        QCOMPARE(catalog.current()->objective, before.objective);
        QCOMPARE(readLogLines(catalog.logFilePath()).size(), logCount);
    }

    void replaceWriteFailureRestoresGoalAndLog()
    {
        QTemporaryDir   dir;
        QSocGoalCatalog catalog;
        catalog.load(dir.path());
        QVERIFY(catalog.create(QStringLiteral("first"), 100, nullptr));
        const QSocGoal  before   = *catalog.current();
        const qsizetype logCount = readLogLines(catalog.logFilePath()).size();
        QVERIFY(blockGoalFile(catalog));

        QString error;
        QVERIFY(!catalog.replace(QStringLiteral("second"), 200, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(catalog.current().has_value());
        QCOMPARE(catalog.current()->id, before.id);
        QCOMPARE(catalog.current()->objective, before.objective);
        QCOMPARE(catalog.current()->tokenBudget, before.tokenBudget);
        QCOMPARE(catalog.current()->updatedAt, before.updatedAt);
        QCOMPARE(readLogLines(catalog.logFilePath()).size(), logCount);
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

    void clearWriteFailureRestoresGoalAndLog()
    {
        QTemporaryDir   dir;
        QSocGoalCatalog catalog;
        catalog.load(dir.path());
        QVERIFY(catalog.create(QStringLiteral("x"), 0, nullptr));
        const QSocGoal  before   = *catalog.current();
        const qsizetype logCount = readLogLines(catalog.logFilePath()).size();
        QVERIFY(blockGoalFile(catalog));

        QString error;
        QVERIFY(!catalog.clear(&error));
        QVERIFY(!error.isEmpty());
        QVERIFY(catalog.current().has_value());
        QCOMPARE(catalog.current()->id, before.id);
        QCOMPARE(catalog.current()->updatedAt, before.updatedAt);
        QCOMPARE(readLogLines(catalog.logFilePath()).size(), logCount);
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

    void setStatusWriteFailureRestoresSnapshot_data()
    {
        QTest::addColumn<bool>("complete");
        QTest::newRow("paused") << false;
        QTest::newRow("complete") << true;
    }

    void setStatusWriteFailureRestoresSnapshot()
    {
        QFETCH(bool, complete);

        QTemporaryDir   dir;
        QSocGoalCatalog catalog;
        catalog.load(dir.path());
        QVERIFY(catalog.create(QStringLiteral("x"), 100, nullptr));
        const QSocGoal  before   = *catalog.current();
        const qsizetype logCount = readLogLines(catalog.logFilePath()).size();
        QSignalSpy      changed(&catalog, &QSocGoalCatalog::goalChanged);
        QVERIFY(blockGoalFile(catalog));

        QString              error;
        const QSocGoalStatus status = complete ? QSocGoalStatus::Complete : QSocGoalStatus::Paused;
        QVERIFY(!catalog.setStatus(status, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(catalog.current().has_value());
        compareGoals(*catalog.current(), before);
        QCOMPARE(readLogLines(catalog.logFilePath()).size(), logCount);
        QCOMPARE(changed.count(), 0);
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

    void completeLogKeepsGoalIdentity()
    {
        QTemporaryDir   dir;
        QSocGoalCatalog catalog;
        catalog.load(dir.path());
        QVERIFY(catalog.create(QStringLiteral("x"), 0, nullptr));
        const QString goalId = catalog.current()->id;

        QVERIFY(catalog.setStatus(QSocGoalStatus::Complete, nullptr));

        bool found = false;
        for (const QString &line : readLogLines(catalog.logFilePath())) {
            const auto event = nlohmann::json::parse(line.toStdString());
            if (event.value("event", std::string()) == "status_changed"
                && event.value("to", std::string()) == "complete") {
                QCOMPARE(QString::fromStdString(event.value("goal_id", std::string())), goalId);
                found = true;
            }
        }
        QVERIFY(found);
    }

    void completeEmitsSettledStateOnce()
    {
        QTemporaryDir   dir;
        QSocGoalCatalog catalog;
        catalog.load(dir.path());
        QVERIFY(catalog.create(QStringLiteral("x"), 0, nullptr));

        QList<bool> observedActiveGoals;
        connect(&catalog, &QSocGoalCatalog::goalChanged, this, [&]() {
            observedActiveGoals.append(catalog.current().has_value());
        });

        QVERIFY(catalog.setStatus(QSocGoalStatus::Complete, nullptr));
        QCOMPARE(observedActiveGoals, QList<bool>{false});
    }

    void completeObserverReplacementWins()
    {
        QTemporaryDir   dir;
        QSocGoalCatalog catalog;
        catalog.load(dir.path());
        QVERIFY(catalog.create(QStringLiteral("first"), 0, nullptr));

        bool reentered          = false;
        bool replacementCreated = false;
        connect(&catalog, &QSocGoalCatalog::goalChanged, this, [&]() {
            if (reentered) {
                return;
            }
            reentered          = true;
            replacementCreated = catalog.replace(QStringLiteral("replacement"), 0, nullptr);
        });

        QVERIFY(catalog.setStatus(QSocGoalStatus::Complete, nullptr));
        QVERIFY(replacementCreated);
        QVERIFY(catalog.current().has_value());
        QCOMPARE(catalog.current()->objective, QStringLiteral("replacement"));

        QSocGoalCatalog reload;
        reload.load(dir.path());
        QVERIFY(reload.current().has_value());
        QCOMPARE(reload.current()->objective, QStringLiteral("replacement"));
    }

    void completeObserverCanDeleteCatalog()
    {
        QTemporaryDir dir;
        auto         *catalog = new QSocGoalCatalog;
        catalog->load(dir.path());
        QVERIFY(catalog->create(QStringLiteral("x"), 0, nullptr));
        QPointer<QSocGoalCatalog> guard(catalog);
        connect(catalog, &QSocGoalCatalog::goalChanged, this, [catalog]() { delete catalog; });

        QVERIFY(catalog->setStatus(QSocGoalStatus::Complete, nullptr));
        QVERIFY(guard.isNull());
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

    void accountUsageEmitsSettledStateOnce()
    {
        QTemporaryDir   dir;
        QSocGoalCatalog catalog;
        catalog.load(dir.path());
        QVERIFY(catalog.create(QStringLiteral("x"), 10, nullptr));

        QList<QSocGoalStatus> observedStatuses;
        connect(&catalog, &QSocGoalCatalog::goalChanged, this, [&]() {
            QVERIFY(catalog.current().has_value());
            observedStatuses.append(catalog.current()->status);
        });

        QVERIFY(catalog.accountUsage(10, 0, nullptr));
        QCOMPARE(observedStatuses, QList<QSocGoalStatus>{QSocGoalStatus::BudgetLimited});
    }

    void accountUsageObserverCanClearCatalog()
    {
        QTemporaryDir   dir;
        QSocGoalCatalog catalog;
        catalog.load(dir.path());
        QVERIFY(catalog.create(QStringLiteral("x"), 10, nullptr));

        bool cleared = false;
        connect(&catalog, &QSocGoalCatalog::goalChanged, this, [&]() {
            if (cleared) {
                return;
            }
            cleared = true;
            catalog.clear(nullptr);
        });

        QVERIFY(catalog.accountUsage(10, 0, nullptr));
        QVERIFY(cleared);
        QVERIFY(!catalog.current().has_value());
    }

    void accountUsageObserverCanDeleteCatalog()
    {
        QTemporaryDir dir;
        auto         *catalog = new QSocGoalCatalog;
        catalog->load(dir.path());
        QVERIFY(catalog->create(QStringLiteral("x"), 10, nullptr));
        QPointer<QSocGoalCatalog> guard(catalog);
        connect(catalog, &QSocGoalCatalog::goalChanged, this, [catalog]() { delete catalog; });

        QVERIFY(catalog->accountUsage(10, 0, nullptr));
        QVERIFY(guard.isNull());
    }

    void accountUsageNoOpWhenNoGoal()
    {
        QTemporaryDir   dir;
        QSocGoalCatalog catalog;
        catalog.load(dir.path());
        QVERIFY(catalog.accountUsage(100, 1, nullptr));
        QVERIFY(!catalog.current().has_value());
    }

    void accountUsageWriteFailureRestoresSnapshot()
    {
        QTemporaryDir   dir;
        QSocGoalCatalog catalog;
        catalog.load(dir.path());
        QVERIFY(catalog.create(QStringLiteral("x"), 10, nullptr));
        const QSocGoal  before   = *catalog.current();
        const qsizetype logCount = readLogLines(catalog.logFilePath()).size();
        QSignalSpy      changed(&catalog, &QSocGoalCatalog::goalChanged);
        QVERIFY(blockGoalFile(catalog));

        QString error;
        QVERIFY(!catalog.accountUsage(10, 7, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(catalog.current().has_value());
        compareGoals(*catalog.current(), before);
        QCOMPARE(readLogLines(catalog.logFilePath()).size(), logCount);
        QCOMPARE(changed.count(), 0);
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

    void updateObjectiveWriteFailureRestoresSnapshot()
    {
        QTemporaryDir   dir;
        QSocGoalCatalog catalog;
        catalog.load(dir.path());
        QVERIFY(catalog.create(QStringLiteral("first"), 0, nullptr));
        const QSocGoal  before   = *catalog.current();
        const qsizetype logCount = readLogLines(catalog.logFilePath()).size();
        QVERIFY(blockGoalFile(catalog));

        QString error;
        QVERIFY(!catalog.updateObjective(QStringLiteral("second"), &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(catalog.current()->objective, before.objective);
        QCOMPARE(catalog.current()->updatedAt, before.updatedAt);
        QCOMPARE(readLogLines(catalog.logFilePath()).size(), logCount);
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

    void setTokenBudgetWriteFailureRestoresSnapshot()
    {
        QTemporaryDir   dir;
        QSocGoalCatalog catalog;
        catalog.load(dir.path());
        QVERIFY(catalog.create(QStringLiteral("x"), 100, nullptr));
        const QSocGoal  before   = *catalog.current();
        const qsizetype logCount = readLogLines(catalog.logFilePath()).size();
        QVERIFY(blockGoalFile(catalog));

        QString error;
        QVERIFY(!catalog.setTokenBudget(200, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(catalog.current()->tokenBudget, before.tokenBudget);
        QCOMPARE(catalog.current()->updatedAt, before.updatedAt);
        QCOMPARE(readLogLines(catalog.logFilePath()).size(), logCount);
    }

    void accountUsageSaturatesCounters()
    {
        QTemporaryDir   dir;
        QSocGoalCatalog catalog;
        catalog.load(dir.path());
        QVERIFY(catalog.create(QStringLiteral("x"), 0, nullptr));
        QVERIFY(catalog.accountUsage(std::numeric_limits<int>::max(), 1, nullptr));
        QVERIFY(catalog.accountUsage(1, std::numeric_limits<qint64>::max(), nullptr));
        QCOMPARE(catalog.current()->tokensUsed, std::numeric_limits<int>::max());
        QCOMPARE(catalog.current()->secondsUsed, std::numeric_limits<qint64>::max());
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
