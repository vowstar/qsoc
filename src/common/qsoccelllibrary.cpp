// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsoccelllibrary.h"
#include "common/qsoccellformal.h"
#include "common/qsocgenerateartifact.h"
#include "common/qsocgenerateprimitiveclock.h"
#include "common/qsocgenerateprimitivepower.h"
#include "common/qsocgenerateprimitivereset.h"

#include <algorithm>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("main", text);
}

QString writeCells(const QString &outputDirectory, const QSocCellBinding &binding)
{
    const QDir                                  output(outputDirectory);
    std::vector<QSocGenerateArtifact::Artifact> artifacts;
    QByteArray                                  list;
    /* A cell file first, so the artifact root is the unit directory */
    QList<QSocCellLibrary::Cell> cells = QSocCellLibrary::cells(binding);
    std::stable_partition(cells.begin(), cells.end(), [](const QSocCellLibrary::Cell &cell) {
        return !QSocCellLibrary::isRole(cell.file);
    });
    for (const QSocCellLibrary::Cell &cell : cells)
        artifacts.push_back({output.filePath(QSocCellLibrary::path(cell.file)), cell.text.toUtf8()});
    for (const QSocCellLibrary::Cell &cell : QSocCellLibrary::cells(binding))
        list += QSocCellLibrary::path(cell.file).toUtf8() + '\n';
    artifacts.push_back(
        {output.filePath(QSocCellLibrary::unit() + "/rtl/" + QSocCellLibrary::unit() + ".fl"),
         list});
    QByteArray models;
    for (const QSocCellLibrary::Cell &model : QSocCellLibrary::models(binding)) {
        const QString path = QSocCellLibrary::unit() + "/model/" + model.file;
        artifacts.push_back({output.filePath(path), model.text.toUtf8()});
        models += path.toUtf8() + '\n';
    }
    if (!models.isEmpty()) {
        artifacts.push_back(
            {output.filePath(
                 QSocCellLibrary::unit() + "/model/" + QSocCellLibrary::unit() + "_model.fl"),
             models});
    }
    if (binding.isAsic()) {
        artifacts.push_back(
            {output.filePath(QSocCellLibrary::unit() + "/" + QSocCellLibrary::unit() + "_role.rpt"),
             binding.report().toUtf8()});
    }
    return QSocGenerateArtifact::write(std::move(artifacts), true, outputDirectory);
}

/* A role file: one module with its frozen ports, and a generic or asic body. */
QSocCellLibrary::Cell role(
    const QSocCellBinding &binding,
    const QString         &name,
    const QString         &brief,
    const QString         &ports,
    const QString         &body)
{
    if (binding.isAsic()) {
        return {
            name + ".v",
            QString(
                "/**\n"
                " * @file %1.v\n"
                " * @brief %2\n"
                " * @details %3\n"
                " */\n\n"
                "`timescale 1ns / 1ps\n\n"
                "(* keep_hierarchy = \"yes\" *)\n"
                "module %1%4endmodule\n")
                .arg(name, brief, binding.detail(name), ports + binding.body(name))};
    }
    return {
        name + ".v",
        QString(
            "/**\n"
            " * @file %1.v\n"
            " * @brief %2\n"
            " * @details Generic behavioral body. A declared technology cell replaces it\n"
            " *          with one instance named u_cell.\n"
            " */\n\n"
            "`timescale 1ns / 1ps\n\n"
            "module %1%3endmodule\n")
            .arg(name, brief, ports + body)};
}

const char *const syncPorts = R"v( #(
    parameter integer STAGES      = 2,    /**< Flop stages, at least 1 */
    parameter [0:0]   RESET_VALUE = 1'b0  /**< Every stage in reset */
) (
    input  wire clk,    /**< Destination clock */
    input  wire rst_n,  /**< Asynchronous reset, active low */
    input  wire d,      /**< Input from another clock domain */
    output wire q       /**< d after STAGES rising edges of clk */
);
    /* Elaboration fails when STAGES is below 1. */
    generate
        if (STAGES < 1) begin : g_bad_stages
            qsoc_param_error_stages_below_one u_error ();
        end
    endgenerate
)v";

const char *const syncBody = R"v(
    /* The last stage may drive asynchronous resets */
    /* verilator lint_off SYNCASYNCNET */
    reg [STAGES-1:0] chain;
    /* verilator lint_on SYNCASYNCNET */

    generate
        if (STAGES == 1) begin : g_one
            always @(posedge clk or negedge rst_n) begin
                if (!rst_n) chain <= RESET_VALUE;
                else        chain <= d;
            end
        end else begin : g_many
            always @(posedge clk or negedge rst_n) begin
                if (!rst_n) chain <= {STAGES{RESET_VALUE}};
                else        chain <= {chain[STAGES-2:0], d};
            end
        end
    endgenerate

    assign q = chain[STAGES-1];
)v";

const char *const gatePorts = R"v( (
    input  wire clk,      /**< Clock input */
    input  wire en,       /**< Enable, latched while clk is at its idle level */
    input  wire test_en,  /**< Scan enable, ORed with en */
    output wire clk_out   /**< Gated clock */
);
)v";

const char *const gateLatch = R"v(    reg iq;
`ifndef SYNTHESIS
    initial iq = 1'b0;  /* sim-only init to block X fanout */
`endif
    /* Level-sensitive latch, use blocking '=' here */
    always @(clk or en or test_en) begin
)v";

const char *const unaryPorts = R"v( (
    input  wire clk_in,   /**< Clock input */
    output wire clk_out   /**< Clock output */
);
)v";

const char *const binaryPorts = R"v( (
    input  wire clk_in0,  /**< Clock input 0 */
    input  wire clk_in1,  /**< Clock input 1 */
    output wire clk_out   /**< Clock output */
);
)v";

/* Unit lists relative to output: qsoc_cell, then <u> and <lib>/<module> in name order. */
QStringList unitLists(const QDir &output)
{
    const auto hasList = [&output](const QString &unit) {
        return QFileInfo(output.filePath(unit + "/rtl/" + QFileInfo(unit).fileName() + ".fl"))
            .isFile();
    };
    QStringList units;
    for (const QString &name : output.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (hasList(name)) {
            units.append(name);
            continue;
        }
        const QDir library(output.filePath(name));
        for (const QString &module :
             library.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            if (hasList(name + "/" + module))
                units.append(name + "/" + module);
        }
    }
    units.sort();
    units.removeAll(QSocCellLibrary::unit());
    if (hasList(QSocCellLibrary::unit()))
        units.prepend(QSocCellLibrary::unit());
    QStringList lists;
    for (const QString &unit : units)
        lists.append(unit + "/rtl/" + QFileInfo(unit).fileName() + ".fl");
    return lists;
}

/* A plain relative file path that stays inside output. */
bool plainEntry(const QString &entry)
{
    const QString clean = QDir::cleanPath(entry);
    return !entry.isEmpty() && !entry.startsWith('+') && !entry.startsWith('-')
           && !entry.contains('\r') && QDir::isRelativePath(entry) && clean != ".."
           && !clean.startsWith("../");
}

class TopList
{
public:
    explicit TopList(const QDir &output)
        : output(output)
    {}

    QString add(const QString &list)
    {
        QFile file(output.filePath(list));
        if (!file.open(QIODevice::ReadOnly))
            return tr("Error: cannot read file list: %1").arg(list);
        QStringList lines = QString::fromUtf8(file.readAll()).split('\n');
        if (lines.last().isEmpty())
            lines.removeLast();
        for (const QString &entry : lines) {
            const QString error = addEntry(list, entry);
            if (!error.isEmpty())
                return error;
        }
        return {};
    }

    QByteArray text() const
    {
        return entries.join('\n').toUtf8() + (entries.isEmpty() ? "" : "\n");
    }

private:
    QString addEntry(const QString &list, const QString &entry)
    {
        if (!plainEntry(entry))
            return tr("Error: %1 lists an entry that is not a plain relative file: %2")
                .arg(list, entry);
        QFile file(output.filePath(entry));
        if (!file.open(QIODevice::ReadOnly))
            return tr("Error: %1 lists a missing file: %2").arg(list, entry);
        static const QRegularExpression
            module("^\\s*module\\s+([A-Za-z_][A-Za-z_0-9$]*)", QRegularExpression::MultilineOption);
        auto match = module.globalMatch(QString::fromUtf8(file.readAll()));
        while (match.hasNext()) {
            const QString name = match.next().captured(1);
            if (owner.contains(name))
                return tr("Error: module %1 is defined in both %2 and %3")
                    .arg(name, owner.value(name), entry);
            owner.insert(name, entry);
        }
        entries.append(entry);
        return {};
    }

    const QDir             output;
    QStringList            entries;
    QMap<QString, QString> owner;
};

} // namespace

QList<QSocCellLibrary::Cell> QSocCellLibrary::roles(const QSocCellBinding &binding)
{
    return {
        role(binding, "qsoc_ck_buf", "Clock buffer role.", unaryPorts, "    assign clk_out = clk_in;\n"),
        role(
            binding,
            "qsoc_ck_inv",
            "Clock inverter role.",
            unaryPorts,
            "    assign clk_out = ~clk_in;\n"),
        role(
            binding,
            "qsoc_ck_or2",
            "Two-input clock OR role.",
            binaryPorts,
            "    assign clk_out = clk_in0 | clk_in1;\n"),
        role(
            binding,
            "qsoc_ck_xor2",
            "Two-input clock XOR role.",
            binaryPorts,
            "    assign clk_out = clk_in0 ^ clk_in1;\n"),
        role(
            binding,
            "qsoc_ck_mux2",
            "Two-input clock multiplexer role.",
            " (\n"
            "    input  wire clk_in0,  /**< Selected when clk_sel is 0 */\n"
            "    input  wire clk_in1,  /**< Selected when clk_sel is 1 */\n"
            "    input  wire clk_sel,  /**< Select */\n"
            "    output wire clk_out   /**< Selected clock */\n"
            ");\n",
            "    assign clk_out = clk_sel ? clk_in1 : clk_in0;\n"),
        role(
            binding,
            "qsoc_ck_icg_pos",
            "Clock gate role: latch while clk is low, output low while disabled.",
            gatePorts,
            gateLatch
                + QString(
                    "        if (!clk) iq = (test_en | en);\n"
                    "    end\n"
                    "    assign clk_out = iq & clk;\n")),
        role(
            binding,
            "qsoc_ck_icg_neg",
            "Clock gate role: latch while clk is high, output high while disabled.",
            gatePorts,
            gateLatch
                + QString(
                    "        if (clk) iq = ~(test_en | en);\n"
                    "    end\n"
                    "    assign clk_out = iq | clk;\n")),
        role(
            binding,
            "qsoc_sync",
            "Synchronizer role: a flop chain with asynchronous reset.",
            syncPorts,
            syncBody),
    };
}

QList<QSocCellLibrary::Cell> QSocCellLibrary::cells(const QSocCellBinding &binding)
{
    return roles(binding)
           + QList<Cell>{
               {clockFile(), QSocClockPrimitive().generateCellVerilog()},
               {resetFile(), QSocResetPrimitive().generateCellVerilog()},
               {powerFile(), QSocPowerPrimitive().generateCellVerilog()},
           };
}

bool QSocCellLibrary::isRole(const QString &file)
{
    return file == QStringLiteral("qsoc_sync.v") || file.startsWith(QStringLiteral("qsoc_ck_"));
}

QString QSocCellLibrary::path(const QString &file)
{
    return unit() + (isRole(file) ? "/rtl/role/" : "/rtl/") + file;
}

bool QSocCellLibrary::isReserved(const QString &name)
{
    return name.compare(QStringLiteral("qsoc"), Qt::CaseInsensitive) == 0
           || name.startsWith(QStringLiteral("qsoc_"), Qt::CaseInsensitive);
}

QString QSocCellLibrary::writeFileList(const QString &outputDirectory)
{
    const QDir output(outputDirectory);
    TopList    top(output);
    for (const QString &list : unitLists(output)) {
        const QString error = top.add(list);
        if (!error.isEmpty())
            return error;
    }
    const QByteArray text = top.text();
    QSaveFile        file(output.filePath("qsoc.fl"));
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(text) != text.size() || !file.commit())
        return tr("Error: could not write file list: %1").arg(file.fileName());
    return {};
}

QList<QSocCellLibrary::Cell> QSocCellLibrary::models(const QSocCellBinding &binding)
{
    QList<Cell> result;
    for (const QSocCellBinding::Cell &cell : binding.cells())
        result.append({cell.name + ".v", QSocCellBinding::model(cell)});
    return result;
}

QString QSocCellLibrary::formalPath(const QString &file, const QSocCellBinding &binding)
{
    return binding.isAsic() && isRole(file) ? unit() + "/formal/role/" + file : path(file);
}

QString QSocCellLibrary::publish(const QString &outputDirectory, const QSocCellBinding &binding)
{
    const QString error = writeCells(outputDirectory, binding);
    return error.isEmpty() ? writeFileList(outputDirectory) : error;
}

QString QSocCellLibrary::publishFormal(const QString &outputDirectory, const QSocCellBinding &binding)
{
    const QDir                                  output(outputDirectory);
    const QDir                                  formal(output.filePath(unit() + "/formal"));
    std::vector<QSocGenerateArtifact::Artifact> artifacts;
    std::vector<QSocGenerateArtifact::Artifact> references;
    QStringList                                 sources;
    QByteArray                                  list;
    for (const Cell &cell : cells()) {
        const QString source = formalPath(cell.file, binding);
        if (source != path(cell.file))
            references.push_back({output.filePath(source), cell.text.toUtf8()});
        sources.append(formal.relativeFilePath(output.filePath(source)));
        list += source.toUtf8() + '\n';
    }
    const QMap<QString, QString> files = QSocCellFormal::generate(sources);
    for (auto file = files.cbegin(); file != files.cend(); ++file) {
        artifacts.push_back({formal.filePath(file.key()), file.value().toUtf8()});
        if (file.key().endsWith(".sv"))
            list += (unit() + "/formal/" + file.key()).toUtf8() + '\n';
    }
    artifacts.push_back({formal.filePath(unit() + "_formal.fl"), list});
    artifacts.insert(artifacts.end(), references.begin(), references.end());
    if (binding.isAsic() && !binding.roles().isEmpty()) {
        const QDir             contract(formal.filePath("contract"));
        QMap<QString, QString> generic;
        QStringList            contractSources;
        QByteArray             contractList;
        for (const Cell &role : roles()) {
            const QString name = QFileInfo(role.file).completeBaseName();
            if (!binding.roles().contains(name))
                continue;
            generic.insert(name, role.text);
            contractSources.append(contract.relativeFilePath(output.filePath(path(role.file))));
            contractList += path(role.file).toUtf8() + '\n';
        }
        for (const Cell &model : models(binding)) {
            const QString modelPath = unit() + "/model/" + model.file;
            contractSources.append(contract.relativeFilePath(output.filePath(modelPath)));
            contractList += modelPath.toUtf8() + '\n';
        }
        const QMap<QString, QString> checks = QSocCellFormal::contracts(generic, contractSources);
        for (auto file = checks.cbegin(); file != checks.cend(); ++file) {
            artifacts.push_back({contract.filePath(file.key()), file.value().toUtf8()});
            if (file.key().endsWith(".sv"))
                contractList += (unit() + "/formal/contract/" + file.key()).toUtf8() + '\n';
        }
        artifacts.push_back({contract.filePath(unit() + "_contract.fl"), contractList});
    }
    return QSocGenerateArtifact::write(std::move(artifacts), true, outputDirectory);
}
