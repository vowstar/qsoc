// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocprcmsequencertl.h"
#include "common/qsocgenerateprimitivefsm.h"
#include "common/qsocprcmsequence.h"

#include <QMap>

namespace {

using Phase  = QSocPrcmPhase;
using Target = QSocPrcmTarget;

const QStringList phaseName{
    "INIT",
    "OFF",
    "POWER",
    "CLOCK",
    "RESET",
    "RELEASE",
    "CONNECT",
    "RESUME",
    "RUN",
    "DRAIN",
    "ISOLATE",
    "STOP",
    "FAULT_RELEASE",
    "FAULT",
    "FAULT_OFF"};

QSocPrcmObservation observation(unsigned value)
{
    return {(value & 8U) != 0, (value & 4U) != 0, (value & 2U) != 0, (value & 1U) != 0};
}

QString condition(unsigned rows, unsigned width = 4)
{
    if (rows == 0)
        return "0";
    if (rows == (1U << (1U << width)) - 1U)
        return "1";
    const unsigned half = 1U << (width - 1);
    const auto     low  = condition(rows & ((1U << half) - 1U), width - 1);
    const auto     high = condition(rows >> half, width - 1);
    if (low == high)
        return low;
    const QStringList signal{"idle_i", "isolation_i", "reset_i", "power_i"};
    const auto        bit = signal[width - 1];
    if (low == "0")
        return high == "1" ? bit : "(" + bit + " && " + high + ")";
    if (high == "0")
        return low == "1" ? "!" + bit : "(!" + bit + " && " + low + ")";
    if (low == "1")
        return "(!" + bit + " || " + high + ")";
    if (high == "1")
        return "(" + bit + " || " + low + ")";
    return "((" + bit + " && " + high + ") || (!" + bit + " && " + low + "))";
}

YAML::Node transitions(Phase phase)
{
    QMap<Phase, QStringList> edge;
    const QStringList        select{"target_off_i", "target_reset_i", "target_run_i"};
    for (auto target : {Target::Off, Target::Reset, Target::Run}) {
        QMap<Phase, unsigned> rows;
        for (unsigned value = 0; value < 16; ++value) {
            const auto next = QSocPrcmSequence::step({phase, target}, target, observation(value));
            if (next.state.phase != phase)
                rows[next.state.phase] |= 1U << value;
        }
        for (auto next = rows.cbegin(); next != rows.cend(); ++next) {
            const auto feedback   = condition(next.value());
            const auto targetPort = select[static_cast<int>(target)];
            edge[next.key()].append(
                feedback == "1" ? targetPort : "(" + targetPort + " && " + feedback + ")");
        }
    }
    YAML::Node result(YAML::NodeType::Sequence);
    for (auto next = edge.cbegin(); next != edge.cend(); ++next) {
        YAML::Node item;
        item["cond"] = next.value().join(" || ").toStdString();
        item["next"] = phaseName[static_cast<int>(next.key())].toStdString();
        result.push_back(item);
    }
    return result;
}

} // namespace

QString QSocPrcmSequenceRtl::generate()
{
    YAML::Node node;
    node["name"]      = "qsoc_prcm_domain";
    node["clk"]       = "clk_i";
    node["rst"]       = "rst_ni";
    node["rst_state"] = "INIT";
    for (qsizetype i = 0; i < phaseName.size(); ++i) {
        const auto                  phase = static_cast<Phase>(i);
        const auto                  name  = phaseName[i].toStdString();
        const QSocPrcmSequenceState state{phase, Target::Off};
        const auto                  control = QSocPrcmSequence::control(state);
        node["trans"][name]                 = transitions(phase);
        auto                      output    = node["moore"][name];
        const QMap<QString, bool> value{
            {"power_o", control.power},
            {"clock_o", control.clock},
            {"reset_o", control.reset},
            {"isolation_o", control.isolation},
            {"quiesce_o", control.quiesce},
            {"state_off_o", phase == Phase::Off},
            {"state_reset_o", phase == Phase::Reset},
            {"state_run_o", phase == Phase::Run},
            {"fault_o", QSocPrcmSequence::fault(state)},
            {"power_watch_o", QSocPrcmSequence::step(state, Target::Off, {}).powerLost}};
        for (auto field = value.cbegin(); field != value.cend(); ++field)
            output[field.key().toStdString()] = field.value() ? "1" : "0";
    }
    QString          rtl;
    QTextStream      stream(&rtl);
    QSocFSMPrimitive generator;
    return generator.generateFSMVerilog(node, stream) ? rtl : QString();
}
