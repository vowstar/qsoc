// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"
#include "common/qsocgenerateartifact.h"
#include "common/qsocprcmbinding.h"
#include "common/qsocprcmcomposition.h"
#include "common/qsocprcmdocument.h"
#include "common/qsocprcmformal.h"
#include "common/qsocprcmgenerator.h"
#include "common/qsocprcmmode.h"
#include "common/qsocprcmreader.h"
#include "common/qsocprcmsequencecheck.h"
#include "common/qsocprcmservicecheck.h"
#include "common/qsocprcmshared.h"
#include "common/qsocverilogutils.h"
#include <QTemporaryDir>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <QCoreApplication>

namespace {

QString describe(const QList<QSocPrcmDiagnostic> &diagnostic)
{
    QStringList lines;
    for (const auto &item : diagnostic) {
        lines.append(item.code + ": " + item.message);
        for (const auto &source : item.source) {
            lines.append(QString("  %1:%2:%3 %4")
                             .arg(source.file)
                             .arg(source.line)
                             .arg(source.column)
                             .arg(source.path));
        }
    }
    return lines.join('\n');
}

QString statusName(QSocPrcmCheckStatus status)
{
    switch (status) {
    case QSocPrcmCheckStatus::Sat:
        return "SAT";
    case QSocPrcmCheckStatus::Unsat:
        return "UNSAT";
    case QSocPrcmCheckStatus::Unknown:
        return "UNKNOWN";
    case QSocPrcmCheckStatus::Timeout:
        return "TIMEOUT";
    case QSocPrcmCheckStatus::Cancelled:
        return "CANCELLED";
    case QSocPrcmCheckStatus::Error:
        return "ERROR";
    }
    return "ERROR";
}

bool containsPrcm(const QStringList &files)
{
    for (const auto &path : files) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            continue;
        try {
            const auto text = file.readAll().toStdString();
            const auto has  = [](const YAML::Node &node) {
                return node.IsMap() && node["prcm"].IsDefined();
            };
            if (has(YAML::Load(text)))
                return true;
            for (const auto &node : YAML::LoadAll(text)) {
                if (has(node))
                    return true;
            }
        } catch (const YAML::Exception &) {
            /* The netlist loader reports malformed input. */
        }
    }
    return false;
}

QString softwareName(const QString &name)
{
    QString result;
    for (const auto ch : name) {
        result += ch.isLetterOrNumber()
                      ? QString(ch)
                      : QString("_%1").arg(quint32(ch.unicode()), 2, 16, QLatin1Char('0'));
    }
    return result;
}

QString softwareHeader(
    const QSocPrcmCircuit &circuit,
    const QSocPrcmInput   &input,
    const QString         &moduleName,
    bool                   shared)
{
    const QString prefix = "QSOC_" + softwareName(moduleName) + "_X";
    QStringList   lines{"#pragma once", "#include <stdint.h>", ""};
    auto          define = [&](const QString &name, quint64 value) {
        lines.append(QString("#define %1_%2 UINT64_C(0x%3)").arg(prefix, name).arg(value, 0, 16));
    };
    QMap<QString, QString> registerName;
    if (shared) {
        for (const auto &domain : input.domain.keys()) {
            for (const auto &word : {"REQUEST", "STATUS", "EVENT"})
                registerName.insert(
                    "DOMAIN_" + domain + '_' + word, "DOMAIN_" + softwareName(domain) + '_' + word);
        }
    }
    for (const auto &reg : circuit.mmio.registers) {
        const auto regName = registerName.value(reg.name, reg.name);
        define(regName + "_OFFSET", reg.byteOffset);
        for (const auto &field : reg.fields) {
            const auto name = regName + '_' + softwareName(field.name);
            define(name + "_SHIFT", field.lsb);
            define(name + "_WIDTH", field.width);
            define(name + "_MASK", ((quint64(1) << field.width) - 1) << field.lsb);
        }
    }
    for (auto domain = input.domain.cbegin(); domain != input.domain.cend(); ++domain) {
        const auto qualifier = shared ? "DOMAIN_" + softwareName(domain.key()) + '_' : QString();
        for (auto mode = domain->mode.cbegin(); mode != domain->mode.cend(); ++mode)
            define(qualifier + "MODE_" + softwareName(mode.key()), mode->code);
    }
    if (shared)
        for (auto mode = input.chipMode.cbegin(); mode != input.chipMode.cend(); ++mode)
            define("CHIP_MODE_" + softwareName(mode.key()), mode->code);
    return lines.join('\n') + '\n';
}

QByteArray integrationReport(
    const QSocPrcmBindingPlan     &plan,
    const QSocPrcmCircuit         &circuit,
    bool                           formal,
    const QSocPrcmCompositionPlan *composition)
{
    const auto &input  = plan.input;
    const auto &domain = *input.domain.cbegin();
    QJsonObject root{
        {"version", 1},
        {"clock", input.clockInput},
        {"reset",
         QJsonObject{
             {"source", input.resetSource},
             {"target", input.resetTarget},
             {"stage", *input.resetStage}}},
        {"domain", input.domain.firstKey()},
        {"binding", circuit.binding},
        {"feedback",
         QJsonObject{
             {"power", input.supplyTable[domain.supply].valid.signal},
             {"idle", domain.quiesce.completion.signal},
             {"isolation", domain.isolation.completion.signal},
             {"reset", domain.reset.target}}},
        {"check",
         QJsonObject{
             {"stable_mode", "pass"},
             {"sequence_model", "pass"},
             {"progress_model", "pass"},
             {"rtl", formal ? "not_run" : "not_generated"},
             {"physical", "not_run"}}},
        {"condition",
         QJsonArray{
             "The controller supply and clock remain available during domain shutdown.",
             "Power, idle, and isolation feedback meet the declared sampling clock timing.",
             "Reset pulse width, recovery, removal, and receiver reliability require physical "
             "checks.",
             "Clock gate and reset cell replacements must preserve the control contract.",
             "Binding names refer to the emitted top module. Cell internals and mapped netlist "
             "objects require checks against the actual implementation.",
             "Held requests receive feedback. A stable target is required for progress.",
             "The sequence model excludes management reset and independent reset intervention.",
             "The RTL checks cover power, isolation, and quiesce requests. Bus checks cover "
             "response state and REQUEST, STATUS, and EVENT values with delayed feedback and "
             "sampled power loss.",
             "Management reset preserves bus transactions. Cold reset cancels them."}}};
    if (composition) {
        QJsonObject domains;
        for (auto item = input.domain.cbegin(); item != input.domain.cend(); ++item) {
            domains.insert(
                item.key(),
                QJsonObject{
                    {"power", input.supplyTable[item->supply].valid.signal},
                    {"idle", item->quiesce.completion.signal},
                    {"isolation", item->isolation.completion.signal},
                    {"reset", item->reset.target}});
        }
        root["version"] = 2;
        root["domain"]  = domains;
        root.remove("feedback");
        auto check             = root["check"].toObject();
        check["service_model"] = composition->service.isEmpty() ? "not_required" : "pass";
        check["composition"]   = "one_service_layer";
        root["check"]          = check;
        auto condition         = root["condition"].toArray();
        condition.append(
            "Providers have no service dependency. Consumers request all providers before waiting "
            "for permission.");
        condition.append(
            "A provider follows its local target after the last consumer releases it. Chip policy "
            "must permit each required service.");
        root["condition"] = condition;
    }
    return QJsonDocument(root).toJson();
}

QString checkSequence(const QSocPrcmSequencePlan &plan, const QString &domain)
{
    const auto safety = QSocPrcmSequenceCheck::safety(plan);
    if (safety.status != QSocPrcmCheckStatus::Unsat)
        return "PRCM_SEQUENCE_" + statusName(safety.status) + ": " + domain + ": " + safety.reason;
    const auto progress = QSocPrcmSequenceCheck::progress(plan);
    if (progress.status != QSocPrcmCheckStatus::Unsat)
        return "PRCM_PROGRESS_" + statusName(progress.status) + ": " + domain + ": "
               + progress.reason;
    return {};
}

bool sharedCircuit(const QSocPrcmInput &input)
{
    if (input.domain.size() != 1 || !input.chipMode.isEmpty())
        return true;
    const auto &domain = *input.domain.cbegin();
    return !domain.require.isEmpty() || !domain.service.isEmpty();
}

} // namespace

bool QSocCliWorker::checkPrcmNetlists(const QStringList &filePathList)
{
    QList<QStringList> groups;
    if (parser.isSet("merge")) {
        groups.append(filePathList);
    } else {
        for (const auto &file : filePathList) {
            groups.append(QStringList{file});
        }
    }
    for (const auto &files : groups) {
        const auto loaded = QSocPrcmDocumentLoader::load(files);
        if (!loaded.document) {
            return showError(1, describe(loaded.diagnostic));
        }
        const auto &document = *loaded.document;
        const auto  path     = files.join(", ");
        const auto binding = QSocPrcmBinding::resolve(document.node, document.file, document.origin);
        if (!binding.plan) {
            return showError(1, describe(binding.diagnostic));
        }
        const auto mode = QSocPrcmModeCheck::check(binding.plan->input);
        if (!mode.diagnostic.isEmpty()) {
            return showError(1, describe(mode.diagnostic));
        }
        if (mode.check.isEmpty()) {
            return showError(1, "PRCM_CHECK_EMPTY: No mode query runs for " + path);
        }
        for (const auto &check : mode.check) {
            if (check.result.status != QSocPrcmCheckStatus::Sat) {
                return showError(
                    1,
                    "PRCM_CHECK_" + statusName(check.result.status) + ": " + check.name + ": "
                        + check.result.reason);
            }
        }
        showInfo(
            0,
            QCoreApplication::translate("main", "%1: resource binding and %2 stable mode queries pass.")
                .arg(path)
                .arg(mode.check.size()));
    }
    return true;
}

std::optional<bool> QSocCliWorker::generatePrcmNetlists(const QStringList &files)
{
    if (!containsPrcm(files)) {
        if (parser.isSet("with-formal"))
            return showError(1, "PRCM_REQUIRED: --with-formal requires a PRCM declaration.");
        return std::nullopt;
    }
    const auto loaded = QSocPrcmDocumentLoader::load(files);
    if (!loaded.document)
        return showError(1, describe(loaded.diagnostic));
    const auto &document = *loaded.document;
    try {
        QSocPrcmDetail::Context      context{document.file, {}, document.origin};
        const QSocPrcmDetail::Reader root(document.node, {}, context);
        for (const auto &key : root.keys()) {
            if (key != "prcm" && key != "clock" && key != "reset")
                root.member(key).fail(
                    "PRCM_GENERATE", "This circuit cannot include the " + key + " section yet.");
        }
        for (const auto &name : {"clock", "reset"}) {
            if (root.member(name).size() != 1)
                root.member(name)
                    .fail("PRCM_GENERATE", "This circuit uses one clock and one reset controller.");
        }
        const auto binding = QSocPrcmBinding::resolve(document.node, document.file, document.origin);
        if (!binding.plan)
            return showError(1, describe(binding.diagnostic));
        const auto &plan = *binding.plan;
        if (!plan.input.resetStage)
            root.member("prcm")
                .member("controller")
                .member("reset")
                .member("stage")
                .fail("PRCM_REQUIRED", "Set the reset sampling stage count for circuit generation.");
        const auto mode = QSocPrcmModeCheck::check(plan.input);
        if (!mode.diagnostic.isEmpty())
            return showError(1, describe(mode.diagnostic));
        if (mode.check.isEmpty())
            return showError(1, "PRCM_CHECK_EMPTY: No stable mode query runs.");
        for (const auto &check : mode.check) {
            if (check.result.status != QSocPrcmCheckStatus::Sat)
                return showError(
                    1,
                    "PRCM_CHECK_" + statusName(check.result.status) + ": " + check.name + ": "
                        + check.result.reason);
        }
        std::optional<QSocPrcmCompositionPlan> composition;
        if (sharedCircuit(plan.input)) {
            const auto selected = QSocPrcmComposition::build(plan.input);
            if (!selected.plan)
                return showError(1, describe(selected.diagnostic));
            composition = *selected.plan;
            for (auto domain = composition->domain.cbegin(); domain != composition->domain.cend();
                 ++domain) {
                const auto error = checkSequence(domain.value(), domain.key());
                if (!error.isEmpty())
                    return showError(1, error);
            }
            if (!composition->service.isEmpty()) {
                const auto safety = QSocPrcmServiceCheck::safety();
                if (safety.status != QSocPrcmCheckStatus::Unsat)
                    return showError(
                        1, "PRCM_SERVICE_" + statusName(safety.status) + ": " + safety.reason);
                const auto progress = QSocPrcmServiceCheck::progress();
                if (progress.status != QSocPrcmCheckStatus::Unsat)
                    return showError(
                        1,
                        "PRCM_SERVICE_PROGRESS_" + statusName(progress.status) + ": "
                            + progress.reason);
            }
        } else {
            const auto sequence = QSocPrcmSequencePlanner::build(plan.input);
            if (!sequence.plan)
                return showError(1, describe(sequence.diagnostic));
            const auto error = checkSequence(*sequence.plan, plan.input.domain.firstKey());
            if (!error.isEmpty())
                return showError(1, error);
        }
        const auto name = QFileInfo(files.first()).baseName();
        auto generated  = composition
                              ? QSocPrcmShared::generate(plan, name, *plan.input.resetStage)
                              : QSocPrcmGenerator::generate(plan, name, *plan.input.resetStage);
        if (!generated.circuit)
            return showError(1, describe(generated.diagnostic));
        if (parser.isSet("format")) {
            QTemporaryDir temporary;
            QFile         file(temporary.filePath(name + ".v"));
            if (!temporary.isValid() || !file.open(QIODevice::WriteOnly))
                return showError(1, "PRCM_FORMAT: Cannot create a formatting candidate.");
            const auto text = generated.circuit->rtl[name + ".v"].toUtf8();
            if (file.write(text) != text.size())
                return showError(1, "PRCM_FORMAT: Cannot write the formatting candidate.");
            file.close();
            if (!QSocVerilogUtils::formatFile(file.fileName()) || !file.open(QIODevice::ReadOnly))
                return showError(1, "PRCM_FORMAT: Cannot format the circuit.");
            generated.circuit->rtl[name + ".v"] = QString::fromUtf8(file.readAll());
        }
        const auto &circuit = *generated.circuit;
        const QDir  output(QDir(projectManager->getOutputPath()).filePath(name));
        std::vector<QSocGenerateArtifact::Artifact> artifact;
        QStringList                                 list;
        for (auto rtl = circuit.rtl.cbegin(); rtl != circuit.rtl.cend(); ++rtl) {
            list.append(rtl.key());
            const auto      path = output.filePath("rtl/" + rtl.key());
            const QFileInfo file(path);
            const bool      cell = rtl.key() == "clock_cell.v" || rtl.key() == "reset_cell.v";
            if (cell && file.exists() && !parser.isSet("force")) {
                if (file.isSymLink() || !file.isFile())
                    return showError(1, "PRCM_OUTPUT: Cell output is not a regular file: " + path);
                continue;
            }
            artifact.push_back({path, rtl.value().toUtf8()});
        }
        artifact.push_back(
            {output.filePath("rtl/" + name + ".fl"), (list.join('\n') + '\n').toUtf8()});
        artifact.push_back(
            {output.filePath("include/" + name + ".h"),
             softwareHeader(circuit, plan.input, name, composition.has_value()).toUtf8()});
        artifact.push_back(
            {output.filePath("integration/" + name + ".json"),
             integrationReport(
                 plan, circuit, parser.isSet("with-formal"), composition ? &*composition : nullptr)});
        if (parser.isSet("with-formal")) {
            auto formal
                = composition
                      ? QSocPrcmFormal::generateShared(
                            plan, *composition, circuit, name, *plan.input.resetStage)
                      : QSocPrcmFormal::generate(plan, circuit, name, *plan.input.resetStage);
            QStringList formalList;
            for (const auto &file : circuit.rtl.keys()) {
                formal.sby.replace('\n' + file + '\n', "\n../rtl/" + file + '\n');
                formalList.append("../rtl/" + file);
            }
            artifact.push_back(
                {output.filePath("formal/" + name + "_formal.sv"), formal.systemVerilog.toUtf8()});
            artifact.push_back({output.filePath("formal/check.sby"), formal.sby.toUtf8()});
            formalList.append(name + "_formal.sv");
            artifact.push_back(
                {output.filePath("formal/" + name + "_formal.fl"),
                 (formalList.join('\n') + '\n').toUtf8()});
        }
        const auto error = QSocGenerateArtifact::write(std::move(artifact), true);
        if (!error.isEmpty())
            return showError(1, error);
        return showInfo(0, "Generated PRCM circuit: " + output.path());
    } catch (const QSocPrcmDiagnostic &diagnostic) {
        return showError(1, describe({diagnostic}));
    }
}
