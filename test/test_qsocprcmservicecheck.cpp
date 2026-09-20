// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocprcmservicecheck.h"
#include "qsoc_test.h"

#include <QtTest>

class Test : public QObject
{
    Q_OBJECT

private slots:
    void safety()
    {
        const auto result = QSocPrcmServiceCheck::safety();
        QCOMPARE(result.status, QSocPrcmCheckStatus::Unsat);
        QVERIFY(!result.smt.isEmpty());
    }

    void progress()
    {
        const auto result = QSocPrcmServiceCheck::progress();
        QCOMPARE(result.status, QSocPrcmCheckStatus::Unsat);
        QVERIFY(result.loop.isEmpty());
    }

    void cancelled()
    {
        std::stop_source stop;
        stop.request_stop();
        QCOMPARE(
            QSocPrcmServiceCheck::safety({}, stop.get_token()).status,
            QSocPrcmCheckStatus::Cancelled);
        QCOMPARE(
            QSocPrcmServiceCheck::progress(stop.get_token()).status, QSocPrcmCheckStatus::Cancelled);
    }

    void budget()
    {
        QSocPrcmCheckBudget budget;
        budget.resourceLimit = 1;
        QCOMPARE(QSocPrcmServiceCheck::safety(budget).status, QSocPrcmCheckStatus::Unknown);
    }
};

QSOC_TEST_MAIN(Test)
#include "test_qsocprcmservicecheck.moc"
