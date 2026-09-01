// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"
#include "common/qsocconsole.h"
#include "common/qsocprojectmanager.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QLockFile>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest>

namespace {

const QString validModule = R"(timer_ctrl:
  generator:
    kind: mmio
    bus: axi4_lite
    register:
      identification:
        offset: 0x00
        field:
          device_id:
            lsb: 0
            width: 8
            access: ro
            value: 0x2a
      control:
        offset: 0x04
        field:
          enable:
            lsb: 0
            access: rw
            reset: 0
            output: enable_o
      status:
        offset: 0x08
        field:
          busy:
            lsb: 0
            access: ro
            input: busy_i
)";

const QString invalidModule = R"(timer_ctrl:
  generator:
    kind: mmio
    bus: axi4_lite
    register:
      control:
        offset: 0x04
        field:
          enable:
            lsb: 0
            access: rw
)";

const QString ordinaryModule = R"(timer_ctrl:
  port:
    status_i:
      direction: input
      type: logic
)";

struct CommandResult
{
    int     exitCode = -1;
    QString output;
};

class Test : public QObject
{
    Q_OBJECT

private:
    static QStringList      messages;
    static QtMessageHandler previousMessageHandler;

    static void messageOutput(QtMsgType type, const QMessageLogContext &context, const QString &text)
    {
        Q_UNUSED(type);
        Q_UNUSED(context);
        messages.append(text);
    }

    static void writeTextFile(const QString &path, const QString &text)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream(&file) << text;
    }

    static void createProject(const QTemporaryDir &directory)
    {
        QVERIFY(directory.isValid());
        QSocProjectManager projectManager;
        projectManager.setCurrentPath(directory.path());
        QVERIFY(projectManager.create("mmio_project"));
    }

    static QStringList projectOptions(const QTemporaryDir &directory)
    {
        return {"-d", directory.path(), "-p", "mmio_project"};
    }

    static QStringList uvmArtifactPaths(const QString &outputDirectory)
    {
        return {
            QDir(outputDirectory).filePath("timer_ctrl_uvm_if.sv"),
            QDir(outputDirectory).filePath("timer_ctrl_uvm_pkg.sv"),
            QDir(outputDirectory).filePath("timer_ctrl_uvm_tb.sv"),
            QDir(outputDirectory).filePath("timer_ctrl_uvm.f"),
        };
    }

    static QStringList formalAndUvmArtifactPaths(const QString &outputDirectory)
    {
        QStringList paths = {
            QDir(outputDirectory).filePath("timer_ctrl.v"),
            QDir(outputDirectory).filePath("timer_ctrl_formal.sv"),
            QDir(outputDirectory).filePath("timer_ctrl_formal.sby"),
        };
        paths.append(uvmArtifactPaths(outputDirectory));
        return paths;
    }

    static CommandResult runCommand(const QStringList &arguments)
    {
        messages.clear();
        QSocCliWorker worker;
        QSignalSpy    exitSpy(&worker, &QSocCliWorker::exit);
        worker.setup(arguments, false);
        worker.run();

        CommandResult result;
        if (exitSpy.count() == 1) {
            result.exitCode = exitSpy.takeFirst().first().toInt();
        }
        result.output = messages.join('\n');
        return result;
    }

private slots:
    void initTestCase();
    void cleanupTestCase();
    void createWritesAnIncompleteDraftWithoutOverwrite();
    void createRefusesLockedLibraryWithoutChangingContent();
    void createReportsLibraryLockPermissionError();
    void missingProjectDoesNotUseCurrentDirectory();
    void validateReportsSuccessAndGeneratorErrors();
    void malformedGeneratorKindReportsPath();
    void validateAndGenerateRejectOrdinaryModule();
    void generateUsesNestedPathAndRequiresForceToOverwrite();
    void generateWithFormalWritesAndReplacesCollateral();
    void generateWithUvmWritesAndReplacesCollateral();
    void formalConflictLeavesAllArtifactsUntouched();
    void formalLockLeavesAllArtifactsUntouched();
    void formalAndUvmConflictLeavesAllArtifactsUntouched();
    void formalAndUvmLockLeavesAllArtifactsUntouched();
    void generateRefusesLockedOutputWithoutChangingContent();
    void invalidGeneratorDoesNotReplaceOutput();
};

QStringList      Test::messages;
QtMessageHandler Test::previousMessageHandler = nullptr;

void Test::initTestCase()
{
    previousMessageHandler = qInstallMessageHandler(messageOutput);
    QSocConsole::setTeeToMessageHandler(true);
}

void Test::cleanupTestCase()
{
    QSocConsole::setTeeToMessageHandler(false);
    qInstallMessageHandler(previousMessageHandler);
}

void Test::createWritesAnIncompleteDraftWithoutOverwrite()
{
    QTemporaryDir directory;
    createProject(directory);

    QStringList arguments = {"qsoc", "module", "create", "--generator", "mmio", "-l", "peripheral"};
    arguments.append(projectOptions(directory));
    arguments.append("timer_ctrl");

    const CommandResult created = runCommand(arguments);
    QCOMPARE(created.exitCode, 0);
    QVERIFY2(created.output.contains("Created MMIO module draft"), qPrintable(created.output));

    const QString modulePath = QDir(directory.path()).filePath("module/peripheral.soc_mod");
    QFile         moduleFile(modulePath);
    QVERIFY(moduleFile.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray original = moduleFile.readAll();
    moduleFile.close();

    const YAML::Node library = YAML::Load(std::string(original.constData(), original.size()));
    QCOMPARE(
        QString::fromStdString(library["timer_ctrl"]["generator"]["kind"].as<std::string>()),
        "mmio");
    QCOMPARE(
        QString::fromStdString(library["timer_ctrl"]["generator"]["bus"].as<std::string>()),
        "axi4_lite");
    QVERIFY(library["timer_ctrl"]["generator"]["register"].IsMap());
    QCOMPARE(library["timer_ctrl"]["generator"]["register"].size(), std::size_t(0));

    const CommandResult duplicate = runCommand(arguments);
    QCOMPARE(duplicate.exitCode, 1);
    QVERIFY2(duplicate.output.contains("already exists"), qPrintable(duplicate.output));
    QVERIFY(moduleFile.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(moduleFile.readAll(), original);
}

void Test::createRefusesLockedLibraryWithoutChangingContent()
{
    QTemporaryDir directory;
    createProject(directory);

    const QString modulePath = QDir(directory.path()).filePath("module/peripheral.soc_mod");
    writeTextFile(modulePath, validModule);
    QFile moduleFile(modulePath);
    QVERIFY(moduleFile.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray original = moduleFile.readAll();
    moduleFile.close();

    QLockFile libraryLock(modulePath + QStringLiteral(".lock"));
    QVERIFY(libraryLock.tryLock());

    QStringList arguments = {"qsoc", "module", "create", "--generator", "mmio", "-l", "peripheral"};
    arguments.append(projectOptions(directory));
    arguments.append("watchdog_ctrl");
    const CommandResult result = runCommand(arguments);

    QCOMPARE(result.exitCode, 1);
    QVERIFY2(result.output.contains("module library is locked"), qPrintable(result.output));
    QVERIFY(moduleFile.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(moduleFile.readAll(), original);
}

void Test::createReportsLibraryLockPermissionError()
{
#ifndef Q_OS_UNIX
    QSKIP("Directory permission errors require Unix permission semantics.");
#else
    QTemporaryDir directory;
    createProject(directory);

    const QString            moduleDirectory     = QDir(directory.path()).filePath("module");
    const QFile::Permissions originalPermissions = QFile::permissions(moduleDirectory);
    const auto               restorePermissions  = qScopeGuard(
        [&]() { (void) QFile::setPermissions(moduleDirectory, originalPermissions); });
    QVERIFY(QFile::setPermissions(moduleDirectory, QFile::ReadOwner | QFile::ExeOwner));

    QStringList arguments = {"qsoc", "module", "create", "--generator", "mmio", "-l", "peripheral"};
    arguments.append(projectOptions(directory));
    arguments.append("watchdog_ctrl");
    const CommandResult result = runCommand(arguments);

    QCOMPARE(result.exitCode, 1);
    QVERIFY2(
        result.output.contains("permission denied while locking module library"),
        qPrintable(result.output));
    QVERIFY(!QFile::exists(QDir(moduleDirectory).filePath("peripheral.soc_mod")));
#endif
}

void Test::missingProjectDoesNotUseCurrentDirectory()
{
    QTemporaryDir requestedDirectory;
    QTemporaryDir baitDirectory;
    createProject(requestedDirectory);
    createProject(baitDirectory);

    const QString previousDirectory = QDir::currentPath();
    const auto    restoreDirectory  = qScopeGuard(
        [previousDirectory]() { QDir::setCurrent(previousDirectory); });
    QVERIFY(QDir::setCurrent(baitDirectory.path()));

    const CommandResult result = runCommand(
        {"qsoc",
         "module",
         "create",
         "--generator",
         "mmio",
         "-l",
         "peripheral",
         "-d",
         requestedDirectory.path(),
         "-p",
         "missing_project",
         "timer_ctrl"});

    QCOMPARE(result.exitCode, 1);
    QVERIFY2(result.output.contains("could not load project"), qPrintable(result.output));
    QVERIFY(!QFile::exists(QDir(baitDirectory.path()).filePath("module/peripheral.soc_mod")));
    QVERIFY(!QFile::exists(QDir(requestedDirectory.path()).filePath("module/peripheral.soc_mod")));
}

void Test::validateReportsSuccessAndGeneratorErrors()
{
    QTemporaryDir directory;
    createProject(directory);
    const QString modulePath = QDir(directory.path()).filePath("module/peripheral.soc_mod");
    writeTextFile(modulePath, validModule);

    QStringList arguments = {"qsoc", "module", "validate", "-l", "peripheral"};
    arguments.append(projectOptions(directory));
    arguments.append("timer_ctrl");

    const CommandResult valid = runCommand(arguments);
    QCOMPARE(valid.exitCode, 0);
    QVERIFY2(valid.output.contains("MMIO source is valid"), qPrintable(valid.output));
    QVERIFY2(
        valid.output.contains("Warning: MMIO_IDENTITY generator.identity: absent"),
        qPrintable(valid.output));

    writeTextFile(modulePath, invalidModule);
    const CommandResult invalid = runCommand(arguments);
    QCOMPARE(invalid.exitCode, 1);
    QVERIFY2(
        invalid.output.contains("generator.register.control.field.enable.reset"),
        qPrintable(invalid.output));
}

void Test::malformedGeneratorKindReportsPath()
{
    QTemporaryDir directory;
    createProject(directory);
    QString malformed = validModule;
    malformed.replace("kind: mmio", "kind: pinmux");
    writeTextFile(QDir(directory.path()).filePath("module/peripheral.soc_mod"), malformed);

    QStringList validateArguments = {"qsoc", "module", "validate", "-l", "peripheral"};
    validateArguments.append(projectOptions(directory));
    validateArguments.append("timer_ctrl");
    const CommandResult validated = runCommand(validateArguments);
    QCOMPARE(validated.exitCode, 1);
    QVERIFY2(validated.output.contains("generator.kind"), qPrintable(validated.output));

    QStringList generateArguments = {"qsoc", "generate", "module", "-l", "peripheral"};
    generateArguments.append(projectOptions(directory));
    generateArguments.append("timer_ctrl");
    const CommandResult generated = runCommand(generateArguments);
    QCOMPARE(generated.exitCode, 1);
    QVERIFY2(generated.output.contains("generator.kind"), qPrintable(generated.output));
    QVERIFY(!QFile::exists(
        QDir(directory.path()).filePath("output/peripheral/timer_ctrl/timer_ctrl.v")));
}

void Test::validateAndGenerateRejectOrdinaryModule()
{
    QTemporaryDir directory;
    createProject(directory);
    writeTextFile(QDir(directory.path()).filePath("module/peripheral.soc_mod"), ordinaryModule);

    QStringList validateArguments = {"qsoc", "module", "validate", "-l", "peripheral"};
    validateArguments.append(projectOptions(directory));
    validateArguments.append("timer_ctrl");
    const CommandResult validated = runCommand(validateArguments);
    QCOMPARE(validated.exitCode, 1);
    QVERIFY2(validated.output.contains("does not declare a generator"), qPrintable(validated.output));

    QStringList generateArguments = {"qsoc", "generate", "module", "-l", "peripheral"};
    generateArguments.append(projectOptions(directory));
    generateArguments.append("timer_ctrl");
    const CommandResult generated = runCommand(generateArguments);
    QCOMPARE(generated.exitCode, 1);
    QVERIFY2(generated.output.contains("does not declare a generator"), qPrintable(generated.output));
    QVERIFY(!QFile::exists(
        QDir(directory.path()).filePath("output/peripheral/timer_ctrl/timer_ctrl.v")));
}

void Test::generateUsesNestedPathAndRequiresForceToOverwrite()
{
    QTemporaryDir directory;
    createProject(directory);
    writeTextFile(QDir(directory.path()).filePath("module/peripheral.soc_mod"), validModule);

    QStringList arguments = {"qsoc", "generate", "module", "-l", "peripheral"};
    arguments.append(projectOptions(directory));
    arguments.append("timer_ctrl");

    const QString outputPath
        = QDir(directory.path()).filePath("output/peripheral/timer_ctrl/timer_ctrl.v");
    const QString outputDirectory = QDir(directory.path()).filePath("output/peripheral/timer_ctrl");
    const QStringList   uvmPaths  = uvmArtifactPaths(outputDirectory);
    const CommandResult generated = runCommand(arguments);
    QCOMPARE(generated.exitCode, 0);
    QCOMPARE(generated.output, QStringLiteral("Generated MMIO Verilog: %1").arg(outputPath));
    QVERIFY2(QFile::exists(outputPath), qPrintable(generated.output));
    QVERIFY(!QFile::exists(
        QDir(directory.path()).filePath("output/peripheral/timer_ctrl/timer_ctrl_formal.sv")));
    QVERIFY(!QFile::exists(
        QDir(directory.path()).filePath("output/peripheral/timer_ctrl/timer_ctrl_formal.sby")));
    for (const QString &path : uvmPaths) {
        QVERIFY(!QFile::exists(path));
    }

    writeTextFile(outputPath, "sentinel\n");
    const CommandResult refused = runCommand(arguments);
    QCOMPARE(refused.exitCode, 1);
    QFile outputFile(outputPath);
    QVERIFY(outputFile.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(outputFile.readAll(), QByteArray("sentinel\n"));
    outputFile.close();

    const QString formalSystemVerilogPath
        = QDir(directory.path()).filePath("output/peripheral/timer_ctrl/timer_ctrl_formal.sv");
    const QString formalSbyPath
        = QDir(directory.path()).filePath("output/peripheral/timer_ctrl/timer_ctrl_formal.sby");
    writeTextFile(formalSystemVerilogPath, "formal sentinel\n");
    writeTextFile(formalSbyPath, "runner sentinel\n");
    for (const QString &path : uvmPaths) {
        writeTextFile(path, "uvm sentinel\n");
    }

    arguments.insert(arguments.size() - 1, "-f");
    const CommandResult replaced = runCommand(arguments);
    QCOMPARE(replaced.exitCode, 0);
    QVERIFY(outputFile.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray verilog = outputFile.readAll();
    QVERIFY(verilog.contains("module timer_ctrl"));
    QVERIFY(!verilog.contains("sentinel"));
    QFile formalSystemVerilogFile(formalSystemVerilogPath);
    QVERIFY(formalSystemVerilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(formalSystemVerilogFile.readAll(), QByteArray("formal sentinel\n"));
    QFile formalSbyFile(formalSbyPath);
    QVERIFY(formalSbyFile.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(formalSbyFile.readAll(), QByteArray("runner sentinel\n"));
    for (const QString &path : uvmPaths) {
        QFile uvmFile(path);
        QVERIFY(uvmFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QCOMPARE(uvmFile.readAll(), QByteArray("uvm sentinel\n"));
    }
}

void Test::generateWithFormalWritesAndReplacesCollateral()
{
    QTemporaryDir directory;
    createProject(directory);
    writeTextFile(QDir(directory.path()).filePath("module/peripheral.soc_mod"), validModule);

    QStringList arguments = {"qsoc", "generate", "module", "--with-formal", "-l", "peripheral"};
    arguments.append(projectOptions(directory));
    arguments.append("timer_ctrl");

    const QDir    projectDirectory(directory.path());
    const QString outputDirectory = projectDirectory.filePath("output/peripheral/timer_ctrl");
    const QString verilogPath     = QDir(outputDirectory).filePath("timer_ctrl.v");
    const QString formalSystemVerilogPath = QDir(outputDirectory).filePath("timer_ctrl_formal.sv");
    const QString formalSbyPath           = QDir(outputDirectory).filePath("timer_ctrl_formal.sby");
    const QStringList uvmPaths            = uvmArtifactPaths(outputDirectory);

    const CommandResult generated = runCommand(arguments);
    QCOMPARE(generated.exitCode, 0);
    QVERIFY2(generated.output.contains(formalSystemVerilogPath), qPrintable(generated.output));
    QVERIFY2(generated.output.contains(formalSbyPath), qPrintable(generated.output));
    QVERIFY(QFile::exists(verilogPath));
    QVERIFY(QFile::exists(formalSystemVerilogPath));
    QVERIFY(QFile::exists(formalSbyPath));
    for (const QString &path : uvmPaths) {
        QVERIFY(!QFile::exists(path));
    }

    writeTextFile(verilogPath, "sentinel\n");
    writeTextFile(formalSystemVerilogPath, "sentinel\n");
    writeTextFile(formalSbyPath, "sentinel\n");
    for (const QString &path : uvmPaths) {
        writeTextFile(path, "uvm sentinel\n");
    }
    arguments.insert(arguments.size() - 1, "-f");

    const CommandResult replaced = runCommand(arguments);
    QCOMPARE(replaced.exitCode, 0);
    for (const QString &path : {verilogPath, formalSystemVerilogPath, formalSbyPath}) {
        QFile outputFile(path);
        QVERIFY(outputFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const QByteArray contents = outputFile.readAll();
        QVERIFY(!contents.isEmpty());
        QVERIFY(!contents.contains("sentinel"));
    }
    for (const QString &path : uvmPaths) {
        QFile uvmFile(path);
        QVERIFY(uvmFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QCOMPARE(uvmFile.readAll(), QByteArray("uvm sentinel\n"));
    }
}

void Test::generateWithUvmWritesAndReplacesCollateral()
{
    QTemporaryDir directory;
    createProject(directory);
    writeTextFile(QDir(directory.path()).filePath("module/peripheral.soc_mod"), validModule);

    QStringList arguments = {"qsoc", "generate", "module", "--with-uvm", "-l", "peripheral"};
    arguments.append(projectOptions(directory));
    arguments.append("timer_ctrl");

    const QString outputDirectory = QDir(directory.path()).filePath("output/peripheral/timer_ctrl");
    const QString verilogPath     = QDir(outputDirectory).filePath("timer_ctrl.v");
    const QStringList uvmPaths    = uvmArtifactPaths(outputDirectory);
    const QString formalSystemVerilogPath = QDir(outputDirectory).filePath("timer_ctrl_formal.sv");
    const QString formalSbyPath           = QDir(outputDirectory).filePath("timer_ctrl_formal.sby");

    const CommandResult generated = runCommand(arguments);
    QCOMPARE(generated.exitCode, 0);
    QVERIFY(QFile::exists(verilogPath));
    for (const QString &path : uvmPaths) {
        QVERIFY2(QFile::exists(path), qPrintable(generated.output));
        QVERIFY2(generated.output.contains(path), qPrintable(generated.output));
    }
    QVERIFY(!QFile::exists(formalSystemVerilogPath));
    QVERIFY(!QFile::exists(formalSbyPath));

    writeTextFile(verilogPath, "verilog sentinel\n");
    for (const QString &path : uvmPaths) {
        writeTextFile(path, "uvm sentinel\n");
    }
    writeTextFile(formalSystemVerilogPath, "formal sentinel\n");
    writeTextFile(formalSbyPath, "runner sentinel\n");
    arguments.insert(arguments.size() - 1, "-f");

    const CommandResult replaced = runCommand(arguments);
    QCOMPARE(replaced.exitCode, 0);
    QStringList selectedPaths = {verilogPath};
    selectedPaths.append(uvmPaths);
    for (const QString &path : selectedPaths) {
        QFile outputFile(path);
        QVERIFY(outputFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const QByteArray contents = outputFile.readAll();
        QVERIFY(!contents.isEmpty());
        QVERIFY(!contents.contains("sentinel"));
    }
    QFile formalSystemVerilogFile(formalSystemVerilogPath);
    QVERIFY(formalSystemVerilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(formalSystemVerilogFile.readAll(), QByteArray("formal sentinel\n"));
    QFile formalSbyFile(formalSbyPath);
    QVERIFY(formalSbyFile.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(formalSbyFile.readAll(), QByteArray("runner sentinel\n"));
}

void Test::formalConflictLeavesAllArtifactsUntouched()
{
    QTemporaryDir directory;
    createProject(directory);
    writeTextFile(QDir(directory.path()).filePath("module/peripheral.soc_mod"), validModule);

    QDir projectDirectory(directory.path());
    QVERIFY(projectDirectory.mkpath("output/peripheral/timer_ctrl"));
    const QString outputDirectory = projectDirectory.filePath("output/peripheral/timer_ctrl");
    const QString verilogPath     = QDir(outputDirectory).filePath("timer_ctrl.v");
    const QString formalSystemVerilogPath = QDir(outputDirectory).filePath("timer_ctrl_formal.sv");
    const QString formalSbyPath           = QDir(outputDirectory).filePath("timer_ctrl_formal.sby");
    writeTextFile(formalSbyPath, "sentinel\n");

    QStringList arguments = {"qsoc", "generate", "module", "--with-formal", "-l", "peripheral"};
    arguments.append(projectOptions(directory));
    arguments.append("timer_ctrl");
    const CommandResult generated = runCommand(arguments);

    QCOMPARE(generated.exitCode, 1);
    QVERIFY2(generated.output.contains(formalSbyPath), qPrintable(generated.output));
    QVERIFY2(generated.output.contains("already exists"), qPrintable(generated.output));
    QVERIFY(!QFile::exists(verilogPath));
    QVERIFY(!QFile::exists(formalSystemVerilogPath));
    QFile formalSbyFile(formalSbyPath);
    QVERIFY(formalSbyFile.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(formalSbyFile.readAll(), QByteArray("sentinel\n"));
}

void Test::formalLockLeavesAllArtifactsUntouched()
{
    QTemporaryDir directory;
    createProject(directory);
    writeTextFile(QDir(directory.path()).filePath("module/peripheral.soc_mod"), validModule);

    QDir projectDirectory(directory.path());
    QVERIFY(projectDirectory.mkpath("output/peripheral/timer_ctrl"));
    const QString outputDirectory = projectDirectory.filePath("output/peripheral/timer_ctrl");
    const QString verilogPath     = QDir(outputDirectory).filePath("timer_ctrl.v");
    const QString formalSystemVerilogPath = QDir(outputDirectory).filePath("timer_ctrl_formal.sv");
    const QString formalSbyPath           = QDir(outputDirectory).filePath("timer_ctrl_formal.sby");

    QLockFile formalLock(formalSbyPath + QStringLiteral(".lock"));
    QVERIFY(formalLock.tryLock());

    QStringList arguments
        = {"qsoc", "generate", "module", "--with-formal", "-f", "-l", "peripheral"};
    arguments.append(projectOptions(directory));
    arguments.append("timer_ctrl");
    const CommandResult generated = runCommand(arguments);

    QCOMPARE(generated.exitCode, 1);
    QVERIFY2(generated.output.contains(formalSbyPath), qPrintable(generated.output));
    QVERIFY2(generated.output.contains("output file is locked"), qPrintable(generated.output));
    QVERIFY(!QFile::exists(verilogPath));
    QVERIFY(!QFile::exists(formalSystemVerilogPath));
    QVERIFY(!QFile::exists(formalSbyPath));
}

void Test::formalAndUvmConflictLeavesAllArtifactsUntouched()
{
    QTemporaryDir directory;
    createProject(directory);
    writeTextFile(QDir(directory.path()).filePath("module/peripheral.soc_mod"), validModule);

    QDir projectDirectory(directory.path());
    QVERIFY(projectDirectory.mkpath("output/peripheral/timer_ctrl"));
    const QString     outputDirectory  = projectDirectory.filePath("output/peripheral/timer_ctrl");
    const QStringList artifactPaths    = formalAndUvmArtifactPaths(outputDirectory);
    const QString     lastArtifactPath = artifactPaths.constLast();
    writeTextFile(lastArtifactPath, "sentinel\n");

    QStringList arguments = {
        "qsoc",
        "generate",
        "module",
        "--with-formal",
        "--with-uvm",
        "-l",
        "peripheral",
    };
    arguments.append(projectOptions(directory));
    arguments.append("timer_ctrl");
    const CommandResult generated = runCommand(arguments);

    QCOMPARE(generated.exitCode, 1);
    QVERIFY2(generated.output.contains(lastArtifactPath), qPrintable(generated.output));
    QVERIFY2(generated.output.contains("already exists"), qPrintable(generated.output));
    for (qsizetype index = 0; index + 1 < artifactPaths.size(); ++index) {
        QVERIFY(!QFile::exists(artifactPaths.at(index)));
    }
    QFile lastArtifact(lastArtifactPath);
    QVERIFY(lastArtifact.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(lastArtifact.readAll(), QByteArray("sentinel\n"));
}

void Test::formalAndUvmLockLeavesAllArtifactsUntouched()
{
    QTemporaryDir directory;
    createProject(directory);
    writeTextFile(QDir(directory.path()).filePath("module/peripheral.soc_mod"), validModule);

    QDir projectDirectory(directory.path());
    QVERIFY(projectDirectory.mkpath("output/peripheral/timer_ctrl"));
    const QString     outputDirectory = projectDirectory.filePath("output/peripheral/timer_ctrl");
    const QStringList artifactPaths   = formalAndUvmArtifactPaths(outputDirectory);
    for (const QString &path : artifactPaths) {
        writeTextFile(path, "sentinel\n");
    }

    const QString lastArtifactPath = artifactPaths.constLast();
    QLockFile     lastArtifactLock(lastArtifactPath + QStringLiteral(".lock"));
    QVERIFY(lastArtifactLock.tryLock());

    QStringList arguments = {
        "qsoc",
        "generate",
        "module",
        "--with-formal",
        "--with-uvm",
        "-f",
        "-l",
        "peripheral",
    };
    arguments.append(projectOptions(directory));
    arguments.append("timer_ctrl");
    const CommandResult generated = runCommand(arguments);

    QCOMPARE(generated.exitCode, 1);
    QVERIFY2(generated.output.contains(lastArtifactPath), qPrintable(generated.output));
    QVERIFY2(generated.output.contains("output file is locked"), qPrintable(generated.output));
    for (const QString &path : artifactPaths) {
        QFile artifact(path);
        QVERIFY(artifact.open(QIODevice::ReadOnly | QIODevice::Text));
        QCOMPARE(artifact.readAll(), QByteArray("sentinel\n"));
    }
}

void Test::generateRefusesLockedOutputWithoutChangingContent()
{
    QTemporaryDir directory;
    createProject(directory);
    writeTextFile(QDir(directory.path()).filePath("module/peripheral.soc_mod"), validModule);

    QDir projectDirectory(directory.path());
    QVERIFY(projectDirectory.mkpath("output/peripheral/timer_ctrl"));
    const QString outputDirectory = projectDirectory.filePath("output/peripheral/timer_ctrl");
    const QString outputPath      = QDir(outputDirectory).filePath("timer_ctrl.v");
    writeTextFile(outputPath, "sentinel\n");

    QLockFile outputLock(outputPath + QStringLiteral(".lock"));
    QVERIFY(outputLock.tryLock());
    const QStringList entriesBefore
        = QDir(outputDirectory).entryList(QDir::Files | QDir::NoDotAndDotDot);

    QStringList arguments = {"qsoc", "generate", "module", "-f", "-l", "peripheral"};
    arguments.append(projectOptions(directory));
    arguments.append("timer_ctrl");
    const CommandResult generated = runCommand(arguments);
    QCOMPARE(generated.exitCode, 1);
    QVERIFY2(generated.output.contains("output file is locked"), qPrintable(generated.output));

    QFile outputFile(outputPath);
    QVERIFY(outputFile.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(outputFile.readAll(), QByteArray("sentinel\n"));
    QCOMPARE(QDir(outputDirectory).entryList(QDir::Files | QDir::NoDotAndDotDot), entriesBefore);
}

void Test::invalidGeneratorDoesNotReplaceOutput()
{
    QTemporaryDir directory;
    createProject(directory);
    writeTextFile(QDir(directory.path()).filePath("module/peripheral.soc_mod"), invalidModule);

    QDir projectDirectory(directory.path());
    QVERIFY(projectDirectory.mkpath("output/peripheral/timer_ctrl"));
    const QString outputDirectory = projectDirectory.filePath("output/peripheral/timer_ctrl");
    const QString outputPath      = QDir(outputDirectory).filePath("timer_ctrl.v");
    const QString formalSystemVerilogPath = QDir(outputDirectory).filePath("timer_ctrl_formal.sv");
    const QString formalSbyPath           = QDir(outputDirectory).filePath("timer_ctrl_formal.sby");
    const QStringList uvmPaths            = uvmArtifactPaths(outputDirectory);
    writeTextFile(outputPath, "verilog sentinel\n");
    writeTextFile(formalSystemVerilogPath, "formal sentinel\n");
    writeTextFile(formalSbyPath, "runner sentinel\n");
    for (const QString &path : uvmPaths) {
        writeTextFile(path, "uvm sentinel\n");
    }

    QStringList arguments = {
        "qsoc",
        "generate",
        "module",
        "--with-formal",
        "--with-uvm",
        "-f",
        "-l",
        "peripheral",
    };
    arguments.append(projectOptions(directory));
    arguments.append("timer_ctrl");
    const CommandResult generated = runCommand(arguments);
    QCOMPARE(generated.exitCode, 1);

    QFile outputFile(outputPath);
    QVERIFY(outputFile.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(outputFile.readAll(), QByteArray("verilog sentinel\n"));
    QFile formalSystemVerilogFile(formalSystemVerilogPath);
    QVERIFY(formalSystemVerilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(formalSystemVerilogFile.readAll(), QByteArray("formal sentinel\n"));
    QFile formalSbyFile(formalSbyPath);
    QVERIFY(formalSbyFile.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(formalSbyFile.readAll(), QByteArray("runner sentinel\n"));
    for (const QString &path : uvmPaths) {
        QFile uvmFile(path);
        QVERIFY(uvmFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QCOMPARE(uvmFile.readAll(), QByteArray("uvm sentinel\n"));
    }
}

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsoccliparsemmio.moc"
