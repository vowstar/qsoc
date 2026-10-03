// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocipc.h"

#include <QtTest>

namespace {

class Test : public QObject
{
    Q_OBJECT

private slots:
    void fragmentedAndCoalescedFrames()
    {
        const QJsonObject  first{{"id", 1}, {"text", QString::fromUtf8("测试")}};
        const QJsonObject  second{{"id", 2}, {"result", true}};
        const auto         wire = QSocIpc::frame(first) + QSocIpc::frame(second);
        QByteArray         pending;
        QList<QJsonObject> received;
        for (char byte : wire) {
            pending.append(byte);
            QJsonObject message;
            const auto  state = QSocIpc::decode(pending, message);
            QVERIFY(state != QSocIpc::DecodeResult::Invalid);
            if (state == QSocIpc::DecodeResult::Complete)
                received.append(message);
        }
        QCOMPARE(received, QList<QJsonObject>({first, second}));
        QVERIFY(pending.isEmpty());
        pending = wire;
        QJsonObject message;
        QCOMPARE(QSocIpc::decode(pending, message), QSocIpc::DecodeResult::Complete);
        QCOMPARE(message, first);
        QCOMPARE(QSocIpc::decode(pending, message), QSocIpc::DecodeResult::Complete);
        QCOMPARE(message, second);
        QVERIFY(pending.isEmpty());
    }

    void rejectsInvalidInput_data()
    {
        QTest::addColumn<QByteArray>("wire");
        QTest::newRow("zero") << QByteArray("00000000");
        QTest::newRow("overflow") << QByteArray("ffffffff");
        QTest::newRow("sign") << QByteArray("+0000002{}");
        QTest::newRow("nonhex") << QByteArray("0000000g");
        QTest::newRow("array") << QByteArray("00000002[]");
        QTest::newRow("malformed") << QByteArray("00000002{{");
        QTest::newRow("over-limit") << QByteArray("00001001");
    }

    void rejectsInvalidInput()
    {
        QFETCH(QByteArray, wire);
        QJsonObject message;
        QString     error;
        QCOMPARE(QSocIpc::decode(wire, message, 4096, &error), QSocIpc::DecodeResult::Invalid);
        QVERIFY(!error.isEmpty());
    }

    void serviceLimitsAreIndependent()
    {
        const QJsonObject value{{"text", QString(4096, QLatin1Char('x'))}};
        const auto        wire = QSocIpc::frame(value);
        QVERIFY(!wire.isEmpty());
        QVERIFY(QSocIpc::frame(value, 4096).isEmpty());
        auto        pending = wire;
        QJsonObject message;
        QCOMPARE(QSocIpc::decode(pending, message, 4096), QSocIpc::DecodeResult::Invalid);
        pending = wire;
        QCOMPARE(QSocIpc::decode(pending, message, 8192), QSocIpc::DecodeResult::Complete);
        QCOMPARE(message, value);
    }
};

} // namespace

QTEST_APPLESS_MAIN(Test)
#include "test_qsocipc.moc"
