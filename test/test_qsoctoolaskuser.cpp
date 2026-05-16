// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolaskuser.h"
#include "qsoc_test.h"

#include <QtTest>

namespace {

QSocToolAskUser::Callback constantPick(const QString &label)
{
    return [label](const QString &, const QString &, const QList<QSocAskUserOption> &)
               -> QSocAskUserResult {
        QSocAskUserResult result;
        result.choice = label;
        return result;
    };
}

QSocToolAskUser::Callback otherWith(const QString &text)
{
    return [text](const QString &, const QString &, const QList<QSocAskUserOption> &)
               -> QSocAskUserResult {
        QSocAskUserResult result;
        result.choice = QStringLiteral("Other");
        result.text   = text;
        return result;
    };
}

QSocToolAskUser::Callback cancelCallback()
{
    return [](const QString &, const QString &, const QList<QSocAskUserOption> &) {
        QSocAskUserResult result;
        result.canceled = true;
        return result;
    };
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void testNameAndSchemaShape()
    {
        QSocToolAskUser tool(nullptr, nullptr);
        QCOMPARE(tool.getName(), QStringLiteral("ask_user"));
        const json schema = tool.getParametersSchema();
        QVERIFY(schema.is_object());
        QVERIFY(schema["properties"].contains("question"));
        QVERIFY(schema["properties"].contains("options"));
        QVERIFY(schema["properties"]["options"].contains("minItems"));
        QCOMPARE(schema["properties"]["options"]["minItems"].get<int>(), 2);
        QCOMPARE(schema["properties"]["options"]["maxItems"].get<int>(), 4);
    }

    void testNoCallbackReturnsError()
    {
        QSocToolAskUser tool(nullptr, nullptr);
        const QString   out = tool.execute(
            {{"question", "q?"}, {"options", json::array({{{"label", "a"}}, {{"label", "b"}}})}});
        QVERIFY(out.contains(QStringLiteral("sub-agent context")));
    }

    void testPicksOption()
    {
        QSocToolAskUser tool(nullptr, constantPick(QStringLiteral("alpha")));
        const QString   out = tool.execute(
            {{"question", "Pick one"},
             {"options",
              json::array({{{"label", "alpha"}, {"description", "first"}}, {{"label", "beta"}}})}});
        QVERIFY2(out.contains(QStringLiteral("\"choice\":\"alpha\"")), qPrintable(out));
        QVERIFY(out.contains(QStringLiteral("\"status\":\"ok\"")));
    }

    void testOtherBranchReturnsText()
    {
        QSocToolAskUser tool(nullptr, otherWith(QStringLiteral("custom answer")));
        const QString   out = tool.execute(
            {{"question", "Which?"},
             {"options", json::array({{{"label", "a"}}, {{"label", "b"}}})}});
        QVERIFY2(out.contains(QStringLiteral("\"choice\":\"Other\"")), qPrintable(out));
        QVERIFY(out.contains(QStringLiteral("\"text\":\"custom answer\"")));
    }

    void testCancelReturnsCanceled()
    {
        QSocToolAskUser tool(nullptr, cancelCallback());
        const QString   out = tool.execute(
            {{"question", "Which?"},
             {"options", json::array({{{"label", "a"}}, {{"label", "b"}}})}});
        QVERIFY2(out.contains(QStringLiteral("\"status\":\"canceled\"")), qPrintable(out));
    }

    void testRejectMissingQuestion()
    {
        QSocToolAskUser tool(nullptr, constantPick(QStringLiteral("a")));
        const QString   out = tool.execute(
            {{"options", json::array({{{"label", "a"}}, {{"label", "b"}}})}});
        QVERIFY(out.contains(QStringLiteral("question is required")));
    }

    void testRejectTooFewOptions()
    {
        QSocToolAskUser tool(nullptr, constantPick(QStringLiteral("a")));
        const QString   out = tool.execute(
            {{"question", "q?"}, {"options", json::array({{{"label", "a"}}})}});
        QVERIFY(out.contains(QStringLiteral("between 2 and 4")));
    }

    void testRejectTooManyOptions()
    {
        QSocToolAskUser tool(nullptr, constantPick(QStringLiteral("a")));
        const QString   out = tool.execute(
            {{"question", "q?"},
             {"options",
              json::array(
                  {{{"label", "a"}},
                   {{"label", "b"}},
                   {{"label", "c"}},
                   {{"label", "d"}},
                   {{"label", "e"}}})}});
        QVERIFY(out.contains(QStringLiteral("between 2 and 4")));
    }

    void testRejectExplicitOther()
    {
        QSocToolAskUser tool(nullptr, constantPick(QStringLiteral("a")));
        const QString   out = tool.execute(
            {{"question", "q?"},
             {"options", json::array({{{"label", "a"}}, {{"label", "Other..."}}})}});
        QVERIFY(out.contains(QStringLiteral("appended automatically")));
    }
};

} // namespace

#include "test_qsoctoolaskuser.moc"

QSOC_TEST_MAIN(Test)
