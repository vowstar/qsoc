// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocmmiogenerator.h"

#include "common/qsocmmioformal.h"
#include "common/qsocmmiouvm.h"
#include "common/qsocmodulemanager.h"
#include "common/qsocverilogutils.h"

#include <algorithm>
#include <limits>
#include <QHash>
#include <QRegularExpression>
#include <QSet>

namespace {

const QSet<QString> kGeneratorKeys
    = {"kind", "bus", "data_width", "address_width", "identity", "register"};
const QSet<QString> kIdentityKeys = {"type", "version"};
const QSet<QString> kRegisterKeys = {"offset", "field", "description"};
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

quint64 maximumForWidth(quint32 width)
{
    return width == 64 ? std::numeric_limits<quint64>::max() : (quint64(1) << width) - 1;
}

bool parseBusWidths(const YAML::Node &generator, QSocMmioPlan *plan, QStringList *errors)
{
    bool valid = true;
    if (generator["data_width"]) {
        quint32    dataWidth = 0;
        const bool parsed    = parseUnsigned32(
            generator["data_width"],
            "generator.data_width",
            std::numeric_limits<quint32>::max(),
            &dataWidth,
            errors);
        if (parsed) {
            plan->dataWidth = dataWidth;
        }
        valid = parsed && valid;
    }

    if (generator["address_width"]) {
        quint32    addressWidth = 0;
        const bool parsed       = parseUnsigned32(
            generator["address_width"],
            "generator.address_width",
            std::numeric_limits<quint32>::max(),
            &addressWidth,
            errors);
        if (parsed) {
            plan->addressWidth = addressWidth;
        }
        valid = parsed && valid;
    }
    return valid;
}

bool parseFieldShape(
    const YAML::Node &node, const QString &path, QSocMmioFieldPlan *field, QStringList *errors)
{
    bool valid = true;
    if (!node["lsb"]) {
        appendError(errors, "REQUIRED", path + ".lsb", "property is required");
        valid = false;
    } else {
        valid = parseUnsigned32(
                    node["lsb"],
                    path + ".lsb",
                    std::numeric_limits<quint32>::max(),
                    &field->lsb,
                    errors)
                && valid;
    }

    if (node["width"]) {
        valid = parseUnsigned32(
                    node["width"],
                    path + ".width",
                    std::numeric_limits<quint32>::max(),
                    &field->width,
                    errors)
                && valid;
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
    if (access == "w1c") {
        field->access = QSocMmioAccess::WriteOneClear;
        return true;
    }
    appendError(errors, "ACCESS", path + ".access", "must be rw, ro, or w1c");
    return false;
}

bool valueFitsWidth(quint64 value, quint32 width)
{
    return value <= maximumForWidth(width);
}

bool parseFieldValues(
    const YAML::Node &node, const QString &path, QSocMmioFieldPlan *field, QStringList *errors)
{
    bool valid = true;
    if (node["reset"]) {
        quint64    resetValue = 0;
        const bool resetValid = parseUnsigned(
            node["reset"], path + ".reset", std::numeric_limits<quint64>::max(), &resetValue, errors);
        if (resetValid) {
            field->resetValue = resetValue;
        }
        valid = resetValid && valid;
    }
    if (node["input"]) {
        valid = parseScalar(node["input"], path + ".input", &field->inputPort, errors) && valid;
    }
    if (node["output"]) {
        valid = parseScalar(node["output"], path + ".output", &field->outputPort, errors) && valid;
    }
    if (node["value"]) {
        quint64    constantValue = 0;
        const bool valueValid    = parseUnsigned(
            node["value"],
            path + ".value",
            std::numeric_limits<quint64>::max(),
            &constantValue,
            errors);
        if (valueValid) {
            field->constantValue = constantValue;
        }
        valid = valueValid && valid;
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

    const bool shapeValid  = parseFieldShape(node, path, field, errors);
    const bool accessValid = parseAccess(node, path, field, errors);
    const bool valuesValid = parseFieldValues(node, path, field, errors);
    return shapeValid && accessValid && valuesValid;
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
    QStringList              *errors)
{
    if (!node || !node.IsMap()) {
        appendError(errors, "TYPE", path, "must be a map");
        return false;
    }

    bool valid = true;
    for (YAML::const_iterator it = node.begin(); it != node.end(); ++it) {
        if (!it->first.IsScalar()) {
            appendError(errors, "TYPE", path, "field names must be scalar");
            valid = false;
            continue;
        }
        const QString name      = QString::fromStdString(it->first.Scalar());
        const QString fieldPath = path + "." + name;

        QSocMmioFieldPlan field;
        if (!parseField(name, it->second, fieldPath, &field, errors)) {
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
    QStringList          *errors)
{
    if (!validateMap(node, kRegisterKeys, path, errors)) {
        return false;
    }
    reg->name = name;
    parseDescription(node["description"], path + ".description", &reg->description, errors);

    bool valid = true;
    if (!node["offset"]) {
        appendError(errors, "REQUIRED", path + ".offset", "property is required");
        valid = false;
    } else {
        valid = parseUnsigned(
                    node["offset"],
                    path + ".offset",
                    std::numeric_limits<quint64>::max(),
                    &reg->byteOffset,
                    errors)
                && valid;
    }
    if (!node["field"]) {
        appendError(errors, "REQUIRED", path + ".field", "property is required");
        return false;
    }
    return parseFields(node["field"], path + ".field", &reg->fields, errors) && valid;
}

bool parseRegisters(const YAML::Node &node, QSocMmioPlan *plan, QStringList *errors)
{
    const QString path = "generator.register";
    if (!node || !node.IsMap()) {
        appendError(errors, "TYPE", path, "must be a map");
        return false;
    }

    bool valid = true;
    for (YAML::const_iterator it = node.begin(); it != node.end(); ++it) {
        if (!it->first.IsScalar()) {
            appendError(errors, "TYPE", path, "register names must be scalar");
            valid = false;
            continue;
        }
        const QString name         = QString::fromStdString(it->first.Scalar());
        const QString registerPath = path + "." + name;

        QSocMmioRegisterPlan reg;
        if (!parseRegister(name, it->second, registerPath, &reg, errors)) {
            valid = false;
            continue;
        }
        plan->registers.append(reg);
    }
    return valid;
}

QSocMmioFieldPlan identityField(const QString &name, quint32 lsb, quint32 width, quint64 value)
{
    QSocMmioFieldPlan field;
    field.name          = name;
    field.lsb           = lsb;
    field.width         = width;
    field.access        = QSocMmioAccess::ReadOnly;
    field.constantValue = value;
    return field;
}

/**
 * @brief Read `generator.identity` and prepend its words to the map.
 *
 * `version` is "major.minor.patch" with each part below 256. `type` is a
 * number, or exactly four printable ASCII characters packed so the first
 * lands in the top byte, which is how the word reads in a register view.
 * The words take byte offsets 0x0 and 0x4; a 64-bit instance holds both in
 * one beat. A user register on those offsets or with those names is an error
 * rather than a silent overlap.
 */
bool parseIdentity(const YAML::Node &node, QSocMmioPlan *plan, QStringList *errors)
{
    const QString path = "generator.identity";
    if (!validateMap(node, kIdentityKeys, path, errors)) {
        return false;
    }
    bool    valid   = true;
    quint64 typeId  = 0;
    quint64 version = 0;
    if (!node["type"]) {
        appendError(errors, "REQUIRED", path + ".type", "property is required");
        valid = false;
    } else {
        QString text;
        if (!parseScalar(node["type"], path + ".type", &text, errors)) {
            valid = false;
        } else {
            static const QRegularExpression numberPattern(
                QStringLiteral("^(?:0[xX][0-9a-fA-F]+|[0-9]+)$"));
            if (numberPattern.match(text).hasMatch()) {
                valid = parseUnsigned(node["type"], path + ".type", 0xffffffffULL, &typeId, errors)
                        && valid;
            } else if (text.size() != 4 || !std::all_of(text.cbegin(), text.cend(), [](QChar c) {
                           return c.unicode() >= 0x21 && c.unicode() <= 0x7e;
                       })) {
                appendError(
                    errors,
                    "TYPE",
                    path + ".type",
                    "must be a number or four printable ASCII characters");
                valid = false;
            } else {
                for (const QChar c : text) {
                    typeId = (typeId << 8) | quint64(c.unicode());
                }
            }
        }
    }
    if (!node["version"]) {
        appendError(errors, "REQUIRED", path + ".version", "property is required");
        valid = false;
    } else {
        QString text;
        if (!parseScalar(node["version"], path + ".version", &text, errors)) {
            valid = false;
        } else {
            static const QRegularExpression versionPattern(
                QStringLiteral("^([0-9]{1,3})\\.([0-9]{1,3})\\.([0-9]{1,3})$"));
            const QRegularExpressionMatch match = versionPattern.match(text);
            bool                          fits  = match.hasMatch();
            for (int part = 1; fits && part <= 3; ++part) {
                fits = match.captured(part).toUInt() <= 255;
            }
            if (!fits) {
                appendError(
                    errors,
                    "TYPE",
                    path + ".version",
                    "must be major.minor.patch with each part below 256");
                valid = false;
            } else {
                version = (quint64(match.captured(1).toUInt()) << 24)
                          | (quint64(match.captured(2).toUInt()) << 16)
                          | (quint64(match.captured(3).toUInt()) << 8);
            }
        }
    }
    if (!valid) {
        return false;
    }
    const bool wide = plan->dataWidth == 64;
    for (const QSocMmioRegisterPlan &reg : plan->registers) {
        if (reg.name == QStringLiteral("version") || reg.name == QStringLiteral("type")) {
            appendError(
                errors,
                "IDENTITY",
                "generator.register." + reg.name,
                "name is taken by generator.identity");
            valid = false;
        }
        if (reg.byteOffset < 8) {
            appendError(
                errors,
                "IDENTITY",
                "generator.register." + reg.name,
                QString("offset 0x%1 is taken by generator.identity").arg(reg.byteOffset, 0, 16));
            valid = false;
        }
    }
    if (!valid) {
        return false;
    }
    QSocMmioRegisterPlan versionReg;
    versionReg.name        = QStringLiteral("version");
    versionReg.description = QStringLiteral("Layout version");
    versionReg.byteOffset  = 0;
    versionReg.fields.append(identityField(QStringLiteral("major"), 24, 8, (version >> 24) & 0xff));
    versionReg.fields.append(identityField(QStringLiteral("minor"), 16, 8, (version >> 16) & 0xff));
    versionReg.fields.append(identityField(QStringLiteral("patch"), 8, 8, (version >> 8) & 0xff));
    QSocMmioRegisterPlan typeReg;
    typeReg.name        = QStringLiteral("type");
    typeReg.description = QStringLiteral("Block type");
    typeReg.byteOffset  = 4;
    if (wide) {
        versionReg.fields.append(identityField(QStringLiteral("type_id"), 32, 32, typeId));
        plan->registers.prepend(versionReg);
    } else {
        typeReg.fields.append(identityField(QStringLiteral("type_id"), 0, 32, typeId));
        plan->registers.prepend(typeReg);
        plan->registers.prepend(versionReg);
    }
    return true;
}

bool parsePlan(const QSocModuleDefinition &definition, QSocMmioPlan *plan, QStringList *errors)
{
    plan->moduleName = definition.moduleName;

    if (definition.hasDuplicateModuleName) {
        appendError(
            errors,
            "DUPLICATE",
            "module.name",
            QString("%1 is duplicated in the library").arg(definition.moduleName));
    }
    for (const QString &key : definition.duplicateKeys) {
        appendError(errors, "DUPLICATE", "module." + key, "property is duplicated");
    }

    const YAML::Node generator = definition.extraAttributes["generator"];
    if (!validateMap(generator, kGeneratorKeys, "generator", errors)) {
        return false;
    }

    QString kind;
    if (!generator["kind"]) {
        appendError(errors, "REQUIRED", "generator.kind", "property is required");
    } else {
        const bool kindDecodable = parseScalar(generator["kind"], "generator.kind", &kind, errors);
        // cppcheck-suppress knownConditionTrueFalse
        if (kindDecodable && kind != "mmio") {
            appendError(errors, "KIND", "generator.kind", "must be mmio");
        }
    }

    QString bus;
    if (!generator["bus"]) {
        appendError(errors, "REQUIRED", "generator.bus", "property is required");
    } else {
        const bool busDecodable = parseScalar(generator["bus"], "generator.bus", &bus, errors);
        // cppcheck-suppress knownConditionTrueFalse
        if (busDecodable && bus != "axi4_lite") {
            appendError(errors, "BUS", "generator.bus", "must be axi4_lite");
        }
    }

    const bool widthsDecodable = parseBusWidths(generator, plan, errors);

    if (definition.hasParameterSection || !definition.parameters.isEmpty()) {
        appendError(errors, "MANUAL_SECTION", "module.parameter", "is not allowed for MMIO modules");
    }
    if (definition.hasPortSection || !definition.ports.isEmpty()) {
        appendError(errors, "MANUAL_SECTION", "module.port", "is not allowed for MMIO modules");
    }
    if (definition.hasBusSection || !definition.busInterfaces.isEmpty()) {
        appendError(errors, "MANUAL_SECTION", "module.bus", "is not allowed for MMIO modules");
    }

    if (!generator["register"]) {
        appendError(errors, "REQUIRED", "generator.register", "property is required");
        return false;
    }
    const bool registersDecodable = parseRegisters(generator["register"], plan, errors);
    bool       identityDecodable  = true;
    if (generator["identity"] && widthsDecodable && registersDecodable) {
        identityDecodable = parseIdentity(generator["identity"], plan, errors);
    }
    return widthsDecodable && registersDecodable && identityDecodable;
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

QString zeroLiteral(quint32 width)
{
    return QString("%1'b0").arg(width);
}

QString addressLiteral(const QSocMmioPlan &plan, quint64 byteOffset)
{
    const int digits = static_cast<int>((plan.addressWidth + 3) / 4);
    return QString("%1'h%2").arg(plan.addressWidth).arg(byteOffset, digits, 16, QLatin1Char('0'));
}

QString localAddressName(const QSocMmioPlan &plan, const QString &base)
{
    QSet<QString> usedNames = kFixedPorts;
    usedNames.unite(kInternalNames);
    for (const QSocMmioRegisterPlan &reg : plan.registers) {
        for (const QSocMmioFieldPlan &field : reg.fields) {
            if (!field.inputPort.isEmpty()) {
                usedNames.insert(field.inputPort);
            }
            if (!field.outputPort.isEmpty()) {
                usedNames.insert(field.outputPort);
            }
        }
    }

    QString name = base;
    while (usedNames.contains(name)) {
        name += QLatin1Char('_');
    }
    return name;
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

bool validatePlanInvariants(const QSocMmioPlan &plan, QStringList *errors)
{
    bool valid = true;
    if (!QSocVerilogUtils::isValidVerilogIdentifier(plan.moduleName)) {
        appendError(errors, "IDENTIFIER", "module.name", "must be a Verilog identifier");
        valid = false;
    }
    if (plan.dataWidth != 32 && plan.dataWidth != 64) {
        appendError(errors, "RANGE", "generator.data_width", "must be 32 or 64");
        return false;
    }
    const quint32 minimumAddressWidth = plan.dataWidth == 64 ? 3 : 2;
    if (plan.addressWidth < minimumAddressWidth || plan.addressWidth > 64) {
        appendError(
            errors,
            "RANGE",
            "generator.address_width",
            QString("must be between %1 and 64").arg(minimumAddressWidth));
        return false;
    }
    if (plan.registers.isEmpty()) {
        appendError(errors, "EMPTY", "generator.register", "must contain at least one register");
        return false;
    }

    QSet<QString>           registerNames;
    QHash<quint64, QString> offsets;
    QSet<QString>           ports     = kFixedPorts;
    const quint64           byteCount = plan.dataWidth / 8;
    for (const QSocMmioRegisterPlan &reg : plan.registers) {
        const QString registerPath = "generator.register." + reg.name;
        if (!QSocVerilogUtils::isValidVerilogIdentifier(reg.name)) {
            appendError(
                errors, "IDENTIFIER", registerPath, "register name must be a Verilog identifier");
            valid = false;
        }
        if (registerNames.contains(reg.name)) {
            appendError(errors, "DUPLICATE", registerPath, "register is duplicated");
            valid = false;
            continue;
        }
        registerNames.insert(reg.name);
        if (reg.byteOffset > maximumForWidth(plan.addressWidth)) {
            appendError(
                errors,
                "RANGE",
                registerPath + ".offset",
                QString("must be at most %1").arg(maximumForWidth(plan.addressWidth)));
            valid = false;
        }
        if ((reg.byteOffset & (byteCount - 1)) != 0) {
            appendError(
                errors,
                "ALIGNMENT",
                registerPath + ".offset",
                QString("must be %1-byte aligned").arg(byteCount));
            valid = false;
        }
        if (offsets.contains(reg.byteOffset)) {
            appendError(
                errors,
                "DUPLICATE",
                registerPath + ".offset",
                QString("duplicates %1").arg(offsets.value(reg.byteOffset)));
            valid = false;
        } else {
            offsets.insert(reg.byteOffset, registerPath + ".offset");
        }
        if (reg.fields.isEmpty()) {
            appendError(errors, "EMPTY", registerPath + ".field", "must contain at least one field");
            valid = false;
            continue;
        }

        quint64       occupied = 0;
        QSet<QString> fieldNames;
        for (const QSocMmioFieldPlan &field : reg.fields) {
            const QString fieldPath = registerPath + ".field." + field.name;
            if (!QSocVerilogUtils::isValidVerilogIdentifier(field.name)) {
                appendError(
                    errors, "IDENTIFIER", fieldPath, "field name must be a Verilog identifier");
                valid = false;
            }
            if (fieldNames.contains(field.name)) {
                appendError(errors, "DUPLICATE", fieldPath, "field is duplicated");
                valid = false;
                continue;
            }
            fieldNames.insert(field.name);
            bool fieldShapeValid = true;
            if (field.width == 0) {
                appendError(errors, "RANGE", fieldPath + ".width", "must be at least 1");
                valid           = false;
                fieldShapeValid = false;
            } else if (field.width > plan.dataWidth) {
                appendError(
                    errors,
                    "RANGE",
                    fieldPath + ".width",
                    QString("must be at most %1").arg(plan.dataWidth));
                valid           = false;
                fieldShapeValid = false;
            }
            if (field.lsb >= plan.dataWidth) {
                appendError(
                    errors,
                    "RANGE",
                    fieldPath + ".lsb",
                    QString("must be at most %1").arg(plan.dataWidth - 1));
                valid           = false;
                fieldShapeValid = false;
            } else if (fieldShapeValid && quint64(field.lsb) + field.width > plan.dataWidth) {
                appendError(
                    errors,
                    "RANGE",
                    fieldPath,
                    QString("field must fit within %1 bits").arg(plan.dataWidth));
                valid           = false;
                fieldShapeValid = false;
            }
            if (fieldShapeValid) {
                const quint64 mask = maximumForWidth(field.width) << field.lsb;
                if ((occupied & mask) != 0) {
                    appendError(errors, "OVERLAP", fieldPath, "field overlaps another field");
                    valid = false;
                } else {
                    occupied |= mask;
                }
            }
            if (!field.inputPort.isEmpty()
                && !QSocVerilogUtils::isValidVerilogIdentifier(field.inputPort)) {
                appendError(
                    errors, "IDENTIFIER", fieldPath + ".input", "must be a Verilog identifier");
                valid = false;
            }
            if (!field.outputPort.isEmpty()
                && !QSocVerilogUtils::isValidVerilogIdentifier(field.outputPort)) {
                appendError(
                    errors, "IDENTIFIER", fieldPath + ".output", "must be a Verilog identifier");
                valid = false;
            }
            if (qsocMmioHasStorage(field.access)) {
                if (!field.resetValue.has_value()) {
                    appendError(
                        errors,
                        "REQUIRED",
                        fieldPath + ".reset",
                        "property is required for rw fields");
                    valid = false;
                } else if (
                    field.width > 0 && field.width <= plan.dataWidth
                    && !valueFitsWidth(*field.resetValue, field.width)) {
                    appendError(
                        errors, "RANGE", fieldPath + ".reset", "value does not fit field width");
                    valid = false;
                }
                if (field.access == QSocMmioAccess::ReadWrite && !field.inputPort.isEmpty()) {
                    appendError(
                        errors, "ACCESS", fieldPath + ".input", "is not allowed for rw fields");
                    valid = false;
                }
                if (field.constantValue.has_value()) {
                    appendError(
                        errors, "ACCESS", fieldPath + ".value", "is not allowed for rw fields");
                    valid = false;
                }
                if (field.access == QSocMmioAccess::WriteOneClear) {
                    if (field.width != 1) {
                        appendError(errors, "RANGE", fieldPath + ".width", "must be 1 for w1c");
                        valid = false;
                    }
                    if (field.inputPort.isEmpty()) {
                        appendError(
                            errors, "REQUIRED", fieldPath + ".input", "w1c needs a set source");
                        valid = false;
                    }
                }
            } else if (field.access == QSocMmioAccess::ReadOnly) {
                const int sources = (field.inputPort.isEmpty() ? 0 : 1)
                                    + (field.constantValue.has_value() ? 1 : 0);
                if (sources != 1) {
                    appendError(
                        errors,
                        "SOURCE",
                        fieldPath,
                        "ro fields require exactly one of input or value");
                    valid = false;
                }
                if (field.constantValue.has_value() && field.width > 0
                    && field.width <= plan.dataWidth
                    && !valueFitsWidth(*field.constantValue, field.width)) {
                    appendError(
                        errors, "RANGE", fieldPath + ".value", "value does not fit field width");
                    valid = false;
                }
                if (field.resetValue.has_value()) {
                    appendError(
                        errors, "ACCESS", fieldPath + ".reset", "is not allowed for ro fields");
                    valid = false;
                }
                if (!field.outputPort.isEmpty()) {
                    appendError(
                        errors, "ACCESS", fieldPath + ".output", "is not allowed for ro fields");
                    valid = false;
                }
            } else {
                appendError(errors, "ACCESS", fieldPath + ".access", "must be rw, ro, or w1c");
                valid = false;
            }
            valid = claimSideband(field, fieldPath, &ports, errors) && valid;
        }
    }
    return valid;
}

QStringList modulePorts(const QSocMmioPlan &plan)
{
    QStringList ports = {
        "input  wire        clk_i",
        "input  wire        rst_ni",
        QString("input  wire [%1:0] s_axi_awaddr").arg(plan.addressWidth - 1),
        "input  wire [2:0]  s_axi_awprot",
        "input  wire        s_axi_awvalid",
        "output wire        s_axi_awready",
        QString("input  wire [%1:0] s_axi_wdata").arg(plan.dataWidth - 1),
        QString("input  wire [%1:0]  s_axi_wstrb").arg(plan.dataWidth / 8 - 1),
        "input  wire        s_axi_wvalid",
        "output wire        s_axi_wready",
        "output reg  [1:0]  s_axi_bresp",
        "output reg         s_axi_bvalid",
        "input  wire        s_axi_bready",
        QString("input  wire [%1:0] s_axi_araddr").arg(plan.addressWidth - 1),
        "input  wire [2:0]  s_axi_arprot",
        "input  wire        s_axi_arvalid",
        "output wire        s_axi_arready",
        QString("output reg  [%1:0] s_axi_rdata").arg(plan.dataWidth - 1),
        "output reg  [1:0]  s_axi_rresp",
        "output reg         s_axi_rvalid",
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
            if (qsocMmioHasStorage(field.access)) {
                lines->append(
                    QString("reg%1 %2;").arg(packedRange(field.width), storageName(storageIndex++)));
            }
        }
    }
    lines->append("reg        aw_pending_q;");
    lines->append(QString("reg [%1:0] awaddr_q;").arg(plan.addressWidth - 1));
    lines->append("reg        w_pending_q;");
    lines->append(QString("reg [%1:0] wdata_q;").arg(plan.dataWidth - 1));
    lines->append(QString("reg [%1:0]  wstrb_q;").arg(plan.dataWidth / 8 - 1));
    lines->append(QString());
}

void appendAddressFunction(QStringList *lines, const QSocMmioPlan &plan)
{
    const QString addressName = localAddressName(plan, QStringLiteral("address"));
    lines->append("function address_is_mapped;");
    lines->append(QString("    input [%1:0] %2;").arg(plan.addressWidth - 1).arg(addressName));
    lines->append("    begin");
    lines->append(QString("        case (%1)").arg(addressName));
    for (const QSocMmioRegisterPlan &reg : plan.registers) {
        lines->append(QString("            %1: address_is_mapped = 1'b1;")
                          .arg(addressLiteral(plan, reg.byteOffset)));
    }
    lines->append("            default: address_is_mapped = 1'b0;");
    lines->append("        endcase");
    lines->append("    end");
    lines->append("endfunction");
    lines->append(QString());
}

QString readSource(const QSocMmioFieldPlan &field, const QString &fieldStorageName)
{
    if (qsocMmioHasStorage(field.access)) {
        return fieldStorageName;
    }
    if (!field.inputPort.isEmpty()) {
        return field.inputPort;
    }
    return verilogLiteral(field.width, *field.constantValue);
}

void appendReadFunction(QStringList *lines, const QSocMmioPlan &plan)
{
    const QString addressName = localAddressName(plan, QStringLiteral("address"));
    lines->append(QString("function [%1:0] read_register;").arg(plan.dataWidth - 1));
    lines->append(QString("    input [%1:0] %2;").arg(plan.addressWidth - 1).arg(addressName));
    lines->append("    begin");
    lines->append("        read_register = " + zeroLiteral(plan.dataWidth) + ";");
    lines->append(QString("        case (%1)").arg(addressName));
    int storageIndex = 0;
    for (const QSocMmioRegisterPlan &reg : plan.registers) {
        lines->append(QString("            %1: begin").arg(addressLiteral(plan, reg.byteOffset)));
        for (const QSocMmioFieldPlan &field : reg.fields) {
            const QString fieldStorageName = qsocMmioHasStorage(field.access)
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

void appendWriteWires(QStringList *lines, const QSocMmioPlan &plan)
{
    lines->append("wire aw_take = s_axi_awvalid && s_axi_awready;");
    lines->append("wire w_take  = s_axi_wvalid && s_axi_wready;");
    lines->append(QString("wire [%1:0] write_address = aw_pending_q ? awaddr_q : s_axi_awaddr;")
                      .arg(plan.addressWidth - 1));
    lines->append(QString("wire [%1:0] write_data    = w_pending_q ? wdata_q : s_axi_wdata;")
                      .arg(plan.dataWidth - 1));
    lines->append(QString("wire [%1:0]  write_strobe  = w_pending_q ? wstrb_q : s_axi_wstrb;")
                      .arg(plan.dataWidth / 8 - 1));
    lines->append("wire write_fire = !s_axi_bvalid && (aw_pending_q || aw_take)");
    lines->append("                  && (w_pending_q || w_take);");
    QStringList lanes;
    for (int lane = static_cast<int>(plan.dataWidth / 8) - 1; lane >= 0; --lane) {
        lanes.append(QString("{8{write_strobe[%1]}}").arg(lane));
    }
    for (qsizetype index = 0; index < lanes.size(); index += 2) {
        const QString prefix = index == 0
                                   ? QString("wire [%1:0] write_mask = {").arg(plan.dataWidth - 1)
                                   : QString(26, QLatin1Char(' '));
        const QString suffix = index + 2 >= lanes.size() ? QString("};") : QString(",");
        lines->append(prefix + lanes.mid(index, 2).join(", ") + suffix);
    }
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
            if (!qsocMmioHasStorage(field.access)) {
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
            hasWriteField = hasWriteField || qsocMmioHasStorage(field.access);
        }
        if (!hasWriteField) {
            continue;
        }
        lines->append(
            QString("                %1: begin").arg(addressLiteral(plan, reg.byteOffset)));
        for (const QSocMmioFieldPlan &field : reg.fields) {
            if (!qsocMmioHasStorage(field.access)) {
                continue;
            }
            const QString fieldStorageName = storageName(storageIndex++);
            const QString range            = bitRange(field);
            if (field.access == QSocMmioAccess::WriteOneClear) {
                lines->append(QString("                    %1 <= %1").arg(fieldStorageName));
                lines->append(
                    QString("                        & ~(write_data%1 & write_mask%1);").arg(range));
                continue;
            }
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

/**
 * @brief Emit the hardware set for every write-one-clear field.
 *
 * This runs after the bus write in the same block, so a set that lands on the
 * cycle software acknowledges the old event keeps the new one.
 */
void appendHardwareSet(QStringList *lines, const QSocMmioPlan &plan)
{
    int storageIndex = 0;
    for (const QSocMmioRegisterPlan &reg : plan.registers) {
        for (const QSocMmioFieldPlan &field : reg.fields) {
            if (!qsocMmioHasStorage(field.access)) {
                continue;
            }
            const QString fieldStorageName = storageName(storageIndex++);
            if (field.access != QSocMmioAccess::WriteOneClear || field.inputPort.isEmpty()) {
                continue;
            }
            /* Conditional, so a cycle with no event leaves the bus write intact. */
            lines->append(QString("        if (%1)").arg(field.inputPort));
            lines->append(QString("            %1 <= 1'b1;").arg(fieldStorageName));
        }
    }
}

void appendWriteProcess(QStringList *lines, const QSocMmioPlan &plan)
{
    lines->append("always @(posedge clk_i or negedge rst_ni) begin");
    lines->append("    if (!rst_ni) begin");
    lines->append("        aw_pending_q <= 1'b0;");
    lines->append("        awaddr_q     <= " + zeroLiteral(plan.addressWidth) + ";");
    lines->append("        w_pending_q  <= 1'b0;");
    lines->append("        wdata_q      <= " + zeroLiteral(plan.dataWidth) + ";");
    lines->append("        wstrb_q      <= " + zeroLiteral(plan.dataWidth / 8) + ";");
    lines->append("        s_axi_bresp  <= AXI_RESP_OKAY;");
    lines->append("        s_axi_bvalid <= 1'b0;");
    int storageIndex = 0;
    for (const QSocMmioRegisterPlan &reg : plan.registers) {
        for (const QSocMmioFieldPlan &field : reg.fields) {
            if (qsocMmioHasStorage(field.access)) {
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
    appendHardwareSet(lines, plan);
    lines->append("    end");
    lines->append("end");
    lines->append(QString());
}

void appendReadProcess(QStringList *lines, const QSocMmioPlan &plan)
{
    lines->append("always @(posedge clk_i or negedge rst_ni) begin");
    lines->append("    if (!rst_ni) begin");
    lines->append("        s_axi_rdata  <= " + zeroLiteral(plan.dataWidth) + ";");
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
    appendWriteWires(&lines, plan);
    appendOutputAssignments(&lines, plan);
    appendWriteProcess(&lines, plan);
    appendReadProcess(&lines, plan);
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

QStringList QSocMmioGenerator::advise(const QSocModuleDefinition &definition)
{
    QStringList      advice;
    const YAML::Node generator = definition.extraAttributes["generator"];
    if (generator && generator.IsMap() && !generator["identity"]) {
        advice.append(QStringLiteral(
            "MMIO_IDENTITY generator.identity: absent, add type and version so software can "
            "recognise the block"));
    }
    return advice;
}

bool QSocMmioGenerator::buildPlan(
    const QSocModuleDefinition &definition, QSocMmioPlan *plan, QStringList *errors)
{
    if (plan) {
        *plan = QSocMmioPlan();
    }

    QSocMmioPlan localPlan;
    QStringList  localErrors;
    const bool   parsed = parsePlan(definition, &localPlan, &localErrors);
    bool         valid  = parsed;
    if (parsed) {
        QStringList canonicalErrors;
        valid = canonicalizePlan(&localPlan, &canonicalErrors);
        localErrors.append(canonicalErrors);
    }
    localErrors.sort(Qt::CaseSensitive);
    if (!valid || !localErrors.isEmpty()) {
        if (errors) {
            *errors = localErrors;
        }
        return false;
    }

    if (errors) {
        errors->clear();
    }
    if (plan) {
        *plan = localPlan;
    }
    return true;
}

bool QSocMmioGenerator::canonicalizePlan(QSocMmioPlan *plan, QStringList *errors)
{
    QStringList localErrors;
    const bool  valid = validatePlanInvariants(*plan, &localErrors);
    localErrors.sort(Qt::CaseSensitive);
    if (!valid || !localErrors.isEmpty()) {
        if (errors) {
            *errors = localErrors;
        }
        return false;
    }
    sortPlan(plan);
    if (errors) {
        errors->clear();
    }
    return true;
}

QString QSocMmioGenerator::generateVerilog(const QSocMmioPlan &plan)
{
    return buildVerilog(plan);
}

QList<QSocMmioPortDescription> QSocMmioGenerator::describePorts(const QSocMmioPlan &plan)
{
    QList<QSocMmioPortDescription> ports
        = {{"clk_i", "input", 1},
           {"rst_ni", "input", 1},
           {"s_axi_awaddr", "input", plan.addressWidth},
           {"s_axi_awprot", "input", 3},
           {"s_axi_awvalid", "input", 1},
           {"s_axi_awready", "output", 1},
           {"s_axi_wdata", "input", plan.dataWidth},
           {"s_axi_wstrb", "input", plan.dataWidth / 8},
           {"s_axi_wvalid", "input", 1},
           {"s_axi_wready", "output", 1},
           {"s_axi_bresp", "output", 2},
           {"s_axi_bvalid", "output", 1},
           {"s_axi_bready", "input", 1},
           {"s_axi_araddr", "input", plan.addressWidth},
           {"s_axi_arprot", "input", 3},
           {"s_axi_arvalid", "input", 1},
           {"s_axi_arready", "output", 1},
           {"s_axi_rdata", "output", plan.dataWidth},
           {"s_axi_rresp", "output", 2},
           {"s_axi_rvalid", "output", 1},
           {"s_axi_rready", "input", 1}};
    for (const QSocMmioRegisterPlan &reg : plan.registers) {
        for (const QSocMmioFieldPlan &field : reg.fields) {
            if (!field.inputPort.isEmpty()) {
                ports.append({field.inputPort, "input", field.width});
            }
            if (!field.outputPort.isEmpty()) {
                ports.append({field.outputPort, "output", field.width});
            }
        }
    }
    return ports;
}

YAML::Node QSocMmioGenerator::describeModuleYaml(const QSocMmioPlan &plan)
{
    YAML::Node                           module(YAML::NodeType::Map);
    const QList<QSocMmioPortDescription> ports = describePorts(plan);
    for (const QSocMmioPortDescription &port : ports) {
        YAML::Node portNode(YAML::NodeType::Map);
        portNode["type"]      = port.width == 1
                                    ? std::string("logic")
                                    : QString("logic[%1:0]").arg(port.width - 1).toStdString();
        portNode["direction"] = port.direction.toStdString();
        module["port"][port.name.toStdString()] = portNode;
    }
    YAML::Node control(YAML::NodeType::Map);
    control["bus"]  = "axi4_lite";
    control["mode"] = "slave";
    YAML::Node mapping(YAML::NodeType::Map);
    for (const QSocMmioPortDescription &port : ports) {
        if (!port.name.startsWith("s_axi_")) {
            continue;
        }
        mapping[port.name.mid(6).toStdString()] = port.name.toStdString();
    }
    control["mapping"]       = mapping;
    module["bus"]["control"] = control;
    return module;
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

bool QSocMmioGenerator::generateFormalCollateral(
    const QSocModuleDefinition &definition,
    QSocMmioFormalCollateral   *collateral,
    QStringList                *errors)
{
    if (collateral) {
        *collateral = QSocMmioFormalCollateral();
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
    if (collateral) {
        *collateral = QSocMmioFormal::generate(plan);
    }
    return true;
}

bool QSocMmioGenerator::generateUvmCollateral(
    const QSocModuleDefinition &definition, QSocMmioUvmCollateral *collateral, QStringList *errors)
{
    if (collateral) {
        *collateral = QSocMmioUvmCollateral();
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
    if (collateral) {
        *collateral = QSocMmioUvm::generate(plan);
    }
    return true;
}
