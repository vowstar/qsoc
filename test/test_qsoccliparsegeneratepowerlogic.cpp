// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"
#include "common/config.h"
#include "common/qsocprojectmanager.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QStringList>
#include <QTemporaryFile>
#include <QTextStream>
#include <QtCore>
#include <QtTest>

struct TestApp
{
    static auto &instance()
    {
        static auto                  argc      = 1;
        static char                  appName[] = "qsoc";
        static std::array<char *, 1> argv      = {{appName}};
        /* Use QCoreApplication for cli test */
        static const QCoreApplication app = QCoreApplication(argc, argv.data());
        return app;
    }
};

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
        QString filePath = QDir(projectManager.getCurrentPath()).filePath(fileName);
        QFile   file(filePath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&file);
            stream << content;
            file.close();
            return filePath;
        }
        return QString();
    }

    void createTestModuleFiles()
    {
        /* Create module directory if it doesn't exist */
        QDir moduleDir(projectManager.getModulePath());
        if (!moduleDir.exists()) {
            moduleDir.mkpath(".");
        }
    }

    /* Helper function to verify Verilog content with normalized whitespace */
    bool verifyVerilogContentNormalized(const QString &verilogContent, const QString &contentToVerify)
    {
        if (verilogContent.isEmpty() || contentToVerify.isEmpty()) {
            return false;
        }

        /* Helper function to normalize whitespace */
        auto normalizeWhitespace = [](const QString &input) -> QString {
            QString result = input;
            /* Replace all whitespace (including tabs and newlines) with a single space */
            result.replace(QRegularExpression("\\s+"), " ");
            /* Remove whitespace before any symbol/operator/punctuation */
            result.replace(
                QRegularExpression("\\s+([\\[\\]\\(\\)\\{\\}<>\"'`+\\-*/%&|^~!#$,.:;=@_])"), "\\1");
            /* Remove whitespace after any symbol/operator/punctuation */
            result.replace(
                QRegularExpression("([\\[\\]\\(\\)\\{\\}<>\"'`+\\-*/%&|^~!#$,.:;=@_])\\s+"), "\\1");

            return result;
        };

        /* Normalize whitespace in both strings before comparing */
        const QString normalizedContent = normalizeWhitespace(verilogContent);
        const QString normalizedVerify  = normalizeWhitespace(contentToVerify);

        /* Check if the normalized content contains the normalized text we're looking for */
        return normalizedContent.contains(normalizedVerify);
    }

    bool verifyPowerCellFileComplete()
    {
        const QString powerCellPath = QDir(projectManager.getOutputPath()).filePath("power_cell.v");
        if (!QFile::exists(powerCellPath)) {
            qWarning() << "power_cell.v not found at" << powerCellPath;
            return false;
        }

        QFile file(powerCellPath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qWarning() << "Failed to open power_cell.v:" << file.errorString();
            return false;
        }
        const QString content = file.readAll();
        file.close();

        /* Check for qsoc_power_fsm module */
        if (!content.contains("module qsoc_power_fsm")) {
            qWarning() << "Missing module in power_cell.v: qsoc_power_fsm";
            return false;
        }

        /* Check for qsoc_rst_pipe module */
        if (!content.contains("module qsoc_rst_pipe")) {
            qWarning() << "Missing module in power_cell.v: qsoc_rst_pipe";
            return false;
        }

        return true;
    }

private slots:
    void initTestCase()
    {
        TestApp::instance();
        qInstallMessageHandler(messageOutput);
        projectName = QFileInfo(__FILE__).baseName() + "_data";
        projectManager.setProjectName(projectName);
        projectManager.setCurrentPath(QDir::current().filePath(projectName));
        projectManager.mkpath();
        projectManager.save(projectName);
        projectManager.load(projectName);
        createTestModuleFiles();
    }

    void cleanupTestCase()
    {
        // Clean up temporary files
        QDir projectDir(projectManager.getCurrentPath());
        projectDir.removeRecursively();
        qInstallMessageHandler(nullptr);
    }

    void test_ao_domain_inference()
    {
        /* Test AO domain inference: no depend key -> AO domain */
        QString netlistContent = R"(
port:
  clk_ao:
    direction: input
    type: logic
  rst_ao:
    direction: input
    type: logic
  pgood_ao:
    direction: input
    type: logic
  icg_en_ao:
    direction: output
    type: logic
  rst_allow_ao:
    direction: output
    type: logic
  rdy_ao:
    direction: output
    type: logic
  flt_ao:
    direction: output
    type: logic

instance: {}

net: {}

power:
  - name: pwr0
    host_clock: clk_ao
    host_reset: rst_ao
    domain:
      - name: ao
        v_mv: 900
        pgood: pgood_ao
        wait_dep: 0
        settle_on: 0
        settle_off: 0
        follow:
          clock: []
          reset: []
)";

        QString netlistPath = createTempFile("test_ao_domain.soc_net", netlistContent);
        QVERIFY(!netlistPath.isEmpty());

        {
            QSocCliWorker socCliWorker;
            QStringList   args;
            args << "qsoc" << "generate" << "verilog" << "-d" << projectManager.getCurrentPath()
                 << netlistPath;

            socCliWorker.setup(args, false);
            socCliWorker.run();
        }

        /* Check if Verilog file was generated */
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_ao_domain.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify AO domain: HAS_SWITCH(0), ctrl_enable(1'b1) */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".HAS_SWITCH (0)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".ctrl_enable (1'b1)"));
        QVERIFY(
            verifyVerilogContentNormalized(verilogContent, "/* ao: AO domain (no depend key) */"));

        /* Verify power_cell.v was generated */
        QVERIFY(verifyPowerCellFileComplete());
    }

    void test_root_domain_inference()
    {
        /* Test root domain inference: depend: [] -> root domain */
        QString netlistContent = R"(
port:
  clk_ao:
    direction: input
    type: logic
  rst_ao:
    direction: input
    type: logic
  pgood_vmem:
    direction: input
    type: logic
  en_vmem:
    direction: input
    type: logic
  clr_vmem:
    direction: input
    type: logic
  icg_en_vmem:
    direction: output
    type: logic
  rst_allow_vmem:
    direction: output
    type: logic
  sw_vmem:
    direction: output
    type: logic
  rdy_vmem:
    direction: output
    type: logic
  flt_vmem:
    direction: output
    type: logic

instance: {}

net: {}

power:
  - name: pwr0
    host_clock: clk_ao
    host_reset: rst_ao
    domain:
      - name: vmem
        depend: []
        v_mv: 1100
        pgood: pgood_vmem
        wait_dep: 50
        settle_on: 100
        settle_off: 50
        follow:
          clock: []
          reset: []
)";

        QString netlistPath = createTempFile("test_root_domain.soc_net", netlistContent);
        QVERIFY(!netlistPath.isEmpty());

        {
            QSocCliWorker socCliWorker;
            QStringList   args;
            args << "qsoc" << "generate" << "verilog" << "-d" << projectManager.getCurrentPath()
                 << netlistPath;

            socCliWorker.setup(args, false);
            socCliWorker.run();
        }

        /* Check if Verilog file was generated */
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_root_domain.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify root domain: HAS_SWITCH(1), has enable/clear controls */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".HAS_SWITCH (1)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".ctrl_enable (en_vmem)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".fault_clear (clr_vmem)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".pwr_switch (sw_vmem)"));
        QVERIFY(
            verifyVerilogContentNormalized(verilogContent, "/* vmem: Root domain (depend: []) */"));
    }

    void test_hard_dependency()
    {
        /* Test hard dependency: must be ready within wait_dep */
        QString netlistContent = R"(
port:
  clk_ao:
    direction: input
    type: logic
  rst_ao:
    direction: input
    type: logic
  pgood_ao:
    direction: input
    type: logic
  pgood_cpu:
    direction: input
    type: logic
  en_cpu:
    direction: input
    type: logic
  clr_cpu:
    direction: input
    type: logic
  icg_en_ao:
    direction: output
    type: logic
  rst_allow_ao:
    direction: output
    type: logic
  icg_en_cpu:
    direction: output
    type: logic
  rst_allow_cpu:
    direction: output
    type: logic
  sw_cpu:
    direction: output
    type: logic
  rdy_ao:
    direction: output
    type: logic
  flt_ao:
    direction: output
    type: logic
  rdy_cpu:
    direction: output
    type: logic
  flt_cpu:
    direction: output
    type: logic

instance: {}

net: {}

power:
  - name: pwr0
    host_clock: clk_ao
    host_reset: rst_ao
    domain:
      - name: ao
        v_mv: 900
        pgood: pgood_ao
        wait_dep: 0
        settle_on: 0
        settle_off: 0
        follow:
          clock: []
          reset: []
      - name: cpu
        depend:
          - name: ao
            type: hard
        v_mv: 900
        pgood: pgood_cpu
        wait_dep: 200
        settle_on: 120
        settle_off: 80
        follow:
          clock: []
          reset: []
)";

        QString netlistPath = createTempFile("test_hard_dep.soc_net", netlistContent);
        QVERIFY(!netlistPath.isEmpty());

        {
            QSocCliWorker socCliWorker;
            QStringList   args;
            args << "qsoc" << "generate" << "verilog" << "-d" << projectManager.getCurrentPath()
                 << netlistPath;

            socCliWorker.setup(args, false);
            socCliWorker.run();
        }

        /* Check if Verilog file was generated */
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_hard_dep.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify hard dependency aggregation */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "wire dep_hard_all_cpu = rdy_ao;"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".dep_hard_all (dep_hard_all_cpu)"));
    }

    void test_soft_dependency()
    {
        /* Test soft dependency: timeout sets fault but proceeds */
        QString netlistContent = R"(
port:
  clk_ao:
    direction: input
    type: logic
  rst_ao:
    direction: input
    type: logic
  pgood_ao:
    direction: input
    type: logic
  pgood_vmem:
    direction: input
    type: logic
  pgood_gpu:
    direction: input
    type: logic
  en_vmem:
    direction: input
    type: logic
  clr_vmem:
    direction: input
    type: logic
  en_gpu:
    direction: input
    type: logic
  clr_gpu:
    direction: input
    type: logic
  icg_en_ao:
    direction: output
    type: logic
  rst_allow_ao:
    direction: output
    type: logic
  icg_en_vmem:
    direction: output
    type: logic
  rst_allow_vmem:
    direction: output
    type: logic
  icg_en_gpu:
    direction: output
    type: logic
  rst_allow_gpu:
    direction: output
    type: logic
  sw_vmem:
    direction: output
    type: logic
  sw_gpu:
    direction: output
    type: logic
  rdy_ao:
    direction: output
    type: logic
  flt_ao:
    direction: output
    type: logic
  rdy_vmem:
    direction: output
    type: logic
  flt_vmem:
    direction: output
    type: logic
  rdy_gpu:
    direction: output
    type: logic
  flt_gpu:
    direction: output
    type: logic

instance: {}

net: {}

power:
  - name: pwr0
    host_clock: clk_ao
    host_reset: rst_ao
    domain:
      - name: ao
        v_mv: 900
        pgood: pgood_ao
        wait_dep: 0
        settle_on: 0
        settle_off: 0
        follow:
          clock: []
          reset: []
      - name: vmem
        depend: []
        v_mv: 1100
        pgood: pgood_vmem
        wait_dep: 50
        settle_on: 100
        settle_off: 50
        follow:
          clock: []
          reset: []
      - name: gpu
        depend:
          - name: ao
            type: hard
          - name: vmem
            type: soft
        v_mv: 900
        pgood: pgood_gpu
        wait_dep: 200
        settle_on: 120
        settle_off: 80
        follow:
          clock: []
          reset: []
)";

        QString netlistPath = createTempFile("test_soft_dep.soc_net", netlistContent);
        QVERIFY(!netlistPath.isEmpty());

        {
            QSocCliWorker socCliWorker;
            QStringList   args;
            args << "qsoc" << "generate" << "verilog" << "-d" << projectManager.getCurrentPath()
                 << netlistPath;

            socCliWorker.setup(args, false);
            socCliWorker.run();
        }

        /* Check if Verilog file was generated */
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_soft_dep.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify mixed hard/soft dependency aggregation */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "wire dep_hard_all_gpu = rdy_ao;"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "wire dep_soft_all_gpu = rdy_vmem;"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".dep_hard_all (dep_hard_all_gpu)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".dep_soft_all (dep_soft_all_gpu)"));
    }

    void test_pgood_signal_connection()
    {
        /* Test pgood signal naming: pgood_* format */
        QString netlistContent = R"(
port:
  clk_ao:
    direction: input
    type: logic
  rst_ao:
    direction: input
    type: logic
  pgood_ao:
    direction: input
    type: logic
  icg_en_ao:
    direction: output
    type: logic
  rst_allow_ao:
    direction: output
    type: logic
  rdy_ao:
    direction: output
    type: logic
  flt_ao:
    direction: output
    type: logic

instance: {}

net: {}

power:
  - name: pwr0
    host_clock: clk_ao
    host_reset: rst_ao
    domain:
      - name: ao
        v_mv: 900
        pgood: pgood_ao
        wait_dep: 0
        settle_on: 0
        settle_off: 0
        follow:
          clock: []
          reset: []
)";

        QString netlistPath = createTempFile("test_pgood_signal.soc_net", netlistContent);
        QVERIFY(!netlistPath.isEmpty());

        {
            QSocCliWorker socCliWorker;
            QStringList   args;
            args << "qsoc" << "generate" << "verilog" << "-d" << projectManager.getCurrentPath()
                 << netlistPath;

            socCliWorker.setup(args, false);
            socCliWorker.run();
        }

        /* Check if Verilog file was generated */
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_pgood_signal.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify pgood_* signal format in module header */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "input wire pgood_ao"));
        /* Verify pgood connection */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".pgood (pgood_ao)"));
    }

    void test_icg_enable_export()
    {
        /* Test ICG enable signal export: icg_en_* */
        QString netlistContent = R"(
port:
  clk_ao:
    direction: input
    type: logic
  rst_ao:
    direction: input
    type: logic
  pgood_ao:
    direction: input
    type: logic
  icg_en_ao:
    direction: output
    type: logic
  rst_allow_ao:
    direction: output
    type: logic
  rdy_ao:
    direction: output
    type: logic
  flt_ao:
    direction: output
    type: logic

instance: {}

net: {}

power:
  - name: pwr0
    host_clock: clk_ao
    host_reset: rst_ao
    domain:
      - name: ao
        v_mv: 900
        pgood: pgood_ao
        wait_dep: 0
        settle_on: 0
        settle_off: 0
        follow:
          clock: []
          reset: []
)";

        QString netlistPath = createTempFile("test_icg_enable.soc_net", netlistContent);
        QVERIFY(!netlistPath.isEmpty());

        {
            QSocCliWorker socCliWorker;
            QStringList   args;
            args << "qsoc" << "generate" << "verilog" << "-d" << projectManager.getCurrentPath()
                 << netlistPath;

            socCliWorker.setup(args, false);
            socCliWorker.run();
        }

        /* Check if Verilog file was generated */
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_icg_enable.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify ICG enable export (not instantiation) */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "output wire icg_en_ao"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".clk_enable (icg_en_ao)"));
        /* Verify no ICG instantiation */
        QVERIFY(!verilogContent.contains("qsoc_tc_clk_gate"));
    }
};

QStringList Test::messageList;

QSOC_TEST_MAIN(Test)
#include "test_qsoccliparsegeneratepowerlogic.moc"
