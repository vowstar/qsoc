// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"
#include "common/qsocconsole.h"
#include "qsoc_prcm_fixture.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

namespace {

class Test : public QObject
{
    Q_OBJECT

private:
    static QStringList messages;
    QtMessageHandler   previous = nullptr;
    static void        capture(QtMsgType, const QMessageLogContext &, const QString &message)
    {
        messages.append(message);
    }

private slots:
    void initTestCase()
    {
        previous = qInstallMessageHandler(capture);
        QSocConsole::setTeeToMessageHandler(true);
    }

    void cleanupTestCase()
    {
        QSocConsole::setTeeToMessageHandler(false);
        qInstallMessageHandler(previous);
    }

    void merge()
    {
        QTemporaryDir directory(QDir::tempPath() + "/test_qsoc_prcm_merge_cli-XXXXXX");
        QVERIFY(directory.isValid());
        auto       control = YAML::Load(qsocPrcmDeclaration());
        YAML::Node clock(YAML::NodeType::Map);
        clock["clock"] = YAML::Clone(control["clock"]);
        control.remove("clock");
        const auto                      a = directory.filePath("clock.soc_net");
        const auto                      b = directory.filePath("control.soc_net");
        const QMap<QString, YAML::Node> files{{a, clock}, {b, control}};
        for (auto it = files.cbegin(); it != files.cend(); ++it) {
            QFile file(it.key());
            QVERIFY(file.open(QIODevice::WriteOnly));
            const auto data = QByteArray::fromStdString(YAML::Dump(it.value()));
            QCOMPARE(file.write(data), data.size());
        }
        messages.clear();
        QSocCliWorker worker;
        QSignalSpy    exitSpy(&worker, &QSocCliWorker::exit);
        worker.setup({"qsoc", "generate", "verilog", "--check", "--merge", a, b}, false);
        worker.run();
        QCOMPARE(exitSpy.size(), 1);
        QCOMPARE(exitSpy[0][0].toInt(), 0);
        QVERIFY(messages.join('\n').contains("2 stable mode queries pass"));
        QCOMPARE(
            QDir(directory.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot),
            (QStringList{"clock.soc_net", "control.soc_net"}));
    }

    void check_data()
    {
        QTest::addColumn<QString>("fault");
        QTest::newRow("valid") << QString();
        for (const auto &name : {"source", "mode", "format", "force", "missing", "yaml"}) {
            QTest::newRow(name) << QString(name);
        }
    }

    void check()
    {
        QFETCH(QString, fault);
        QTemporaryDir directory(QDir::tempPath() + "/test_qsoc_prcm_cli-XXXXXX");
        QVERIFY(directory.isValid());
        auto node = YAML::Load(qsocPrcmDeclaration());
        if (fault == "source")
            node["reset"][0]["target"]["periph_n"]["link"].remove("hold_n");
        if (fault == "mode")
            node["prcm"]["domain"]["periph"]["mode"]["RUN"]["power"] = "off";
        const auto path = directory.filePath("control.soc_net");
        if (fault != "missing") {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            const auto data = fault == "yaml" ? QByteArray("clock: [")
                                              : QByteArray::fromStdString(YAML::Dump(node));
            QCOMPARE(file.write(data), data.size());
        }
        const QStringList before
            = QDir(directory.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
        messages.clear();
        QStringList args{"qsoc", "generate", "verilog", "--check", "-d", directory.path()};
        if (fault == "format" || fault == "force")
            args.append("--" + fault);
        args.append(path);
        QSocCliWorker worker;
        QSignalSpy    exitSpy(&worker, &QSocCliWorker::exit);
        worker.setup(args, false);
        worker.run();
        QCOMPARE(exitSpy.size(), 1);
        QCOMPARE(exitSpy[0][0].toInt(), fault.isEmpty() ? 0 : 1);
        QCOMPARE(QDir(directory.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), before);
        const auto output = messages.join('\n');
        if (fault.isEmpty())
            QVERIFY(output.contains("2 stable mode queries pass"));
        if (fault == "source") {
            QVERIFY(output.contains("PRCM_RESOURCE_REFERENCE"));
            QVERIFY(output.contains("reset[0].target.periph_n.link"));
            QVERIFY(output.contains(path + ":"));
        }
        if (fault == "mode") {
            QVERIFY(output.contains("PRCM_MODE_CONFLICT"));
            QVERIFY(output.contains("prcm.domain.periph.mode.RUN.power"));
        }
        if (fault == "yaml")
            QVERIFY(output.contains("PRCM_YAML"));
    }
};

QStringList Test::messages;

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsoccliparseprcm.moc"
