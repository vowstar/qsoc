// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocprcminput.h"
#include "common/qsocprcmreader.h"

#include <limits>

namespace {

using QSocPrcmDetail::Context;
using QSocPrcmDetail::Reader;

QSocPrcmFeedback feedback(const Reader &reader)
{
    reader.fields({"signal", "sample_clock"});
    return {reader.member("signal").name(), reader.member("sample_clock").name()};
}

QSocPrcmHandshake handshake(const Reader &reader, const QString &completion)
{
    reader.fields({"request", completion});
    return {reader.member("request").name(), feedback(reader.member(completion))};
}

QSocPrcmSupply supply(const Reader &reader)
{
    reader.fields({}, {"always_on", "request", "valid"});
    QSocPrcmSupply result;
    if (reader.has("always_on")) {
        result.alwaysOn = reader.member("always_on").choice("true", "false");
    }
    if (result.alwaysOn) {
        reader.fields({"always_on"});
    } else {
        reader.fields({"request", "valid"}, {"always_on"});
        result.request = reader.member("request").name();
        result.valid   = feedback(reader.member("valid"));
    }
    return result;
}

QSocPrcmMode mode(const Reader &reader)
{
    reader.fields({"code", "power", "clock", "reset", "isolation"});
    return {
        reader.member("code").number(),
        reader.member("power").choice("on", "off"),
        reader.member("clock").choice("running", "stopped"),
        reader.member("reset").choice("asserted", "released"),
        reader.member("isolation").choice("enabled", "disabled")};
}

QSocPrcmDomain domain(const Reader &reader)
{
    reader.fields(
        {"supply", "clock", "reset", "quiesce", "isolation", "reset_mode", "mode", "transition"},
        {"service", "require"});
    QSocPrcmDomain result;
    result.supply    = reader.member("supply").name();
    const auto clock = reader.member("clock");
    clock.fields({"controller", "target", "stage"});
    result.clock
        = {clock.member("controller").name(),
           clock.member("target").name(),
           clock.member("stage").text()};
    const auto reset = reader.member("reset");
    reset.fields({"controller", "source", "target"});
    result.reset
        = {reset.member("controller").name(),
           reset.member("source").name(),
           reset.member("target").name()};
    result.quiesce       = handshake(reader.member("quiesce"), "ack");
    result.isolation     = handshake(reader.member("isolation"), "active");
    result.resetMode     = reader.member("reset_mode").name();
    const auto modeTable = reader.member("mode");
    for (const auto &name : modeTable.table()) {
        result.mode.insert(name, mode(modeTable.member(name)));
    }
    const auto transition = reader.member("transition");
    const auto count      = transition.size();
    for (std::size_t i = 0; i < count; ++i) {
        const auto entry = transition.item(i);
        entry.fields({"from", "to"});
        result.transition.append({entry.member("from").name(), entry.member("to").name()});
    }
    if (reader.has("service")) {
        const auto service = reader.member("service");
        for (const auto &name : service.table(false)) {
            const auto entry = service.member(name);
            entry.fields({"mode"});
            result.service.insert(name, entry.member("mode").name());
        }
    }
    if (reader.has("require")) {
        const auto require = reader.member("require");
        for (const auto &name : require.table(false)) {
            const auto entry = require.member(name);
            entry.fields({"service", "mode"});
            const auto service = entry.member("service");
            const auto part    = service.text().split('.');
            if (part.size() != 2 || !QSocVerilogUtils::isValidVerilogIdentifier(part[0])
                || !QSocVerilogUtils::isValidVerilogIdentifier(part[1])) {
                service.fail("PRCM_REFERENCE", "Expected domain.service.");
            }
            result.require.insert(name, {part[0], part[1], entry.member("mode").nameList()});
        }
    }
    return result;
}

QSocPrcmChipMode chipMode(const Reader &reader)
{
    reader.fields({"code", "domain"});
    QSocPrcmChipMode result;
    result.code            = reader.member("code").number();
    const auto domainTable = reader.member("domain");
    for (const auto &name : domainTable.table()) {
        const auto entry = domainTable.member(name);
        entry.fields({}, {"allow", "target"});
        if (entry.has("allow") == entry.has("target")) {
            entry.fail("PRCM_POLICY", "Specify either allow or target.");
        }
        QSocPrcmDomainPolicy policy;
        if (entry.has("allow")) {
            policy.allow = entry.member("allow").nameList();
        } else {
            policy.target = entry.member("target").name();
        }
        result.domain.insert(name, policy);
    }
    return result;
}

QSocPrcmInput input(const Reader &reader)
{
    reader.fields({"version", "controller", "mmio", "supply", "domain"}, {"chip"});
    const auto version = reader.member("version");
    if (version.number() != 1) {
        version.fail("PRCM_VERSION", "Only PRCM input version 1 is supported.");
    }
    QSocPrcmInput result;
    const auto    controller = reader.member("controller");
    controller.fields({"clock", "reset", "supply"});
    const auto clock = controller.member("clock");
    clock.fields({"controller", "input"});
    result.clockController = clock.member("controller").name();
    result.clockInput      = clock.member("input").name();
    const auto reset       = controller.member("reset");
    reset.fields({"controller", "source"});
    result.resetController = reset.member("controller").name();
    result.resetSource     = reset.member("source").name();
    result.supply          = controller.member("supply").name();
    const auto mmio        = reader.member("mmio");
    mmio.fields({"bus", "data_width", "address_width"});
    const auto bus = QSocMmioGenerator::parseBus(mmio.member("bus").text());
    if (!bus) {
        mmio.member("bus").fail("PRCM_BUS", "Unknown MMIO bus.");
    }
    result.bus       = *bus;
    result.dataWidth = static_cast<quint32>(
        mmio.member("data_width").number(std::numeric_limits<quint32>::max()));
    result.addressWidth = static_cast<quint32>(
        mmio.member("address_width").number(std::numeric_limits<quint32>::max()));
    const auto supplyTable = reader.member("supply");
    for (const auto &name : supplyTable.table()) {
        result.supplyTable.insert(name, supply(supplyTable.member(name)));
    }
    const auto domainTable = reader.member("domain");
    for (const auto &name : domainTable.table()) {
        result.domain.insert(name, domain(domainTable.member(name)));
    }
    if (reader.has("chip")) {
        const auto chip = reader.member("chip");
        chip.fields({"reset_mode", "mode"});
        result.chipResetMode = chip.member("reset_mode").name();
        const auto modeTable = chip.member("mode");
        for (const auto &name : modeTable.table()) {
            result.chipMode.insert(name, chipMode(modeTable.member(name)));
        }
    }
    return result;
}

} // namespace

QSocPrcmParseResult QSocPrcmParser::parse(const YAML::Node &netlist, const QString &file)
{
    QSocPrcmParseResult result;
    Context             context{file, {}};
    try {
        const Reader root(netlist, {}, context);
        root.keys();
        if (!root.has("prcm")) {
            root.fail("PRCM_REQUIRED", "Missing prcm declaration.");
        }
        auto parsed   = input(root.member("prcm"));
        parsed.source = context.source;
        result.input  = std::move(parsed);
    } catch (const QSocPrcmDiagnostic &diagnostic) {
        result.diagnostic.append(diagnostic);
    } catch (const YAML::Exception &error) {
        result.diagnostic.append(
            {"PRCM_YAML",
             QString::fromUtf8(error.msg.c_str()),
             {{file,
               {},
               error.mark.is_null() ? 0 : error.mark.line + 1,
               error.mark.is_null() ? 0 : error.mark.column + 1}}});
    }
    return result;
}
