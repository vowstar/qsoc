// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocmemoryrecall.h"
#include "qsoc_test.h"

#include <QtCore>
#include <QtTest>

class Test : public QObject
{
    Q_OBJECT

    /* Build a header list for the selection/assembly tests. */
    static QList<QSocMemoryManager::MemoryHeader> sampleHeaders()
    {
        QList<QSocMemoryManager::MemoryHeader> headers;
        const auto                             make =
            [](const QString &name, const QString &type, const QString &desc, int ageDays) {
                QSocMemoryManager::MemoryHeader header;
                header.scope       = QStringLiteral("user");
                header.name        = name;
                header.type        = type;
                header.description = desc;
                header.ageDays     = ageDays;
                return header;
            };
        headers.append(make("user-role", "user", "User is an ASIC designer", 0));
        headers.append(make("build-tips", "feedback", "Use ninja, -j16", 3));
        headers.append(make("api-quirks", "reference", "chat API message format quirks", 10));
        return headers;
    }

private slots:
    /* queryIsRecallable: needs at least two whitespace tokens. */
    void testQueryRecallable()
    {
        QVERIFY(!QSocMemoryRecall::queryIsRecallable(QStringLiteral("hello")));
        QVERIFY(!QSocMemoryRecall::queryIsRecallable(QStringLiteral("   word   ")));
        QVERIFY(QSocMemoryRecall::queryIsRecallable(QStringLiteral("fix the build")));
        QVERIFY(!QSocMemoryRecall::queryIsRecallable(QString()));
    }

    /* Selector prompt lists every candidate with its metadata. */
    void testBuildSelectorPrompt()
    {
        QSocMemoryRecall::Config cfg;
        cfg.maxFiles = 5;
        const QSocMemoryRecall recall(cfg);

        const QString prompt
            = recall.buildSelectorPrompt(sampleHeaders(), QStringLiteral("how do I build"));
        QVERIFY(prompt.contains(QStringLiteral("how do I build")));
        QVERIFY(prompt.contains(QStringLiteral("user-role")));
        QVERIFY(prompt.contains(QStringLiteral("build-tips")));
        QVERIFY(prompt.contains(QStringLiteral("[feedback]")));
        QVERIFY(prompt.contains(QStringLiteral("Select up to 5")));
    }

    /* parseSelection tolerates raw JSON, code fences, prose, bare arrays. */
    void testParseSelection()
    {
        QCOMPARE(
            QSocMemoryRecall::parseSelection(
                QStringLiteral("{\"selected_memories\":[\"a\",\"b\"]}")),
            (QStringList{"a", "b"}));

        QCOMPARE(
            QSocMemoryRecall::parseSelection(
                QStringLiteral("Sure!\n```json\n{\"selected_memories\":[\"build-tips\"]}\n```\n")),
            (QStringList{"build-tips"}));

        QCOMPARE(
            QSocMemoryRecall::parseSelection(QStringLiteral("[\"x\", \"y\"]")),
            (QStringList{"x", "y"}));

        QCOMPARE(QSocMemoryRecall::parseSelection(QStringLiteral("not json at all")), QStringList{});
        QCOMPARE(
            QSocMemoryRecall::parseSelection(QStringLiteral("{\"selected_memories\":[]}")),
            QStringList{});
    }

    /* stripFrontmatter removes the leading YAML block. */
    void testStripFrontmatter()
    {
        const QString withFm = QStringLiteral("---\nname: x\ntype: user\n---\n\nThe body here.\n");
        QCOMPARE(QSocMemoryRecall::stripFrontmatter(withFm), QStringLiteral("The body here."));

        const QString noFm = QStringLiteral("Just content, no frontmatter.");
        QCOMPARE(QSocMemoryRecall::stripFrontmatter(noFm), noFm);

        /* A body that merely opens with a --- thematic rule (no key: line)
         * must be preserved, not eaten as frontmatter. */
        const QString rule = QStringLiteral("---\nIntro paragraph\n---\nSection\n");
        QCOMPARE(QSocMemoryRecall::stripFrontmatter(rule), rule);
    }

    /* assembleBlock wraps selected memories with a marker + freshness. */
    void testAssembleBlock()
    {
        QSocMemoryRecall::Config cfg;
        cfg.maxFiles   = 5;
        cfg.perFileCap = 4096;
        cfg.turnBudget = 61440;
        const QSocMemoryRecall recall(cfg);

        const auto reader = [](const QString &scope, const QString &name) -> QString {
            Q_UNUSED(scope);
            return QStringLiteral("---\nname: %1\n---\n\nbody of %1").arg(name);
        };

        const QString block = recall.assembleBlock(sampleHeaders(), reader);
        QVERIFY(block.startsWith(QStringLiteral("<recalled_memory>")));
        QVERIFY(block.endsWith(QStringLiteral("</recalled_memory>")));
        QVERIFY(block.contains(QStringLiteral("## user-role (updated today)")));
        QVERIFY(block.contains(QStringLiteral("## build-tips (updated 3 days ago)")));
        QVERIFY(block.contains(QStringLiteral("body of api-quirks")));
        /* Frontmatter must not leak into the injected block. */
        QVERIFY(!block.contains(QStringLiteral("name: user-role")));
    }

    /* Empty selection or missing reader yields an empty block. */
    void testAssembleEmpty()
    {
        QSocMemoryRecall::Config cfg;
        const QSocMemoryRecall   recall(cfg);
        const auto reader = [](const QString &, const QString &) { return QString(); };

        QVERIFY(recall.assembleBlock({}, reader).isEmpty());
        QVERIFY(recall.assembleBlock(sampleHeaders(), nullptr).isEmpty());
        /* Reader returns empty bodies -> nothing to surface. */
        QVERIFY(recall.assembleBlock(sampleHeaders(), reader).isEmpty());
    }

    /* Per-file cap truncates an oversized body and marks it. */
    void testPerFileCap()
    {
        QSocMemoryRecall::Config cfg;
        cfg.maxFiles   = 5;
        cfg.perFileCap = 200;
        cfg.turnBudget = 61440;
        const QSocMemoryRecall recall(cfg);

        const auto reader = [](const QString &, const QString &) {
            return QStringLiteral("line\n").repeated(500); /* ~2500 bytes */
        };

        QList<QSocMemoryManager::MemoryHeader> one   = {sampleHeaders().first()};
        const QString                          block = recall.assembleBlock(one, reader);
        QVERIFY(block.contains(QStringLiteral("(truncated)")));
        QVERIFY(block.toUtf8().size() < 1000);
    }

    /* Byte-offset cap on multibyte content must not leave a U+FFFD. */
    void testPerFileCapMultibyte()
    {
        QSocMemoryRecall::Config cfg;
        cfg.maxFiles = 5;
        /* 98 = 32*3 + 2, so the cut lands 2 bytes into a 3-byte char and
         * Qt leaves 2 trailing U+FFFD: exercises the multi-chop loop. */
        cfg.perFileCap = 98;
        cfg.turnBudget = 61440;
        const QSocMemoryRecall recall(cfg);

        /* CJK chars are 3 bytes in UTF-8, no newlines to recover on. */
        const auto reader = [](const QString &, const QString &) {
            return QStringLiteral("你好").repeated(200);
        };

        QList<QSocMemoryManager::MemoryHeader> one   = {sampleHeaders().first()};
        const QString                          block = recall.assembleBlock(one, reader);
        QVERIFY(!block.contains(QChar(QChar::ReplacementCharacter)));
        QVERIFY(block.contains(QStringLiteral("(truncated)")));
    }

    /* Turn budget stops injection after the cap is reached. */
    void testTurnBudget()
    {
        QSocMemoryRecall::Config cfg;
        cfg.maxFiles   = 5;
        cfg.perFileCap = 4096;
        cfg.turnBudget = 500; /* room for one ~391-byte entry, not two */
        const QSocMemoryRecall recall(cfg);

        const auto reader = [](const QString &, const QString &name) {
            return QStringLiteral("body of %1 ").arg(name).repeated(20);
        };

        const QString block = recall.assembleBlock(sampleHeaders(), reader);
        /* Only the first entry fits under the budget. */
        QVERIFY(block.contains(QStringLiteral("## user-role")));
        QVERIFY(!block.contains(QStringLiteral("## build-tips")));
        QVERIFY(!block.contains(QStringLiteral("## api-quirks")));
    }
};

QSOC_TEST_MAIN(Test)

#include "test_qsocmemoryrecall.moc"
