// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"
#include "common/qsocconfig.h"
#include "common/qsocgeneratemanager.h"
#include "common/qsociomuxformal.h"
#include "common/qsociomuxgenerator.h"
#include "common/qsocmmioformal.h"
#include "common/qsocmmiogenerator.h"
#include "common/qsocmmiouvm.h"
#include "common/qsocmodulemanager.h"
#include "common/qsocprojectmanager.h"
#include "common/qsocuvmresources.h"
#include "common/qsocverilogutils.h"
#include "common/qsocyamlutils.h"

#include <fstream>
#include <memory>
#include <vector>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLockFile>
#include <QSaveFile>
#include <QTextStream>

namespace {

struct GeneratedArtifact
{
    QString    path;
    QByteArray contents;
};

QString prepareGeneratedArtifacts(std::vector<GeneratedArtifact> *artifacts)
{
    QMap<QString, QString> paths;
    for (const auto &artifact : *artifacts) {
        paths.insert(QFileInfo(artifact.path).fileName(), artifact.path);
    }
    std::vector<GeneratedArtifact> dependencies;
    for (auto &artifact : *artifacts) {
        const QFileInfo info(artifact.path);
        const QDir      directory = info.dir();
        const QString   legacy    = QDir::cleanPath(
            directory.filePath(QStringLiteral("../") + info.fileName()));
        if (QFileInfo::exists(legacy) || QFileInfo(legacy).isSymLink()) {
            return QCoreApplication::translate(
                       "main",
                       "Error: legacy output exists: %1. Move the old module output directory "
                       "before generating.")
                .arg(QDir::cleanPath(legacy));
        }
        const bool fileList = artifact.path.endsWith(QStringLiteral(".fl"));
        const bool sby      = artifact.path.endsWith(QStringLiteral(".sby"));
        if (!fileList && !sby) {
            continue;
        }
        QStringList lines = QString::fromUtf8(artifact.contents).split(QLatin1Char('\n'));
        bool        files = fileList;
        for (QString &line : lines) {
            if (sby && line.startsWith(QLatin1Char('['))) {
                files = line == QStringLiteral("[files]");
            }
            if (files && paths.contains(line)) {
                line = directory.relativeFilePath(paths.value(line));
            }
        }
        artifact.contents = lines.join(QLatin1Char('\n')).toUtf8();
        if (directory.dirName() != QStringLiteral("uvm")
            || !info.fileName().endsWith(QStringLiteral("_uvm.fl"))) {
            continue;
        }
        const auto sources = QSocUvmResources::sources();
        if (!sources.contains(QStringLiteral("src/uvm_pkg.sv"))) {
            return QCoreApplication::translate("main", "Error: embedded UVM sources are missing.");
        }
        for (auto source = sources.cbegin(); source != sources.cend(); ++source) {
            dependencies.push_back(
                {directory.filePath(QStringLiteral("uvm-core/") + source.key()), source.value()});
        }
        QString standalone = info.fileName();
        standalone.chop(3);
        standalone += QStringLiteral("_standalone.fl");
        dependencies.push_back(
            {directory.filePath(standalone),
             QByteArray("+incdir+uvm-core/src\nuvm-core/src/uvm_pkg.sv\n") + artifact.contents});
    }
    artifacts->insert(artifacts->end(), dependencies.begin(), dependencies.end());
    return {};
}

QString writeGeneratedArtifacts(std::vector<GeneratedArtifact> artifacts, bool force)
{
    const QString preparationError = prepareGeneratedArtifacts(&artifacts);
    if (!preparationError.isEmpty()) {
        return preparationError;
    }
    const QString root = QFileInfo(QFileInfo(artifacts.front().path).absolutePath()).absolutePath();
    for (const auto &artifact : artifacts) {
        const QFileInfo target(artifact.path);
        if (target.isSymLink() || (target.exists() && !target.isFile())) {
            return QCoreApplication::translate("main", "Error: output is not a regular file: %1")
                .arg(artifact.path);
        }
        if (!force && target.exists()) {
            return QCoreApplication::translate("main", "Error: output file already exists: %1")
                .arg(artifact.path);
        }
        QString parent = target.absolutePath();
        while (true) {
            const QFileInfo entry(parent);
            if (entry.isSymLink() || (entry.exists() && !entry.isDir())) {
                return QCoreApplication::translate("main", "Error: invalid output directory: %1")
                    .arg(parent);
            }
            if (parent == root) {
                break;
            }
            parent = entry.absolutePath();
        }
    }
    for (const auto &artifact : artifacts) {
        const QFileInfo target(artifact.path);
        if (!QDir().mkpath(target.absolutePath())) {
            return QCoreApplication::translate("main", "Error: could not create output directory: %1")
                .arg(target.absolutePath());
        }
    }
    QLockFile outputLock(QDir(root).filePath(QStringLiteral(".generate.lock")));
    if (!outputLock.tryLock()) {
        return QCoreApplication::translate("main", "Error: module output is locked: %1").arg(root);
    }
    if (!force) {
        for (const GeneratedArtifact &artifact : artifacts) {
            if (QFile::exists(artifact.path)) {
                return QCoreApplication::translate("main", "Error: output file already exists: %1")
                    .arg(artifact.path);
            }
        }
    }

    std::vector<std::unique_ptr<QSaveFile>> outputFiles;
    outputFiles.reserve(artifacts.size());
    for (const GeneratedArtifact &artifact : artifacts) {
        auto outputFile = std::make_unique<QSaveFile>(artifact.path);
        outputFile->setDirectWriteFallback(false);
        if (!outputFile->open(QIODevice::WriteOnly)) {
            return QCoreApplication::translate("main", "Error: could not open output file: %1")
                .arg(outputFile->errorString());
        }
        if (outputFile->write(artifact.contents) != artifact.contents.size()) {
            const QString error = outputFile->errorString();
            outputFile->cancelWriting();
            return QCoreApplication::translate("main", "Error: could not write output file: %1")
                .arg(error);
        }
        outputFiles.push_back(std::move(outputFile));
    }
    for (const std::unique_ptr<QSaveFile> &outputFile : outputFiles) {
        if (!outputFile->commit()) {
            return QCoreApplication::translate("main", "Error: could not commit output file: %1")
                .arg(outputFile->errorString());
        }
    }
    return QString();
}

bool isValidLibraryBasename(const QString &name)
{
    if (name.isEmpty() || name == "." || name == "..") {
        return false;
    }
    const QString invalidChars = "\\/:*?\"<>|.";
    for (const QChar character : invalidChars) {
        if (name.contains(character)) {
            return false;
        }
    }
    return true;
}

} // namespace

bool QSocCliWorker::parseGenerate(const QStringList &appArguments)
{
    /* Clear upstream positional arguments and setup subcommand */
    parser.clearPositionalArguments();
    parser.addPositionalArgument(
        "subcommand",
        QCoreApplication::translate(
            "main",
            "verilog    Generate Verilog code from netlist file.\n"
            "module     Generate artifacts for a generated module.\n"
            "template   Generate files from Jinja2 templates.\n"
            "stub       Generate stub files for modules."),
        "generate <subcommand> [subcommand options]");

    parser.parse(appArguments);
    const QStringList positionalArgs = parser.positionalArguments();
    if (positionalArgs.isEmpty()) {
        return showHelpOrError(1, QCoreApplication::translate("main", "Error: missing subcommand."));
    }
    const QString &command       = positionalArgs.first();
    QStringList    nextArguments = appArguments;
    if (command == "verilog") {
        nextArguments.removeOne(command);
        if (!parseGenerateVerilog(nextArguments)) {
            return false;
        }
    } else if (command == "module") {
        nextArguments.removeOne(command);
        if (!parseGenerateModule(nextArguments)) {
            return false;
        }
    } else if (command == "template") {
        nextArguments.removeOne(command);
        if (!parseGenerateTemplate(nextArguments)) {
            return false;
        }
    } else if (command == "stub") {
        nextArguments.removeOne(command);
        if (!parseGenerateStub(nextArguments)) {
            return false;
        }
    } else {
        return showHelpOrError(
            1, QCoreApplication::translate("main", "Error: unknown subcommand: %1.").arg(command));
    }

    return true;
}

bool QSocCliWorker::parseGenerateModule(const QStringList &appArguments)
{
    parser.clearPositionalArguments();
    parser.addOptions({
        {{"d", "directory"},
         QCoreApplication::translate("main", "The path to the project directory."),
         "project directory"},
        {{"p", "project"}, QCoreApplication::translate("main", "The project name."), "project name"},
        {{"l", "library"},
         QCoreApplication::translate("main", "The exact module library name."),
         "library name"},
        {{"f", "force"},
         QCoreApplication::translate("main", "Replace existing requested output files.")},
        {"with-formal",
         QCoreApplication::translate("main", "Generate formal verification collateral.")},
        {"formal-bank",
         QCoreApplication::translate("main", "Pins per IOMUX routing proof task (default 16)."),
         "pins"},
        {"with-uvm", QCoreApplication::translate("main", "Generate a UVM testbench.")},
    });
    parser.addPositionalArgument(
        "module", QCoreApplication::translate("main", "The exact module name."), "<module>");

    if (!parseOptions(appArguments)) {
        return false;
    }

    const QStringList positionalArgs = parser.positionalArguments();
    if (positionalArgs.size() != 1) {
        return showHelpOrError(
            1, QCoreApplication::translate("main", "Error: expected one module name."));
    }
    if (!parser.isSet("library")) {
        return showHelpOrError(1, QCoreApplication::translate("main", "Error: missing library name."));
    }

    const QString libraryName = parser.value("library");
    const QString moduleName  = positionalArgs.first();
    if (!isValidLibraryBasename(libraryName)) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid library name: %1.").arg(libraryName));
    }
    if (!QSocVerilogUtils::isValidVerilogIdentifier(moduleName)) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid module name: %1.").arg(moduleName));
    }

    if (!loadSelectedProject()) {
        return false;
    }
    if (!projectManager->isValidModulePath()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid module directory: %1")
                .arg(projectManager->getModulePath()));
    }
    if (!projectManager->isValidOutputPath()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid output directory: %1")
                .arg(projectManager->getOutputPath()));
    }
    if (!moduleManager->load(libraryName)
        || !moduleManager->listModulesInLibrary(libraryName).contains(moduleName)) {
        return showError(
            1,
            QCoreApplication::translate("main", "Error: module not found: %1/%2.")
                .arg(libraryName, moduleName));
    }

    const QSocModuleDefinition definition
        = moduleManager->getModuleDefinition(libraryName, moduleName);
    if (!definition.extraAttributes["generator"]) {
        return showError(
            1,
            QCoreApplication::translate("main", "Error: module does not declare a generator: %1/%2.")
                .arg(libraryName, moduleName));
    }
    if (QSocIomuxGenerator::isIomux(definition)) {
        QSocIomuxPlan plan;
        QStringList   planErrors;
        if (!QSocIomuxGenerator::buildPlan(definition, &plan, &planErrors)) {
            QStringList messages;
            messages.reserve(planErrors.size());
            for (const QString &error : planErrors) {
                messages.append(QCoreApplication::translate("main", "Error: %1").arg(error));
            }
            if (messages.isEmpty()) {
                messages.append(
                    QCoreApplication::translate("main", "Error: IOMUX generation failed."));
            }
            return showError(1, messages.join('\n'));
        }

        const auto reportPadErrors = [&](const QStringList &padErrors) {
            QStringList messages;
            messages.reserve(padErrors.size());
            for (const QString &error : padErrors) {
                messages.append(QCoreApplication::translate("main", "Error: %1").arg(error));
            }
            return showError(1, messages.join('\n'));
        };
        QStringList cellErrors;
        if (!moduleManager->resolveIomuxCells(&plan, &cellErrors)) {
            return reportPadErrors(cellErrors);
        }

        const QStringList libraryModules = moduleManager->listModulesInLibrary(libraryName);
        for (const QString &suffix :
             {QStringLiteral("_regs"),
              QStringLiteral("_conn"),
              QStringLiteral("_core"),
              QStringLiteral("_io")}) {
            const QString derived = moduleName + suffix;
            if (libraryModules.contains(derived)) {
                return showError(
                    1,
                    QCoreApplication::translate(
                        "main", "Error: generated module name collides with %1/%2.")
                        .arg(libraryName, derived));
            }
        }

        QDir          outputDirectory(projectManager->getOutputPath());
        const QString relativeDirectory = QStringLiteral("%1/%2").arg(libraryName, moduleName);
        if (!outputDirectory.mkpath(relativeDirectory)) {
            return showError(
                1,
                QCoreApplication::translate("main", "Error: could not create output directory: %1")
                    .arg(outputDirectory.filePath(relativeDirectory)));
        }
        const auto outputFilePath = [&](const QString &fileName,
                                        const QString &group = QStringLiteral("rtl")) {
            return QDir(outputDirectory.filePath(relativeDirectory))
                .filePath(group + QLatin1Char('/') + fileName);
        };
        const QString regsPath = outputFilePath(moduleName + QStringLiteral("_regs.v"));
        const QString connPath = outputFilePath(moduleName + QStringLiteral("_conn.v"));
        const QString topPath  = outputFilePath(moduleName + QStringLiteral(".v"));
        const QString listPath = outputFilePath(moduleName + QStringLiteral(".fl"));
        const QString reportPath
            = outputFilePath(moduleName + QStringLiteral(".iomux.rpt"), QStringLiteral("reports"));
        const QString integrationPath = outputFilePath(
            moduleName + QStringLiteral("_integration.soc_net"), QStringLiteral("integration"));

        std::vector<GeneratedArtifact> artifacts
            = {{regsPath, QSocIomuxGenerator::generateRegsVerilog(plan).toUtf8()},
               {connPath, QSocIomuxGenerator::generateConnVerilog(plan).toUtf8()},
               {topPath, QSocIomuxGenerator::generateTopVerilog(plan).toUtf8()},
               {listPath, QSocIomuxGenerator::generateFileList(plan).toUtf8()},
               {reportPath, QSocIomuxGenerator::generateReport(plan).toUtf8()},
               {outputFilePath(moduleName + QStringLiteral("_regs.h"), QStringLiteral("include")),
                QSocIomuxGenerator::generateSoftwareHeader(plan).toUtf8()},
               {integrationPath, QSocIomuxGenerator::generateIntegrationNetlist(plan).toUtf8()}};
        const QString ioModule = QSocIomuxGenerator::ioModuleName(moduleName);
        if (plan.hasPadCell()) {
            artifacts.push_back(
                {outputFilePath(ioModule + QStringLiteral(".v")),
                 QSocIomuxGenerator::generateIoVerilog(plan).toUtf8()});
        }
        if (plan.ioRing.declared) {
            artifacts.push_back(
                {outputFilePath(moduleName + QStringLiteral(".ring.rpt"), QStringLiteral("reports")),
                 QSocIomuxGenerator::generateRingReport(plan).toUtf8()});
            const QSocIoRingGeometry geometry = QSocIomuxGenerator::ringGeometry(plan);
            if (!geometry.contradiction.isEmpty()) {
                return showError(
                    1,
                    QCoreApplication::translate("main", "Error: the ring does not fit: %1")
                        .arg(geometry.contradiction.join("; ")));
            }
            if (geometry.complete) {
                artifacts.push_back(
                    {outputFilePath(ioModule + QStringLiteral(".def"), QStringLiteral("integration")),
                     QSocIomuxGenerator::generateRingDef(plan).toUtf8()});
            } else {
                showInfo(
                    0,
                    QCoreApplication::translate("main", "Ring DEF not written, needs %1")
                        .arg(geometry.missing.join("; ")));
            }
        }
        QString formalSystemVerilogPath;
        QString formalSbyPath;
        QString hsFormalSystemVerilogPath;
        QString hsFormalSbyPath;
        QString padFormalSystemVerilogPath;
        QString padFormalSbyPath;
        QString formalListPath;
        quint32 bankPins = QSocIomuxFormal::kDefaultBankPins;
        if (parser.isSet("formal-bank")) {
            bool ok  = false;
            bankPins = parser.value("formal-bank").toUInt(&ok);
            if (!ok || bankPins == 0) {
                return showError(
                    1,
                    QCoreApplication::translate(
                        "main", "Error: --formal-bank takes a pin count of 1 or more."));
            }
        }
        if (parser.isSet("with-formal")) {
            const QSocMmioFormalCollateral collateral = QSocMmioFormal::generate(plan.mmio);
            formalSystemVerilogPath                   = outputFilePath(
                moduleName + QStringLiteral("_regs_formal.sv"), QStringLiteral("formal"));
            formalSbyPath = outputFilePath(
                moduleName + QStringLiteral("_regs_formal.sby"), QStringLiteral("formal"));
            artifacts.push_back({formalSystemVerilogPath, collateral.systemVerilog.toUtf8()});
            artifacts.push_back({formalSbyPath, collateral.sby.toUtf8()});
            const QSocIomuxFormalCollateral hsCollateral = QSocIomuxFormal::generate(plan, bankPins);
            hsFormalSystemVerilogPath = outputFilePath(
                moduleName + QStringLiteral("_hs_formal.sv"), QStringLiteral("formal"));
            hsFormalSbyPath = outputFilePath(
                moduleName + QStringLiteral("_hs_formal.sby"), QStringLiteral("formal"));
            artifacts.push_back({hsFormalSystemVerilogPath, hsCollateral.systemVerilog.toUtf8()});
            artifacts.push_back({hsFormalSbyPath, hsCollateral.sby.toUtf8()});
            const QSocIomuxFormalCollateral padCollateral = QSocIomuxFormal::generatePad(plan);
            if (!padCollateral.systemVerilog.isEmpty()) {
                padFormalSystemVerilogPath = outputFilePath(
                    ioModule + QStringLiteral("_formal.sv"), QStringLiteral("formal"));
                padFormalSbyPath = outputFilePath(
                    ioModule + QStringLiteral("_formal.sby"), QStringLiteral("formal"));
                artifacts.push_back(
                    {padFormalSystemVerilogPath, padCollateral.systemVerilog.toUtf8()});
                artifacts.push_back({padFormalSbyPath, padCollateral.sby.toUtf8()});
            }
            formalListPath
                = outputFilePath(moduleName + QStringLiteral("_formal.fl"), QStringLiteral("formal"));
            artifacts.push_back({formalListPath, QSocIomuxFormal::generateFileList(plan).toUtf8()});
        }
        QString uvmInterfacePath;
        QString uvmPackagePath;
        QString uvmTestbenchPath;
        QString uvmFileListPath;
        if (parser.isSet("with-uvm")) {
            const QSocMmioUvmCollateral collateral = QSocMmioUvm::generate(plan.mmio);
            uvmInterfacePath                       = outputFilePath(
                moduleName + QStringLiteral("_regs_uvm_if.sv"), QStringLiteral("uvm"));
            uvmPackagePath = outputFilePath(
                moduleName + QStringLiteral("_regs_uvm_pkg.sv"), QStringLiteral("uvm"));
            uvmTestbenchPath = outputFilePath(
                moduleName + QStringLiteral("_regs_uvm_tb.sv"), QStringLiteral("uvm"));
            uvmFileListPath
                = outputFilePath(moduleName + QStringLiteral("_regs_uvm.fl"), QStringLiteral("uvm"));
            artifacts.push_back({uvmInterfacePath, collateral.interfaceSource.toUtf8()});
            artifacts.push_back({uvmPackagePath, collateral.packageSource.toUtf8()});
            artifacts.push_back({uvmTestbenchPath, collateral.testbenchSource.toUtf8()});
            artifacts.push_back({uvmFileListPath, collateral.fileList.toUtf8()});
        }

        const QString writeError = writeGeneratedArtifacts(artifacts, parser.isSet("force"));
        if (!writeError.isEmpty()) {
            return showError(1, writeError);
        }

        QStringList messages = {
            QCoreApplication::translate("main", "Generated IOMUX Verilog: %1, %2, %3")
                .arg(regsPath, connPath, topPath),
            QCoreApplication::translate("main", "Generated IOMUX file list: %1").arg(listPath),
            QCoreApplication::translate("main", "Generated IOMUX report: %1").arg(reportPath),
            QCoreApplication::translate("main", "Generated IOMUX integration netlist: %1")
                .arg(integrationPath),
        };
        if (parser.isSet("with-formal")) {
            messages.append(
                QCoreApplication::translate("main", "Generated MMIO formal collateral: %1, %2")
                    .arg(formalSystemVerilogPath, formalSbyPath));
            messages.append(
                QCoreApplication::translate("main", "Generated HS formal collateral: %1, %2")
                    .arg(hsFormalSystemVerilogPath, hsFormalSbyPath));
            if (!padFormalSystemVerilogPath.isEmpty()) {
                messages.append(
                    QCoreApplication::translate("main", "Generated pad formal collateral: %1, %2")
                        .arg(padFormalSystemVerilogPath, padFormalSbyPath));
            }
            messages.append(
                QCoreApplication::translate("main", "Generated formal file list: %1")
                    .arg(formalListPath));
            const quint32 banks = (plan.pinCount + bankPins - 1) / bankPins;
            if (banks > 1) {
                messages.append(
                    QCoreApplication::translate(
                        "main",
                        "HS routing proof: %1 banks, bank size %2, tasks prove_bN and bmc_bN")
                        .arg(banks)
                        .arg(bankPins));
            }
        }
        if (parser.isSet("with-uvm")) {
            messages.append(
                QCoreApplication::translate(
                    "main",
                    "Generated MMIO UVM testbench (covers %1_regs only, not routing): "
                    "%2, %3, %4, %5")
                    .arg(
                        moduleName,
                        uvmInterfacePath,
                        uvmPackagePath,
                        uvmTestbenchPath,
                        uvmFileListPath));
            messages.append(
                QCoreApplication::translate("main", "Standalone UVM file list: %1")
                    .arg(uvmFileListPath.chopped(3) + QStringLiteral("_standalone.fl")));
        }
        return showInfo(0, messages.join('\n'));
    }

    if (parser.isSet("formal-bank")) {
        return showError(
            1,
            QCoreApplication::translate(
                "main", "Error: --formal-bank applies to IOMUX modules only."));
    }
    QString     verilog;
    QStringList errors;
    if (!QSocMmioGenerator::generateVerilog(definition, &verilog, &errors)) {
        QStringList messages;
        messages.reserve(errors.size());
        for (const QString &error : errors) {
            messages.append(QCoreApplication::translate("main", "Error: %1").arg(error));
        }
        if (messages.isEmpty()) {
            messages.append(QCoreApplication::translate("main", "Error: MMIO generation failed."));
        }
        return showError(1, messages.join('\n'));
    }

    QSocMmioFormalCollateral formalCollateral;
    const bool               withFormal = parser.isSet("with-formal");
    if (withFormal
        && !QSocMmioGenerator::generateFormalCollateral(definition, &formalCollateral, &errors)) {
        QStringList messages;
        messages.reserve(errors.size());
        for (const QString &error : errors) {
            messages.append(QCoreApplication::translate("main", "Error: %1").arg(error));
        }
        if (messages.isEmpty()) {
            messages.append(QCoreApplication::translate("main", "Error: formal generation failed."));
        }
        return showError(1, messages.join('\n'));
    }

    QSocMmioUvmCollateral uvmCollateral;
    const bool            withUvm = parser.isSet("with-uvm");
    if (withUvm && !QSocMmioGenerator::generateUvmCollateral(definition, &uvmCollateral, &errors)) {
        QStringList messages;
        messages.reserve(errors.size());
        for (const QString &error : errors) {
            messages.append(QCoreApplication::translate("main", "Error: %1").arg(error));
        }
        if (messages.isEmpty()) {
            messages.append(QCoreApplication::translate("main", "Error: UVM generation failed."));
        }
        return showError(1, messages.join('\n'));
    }

    QDir          outputDirectory(projectManager->getOutputPath());
    const QString relativeDirectory = QStringLiteral("%1/%2").arg(libraryName, moduleName);
    if (!outputDirectory.mkpath(relativeDirectory)) {
        return showError(
            1,
            QCoreApplication::translate("main", "Error: could not create output directory: %1")
                .arg(outputDirectory.filePath(relativeDirectory)));
    }

    const auto outputFilePath = [&](const QString &name,
                                    const QString &group = QStringLiteral("rtl")) {
        return QDir(outputDirectory.filePath(relativeDirectory))
            .filePath(group + QLatin1Char('/') + name);
    };
    const QString                  outputPath = outputFilePath(moduleName + QStringLiteral(".v"));
    std::vector<GeneratedArtifact> artifacts
        = {{outputPath, verilog.toUtf8()},
           {outputFilePath(moduleName + QStringLiteral(".fl")),
            (moduleName + QStringLiteral(".v\n")).toUtf8()}};
    QString formalSystemVerilogPath;
    QString formalSbyPath;
    QString formalListPath;
    if (withFormal) {
        formalSystemVerilogPath
            = outputFilePath(moduleName + QStringLiteral("_formal.sv"), QStringLiteral("formal"));
        formalSbyPath
            = outputFilePath(moduleName + QStringLiteral("_formal.sby"), QStringLiteral("formal"));
        formalListPath
            = outputFilePath(moduleName + QStringLiteral("_formal.fl"), QStringLiteral("formal"));
        artifacts.push_back({formalSystemVerilogPath, formalCollateral.systemVerilog.toUtf8()});
        artifacts.push_back({formalSbyPath, formalCollateral.sby.toUtf8()});
        artifacts.push_back(
            {formalListPath, QStringLiteral("%1.v\n%1_formal.sv\n").arg(moduleName).toUtf8()});
    }
    QString uvmInterfacePath;
    QString uvmPackagePath;
    QString uvmTestbenchPath;
    QString uvmFileListPath;
    if (withUvm) {
        uvmInterfacePath
            = outputFilePath(moduleName + QStringLiteral("_uvm_if.sv"), QStringLiteral("uvm"));
        uvmPackagePath
            = outputFilePath(moduleName + QStringLiteral("_uvm_pkg.sv"), QStringLiteral("uvm"));
        uvmTestbenchPath
            = outputFilePath(moduleName + QStringLiteral("_uvm_tb.sv"), QStringLiteral("uvm"));
        uvmFileListPath
            = outputFilePath(moduleName + QStringLiteral("_uvm.fl"), QStringLiteral("uvm"));
        artifacts.push_back({uvmInterfacePath, uvmCollateral.interfaceSource.toUtf8()});
        artifacts.push_back({uvmPackagePath, uvmCollateral.packageSource.toUtf8()});
        artifacts.push_back({uvmTestbenchPath, uvmCollateral.testbenchSource.toUtf8()});
        artifacts.push_back({uvmFileListPath, uvmCollateral.fileList.toUtf8()});
    }

    const QString writeError = writeGeneratedArtifacts(artifacts, parser.isSet("force"));
    if (!writeError.isEmpty()) {
        return showError(1, writeError);
    }

    QStringList messages = {
        QCoreApplication::translate("main", "Generated MMIO Verilog: %1").arg(outputPath),
    };
    if (withFormal) {
        messages.append(
            QCoreApplication::translate("main", "Generated MMIO formal collateral: %1, %2, %3")
                .arg(formalSystemVerilogPath, formalSbyPath, formalListPath));
    }
    if (withUvm) {
        messages.append(
            QCoreApplication::translate("main", "Generated MMIO UVM testbench: %1, %2, %3, %4")
                .arg(uvmInterfacePath, uvmPackagePath, uvmTestbenchPath, uvmFileListPath));
        messages.append(
            QCoreApplication::translate("main", "Standalone UVM file list: %1")
                .arg(uvmFileListPath.chopped(3) + QStringLiteral("_standalone.fl")));
    }
    return showInfo(0, messages.join('\n'));
}

bool QSocCliWorker::parseGenerateVerilog(const QStringList &appArguments)
{
    /* Clear upstream positional arguments and setup subcommand */
    parser.clearPositionalArguments();
    parser.addOptions({
        {{"d", "directory"},
         QCoreApplication::translate("main", "The path to the project directory."),
         "project directory"},
        {{"p", "project"}, QCoreApplication::translate("main", "The project name."), "project name"},
        {{"m", "merge"},
         QCoreApplication::translate(
             "main", "Merge multiple netlist files in order before processing.")},
        {{"f", "force"},
         QCoreApplication::translate(
             "main", "Replace existing clock, reset, and power primitive cell files.")},
        {"format",
         QCoreApplication::translate(
             "main",
             "Run verible-verilog-format from PATH on each generated top-level Verilog file.")},
        {"check",
         QCoreApplication::translate(
             "main", "Check PRCM resource binding and stable modes without writing RTL.")},
    });

    parser.addPositionalArgument(
        "files",
        QCoreApplication::translate("main", "The netlist files to be processed."),
        "[<netlist files>]");

    if (!parser.parse(appArguments)) {
        return showErrorWithHelp(
            1, QCoreApplication::translate("main", "Error: %1").arg(parser.errorText()));
    }

    if (parser.isSet("help")) {
        return showHelp(0);
    }

    const QStringList  positionalArgs = parser.positionalArguments();
    const QStringList &filePathList   = positionalArgs;
    if (filePathList.isEmpty()) {
        return showHelpOrError(
            1, QCoreApplication::translate("main", "Error: missing netlist files."));
    }

    if (parser.isSet("check")) {
        if (parser.isSet("merge") || parser.isSet("force") || parser.isSet("format")) {
            return showError(
                1,
                QCoreApplication::translate(
                    "main", "Error: --check does not support --merge, --force or --format."));
        }
        return checkPrcmNetlists(filePathList);
    }

    /* Setup project manager and project path  */
    if (parser.isSet("directory")) {
        const QString dirPath = parser.value("directory");
        projectManager->setProjectPath(dirPath);
    }

    if (parser.isSet("project")) {
        projectManager->load(parser.value("project"));
    } else {
        const QStringList &projectNameList = projectManager->list(QRegularExpression(".*"));
        if (projectNameList.length() > 1) {
            return showErrorWithHelp(
                1,
                QCoreApplication::translate(
                    "main",
                    "Error: multiple projects found, please specify the project name.\n"
                    "Available projects are:\n%1\n")
                    .arg(projectNameList.join("\n")));
        }
        projectManager->loadFirst();
    }

    /* Check if output path is valid */
    if (!projectManager->isValidOutputPath()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid output directory: %1")
                .arg(projectManager->getOutputPath()));
    }

    /* Load modules */
    if (!moduleManager->load(QRegularExpression(".*"))) {
        return showErrorWithHelp(
            1, QCoreApplication::translate("main", "Error: could not load library"));
    }

    /* Load buses */
    if (!busManager->load(QRegularExpression(".*"))) {
        return showErrorWithHelp(
            1, QCoreApplication::translate("main", "Error: could not load buses"));
    }

    /* Check if merge mode is enabled */
    const bool mergeMode = parser.isSet("merge");

    /* Set force overwrite mode if enabled */
    if (parser.isSet("force")) {
        generateManager->setForceOverwrite(true);
    }

    if (mergeMode && filePathList.size() > 1) {
        /* Merge mode: combine multiple netlist files */
        return processMergedNetlists(filePathList);
    }
    /* Normal mode: process each netlist file separately */
    return processIndividualNetlists(filePathList);
}

bool QSocCliWorker::processMergedNetlists(const QStringList &filePathList)
{
    /* Validate all files exist first */
    for (const QString &netlistFilePath : filePathList) {
        if (!QFile::exists(netlistFilePath)) {
            return showError(
                1,
                QCoreApplication::translate("main", "Error: Netlist file does not exist: \"%1\"")
                    .arg(netlistFilePath));
        }
    }

    /* Load and merge all netlist files */
    YAML::Node mergedNetlist;
    QString    outputFileName;
    const auto isIomuxInstance = [this](const YAML::Node &instanceNode) {
        if (!instanceNode || !instanceNode.IsMap() || !instanceNode["module"]
            || !instanceNode["module"].IsScalar()) {
            return false;
        }
        const QString moduleName = QString::fromStdString(instanceNode["module"].as<std::string>());
        if (!moduleManager->isModuleExist(moduleName)) {
            return false;
        }
        const YAML::Node generator = moduleManager->getModuleYaml(moduleName)["generator"];
        return generator && generator.IsMap() && generator["kind"] && generator["kind"].IsScalar()
               && generator["kind"].Scalar() == "iomux";
    };

    for (int i = 0; i < filePathList.size(); ++i) {
        const QString &netlistFilePath = filePathList.at(i);

        /* Load the current netlist file */
        std::ifstream fileStream(netlistFilePath.toStdString());
        if (!fileStream.is_open()) {
            return showError(
                1,
                QCoreApplication::translate("main", "Error: Unable to open netlist file: \"%1\"")
                    .arg(netlistFilePath));
        }

        try {
            const YAML::Node currentNetlist = YAML::Load(fileStream);
            fileStream.close();

            if (i == 0) {
                /* For the first file, use it as the base */
                mergedNetlist = currentNetlist;

                /* Use the first file's basename for output */
                const QFileInfo fileInfo(netlistFilePath);
                outputFileName = fileInfo.baseName();
            } else {
                if (mergedNetlist["instance"] && mergedNetlist["instance"].IsMap()
                    && currentNetlist["instance"] && currentNetlist["instance"].IsMap()) {
                    for (const auto &instancePair : currentNetlist["instance"]) {
                        if (!instancePair.first.IsScalar()) {
                            continue;
                        }
                        const std::string instanceName = instancePair.first.as<std::string>();
                        const YAML::Node  existing     = mergedNetlist["instance"][instanceName];
                        if (existing
                            && (isIomuxInstance(existing) || isIomuxInstance(instancePair.second))) {
                            return showError(
                                1,
                                QCoreApplication::translate(
                                    "main",
                                    "Error: generated IOMUX instance is declared in more than one "
                                    "merged netlist: %1")
                                    .arg(QString::fromStdString(instanceName)));
                        }
                    }
                }
                /* For subsequent files, merge them using the QSocYamlUtils mergeNodes function */
                mergedNetlist = QSocYamlUtils::mergeNodes(mergedNetlist, currentNetlist);

                /* Keep using the first file's basename for output filename */
                /* No need to append additional names for merged content */
            }

            showInfo(
                0,
                QCoreApplication::translate("main", "Loaded netlist file: %1").arg(netlistFilePath));

        } catch (const YAML::Exception &e) {
            return showError(
                1,
                QCoreApplication::translate("main", "Error parsing YAML file: %1: %2")
                    .arg(netlistFilePath, e.what()));
        }
    }

    /* Set the merged netlist data in the generate manager */
    if (!generateManager->setNetlistData(mergedNetlist)) {
        return showError(
            1, QCoreApplication::translate("main", "Error: failed to set merged netlist data"));
    }

    /* Process the merged netlist */
    if (!generateManager->processNetlist()) {
        return showError(
            1, QCoreApplication::translate("main", "Error: failed to process merged netlist"));
    }

    /* Generate Verilog code for the merged netlist */
    if (!generateManager->generateVerilog(outputFileName, parser.isSet("format"))) {
        const QString message
            = parser.isSet("format")
                  ? QCoreApplication::translate(
                        "main", "Error: failed to generate or format merged Verilog code: %1")
                  : QCoreApplication::translate(
                        "main", "Error: failed to generate Verilog code for merged netlist: %1");
        return showError(1, message.arg(outputFileName));
    }

    showInfo(
        0,
        QCoreApplication::translate(
            "main", "Successfully generated Verilog code for merged netlist: %1")
            .arg(QDir(projectManager->getOutputPath()).filePath(outputFileName + ".v")));

    return true;
}

bool QSocCliWorker::processIndividualNetlists(const QStringList &filePathList)
{
    /* Generate Verilog code for each netlist file individually */
    for (const QString &netlistFilePath : filePathList) {
        /* Check if the netlist file exists before trying to load it */
        if (!QFile::exists(netlistFilePath)) {
            return showError(
                1,
                QCoreApplication::translate("main", "Error: Netlist file does not exist: \"%1\"")
                    .arg(netlistFilePath));
        }

        /* Load the netlist file */
        if (!generateManager->loadNetlist(netlistFilePath)) {
            return showError(
                1,
                QCoreApplication::translate("main", "Error: failed to load netlist file: %1")
                    .arg(netlistFilePath));
        }

        /* Process the netlist */
        if (!generateManager->processNetlist()) {
            return showError(
                1,
                QCoreApplication::translate("main", "Error: failed to process netlist file: %1")
                    .arg(netlistFilePath));
        }

        /* Generate Verilog code */
        const QFileInfo fileInfo(netlistFilePath);
        const QString   outputFileName = fileInfo.baseName();
        if (!generateManager->generateVerilog(outputFileName, parser.isSet("format"))) {
            const QString message
                = parser.isSet("format")
                      ? QCoreApplication::translate(
                            "main", "Error: failed to generate or format Verilog code for: %1")
                      : QCoreApplication::translate(
                            "main", "Error: failed to generate Verilog code for: %1");
            return showError(1, message.arg(outputFileName));
        }

        showInfo(
            0,
            QCoreApplication::translate("main", "Successfully generated Verilog code: %1")
                .arg(QDir(projectManager->getOutputPath()).filePath(outputFileName + ".v")));
    }

    return true;
}

bool QSocCliWorker::parseGenerateTemplate(const QStringList &appArguments)
{
    /* Clear upstream positional arguments and setup subcommand */
    parser.clearPositionalArguments();
    parser.addOptions({
        {{"d", "directory"},
         QCoreApplication::translate("main", "The path to the project directory."),
         "project directory"},
        {{"p", "project"}, QCoreApplication::translate("main", "The project name."), "project name"},
        {"csv",
         QCoreApplication::translate("main", "CSV data file (can be used multiple times)."),
         "csv file"},
        {"yaml",
         QCoreApplication::translate("main", "YAML data file (can be used multiple times)."),
         "yaml file"},
        {"json",
         QCoreApplication::translate("main", "JSON data file (can be used multiple times)."),
         "json file"},
        {"rdl",
         QCoreApplication::translate("main", "SystemRDL data file (can be used multiple times)."),
         "rdl file"},
        {"rcsv",
         QCoreApplication::translate(
             "main", "RCSV (Register-CSV) data file (can be used multiple times)."),
         "rcsv file"},
    });

    parser.addPositionalArgument(
        "templates",
        QCoreApplication::translate("main", "The Jinja2 template files to be processed."),
        "<template.j2> [<template2.j2>...]");

    parser.parse(appArguments);

    if (parser.isSet("help")) {
        return showHelp(0);
    }

    const QStringList  positionalArgs   = parser.positionalArguments();
    const QStringList &templateFileList = positionalArgs;
    if (templateFileList.isEmpty()) {
        return showHelpOrError(
            1, QCoreApplication::translate("main", "Error: missing template files."));
    }

    /* Setup project manager and project path  */
    if (parser.isSet("directory")) {
        const QString dirPath = parser.value("directory");
        projectManager->setProjectPath(dirPath);
    }

    if (parser.isSet("project")) {
        projectManager->load(parser.value("project"));
    } else {
        const QStringList &projectNameList = projectManager->list(QRegularExpression(".*"));
        if (projectNameList.length() > 1) {
            return showErrorWithHelp(
                1,
                QCoreApplication::translate(
                    "main",
                    "Error: multiple projects found, please specify the project name.\n"
                    "Available projects are:\n%1\n")
                    .arg(projectNameList.join("\n")));
        }
        projectManager->loadFirst();
    }

    /* Check if output path is valid */
    if (!projectManager->isValidOutputPath()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid output directory: %1")
                .arg(projectManager->getOutputPath()));
    }

    /* Collect data files */
    QStringList csvFiles;
    QStringList yamlFiles;
    QStringList jsonFiles;
    QStringList rdlFiles;
    QStringList rcsvFiles;

    if (parser.isSet("csv")) {
        csvFiles = parser.values("csv");
    }

    if (parser.isSet("yaml")) {
        yamlFiles = parser.values("yaml");
    }

    if (parser.isSet("json")) {
        jsonFiles = parser.values("json");
    }

    if (parser.isSet("rdl")) {
        rdlFiles = parser.values("rdl");
    }

    if (parser.isSet("rcsv")) {
        rcsvFiles = parser.values("rcsv");
    }

    /* Process each template file */
    for (const QString &templateFilePath : templateFileList) {
        /* Check if the template file exists before trying to load it */
        if (!QFile::exists(templateFilePath)) {
            return showError(
                101,
                QCoreApplication::translate("main", "Error: Template file does not exist: \"%1\"")
                    .arg(templateFilePath));
        }

        /* Process the template */
        const QFileInfo fileInfo(templateFilePath);
        QString         outputFileName = fileInfo.fileName();
        /* Remove only the template extension (the last extension) */
        const int lastDotIndex = static_cast<int>(outputFileName.lastIndexOf('.'));
        if (lastDotIndex > 0) {
            outputFileName = outputFileName.left(lastDotIndex);
        }

        if (!generateManager->renderTemplate(
                templateFilePath,
                csvFiles,
                yamlFiles,
                jsonFiles,
                rdlFiles,
                rcsvFiles,
                outputFileName)) {
            return showError(
                1,
                QCoreApplication::translate("main", "Error: failed to render template: %1")
                    .arg(templateFilePath));
        }

        showInfo(
            0,
            QCoreApplication::translate("main", "Successfully generated file from template: %1")
                .arg(QDir(projectManager->getOutputPath()).filePath(outputFileName)));
    }

    return true;
}

bool QSocCliWorker::parseGenerateStub(const QStringList &appArguments)
{
    /* Clear upstream positional arguments and setup subcommand */
    parser.clearPositionalArguments();
    parser.addOptions({
        {{"d", "directory"},
         QCoreApplication::translate("main", "The path to the project directory."),
         "project directory"},
        {{"p", "project"}, QCoreApplication::translate("main", "The project name."), "project name"},
        {{"l", "library"},
         QCoreApplication::translate("main", "The library base name or regex."),
         "library base name or regex"},
        {{"m", "module"},
         QCoreApplication::translate("main", "The module name or regex."),
         "module name or regex"},
    });

    parser.addPositionalArgument(
        "stubname", QCoreApplication::translate("main", "The stub name to generate."), "<stubname>");

    parser.parse(appArguments);

    if (parser.isSet("help")) {
        return showHelp(0);
    }

    const QStringList positionalArgs = parser.positionalArguments();
    if (positionalArgs.isEmpty()) {
        return showHelpOrError(1, QCoreApplication::translate("main", "Error: missing stub name."));
    }

    const QString &stubName = positionalArgs.first();

    /* Setup project manager and project path  */
    if (parser.isSet("directory")) {
        const QString dirPath = parser.value("directory");
        projectManager->setProjectPath(dirPath);
    }

    if (parser.isSet("project")) {
        projectManager->load(parser.value("project"));
    } else {
        const QStringList &projectNameList = projectManager->list(QRegularExpression(".*"));
        if (projectNameList.length() > 1) {
            return showErrorWithHelp(
                1,
                QCoreApplication::translate(
                    "main",
                    "Error: multiple projects found, please specify the project name.\n"
                    "Available projects are:\n%1\n")
                    .arg(projectNameList.join("\n")));
        }
        projectManager->loadFirst();
    }

    /* Check if output path is valid */
    if (!projectManager->isValidOutputPath()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid output directory: %1")
                .arg(projectManager->getOutputPath()));
    }

    /* Load modules */
    QRegularExpression libraryRegex(".*");
    if (parser.isSet("library")) {
        libraryRegex = QRegularExpression(parser.value("library"));
    }

    if (!moduleManager->load(libraryRegex)) {
        return showErrorWithHelp(
            1, QCoreApplication::translate("main", "Error: could not load library"));
    }

    QRegularExpression moduleRegex(".*");
    if (parser.isSet("module")) {
        moduleRegex = QRegularExpression(parser.value("module"));
    }

    /* Generate stub files */
    if (!generateManager->generateStub(stubName, libraryRegex, moduleRegex)) {
        return showError(
            1,
            QCoreApplication::translate("main", "Error: failed to generate stub files for: %1")
                .arg(stubName));
    }

    showInfo(
        0,
        QCoreApplication::translate("main", "Successfully generated stub files: %1").arg(stubName));

    return true;
}
