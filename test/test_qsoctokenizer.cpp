// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsoctokenizer.h"
#include "qsoc_test.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QtTest>

namespace {

/* Constants below were computed once with tiktoken 0.14.0 encode_ordinary. */
constexpr char tableSha256[]   = "446a9538cb6c348e3516120d7c08b09f57c36495e2acfffe59a5bf8b0cfb1a2d";
constexpr int  tableSize       = 199998;
constexpr quint64 corpusBytes  = 864324;
constexpr qint64  corpusTokens = 279324;
constexpr quint64 corpusDigest = 0x649909baf253b425ULL;
constexpr qint64  repeatedTokens  = 131072;
constexpr qint64  ideographTokens = 670656;

const char16_t *const pool[] = {
    u"The quick brown fox jumps over the lazy dog. ",
    u"It's what they've said: we'll see, I'd guess, you're right, I'm sure. ",
    u"DON'T SHOUT, IT'S LOUD! ",
    u"module counter #(parameter WIDTH = 8) (input wire clk, input wire rst_n, output reg "
    u"[WIDTH-1:0] q);\n",
    u"  always @(posedge clk or negedge rst_n) if (!rst_n) q <= '0; else q <= q + 1'b1;\n",
    u"endmodule // counter\n\n",
    u"for (int i = 0; i < n; ++i) { sum += a[i] * b[i]; }\n",
    u"std::vector<std::pair<int, std::string>> table{{1, \"one\"}, {2, \"two\"}};\n",
    u"{\"role\": \"tool\", \"tool_call_id\": \"call_0\", \"content\": \"ok\"}\n",
    u"{\n  \"name\": \"qsoc\",\n  \"version\": \"2.5.1\",\n  \"tags\": [\"eda\", \"soc\"]\n}\n",
    u"key: value\nlist:\n  - alpha\n  - beta\n",
    u"# Heading\n\n| a | b |\n|---|---|\n| 1 | 2 |\n\n",
    u"Numbers 1234567890 and 3.14159 and 1e-9 and 0xDEADBEEF and 2026-10-02. ",
    u"  \t  indented   spaces\t\ttabs   ",
    u"line one\r\nline two\r\n\r\n",
    u"path/to/some/file.cpp:42:7: error: expected ';'\n",
    u"https://example.com/a/b?c=d&e=f#g ",
    u"\u4F60\u597D\uFF0C\u4E16\u754C\u3002\u8FD9\u662F\u4E00\u4E2A\u6D4B\u8BD5\u3002",
    u"\u65F6\u949F\u57DF\u548C\u590D\u4F4D\u57DF\u7684\u5BC4\u5B58\u5668\u5B9A\u4E49\u3002\n",
    u"\u3053\u3093\u306B\u3061\u306F\u4E16\u754C\u3001\u30AB\u30BF\u30AB\u30CA\u3082\u3002",
    u"\uD55C\uAD6D\uC5B4 \uBB38\uC7A5\uC785\uB2C8\uB2E4. ",
    u"\u041F\u0440\u0438\u0432\u0435\u0442, \u043C\u0438\u0440! ",
    u"\u0645\u0631\u062D\u0628\u0627 \u0628\u0627\u0644\u0639\u0627\u0644\u0645 ",
    u"\u0928\u092E\u0938\u094D\u0924\u0947 \u0926\u0941\u0928\u093F\u092F\u093E ",
    u"\u0E2A\u0E27\u0E31\u0E2A\u0E14\u0E35\u0E0A\u0E32\u0E27\u0E42\u0E25\u0E01 ",
    u"Cafe\u0301 na\u00EFve r\u00E9sum\u00E9 \u00DCber Stra\u00DFe ",
    u"\U0001F468\u200D\U0001F469\u200D\U0001F467\u200D\U0001F466 family \U0001F44D\U0001F3FD ok "
    u"\U0001F680",
    u"\u00A0non\u00A0breaking\u3000ideographic\u2028separator\u0085next ",
    u"zero\u200Bwidth\uFEFFbom ",
    u"\u2211 x\u00B2 \u2264 \u221E, \u03B1\u03B2\u03B3 \u2192 \u03A9 ",
    u"!!!??? ... --- *** ### @@@ $$$ %%% ^^^ &&& ",
    u"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa ",
    u"QmFzZTY0IGVuY29kZWQgZGF0YSBsb29rcyBsaWtlIHRoaXMgb25lLg== ",
    u"\n\n\n",
    u"   \n   \n",
    u"camelCaseIdentifier snake_case_identifier SCREAMING_CASE kebab-case ",
    u"x86_64-linux-gnu-g++ -O2 -Wall -Wextra -std=c++20 ",
    u"<div class=\"note\">&lt;tag&gt; &amp; text</div>\n",
    u"\u2014 \u2013 \u2026 \u201Cquoted\u201D \u2018single\u2019 ",
    u"\u05E9\u05DC\u05D5\u05DD \u05E2\u05D5\u05DC\u05DD ",
};

quint64 next(quint64 &state)
{
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return state >> 33;
}

QString corpus()
{
    quint64 state = 20261002;
    QString text;
    for (int i = 0; i < 20000; ++i) {
        text += QString::fromUtf16(pool[next(state) % std::size(pool)]);
    }
    return text;
}

QString ideographs()
{
    quint64 state = 20261002;
    QString text;
    text.reserve(349525);
    for (int i = 0; i < 349525; ++i) {
        text += QChar(char16_t(0x4E00 + next(state) % 20902));
    }
    return text;
}

quint64 digest(const std::vector<int> &tokens)
{
    quint64 hash = 0xcbf29ce484222325ULL;
    for (const int token : tokens) {
        for (int shift = 0; shift < 32; shift += 8) {
            hash ^= quint64((quint32(token) >> shift) & 0xFF);
            hash *= 0x100000001b3ULL;
        }
    }
    return hash;
}

class Test final : public QObject
{
    Q_OBJECT
private slots:
    void resourceMatchesThePublishedTable()
    {
        QVERIFY(QSocTokenizer::available());
        QFile file(QStringLiteral(":/tokenizer/o200k_base.tiktoken"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(
            QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex(),
            QByteArray(tableSha256));
        QFile license(QStringLiteral(":/license/tiktoken.txt"));
        QVERIFY(license.open(QIODevice::ReadOnly));
        QVERIFY(license.readAll().contains("Copyright (c) 2022 OpenAI, Shantanu Jain"));
    }

    void everyTokenMergesBackToItself()
    {
        QCOMPARE(QSocTokenizer::vocabularySize(), tableSize);
        for (int byte = 0; byte < 256; ++byte) {
            QCOMPARE(QSocTokenizer::encodePiece(QByteArray(1, char(byte))).size(), size_t(1));
        }
        int mismatches = 0;
        for (int rank = 0; rank < tableSize; ++rank) {
            const auto tokens = QSocTokenizer::encodePiece(QSocTokenizer::tokenBytes(rank));
            mismatches += tokens.size() != 1 || tokens.front() != rank;
        }
        QCOMPARE(mismatches, 0);
    }

    void literalVectors()
    {
        const QList<QPair<QString, std::vector<int>>> vectors = {
            {QString::fromUtf16(u"hello world"), {24912, 2375}},
            {QString::fromUtf16(u"I'm sure they'll say it's DON'T"),
             {15390, 3239, 57956, 2891, 4275, 153384}},
            {QString::fromUtf16(u"1234567"), {7633, 19354, 22}},
            {QString::fromUtf16(u"a\r\nb\r\n\r\n  c"), {64, 370, 65, 1414, 220, 274}},
            {QString::fromUtf16(u"   \t\n  x"), {271, 2775, 220, 1215}},
            {QString::fromUtf16(u"\U0001F468‍\U0001F469‍\U0001F467"),
             {28823, 101, 2524, 28823, 102, 2524, 28823, 100}},
            {QString::fromUtf16(u"你好，世界"), {177519, 979, 28428}},
            {QString::fromUtf16(u"naïve café"), {1503, 9954, 737, 30469}},
            {QString::fromUtf16(u"x = foo(bar)//baz\n"),
             {87, 314, 30551, 106832, 60375, 91457, 198}},
            {QString::fromUtf16(u" 　 \u0085"), {5310, 1397, 51008, 126, 227}},
        };
        for (const auto &[text, expected] : vectors) {
            QCOMPARE(QSocTokenizer::encode(text), expected);
            QCOMPARE(QSocTokenizer::count(text), qint64(expected.size()));
        }
        QCOMPARE(QSocTokenizer::count(QString()), qint64(0));
    }

    void seededCorpusMatchesTheReference()
    {
        const QString text = corpus();
        QCOMPARE(quint64(text.toUtf8().size()), corpusBytes);
        const auto tokens = QSocTokenizer::encode(text);
        QCOMPARE(qint64(tokens.size()), corpusTokens);
        QCOMPARE(digest(tokens), corpusDigest);
        QCOMPARE(QSocTokenizer::count(text), corpusTokens);
        QCOMPARE(QSocTokenizer::count(text), corpusTokens);
    }

    void pathologicalInputStaysNearLinear()
    {
        const QList<QPair<QString, qint64>> inputs
            = {{QString(1 << 20, QLatin1Char('a')), repeatedTokens},
               {ideographs(), ideographTokens}};
        for (const auto &[text, expected] : inputs) {
            QElapsedTimer timer;
            timer.start();
            QCOMPARE(qint64(QSocTokenizer::encode(text).size()), expected);
            QVERIFY2(timer.elapsed() < 20000, qPrintable(QString::number(timer.elapsed())));
        }
    }

    void loneSurrogatesCountAsReplacement()
    {
        const QString broken   = QStringLiteral("a") + QChar(0xD800) + QStringLiteral("b")
                                 + QChar(0xDC00);
        const QString replaced = QStringLiteral("a") + QChar(QChar::ReplacementCharacter)
                                 + QStringLiteral("b") + QChar(QChar::ReplacementCharacter);
        QCOMPARE(QSocTokenizer::encode(broken), QSocTokenizer::encode(replaced));
        QCOMPARE(
            QSocTokenizer::count(broken, QSocTokenizer::Mode::Bytes),
            QSocTokenizer::count(replaced, QSocTokenizer::Mode::Bytes));
    }

    void bytesModeRoundsUpQuarterBytes()
    {
        using Mode = QSocTokenizer::Mode;
        QCOMPARE(QSocTokenizer::count(QString(), Mode::Bytes), qint64(0));
        QCOMPARE(QSocTokenizer::count(QStringLiteral("abcd"), Mode::Bytes), qint64(1));
        QCOMPARE(QSocTokenizer::count(QStringLiteral("abcde"), Mode::Bytes), qint64(2));
        QCOMPARE(QSocTokenizer::count(QString::fromUtf16(u"你"), Mode::Bytes), qint64(1));
        QCOMPARE(QSocTokenizer::count(QString::fromUtf16(u"\U0001F468你"), Mode::Bytes), qint64(2));
    }

    void truncateKeepsAPrefixWithinTheLimit()
    {
        const QString text = corpus().left(20000);
        for (const auto mode : {QSocTokenizer::Mode::O200k, QSocTokenizer::Mode::Bytes}) {
            const qint64 total = QSocTokenizer::count(text, mode);
            QCOMPARE(QSocTokenizer::truncate(text, total, mode), text);
            QVERIFY(QSocTokenizer::truncate(text, 0, mode).isEmpty());
            qint64 previous = 0;
            for (qint64 limit = 1; limit < total; limit += 97) {
                const QString prefix = QSocTokenizer::truncate(text, limit, mode);
                const qint64  used   = QSocTokenizer::count(prefix, mode);
                QVERIFY(text.startsWith(prefix));
                QVERIFY(used <= limit);
                QVERIFY(used + 8 >= limit);
                QVERIFY(prefix.size() >= previous);
                previous = prefix.size();
            }
        }
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsoctokenizer.moc"
