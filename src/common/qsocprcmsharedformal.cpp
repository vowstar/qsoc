// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocmmioformal.h"
#include "common/qsocprcmcomposition.h"
#include "common/qsocprcmformal.h"
#include "common/qsocprcmgenerator.h"

#include <bit>
#include <QJsonArray>
#include <QRegularExpression>
#include <QTextStream>

namespace {

QString referenceModel()
{
    return QStringLiteral(R"sv(
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

module action_contract (
    input wire clk,
    input wire cold_n,
    input wire [1:0] target,
    input wire power_valid,
    input wire reset_active,
    input wire isolation_active,
    input wire idle,
    input wire service_fault,
    output wire power_request,
    output wire clock_request,
    output wire reset_request,
    output wire isolation_request,
    output wire quiesce_request,
    output wire state_off,
    output wire state_reset,
    output wire state_run,
    output wire fault,
    output wire watch
);
localparam INIT=0, OFF=1, POWER=2, CLOCK=3, HELD=4, RELEASE=5,
           CONNECT=6, RESUME=7, RUN=8, DRAIN=9, ISOLATE=10, STOP=11,
           FAULT_RELEASE=12, FAULT=13, FAULT_OFF=14, FAULT_POWER=15;
reg [3:0] phase=INIT, next_phase;
wire protected_domain = reset_active && isolation_active;
assign state_off = phase == OFF;
assign state_reset = phase == HELD;
assign state_run = phase == RUN;
assign fault = phase >= FAULT_RELEASE;
assign watch = phase >= CLOCK && phase <= STOP;
assign power_request = phase != INIT && phase != OFF && phase != FAULT_OFF;
assign clock_request = phase >= CLOCK && phase <= ISOLATE;
assign reset_request = !(phase >= RELEASE && phase <= ISOLATE);
assign isolation_request = phase != CONNECT && phase != RESUME && phase != RUN && phase != DRAIN;
assign quiesce_request = phase != RESUME && phase != RUN && phase != FAULT_RELEASE;
always @(posedge clk or negedge cold_n) begin
    if (!cold_n) phase <= INIT;
    else phase <= next_phase;
end
always @* begin
    next_phase = phase;
    if (service_fault && !fault) begin
        if (phase == INIT || phase == OFF) next_phase = FAULT_OFF;
        else if (phase == POWER) next_phase = FAULT_POWER;
        else next_phase = !quiesce_request && idle ? FAULT_RELEASE : FAULT;
    end else if (watch && !power_valid) begin
        next_phase = !quiesce_request && idle ? FAULT_RELEASE : FAULT;
    end else case (phase)
        INIT: next_phase = OFF;
        OFF: if (target != 0 && !power_valid && protected_domain) next_phase = POWER;
        POWER: if (power_valid && protected_domain) next_phase = target == 0 ? OFF : CLOCK;
        CLOCK, HELD: if (protected_domain)
            case (target)
                0: next_phase = STOP;
                1: next_phase = HELD;
                2: next_phase = RELEASE;
            endcase
        RELEASE: if (!reset_active && isolation_active) next_phase = target == 2 ? CONNECT : HELD;
        CONNECT: if (!isolation_active && idle) next_phase = target == 2 ? RESUME : ISOLATE;
        RESUME: if (!idle) next_phase = target == 2 ? RUN : DRAIN;
        RUN: if (target != 2) next_phase = DRAIN;
        DRAIN: if (idle) next_phase = target == 2 ? RESUME : ISOLATE;
        ISOLATE: if (isolation_active) next_phase = target == 2 ? CONNECT : HELD;
        STOP: if (protected_domain) next_phase = target == 0 ? OFF : CLOCK;
        FAULT_RELEASE: if (!idle) next_phase = FAULT;
        FAULT: if (protected_domain && idle) next_phase = FAULT_OFF;
        FAULT_OFF: if (target == 0 && !power_valid && protected_domain) next_phase = OFF;
        FAULT_POWER: if (power_valid) next_phase = FAULT;
    endcase
end
endmodule

// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

module service_contract (
    input wire clk,
    input wire cold_n,
    input wire need,
    input wire available,
    input wire provider_fault,
    input wire consumer_fault,
    input wire released,
    output wire request,
    output wire hold,
    output reg grant=0,
    output wire failure,
    output wire permission
);
localparam IDLE=0, WAIT=1, HOLD=2, RETURN=3;
reg [1:0] phase=IDLE;
assign request = phase == WAIT || phase == HOLD;
assign hold = phase == HOLD;
assign failure = request && (hold ? !grant || !available : provider_fault);
assign permission = hold && grant && available && !provider_fault;
always @(posedge clk or negedge cold_n) begin
    if (!cold_n) begin
        phase <= IDLE;
        grant <= 0;
    end else begin
        grant <= request && available;
        case (phase)
            IDLE: if (need && !consumer_fault) phase <= WAIT;
            WAIT: if (provider_fault) phase <= RETURN;
                  else if (grant) phase <= HOLD;
            HOLD: if ((!need || consumer_fault || failure) && released) phase <= RETURN;
            RETURN: if (!grant) phase <= IDLE;
        endcase
    end
end
endmodule
)sv");
}

QString either(const QStringList &term)
{
    return term.isEmpty() ? "1'b0" : '(' + term.join(" || ") + ')';
}

struct Domain
{
    QString          name, model, actual;
    quint32          width   = 1;
    quint64          request = 0, status = 0, event = 0;
    QList<qsizetype> use, serve;
};

class Proof
{
public:
    Proof(
        const QSocPrcmBindingPlan     &binding,
        const QSocPrcmCircuit         &circuit,
        const QSocPrcmCompositionPlan &plan,
        const QString                 &name,
        int                            stage)
        : binding(binding)
        , input(binding.input)
        , circuit(circuit)
        , plan(plan)
        , name(name)
        , stage(stage)
    {}

    QSocMmioFormalCollateral generate()
    {
        const auto original = circuit.rtl.value(name + ".v");
        while (original.contains(prefix))
            prefix += '_';
        qsizetype  offset  = plan.chip.isEmpty() ? 0 : 2;
        const auto domains = circuit.binding["domain"].toObject();
        for (auto it = plan.domain.cbegin(); it != plan.domain.cend(); ++it) {
            Domain d;
            d.name    = it.key();
            d.model   = prefix + "d" + QString::number(node.size()) + '_';
            d.actual  = domains[it.key()].toObject()["prefix"].toString();
            d.width   = qMax(1U, unsigned(std::bit_width(it->mode.lastKey())));
            d.request = offset++ * (input.dataWidth / 8);
            d.status  = offset++ * (input.dataWidth / 8);
            d.event   = offset++ * (input.dataWidth / 8);
            node.insert(it.key(), d);
        }
        for (qsizetype i = 0; i < plan.service.size(); ++i) {
            node[plan.service[i].consumer].use.append(i);
            node[plan.service[i].provider].serve.append(i);
        }
        QTextStream out(&body);
        out << "\nlocalparam @D@=" << input.dataWidth << ", @A@=" << input.addressWidth
            << ", @STAGE@=" << stage << ";\nwire " << prefix << "clk=" << input.clockInput << ";\n";
        startup(out);
        if (!plan.chip.isEmpty())
            chip(out);
        for (const auto &d : node)
            domain(out, d);
        for (qsizetype i = 0; i < plan.service.size(); ++i)
            service(out, i);
        if (!plan.chip.isEmpty())
            chipStatus(out);
        registerRead(out);
        bus(out);
        reachability(out);
        out.flush();
        QString top = original;
        top.replace(
            QRegularExpression("module\\s+" + QRegularExpression::escape(name) + "(?=\\s*\\()"),
            "module " + name + "_formal #(parameter @FAULT@=0, @COVER@=0)");
        top.replace(QRegularExpression("(?m)^[\t ]*endmodule[\t ]*$"), body + "\nendmodule");
        auto model = referenceModel();
        model.replace("action_contract", prefix + "action_contract");
        model.replace("service_contract", prefix + "service_contract");
        QSocMmioFormalCollateral result;
        result.systemVerilog    = top + '\n' + model;
        const auto rtl          = circuit.rtl.keys();
        const auto file         = name + "_formal.sv";
        int        releaseStage = stage;
        for (const auto &target : binding.reset.targets)
            releaseStage = qMax(releaseStage, target.async.stage);
        const qsizetype depth = 160 + 16 * node.size() + 8 * qsizetype(releaseStage);
        result.sby
            = "[tasks]\nnormal prove\nfault prove error\ncover reach\ncover_fault reach error\n"
              "[options]\nprove: mode prove\nreach: mode cover\nmulticlock on\n"
              "timeout 180\nprove: aigsmt z3\nreach: depth "
              + QString::number(depth)
              + "\n[engines]\nprove: abc pdr\nreach: btor btormc\n"
                "[script]\nread -sv -noautowire "
              + rtl.join(' ') + "\nread -formal -noautowire " + file
              + "\nerror: chparam -set @FAULT@ 1 " + name + "_formal"
              + "\nreach: chparam -set @COVER@ 1 " + name + "_formal" + "\nprep -top " + name
              + "_formal -flatten\ncheck -assert\n[files]\n" + rtl.join('\n') + '\n' + file + '\n';
        const QMap<QString, QString> token{
            {"@D@", prefix + "data_width"},
            {"@A@", prefix + "address_width"},
            {"@STAGE@", prefix + "sample_stage"},
            {"@FAULT@", prefix + "fault_case"},
            {"@COVER@", prefix + "cover_case"},
            {"@lane@", prefix + "lane"}};
        for (auto it = token.cbegin(); it != token.cend(); ++it) {
            result.systemVerilog.replace(it.key(), it.value());
            result.sby.replace(it.key(), it.value());
        }
        return result;
    }

private:
    QString serviceName(qsizetype i) const { return prefix + "s" + QString::number(i) + '_'; }
    QString activeReset(const QString &name) const
    {
        const auto d = input.domain[name];
        return (binding.domain[name].resetTargetActiveLow ? "!" : "") + d.reset.target;
    }
    void startup(QTextStream &out)
    {
        QString management = "1'b0";
        for (const auto &target : binding.reset.targets)
            if (target.name == input.resetTarget)
                management = (target.active == "low" ? "!" : "") + target.name;
        out << "initial assume(!" << input.resetSource << ");\n"
            << "always @($global_clock) begin\n"
            << " if($initstate) assume(!" << input.clockInput << ");\n"
            << " else assume(" << input.clockInput << " != $past(" << input.clockInput
            << "));\nend\n"
            << "reg [@STAGE@-1:0] " << prefix << "awake=0;\n"
            << "always @(posedge " << prefix << "clk or negedge " << input.resetSource << ")\n"
            << " if(!" << input.resetSource << ") " << prefix << "awake<=0;\n"
            << " else " << prefix << "awake<={" << prefix << "awake[@STAGE@-2:0],1'b1};\n"
            << "wire " << prefix << "ready=" << prefix << "awake[@STAGE@-1];\n"
            << "reg [@STAGE@-1:0] " << prefix << "clear_sample='1;\n"
            << "always @(posedge " << prefix << "clk or negedge " << prefix << "ready)\n"
            << " if(!" << prefix << "ready) " << prefix << "clear_sample<='1;\n"
            << " else " << prefix << "clear_sample<={" << prefix << "clear_sample[@STAGE@-2:0],"
            << management << "};\n"
            << "wire " << prefix << "clear=" << prefix << "clear_sample[@STAGE@-1];\n"
            << "reg " << prefix << "history=0;\n"
            << "always @(posedge " << prefix << "clk or negedge " << prefix << "ready)\n"
            << " if(!" << prefix << "ready) " << prefix << "history<=0; else " << prefix
            << "history<=1;\n"
            << "wire [@D@-1:0] " << prefix << "byte_mask;\n"
            << "for(genvar @lane@=0;@lane@<@D@/8;@lane@=@lane@+1)\n"
            << " assign " << prefix << "byte_mask[8*@lane@+:8]={8{" << prefix
            << "strobe[@lane@]}};\n";
    }
    void request(QTextStream &out, const QString &p, quint32 width, quint64 address, quint64 initial)
    {
        out << "reg [" << width - 1 << ":0] " << p << "mode=" << width << "'d" << initial << ";\n"
            << "always @(posedge " << prefix << "clk or negedge " << prefix << "ready)\n"
            << " if(!" << prefix << "ready) " << p << "mode<=" << width << "'d" << initial << ";\n"
            << " else if(" << prefix << "clear) " << p << "mode<=" << width << "'d" << initial
            << ";\n"
            << " else if(" << prefix << "store && " << prefix
            << "write_address==" << input.addressWidth << "'d" << address << ")\n"
            << "  " << p << "mode<= (" << p << "mode & ~" << prefix << "byte_mask[" << width - 1
            << ":0]) | (" << prefix << "data[" << width - 1 << ":0] & " << prefix << "byte_mask["
            << width - 1 << ":0]);\n";
        read.insert(address, p + "mode");
    }
    void chip(QTextStream &out)
    {
        chipWidth    = qMax(1U, unsigned(std::bit_width(plan.chip.lastKey())));
        const auto p = prefix + "chip_";
        request(out, p, chipWidth, 0, plan.resetCode);
        QStringList valid;
        for (auto it = plan.chip.cbegin(); it != plan.chip.cend(); ++it)
            valid.append(
                p + "mode == " + QString::number(chipWidth) + "'d" + QString::number(it.key()));
        out << "wire " << p << "valid=" << either(valid) << ";\n"
            << "reg [" << chipWidth - 1 << ":0] " << p << "last=" << chipWidth << "'d"
            << plan.resetCode << ";\n"
            << "always @(posedge " << prefix << "clk or negedge " << prefix << "ready)\n"
            << " if(!" << prefix << "ready) " << p << "last<=" << chipWidth << "'d"
            << plan.resetCode << ";\n"
            << " else if(" << p << "valid) " << p << "last<=" << p << "mode;\n"
            << "wire [" << chipWidth - 1 << ":0] " << p << "target=" << p << "valid ? " << p
            << "mode : " << p << "last;\n";
    }
    void domain(QTextStream &out, const Domain &d);
    void physical(QTextStream &out, const Domain &d);
    void service(QTextStream &out, qsizetype i);
    void chipStatus(QTextStream &out);
    void registerRead(QTextStream &out);
    void bus(QTextStream &out);
    void reachability(QTextStream &out);

    const QSocPrcmBindingPlan     &binding;
    const QSocPrcmInput           &input;
    const QSocPrcmCircuit         &circuit;
    const QSocPrcmCompositionPlan &plan;
    QString                        name, body, prefix = "proof_";
    int                            stage;
    quint32                        chipWidth = 0;
    QMap<QString, Domain>          node;
    QMap<quint64, QString>         read;
};

} // namespace

namespace {
void Proof::domain(QTextStream &out, const Domain &d)
{
    const auto &p      = d.model;
    const auto &mode   = plan.domain[d.name];
    const auto &config = input.domain[d.name];
    const auto &supply = input.supplyTable[config.supply];
    request(out, p, d.width, d.request, mode.resetCode);
    out << "function [1:0] " << p << "decode(input [" << d.width - 1
        << ":0] value);\ncase(value)\n";
    for (auto it = mode.mode.cbegin(); it != mode.mode.cend(); ++it)
        out << d.width << "'d" << it.key() << ": " << p << "decode=2'd" << int(it.value()) << ";\n";
    out << "default: " << p << "decode=3;\nendcase\nendfunction\n"
        << "wire [1:0] " << p << "decoded=" << p << "decode(" << p << "mode);\n"
        << "wire " << p << "valid=" << p << "decoded!=3;\n"
        << "reg [1:0] " << p << "last=0;\n"
        << "always @(posedge " << prefix << "clk or negedge " << prefix << "ready)\n"
        << " if(!" << prefix << "ready) " << p << "last<=0;\n"
        << " else if(" << p << "valid) " << p << "last<=" << p << "decoded;\n"
        << "wire [1:0] " << p << "desired=" << p << "valid ? " << p << "decoded : " << p
        << "last;\n"
        << "reg [1:0] " << p << "base;\nalways @* begin\n " << p << "base=" << p << "desired;\n";
    if (!plan.chip.isEmpty()) {
        out << "case(" << prefix << "chip_target)\n";
        for (auto it = plan.chip.cbegin(); it != plan.chip.cend(); ++it) {
            const auto value = it.value()[d.name];
            if (value)
                out << chipWidth << "'d" << it.key() << ": " << p << "base=2'd" << int(*value)
                    << ";\n";
        }
        out << "default: begin end\nendcase\n";
    }
    QStringList incoming, missing, failure;
    for (auto i : d.serve)
        incoming.append(serviceName(i) + "request");
    for (auto i : d.use) {
        missing.append('!' + serviceName(i) + "permission");
        failure.append(serviceName(i) + "failure");
    }
    out << "end\nwire " << p << "in_use=" << either(incoming) << ";\n"
        << "wire " << p << "service_failure=" << either(failure) << ";\n"
        << "wire [1:0] " << p << "wanted=" << p << "in_use ? 2'd2 : " << p << "base;\n"
        << "wire " << p << "wait_service=" << p << "wanted==2 && " << either(missing) << ";\n"
        << "wire [1:0] " << p << "target=" << p << "wait_service && !" << p << "fault ? (" << p
        << "power ? 2'd1 : 2'd0) : " << p << "wanted;\n"
        << "reg [@STAGE@-1:0] " << p << "reset_sample='1;\n"
        << "always @(posedge " << prefix << "clk or negedge " << prefix << "ready)\n"
        << " if(!" << prefix << "ready) " << p << "reset_sample<='1;\n"
        << " else " << p << "reset_sample<={" << p << "reset_sample[@STAGE@-2:0],"
        << activeReset(d.name) << "};\n"
        << "wire " << p << "held_reset=" << p << "reset_sample[@STAGE@-1];\n";
    QString                      text = R"sv(
wire @P@power,@P@clock,@P@reset,@P@isolation,@P@quiesce;
wire @P@off,@P@held,@P@run,@P@fault,@P@watch;
@F@action_contract @P@action (
 .clk(@F@clk),.cold_n(@F@ready),.target(@P@target),
 .power_valid(@GOOD@),.reset_active(@P@held_reset),.isolation_active(@ISO@),.idle(@IDLE@),
 .service_fault(@P@service_failure),.power_request(@P@power),.clock_request(@P@clock),
 .reset_request(@P@reset),.isolation_request(@P@isolation),.quiesce_request(@P@quiesce),
 .state_off(@P@off),.state_reset(@P@held),.state_run(@P@run),.fault(@P@fault),.watch(@P@watch));
wire @P@ready_off=@P@off && !@GOOD@ && @P@held_reset && @ISO@ && @IDLE@;
wire @P@ready_reset=@P@held && @GOOD@ && @P@held_reset && @ISO@ && @IDLE@;
wire @P@ready_run=@P@run && @GOOD@ && !@P@held_reset && !@ISO@ && !@IDLE@;
wire @P@released=@P@ready_off || @P@ready_reset ||
 (@P@fault && !@P@power && !@GOOD@ && @P@held_reset && @ISO@ && @IDLE@);
wire @P@blocked_by_chip=@P@base!=@P@desired;
wire @P@done=@P@valid && !@P@blocked_by_chip && (
 (@P@decoded==0 && @P@ready_off) || (@P@decoded==1 && @P@ready_reset) || (@P@decoded==2 && @P@ready_run));
always @(posedge @F@clk) begin
 if(!@F@history) assume(!@GOOD@ && @ISO@ && @IDLE@);
 else begin
  assume(@GOOD@==$past(@GOOD@) || @GOOD@==$past(@POWER@) || (@FAULT@ && !@GOOD@));
  assume(@ISO@==$past(@ISO@) || @ISO@==$past(@ISOLATE@));
  assume(@IDLE@==$past(@IDLE@) || @IDLE@==$past(@STOP@));
 end
 if(@F@ready) begin
  @P@power_match: assert(@POWER@==@P@power);
  @P@clock_match: assert(@CLOCK@==@P@clock);
  @P@reset_match: assert(@RESET@==@P@reset);
  @P@isolation_match: assert(@ISOLATE@==@P@isolation);
  @P@quiesce_match: assert(@STOP@==@P@quiesce);
  if(!@FAULT@) @P@normal_fault: assert(!@P@fault);
 end
end
)sv";
    const QMap<QString, QString> tokens{
        {"P", p},
        {"F", prefix},
        {"GOOD", supply.valid.signal},
        {"ISO", config.isolation.completion.signal},
        {"IDLE", config.quiesce.completion.signal},
        {"POWER", supply.request},
        {"ISOLATE", config.isolation.request},
        {"STOP", config.quiesce.request},
        {"CLOCK", binding.domain[d.name].clockEnable},
        {"RESET", d.actual + "reset_request"}};
    for (auto it = tokens.cbegin(); it != tokens.cend(); ++it)
        text.replace('@' + it.key() + '@', it.value());
    out << text;
    QStringList flags{p + "done", '!' + p + "valid", p + "fault"};
    if (!plan.chip.isEmpty())
        flags.append(p + "blocked_by_chip");
    if (!d.serve.isEmpty())
        flags.append(p + "in_use");
    if (!d.use.isEmpty())
        flags.append(p + "wait_service");
    out << "reg [@D@-1:0] " << p << "status;\nalways @* begin\n " << p << "status=0;\n " << p
        << "status[" << d.width - 1 << ":0]=" << p << "mode;\n";
    for (qsizetype i = 0; i < flags.size(); ++i)
        out << ' ' << p << "status[" << d.width + i << "]=" << flags[i] << ";\n";
    out << "end\n";
    read.insert(d.status, p + "status");
    const int count = d.use.isEmpty() ? 1 : 2;
    out << "reg [" << count - 1 << ":0] " << p << "event_value=0;\n"
        << "always @(posedge " << prefix << "clk or negedge " << prefix << "ready) begin\n"
        << " if(!" << prefix << "ready) " << p << "event_value<=0;\n else begin\n";
    for (int i = 0; i < count; ++i) {
        out << "  if(" << (i == 0 ? p + "watch && !" + supply.valid.signal : p + "service_failure")
            << ") " << p << "event_value[" << i << "]<=1;\n"
            << "  else if(" << prefix << "store && " << prefix
            << "write_address==" << input.addressWidth << "'d" << d.event << " && " << prefix
            << "strobe[0] && " << prefix << "data[" << i << "]) " << p << "event_value[" << i
            << "]<=0;\n";
    }
    out << " end\nend\n";
    read.insert(d.event, p + "event_value");
    physical(out, d);
}

void Proof::service(QTextStream &out, qsizetype i)
{
    const auto &e = plan.service[i];
    const auto  c = node[e.consumer].model, p = node[e.provider].model, s = serviceName(i);
    out << "wire " << s << "request," << s << "hold," << s << "grant," << s << "failure," << s
        << "permission;\n"
        << prefix << "service_contract " << s << "handshake (\n"
        << ".clk(" << prefix << "clk),.cold_n(" << prefix << "ready),.need(" << c << "wanted==2),\n"
        << ".available(" << p << "ready_run),.provider_fault(" << p << "fault),.consumer_fault("
        << c << "fault),.released(" << c << "released),\n"
        << ".request(" << s << "request),.hold(" << s << "hold),.grant(" << s << "grant),.failure("
        << s << "failure),.permission(" << s << "permission));\n";
    const auto actual = circuit.binding["service"].toArray()[i].toObject();
    out << "always @(posedge " << prefix << "clk) if(" << prefix << "ready) begin\n"
        << ' ' << s << "request_match: assert(" << actual["request"].toString() << "==" << s
        << "request);\n"
        << ' ' << s << "grant_match: assert(" << actual["grant"].toString() << "==" << s
        << "grant);\nend\n";
}

void Proof::chipStatus(QTextStream &out)
{
    const auto p = prefix + "chip_";
    out << "reg " << p << "done;\nalways @* begin\n " << p << "done=0;\n case(" << p << "mode)\n";
    const QStringList state{"off", "reset", "run"};
    for (auto policy = plan.chip.cbegin(); policy != plan.chip.cend(); ++policy) {
        QStringList term;
        for (const auto &d : node) {
            const auto target = policy.value()[d.name];
            if (target)
                term.append(
                    '(' + d.model + "wanted==2'd" + QString::number(int(*target)) + " && !"
                    + d.model + "wait_service && " + d.model + "ready_" + state[int(*target)] + ')');
            else {
                QStringList valid;
                for (auto value : plan.domain[d.name].mode)
                    valid.append(d.model + "ready_" + state[int(value)]);
                valid.removeDuplicates();
                term.append(either(valid));
            }
        }
        out << chipWidth << "'d" << policy.key() << ": " << p << "done=" << term.join(" && ")
            << ";\n";
    }
    out << "default: begin end\nendcase\nend\n"
        << "wire [@D@-1:0] " << p << "status={ !" << p << "valid, " << p << "done, " << p
        << "mode};\n";
    read.insert(input.dataWidth / 8, p + "status");
}

void Proof::registerRead(QTextStream &out)
{
    out << "function [@D@-1:0] " << prefix
        << "read_value(input [@A@-1:0] address);\ncase(address)\n";
    for (auto it = read.cbegin(); it != read.cend(); ++it)
        out << input.addressWidth << "'d" << it.key() << ": " << prefix
            << "read_value=" << it.value() << ";\n";
    out << "default: " << prefix << "read_value=0;\nendcase\nendfunction\n"
        << "function " << prefix << "mapped(input [@A@-1:0] address);\n " << prefix << "mapped=";
    QStringList match;
    for (auto it = read.cbegin(); it != read.cend(); ++it)
        match.append(
            "address==" + QString::number(input.addressWidth) + "'d" + QString::number(it.key()));
    out << either(match) << ";\nendfunction\n";
}

void Proof::bus(QTextStream &out)
{
    if (input.bus == QSocMmioBus::Axi4Lite) {
        QString text = R"sv(
reg @F@aw = 0, @F@w = 0, @F@b = 0, @F@r = 0;
reg [@A@-1:0] @F@address = 0;
reg [@D@-1:0] @F@saved_data = 0;
reg [@D@/8-1:0] @F@saved_strobe = 0;
reg [1:0] @F@bresp = 0, @F@rresp = 0;
reg [@D@-1:0] @F@read_data = 0;
wire @F@aw_take = s_axi_awvalid && s_axi_awready;
wire @F@w_take = s_axi_wvalid && s_axi_wready;
wire @F@ar_take = s_axi_arvalid && s_axi_arready;
wire [@A@-1:0] @F@write_address = @F@aw ? @F@address : s_axi_awaddr;
wire [@D@-1:0] @F@data = @F@w ? @F@saved_data : s_axi_wdata;
wire [@D@/8-1:0] @F@strobe = @F@w ? @F@saved_strobe : s_axi_wstrb;
wire @F@commit = @F@ready && !@F@b && (@F@aw || @F@aw_take) && (@F@w || @F@w_take) && !@F@clear;
wire @F@store = @F@commit;
always @(posedge @F@clk or negedge @F@ready) begin
    if (!@F@ready) begin
        @F@aw <= 0; @F@w <= 0; @F@b <= 0; @F@r <= 0;
        @F@address <= 0; @F@saved_data <= 0; @F@saved_strobe <= 0;
        @F@bresp <= 0; @F@rresp <= 0; @F@read_data <= 0;
    end else begin
        if (@F@b && s_axi_bready) @F@b <= 0;
        if (@F@r && s_axi_rready) @F@r <= 0;
        if (@F@aw_take) begin @F@aw <= 1; @F@address <= s_axi_awaddr; end
        if (@F@w_take) begin @F@w <= 1; @F@saved_data <= s_axi_wdata; @F@saved_strobe <= s_axi_wstrb; end
        if (@F@commit) begin
            @F@aw <= 0; @F@w <= 0; @F@b <= 1;
            @F@bresp <= @F@mapped(@F@write_address) ? 0 : 2;
        end
        if (@F@ar_take) begin
            @F@r <= 1;
            @F@rresp <= @F@mapped(s_axi_araddr) ? 0 : 2;
            @F@read_data <= @F@read_value(s_axi_araddr);
        end
    end
end
always @(posedge @F@clk) begin
    if (@F@ready) begin
        @F@aw_ready: assert(s_axi_awready == (!@F@aw && !@F@b));
        @F@w_ready: assert(s_axi_wready == (!@F@w && !@F@b));
        @F@ar_ready: assert(s_axi_arready == (!@F@r && !@F@clear));
        @F@b_valid: assert(s_axi_bvalid == @F@b);
        @F@r_valid: assert(s_axi_rvalid == @F@r);
        if (@F@b) @F@b_error: assert(s_axi_bresp == @F@bresp);
        if (@F@r) begin
            @F@r_error: assert(s_axi_rresp == @F@rresp);
            @F@r_data: assert(s_axi_rdata == @F@read_data);
        end
        if (@F@history && $past(@F@ready)) begin
            if ($past(s_axi_awvalid && !s_axi_awready))
                assume(s_axi_awvalid && {s_axi_awaddr, s_axi_awprot} == $past({s_axi_awaddr, s_axi_awprot}));
            if ($past(s_axi_wvalid && !s_axi_wready))
                assume(s_axi_wvalid && {s_axi_wdata, s_axi_wstrb} == $past({s_axi_wdata, s_axi_wstrb}));
            if ($past(s_axi_arvalid && !s_axi_arready))
                assume(s_axi_arvalid && {s_axi_araddr, s_axi_arprot} == $past({s_axi_araddr, s_axi_arprot}));
            if ($past(s_axi_bvalid && !s_axi_bready))
                @F@b_hold: assert(s_axi_bvalid && s_axi_bresp == $past(s_axi_bresp));
            if ($past(s_axi_rvalid && !s_axi_rready))
                @F@r_hold: assert(s_axi_rvalid && {s_axi_rdata, s_axi_rresp} == $past({s_axi_rdata, s_axi_rresp}));
        end
    end
end
)sv";
        text.replace("@F@", prefix);
        out << text;
        return;
    }
    QString text = R"sv(
wire @F@access=s_apb_pselx && s_apb_penable;
wire @F@commit=@F@access && s_apb_pready;
wire @F@store=@F@commit && s_apb_pwrite;
wire [@A@-1:0] @F@write_address=s_apb_paddr;
wire [@D@-1:0] @F@data=s_apb_pwdata;
wire [@D@/8-1:0] @F@strobe=s_apb_pstrb;
always @(posedge @F@clk) begin
 if(@F@ready) begin
  @F@apb_ready: assert(s_apb_pready==!@F@clear);
  if(@F@history && $past(@F@ready)) begin
   if($past(s_apb_pselx && (!s_apb_penable || !s_apb_pready))) begin
    assume(@F@access);
    assume({s_apb_paddr,s_apb_pwrite,s_apb_pwdata,s_apb_pstrb,s_apb_pprot}==
      $past({s_apb_paddr,s_apb_pwrite,s_apb_pwdata,s_apb_pstrb,s_apb_pprot}));
   end else assume(!s_apb_penable);
  end
  if(@F@commit) begin
   @F@apb_error: assert(s_apb_pslverr==!@F@mapped(s_apb_paddr));
   if(!s_apb_pwrite) @F@apb_read: assert(s_apb_prdata==@F@read_value(s_apb_paddr));
  end
 end else assume(!s_apb_pselx && !s_apb_penable);
end
)sv";
    text.replace("@F@", prefix);
    out << text;
}
} // namespace

namespace {
void Proof::physical(QTextStream &out, const Domain &d)
{
    const auto                      p      = d.model;
    const auto                     &config = input.domain[d.name];
    const auto                     &supply = input.supplyTable[config.supply];
    QSocResetPrimitive::ResetTarget target;
    for (const auto &item : binding.reset.targets)
        if (item.name == config.reset.target)
            target = item;
    QStringList inactive;
    for (const auto &link : target.links) {
        if (link.source == config.reset.source) {
            inactive.append('!' + p + "reset");
            continue;
        }
        bool low = true;
        for (const auto &source : binding.reset.sources)
            if (source.name == link.source)
                low = source.active == "low";
        const auto value = (low ? "" : "!") + link.source;
        inactive.append(value);
        if (link.source != input.resetSource)
            out << "always @* if(!@FAULT@) assume(" << value << ");\n";
    }
    const int n = target.async.stage;
    out << "reg " << p << "gate;\n"
        << "always @* if(!" << prefix << "clk) " << p << "gate=" << p << "clock;\n"
        << "wire " << p << "clock_value=" << p << "gate && " << prefix << "clk;\n"
        << "wire " << p << "release_enable=" << inactive.join(" && ") << ";\n"
        << "reg [" << n - 1 << ":0] " << p << "release_sample=0;\n"
        << "always @(posedge " << p << "clock_value or negedge " << p << "release_enable)\n"
        << " if(!" << p << "release_enable) " << p << "release_sample<=0;\n"
        << " else " << p << "release_sample<={" << p << "release_sample[" << n - 2 << ":0],1'b1};\n"
        << "always @($global_clock) if(" << prefix << "ready) begin\n"
        << ' ' << p << "gated_clock: assert(" << config.clock.target << "==" << p
        << "clock_value);\n"
        << ' ' << p << "reset_output: assert((" << activeReset(d.name) << ")==!" << p
        << "release_sample[" << n - 1 << "]);\n"
        << " if(!" << supply.request << ") " << p << "off_protection: assert(("
        << activeReset(d.name) << ") && !" << config.clock.target << ");\nend\n"
        << "reg " << p << "seen_work=0;\n"
        << "always @(posedge " << prefix << "clk or negedge " << prefix << "ready)\n"
        << " if(!" << prefix << "ready) " << p << "seen_work<=0;\n"
        << " else if(!" << config.quiesce.request << ") " << p << "seen_work<=1;\n"
        << "always @(posedge " << prefix << "clk) if(" << prefix << "ready && " << prefix
        << "history && $past(" << prefix << "ready)) begin\n"
        << " if($past(" << supply.request << ") && !" << supply.request << ") begin\n"
        << ' ' << p << "isolated_shutdown: assert(" << config.isolation.completion.signal << ");\n"
        << " if(" << p << "seen_work) " << p << "idle_shutdown: assert("
        << config.quiesce.completion.signal << ");\n end\nend\n";
}

void Proof::reachability(QTextStream &out)
{
    out << "if (@COVER@) begin\nreg " << prefix << "boot=0;\n"
        << "always @(posedge " << prefix << "clk) " << prefix << "boot<=1;\n"
        << "always @* begin\n assume(" << input.resetSource << "==" << prefix << "boot);\n"
        << " assume(" << prefix << "strobe=='1);\nend\n";
    for (const auto &d : node) {
        const auto &config = input.domain[d.name];
        const auto &supply = input.supplyTable[config.supply];
        const auto &mode   = plan.domain[d.name];
        const auto  p      = d.model + "reach_";
        out << "always @(posedge " << prefix << "clk) if(" << prefix << "history) begin\n"
            << " if(!@FAULT@) assume(" << supply.valid.signal << "==$past(" << supply.request
            << "));\n"
            << " assume(" << config.isolation.completion.signal << "==$past("
            << config.isolation.request << "));\n"
            << " assume(" << config.quiesce.completion.signal << "==$past("
            << config.quiesce.request << "));\nend\n"
            << "always @(posedge " << prefix << "clk) if(" << prefix << "ready) begin\n";
        for (auto it = mode.mode.cbegin(); it != mode.mode.cend(); ++it)
            out << ' ' << p << "mode_" << it.key() << ": cover(" << d.actual << "mode==" << d.width
                << "'d" << it.key() << " && " << d.actual << "done);\n";
        out << "end\n";
        const auto value  = mode.mode.values();
        const bool hasRun = value.contains(QSocPrcmTarget::Run);
        if (!hasRun && !value.contains(QSocPrcmTarget::Reset))
            continue;
        const auto active = d.actual + (hasRun ? "ready_run" : "ready_reset");
        out << "reg " << p << "seen_active=0," << p << "stop=0;\n"
            << "always @(posedge " << prefix << "clk or negedge " << prefix << "ready) begin\n"
            << " if(!" << prefix << "ready) begin " << p << "seen_active<=0; " << p
            << "stop<=0; end\n"
            << " else begin\n  if(" << active << ") " << p << "seen_active<=1;\n"
            << "  if(" << prefix << "clear) " << p << "stop<=0;\n"
            << "  else if(" << active << " && " << prefix << "store && " << prefix
            << "write_address==" << input.addressWidth << "'d" << d.request << " && " << d.model
            << "decode(" << prefix << "data[" << d.width - 1 << ":0])==0) " << p
            << "stop<=1;\n end\nend\n"
            << "always @(posedge " << prefix << "clk) if(" << prefix << "ready) begin\n"
            << ' ' << p << "cycle: cover(" << p << "stop && " << d.actual << "ready_off && "
            << d.actual << "done);\n"
            << " if(@FAULT@) " << p << "fault: cover(" << p << "seen_active && " << d.actual
            << "fault);\n";
        if (d.width == 64 || quint64(mode.mode.size()) < (quint64{1} << d.width))
            out << ' ' << p << "invalid: cover(!" << d.actual << "valid && " << active << ");\n";
        for (const auto &target : binding.reset.targets)
            if (target.name == input.resetTarget && target.links.size() > 1)
                out << ' ' << p << "management: cover(" << p << "seen_active && " << active
                    << " && " << prefix << "clear);\n";
        if (!d.use.isEmpty())
            out << " if(@FAULT@) " << p << "service_fault: cover(" << p << "seen_active && "
                << d.actual << "service_failure);\n";
        out << "end\n";
    }
    for (qsizetype i = 0; i < plan.service.size(); ++i) {
        const auto actual   = circuit.binding["service"].toArray()[i].toObject();
        const auto request  = actual["request"].toString();
        const auto grant    = actual["grant"].toString();
        const auto p        = serviceName(i) + "reach_";
        const auto consumer = node[plan.service[i].consumer].actual;
        out << "reg " << p << "used=0;\n"
            << "always @(posedge " << prefix << "clk or negedge " << prefix << "ready)\n"
            << " if(!" << prefix << "ready) " << p << "used<=0;\n"
            << " else if(" << consumer << "ready_run && " << request << " && " << grant << ") " << p
            << "used<=1;\n"
            << "always @(posedge " << prefix << "clk) if(" << prefix << "ready) begin\n"
            << ' ' << p << "use: cover(" << p << "used);\n"
            << ' ' << p << "release: cover(" << p << "used && !" << request << " && !" << grant
            << ");\nend\n";
    }
    out << "end\n";
}

} // namespace

QSocMmioFormalCollateral QSocPrcmFormal::generateShared(
    const QSocPrcmBindingPlan     &binding,
    const QSocPrcmCompositionPlan &composition,
    const QSocPrcmCircuit         &circuit,
    const QString                 &moduleName,
    int                            sampleStage)
{
    return Proof(binding, circuit, composition, moduleName, sampleStage).generate();
}
