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
#ifdef ENABLE_TEST_CLEANUP
        /* Clean up the test project directory */
        QDir projectDir(projectManager.getCurrentPath());
        if (projectDir.exists()) {
            projectDir.removeRecursively();
        }
#endif // ENABLE_TEST_CLEANUP
    }

    void init() { messageList.clear(); }

    void testBasicResetController()
    {
        QString netlistContent = R"(
# Test netlist with basic reset controller (ASYNC_COMB type)
port:
  clk_sys:
    direction: input
    type: logic
  por_rst_n:
    direction: input
    type: logic
  cpu_rst_n:
    direction: output
    type: logic
  peri_rst_n:
    direction: output
    type: logic
  test_en:
    direction: input
    type: logic

instance: {}

net: {}

reset:
  - name: basic_reset_ctrl
    clock: clk_sys
    test_enable: test_en
    source:
      por_rst_n: low
    target:
      cpu_rst_n:
        polarity: low
        link:
          por_rst_n:
            type: ASYNC_COMB
      peri_rst_n:
        polarity: low
        link:
          por_rst_n:
            type: ASYNC_COMB
)";

        QString netlistPath = createTempFile("test_basic_reset.soc_net", netlistContent);
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
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_basic_reset.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify reset controller comment */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "/* Reset primitive controllers */"));

        /* Verify basic reset assignments for ASYNC_COMB type (combinational) */
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "assign normalize_por_rst_n_to_cpu_rst_n = por_rst_n"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "assign normalize_por_rst_n_to_peri_rst_n = por_rst_n"));

        /* Verify output assignments */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "assign cpu_rst_n = &"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "assign peri_rst_n = &"));
    }

    void testSyncResetController()
    {
        QString netlistContent = R"(
# Test netlist with sync reset controller (ASYNC_SYNC type)
port:
  clk_sys:
    direction: input
    type: logic
  i3c_soc_rst:
    direction: input
    type: logic
  cpu_rst_n:
    direction: output
    type: logic
  test_en:
    direction: input
    type: logic

instance: {}

net: {}

reset:
  - name: sync_reset_ctrl
    clock: clk_sys
    test_enable: test_en
    source:
      i3c_soc_rst: high
    target:
      cpu_rst_n:
        polarity: low
        link:
          i3c_soc_rst:
            type: ASYNC_SYNC
            sync_depth: 4
)";

        QString netlistPath = createTempFile("test_sync_reset.soc_net", netlistContent);
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
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_sync_reset.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify reset sync module instantiation */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "datapath_reset_sync #("));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".RST_SYNC_LEVEL(4)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".clk(clk_sys)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".rst_n_a(~i3c_soc_rst)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".reset_bypass(test_en)"));
    }

    void testCounterResetController()
    {
        QString netlistContent = R"(
# Test netlist with counter reset controller (ASYNC_CNT type)
port:
  clk_sys:
    direction: input
    type: logic
  por_rst_n:
    direction: input
    type: logic
  cpu_por_rst_n:
    direction: output
    type: logic
  test_en:
    direction: input
    type: logic

instance: {}

net: {}

reset:
  - name: counter_reset_ctrl
    clock: clk_sys
    test_enable: test_en
    source:
      por_rst_n: low
    target:
      cpu_por_rst_n:
        polarity: low
        link:
          por_rst_n:
            type: ASYNC_CNT
            sync_depth: 2
            counter_width: 8
            timeout_cycles: 255
)";

        QString netlistPath = createTempFile("test_counter_reset.soc_net", netlistContent);
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
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_counter_reset.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify reset counter module instantiation */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "datapath_reset_counter #("));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".RST_SYNC_LEVEL(2)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".CNT_WIDTH(8)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".TIMEOUT(255)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".clk(clk_sys)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".rst_n_a(por_rst_n)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".reset_bypass(test_en)"));
    }

    void testMultiSourceMultiTarget()
    {
        QString netlistContent = R"(
# Test netlist with multi-source multi-target reset matrix
port:
  clk_sys:
    direction: input
    type: logic
  por_rst_n:
    direction: input
    type: logic
  i3c_soc_rst:
    direction: input
    type: logic
  trig_cpu_rst:
    direction: input
    type: logic
  cpu_rst_n:
    direction: output
    type: logic
  i3c_rst_n:
    direction: output
    type: logic
  test_en:
    direction: input
    type: logic

instance: {}

net: {}

reset:
  - name: multi_reset_ctrl
    clock: clk_sys
    test_enable: test_en
    source:
      por_rst_n: low
      i3c_soc_rst: high
      trig_cpu_rst: high
    target:
      cpu_rst_n:
        polarity: low
        link:
          por_rst_n:
            type: ASYNC_SYNC
            sync_depth: 4
          i3c_soc_rst:
            type: ASYNC_SYNC
            sync_depth: 4
          trig_cpu_rst:
            type: ASYNC_SYNC
            sync_depth: 4
      i3c_rst_n:
        polarity: low
        link:
          por_rst_n:
            type: ASYNC_COMB
          i3c_soc_rst:
            type: ASYNC_COMB
)";

        QString netlistPath = createTempFile("test_multi_reset.soc_net", netlistContent);
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
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_multi_reset.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify multiple reset sync modules for cpu_rst_n */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "datapath_reset_sync"));

        /* Verify wire declarations for intermediate signals */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "wire"));

        /* Verify AND logic for combining multiple reset sources */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "assign cpu_rst_n = &"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "assign i3c_rst_n = &"));

        /* Verify polarity handling */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "~i3c_soc_rst"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "~trig_cpu_rst"));
    }

    void testSyncOnlyReset()
    {
        QString netlistContent = R"(
# Test netlist with SYNC_ONLY reset controller
port:
  clk_sys:
    direction: input
    type: logic
  sync_rst_n:
    direction: input
    type: logic
  peri_rst_n:
    direction: output
    type: logic
  test_en:
    direction: input
    type: logic

instance: {}

net: {}

reset:
  - name: sync_only_reset_ctrl
    clock: clk_sys
    test_enable: test_en
    source:
      sync_rst_n: low
    target:
      peri_rst_n:
        polarity: low
        link:
          sync_rst_n:
            type: SYNC_ONLY
            sync_depth: 2
)";

        QString netlistPath = createTempFile("test_sync_only_reset.soc_net", netlistContent);
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
        QString verilogPath
            = QDir(projectManager.getOutputPath()).filePath("test_sync_only_reset.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify SYNC_ONLY reset module instantiation */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "datapath_reset_sync #("));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".RST_SYNC_LEVEL(2)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".clk(clk_sys)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".rst_n_a(sync_rst_n)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".reset_bypass(test_en)"));
    }

    void testAsyncPipeReset()
    {
        QString netlistContent = R"(
# Test netlist with ASYNC_PIPE reset controller
port:
  clk_sys:
    direction: input
    type: logic
  trig_rst:
    direction: input
    type: logic
  dma_rst_n:
    direction: output
    type: logic
  test_en:
    direction: input
    type: logic

instance: {}

net: {}

reset:
  - name: pipe_reset_ctrl
    clock: clk_sys
    test_enable: test_en
    source:
      trig_rst: low
    target:
      dma_rst_n:
        polarity: low
        link:
          trig_rst:
            type: ASYNC_PIPE
            sync_depth: 3
            pipe_depth: 4
)";

        QString netlistPath = createTempFile("test_pipe_reset.soc_net", netlistContent);
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
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_pipe_reset.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify ASYNC_PIPE reset module instantiation (simplified as async_sync for now) */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "datapath_reset_sync #("));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".RST_SYNC_LEVEL(3)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".clk(clk_sys)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".rst_n_a(~trig_rst)"));
    }
};

QStringList Test::messageList;

QSOC_TEST_MAIN(Test)

#include "test_qsoccliparsegenerateresetlogic.moc"
