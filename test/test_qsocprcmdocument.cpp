// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocprcmbinding.h"
#include "common/qsocprcmdocument.h"
#include "qsoc_prcm_fixture.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

namespace {

void write(const QString &path, const QByteArray &data)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(data), data.size());
}

void write(const QString &path, const YAML::Node &data)
{
    write(path, QByteArray::fromStdString(YAML::Dump(data)));
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void resourceOrigin()
    {
        QTemporaryDir directory(QDir::tempPath() + "/test_qsoc_prcm_document-XXXXXX");
        QVERIFY(directory.isValid());
        const auto unused  = directory.filePath("unused.soc_net");
        const auto clock   = directory.filePath("clock.soc_net");
        const auto control = directory.filePath("control.soc_net");
        write(
            unused,
            QByteArray("clock:\n  - name: unused\n    input: {other_clk: {}}\n    target: {}\n"));
        const QByteArray clockText(
            "clock:\n  - name: clock\n    input: {aon_clk: {}}\n    target:\n      periph_clk:\n   "
            "     icg: {enable: gate_en, reset: por_n}\n        link: {aon_clk: {}}\n");
        write(clock, clockText);
        auto root = YAML::Load(qsocPrcmDeclaration());
        root.remove("clock");
        write(control, root);
        const auto document = QSocPrcmDocumentLoader::load({unused, clock, control});
        QVERIFY(document.document);
        QCOMPARE(document.document->node["clock"].size(), std::size_t(2));
        const auto result = QSocPrcmBinding::resolve(
            document.document->node, document.document->file, document.document->origin);
        QVERIFY2(
            result.plan.has_value(),
            result.diagnostic.isEmpty() ? "No plan" : qPrintable(result.diagnostic[0].message));
        const auto source = result.plan->input.source.value("clock[1].target.periph_clk.icg.enable");
        QCOMPARE(source.file, clock);
        QCOMPARE(source.path, "clock[0].target.periph_clk.icg.enable");
        QCOMPARE(source.line, 6);
        QCOMPARE(source.column, 23);
        QCOMPARE(result.plan->input.source.value("prcm.domain.periph.clock.stage").file, control);
        auto changed = clockText;
        changed.replace("gate_en", "aon_clk");
        write(clock, changed);
        const auto bad = QSocPrcmDocumentLoader::load({unused, clock, control});
        QVERIFY(bad.document);
        const auto conflict
            = QSocPrcmBinding::resolve(bad.document->node, bad.document->file, bad.document->origin);
        QVERIFY(!conflict.plan);
        QCOMPARE(conflict.diagnostic[0].code, "PRCM_RESOURCE_CONFLICT");
        QCOMPARE(conflict.diagnostic[0].source[0].file, clock);
        QCOMPARE(conflict.diagnostic[0].source[0].path, "clock[0].target.periph_clk.icg.enable");
        QCOMPARE(conflict.diagnostic[0].source[0].line, 6);
        QVERIFY(conflict.diagnostic[0].source[1].line > 0);
    }

    void disjointTable()
    {
        QTemporaryDir directory(QDir::tempPath() + "/test_qsoc_prcm_table-XXXXXX");
        QVERIFY(directory.isValid());
        const auto a = directory.filePath("a.soc_net");
        const auto b = directory.filePath("b.soc_net");
        write(a, QByteArray("instance: {timer_a: {module: timer}}\n"));
        write(b, QByteArray("instance: {timer_b: {module: gpio}}\n"));
        const auto result = QSocPrcmDocumentLoader::load({a, b});
        QVERIFY(result.document);
        const auto table = result.document->node["instance"];
        QCOMPARE(table.size(), std::size_t(2));
        QCOMPARE(table["timer_a"]["module"].as<std::string>(), std::string("timer"));
        QCOMPARE(table["timer_b"]["module"].as<std::string>(), std::string("gpio"));
    }

    void duplicate_data()
    {
        QTest::addColumn<QByteArray>("first");
        QTest::addColumn<QByteArray>("second");
        QTest::addColumn<int>("secondLine");
        QTest::newRow("manager") << QByteArray("prcm: {}\n") << QByteArray("\nprcm: {}\n") << 2;
        QTest::newRow("resource") << QByteArray("clock: [{name: clock}]\n")
                                  << QByteArray("clock:\n  - name: clock\n") << 2;
        QTest::newRow("instance") << QByteArray("instance: {timer: {module: timer}}\n")
                                  << QByteArray("instance:\n  timer: {module: other}\n") << 2;
    }

    void duplicate()
    {
        QFETCH(QByteArray, first);
        QFETCH(QByteArray, second);
        QFETCH(int, secondLine);
        QTemporaryDir directory(QDir::tempPath() + "/test_qsoc_prcm_duplicate-XXXXXX");
        QVERIFY(directory.isValid());
        const auto a = directory.filePath("a.soc_net");
        const auto b = directory.filePath("b.soc_net");
        write(a, first);
        write(b, second);
        const auto result = QSocPrcmDocumentLoader::load({a, b});
        QVERIFY(!result.document);
        QCOMPARE(result.diagnostic[0].code, "PRCM_DUPLICATE");
        QCOMPARE(result.diagnostic[0].source.size(), 2);
        QCOMPARE(result.diagnostic[0].source[0].file, a);
        QCOMPARE(result.diagnostic[0].source[1].file, b);
        QCOMPARE(result.diagnostic[0].source[1].line, secondLine);
    }

    void tree_data()
    {
        QTest::addColumn<QByteArray>("text");
        QTest::addColumn<QString>("code");
        QTest::newRow("nested-duplicate") << QByteArray(
            "clock:\n - name: clock\n   target:\n     core: {icg: {enable: a, enable: b}}\n")
                                          << QString("PRCM_DUPLICATE");
        QTest::newRow("alias-cycle")
            << QByteArray("instance: &loop {child: *loop}\n") << QString("PRCM_ALIAS_CYCLE");
        QTest::newRow("shared-alias")
            << QByteArray("instance: {a: &base {module: timer}, b: *base}\n") << QString();
        QTest::newRow("shape") << QByteArray("clock: {}\n") << QString("PRCM_TYPE");
        QTest::newRow("yaml") << QByteArray("clock: [\n") << QString("PRCM_YAML");
    }

    void tree()
    {
        QFETCH(QByteArray, text);
        QFETCH(QString, code);
        QTemporaryDir directory(QDir::tempPath() + "/test_qsoc_prcm_tree-XXXXXX");
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("tree.soc_net");
        write(path, text);
        const auto result = QSocPrcmDocumentLoader::load({path});
        QCOMPARE(result.document.has_value(), code.isEmpty());
        if (!code.isEmpty()) {
            QCOMPARE(result.diagnostic[0].code, code);
            QCOMPARE(result.diagnostic[0].source[0].file, path);
        }
        if (code == "PRCM_DUPLICATE") {
            QCOMPARE(result.diagnostic[0].source.size(), 2);
            QCOMPARE(result.diagnostic[0].source[0].line, 4);
            QCOMPARE(result.diagnostic[0].source[1].line, 4);
            QVERIFY(result.diagnostic[0].source[0].column < result.diagnostic[0].source[1].column);
        }
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocprcmdocument.moc"
