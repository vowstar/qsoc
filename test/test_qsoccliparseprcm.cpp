// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"
#include "common/qsocconsole.h"
#include "qsoc_prcm_fixture.h"
#include "qsoc_test.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
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

    void generate_data()
    {
        QTest::addColumn<bool>("axi");
        QTest::newRow("apb8") << false;
        QTest::newRow("axi64") << true;
    }

    void generate()
    {
        QFETCH(bool, axi);
        QTemporaryDir directory(QDir::tempPath() + "/test_qsoc_prcm_generate-XXXXXX");
        QVERIFY(directory.isValid());
        QSocProjectManager project;
        project.setCurrentPath(directory.path());
        QVERIFY(project.create("control"));
        auto node                                    = YAML::Load(qsocPrcmDeclaration());
        node["prcm"]["controller"]["reset"]["stage"] = axi ? 3 : 2;
        node["prcm"]["mmio"]["data_width"]           = axi ? 64 : 8;
        node["prcm"]["mmio"]["bus"]                  = axi ? "axi4_lite" : "apb4";
        if (axi)
            node["prcm"]["domain"]["periph"]["mode"]["RUN"]["code"] = quint64(1) << 60;
        const auto path = directory.filePath("controller.soc_net");
        const QDir output(QDir(project.getOutputPath()).filePath("controller"));
        auto       save = [&](const QString &name, const QByteArray &bytes) {
            QFile file(name);
            return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
        };
        auto read = [](const QString &name) {
            QFile file(name);
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        };
        auto run = [&](const QStringList &extra = QStringList{}) {
            messages.clear();
            QSocCliWorker worker;
            QSignalSpy    exitSpy(&worker, &QSocCliWorker::exit);
            QStringList args{"qsoc", "generate", "verilog", "-d", directory.path(), "-p", "control"};
            args.append(extra);
            args.append(path);
            worker.setup(args, false);
            worker.run();
            return exitSpy.size() == 1 ? exitSpy[0][0].toInt() : -1;
        };
        QVERIFY(save(path, QByteArray::fromStdString(YAML::Dump(node))));
        QCOMPARE(run(), 0);
        const auto rtl = read(output.filePath("rtl/controller.v"));
        QVERIFY(rtl.contains(axi ? ".STAGE(3)" : ".STAGE(2)"));
        QVERIFY(rtl.contains(axi ? "s_axi_awvalid" : "s_apb_psel"));
        const auto list = read(output.filePath("rtl/controller.fl")).split('\n');
        QCOMPARE(list.size(), 8);
        for (const auto &leaf : list) {
            if (!leaf.isEmpty()) {
                QVERIFY(!leaf.contains('/') && leaf.endsWith(".v"));
                QVERIFY(QFile::exists(output.filePath("rtl/" + QString::fromUtf8(leaf))));
            }
        }
        const auto header = read(output.filePath("include/controller.h"));
        QVERIFY(
            header.contains(axi ? "STATUS_OFFSET UINT64_C(0x8)" : "STATUS_OFFSET UINT64_C(0x1)"));
        QVERIFY(header.contains(axi ? "EVENT_OFFSET UINT64_C(0x10)" : "EVENT_OFFSET UINT64_C(0x2)"));
        QVERIFY(header.contains(
            axi ? "MODE_RUN UINT64_C(0x1000000000000000)" : "MODE_RUN UINT64_C(0x1)"));
        QVERIFY(header.contains(
            axi ? "STATUS_fault_MASK UINT64_C(0x8000000000000000)"
                : "STATUS_fault_MASK UINT64_C(0x8)"));
        const auto report
            = QJsonDocument::fromJson(read(output.filePath("integration/controller.json"))).object();
        QCOMPARE(report["check"].toObject()["physical"].toString(), "not_run");
        QCOMPARE(report["check"].toObject()["rtl"].toString(), "not_generated");
        QCOMPARE(report["reset"].toObject()["stage"].toInt(), axi ? 3 : 2);
        const auto clockPath = output.filePath("rtl/clock_cell.v");
        const auto clock     = read(clockPath);
        const auto custom    = clock + "\n// Custom cell boundary.\n";
        QVERIFY(save(clockPath, custom));
        QCOMPARE(run(), 0);
        QCOMPARE(read(clockPath), custom);
        QCOMPARE(run({"--force"}), 0);
        QCOMPARE(read(clockPath), clock);
        QCOMPARE(run({"--with-formal"}), 0);
        const auto formalList = read(output.filePath("formal/controller_formal.fl")).split('\n');
        QCOMPARE(formalList.size(), 9);
        for (const auto &file : formalList) {
            if (!file.isEmpty())
                QVERIFY(QFile::exists(output.filePath("formal/" + QString::fromUtf8(file))));
        }
        QVERIFY(read(output.filePath("formal/check.sby")).contains("../rtl/clock_cell.v"));
        QVERIFY(read(output.filePath("formal/controller_formal.sv")).contains("power_off: assert"));
        const auto formalReport
            = QJsonDocument::fromJson(read(output.filePath("integration/controller.json"))).object();
        QCOMPARE(formalReport["check"].toObject()["rtl"].toString(), "not_run");
        QCOMPARE(run(), 0);

        QMap<QString, QByteArray> before;
        QDirIterator              item(output.path(), QDir::Files, QDirIterator::Subdirectories);
        while (item.hasNext()) {
            const auto file = item.next();
            before.insert(file, read(file));
        }
        const QMap<QString, QString> fault{
            {"stage-missing", "PRCM_REQUIRED"},
            {"stage-one", "PRCM_RANGE"},
            {"extra", "instance"},
            {"resource", "one clock and one reset"},
            {"mode", "PRCM_MODE_CONFLICT"},
            {"transition", "PRCM_SEQUENCE_UNSUPPORTED"}};
        for (auto entry = fault.cbegin(); entry != fault.cend(); ++entry) {
            auto changed = YAML::Clone(node);
            if (entry.key() == "stage-missing")
                changed["prcm"]["controller"]["reset"].remove("stage");
            if (entry.key() == "stage-one")
                changed["prcm"]["controller"]["reset"]["stage"] = 1;
            if (entry.key() == "extra")
                changed["instance"]["extra"]["module"] = "other";
            if (entry.key() == "resource") {
                auto extra    = YAML::Clone(changed["clock"][0]);
                extra["name"] = "other";
                changed["clock"].push_back(extra);
            }
            if (entry.key() == "mode")
                changed["prcm"]["domain"]["periph"]["mode"]["RUN"]["power"] = "off";
            if (entry.key() == "transition")
                changed["prcm"]["domain"]["periph"]["transition"] = YAML::Node(
                    YAML::NodeType::Sequence);
            QVERIFY(save(path, QByteArray::fromStdString(YAML::Dump(changed))));
            QCOMPARE(run(), 1);
            QVERIFY2(messages.join('\n').contains(entry.value()), qPrintable(messages.join('\n')));
            for (auto file = before.cbegin(); file != before.cend(); ++file)
                QCOMPARE(read(file.key()), file.value());
        }
        const auto valid = QByteArray::fromStdString(YAML::Dump(node));
        for (const auto &multiple :
             {valid + "\n---\ninstance: {unused: {module: other}}\n",
              QByteArray("instance: {unused: {module: other}}\n---\n") + valid}) {
            QVERIFY(save(path, multiple));
            QCOMPARE(run(), 1);
            QVERIFY(messages.join('\n').contains("PRCM_DOCUMENT"));
            for (auto file = before.cbegin(); file != before.cend(); ++file)
                QCOMPARE(read(file.key()), file.value());
        }
        QVERIFY(save(path, valid + "\n---\ninstance: [\n"));
        QCOMPARE(run(), 1);
        QVERIFY(messages.join('\n').contains("PRCM_YAML"));
        for (auto file = before.cbegin(); file != before.cend(); ++file)
            QCOMPARE(read(file.key()), file.value());
        QVERIFY(save(path, valid));
        QCOMPARE(run(), 0);
        {
            const auto oldPath = qgetenv("PATH");
            const auto restore = qScopeGuard([&] { qputenv("PATH", oldPath); });
            qputenv("PATH", directory.path().toUtf8());
            QCOMPARE(run({"--format"}), 1);
            QVERIFY(messages.join('\n').contains("PRCM_FORMAT"));
            for (auto file = before.cbegin(); file != before.cend(); ++file)
                QCOMPARE(read(file.key()), file.value());
        }
        if (!QStandardPaths::findExecutable("verible-verilog-format").isEmpty())
            QCOMPARE(run({"--format"}), 0);
        QVERIFY(save(path, "instance: {unused: {module: other}}\n"));
        QCOMPARE(run({"--with-formal"}), 1);
        QVERIFY(messages.join('\n').contains("PRCM_REQUIRED"));
    }

    void generateMerged()
    {
        QTemporaryDir directory(QDir::tempPath() + "/test_qsoc_prcm_generate_merge-XXXXXX");
        QVERIFY(directory.isValid());
        QSocProjectManager project;
        project.setCurrentPath(directory.path());
        QVERIFY(project.create("control"));
        auto control                                    = YAML::Load(qsocPrcmDeclaration());
        control["prcm"]["controller"]["reset"]["stage"] = 2;
        YAML::Node clock(YAML::NodeType::Map);
        clock["clock"] = YAML::Clone(control["clock"]);
        control.remove("clock");
        const auto a    = directory.filePath("controller.soc_net");
        const auto b    = directory.filePath("control.soc_net");
        auto       save = [](const QString &path, const YAML::Node &node) {
            QFile      file(path);
            const auto bytes = QByteArray::fromStdString(YAML::Dump(node));
            return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
        };
        auto run = [&] {
            messages.clear();
            QSocCliWorker worker;
            QSignalSpy    exitSpy(&worker, &QSocCliWorker::exit);
            worker.setup(
                {"qsoc",
                 "generate",
                 "verilog",
                 "-d",
                 directory.path(),
                 "-p",
                 "control",
                 "--merge",
                 a,
                 b},
                false);
            worker.run();
            return exitSpy.size() == 1 ? exitSpy[0][0].toInt() : -1;
        };
        QVERIFY(save(a, clock));
        QVERIFY(save(b, control));
        QCOMPARE(run(), 0);
        QVERIFY(
            QFile::exists(QDir(project.getOutputPath()).filePath("controller/rtl/controller.v")));
        auto missing = YAML::Clone(control);
        missing["prcm"]["controller"]["reset"].remove("stage");
        QVERIFY(save(b, missing));
        QCOMPARE(run(), 1);
        QVERIFY(messages.join('\n').contains(b + ":"));
        QVERIFY(messages.join('\n').contains("prcm.controller.reset.stage"));
        control["instance"]["unused"]["module"] = "other";
        QVERIFY(save(b, control));
        QCOMPARE(run(), 1);
        QVERIFY(messages.join('\n').contains("cannot include the instance section"));
        QVERIFY(messages.join('\n').contains(b + ":"));
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
