// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"
#include "common/qsocconsole.h"
#include "common/qsocprojectmanager.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QStringList>
#include <QTextStream>
#include <QtCore>
#include <QtTest>

class Test : public QObject
{
    Q_OBJECT

private:
    static QStringList messageList;
    QString            projectName;
    QSocProjectManager projectManager;

    static void messageOutput(QtMsgType type, const QMessageLogContext &context, const QString &msg)
    {
        Q_UNUSED(type);
        Q_UNUSED(context);
        messageList << msg;
    }

    QString createTempFile(const QString &fileName, const QString &content)
    {
        const QString filePath = QDir(projectManager.getOutputPath()).filePath(fileName);
        QFile         file(filePath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&file);
            stream << content;
            file.close();
        }
        return filePath;
    }

    QString readVerilog(const QString &baseFileName)
    {
        const QString filePath = QDir(projectManager.getOutputPath()).filePath(baseFileName + ".v");
        QFile         file(filePath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return {};
        }
        QTextStream   in(&file);
        const QString content = in.readAll();
        file.close();
        return content;
    }

    bool verilogExists(const QString &baseFileName)
    {
        return QFile::exists(QDir(projectManager.getOutputPath()).filePath(baseFileName + ".v"));
    }

    void runGenerate(const QString &netlistPath)
    {
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), netlistPath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();
    }

    void writeBufferModule()
    {
        const QString moduleContent = R"(
buf_cell:
  port:
    a:
      type: logic
      direction: in
    z:
      type: logic
      direction: out
)";
        const QDir    moduleDir(projectManager.getModulePath());
        const QString modulePath = moduleDir.filePath("buf_cell.soc_mod");
        QFile         moduleFile(modulePath);
        if (moduleFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&moduleFile);
            stream << moduleContent;
            moduleFile.close();
        }
    }

private slots:
    void initTestCase()
    {
        qInstallMessageHandler(messageOutput);
        QSocConsole::setTeeToMessageHandler(true);
        projectName = QFileInfo(__FILE__).baseName() + "_data";
        /* Wipe leftovers from earlier runs; a stale .v must never satisfy
           an existence or content assertion. A wipe that cannot finish
           fails the run: reusing old output is exactly the lie this
           guards against. */
        const QString staleDataPath = QDir::current().filePath(projectName);
        QDir          staleDataDir(staleDataPath);
        if (staleDataDir.exists()) {
            QVERIFY2(
                staleDataDir.removeRecursively(),
                qPrintable("could not remove stale data directory: " + staleDataPath));
        }
        QVERIFY2(
            !QDir(staleDataPath).exists(),
            qPrintable("stale data directory survives: " + staleDataPath));
        projectManager.setProjectName(projectName);
        projectManager.setCurrentPath(QDir::current().filePath(projectName));
        projectManager.mkpath();
        projectManager.save(projectName);
        projectManager.load(projectName);
        writeBufferModule();
    }

    void cleanupTestCase()
    {
#ifdef ENABLE_TEST_CLEANUP
        QDir projectDir(projectManager.getCurrentPath());
        if (projectDir.exists()) {
            projectDir.removeRecursively();
        }
#endif
    }

    /* Two drivers gated by `ifdef X` and `ifndef X` of the same macro form
     * disjoint cubes (the canonical FPGA / ASIC tech split). The multi-driver
     * census must NOT refuse them, since at most one driver is active in
     * any single build configuration. */
    void testIfdefIfndefSameMacroIsDisjoint()
    {
        const QString netlist = R"(
port:
  jtag_tck:
    type: logic
    direction: out

instance:
  u_bufg:
    module: buf_cell
    ifdef:
      - TECH_FPGA
  u_buf:
    module: buf_cell
    ifndef:
      - TECH_FPGA

net:
  jtag_tck:
    - instance: u_bufg
      port: z
    - instance: u_buf
      port: z
    - instance: top
      port: jtag_tck
)";
        const QString path    = createTempFile("test_guard_disjoint.soc_net", netlist);
        runGenerate(path);

        const QString verilog = readVerilog("test_guard_disjoint");
        QVERIFY2(!verilog.isEmpty(), "expected generated Verilog file");
        QVERIFY2(!verilog.contains("multiple drivers"), "guard-disjoint drivers must stay legal");
        QVERIFY2(verilog.contains("u_bufg"), "expected ifdef-guarded instance present");
        QVERIFY2(verilog.contains("u_buf"), "expected ifndef-guarded instance present");
    }

    /* Same shape, but no ifdef/ifndef. Both drivers are universally active,
     * so the double driver stands and the netlist is refused. */
    void testNoGuardsRejectsTheDoubleDriver()
    {
        const QString netlist = R"(
port:
  jtag_tck:
    type: logic
    direction: out

instance:
  u_bufg:
    module: buf_cell
  u_buf:
    module: buf_cell

net:
  jtag_tck:
    - instance: u_bufg
      port: z
    - instance: u_buf
      port: z
    - instance: top
      port: jtag_tck
)";
        const QString path    = createTempFile("test_guard_unguarded.soc_net", netlist);
        messageList.clear();
        runGenerate(path);

        QCOMPARE(messageList.filter("takes multiple output drivers").size(), 1);
        QCOMPARE(messageList.filter("Successfully generated Verilog code").size(), 0);
        QVERIFY2(
            !verilogExists("test_guard_unguarded"),
            "a refused netlist must not leave a Verilog file");
    }

    /* Different macros (ifdef X vs ifdef Y). The cubes can both be true when
     * X and Y are simultaneously defined, so the double driver is refused. */
    void testDifferentMacrosRejectTheDoubleDriver()
    {
        const QString netlist = R"(
port:
  jtag_tck:
    type: logic
    direction: out

instance:
  u_bufg:
    module: buf_cell
    ifdef:
      - HAS_FPGA_BUFG
  u_buf:
    module: buf_cell
    ifdef:
      - HAS_ASIC_BUF

net:
  jtag_tck:
    - instance: u_bufg
      port: z
    - instance: u_buf
      port: z
    - instance: top
      port: jtag_tck
)";
        const QString path    = createTempFile("test_guard_different_macros.soc_net", netlist);
        messageList.clear();
        runGenerate(path);

        QCOMPARE(messageList.filter("takes multiple output drivers").size(), 1);
        QCOMPARE(messageList.filter("Successfully generated Verilog code").size(), 0);
        QVERIFY2(
            !verilogExists("test_guard_different_macros"),
            "a refused netlist must not leave a Verilog file");
    }

    /* Multi-literal cube `ifdef [X, Y]` against `ifndef [X]`. The +X / -X
     * collision alone is enough to prove the cubes disjoint; Y is irrelevant.
     */
    void testCompoundCubeWithSinglePolarityCollision()
    {
        const QString netlist = R"(
port:
  jtag_tck:
    type: logic
    direction: out

instance:
  u_bufg:
    module: buf_cell
    ifdef:
      - TECH_FPGA
      - SOME_FPGA_FLAG
  u_buf:
    module: buf_cell
    ifndef:
      - TECH_FPGA

net:
  jtag_tck:
    - instance: u_bufg
      port: z
    - instance: u_buf
      port: z
    - instance: top
      port: jtag_tck
)";
        const QString path    = createTempFile("test_guard_compound.soc_net", netlist);
        runGenerate(path);

        const QString verilog = readVerilog("test_guard_compound");
        QVERIFY2(!verilog.isEmpty(), "expected generated Verilog file");
        QVERIFY2(
            !verilog.contains("multiple drivers"),
            "compound cube with polarity collision must be recognised as disjoint");
    }

    /* Direct API check on the cube-disjointness predicate. */
    void testGuardsAreDisjointPredicate()
    {
        using PortDetailInfo = QSocGenerateManager::PortDetailInfo;

        PortDetailInfo lhs
            = PortDetailInfo::createModulePort("u_a", "z", "logic", "output", "", {"TECH_FPGA"}, {});
        PortDetailInfo rhs
            = PortDetailInfo::createModulePort("u_b", "z", "logic", "output", "", {}, {"TECH_FPGA"});
        QVERIFY(QSocGenerateManager::guardsAreDisjoint(lhs, rhs));

        PortDetailInfo same
            = PortDetailInfo::createModulePort("u_c", "z", "logic", "output", "", {"TECH_FPGA"}, {});
        QVERIFY(!QSocGenerateManager::guardsAreDisjoint(lhs, same));

        PortDetailInfo empty = PortDetailInfo::createModulePort("u_d", "z", "logic", "output");
        QVERIFY(!QSocGenerateManager::guardsAreDisjoint(lhs, empty));
        QVERIFY(!QSocGenerateManager::guardsAreDisjoint(empty, empty));
    }
};

QStringList Test::messageList;

QSOC_TEST_MAIN(Test)

#include "test_qsoccliparsegeneratemultidriverguard.moc"
