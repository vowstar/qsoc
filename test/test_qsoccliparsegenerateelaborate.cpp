// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"
#include "common/qslangdriver.h"
#include "common/qsocprojectmanager.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QtCore>
#include <QtTest>

/**
 * @brief Elaboration tests for generated RTL.
 * @details The other generator tests match strings in the output, which lets
 *          illegal Verilog pass unnoticed: a packed array port once produced a
 *          two-bit wire, and a single-stage synchronizer once produced an
 *          sr[-1:0] part select. Feeding the output back through slang turns
 *          both classes of defect into a failing test.
 */
class Test : public QObject
{
    Q_OBJECT

private:
    QSocProjectManager projectManager;
    QString            projectName;

    QString writeFile(const QString &directory, const QString &fileName, const QString &content)
    {
        const QString filePath = QDir(directory).filePath(fileName);
        QFile         file(filePath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            return {};
        }
        QTextStream stream(&file);
        stream << content;
        file.close();
        return filePath;
    }

    /**
     * @brief Elaborate generated files as one design.
     * @details The cells carry parameters the design overrides on
     *          instantiation, so a defect that only appears under an override
     *          is invisible unless everything elaborates together.
     * @param[in] fileNames Files inside the project output directory.
     * @return true when slang accepted the design.
     */
    bool elaborates(const QStringList &fileNames)
    {
        QStringList paths;
        for (const QString &fileName : fileNames) {
            const QString filePath = QDir(projectManager.getOutputPath()).filePath(fileName);
            if (!QFile::exists(filePath)) {
                qWarning() << "generated file missing:" << filePath;
                return false;
            }
            paths << filePath;
        }
        QSlangDriver driver;
        return driver.parseArgs(
            QString("slang --single-unit --ignore-unknown-modules %1").arg(paths.join(' ')));
    }

    bool generate(const QString &netlistFileName)
    {
        const QString filePath = QDir(projectManager.getOutputPath()).filePath(netlistFileName);
        QSocCliWorker socCliWorker;
        socCliWorker.setup(
            {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), filePath}, false);
        socCliWorker.run();
        return true;
    }

private slots:
    void initTestCase()
    {
        projectName = QFileInfo(__FILE__).baseName() + "_data";
        projectManager.setProjectName(projectName);
        projectManager.setCurrentPath(QDir::current().filePath(projectName));
        projectManager.mkpath();
        projectManager.save(projectName);
        projectManager.load(projectName);
    }

    void cleanupTestCase() { QDir(projectManager.getCurrentPath()).removeRecursively(); }

    void resetControllerElaborates()
    {
        const QString netlist = R"(
---
version: "1.0"
module: "elab_reset"
instance: {}
reset:
  - name: rst_ctrl
    clock: clk_sys
    test_enable: test_en
    source:
      por_n:
        active: low
    target:
      cpu_rst_n:
        active: low
        link:
          por_n:
            async:
              clock: clk_sys
              stage: 3
)";
        QVERIFY(!writeFile(projectManager.getOutputPath(), "elab_reset.soc_net", netlist).isEmpty());
        QVERIFY(generate("elab_reset.soc_net"));
        QVERIFY(elaborates({"reset_cell.v", "elab_reset.v"}));
    }

    void clockControllerElaborates()
    {
        const QString netlist = R"(
---
version: "1.0"
module: "elab_clock"
instance: {}
clock:
  - name: clk_ctrl
    test_enable: test_en
    input:
      osc_24m:
        freq: 24MHz
    target:
      cpu_clk:
        freq: 800MHz
        div:
          default: 3
          width: 4
        link:
          osc_24m: ~
)";
        QVERIFY(!writeFile(projectManager.getOutputPath(), "elab_clock.soc_net", netlist).isEmpty());
        QVERIFY(generate("elab_clock.soc_net"));
        QVERIFY(elaborates({"clock_cell.v", "elab_clock.v"}));
    }

    void powerControllerElaborates()
    {
        const QString netlist = R"(
---
version: "1.0"
module: "elab_power"
instance: {}
power:
  - name: pwr_ctrl
    host_clock: ao_clk
    host_reset: ao_rst_n
    domain:
      - name: gpu
        depend: []
        pgood: pgood_gpu
        wait_dep: 4
        settle_on: 8
        settle_off: 8
        follow:
          - clock: clk_gpu
            reset: rst_gpu_n
            stage: 1
)";
        QVERIFY(!writeFile(projectManager.getOutputPath(), "elab_power.soc_net", netlist).isEmpty());
        QVERIFY(generate("elab_power.soc_net"));
        QVERIFY(elaborates({"power_cell.v", "elab_power.v"}));
    }

    void packedArrayPortsElaborate()
    {
        const QString moduleContent = R"(
elab_packed_src:
  port:
    pdata:
      type: logic[1:0][3:0]
      direction: out
elab_packed_sink:
  port:
    pdata:
      type: logic[1:0][3:0]
      direction: in
)";
        QVERIFY(!writeFile(projectManager.getModulePath(), "elab_packed.soc_mod", moduleContent)
                     .isEmpty());

        const QString netlist = R"(
---
version: "1.0"
module: "elab_packed"
instance:
  u_src:
    module: elab_packed_src
  u_sink:
    module: elab_packed_sink
net:
  p_bus:
    - instance: u_src
      port: pdata
    - instance: u_sink
      port: pdata
)";
        QVERIFY(
            !writeFile(projectManager.getOutputPath(), "elab_packed.soc_net", netlist).isEmpty());
        QVERIFY(generate("elab_packed.soc_net"));
        QVERIFY(elaborates({"elab_packed.v"}));
    }
};

QSOC_TEST_MAIN(Test)
#include "test_qsoccliparsegenerateelaborate.moc"
