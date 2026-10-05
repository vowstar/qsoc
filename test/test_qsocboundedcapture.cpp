// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocboundedcapture.h"

#include <QtTest>

namespace {

QByteArray numberedLines(int count)
{
    QByteArray data;
    for (int line = 1; line <= count; ++line)
        data += "line " + QByteArray::number(line) + " \xe4\xbd\xa0\xf0\x9f\x98\x80\n";
    return data;
}

bool validUtf8(const QByteArray &data)
{
    return QString::fromUtf8(data).toUtf8() == data;
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void shortStreamIsKeptWhole()
    {
        QSocBoundedCapture capture(1024);
        capture.append("abc");
        capture.append("def");
        QCOMPARE(capture.bytes(), QByteArray("abcdef"));
        QCOMPARE(capture.totalBytes(), qint64(6));
        QVERIFY(!capture.isElided());
        QVERIFY(!QSocBoundedCapture::isElided(capture.text()));
    }

    void streamAtTheLimitIsKeptWhole()
    {
        const QByteArray   data = numberedLines(500);
        QSocBoundedCapture capture(data.size());
        capture.append(data);
        QCOMPARE(capture.bytes(), data);
    }

    void longStreamKeepsHeadAndTailWithinTheLimit()
    {
        const QByteArray data = numberedLines(20000) + "ERROR_TAIL\n";
        for (const qint64 limit : {qint64(64), qint64(4096), qint64(100001)}) {
            QSocBoundedCapture capture(limit);
            capture.append(data);
            const QByteArray kept = capture.bytes();
            QVERIFY(capture.isElided());
            QVERIFY(kept.size() <= limit);
            QVERIFY(kept.size() + 16 >= limit);
            QVERIFY(validUtf8(kept));
            QVERIFY(QSocBoundedCapture::isElided(QString::fromUtf8(kept)));
            QVERIFY(kept.startsWith("line 1 "));
            QVERIFY(kept.endsWith("ERROR_TAIL\n"));
            const QByteArray marker = QSocBoundedCapture::marker(0).toUtf8().left(6);
            const qsizetype  cut    = kept.indexOf(marker);
            QVERIFY(cut >= 0);
            const qsizetype  end  = kept.indexOf("...]\n", cut) + 5;
            const QByteArray head = kept.left(cut);
            const QByteArray tail = kept.mid(end);
            QVERIFY(data.startsWith(head));
            QVERIFY(data.endsWith(tail));
            QCOMPARE(
                kept.mid(cut, end - cut),
                QSocBoundedCapture::marker(data.size() - head.size() - tail.size()).toUtf8());
        }
    }

    void chunkedAppendMatchesOneAppend()
    {
        const QByteArray   data = numberedLines(5000);
        QSocBoundedCapture whole(3000);
        whole.append(data);
        for (const int chunk : {1, 7, 4096}) {
            QSocBoundedCapture pieces(3000);
            for (qsizetype offset = 0; offset < data.size(); offset += chunk)
                pieces.append(QByteArrayView(data)
                                  .sliced(offset, std::min<qsizetype>(chunk, data.size() - offset)));
            QCOMPARE(pieces.bytes(), whole.bytes());
            QCOMPARE(pieces.totalBytes(), qint64(data.size()));
        }
    }

    void tailBytesEndsTheStream()
    {
        const QByteArray   data = numberedLines(5000);
        QSocBoundedCapture small(1 << 20);
        small.append(data);
        QSocBoundedCapture large(2000);
        large.append(data);
        for (const auto *capture : {&small, &large}) {
            const QByteArray tail = capture->tailBytes(100);
            QVERIFY(tail.size() <= 100);
            QVERIFY(tail.size() > 90);
            QVERIFY(validUtf8(tail));
            QVERIFY(data.endsWith(tail));
        }
        QCOMPARE(small.tailBytes(qint64(data.size()) + 10), data);
    }

    void boundCutsTextLikeACapture()
    {
        const QString text = QString::fromUtf8(numberedLines(3000));
        QCOMPARE(QSocBoundedCapture::bound(text, text.toUtf8().size()), text);
        const QString bounded = QSocBoundedCapture::bound(text, 4000);
        QVERIFY(bounded.toUtf8().size() <= 4000);
        QVERIFY(QSocBoundedCapture::isElided(bounded));
        QVERIFY(bounded.startsWith(QStringLiteral("line 1 ")));
        QVERIFY(bounded.endsWith(text.right(40)));
    }
};

} // namespace

QTEST_APPLESS_MAIN(Test)
#include "test_qsocboundedcapture.moc"
