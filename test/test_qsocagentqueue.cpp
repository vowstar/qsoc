// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "qsoc_test.h"

namespace {

class Test : public QObject
{
    Q_OBJECT

private slots:
    void rejectsExcessRequestsAndRecovers()
    {
        QSocAgent agent;
        for (int index = 0; index < 64; ++index)
            QVERIFY(agent.queueRequest(QString::number(index)));
        QVERIFY(!agent.queueRequest(QStringLiteral("overflow")));
        QCOMPARE(agent.pendingRequestCount(), 64);
        agent.clearPendingRequests();
        QVERIFY(agent.queueRequest(QStringLiteral("next")));
        QCOMPARE(agent.pendingRequestCount(), 1);
    }

    void limitsCombinedText()
    {
        QSocAgent     agent;
        const QString half(4 * 1024 * 1024, QLatin1Char('x'));
        QVERIFY(agent.queueRequest(half));
        QVERIFY(agent.queueRequest(half));
        QVERIFY(!agent.queueRequest(QStringLiteral("x")));
        QCOMPARE(agent.pendingRequestCount(), 2);
        agent.clearPendingRequests();
        QVERIFY(!agent.queueRequest(QString(8 * 1024 * 1024 + 1, QLatin1Char('x'))));
        QCOMPARE(agent.pendingRequestCount(), 0);
        QVERIFY(agent.queueRequest(half));
    }

    void pendingNotificationsCountTowardAdmission()
    {
        QSocAgent agent;
        for (int index = 0; index < 64; ++index)
            QVERIFY(agent.queueTaskNotification(QString::number(index)));
        QVERIFY(!agent.queueRequest(QStringLiteral("overflow")));
        QCOMPARE(agent.pendingRequestCount(), 64);
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocagentqueue.moc"
