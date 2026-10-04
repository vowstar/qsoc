// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "tui/qsocdiagram.h"
#include "tui/qtuicodeblock.h"
#include "tui/qtuicompositor.h"
#include "tui/qtuiscreen.h"
#include "tui/qtuiwidget.h"
#include <QStringDecoder>
#include <QtTest>

namespace {
QStringList textRows(const QSocDiagram::Result &result)
{
    QStringList rows;
    for (const auto &row : result.rows) {
        QString text;
        for (const auto &span : row)
            text += span.text;
        rows.append(text);
    }
    return rows;
}
QString screenText(QTuiBlock &block, int width)
{
    block.layout(width);
    QTuiScreen screen(width, block.rowCount());
    for (int row = 0; row < block.rowCount(); ++row)
        block.paintRow(screen, row, row, 0, width, false, false);
    QStringList result;
    for (int y = 0; y < screen.height(); ++y) {
        QString line;
        for (int x = 0; x < screen.width(); ++x) {
            const auto scalar = screen.at(x, y).codePoint();
            line += QString::fromUcs4(&scalar, 1);
            if (QTuiText::isWideChar(scalar))
                ++x;
        }
        result.append(line.trimmed());
    }
    return result.join('\n');
}
struct Box
{
    int   start;
    int   end;
    int   border;
    QChar label;
};
QList<QPair<QChar, QChar>> routedEdges(const QStringList &rows, bool vertical)
{
    const auto at = [&rows](int x, int y) {
        return y >= 0 && y < rows.size() && x >= 0 && x < rows[y].size() ? rows[y][x] : QChar(' ');
    };
    QList<Box> boxes;
    const int  length = vertical ? rows.size() : rows.first().size();
    for (int pos = 0; pos < length; ++pos) {
        if ((vertical ? at(0, pos) : at(pos, 0)) != QChar(u'┌'))
            continue;
        int end = pos + 1;
        while (end < length
               && (vertical ? at(0, end) : at(end, 0)) != (vertical ? QChar(u'└') : QChar(u'┐')))
            ++end;
        int border = 1;
        while (border < 1000
               && (vertical ? at(border, pos) : at(pos, border))
                      != (vertical ? QChar(u'┐') : QChar(u'└')))
            ++border;
        boxes.append({pos, end, border, vertical ? at(2, pos + 1) : at(pos + 2, 1)});
    }
    QList<QPair<QChar, QChar>> edges;
    for (const auto &box : boxes)
        for (int position = box.start + 1; position < box.end; ++position) {
            int x = vertical ? box.border + 1 : position;
            int y = vertical ? position : box.border + 1;
            if (at(x, y) != (vertical ? QChar(u'─') : QChar(u'│')))
                continue;
            for (int step = 0; step < 1000; ++step) {
                if (QStringLiteral("┐┘└").contains(at(x, y)))
                    break;
                if (vertical)
                    ++x;
                else
                    ++y;
            }
            const int turn = vertical ? (at(x, y) == QChar(u'┐') ? 1 : -1)
                                      : (at(x, y) == QChar(u'└') ? 1 : -1);
            for (int step = 0; step < 1000; ++step) {
                if (vertical)
                    y += turn;
                else
                    x += turn;
                if (QStringLiteral("┐┘└").contains(at(x, y)))
                    break;
            }
            const int targetPosition = vertical ? y : x;
            for (const auto &target : boxes) {
                if (targetPosition <= target.start || targetPosition >= target.end)
                    continue;
                const auto tip = vertical ? at(target.border + 1, y) : at(x, target.border + 1);
                if (tip == (vertical ? QChar(u'◄') : QChar(u'▲')))
                    edges.append({box.label, target.label});
            }
        }
    std::sort(edges.begin(), edges.end());
    return edges;
}
class Test : public QObject
{
    Q_OBJECT
private slots:
    void families_data()
    {
        QTest::addColumn<QString>("source");
        QTest::addColumn<QStringList>("labels");
        QTest::newRow("flow") << QString::fromUcs4(
            U"flowchart LR\nA([输入🚀]) & B{𝔸?} -->|ready| C[Done]\nC -. retry .-> A\nC <--> C")
                              << QStringList{
                                     QString::fromUcs4(U"输入🚀"),
                                     QString::fromUcs4(U"𝔸?"),
                                     "ready",
                                     "retry",
                                     "Done"};
        QTest::newRow("sequence") << QString(
            "sequenceDiagram\nactor U as User\nparticipant S as Service\nloop requests\nU->>S: "
            "start\nalt ready\nS-->>U: value\nelse wait\nS->>S: retry\nend\nNote over U,S: "
            "complete\nend")
                                  << QStringList{
                                         "User (actor)",
                                         "Service",
                                         "loop requests",
                                         "alt ready",
                                         "else wait",
                                         "start",
                                         "value",
                                         "retry",
                                         "Note: complete"};
        QTest::newRow("state") << QString(
            "stateDiagram-v2\ndirection LR\nstate \"Waiting\" as Idle\nIdle: entry\n[*] --> "
            "Idle\nIdle --> Done: finish\nDone --> [*]")
                               << QStringList{
                                      QStringLiteral("● initial"),
                                      "Waiting",
                                      "entry",
                                      "Done",
                                      "finish",
                                      QStringLiteral("◎ final")};
        QTest::newRow("class") << QString(
            "classDiagram\nclass Base {\n+name: String\n+run()\n}\nBase <|.. \"many\" Derived : "
            "realizes\nDerived *-- Item\nItem o-- Other")
                               << QStringList{
                                      "Base",
                                      "+name: String",
                                      "+run()",
                                      "Derived",
                                      "realizes",
                                      "(many)",
                                      QStringLiteral("◁"),
                                      QStringLiteral("◆"),
                                      QStringLiteral("◇")};
        QTest::newRow("er") << QString(
            "erDiagram\nUser {\nint id PK\nstring name UK \"display name\"\n}\nUser ||--o{ Record "
            ": owns\nRecord }o..o| Group : belongs")
                            << QStringList{
                                   "User",
                                   "int id PK",
                                   "display name",
                                   "Record",
                                   "(1)",
                                   "(0..many)",
                                   "(0..1)",
                                   "owns",
                                   "belongs"};
    }
    void families()
    {
        QFETCH(QString, source);
        QFETCH(QStringList, labels);
        const auto result = QSocDiagram::render(source, 400);
        QVERIFY(result.valid());
        const auto rows = textRows(result);
        const auto text = rows.join('\n');
        for (const auto &label : labels)
            QVERIFY2(text.contains(label), qPrintable(label + '\n' + text));
        for (const auto &row : rows)
            QVERIFY(QTuiText::visualWidth(row) <= 400);
    }
    void rejects_data()
    {
        QTest::addColumn<QString>("source");
        QTest::newRow("unknown-family") << QString("pie\nA: 1");
        QTest::newRow("directive") << QString("%%{init: {}}\nflowchart\nA");
        QTest::newRow("subgraph") << QString("graph TD\nsubgraph X\nA\nend");
        QTest::newRow("shape") << QString("graph TD\nA[[subroutine]]");
        QTest::newRow("html") << QString("graph TD\nA[<b>bold</b>]");
        QTest::newRow("entity") << QString("graph TD\nA[&amp;]");
        QTest::newRow("unsupported-symbol-width") << QStringLiteral("graph\nA[☕]");
        QTest::newRow("unsupported-presentation") << QStringLiteral("graph\nA[☕️]");
        QTest::newRow("joined-letters") << QStringLiteral("graph\nA[لا]");
        QTest::newRow("combining") << QString::fromUcs4(U"graph TD\nA[e\u0301]");
        QTest::newRow("joiner") << QString::fromUcs4(U"graph TD\nA[👩\u200d💻]");
        QTest::newRow("conflicting-label") << QString("graph TD\nA[first]\nA[second]");
        QTest::newRow("sequence-open") << QString("sequenceDiagram\nA->>B: hi\nloop pending");
        QTest::newRow("sequence-else") << QString("sequenceDiagram\nA->>B: hi\nelse pending");
        QTest::newRow("sequence-direction") << QString("sequenceDiagram\nA=>B: hi");
        QTest::newRow("state-compound") << QString("stateDiagram\nstate A {\nB\n}");
        QTest::newRow("class-inline") << QString("classDiagram\nclass A {member}");
        QTest::newRow("er-invalid-key") << QString("erDiagram\nA {\nint id INDEX\n}");
        QTest::newRow("er-quoted-label") << QString("erDiagram\nA ||--|| B: \"name\"");
    }
    void rejects()
    {
        QFETCH(QString, source);
        const auto result = QSocDiagram::render(source, 400);
        QVERIFY(!result.valid());
        QVERIFY(result.rows.isEmpty());
    }
    void limits()
    {
        QCOMPARE(QSocDiagram::render(QString(16385, 'x'), 400).error, QSocDiagram::Error::Limit);
        QCOMPARE(
            QSocDiagram::render("graph\nA[" + QString(41, 'x') + "]", 400).error,
            QSocDiagram::Error::Limit);
        QString nodes = "graph\n";
        for (int i = 0; i < 17; ++i)
            nodes += QString("N%1\n").arg(i);
        QCOMPARE(QSocDiagram::render(nodes, 400).error, QSocDiagram::Error::Limit);
        QString edges = "graph\n";
        for (int i = 0; i < 25; ++i)
            edges += "A-->B\n";
        QCOMPARE(QSocDiagram::render(edges, 400).error, QSocDiagram::Error::Limit);
        QCOMPARE(QSocDiagram::render("graph\nA-->B", 3).error, QSocDiagram::Error::TooWide);
    }
    void relationshipEndpoints()
    {
        const QStringList left{"", "<", "<|", "*", "o"};
        const QStringList right{"", ">", "|>", "*", "o"};
        const QStringList tips{
            QStringLiteral("─"),
            QStringLiteral("◄"),
            QStringLiteral("◁"),
            QStringLiteral("◆"),
            QStringLiteral("◇")};
        for (int a = 0; a < left.size(); ++a)
            for (int b = 0; b < right.size(); ++b)
                for (const QString &stem : {"--", ".."}) {
                    const auto source = "classDiagram\nA \"one\" " + left[a] + stem + right[b]
                                        + " \"many\" B : relation";
                    const auto result = QSocDiagram::render(source, 120);
                    QVERIFY(result.valid());
                    const auto text = textRows(result).join('\n');
                    QVERIFY(text.contains(tips[a] + "(one) relation"));
                    QVERIFY(text.contains(tips[b] + "(many)"));
                    if (stem == "..")
                        QVERIFY(text.contains(QStringLiteral("┆")));
                }
        const QStringList sourceCards{"||", "|o", "}|", "}o"};
        const QStringList targetCards{"||", "o|", "|{", "o{"};
        const QStringList labels{"1", "0..1", "1..many", "0..many"};
        for (int a = 0; a < 4; ++a)
            for (int b = 0; b < 4; ++b)
                for (const QString &stem : {"--", ".."}) {
                    const auto result = QSocDiagram::render(
                        "erDiagram\nA " + sourceCards[a] + stem + targetCards[b] + " B : relation",
                        120);
                    QVERIFY(result.valid());
                    const auto rows = textRows(result);
                    const auto text = rows.join('\n');
                    QVERIFY(text.contains('(' + labels[a] + ") relation"));
                    QVERIFY(text.contains('(' + labels[b] + ')'));
                    QVERIFY(text.indexOf("relation") < text.indexOf(" B "));
                }
    }
    void nestedSequenceBranches()
    {
        const auto result = QSocDiagram::render(
            "sequenceDiagram\nalt ready\nA->>B: request\nloop each\nB-->>A: reply\nend\nelse "
            "absent\nB--xA: fail\nend",
            120);
        QVERIFY(result.valid());
        const auto rows = textRows(result);
        const auto text = rows.join('\n');
        QVERIFY(text.indexOf("request") < text.indexOf("reply"));
        QVERIFY(text.indexOf("reply") < text.indexOf("else absent"));
        QVERIFY(text.indexOf("else absent") < text.indexOf("fail"));
        for (const auto &row : rows)
            if (row.contains("else absent")) {
                QVERIFY(row.startsWith(QStringLiteral("├")));
                QVERIFY(row.endsWith(QStringLiteral("┤")));
            }
    }
    void familyLimitsAndWidth()
    {
        QString members = "classDiagram\nclass A {\n";
        for (int i = 0; i < 17; ++i)
            members += "+member\n";
        QCOMPARE(QSocDiagram::render(members + "}\n", 400).error, QSocDiagram::Error::Limit);
        QString people = "sequenceDiagram\n";
        for (int i = 0; i < 9; ++i)
            people += QString("participant N%1\n").arg(i);
        QCOMPARE(QSocDiagram::render(people, 400).error, QSocDiagram::Error::Limit);
        QString events = "sequenceDiagram\n";
        for (int i = 0; i < 65; ++i)
            events += "A->B: event\n";
        QCOMPARE(QSocDiagram::render(events, 400).error, QSocDiagram::Error::Limit);
        QCOMPARE(
            QSocDiagram::render("sequenceDiagram\nloop a\nloop b\nloop c\nloop d\nloop e", 400).error,
            QSocDiagram::Error::Limit);
        for (const QString &source :
             {"graph LR\nA-->B",
              "sequenceDiagram\nA->>B: hi",
              "stateDiagram\n[*]-->A",
              "classDiagram\nA<|--B",
              "erDiagram\nA||--o{B: owns"}) {
            const auto result = QSocDiagram::render(source, 400);
            QVERIFY(result.valid());
            int width = 0;
            for (const auto &row : textRows(result))
                width = std::max(width, QTuiText::visualWidth(row));
            QVERIFY(QSocDiagram::render(source, width).valid());
            QCOMPARE(QSocDiagram::render(source, width - 1).error, QSocDiagram::Error::TooWide);
            for (int cut = 0; cut < source.size(); ++cut) {
                const auto prefix = QSocDiagram::render(source.first(cut), 40);
                for (const auto &row : textRows(prefix))
                    QVERIFY(QTuiText::visualWidth(row) <= 40);
            }
        }
    }
    void everyThreeNodeTopology()
    {
        for (const QString &direction : {"TD", "BT", "LR", "RL"})
            for (int mask = 0; mask < 512; ++mask) {
                QString                    source = "graph " + direction + "\nA\nB\nC\n";
                QList<QPair<QChar, QChar>> expected;
                for (int bit = 0; bit < 9; ++bit)
                    if (mask & (1 << bit)) {
                        const QChar from('A' + bit / 3), to('A' + bit % 3);
                        source += from + QString("-->") + to + '\n';
                        expected.append({from, to});
                    }
                std::sort(expected.begin(), expected.end());
                const auto result = QSocDiagram::render(source, 400);
                QVERIFY(result.valid());
                QCOMPARE(
                    routedEdges(textRows(result), direction == "TD" || direction == "BT"), expected);
            }
    }
    void copyResizeAndClosedFence()
    {
        const QString source = "flowchart TD\nA-->B\n";
        QTuiCodeBlock block("mermaid", source, true, 1);
        QVERIFY(screenText(block, 80).contains("A-->B"));
        block.setClosed();
        const auto drawn = screenText(block, 80);
        QVERIFY(drawn.contains(QStringLiteral("┌")));
        QVERIFY(!drawn.contains("A-->B"));
        QCOMPARE(block.toPlainText(), source);
        QCOMPARE(block.toMarkdown(), "```mermaid\n" + source + "```\n");
        QVERIFY(!screenText(block, 8).contains(QStringLiteral("┌")));
        QCOMPARE(screenText(block, 80), drawn);
        QVERIFY(
            block.selectedLogicalText(1, 0, block.rowCount() - 1, 80).contains(QStringLiteral("┌")));
        QTuiScreen screen(80, block.rowCount());
        block.paintRow(screen, 1, 1, 0, 80, false, false);
        QVERIFY(screen.at(0, 1).dim);
        QVERIFY(screen.at(0, 1).italic);
    }
    void streaming()
    {
        const QString source = QString::fromUcs4(U"```mermaid\ngraph\nA[🚀]-->B\n```");
        const auto    bytes  = source.toUtf8();
        for (int cut = 0; cut <= bytes.size(); ++cut) {
            if (cut < bytes.size() && (static_cast<unsigned char>(bytes[cut]) & 0xc0) == 0x80)
                continue;
            QTuiCompositor compositor;
            compositor.appendAssistantChunk(QString::fromUtf8(bytes.first(cut)));
            compositor.appendAssistantChunk(QString::fromUtf8(bytes.sliced(cut)));
            compositor.finishStream();
            auto *block = compositor.contentView().lastBlock();
            QVERIFY(block);
            QVERIFY(screenText(*block, 80).contains(QStringLiteral("┌")));
            QCOMPARE(block->toMarkdown(), source + '\n');
        }
        QTuiCompositor incomplete;
        incomplete.appendAssistantChunk("```mermaid\ngraph\nA-->B\n");
        incomplete.finishStream();
        QVERIFY(screenText(*incomplete.contentView().lastBlock(), 80).contains("A-->B"));
    }
};
} // namespace
QSOC_TEST_MAIN(Test)
#include "test_qsocdiagram.moc"
