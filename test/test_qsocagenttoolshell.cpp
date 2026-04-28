// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolshell.h"
#include "qsoc_test.h"

#include <QtCore>
#include <QtTest>

namespace {

struct TestApp
{
    static auto &instance()
    {
        static auto                   argc      = 1;
        static char                   appName[] = "qsoc";
        static std::array<char *, 1>  argv      = {{appName}};
        static const QCoreApplication app       = QCoreApplication(argc, argv.data());
        return app;
    }
};

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { TestApp::instance(); }

    void prompt_ynVariants()
    {
        /* Detector returns the last match in the trailing slice; the
         * "(y/n)" parenthesized form should win over a vaguer "are you
         * sure" earlier in the same line. */
        QCOMPARE(QSocToolShellBash::detectInteractivePrompt("Continue (y/n)?"), QString("(y/n)"));
        QCOMPARE(QSocToolShellBash::detectInteractivePrompt("Are you sure (Y/N)? "), QString("(Y/N)"));
        /* Bracket variant is intentionally NOT supported: "[n/Y]" gets
         * past the parens-only matcher. */
        QVERIFY(QSocToolShellBash::detectInteractivePrompt("Proceed [n/Y] ").isEmpty());
    }

    void prompt_pressEnter()
    {
        QVERIFY(!QSocToolShellBash::detectInteractivePrompt("Press Enter to continue").isEmpty());
        QVERIFY(!QSocToolShellBash::detectInteractivePrompt("press any key to continue").isEmpty());
    }

    void prompt_password()
    {
        QCOMPARE(
            QSocToolShellBash::detectInteractivePrompt("[sudo] password: "), QString("password:"));
        QCOMPARE(
            QSocToolShellBash::detectInteractivePrompt("Enter passphrase: "),
            QString("passphrase:"));
    }

    void prompt_negativeCases()
    {
        /* Real progress output that mentions yes/no in passing must NOT
         * trigger; the regex is anchored to the trailing fragment. */
        QVERIFY(QSocToolShellBash::detectInteractivePrompt("yes I will do it later").isEmpty());
        QVERIFY(QSocToolShellBash::detectInteractivePrompt("hello world").isEmpty());
        /* "every PR" style trailing words that vaguely look like prompts. */
        QVERIFY(
            QSocToolShellBash::detectInteractivePrompt("printed continue successfully").isEmpty());
    }

    void prompt_promptMidStreamDecaysAsTailGrows()
    {
        /* If "(y/n)?" appeared 200 chars ago, the new tail's last 120
         * chars should not match. The detector slices to 120 chars. */
        QString tail = QStringLiteral("(y/n)?\n") + QString(200, QLatin1Char('x'));
        QVERIFY(QSocToolShellBash::detectInteractivePrompt(tail).isEmpty());
    }
};

} /* namespace */

QSOC_TEST_MAIN(Test)
#include "test_qsocagenttoolshell.moc"
