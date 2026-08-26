// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocmmiogenerator.h"

#include "common/qsocmodulemanager.h"
#include "common/qsocverilogutils.h"

#include <algorithm>
#include <limits>
#include <QHash>
#include <QRegularExpression>
#include <QSet>

namespace {

const QSet<QString> kGeneratorKeys = {"kind", "bus", "register"};
const QSet<QString> kRegisterKeys  = {"offset", "field", "description"};
const QSet<QString> kFieldKeys
    = {"lsb", "width", "access", "reset", "input", "output", "value", "description"};
const QSet<QString> kFixedPorts = {
    "clk_i",         "rst_ni",        "s_axi_awaddr", "s_axi_awprot", "s_axi_awvalid",
    "s_axi_awready", "s_axi_wdata",   "s_axi_wstrb",  "s_axi_wvalid", "s_axi_wready",
    "s_axi_bresp",   "s_axi_bvalid",  "s_axi_bready", "s_axi_araddr", "s_axi_arprot",
    "s_axi_arvalid", "s_axi_arready", "s_axi_rdata",  "s_axi_rresp",  "s_axi_rvalid",
    "s_axi_rready",
};
const QSet<QString> kInternalNames = {
    "AXI_RESP_OKAY",
    "AXI_RESP_SLVERR",
    "address_is_mapped",
    "read_register",
    "aw_pending_q",
    "awaddr_q",
    "w_pending_q",
    "wdata_q",
    "wstrb_q",
    "aw_take",
    "w_take",
    "write_address",
    "write_data",
    "write_strobe",
    "write_fire",
    "write_mask",
};

void appendError(
    QStringList *errors, const QString &code, const QString &path, const QString &message)
{
    errors->append(QString("MMIO_%1 %2: %3").arg(code, path, message));
}

bool validateMap(
    const YAML::Node &node, const QSet<QString> &allowed, const QString &path, QStringList *errors)
{
    if (!node || !node.IsMap()) {
        appendError(errors, "TYPE", path, "must be a map");
        return false;
    }

    QSet<QString> seen;
    for (YAML::const_iterator it = node.begin(); it != node.end(); ++it) {
        if (!it->first.IsScalar()) {
            appendError(errors, "TYPE", path, "property names must be scalar");
            continue;
        }
        const QString key = QString::fromStdString(it->first.Scalar());
        if (seen.contains(key)) {
            appendError(errors, "DUPLICATE", path + "." + key, "property is duplicated");
        }
        seen.insert(key);
        if (!allowed.contains(key)) {
            appendError(errors, "UNSUPPORTED", path + "." + key, "unsupported property");
        }
    }
    return true;
}

bool parseScalar(const YAML::Node &node, const QString &path, QString *value, QStringList *errors)
{
    if (!node || !node.IsScalar()) {
        appendError(errors, "TYPE", path, "must be a scalar");
        return false;
    }
    *value = QString::fromStdString(node.Scalar());
    return true;
}

bool parseIdentifier(const YAML::Node &node, const QString &path, QString *value, QStringList *errors)
{
    if (!parseScalar(node, path, value, errors)) {
        return false;
    }
    if (!QSocVerilogUtils::isValidVerilogIdentifier(*value)) {
        appendError(errors, "IDENTIFIER", path, "must be a Verilog identifier");
        return false;
    }
    return true;
}

bool parseUnsigned(
    const YAML::Node &node, const QString &path, quint64 maximum, quint64 *value, QStringList *errors)
{
    QString text;
    if (!parseScalar(node, path, &text, errors)) {
        return false;
    }

    static const QRegularExpression numberPattern(QStringLiteral("^(?:0[xX][0-9a-fA-F]+|[0-9]+)$"));
    if (!numberPattern.match(text).hasMatch()) {
        appendError(errors, "TYPE", path, "must be an unsigned decimal or hexadecimal integer");
        return false;
    }

    bool          ok     = false;
    const int     base   = text.startsWith("0x", Qt::CaseInsensitive) ? 16 : 10;
    const QString digits = base == 16 ? text.mid(2) : text;
    const quint64 parsed = digits.toULongLong(&ok, base);
    if (!ok || parsed > maximum) {
        appendError(errors, "RANGE", path, QString("must be at most %1").arg(maximum));
        return false;
    }
    *value = parsed;
    return true;
}

bool parseUnsigned32(
    const YAML::Node &node, const QString &path, quint32 maximum, quint32 *value, QStringList *errors)
{
    quint64 parsed = 0;
    if (!parseUnsigned(node, path, maximum, &parsed, errors)) {
        return false;
    }
    *value = static_cast<quint32>(parsed);
    return true;
}

void parseDescription(
    const YAML::Node &node, const QString &path, QString *description, QStringList *errors)
{
    if (!node) {
        description->clear();
        return;
    }
    if (!node.IsScalar()) {
        appendError(errors, "TYPE", path, "must be a scalar");
        return;
    }
    *description = QString::fromStdString(node.Scalar());
}

bool parseFieldShape(
    const YAML::Node &node, const QString &path, QSocMmioFieldPlan *field, QStringList *errors)
{
    bool valid = true;
    if (!node["lsb"]) {
        appendError(errors, "REQUIRED", path + ".lsb", "property is required");
        valid = false;
    } else {
        valid = parseUnsigned32(node["lsb"], path + ".lsb", 31, &field->lsb, errors) && valid;
    }

    if (node["width"]) {
        valid = parseUnsigned32(node["width"], path + ".width", 32, &field->width, errors) && valid;
        if (field->width == 0) {
            appendError(errors, "RANGE", path + ".width", "must be at least 1");
            valid = false;
        }
    }
    if (valid && field->lsb + field->width > 32) {
        appendError(errors, "RANGE", path, "field must fit within 32 bits");
        valid = false;
    }
    return valid;
}

bool parseAccess(
    const YAML::Node &node, const QString &path, QSocMmioFieldPlan *field, QStringList *errors)
{
    QString access;
    if (!node["access"]) {
        appendError(errors, "REQUIRED", path + ".access", "property is required");
        return false;
    }
    if (!parseScalar(node["access"], path + ".access", &access, errors)) {
        return false;
    }
    if (access == "rw") {
        field->access = QSocMmioAccess::ReadWrite;
        return true;
    }
    if (access == "ro") {
        field->access = QSocMmioAccess::ReadOnly;
        return true;
    }
    appendError(errors, "ACCESS", path + ".access", "must be rw or ro");
    return false;
}

bool valueFitsWidth(quint64 value, quint32 width)
{
    const quint64 maximum = width == 32 ? std::numeric_limits<quint32>::max()
                                        : (quint64(1) << width) - 1;
    return value <= maximum;
}

bool parseReadWriteField(
    const YAML::Node &node, const QString &path, QSocMmioFieldPlan *field, QStringList *errors)
{
    bool valid = true;
    if (!node["reset"]) {
        appendError(errors, "REQUIRED", path + ".reset", "property is required for rw fields");
        valid = false;
    } else {
        quint64    resetValue = 0;
        const bool resetValid = parseUnsigned(
            node["reset"], path + ".reset", std::numeric_limits<quint32>::max(), &resetValue, errors);
        if (resetValid) {
            field->resetValue = resetValue;
        }
        valid = resetValid && valid;
        if (valid && !valueFitsWidth(*field->resetValue, field->width)) {
            appendError(errors, "RANGE", path + ".reset", "value does not fit field width");
            valid = false;
        }
    }
    if (node["input"]) {
        appendError(errors, "ACCESS", path + ".input", "is not allowed for rw fields");
        valid = false;
    }
    if (node["value"]) {
        appendError(errors, "ACCESS", path + ".value", "is not allowed for rw fields");
        valid = false;
    }
    if (node["output"]) {
        valid = parseIdentifier(node["output"], path + ".output", &field->outputPort, errors)
                && valid;
    }
    return valid;
}

bool parseReadOnlyField(
    const YAML::Node &node, const QString &path, QSocMmioFieldPlan *field, QStringList *errors)
{
    bool valid       = true;
    int  sourceCount = node["input"] ? 1 : 0;
    sourceCount += node["value"] ? 1 : 0;
    if (sourceCount != 1) {
        appendError(errors, "SOURCE", path, "ro fields require exactly one of input or value");
        valid = false;
    }
    if (node["reset"]) {
        appendError(errors, "ACCESS", path + ".reset", "is not allowed for ro fields");
        valid = false;
    }
    if (node["output"]) {
        appendError(errors, "ACCESS", path + ".output", "is not allowed for ro fields");
        valid = false;
    }
    if (node["input"]) {
        valid = parseIdentifier(node["input"], path + ".input", &field->inputPort, errors) && valid;
    }
    if (node["value"]) {
        quint64    constantValue = 0;
        const bool valueValid    = parseUnsigned(
            node["value"],
            path + ".value",
            std::numeric_limits<quint32>::max(),
            &constantValue,
            errors);
        if (valueValid) {
            field->constantValue = constantValue;
        }
        valid = valueValid && valid;
        if (valid && !valueFitsWidth(*field->constantValue, field->width)) {
            appendError(errors, "RANGE", path + ".value", "value does not fit field width");
            valid = false;
        }
    }
    return valid;
}

bool parseField(
    const QString     &name,
    const YAML::Node  &node,
    const QString     &path,
    QSocMmioFieldPlan *field,
    QStringList       *errors)
{
    if (!validateMap(node, kFieldKeys, path, errors)) {
        return false;
    }
    field->name = name;
    parseDescription(node["description"], path + ".description", &field->description, errors);

    bool valid = QSocVerilogUtils::isValidVerilogIdentifier(name);
    if (!valid) {
        appendError(errors, "IDENTIFIER", path, "field name must be a Verilog identifier");
        valid = false;
    }
    const bool shapeValid  = parseFieldShape(node, path, field, errors);
    const bool accessValid = parseAccess(node, path, field, errors);
    valid                  = shapeValid && accessValid && valid;
    if (!accessValid) {
        return false;
    }
    if (field->access == QSocMmioAccess::ReadWrite) {
        return parseReadWriteField(node, path, field, errors) && valid;
    }
    return parseReadOnlyField(node, path, field, errors) && valid;
}

bool claimSideband(
    const QSocMmioFieldPlan &field, const QString &path, QSet<QString> *ports, QStringList *errors)
{
    const QString name = field.inputPort.isEmpty() ? field.outputPort : field.inputPort;
    if (name.isEmpty()) {
        return true;
    }
    static const QRegularExpression storageNamePattern(QStringLiteral("^mmio_field_[0-9]+_q$"));
    if (ports->contains(name) || kInternalNames.contains(name)
        || storageNamePattern.match(name).hasMatch()) {
        const QString key = field.inputPort.isEmpty() ? ".output" : ".input";
        appendError(errors, "CONFLICT", path + key, "port name is already in use");
        return false;
    }
    ports->insert(name);
    return true;
}

bool parseFields(
    const YAML::Node         &node,
    const QString            &path,
    QList<QSocMmioFieldPlan> *fields,
    QSet<QString>            *ports,
    QStringList              *errors)
{
    if (!node || !node.IsMap()) {
        appendError(errors, "TYPE", path, "must be a map");
        return false;
    }
    if (node.size() == 0) {
        appendError(errors, "EMPTY", path, "must contain at least one field");
        return false;
    }

    bool          valid    = true;
    quint64       occupied = 0;
    QSet<QString> seenFields;
    for (YAML::const_iterator it = node.begin(); it != node.end(); ++it) {
        if (!it->first.IsScalar()) {
            appendError(errors, "TYPE", path, "field names must be scalar");
            valid = false;
            continue;
        }
        const QString name      = QString::fromStdString(it->first.Scalar());
        const QString fieldPath = path + "." + name;
        if (seenFields.contains(name)) {
            appendError(errors, "DUPLICATE", fieldPath, "field is duplicated");
            valid = false;
            continue;
        }
        seenFields.insert(name);

        QSocMmioFieldPlan field;
        if (!parseField(name, it->second, fieldPath, &field, errors)) {
            valid = false;
            continue;
        }
        const quint64 mask = ((quint64(1) << field.width) - 1) << field.lsb;
        if ((occupied & mask) != 0) {
            appendError(errors, "OVERLAP", fieldPath, "field overlaps another field");
            valid = false;
            continue;
        }
        occupied |= mask;
        if (!claimSideband(field, fieldPath, ports, errors)) {
            valid = false;
            continue;
        }
        fields->append(field);
    }
    return valid;
}

bool parseRegister(
    const QString        &name,
    const YAML::Node     &node,
    const QString        &path,
    QSocMmioRegisterPlan *reg,
    QSet<QString>        *ports,
    QStringList          *errors)
{
    if (!validateMap(node, kRegisterKeys, path, errors)) {
        return false;
    }
    reg->name = name;
    parseDescription(node["description"], path + ".description", &reg->description, errors);

    bool valid = QSocVerilogUtils::isValidVerilogIdentifier(name);
    if (!valid) {
        appendError(errors, "IDENTIFIER", path, "register name must be a Verilog identifier");
        valid = false;
    }
    if (!node["offset"]) {
        appendError(errors, "REQUIRED", path + ".offset", "property is required");
        valid = false;
    } else {
        valid = parseUnsigned(
                    node["offset"],
                    path + ".offset",
                    std::numeric_limits<quint32>::max(),
                    &reg->byteOffset,
                    errors)
                && valid;
        if (valid && (reg->byteOffset & 3ULL) != 0) {
            appendError(errors, "ALIGNMENT", path + ".offset", "must be 4-byte aligned");
            valid = false;
        }
    }
    if (!node["field"]) {
        appendError(errors, "REQUIRED", path + ".field", "property is required");
        return false;
    }
    return parseFields(node["field"], path + ".field", &reg->fields, ports, errors) && valid;
}

bool parseRegisters(
    const YAML::Node &node, QSocMmioPlan *plan, QSet<QString> *ports, QStringList *errors)
{
    const QString path = "generator.register";
    if (!node || !node.IsMap()) {
        appendError(errors, "TYPE", path, "must be a map");
        return false;
    }
    if (node.size() == 0) {
        appendError(errors, "EMPTY", path, "must contain at least one register");
        return false;
    }

    bool                    valid = true;
    QSet<QString>           names;
    QHash<quint64, QString> offsets;
    for (YAML::const_iterator it = node.begin(); it != node.end(); ++it) {
        if (!it->first.IsScalar()) {
            appendError(errors, "TYPE", path, "register names must be scalar");
            valid = false;
            continue;
        }
        const QString name         = QString::fromStdString(it->first.Scalar());
        const QString registerPath = path + "." + name;
        if (names.contains(name)) {
            appendError(errors, "DUPLICATE", registerPath, "register is duplicated");
            valid = false;
            continue;
        }
        names.insert(name);

        QSocMmioRegisterPlan reg;
        if (!parseRegister(name, it->second, registerPath, &reg, ports, errors)) {
            valid = false;
            continue;
        }
        if (offsets.contains(reg.byteOffset)) {
            appendError(
                errors,
                "DUPLICATE",
                registerPath + ".offset",
                QString("duplicates %1").arg(offsets.value(reg.byteOffset)));
            valid = false;
            continue;
        }
        offsets.insert(reg.byteOffset, registerPath + ".offset");
        plan->registers.append(reg);
    }
    return valid;
}

bool parsePlan(const QSocModuleDefinition &definition, QSocMmioPlan *plan, QStringList *errors)
{
    plan->moduleName = definition.moduleName;
    bool valid       = true;
    if (!QSocVerilogUtils::isValidVerilogIdentifier(definition.moduleName)) {
        appendError(errors, "IDENTIFIER", "module.name", "must be a Verilog identifier");
        valid = false;
    }

    if (definition.hasDuplicateModuleName) {
        appendError(
            errors,
            "DUPLICATE",
            "module.name",
            QString("%1 is duplicated in the library").arg(definition.moduleName));
        valid = false;
    }
    for (const QString &key : definition.duplicateKeys) {
        appendError(errors, "DUPLICATE", "module." + key, "property is duplicated");
        valid = false;
    }

    const YAML::Node generator = definition.extraAttributes["generator"];
    if (!validateMap(generator, kGeneratorKeys, "generator", errors)) {
        return false;
    }

    QString kind;
    if (!generator["kind"]) {
        appendError(errors, "REQUIRED", "generator.kind", "property is required");
        valid = false;
    } else if (!parseScalar(generator["kind"], "generator.kind", &kind, errors) || kind != "mmio") {
        if (!kind.isEmpty()) {
            appendError(errors, "KIND", "generator.kind", "must be mmio");
        }
        valid = false;
    }

    QString bus;
    if (!generator["bus"]) {
        appendError(errors, "REQUIRED", "generator.bus", "property is required");
        valid = false;
    } else if (!parseScalar(generator["bus"], "generator.bus", &bus, errors) || bus != "axi4_lite") {
        if (!bus.isEmpty()) {
            appendError(errors, "BUS", "generator.bus", "must be axi4_lite");
        }
        valid = false;
    }

    if (definition.hasParameterSection || !definition.parameters.isEmpty()) {
        appendError(errors, "MANUAL_SECTION", "module.parameter", "is not allowed for MMIO modules");
        valid = false;
    }
    if (definition.hasPortSection || !definition.ports.isEmpty()) {
        appendError(errors, "MANUAL_SECTION", "module.port", "is not allowed for MMIO modules");
        valid = false;
    }
    if (definition.hasBusSection || !definition.busInterfaces.isEmpty()) {
        appendError(errors, "MANUAL_SECTION", "module.bus", "is not allowed for MMIO modules");
        valid = false;
    }

    if (!generator["register"]) {
        appendError(errors, "REQUIRED", "generator.register", "property is required");
        return false;
    }
    QSet<QString> ports = kFixedPorts;
    return parseRegisters(generator["register"], plan, &ports, errors) && valid;
}

QString packedRange(quint32 width)
{
    return width == 1 ? QString() : QString(" [%1:0]").arg(width - 1);
}

QString bitRange(const QSocMmioFieldPlan &field)
{
    if (field.width == 1) {
        return QString("[%1]").arg(field.lsb);
    }
    return QString("[%1:%2]").arg(field.lsb + field.width - 1).arg(field.lsb);
}

QString verilogLiteral(quint32 width, quint64 value)
{
    return QString("%1'h%2").arg(width).arg(QString::number(value, 16));
}

void sortPlan(QSocMmioPlan *plan)
{
    std::sort(
        plan->registers.begin(),
        plan->registers.end(),
        [](const QSocMmioRegisterPlan &left, const QSocMmioRegisterPlan &right) {
            return left.byteOffset < right.byteOffset;
        });
    for (QSocMmioRegisterPlan &reg : plan->registers) {
        std::sort(
            reg.fields.begin(),
            reg.fields.end(),
            [](const QSocMmioFieldPlan &left, const QSocMmioFieldPlan &right) {
                return left.lsb < right.lsb;
            });
    }
}

QString storageName(int index)
{
    return QString("mmio_field_%1_q").arg(index);
}

QStringList modulePorts(const QSocMmioPlan &plan)
{
    QStringList ports = {
        "input  wire        clk_i",         "input  wire        rst_ni",
        "input  wire [31:0] s_axi_awaddr",  "input  wire [2:0]  s_axi_awprot",
        "input  wire        s_axi_awvalid", "output wire        s_axi_awready",
        "input  wire [31:0] s_axi_wdata",   "input  wire [3:0]  s_axi_wstrb",
        "input  wire        s_axi_wvalid",  "output wire        s_axi_wready",
        "output reg  [1:0]  s_axi_bresp",   "output reg         s_axi_bvalid",
        "input  wire        s_axi_bready",  "input  wire [31:0] s_axi_araddr",
        "input  wire [2:0]  s_axi_arprot",  "input  wire        s_axi_arvalid",
        "output wire        s_axi_arready", "output reg  [31:0] s_axi_rdata",
        "output reg  [1:0]  s_axi_rresp",   "output reg         s_axi_rvalid",
        "input  wire        s_axi_rready",
    };
    for (const QSocMmioRegisterPlan &reg : plan.registers) {
        for (const QSocMmioFieldPlan &field : reg.fields) {
            if (!field.inputPort.isEmpty()) {
                ports.append(
                    QString("input  wire%1 %2").arg(packedRange(field.width), field.inputPort));
            }
            if (!field.outputPort.isEmpty()) {
                ports.append(
                    QString("output wire%1 %2").arg(packedRange(field.width), field.outputPort));
            }
        }
    }
    return ports;
}

void appendHeader(QStringList *lines, const QSocMmioPlan &plan)
{
    lines->append("// Generated by QSoC. Do not edit.");
    lines->append(QString("module %1 (").arg(plan.moduleName));
    const QStringList ports = modulePorts(plan);
    for (qsizetype index = 0; index < ports.size(); ++index) {
        const QString suffix = index + 1 == ports.size() ? QString() : QString(",");
        lines->append("    " + ports.at(index) + suffix);
    }
    lines->append(");");
    lines->append(QString());
}

void appendStorage(QStringList *lines, const QSocMmioPlan &plan)
{
    lines->append("localparam [1:0] AXI_RESP_OKAY   = 2'b00;");
    lines->append("localparam [1:0] AXI_RESP_SLVERR = 2'b10;");
    lines->append(QString());
    int storageIndex = 0;
    for (const QSocMmioRegisterPlan &reg : plan.registers) {
        for (const QSocMmioFieldPlan &field : reg.fields) {
            if (field.access == QSocMmioAccess::ReadWrite) {
                lines->append(
                    QString("reg%1 %2;").arg(packedRange(field.width), storageName(storageIndex++)));
            }
        }
    }
    lines->append("reg        aw_pending_q;");
    lines->append("reg [31:0] awaddr_q;");
    lines->append("reg        w_pending_q;");
    lines->append("reg [31:0] wdata_q;");
    lines->append("reg [3:0]  wstrb_q;");
    lines->append(QString());
}

void appendAddressFunction(QStringList *lines, const QSocMmioPlan &plan)
{
    lines->append("function address_is_mapped;");
    lines->append("    input [31:0] address;");
    lines->append("    begin");
    lines->append("        case (address)");
    for (const QSocMmioRegisterPlan &reg : plan.registers) {
        lines->append(QString("            32'h%1: address_is_mapped = 1'b1;")
                          .arg(reg.byteOffset, 8, 16, QLatin1Char('0')));
    }
    lines->append("            default: address_is_mapped = 1'b0;");
    lines->append("        endcase");
    lines->append("    end");
    lines->append("endfunction");
    lines->append(QString());
}

QString readSource(const QSocMmioFieldPlan &field, const QString &fieldStorageName)
{
    if (field.access == QSocMmioAccess::ReadWrite) {
        return fieldStorageName;
    }
    if (!field.inputPort.isEmpty()) {
        return field.inputPort;
    }
    return verilogLiteral(field.width, *field.constantValue);
}

void appendReadFunction(QStringList *lines, const QSocMmioPlan &plan)
{
    lines->append("function [31:0] read_register;");
    lines->append("    input [31:0] address;");
    lines->append("    begin");
    lines->append("        read_register = 32'b0;");
    lines->append("        case (address)");
    int storageIndex = 0;
    for (const QSocMmioRegisterPlan &reg : plan.registers) {
        lines->append(
            QString("            32'h%1: begin").arg(reg.byteOffset, 8, 16, QLatin1Char('0')));
        for (const QSocMmioFieldPlan &field : reg.fields) {
            const QString fieldStorageName = field.access == QSocMmioAccess::ReadWrite
                                                 ? storageName(storageIndex++)
                                                 : QString();
            lines->append(QString("                read_register%1 = %2;")
                              .arg(bitRange(field), readSource(field, fieldStorageName)));
        }
        lines->append("            end");
    }
    lines->append("            default: begin end");
    lines->append("        endcase");
    lines->append("    end");
    lines->append("endfunction");
    lines->append(QString());
}

void appendWriteWires(QStringList *lines)
{
    lines->append("wire aw_take = s_axi_awvalid && s_axi_awready;");
    lines->append("wire w_take  = s_axi_wvalid && s_axi_wready;");
    lines->append("wire [31:0] write_address = aw_pending_q ? awaddr_q : s_axi_awaddr;");
    lines->append("wire [31:0] write_data    = w_pending_q ? wdata_q : s_axi_wdata;");
    lines->append("wire [3:0]  write_strobe  = w_pending_q ? wstrb_q : s_axi_wstrb;");
    lines->append("wire write_fire = !s_axi_bvalid && (aw_pending_q || aw_take)");
    lines->append("                  && (w_pending_q || w_take);");
    lines->append("wire [31:0] write_mask = {{8{write_strobe[3]}}, {8{write_strobe[2]}},");
    lines->append("                          {8{write_strobe[1]}}, {8{write_strobe[0]}}};");
    lines->append(QString());
    lines->append("assign s_axi_awready = rst_ni && !aw_pending_q && !s_axi_bvalid;");
    lines->append("assign s_axi_wready  = rst_ni && !w_pending_q && !s_axi_bvalid;");
    lines->append("assign s_axi_arready = rst_ni && !s_axi_rvalid;");
    lines->append(QString());
}

void appendOutputAssignments(QStringList *lines, const QSocMmioPlan &plan)
{
    int storageIndex = 0;
    for (const QSocMmioRegisterPlan &reg : plan.registers) {
        for (const QSocMmioFieldPlan &field : reg.fields) {
            if (field.access != QSocMmioAccess::ReadWrite) {
                continue;
            }
            const QString fieldStorageName = storageName(storageIndex++);
            if (!field.outputPort.isEmpty()) {
                lines->append(QString("assign %1 = %2;").arg(field.outputPort, fieldStorageName));
            }
        }
    }
    if (!lines->constLast().isEmpty()) {
        lines->append(QString());
    }
}

void appendWriteCase(QStringList *lines, const QSocMmioPlan &plan)
{
    lines->append("            case (write_address)");
    int storageIndex = 0;
    for (const QSocMmioRegisterPlan &reg : plan.registers) {
        bool hasWriteField = false;
        for (const QSocMmioFieldPlan &field : reg.fields) {
            hasWriteField = hasWriteField || field.access == QSocMmioAccess::ReadWrite;
        }
        if (!hasWriteField) {
            continue;
        }
        lines->append(
            QString("                32'h%1: begin").arg(reg.byteOffset, 8, 16, QLatin1Char('0')));
        for (const QSocMmioFieldPlan &field : reg.fields) {
            if (field.access != QSocMmioAccess::ReadWrite) {
                continue;
            }
            const QString fieldStorageName = storageName(storageIndex++);
            const QString range            = bitRange(field);
            lines->append(QString("                    %1 <= (%1 & ~write_mask%2)")
                              .arg(fieldStorageName, range));
            lines->append(
                QString("                        | (write_data%1 & write_mask%1);").arg(range));
        }
        lines->append("                end");
    }
    lines->append("                default: begin end");
    lines->append("            endcase");
}

void appendWriteProcess(QStringList *lines, const QSocMmioPlan &plan)
{
    lines->append("always @(posedge clk_i or negedge rst_ni) begin");
    lines->append("    if (!rst_ni) begin");
    lines->append("        aw_pending_q <= 1'b0;");
    lines->append("        awaddr_q     <= 32'b0;");
    lines->append("        w_pending_q  <= 1'b0;");
    lines->append("        wdata_q      <= 32'b0;");
    lines->append("        wstrb_q      <= 4'b0;");
    lines->append("        s_axi_bresp  <= AXI_RESP_OKAY;");
    lines->append("        s_axi_bvalid <= 1'b0;");
    int storageIndex = 0;
    for (const QSocMmioRegisterPlan &reg : plan.registers) {
        for (const QSocMmioFieldPlan &field : reg.fields) {
            if (field.access == QSocMmioAccess::ReadWrite) {
                lines->append(QString("        %1 <= %2;")
                                  .arg(
                                      storageName(storageIndex++),
                                      verilogLiteral(field.width, *field.resetValue)));
            }
        }
    }
    lines->append("    end else begin");
    lines->append("        if (s_axi_bvalid && s_axi_bready)");
    lines->append("            s_axi_bvalid <= 1'b0;");
    lines->append("        if (aw_take) begin");
    lines->append("            aw_pending_q <= 1'b1;");
    lines->append("            awaddr_q     <= s_axi_awaddr;");
    lines->append("        end");
    lines->append("        if (w_take) begin");
    lines->append("            w_pending_q <= 1'b1;");
    lines->append("            wdata_q     <= s_axi_wdata;");
    lines->append("            wstrb_q     <= s_axi_wstrb;");
    lines->append("        end");
    lines->append("        if (write_fire) begin");
    lines->append("            aw_pending_q <= 1'b0;");
    lines->append("            w_pending_q  <= 1'b0;");
    lines->append("            s_axi_bvalid <= 1'b1;");
    lines->append("            s_axi_bresp  <= address_is_mapped(write_address)");
    lines->append("                            ? AXI_RESP_OKAY : AXI_RESP_SLVERR;");
    lines->append("            if (address_is_mapped(write_address)) begin");
    appendWriteCase(lines, plan);
    lines->append("            end");
    lines->append("        end");
    lines->append("    end");
    lines->append("end");
    lines->append(QString());
}

void appendReadProcess(QStringList *lines)
{
    lines->append("always @(posedge clk_i or negedge rst_ni) begin");
    lines->append("    if (!rst_ni) begin");
    lines->append("        s_axi_rdata  <= 32'b0;");
    lines->append("        s_axi_rresp  <= AXI_RESP_OKAY;");
    lines->append("        s_axi_rvalid <= 1'b0;");
    lines->append("    end else begin");
    lines->append("        if (s_axi_rvalid && s_axi_rready)");
    lines->append("            s_axi_rvalid <= 1'b0;");
    lines->append("        if (s_axi_arvalid && s_axi_arready) begin");
    lines->append("            s_axi_rvalid <= 1'b1;");
    lines->append("            s_axi_rdata  <= read_register(s_axi_araddr);");
    lines->append("            s_axi_rresp  <= address_is_mapped(s_axi_araddr)");
    lines->append("                            ? AXI_RESP_OKAY : AXI_RESP_SLVERR;");
    lines->append("        end");
    lines->append("    end");
    lines->append("end");
    lines->append(QString());
    lines->append("endmodule");
}

QString buildVerilog(const QSocMmioPlan &plan)
{
    QStringList lines;
    appendHeader(&lines, plan);
    appendStorage(&lines, plan);
    appendAddressFunction(&lines, plan);
    appendReadFunction(&lines, plan);
    appendWriteWires(&lines);
    appendOutputAssignments(&lines, plan);
    appendWriteProcess(&lines, plan);
    appendReadProcess(&lines);
    return lines.join('\n') + '\n';
}

} // namespace

bool QSocMmioGenerator::isMmio(const QSocModuleDefinition &definition)
{
    const YAML::Node generator = definition.extraAttributes["generator"];
    if (!generator || !generator.IsMap()) {
        return false;
    }
    const YAML::Node kind = generator["kind"];
    return kind && kind.IsScalar() && kind.Scalar() == "mmio";
}

YAML::Node QSocMmioGenerator::createDraftGenerator()
{
    YAML::Node generator(YAML::NodeType::Map);
    generator["kind"]     = "mmio";
    generator["bus"]      = "axi4_lite";
    generator["register"] = YAML::Node(YAML::NodeType::Map);
    return generator;
}

QStringList QSocMmioGenerator::validate(const QSocModuleDefinition &definition)
{
    QStringList errors;
    buildPlan(definition, nullptr, &errors);
    return errors;
}

bool QSocMmioGenerator::buildPlan(
    const QSocModuleDefinition &definition, QSocMmioPlan *plan, QStringList *errors)
{
    if (plan) {
        *plan = QSocMmioPlan();
    }

    QSocMmioPlan localPlan;
    QStringList  localErrors;
    const bool   valid = parsePlan(definition, &localPlan, &localErrors);
    localErrors.sort(Qt::CaseSensitive);
    if (!valid || !localErrors.isEmpty()) {
        if (errors) {
            *errors = localErrors;
        }
        return false;
    }

    sortPlan(&localPlan);
    if (errors) {
        errors->clear();
    }
    if (plan) {
        *plan = localPlan;
    }
    return true;
}

bool QSocMmioGenerator::generateVerilog(
    const QSocModuleDefinition &definition, QString *verilog, QStringList *errors)
{
    if (verilog) {
        verilog->clear();
    }
    QSocMmioPlan plan;
    QStringList  localErrors;
    if (!buildPlan(definition, &plan, &localErrors)) {
        if (errors) {
            *errors = localErrors;
        }
        return false;
    }
    if (errors) {
        errors->clear();
    }
    if (verilog) {
        *verilog = buildVerilog(plan);
    }
    return true;
}
