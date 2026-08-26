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

enum class MmioAccess { ReadWrite, ReadOnly };

struct MmioField
{
    QString    path;
    quint32    lsb    = 0;
    quint32    width  = 1;
    MmioAccess access = MmioAccess::ReadOnly;
    quint32    reset  = 0;
    quint32    value  = 0;
    QString    input;
    QString    output;
    QString    storage;
};

struct MmioRegister
{
    quint32          offset = 0;
    QList<MmioField> fields;
};

struct MmioConfig
{
    QString             moduleName;
    QList<MmioRegister> registers;
};

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
    const YAML::Node &node, const QString &path, quint64 maximum, quint32 *value, QStringList *errors)
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
    *value = static_cast<quint32>(parsed);
    return true;
}

void validateDescription(const YAML::Node &node, const QString &path, QStringList *errors)
{
    if (node && !node.IsScalar()) {
        appendError(errors, "TYPE", path, "must be a scalar");
    }
}

bool parseFieldShape(
    const YAML::Node &node, const QString &path, MmioField *field, QStringList *errors)
{
    bool valid = true;
    if (!node["lsb"]) {
        appendError(errors, "REQUIRED", path + ".lsb", "property is required");
        valid = false;
    } else {
        valid = parseUnsigned(node["lsb"], path + ".lsb", 31, &field->lsb, errors) && valid;
    }

    if (node["width"]) {
        valid = parseUnsigned(node["width"], path + ".width", 32, &field->width, errors) && valid;
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

bool parseAccess(const YAML::Node &node, const QString &path, MmioField *field, QStringList *errors)
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
        field->access = MmioAccess::ReadWrite;
        return true;
    }
    if (access == "ro") {
        field->access = MmioAccess::ReadOnly;
        return true;
    }
    appendError(errors, "ACCESS", path + ".access", "must be rw or ro");
    return false;
}

bool valueFitsWidth(quint32 value, quint32 width)
{
    const quint64 maximum = width == 32 ? std::numeric_limits<quint32>::max()
                                        : (quint64(1) << width) - 1;
    return value <= maximum;
}

bool parseReadWriteField(
    const YAML::Node &node, const QString &path, MmioField *field, QStringList *errors)
{
    bool valid = true;
    if (!node["reset"]) {
        appendError(errors, "REQUIRED", path + ".reset", "property is required for rw fields");
        valid = false;
    } else {
        valid = parseUnsigned(
                    node["reset"],
                    path + ".reset",
                    std::numeric_limits<quint32>::max(),
                    &field->reset,
                    errors)
                && valid;
        if (valid && !valueFitsWidth(field->reset, field->width)) {
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
        valid = parseIdentifier(node["output"], path + ".output", &field->output, errors) && valid;
    }
    return valid;
}

bool parseReadOnlyField(
    const YAML::Node &node, const QString &path, MmioField *field, QStringList *errors)
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
        valid = parseIdentifier(node["input"], path + ".input", &field->input, errors) && valid;
    }
    if (node["value"]) {
        valid = parseUnsigned(
                    node["value"],
                    path + ".value",
                    std::numeric_limits<quint32>::max(),
                    &field->value,
                    errors)
                && valid;
        if (valid && !valueFitsWidth(field->value, field->width)) {
            appendError(errors, "RANGE", path + ".value", "value does not fit field width");
            valid = false;
        }
    }
    return valid;
}

bool parseField(
    const QString    &name,
    const YAML::Node &node,
    const QString    &path,
    MmioField        *field,
    QStringList      *errors)
{
    if (!validateMap(node, kFieldKeys, path, errors)) {
        return false;
    }
    validateDescription(node["description"], path + ".description", errors);

    bool valid = QSocVerilogUtils::isValidVerilogIdentifier(name);
    if (!valid) {
        appendError(errors, "IDENTIFIER", path, "field name must be a Verilog identifier");
    }
    const bool shapeValid  = parseFieldShape(node, path, field, errors);
    const bool accessValid = parseAccess(node, path, field, errors);
    valid                  = shapeValid && accessValid && valid;
    if (!accessValid) {
        return false;
    }
    if (field->access == MmioAccess::ReadWrite) {
        return parseReadWriteField(node, path, field, errors) && valid;
    }
    return parseReadOnlyField(node, path, field, errors) && valid;
}

bool claimSideband(const MmioField &field, QSet<QString> *ports, QStringList *errors)
{
    const QString name = field.input.isEmpty() ? field.output : field.input;
    if (name.isEmpty()) {
        return true;
    }
    static const QRegularExpression storageNamePattern(QStringLiteral("^mmio_field_[0-9]+_q$"));
    if (ports->contains(name) || kInternalNames.contains(name)
        || storageNamePattern.match(name).hasMatch()) {
        const QString key = field.input.isEmpty() ? ".output" : ".input";
        appendError(errors, "CONFLICT", field.path + key, "port name is already in use");
        return false;
    }
    ports->insert(name);
    return true;
}

bool parseFields(
    const YAML::Node &node,
    const QString    &path,
    QList<MmioField> *fields,
    QSet<QString>    *ports,
    QStringList      *errors)
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

        MmioField field;
        field.path = fieldPath;
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
        if (!claimSideband(field, ports, errors)) {
            valid = false;
            continue;
        }
        fields->append(field);
    }
    return valid;
}

bool parseRegister(
    const QString    &name,
    const YAML::Node &node,
    const QString    &path,
    MmioRegister     *reg,
    QSet<QString>    *ports,
    QStringList      *errors)
{
    if (!validateMap(node, kRegisterKeys, path, errors)) {
        return false;
    }
    validateDescription(node["description"], path + ".description", errors);

    bool valid = QSocVerilogUtils::isValidVerilogIdentifier(name);
    if (!valid) {
        appendError(errors, "IDENTIFIER", path, "register name must be a Verilog identifier");
    }
    if (!node["offset"]) {
        appendError(errors, "REQUIRED", path + ".offset", "property is required");
        valid = false;
    } else {
        valid = parseUnsigned(
                    node["offset"],
                    path + ".offset",
                    std::numeric_limits<quint32>::max(),
                    &reg->offset,
                    errors)
                && valid;
        if (valid && (reg->offset & 3U) != 0) {
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
    const YAML::Node &node, MmioConfig *config, QSet<QString> *ports, QStringList *errors)
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
    QHash<quint32, QString> offsets;
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

        MmioRegister reg;
        if (!parseRegister(name, it->second, registerPath, &reg, ports, errors)) {
            valid = false;
            continue;
        }
        if (offsets.contains(reg.offset)) {
            appendError(
                errors,
                "DUPLICATE",
                registerPath + ".offset",
                QString("duplicates %1").arg(offsets.value(reg.offset)));
            valid = false;
            continue;
        }
        offsets.insert(reg.offset, registerPath + ".offset");
        config->registers.append(reg);
    }
    return valid;
}

bool parseConfig(const QSocModuleDefinition &definition, MmioConfig *config, QStringList *errors)
{
    config->moduleName = definition.moduleName;
    bool valid         = true;
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
    return parseRegisters(generator["register"], config, &ports, errors) && valid;
}

QString packedRange(quint32 width)
{
    return width == 1 ? QString() : QString(" [%1:0]").arg(width - 1);
}

QString bitRange(const MmioField &field)
{
    if (field.width == 1) {
        return QString("[%1]").arg(field.lsb);
    }
    return QString("[%1:%2]").arg(field.lsb + field.width - 1).arg(field.lsb);
}

QString verilogLiteral(quint32 width, quint32 value)
{
    return QString("%1'h%2").arg(width).arg(QString::number(value, 16));
}

void sortConfig(MmioConfig *config)
{
    std::sort(
        config->registers.begin(),
        config->registers.end(),
        [](const MmioRegister &left, const MmioRegister &right) {
            return left.offset < right.offset;
        });
    int storageIndex = 0;
    for (MmioRegister &reg : config->registers) {
        std::sort(
            reg.fields.begin(),
            reg.fields.end(),
            [](const MmioField &left, const MmioField &right) { return left.lsb < right.lsb; });
        for (MmioField &field : reg.fields) {
            if (field.access == MmioAccess::ReadWrite) {
                field.storage = QString("mmio_field_%1_q").arg(storageIndex++);
            }
        }
    }
}

QStringList modulePorts(const MmioConfig &config)
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
    for (const MmioRegister &reg : config.registers) {
        for (const MmioField &field : reg.fields) {
            if (!field.input.isEmpty()) {
                ports.append(QString("input  wire%1 %2").arg(packedRange(field.width), field.input));
            }
            if (!field.output.isEmpty()) {
                ports.append(
                    QString("output wire%1 %2").arg(packedRange(field.width), field.output));
            }
        }
    }
    return ports;
}

void appendHeader(QStringList *lines, const MmioConfig &config)
{
    lines->append("// Generated by QSoC. Do not edit.");
    lines->append(QString("module %1 (").arg(config.moduleName));
    const QStringList ports = modulePorts(config);
    for (qsizetype index = 0; index < ports.size(); ++index) {
        const QString suffix = index + 1 == ports.size() ? QString() : QString(",");
        lines->append("    " + ports.at(index) + suffix);
    }
    lines->append(");");
    lines->append(QString());
}

void appendStorage(QStringList *lines, const MmioConfig &config)
{
    lines->append("localparam [1:0] AXI_RESP_OKAY   = 2'b00;");
    lines->append("localparam [1:0] AXI_RESP_SLVERR = 2'b10;");
    lines->append(QString());
    for (const MmioRegister &reg : config.registers) {
        for (const MmioField &field : reg.fields) {
            if (field.access == MmioAccess::ReadWrite) {
                lines->append(QString("reg%1 %2;").arg(packedRange(field.width), field.storage));
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

void appendAddressFunction(QStringList *lines, const MmioConfig &config)
{
    lines->append("function address_is_mapped;");
    lines->append("    input [31:0] address;");
    lines->append("    begin");
    lines->append("        case (address)");
    for (const MmioRegister &reg : config.registers) {
        lines->append(QString("            32'h%1: address_is_mapped = 1'b1;")
                          .arg(reg.offset, 8, 16, QLatin1Char('0')));
    }
    lines->append("            default: address_is_mapped = 1'b0;");
    lines->append("        endcase");
    lines->append("    end");
    lines->append("endfunction");
    lines->append(QString());
}

QString readSource(const MmioField &field)
{
    if (field.access == MmioAccess::ReadWrite) {
        return field.storage;
    }
    if (!field.input.isEmpty()) {
        return field.input;
    }
    return verilogLiteral(field.width, field.value);
}

void appendReadFunction(QStringList *lines, const MmioConfig &config)
{
    lines->append("function [31:0] read_register;");
    lines->append("    input [31:0] address;");
    lines->append("    begin");
    lines->append("        read_register = 32'b0;");
    lines->append("        case (address)");
    for (const MmioRegister &reg : config.registers) {
        lines->append(QString("            32'h%1: begin").arg(reg.offset, 8, 16, QLatin1Char('0')));
        for (const MmioField &field : reg.fields) {
            lines->append(QString("                read_register%1 = %2;")
                              .arg(bitRange(field), readSource(field)));
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

void appendOutputAssignments(QStringList *lines, const MmioConfig &config)
{
    for (const MmioRegister &reg : config.registers) {
        for (const MmioField &field : reg.fields) {
            if (!field.output.isEmpty()) {
                lines->append(QString("assign %1 = %2;").arg(field.output, field.storage));
            }
        }
    }
    if (!lines->constLast().isEmpty()) {
        lines->append(QString());
    }
}

void appendWriteCase(QStringList *lines, const MmioConfig &config)
{
    lines->append("            case (write_address)");
    for (const MmioRegister &reg : config.registers) {
        bool hasWriteField = false;
        for (const MmioField &field : reg.fields) {
            hasWriteField = hasWriteField || field.access == MmioAccess::ReadWrite;
        }
        if (!hasWriteField) {
            continue;
        }
        lines->append(
            QString("                32'h%1: begin").arg(reg.offset, 8, 16, QLatin1Char('0')));
        for (const MmioField &field : reg.fields) {
            if (field.access != MmioAccess::ReadWrite) {
                continue;
            }
            const QString range = bitRange(field);
            lines->append(
                QString("                    %1 <= (%1 & ~write_mask%2)").arg(field.storage, range));
            lines->append(
                QString("                        | (write_data%1 & write_mask%1);").arg(range));
        }
        lines->append("                end");
    }
    lines->append("                default: begin end");
    lines->append("            endcase");
}

void appendWriteProcess(QStringList *lines, const MmioConfig &config)
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
    for (const MmioRegister &reg : config.registers) {
        for (const MmioField &field : reg.fields) {
            if (field.access == MmioAccess::ReadWrite) {
                lines->append(QString("        %1 <= %2;")
                                  .arg(field.storage, verilogLiteral(field.width, field.reset)));
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
    appendWriteCase(lines, config);
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

QString buildVerilog(MmioConfig config)
{
    sortConfig(&config);
    QStringList lines;
    appendHeader(&lines, config);
    appendStorage(&lines, config);
    appendAddressFunction(&lines, config);
    appendReadFunction(&lines, config);
    appendWriteWires(&lines);
    appendOutputAssignments(&lines, config);
    appendWriteProcess(&lines, config);
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
    MmioConfig  config;
    QStringList errors;
    parseConfig(definition, &config, &errors);
    errors.sort(Qt::CaseSensitive);
    return errors;
}

bool QSocMmioGenerator::generateVerilog(
    const QSocModuleDefinition &definition, QString *verilog, QStringList *errors)
{
    if (verilog) {
        verilog->clear();
    }
    MmioConfig  config;
    QStringList localErrors;
    if (!parseConfig(definition, &config, &localErrors) || !localErrors.isEmpty()) {
        localErrors.sort(Qt::CaseSensitive);
        if (errors) {
            *errors = localErrors;
        }
        return false;
    }
    if (errors) {
        errors->clear();
    }
    if (verilog) {
        *verilog = buildVerilog(config);
    }
    return true;
}
