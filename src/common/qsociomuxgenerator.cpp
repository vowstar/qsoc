// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsociomuxgenerator.h"

#include "common/qsocmodulemanager.h"
#include "common/qsocverilogutils.h"

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <QRegularExpression>
#include <QSet>

namespace {

const QSet<QString> kGeneratorKeys
    = {"kind",
       "bus",
       "data_width",
       "address_width",
       "pin_count",
       "hs_slots",
       "option",
       "pad_cell",
       "integration",
       "route"};
const QSet<QString> kOptionKeys      = {"gpio", "interrupt"};
const QSet<QString> kIntegrationKeys = {"instance", "clock", "reset", "control", "pad"};
const QSet<QString> kPadKeys
    = {"io", "input_value", "input_enable", "output_value", "output_enable"};
const QSet<QString> kRouteKeys
    = {"pin",
       "slot",
       "function",
       "signal",
       "input_value",
       "input_enable",
       "output_value",
       "output_enable",
       "pull",
       "drive"};
const QSet<QString> kEndpointKeys  = {"link", "bit", "invert"};
const QSet<QString> kRoutePullKeys = {"mode", "strength"};

constexpr quint32 kMinimumPinCount = 1;
constexpr quint32 kMaximumPinCount = 256;
constexpr quint32 kMinimumHsSlots  = 2;
constexpr quint32 kMaximumHsSlots  = 8;
constexpr quint32 kDefaultHsSlots  = 4;
constexpr quint32 kSelectorLane    = 4;

const std::array<QSocIomuxRole, 4> kRoles
    = {QSocIomuxRole::InputValue,
       QSocIomuxRole::InputEnable,
       QSocIomuxRole::OutputValue,
       QSocIomuxRole::OutputEnable};

QString roleKey(QSocIomuxRole role)
{
    switch (role) {
    case QSocIomuxRole::InputValue:
        return QStringLiteral("input_value");
    case QSocIomuxRole::InputEnable:
        return QStringLiteral("input_enable");
    case QSocIomuxRole::OutputValue:
        return QStringLiteral("output_value");
    case QSocIomuxRole::OutputEnable:
        return QStringLiteral("output_enable");
    }
    return QString();
}

QSocIomuxEndpointPlan &routeRole(QSocIomuxRoutePlan &route, QSocIomuxRole role)
{
    switch (role) {
    case QSocIomuxRole::InputValue:
        return route.inputValue;
    case QSocIomuxRole::InputEnable:
        return route.inputEnable;
    case QSocIomuxRole::OutputValue:
        return route.outputValue;
    case QSocIomuxRole::OutputEnable:
        break;
    }
    return route.outputEnable;
}

const QSocIomuxEndpointPlan &routeRole(const QSocIomuxRoutePlan &route, QSocIomuxRole role)
{
    return routeRole(const_cast<QSocIomuxRoutePlan &>(route), role);
}

void appendError(
    QStringList *errors, const QString &code, const QString &path, const QString &message)
{
    errors->append(QString("IOMUX_%1 %2: %3").arg(code, path, message));
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

bool parseLabel(const YAML::Node &node, const QString &path, QString *value, QStringList *errors)
{
    if (!parseScalar(node, path, value, errors)) {
        return false;
    }
    *value = value->trimmed();
    if (value->isEmpty()) {
        appendError(errors, "EMPTY", path, "must be a non-empty string");
        return false;
    }
    return true;
}

bool parseStrictUnsigned(
    const YAML::Node &node,
    const QString    &path,
    quint64           minimum,
    quint64           maximum,
    quint64          *value,
    QStringList      *errors)
{
    if (!node || !node.IsScalar() || node.Tag() == "!") {
        appendError(errors, "TYPE", path, "must be an unsigned decimal integer");
        return false;
    }
    const QString                   text = QString::fromStdString(node.Scalar());
    static const QRegularExpression integerPattern(QStringLiteral("^(0|[1-9][0-9]*)$"));
    if (!integerPattern.match(text).hasMatch()) {
        appendError(errors, "TYPE", path, "must be an unsigned decimal integer");
        return false;
    }
    bool          ok     = false;
    const quint64 parsed = text.toULongLong(&ok, 10);
    if (!ok || parsed < minimum || parsed > maximum) {
        appendError(
            errors, "RANGE", path, QString("must be between %1 and %2").arg(minimum).arg(maximum));
        return false;
    }
    *value = parsed;
    return true;
}

bool parseStrictBool(const YAML::Node &node, const QString &path, bool *value, QStringList *errors)
{
    if (!node || !node.IsScalar() || node.Tag() == "!") {
        appendError(errors, "TYPE", path, "must be true or false");
        return false;
    }
    const QString text = QString::fromStdString(node.Scalar());
    if (text == "true") {
        *value = true;
        return true;
    }
    if (text == "false") {
        *value = false;
        return true;
    }
    appendError(errors, "TYPE", path, "must be true or false");
    return false;
}

quint64 maximumForWidth(quint32 width)
{
    return width == 64 ? std::numeric_limits<quint64>::max() : (quint64(1) << width) - 1;
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

bool parseBusWidths(const YAML::Node &generator, QSocMmioPlan *mmio, QStringList *errors)
{
    bool valid = true;
    if (generator["data_width"]) {
        quint64    dataWidth = 0;
        const bool parsed
            = parseUnsigned(generator["data_width"], "generator.data_width", 64, &dataWidth, errors);
        if (parsed && dataWidth != 32 && dataWidth != 64) {
            appendError(errors, "RANGE", "generator.data_width", "must be 32 or 64");
            valid = false;
        } else if (parsed) {
            mmio->dataWidth = static_cast<quint32>(dataWidth);
        } else {
            valid = false;
        }
    }

    if (generator["address_width"]) {
        quint64    addressWidth = 0;
        const bool parsed       = parseUnsigned(
            generator["address_width"], "generator.address_width", 64, &addressWidth, errors);
        const quint32 minimum = mmio->dataWidth == 64 ? 3 : 2;
        if (parsed && addressWidth < minimum) {
            appendError(
                errors,
                "RANGE",
                "generator.address_width",
                QString("must be between %1 and 64").arg(minimum));
            valid = false;
        } else if (parsed) {
            mmio->addressWidth = static_cast<quint32>(addressWidth);
        } else {
            valid = false;
        }
    }
    return valid;
}

bool parseIntegration(
    const YAML::Node         &node,
    bool                      hasPadCell,
    QSocIomuxIntegrationPlan *integration,
    QStringList              *errors)
{
    const QString path = "generator.integration";
    if (!validateMap(node, kIntegrationKeys, path, errors)) {
        return false;
    }

    bool                                               valid = true;
    const std::array<std::pair<QString, QString *>, 4> links
        = {std::pair<QString, QString *>{"instance", &integration->instance},
           std::pair<QString, QString *>{"clock", &integration->clock},
           std::pair<QString, QString *>{"reset", &integration->reset},
           std::pair<QString, QString *>{"control", &integration->control}};
    for (const auto &[key, value] : links) {
        if (!node[key.toStdString()]) {
            appendError(errors, "REQUIRED", path + "." + key, "property is required");
            valid = false;
            continue;
        }
        valid = parseIdentifier(node[key.toStdString()], path + "." + key, value, errors) && valid;
    }

    if (!node["pad"]) {
        appendError(errors, "REQUIRED", path + ".pad", "property is required");
        return false;
    }
    const YAML::Node pad     = node["pad"];
    const QString    padPath = path + ".pad";
    if (!validateMap(pad, kPadKeys, padPath, errors)) {
        return false;
    }
    const std::array<std::pair<QString, QString *>, 4> pads
        = {std::pair<QString, QString *>{"input_value", &integration->padInputValue},
           std::pair<QString, QString *>{"input_enable", &integration->padInputEnable},
           std::pair<QString, QString *>{"output_value", &integration->padOutputValue},
           std::pair<QString, QString *>{"output_enable", &integration->padOutputEnable}};
    /* With a pad cell the generator owns the four control vectors, so the only
     * thing left to name is the pad net itself. Without one the four vectors
     * are the boundary and there is no pad net. */
    if (hasPadCell) {
        if (!pad["io"]) {
            appendError(errors, "REQUIRED", padPath + ".io", "property is required with pad_cell");
            valid = false;
        } else {
            valid = parseIdentifier(pad["io"], padPath + ".io", &integration->padIo, errors)
                    && valid;
        }
        for (const auto &[key, value] : pads) {
            if (pad[key.toStdString()]) {
                appendError(
                    errors, "CONFLICT", padPath + "." + key, "is owned by pad_cell, remove it");
                valid = false;
            }
        }
        return valid;
    }
    if (pad["io"]) {
        appendError(errors, "CONFLICT", padPath + ".io", "needs pad_cell to be declared");
        valid = false;
    }
    for (const auto &[key, value] : pads) {
        if (!pad[key.toStdString()]) {
            appendError(errors, "REQUIRED", padPath + "." + key, "property is required");
            valid = false;
            continue;
        }
        valid = parseIdentifier(pad[key.toStdString()], padPath + "." + key, value, errors)
                && valid;
    }
    return valid;
}

const QSet<QString> kPadCellKeys    = {"cell", "port", "pull", "drive", "constraint"};
const QSet<QString> kConstraintKeys = {"name", "kind", "expr", "property"};
const QSet<QString> kPadPortKeys
    = {"pad", "input_value", "input_enable", "output_value", "output_enable"};
const QSet<QString> kPadPullKeys  = {"port", "table", "kind"};
const QSet<QString> kPadDriveKeys = {"port", "table"};

/**
 * @brief Read one transcribed row and check it against the port count.
 */
bool parsePadRow(
    const YAML::Node       &node,
    const QString          &path,
    const QString          &label,
    qsizetype               width,
    QList<QSocPadTableRow> *rows,
    QStringList            *errors)
{
    if (!node.IsSequence()) {
        appendError(errors, "TYPE", path, "must be a sequence of 0, 1 or x");
        return false;
    }
    QSocPadTableRow row;
    row.label = label;
    for (const YAML::Node &cell : node) {
        if (!cell.IsScalar()) {
            appendError(errors, "TYPE", path, "must be a sequence of 0, 1 or x");
            return false;
        }
        const QString text = QString::fromStdString(cell.Scalar());
        if (text != "0" && text != "1" && text != "x") {
            appendError(errors, "VALUE", path, "each entry must be 0, 1 or x");
            return false;
        }
        row.value.append(text);
    }
    if (row.value.size() != width) {
        appendError(
            errors,
            "RANGE",
            path,
            QString("holds %1 entries but the port list holds %2").arg(row.value.size()).arg(width));
        return false;
    }
    rows->append(row);
    return true;
}

/**
 * @brief Read one direction, which is either a single row or a map of labelled rows.
 */
bool parsePadDirection(
    const YAML::Node       &node,
    const QString          &path,
    qsizetype               width,
    QList<QSocPadTableRow> *rows,
    QStringList            *errors)
{
    if (node.IsSequence()) {
        return parsePadRow(node, path, QString(), width, rows, errors);
    }
    if (!node.IsMap()) {
        appendError(errors, "TYPE", path, "must be a sequence or a map of labelled sequences");
        return false;
    }
    bool valid = true;
    for (const auto &entry : node) {
        if (!entry.first.IsScalar()) {
            appendError(errors, "TYPE", path, "strength labels must be scalar");
            valid = false;
            continue;
        }
        const QString label = QString::fromStdString(entry.first.Scalar());
        valid = parsePadRow(entry.second, path + "." + label, label, width, rows, errors) && valid;
    }
    return valid;
}

bool parsePadCell(const YAML::Node &node, QSocPadCellPlan *plan, QStringList *errors)
{
    const QString path = QStringLiteral("generator.pad_cell");
    if (!validateMap(node, kPadCellKeys, path, errors)) {
        return false;
    }
    bool valid = true;
    if (!node["cell"]) {
        appendError(errors, "REQUIRED", path + ".cell", "property is required");
        return false;
    }
    valid = parseIdentifier(node["cell"], path + ".cell", &plan->cell, errors) && valid;

    if (!node["port"]) {
        appendError(errors, "REQUIRED", path + ".port", "property is required");
        return false;
    }
    const YAML::Node port     = node["port"];
    const QString    portPath = path + ".port";
    if (!validateMap(port, kPadPortKeys, portPath, errors)) {
        return false;
    }
    if (!port["pad"]) {
        appendError(errors, "REQUIRED", portPath + ".pad", "property is required");
        valid = false;
    } else {
        valid = parseIdentifier(port["pad"], portPath + ".pad", &plan->portPad, errors) && valid;
    }
    const std::array<std::pair<QString, QString *>, 4> roles
        = {std::pair<QString, QString *>{"input_value", &plan->portInputValue},
           std::pair<QString, QString *>{"input_enable", &plan->portInputEnable},
           std::pair<QString, QString *>{"output_value", &plan->portOutputValue},
           std::pair<QString, QString *>{"output_enable", &plan->portOutputEnable}};
    for (const auto &[key, target] : roles) {
        /* An absent role means the cell cannot do it, which a route then cannot ask for. */
        if (!port[key.toStdString()]) {
            continue;
        }
        valid = parseIdentifier(port[key.toStdString()], portPath + "." + key, target, errors)
                && valid;
    }

    if (node["pull"]) {
        const YAML::Node pull     = node["pull"];
        const QString    pullPath = path + ".pull";
        if (!validateMap(pull, kPadPullKeys, pullPath, errors)) {
            return false;
        }
        if (!pull["port"] || !pull["port"].IsSequence()) {
            appendError(errors, "REQUIRED", pullPath + ".port", "must be a sequence of port names");
            return false;
        }
        for (const YAML::Node &entry : pull["port"]) {
            QString name;
            valid = parseIdentifier(entry, pullPath + ".port", &name, errors) && valid;
            plan->pull.port.append(name);
        }
        if (pull["kind"]) {
            const QString kind = QString::fromStdString(pull["kind"].Scalar());
            if (kind != "resistor" && kind != "driver") {
                appendError(errors, "VALUE", pullPath + ".kind", "must be resistor or driver");
                valid = false;
            }
            plan->pull.isDriver = kind == "driver";
        }
        if (!pull["table"]) {
            appendError(errors, "REQUIRED", pullPath + ".table", "property is required");
            return false;
        }
        const YAML::Node table     = pull["table"];
        const QString    tablePath = pullPath + ".table";
        if (!table.IsMap()) {
            appendError(errors, "TYPE", tablePath, "must be a map of mode names");
            return false;
        }
        const qsizetype width = plan->pull.port.size();
        for (const auto &entry : table) {
            if (!entry.first.IsScalar()) {
                appendError(errors, "TYPE", tablePath, "mode names must be scalar");
                valid = false;
                continue;
            }
            const QString          name = QString::fromStdString(entry.first.Scalar());
            QList<QSocPadTableRow> rows;
            if (parsePadDirection(entry.second, tablePath + "." + name, width, &rows, errors)) {
                plan->pull.mode.insert(name, rows);
            } else {
                valid = false;
            }
        }
        if (!plan->pull.has(QStringLiteral("none"))) {
            appendError(errors, "REQUIRED", tablePath + ".none", "the table needs a none row");
            valid = false;
        }
    }

    if (node["drive"]) {
        const YAML::Node drive     = node["drive"];
        const QString    drivePath = path + ".drive";
        if (!validateMap(drive, kPadDriveKeys, drivePath, errors)) {
            return false;
        }
        if (!drive["port"] || !drive["port"].IsSequence()) {
            appendError(errors, "REQUIRED", drivePath + ".port", "must be a sequence of port names");
            return false;
        }
        for (const YAML::Node &entry : drive["port"]) {
            QString name;
            valid = parseIdentifier(entry, drivePath + ".port", &name, errors) && valid;
            plan->drive.port.append(name);
        }
        if (!drive["table"] || !drive["table"].IsMap()) {
            appendError(errors, "REQUIRED", drivePath + ".table", "must be a map of labelled rows");
            return false;
        }
        for (const auto &entry : drive["table"]) {
            const QString label = QString::fromStdString(entry.first.Scalar());
            valid               = parsePadRow(
                                      entry.second,
                                      drivePath + ".table." + label,
                                      label,
                                      plan->drive.port.size(),
                                      &plan->drive.level,
                                      errors)
                                  && valid;
        }
    }

    if (node["constraint"]) {
        if (!node["constraint"].IsSequence()) {
            appendError(errors, "TYPE", path + ".constraint", "must be a sequence");
            return false;
        }
        int index = 0;
        for (const YAML::Node &entry : node["constraint"]) {
            const QString itemPath = QString("%1.constraint[%2]").arg(path).arg(index++);
            if (!validateMap(entry, kConstraintKeys, itemPath, errors)) {
                valid = false;
                continue;
            }
            QSocPadConstraint item;
            if (!entry["name"]) {
                appendError(errors, "REQUIRED", itemPath + ".name", "property is required");
                valid = false;
            } else {
                valid = parseIdentifier(entry["name"], itemPath + ".name", &item.name, errors)
                        && valid;
            }
            const bool hasExpr = entry["expr"] && entry["expr"].IsScalar();
            const bool hasProp = entry["property"] && entry["property"].IsScalar();
            if (hasExpr == hasProp) {
                appendError(errors, "REQUIRED", itemPath, "needs exactly one of expr or property");
                valid = false;
                continue;
            }
            item.temporal = hasProp;
            item.body     = QString::fromStdString(
                (hasProp ? entry["property"] : entry["expr"]).Scalar());
            if (entry["kind"]) {
                const QString kind = QString::fromStdString(entry["kind"].Scalar());
                if (kind != "assert" && kind != "assume") {
                    appendError(errors, "VALUE", itemPath + ".kind", "must be assert or assume");
                    valid = false;
                }
                item.kindGiven = true;
                item.assume    = kind == "assume";
            }
            plan->constraint.append(item);
        }
    }
    return valid;
}

bool parseEndpoint(
    const YAML::Node      &node,
    const QString         &path,
    bool                   allowConstant,
    QSocIomuxEndpointPlan *endpoint,
    QStringList           *errors)
{
    if (node.IsMap()) {
        if (!validateMap(node, kEndpointKeys, path, errors)) {
            return false;
        }
        bool valid = true;
        if (!node["link"]) {
            appendError(errors, "REQUIRED", path + ".link", "property is required");
            valid = false;
        } else {
            valid = parseIdentifier(node["link"], path + ".link", &endpoint->link, errors) && valid;
        }
        if (node["bit"]) {
            quint64 bit = 0;
            if (parseStrictUnsigned(
                    node["bit"],
                    path + ".bit",
                    0,
                    std::numeric_limits<quint32>::max(),
                    &bit,
                    errors)) {
                endpoint->bit = static_cast<quint32>(bit);
            } else {
                valid = false;
            }
        }
        if (node["invert"]) {
            valid = parseStrictBool(node["invert"], path + ".invert", &endpoint->invert, errors)
                    && valid;
        }
        return valid;
    }

    if (node.IsScalar() && allowConstant && node.Tag() != "!") {
        const QString text = QString::fromStdString(node.Scalar());
        if (text == "0" || text == "1") {
            endpoint->constant = static_cast<quint8>(text == "1" ? 1 : 0);
            return true;
        }
    }
    if (allowConstant) {
        appendError(errors, "TYPE", path, "must be an endpoint map or integer 0 or 1");
    } else {
        appendError(errors, "TYPE", path, "must be an endpoint map");
    }
    return false;
}

bool parseRoute(
    const YAML::Node   &node,
    const QString      &path,
    bool                pinCountValid,
    quint32             pinCount,
    bool                hsSlotsValid,
    quint32             hsSlots,
    QSocIomuxRoutePlan *route,
    QStringList        *errors)
{
    if (!validateMap(node, kRouteKeys, path, errors)) {
        return false;
    }

    bool valid = true;
    if (!node["pin"]) {
        appendError(errors, "REQUIRED", path + ".pin", "property is required");
        valid = false;
    } else {
        quint64 pin = 0;
        if (parseStrictUnsigned(
                node["pin"], path + ".pin", 0, std::numeric_limits<quint32>::max(), &pin, errors)) {
            route->pin = static_cast<quint32>(pin);
            if (pinCountValid && route->pin >= pinCount) {
                appendError(
                    errors,
                    "RANGE",
                    path + ".pin",
                    QString("must be less than pin_count %1").arg(pinCount));
                valid = false;
            }
        } else {
            valid = false;
        }
    }

    if (!node["slot"]) {
        appendError(errors, "REQUIRED", path + ".slot", "property is required");
        valid = false;
    } else {
        quint64 slot = 0;
        if (parseStrictUnsigned(
                node["slot"], path + ".slot", 0, std::numeric_limits<quint32>::max(), &slot, errors)) {
            route->slot = static_cast<quint32>(slot);
            if (hsSlotsValid && route->slot >= hsSlots) {
                appendError(
                    errors,
                    "RANGE",
                    path + ".slot",
                    QString("must be less than hs_slots %1").arg(hsSlots));
                valid = false;
            }
        } else {
            valid = false;
        }
    }

    if (!node["function"]) {
        appendError(errors, "REQUIRED", path + ".function", "property is required");
        valid = false;
    } else {
        valid = parseLabel(node["function"], path + ".function", &route->function, errors) && valid;
    }
    if (!node["signal"]) {
        appendError(errors, "REQUIRED", path + ".signal", "property is required");
        valid = false;
    } else {
        valid = parseLabel(node["signal"], path + ".signal", &route->signal, errors) && valid;
    }

    int declaredRoles = 0;
    for (const QSocIomuxRole role : kRoles) {
        const QString    key      = roleKey(role);
        const YAML::Node roleNode = node[key.toStdString()];
        if (!roleNode) {
            continue;
        }
        ++declaredRoles;
        const bool allowConstant = role != QSocIomuxRole::InputValue;
        valid                    = parseEndpoint(
                                       roleNode, path + "." + key, allowConstant, &routeRole(*route, role), errors)
                                   && valid;
    }
    if (node["pull"]) {
        const YAML::Node pull = node["pull"];
        if (pull.IsScalar()) {
            valid = parseLabel(pull, path + ".pull", &route->pullMode, errors) && valid;
        } else if (pull.IsMap() && validateMap(pull, kRoutePullKeys, path + ".pull", errors)) {
            if (!pull["mode"]) {
                appendError(errors, "REQUIRED", path + ".pull.mode", "property is required");
                valid = false;
            } else {
                valid = parseLabel(pull["mode"], path + ".pull.mode", &route->pullMode, errors)
                        && valid;
            }
            if (pull["strength"]) {
                valid = parseLabel(
                            pull["strength"], path + ".pull.strength", &route->pullStrength, errors)
                        && valid;
            }
        } else {
            appendError(errors, "TYPE", path + ".pull", "must be a mode name or a map");
            valid = false;
        }
        ++declaredRoles;
    }
    if (node["drive"]) {
        valid = parseLabel(node["drive"], path + ".drive", &route->driveLevel, errors) && valid;
        ++declaredRoles;
    }
    if (declaredRoles == 0) {
        appendError(errors, "ROLE", path, "at least one role is required");
        valid = false;
    }
    return valid;
}

QString sinkKey(const QSocIomuxEndpointPlan &endpoint)
{
    if (endpoint.bit.has_value()) {
        return QString("%1[%2]").arg(endpoint.link).arg(*endpoint.bit);
    }
    return endpoint.link;
}

bool parseRoutes(
    const YAML::Node &node,
    bool              pinCountValid,
    bool              hsSlotsValid,
    QSocIomuxPlan    *plan,
    QStringList      *errors)
{
    const QString path = "generator.route";
    if (!node || !node.IsSequence()) {
        appendError(errors, "TYPE", path, "must be a sequence");
        return false;
    }

    bool          valid = true;
    QSet<quint64> pinSlots;
    QSet<QString> sinks;
    for (qsizetype index = 0; index < static_cast<qsizetype>(node.size()); ++index) {
        const QString      routePath = QString("%1[%2]").arg(path).arg(index);
        QSocIomuxRoutePlan route;
        if (!parseRoute(
                node[index],
                routePath,
                pinCountValid,
                plan->pinCount,
                hsSlotsValid,
                plan->hsSlots,
                &route,
                errors)) {
            valid = false;
            continue;
        }
        const quint64 pinSlot = (quint64(route.pin) << 32) | route.slot;
        if (pinSlots.contains(pinSlot)) {
            appendError(
                errors,
                "DUPLICATE",
                routePath,
                QString("pin %1 slot %2 is already routed").arg(route.pin).arg(route.slot));
            valid = false;
            continue;
        }
        pinSlots.insert(pinSlot);
        if (!route.inputValue.link.isEmpty()) {
            const QString key = sinkKey(route.inputValue);
            if (sinks.contains(key)) {
                appendError(
                    errors,
                    "DUPLICATE",
                    routePath + ".input_value",
                    QString("sink %1 is already driven").arg(key));
                valid = false;
                continue;
            }
            sinks.insert(key);
        }
        plan->routes.append(route);
    }
    return valid;
}

quint32 selectorWidth(quint32 hsSlots)
{
    if (hsSlots <= 2) {
        return 1;
    }
    if (hsSlots <= 4) {
        return 2;
    }
    return 3;
}

quint32 pinsPerWord(quint32 dataWidth)
{
    return dataWidth / kSelectorLane;
}

quint32 selectorWordCount(quint32 pinCount, quint32 dataWidth)
{
    const quint32 lanes = pinsPerWord(dataWidth);
    return (pinCount + lanes - 1) / lanes;
}

/**
 * @brief Append the register families that back software control of a pin.
 *
 * Two shapes, matching the two access patterns. A banked vector holds one bit
 * per pin so one store flips the same bit on a whole word of pins. A per-pin
 * word holds that pin's whole source configuration so one store reconfigures a
 * pin without a read-modify-write and without touching its neighbours.
 */
void composeGpio(QSocIomuxPlan *plan)
{
    const quint32 dataWidth = plan->mmio.dataWidth;
    const quint32 byteCount = dataWidth / 8;
    const quint32 words     = (plan->pinCount + dataWidth - 1) / dataWidth;
    quint64       offset    = plan->mmio.registers.constLast().byteOffset + byteCount;

    struct Family
    {
        const char    *name;
        QSocMmioAccess access;
    };
    const Family families[]
        = {{"input_value", QSocMmioAccess::ReadOnly},
           {"input_enable", QSocMmioAccess::ReadWrite},
           {"output_value", QSocMmioAccess::ReadWrite},
           {"output_enable", QSocMmioAccess::ReadWrite}};

    for (const Family &family : families) {
        for (quint32 word = 0; word < words; ++word) {
            QSocMmioRegisterPlan bank;
            bank.name       = QString("%1_%2").arg(family.name).arg(word);
            bank.byteOffset = offset;
            offset += byteCount;
            const quint32 first = word * dataWidth;
            const quint32 last  = std::min(plan->pinCount, first + dataWidth);
            for (quint32 pin = first; pin < last; ++pin) {
                QSocMmioFieldPlan field;
                field.name   = QString("pin_%1_%2").arg(pin).arg(family.name);
                field.lsb    = pin - first;
                field.width  = 1;
                field.access = family.access;
                if (family.access == QSocMmioAccess::ReadOnly) {
                    field.inputPort = QString("pin_%1_%2_i").arg(pin).arg(family.name);
                } else {
                    field.resetValue = 0;
                    field.outputPort = QString("pin_%1_%2_o").arg(pin).arg(family.name);
                }
                bank.fields.append(field);
            }
            plan->mmio.registers.append(bank);
        }
    }

    for (quint32 pin = 0; pin < plan->pinCount; ++pin) {
        QSocMmioRegisterPlan control;
        control.name       = QString("pin_src_ctrl_%1").arg(pin);
        control.byteOffset = offset;
        offset += byteCount;

        struct Source
        {
            const char *name;
            quint32     lsb;
            quint32     width;
        };
        const Source sources[]
            = {{"input_enable_src", 0, 1}, {"output_value_src", 2, 2}, {"output_enable_src", 4, 2}};
        for (const Source &source : sources) {
            QSocMmioFieldPlan field;
            field.name       = QString(source.name);
            field.lsb        = source.lsb;
            field.width      = source.width;
            field.access     = QSocMmioAccess::ReadWrite;
            field.resetValue = 0;
            field.outputPort = QString("pin_%1_%2_o").arg(pin).arg(source.name);
            control.fields.append(field);
        }
        plan->mmio.registers.append(control);
    }
}

/**
 * @brief Append the interrupt enable and pending banks.
 *
 * Pending records the event whether or not the enable is set, so a design can
 * poll a pin it never wired to an interrupt line. The enable gates the output
 * only. The reference this follows gates the set as well, which makes polling
 * impossible and loses an event that arrives while the enable is off.
 */
void composeInterrupt(QSocIomuxPlan *plan)
{
    const quint32 dataWidth = plan->mmio.dataWidth;
    const quint32 byteCount = dataWidth / 8;
    const quint32 words     = (plan->pinCount + dataWidth - 1) / dataWidth;
    quint64       offset    = plan->mmio.registers.constLast().byteOffset + byteCount;

    const char *kinds[] = {"high", "low", "rise", "fall"};

    for (bool pending : {false, true}) {
        for (const char *kind : kinds) {
            for (quint32 word = 0; word < words; ++word) {
                QSocMmioRegisterPlan bank;
                bank.name = QString("%1_int_%2_%3").arg(kind, pending ? "pend" : "en").arg(word);
                bank.byteOffset = offset;
                offset += byteCount;
                const quint32 first = word * dataWidth;
                const quint32 last  = std::min(plan->pinCount, first + dataWidth);
                for (quint32 pin = first; pin < last; ++pin) {
                    QSocMmioFieldPlan field;
                    field.name
                        = QString("pin_%1_%2_int_%3").arg(pin).arg(kind, pending ? "pend" : "en");
                    field.lsb        = pin - first;
                    field.width      = 1;
                    field.resetValue = 0;
                    field.access     = pending ? QSocMmioAccess::WriteOneClear
                                               : QSocMmioAccess::ReadWrite;
                    if (pending) {
                        field.inputPort = QString("pin_%1_%2_detect_i").arg(pin).arg(kind);
                    }
                    field.outputPort
                        = QString("pin_%1_%2_int_%3_o").arg(pin).arg(kind, pending ? "pend" : "en");
                    bank.fields.append(field);
                }
                plan->mmio.registers.append(bank);
            }
        }
    }
}

namespace {

/**
 * @brief The pull rows in the order the selector encodes them.
 */
/**
 * @brief The pull rows in the order the selector encodes them.
 *
 * `none` leads so that an all-zero selector is the state that drives nothing,
 * and the remaining modes follow in name order so the encoding is stable.
 */
QList<QSocPadTableRow> padPullRows(const QSocPadCellPlan &cell)
{
    QList<QSocPadTableRow> rows = cell.pull.mode.value(QStringLiteral("none"));
    for (auto it = cell.pull.mode.cbegin(); it != cell.pull.mode.cend(); ++it) {
        if (it.key() != QStringLiteral("none")) {
            rows.append(it.value());
        }
    }
    return rows;
}

quint32 encodingWidth(qsizetype count)
{
    quint32 width = 1;
    while ((qsizetype(1) << width) < count) {
        ++width;
    }
    return width;
}

/**
 * @brief Drive one group of cell pins from a transcribed table.
 *
 * Continuous assignment rather than a clocked or sensitivity based block: a
 * selector that never leaves its reset value produces no event, and an
 * always block would then hold x on a pad control pin for the whole run.
 */
void appendPadTableCase(
    QStringList                  *lines,
    quint32                       pin,
    const QString                &selector,
    const QList<QString>         &port,
    const QList<QSocPadTableRow> &rows)
{
    const quint32 width = encodingWidth(rows.size());
    for (qsizetype index = 0; index < port.size(); ++index) {
        QString expression;
        for (qsizetype row = 0; row < rows.size(); ++row) {
            /* A table entry of x means the pin does not matter in that row, so
             * drive zero and keep the netlist two valued. */
            const QString value = rows.at(row).value.at(index) == "1" ? "1" : "0";
            expression
                += QString("(%1 == %2'd%3) ? 1'b%4 : ").arg(selector).arg(width).arg(row).arg(value);
        }
        expression += "1'b0";
        lines->append(QString("wire %1_%2_w = %3;").arg(port.at(index)).arg(pin).arg(expression));
    }
}

/**
 * @brief Index of a requested pull row in the encoding order, or -1.
 */
int padPullCode(const QSocPadCellPlan &cell, const QString &mode, const QString &strength)
{
    /* Same order padPullRows built: none first, then the remaining names. */
    QStringList names = {QStringLiteral("none")};
    for (auto it = cell.pull.mode.cbegin(); it != cell.pull.mode.cend(); ++it) {
        if (it.key() != QStringLiteral("none")) {
            names.append(it.key());
        }
    }
    int index = 0;
    for (const QString &name : names) {
        for (const QSocPadTableRow &row : cell.pull.mode.value(name)) {
            if (name == mode && row.label == strength) {
                return index;
            }
            ++index;
        }
    }
    return -1;
}

int padDriveCode(const QSocPadCellPlan &cell, const QString &level)
{
    for (qsizetype index = 0; index < cell.drive.level.size(); ++index) {
        if (cell.drive.level.at(index).label == level) {
            return int(index);
        }
    }
    return -1;
}

} // namespace

/**
 * @brief Words a constraint body may use besides cell ports.
 */
const QSet<QString> kConstraintWords = {"and",         "or",           "not",        "implies",
                                        "iff",         "throughout",   "until",      "within",
                                        "first_match", "intersect",    "always",     "eventually",
                                        "nexttime",    "s_eventually", "s_nexttime", "s_until",
                                        "s_always",    "strong",       "weak",       "disable",
                                        "if",          "else",         "case",       "endcase",
                                        "default"};

/**
 * @brief The per-pin net a cell port maps to inside the pad module.
 */
QString padPortNet(const QSocPadCellPlan &cell, const QString &port, quint32 pin)
{
    if (port == cell.portInputValue) {
        return QString("pad_input_value_o[%1]").arg(pin);
    }
    if (port == cell.portInputEnable) {
        return QString("pad_input_enable_i[%1]").arg(pin);
    }
    if (port == cell.portOutputValue) {
        return QString("pad_output_value_i[%1]").arg(pin);
    }
    if (port == cell.portOutputEnable) {
        return QString("pad_output_enable_i[%1]").arg(pin);
    }
    if (port == cell.portPad) {
        return QString("pad_io[%1]").arg(pin);
    }
    if (cell.pull.port.contains(port) || cell.drive.port.contains(port)) {
        return QString("%1_%2_w").arg(port).arg(pin);
    }
    return QString();
}

/**
 * @brief Rewrite one constraint for one pin, or report the first unknown word.
 *
 * Only identifiers are touched. A word after `$` is a system function and a
 * word after `'` is a literal base, both pass through untouched.
 */
bool rewriteConstraint(
    const QSocPadCellPlan &cell,
    const QString         &body,
    quint32                pin,
    QString               *out,
    QSet<QString>         *ports,
    QString               *unknown)
{
    static const QRegularExpression word("[A-Za-z_][A-Za-z0-9_]*");
    QString                         result;
    qsizetype                       cursor = 0;
    auto                            it     = word.globalMatch(body);
    while (it.hasNext()) {
        const QRegularExpressionMatch m     = it.next();
        const qsizetype               start = m.capturedStart();
        const QChar                   prev  = start > 0 ? body.at(start - 1) : QChar();
        result += body.mid(cursor, start - cursor);
        cursor              = m.capturedEnd();
        const QString token = m.captured();
        if (prev == '$' || prev == '\'' || (prev.isDigit())) {
            result += token;
            continue;
        }
        const QString net = padPortNet(cell, token, pin);
        if (!net.isEmpty()) {
            result += net;
            if (ports) {
                ports->insert(token);
            }
            continue;
        }
        if (kConstraintWords.contains(token)) {
            result += token;
            continue;
        }
        if (unknown) {
            *unknown = token;
        }
        return false;
    }
    result += body.mid(cursor);
    if (out) {
        *out = result;
    }
    return true;
}

/**
 * @brief Check every constraint and settle its kind.
 *
 * A body over pull and drive pins alone speaks about logic this generator
 * emits, so it is a claim to prove. A body that reaches a role pin speaks
 * about what routes and registers will do, so it bounds the environment.
 */
bool validatePadConstraints(QSocIomuxPlan *plan, QStringList *errors)
{
    QSocPadCellPlan &cell  = plan->integration.padCell;
    bool             valid = true;
    for (qsizetype index = 0; index < cell.constraint.size(); ++index) {
        QSocPadConstraint &item = cell.constraint[index];
        const QString      path = QString("generator.pad_cell.constraint[%1]").arg(index);
        QSet<QString>      ports;
        QString            unknown;
        if (!rewriteConstraint(cell, item.body, 0, nullptr, &ports, &unknown)) {
            appendError(
                errors,
                "CONSTRAINT",
                path,
                QString("%1 is not a port of %2 nor a SystemVerilog word").arg(unknown, cell.cell));
            valid = false;
            continue;
        }
        if (ports.isEmpty()) {
            appendError(errors, "CONSTRAINT", path, "names no port of the pad cell");
            valid = false;
            continue;
        }
        if (!item.kindGiven) {
            bool onlyTable = true;
            for (const QString &port : ports) {
                onlyTable = onlyTable
                            && (cell.pull.port.contains(port) || cell.drive.port.contains(port));
            }
            item.assume = !onlyTable;
        }
    }
    return valid;
}

/**
 * @brief Refuse a route that asks the declared pad cell for something it lacks.
 *
 * A cell that carries no output driver has no output_enable port, so a route
 * that drives the pin is a source error rather than a netlist that elaborates
 * into a pad tied off at random.
 */
bool validatePadCapability(const QSocIomuxPlan &plan, QStringList *errors)
{
    const QSocPadCellPlan &cell = plan.integration.padCell;
    if (!cell.declared()) {
        bool valid = true;
        for (const QSocIomuxRoutePlan &route : plan.routes) {
            if (route.pullMode.isEmpty() && route.driveLevel.isEmpty()) {
                continue;
            }
            appendError(
                errors,
                "CAPABILITY",
                QString("generator.route.pin %1 slot %2").arg(route.pin).arg(route.slot),
                "pull and drive need a pad_cell declaration");
            valid = false;
        }
        return valid;
    }
    bool valid = true;
    for (const QSocIomuxRoutePlan &route : plan.routes) {
        for (const QSocIomuxRole role : kRoles) {
            const QSocIomuxEndpointPlan &endpoint = routeRole(route, role);
            if (endpoint.link.isEmpty() && !endpoint.constant.has_value()) {
                continue;
            }
            QString port;
            switch (role) {
            case QSocIomuxRole::InputValue:
                port = cell.portInputValue;
                break;
            case QSocIomuxRole::InputEnable:
                port = cell.portInputEnable;
                break;
            case QSocIomuxRole::OutputValue:
                port = cell.portOutputValue;
                break;
            case QSocIomuxRole::OutputEnable:
                port = cell.portOutputEnable;
                break;
            }
            if (!port.isEmpty()) {
                continue;
            }
            appendError(
                errors,
                "CAPABILITY",
                QString("generator.route.pin %1 slot %2.%3")
                    .arg(route.pin)
                    .arg(route.slot)
                    .arg(roleKey(role)),
                QString("pad cell %1 declares no port for this role").arg(cell.cell));
            valid = false;
        }
        const QString routePath
            = QString("generator.route.pin %1 slot %2").arg(route.pin).arg(route.slot);
        if (!route.pullMode.isEmpty()) {
            const bool wantsFeedback = route.pullMode == QStringLiteral("keeper")
                                       || route.pullMode == QStringLiteral("oscillator");
            const bool derivedKeeper = wantsFeedback && !cell.pull.has(route.pullMode)
                                       && cell.canKeep();
            if (wantsFeedback && !cell.pull.has(route.pullMode) && !cell.canKeep()) {
                appendError(
                    errors,
                    "CAPABILITY",
                    routePath + ".pull.mode",
                    cell.pull.isDriver
                        ? QString("pad cell %1 pulls with its driver, so %2 cannot be woven")
                              .arg(cell.cell, route.pullMode)
                        : QString("pad cell %1 needs both up and down rows to weave %2")
                              .arg(cell.cell, route.pullMode));
                valid = false;
                continue;
            }
            if (derivedKeeper && !route.pullStrength.isEmpty()) {
                appendError(
                    errors,
                    "CAPABILITY",
                    routePath + ".pull.strength",
                    QString("woven %1 uses the first up and down rows, drop strength")
                        .arg(route.pullMode));
                valid = false;
                continue;
            }
            if (!cell.pull.has(route.pullMode) && !derivedKeeper) {
                appendError(
                    errors,
                    "CAPABILITY",
                    routePath + ".pull.mode",
                    QString("pad cell %1 has no pull mode %2").arg(cell.cell, route.pullMode));
                valid = false;
            } else if (!derivedKeeper) {
                const QList<QSocPadTableRow> rows = cell.pull.mode.value(route.pullMode);
                const bool labelled = rows.size() > 1 || !rows.first().label.isEmpty();
                bool       found    = false;
                for (const QSocPadTableRow &row : rows) {
                    found = found || row.label == route.pullStrength;
                }
                if (labelled && !found) {
                    appendError(
                        errors,
                        "CAPABILITY",
                        routePath + ".pull.strength",
                        QString("pull mode %1 of %2 has no strength %3")
                            .arg(route.pullMode, cell.cell, route.pullStrength));
                    valid = false;
                } else if (!labelled && !route.pullStrength.isEmpty()) {
                    appendError(
                        errors,
                        "CAPABILITY",
                        routePath + ".pull.strength",
                        QString("pull mode %1 of %2 has a single row, drop strength")
                            .arg(route.pullMode, cell.cell));
                    valid = false;
                }
            }
        }
        if (!route.driveLevel.isEmpty()) {
            bool found = false;
            for (const QSocPadTableRow &row : cell.drive.level) {
                found = found || row.label == route.driveLevel;
            }
            if (!found) {
                appendError(
                    errors,
                    "CAPABILITY",
                    routePath + ".drive",
                    QString("pad cell %1 has no drive level %2").arg(cell.cell, route.driveLevel));
                valid = false;
            }
        }
    }

    /* The cell gates its receiver with the input enable, so a pin whose sinks
     * listen while no slot ever raises that enable reads zero forever. The gpio
     * register path can raise it at run time, which is the one way out. */
    if (!cell.portInputEnable.isEmpty() && !plan.option.gpio) {
        QSet<quint32> listening;
        QSet<quint32> enabling;
        for (const QSocIomuxRoutePlan &route : plan.routes) {
            if (!route.inputValue.link.isEmpty()) {
                listening.insert(route.pin);
            }
            const QSocIomuxEndpointPlan &ie = route.inputEnable;
            if (!ie.link.isEmpty() || (ie.constant.has_value() && *ie.constant == 1)) {
                enabling.insert(route.pin);
            }
        }
        QList<quint32> dead = (listening - enabling).values();
        std::sort(dead.begin(), dead.end());
        for (quint32 pin : dead) {
            appendError(
                errors,
                "CAPABILITY",
                QString("generator.route.pin %1").arg(pin),
                "has input_value sinks but no slot enables the input buffer");
            valid = false;
        }
    }
    return valid;
}

bool composeMmio(QSocIomuxPlan *plan, QStringList *errors)
{
    const quint32 dataWidth = plan->mmio.dataWidth;
    const quint32 byteCount = dataWidth / 8;
    const quint32 lanes     = pinsPerWord(dataWidth);
    const quint32 words     = selectorWordCount(plan->pinCount, dataWidth);
    const quint32 width     = selectorWidth(plan->hsSlots);

    QSocMmioRegisterPlan capability;
    capability.name       = QStringLiteral("capability");
    capability.byteOffset = 0;

    QSocMmioFieldPlan pinCountField;
    pinCountField.name          = QStringLiteral("pin_count");
    pinCountField.lsb           = 0;
    pinCountField.width         = 16;
    pinCountField.access        = QSocMmioAccess::ReadOnly;
    pinCountField.constantValue = plan->pinCount;
    capability.fields.append(pinCountField);

    QSocMmioFieldPlan hsSlotsField;
    hsSlotsField.name          = QStringLiteral("hs_slots");
    hsSlotsField.lsb           = 16;
    hsSlotsField.width         = 8;
    hsSlotsField.access        = QSocMmioAccess::ReadOnly;
    hsSlotsField.constantValue = plan->hsSlots;
    capability.fields.append(hsSlotsField);
    plan->mmio.registers.append(capability);

    for (quint32 word = 0; word < words; ++word) {
        QSocMmioRegisterPlan selector;
        selector.name       = QString("hs_select_%1").arg(word);
        selector.byteOffset = quint64(1 + word) * byteCount;
        const quint32 first = word * lanes;
        const quint32 last  = std::min(plan->pinCount, first + lanes);
        for (quint32 pin = first; pin < last; ++pin) {
            QSocMmioFieldPlan field;
            field.name       = QString("pin_%1_select").arg(pin);
            field.lsb        = (pin % lanes) * kSelectorLane;
            field.width      = width;
            field.access     = QSocMmioAccess::ReadWrite;
            field.resetValue = 0;
            field.outputPort = QString("pin_%1_select_o").arg(pin);
            selector.fields.append(field);
        }
        plan->mmio.registers.append(selector);
    }

    if (plan->option.gpio) {
        composeGpio(plan);
    }
    if (plan->option.interrupt) {
        composeInterrupt(plan);
    }

    const quint64 aperture  = plan->mmio.registers.constLast().byteOffset + byteCount;
    const quint64 available = plan->mmio.addressWidth >= 64
                                  ? std::numeric_limits<quint64>::max()
                                  : (quint64(1) << plan->mmio.addressWidth);
    if (aperture > available) {
        quint32 minimumWidth = 2;
        while ((quint64(1) << minimumWidth) < aperture) {
            ++minimumWidth;
        }
        appendError(
            errors,
            "RANGE",
            "generator.address_width",
            QString(
                "aperture needs %1 bytes but %2-bit addressing provides %3 bytes, minimum "
                "address_width is %4")
                .arg(aperture)
                .arg(plan->mmio.addressWidth)
                .arg(available)
                .arg(minimumWidth));
        return false;
    }
    return true;
}

bool parsePlan(const QSocModuleDefinition &definition, QSocIomuxPlan *plan, QStringList *errors)
{
    plan->moduleName      = definition.moduleName;
    plan->mmio.moduleName = definition.moduleName + "_regs";

    bool valid = true;
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
    } else if (!parseScalar(generator["kind"], "generator.kind", &kind, errors) || kind != "iomux") {
        if (!kind.isEmpty()) {
            appendError(errors, "KIND", "generator.kind", "must be iomux");
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

    valid = parseBusWidths(generator, &plan->mmio, errors) && valid;

    if (definition.hasParameterSection || !definition.parameters.isEmpty()) {
        appendError(errors, "MANUAL_SECTION", "module.parameter", "is not allowed for IOMUX modules");
        valid = false;
    }
    if (definition.hasPortSection || !definition.ports.isEmpty()) {
        appendError(errors, "MANUAL_SECTION", "module.port", "is not allowed for IOMUX modules");
        valid = false;
    }
    if (definition.hasBusSection || !definition.busInterfaces.isEmpty()) {
        appendError(errors, "MANUAL_SECTION", "module.bus", "is not allowed for IOMUX modules");
        valid = false;
    }

    bool pinCountValid = false;
    if (!generator["pin_count"]) {
        appendError(errors, "REQUIRED", "generator.pin_count", "property is required");
        valid = false;
    } else {
        quint64 pinCount = 0;
        if (parseStrictUnsigned(
                generator["pin_count"],
                "generator.pin_count",
                kMinimumPinCount,
                kMaximumPinCount,
                &pinCount,
                errors)) {
            plan->pinCount = static_cast<quint32>(pinCount);
            pinCountValid  = true;
        } else {
            valid = false;
        }
    }

    bool hsSlotsValid = true;
    if (generator["hs_slots"]) {
        quint64 hsSlots = 0;
        if (parseStrictUnsigned(
                generator["hs_slots"],
                "generator.hs_slots",
                kMinimumHsSlots,
                kMaximumHsSlots,
                &hsSlots,
                errors)) {
            plan->hsSlots = static_cast<quint32>(hsSlots);
        } else {
            hsSlotsValid = false;
            valid        = false;
        }
    } else {
        plan->hsSlots = kDefaultHsSlots;
    }

    if (generator["option"]) {
        const YAML::Node option = generator["option"];
        if (!validateMap(option, kOptionKeys, "generator.option", errors)) {
            valid = false;
        } else {
            if (option["gpio"]) {
                valid = parseStrictBool(
                            option["gpio"], "generator.option.gpio", &plan->option.gpio, errors)
                        && valid;
            }
            if (option["interrupt"]) {
                valid = parseStrictBool(
                            option["interrupt"],
                            "generator.option.interrupt",
                            &plan->option.interrupt,
                            errors)
                        && valid;
            }
        }
    }

    if (generator["pad_cell"]) {
        valid = parsePadCell(generator["pad_cell"], &plan->integration.padCell, errors) && valid;
    }

    if (!generator["integration"]) {
        appendError(errors, "REQUIRED", "generator.integration", "property is required");
        valid = false;
    } else {
        valid = parseIntegration(
                    generator["integration"],
                    plan->integration.padCell.declared(),
                    &plan->integration,
                    errors)
                && valid;
    }

    if (!generator["route"]) {
        appendError(errors, "REQUIRED", "generator.route", "property is required");
        return false;
    }
    return parseRoutes(generator["route"], pinCountValid, hsSlotsValid, plan, errors) && valid;
}

void sortPlan(QSocIomuxPlan *plan)
{
    std::sort(
        plan->routes.begin(),
        plan->routes.end(),
        [](const QSocIomuxRoutePlan &left, const QSocIomuxRoutePlan &right) {
            return left.pin != right.pin ? left.pin < right.pin : left.slot < right.slot;
        });
}

struct EndpointPort
{
    quint32                      pin  = 0;
    quint32                      slot = 0;
    QSocIomuxRole                role = QSocIomuxRole::InputValue;
    const QSocIomuxRoutePlan    *route;
    const QSocIomuxEndpointPlan *endpoint;
};

QString endpointName(const EndpointPort &port)
{
    return QSocIomuxGenerator::endpointPortName(port.pin, port.slot, port.role);
}

QString endpointComment(const EndpointPort &port)
{
    QString                         label = port.route->function + "." + port.route->signal;
    static const QRegularExpression unsafePattern(QStringLiteral("[^A-Za-z0-9_.:\\[\\]-]"));
    label.replace(unsafePattern, QStringLiteral("_"));
    return QString(" /* %1 */").arg(label);
}

QList<EndpointPort> endpointPorts(const QSocIomuxPlan &plan)
{
    QList<EndpointPort> ports;
    for (const QSocIomuxRoutePlan &route : plan.routes) {
        for (const QSocIomuxRole role : kRoles) {
            const QSocIomuxEndpointPlan &endpoint = routeRole(route, role);
            if (endpoint.link.isEmpty()) {
                continue;
            }
            ports.append({route.pin, route.slot, role, &route, &endpoint});
        }
    }
    return ports;
}

bool isControlPort(const QSocMmioPortDescription &port)
{
    return port.name == "clk_i" || port.name == "rst_ni" || port.name.startsWith("s_axi_");
}

quint32 interruptLineCount(const QSocIomuxPlan &plan)
{
    const quint32 dataWidth = plan.mmio.dataWidth;
    return dataWidth == 0 ? 0 : (plan.pinCount + dataWidth - 1) / dataWidth;
}

QList<QSocMmioPortDescription> publicPortDescriptions(const QSocIomuxPlan &plan)
{
    QList<QSocMmioPortDescription> ports;
    for (const QSocMmioPortDescription &port : QSocMmioGenerator::describePorts(plan.mmio)) {
        if (isControlPort(port)) {
            ports.append(port);
        }
    }
    if (plan.integration.padCell.declared()) {
        ports.append({"pad_io", "inout", plan.pinCount});
    } else {
        ports.append({"pad_input_value_i", "input", plan.pinCount});
        ports.append({"pad_input_enable_o", "output", plan.pinCount});
        ports.append({"pad_output_value_o", "output", plan.pinCount});
        ports.append({"pad_output_enable_o", "output", plan.pinCount});
    }
    if (plan.option.interrupt) {
        /* Endpoints stay last, because the wrapper aligns route comments by
         * counting back from the end of this list. */
        ports.append({"irq_o", "output", interruptLineCount(plan)});
    }
    for (const EndpointPort &endpoint : endpointPorts(plan)) {
        ports.append(
            {endpointName(endpoint),
             endpoint.role == QSocIomuxRole::InputValue ? QStringLiteral("output")
                                                        : QStringLiteral("input"),
             1});
    }
    return ports;
}

const QSocIomuxRoutePlan *findRoute(const QSocIomuxPlan &plan, quint32 pin, quint32 slot)
{
    for (const QSocIomuxRoutePlan &route : plan.routes) {
        if (route.pin == pin && route.slot == slot) {
            return &route;
        }
    }
    return nullptr;
}

QString vectorRange(quint64 width)
{
    return QString("[%1:0]").arg(width - 1);
}

quint64 denseIndex(const QSocIomuxPlan &plan, quint32 slot, quint32 pin)
{
    return quint64(slot) * plan.pinCount + pin;
}

QString txLaneExpression(const QSocIomuxPlan &plan, quint32 pin, quint32 slot, QSocIomuxRole role)
{
    const QSocIomuxRoutePlan *route = findRoute(plan, pin, slot);
    if (route == nullptr) {
        return QStringLiteral("1'b0");
    }
    const QSocIomuxEndpointPlan &endpoint = routeRole(*route, role);
    if (!endpoint.link.isEmpty()) {
        EndpointPort  port{pin, slot, role, route, &endpoint};
        const QString name = endpointName(port);
        return endpoint.invert ? name + " ^ 1'b1" : name;
    }
    if (endpoint.constant.has_value() && *endpoint.constant == 1) {
        return QStringLiteral("1'b1");
    }
    return QStringLiteral("1'b0");
}

} // namespace

bool QSocIomuxGenerator::checkPadCellPorts(
    const QSocIomuxPlan &plan, const QMap<QString, QString> &cellPorts, QStringList *errors)
{
    const QSocPadCellPlan &cell = plan.integration.padCell;
    if (!cell.declared()) {
        return true;
    }
    QStringList local;
    const auto  expect = [&](const QString &port, const QString &want, const QString &what) {
        if (port.isEmpty()) {
            return;
        }
        if (!cellPorts.contains(port)) {
            local.append(QString("IOMUX_PAD generator.pad_cell.%1: %2 has no port %3")
                             .arg(what, cell.cell, port));
            return;
        }
        const QString have = cellPorts.value(port);
        if (have != want && have != "inout") {
            local.append(QString("IOMUX_PAD generator.pad_cell.%1: %2 port %3 is %4, expected %5")
                             .arg(what, cell.cell, port, have, want));
        }
    };

    expect(cell.portPad, QStringLiteral("inout"), QStringLiteral("port.pad"));
    expect(cell.portInputValue, QStringLiteral("out"), QStringLiteral("port.input_value"));
    expect(cell.portInputEnable, QStringLiteral("in"), QStringLiteral("port.input_enable"));
    expect(cell.portOutputValue, QStringLiteral("in"), QStringLiteral("port.output_value"));
    expect(cell.portOutputEnable, QStringLiteral("in"), QStringLiteral("port.output_enable"));
    for (const QString &port : cell.pull.port) {
        expect(port, QStringLiteral("in"), QStringLiteral("pull.port"));
    }
    for (const QString &port : cell.drive.port) {
        expect(port, QStringLiteral("in"), QStringLiteral("drive.port"));
    }

    /* Every input of the cell must be named somewhere, or the instance would
     * leave it floating, which elaborates and is wrong. */
    QSet<QString> driven
        = {cell.portPad, cell.portInputEnable, cell.portOutputValue, cell.portOutputEnable};
    for (const QString &port : cell.pull.port) {
        driven.insert(port);
    }
    for (const QString &port : cell.drive.port) {
        driven.insert(port);
    }
    QStringList undriven;
    for (auto it = cellPorts.cbegin(); it != cellPorts.cend(); ++it) {
        if ((it.value() == "in" || it.value() == "inout") && !driven.contains(it.key())) {
            undriven.append(it.key());
        }
    }
    if (!undriven.isEmpty()) {
        undriven.sort();
        local.append(QString(
                         "IOMUX_PAD generator.pad_cell: %1 input pins %2 are not named by any "
                         "role, pull, or drive")
                         .arg(cell.cell, undriven.join(", ")));
    }

    local.sort(Qt::CaseSensitive);
    if (errors) {
        *errors = local;
    }
    return local.isEmpty();
}

QString QSocIomuxGenerator::endpointPortName(quint32 pin, quint32 slot, QSocIomuxRole role)
{
    const QString suffix = role == QSocIomuxRole::InputValue ? QStringLiteral("o")
                                                             : QStringLiteral("i");
    return QString("hs_p%1_s%2_%3_%4").arg(pin).arg(slot).arg(roleKey(role), suffix);
}

bool QSocIomuxGenerator::isIomux(const QSocModuleDefinition &definition)
{
    const YAML::Node generator = definition.extraAttributes["generator"];
    if (!generator || !generator.IsMap()) {
        return false;
    }
    const YAML::Node kind = generator["kind"];
    return kind && kind.IsScalar() && kind.Scalar() == "iomux";
}

YAML::Node QSocIomuxGenerator::createDraftGenerator()
{
    YAML::Node generator(YAML::NodeType::Map);
    generator["kind"]  = "iomux";
    generator["bus"]   = "axi4_lite";
    generator["route"] = YAML::Node(YAML::NodeType::Sequence);
    return generator;
}

QStringList QSocIomuxGenerator::validate(const QSocModuleDefinition &definition)
{
    QStringList errors;
    buildPlan(definition, nullptr, &errors);
    return errors;
}

bool QSocIomuxGenerator::buildPlan(
    const QSocModuleDefinition &definition, QSocIomuxPlan *plan, QStringList *errors)
{
    if (plan) {
        *plan = QSocIomuxPlan();
    }

    QSocIomuxPlan localPlan;
    QStringList   localErrors;
    bool          valid = parsePlan(definition, &localPlan, &localErrors);
    if (valid && localErrors.isEmpty()) {
        valid = validatePadCapability(localPlan, &localErrors);
    }
    if (valid && localErrors.isEmpty()) {
        valid = validatePadConstraints(&localPlan, &localErrors);
    }
    if (valid && localErrors.isEmpty()) {
        valid = composeMmio(&localPlan, &localErrors);
    }
    if (valid && localErrors.isEmpty()) {
        valid = QSocMmioGenerator::canonicalizePlan(&localPlan.mmio, &localErrors);
    }
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

QString QSocIomuxGenerator::generateCoreVerilog(const QSocIomuxPlan &plan)
{
    if (plan.pinCount == 0 || plan.hsSlots == 0) {
        return QString();
    }
    const quint32 width = selectorWidth(plan.hsSlots);
    const quint64 dense = quint64(plan.hsSlots) * plan.pinCount;

    QStringList lines;
    lines.append("// Generated by QSoC. Do not edit.");
    lines.append(QString("module %1_core (").arg(plan.moduleName));
    QStringList ports;
    ports.append(QString("    input  wire %1 pad_input_value_i").arg(vectorRange(plan.pinCount)));
    ports.append(QString("    output wire %1 pad_input_enable_o").arg(vectorRange(plan.pinCount)));
    ports.append(QString("    output wire %1 pad_output_value_o").arg(vectorRange(plan.pinCount)));
    ports.append(QString("    output wire %1 pad_output_enable_o").arg(vectorRange(plan.pinCount)));
    ports.append(QString("    input  wire %1 tx_input_enable_i").arg(vectorRange(dense)));
    ports.append(QString("    input  wire %1 tx_output_value_i").arg(vectorRange(dense)));
    ports.append(QString("    input  wire %1 tx_output_enable_i").arg(vectorRange(dense)));
    ports.append(QString("    output wire %1 rx_input_value_o").arg(vectorRange(dense)));
    for (quint32 pin = 0; pin < plan.pinCount; ++pin) {
        ports.append(QString("    input  wire %1 pin_%2_select_i").arg(vectorRange(width)).arg(pin));
        if (plan.option.gpio) {
            ports.append(QString("    input  wire        pin_%1_input_enable_i").arg(pin));
            ports.append(QString("    input  wire        pin_%1_output_value_i").arg(pin));
            ports.append(QString("    input  wire        pin_%1_output_enable_i").arg(pin));
            ports.append(QString("    input  wire        pin_%1_input_enable_src_i").arg(pin));
            ports.append(QString("    input  wire [1:0]  pin_%1_output_value_src_i").arg(pin));
            ports.append(QString("    input  wire [1:0]  pin_%1_output_enable_src_i").arg(pin));
        }
    }
    for (qsizetype index = 0; index < ports.size(); ++index) {
        const QString suffix = index + 1 == ports.size() ? QString() : QString(",");
        lines.append(ports.at(index) + suffix);
    }
    lines.append(");");
    lines.append(QString());

    for (quint32 pin = 0; pin < plan.pinCount; ++pin) {
        lines.append(QString("reg [2:0] tx_bundle_%1;").arg(pin));
        lines.append("always @(*) begin");
        lines.append(QString("    case (pin_%1_select_i)").arg(pin));
        for (quint32 slot = 0; slot < plan.hsSlots; ++slot) {
            const quint64 index = denseIndex(plan, slot, pin);
            lines.append(QString(
                             "        %1'd%2: tx_bundle_%3 = {tx_input_enable_i[%4], "
                             "tx_output_value_i[%4], tx_output_enable_i[%4]};")
                             .arg(width)
                             .arg(slot)
                             .arg(pin)
                             .arg(index));
        }
        lines.append(QString("        default: tx_bundle_%1 = 3'b000;").arg(pin));
        lines.append("    endcase");
        lines.append("end");
        if (!plan.option.gpio) {
            lines.append(QString("assign pad_input_enable_o[%1]  = tx_bundle_%1[2];").arg(pin));
            lines.append(QString("assign pad_output_value_o[%1]  = tx_bundle_%1[1];").arg(pin));
            lines.append(QString("assign pad_output_enable_o[%1] = tx_bundle_%1[0];").arg(pin));
            lines.append(QString());
            continue;
        }

        /* A cross tap reads the slot mux output, never the source mux output,
         * so no encoding of the two source fields can close a loop. */
        lines.append(QString(
                         "assign pad_input_enable_o[%1] = pin_%1_input_enable_src_i"
                         " ? pin_%1_input_enable_i : tx_bundle_%1[2];")
                         .arg(pin));
        lines.append(QString("reg pad_output_value_%1;").arg(pin));
        lines.append("always @(*) begin");
        lines.append(QString("    case (pin_%1_output_value_src_i)").arg(pin));
        lines.append(QString("        2'd1: pad_output_value_%1 = pin_%1_output_value_i;").arg(pin));
        lines.append(QString("        2'd2: pad_output_value_%1 = tx_bundle_%1[2];").arg(pin));
        lines.append(QString("        2'd3: pad_output_value_%1 = tx_bundle_%1[0];").arg(pin));
        lines.append(QString("        default: pad_output_value_%1 = tx_bundle_%1[1];").arg(pin));
        lines.append("    endcase");
        lines.append("end");
        lines.append(QString("assign pad_output_value_o[%1] = pad_output_value_%1;").arg(pin));
        lines.append(QString("reg pad_output_enable_%1;").arg(pin));
        lines.append("always @(*) begin");
        lines.append(QString("    case (pin_%1_output_enable_src_i)").arg(pin));
        lines.append(
            QString("        2'd1: pad_output_enable_%1 = pin_%1_output_enable_i;").arg(pin));
        lines.append(QString("        2'd2: pad_output_enable_%1 = tx_bundle_%1[1];").arg(pin));
        lines.append(QString("        2'd3: pad_output_enable_%1 = 1'b0;").arg(pin));
        lines.append(QString("        default: pad_output_enable_%1 = tx_bundle_%1[0];").arg(pin));
        lines.append("    endcase");
        lines.append("end");
        lines.append(QString("assign pad_output_enable_o[%1] = pad_output_enable_%1;").arg(pin));
        lines.append(QString());
    }

    for (quint32 pin = 0; pin < plan.pinCount; ++pin) {
        for (quint32 slot = 0; slot < plan.hsSlots; ++slot) {
            lines.append(QString("assign rx_input_value_o[%1] = pad_input_value_i[%2];")
                             .arg(denseIndex(plan, slot, pin))
                             .arg(pin));
        }
    }
    lines.append(QString());
    lines.append("endmodule");
    lines.append(QString());
    return lines.join('\n');
}

QString QSocIomuxGenerator::generateConnVerilog(const QSocIomuxPlan &plan)
{
    if (plan.pinCount == 0 || plan.hsSlots == 0) {
        return QString();
    }
    const quint64             dense = quint64(plan.hsSlots) * plan.pinCount;
    const QList<EndpointPort> ports = endpointPorts(plan);

    QStringList lines;
    lines.append("// Generated by QSoC. Do not edit.");
    lines.append(QString("module %1_conn (").arg(plan.moduleName));
    QStringList declarations;
    declarations.append(QString("    output wire %1 tx_input_enable_o").arg(vectorRange(dense)));
    declarations.append(QString("    output wire %1 tx_output_value_o").arg(vectorRange(dense)));
    declarations.append(QString("    output wire %1 tx_output_enable_o").arg(vectorRange(dense)));
    declarations.append(QString("    input  wire %1 rx_input_value_i").arg(vectorRange(dense)));
    QStringList comments = {QString(), QString(), QString(), QString()};
    for (const EndpointPort &port : ports) {
        const QString direction = port.role == QSocIomuxRole::InputValue ? QStringLiteral("output")
                                                                         : QStringLiteral("input ");
        declarations.append(QString("    %1 wire %2").arg(direction, endpointName(port)));
        comments.append(endpointComment(port));
    }
    for (qsizetype index = 0; index < declarations.size(); ++index) {
        const QString suffix = index + 1 == declarations.size() ? QString() : QString(",");
        lines.append(declarations.at(index) + suffix + comments.at(index));
    }
    lines.append(");");
    lines.append(QString());

    for (quint32 pin = 0; pin < plan.pinCount; ++pin) {
        for (quint32 slot = 0; slot < plan.hsSlots; ++slot) {
            const quint64 index = denseIndex(plan, slot, pin);
            lines.append(QString("assign tx_input_enable_o[%1] = %2;")
                             .arg(index)
                             .arg(txLaneExpression(plan, pin, slot, QSocIomuxRole::InputEnable)));
            lines.append(QString("assign tx_output_value_o[%1] = %2;")
                             .arg(index)
                             .arg(txLaneExpression(plan, pin, slot, QSocIomuxRole::OutputValue)));
            lines.append(QString("assign tx_output_enable_o[%1] = %2;")
                             .arg(index)
                             .arg(txLaneExpression(plan, pin, slot, QSocIomuxRole::OutputEnable)));
        }
    }
    lines.append(QString());

    for (const EndpointPort &port : ports) {
        if (port.role != QSocIomuxRole::InputValue) {
            continue;
        }
        const quint64 index      = denseIndex(plan, port.slot, port.pin);
        const QString expression = port.endpoint->invert
                                       ? QString("rx_input_value_i[%1] ^ 1'b1").arg(index)
                                       : QString("rx_input_value_i[%1]").arg(index);
        lines.append(QString("assign %1 = %2;").arg(endpointName(port), expression));
    }
    lines.append(QString());
    lines.append("endmodule");
    lines.append(QString());
    return lines.join('\n');
}

QString QSocIomuxGenerator::generateRegsVerilog(const QSocIomuxPlan &plan)
{
    if (plan.pinCount == 0 || plan.hsSlots == 0) {
        return QString();
    }
    return QSocMmioGenerator::generateVerilog(plan.mmio);
}

QString QSocIomuxGenerator::generateTopVerilog(const QSocIomuxPlan &plan)
{
    if (plan.pinCount == 0 || plan.hsSlots == 0) {
        return QString();
    }
    const quint32                        width       = selectorWidth(plan.hsSlots);
    const quint64                        dense       = quint64(plan.hsSlots) * plan.pinCount;
    const QList<EndpointPort>            endpoints   = endpointPorts(plan);
    const QList<QSocMmioPortDescription> regsPorts   = QSocMmioGenerator::describePorts(plan.mmio);
    const QList<QSocMmioPortDescription> publicPorts = publicPortDescriptions(plan);

    QStringList lines;
    lines.append(QString("module %1 (").arg(plan.moduleName));
    QStringList     declarations;
    QStringList     comments;
    const qsizetype firstEndpoint = publicPorts.size() - endpoints.size();
    for (qsizetype index = 0; index < publicPorts.size(); ++index) {
        const QSocMmioPortDescription &port = publicPorts.at(index);
        const QString keyword = port.direction == "output"  ? QStringLiteral("output")
                                : port.direction == "inout" ? QStringLiteral("inout ")
                                                            : QStringLiteral("input ");
        if (port.width == 1) {
            declarations.append(QString("    %1 wire %2").arg(keyword, port.name));
        } else {
            declarations.append(
                QString("    %1 wire [%2:0] %3").arg(keyword).arg(port.width - 1).arg(port.name));
        }
        comments.append(
            index < firstEndpoint ? QString()
                                  : endpointComment(endpoints.at(index - firstEndpoint)));
    }
    for (qsizetype index = 0; index < declarations.size(); ++index) {
        const QString suffix = index + 1 == declarations.size() ? QString() : QString(",");
        lines.append(declarations.at(index) + suffix + comments.at(index));
    }
    lines.append(");");
    lines.append(QString());

    lines.append(QString("wire %1 tx_input_enable_w;").arg(vectorRange(dense)));
    lines.append(QString("wire %1 tx_output_value_w;").arg(vectorRange(dense)));
    lines.append(QString("wire %1 tx_output_enable_w;").arg(vectorRange(dense)));
    lines.append(QString("wire %1 rx_input_value_w;").arg(vectorRange(dense)));
    const QSocPadCellPlan &padCell = plan.integration.padCell;
    if (padCell.declared()) {
        /* With a pad cell the four control vectors stay inside this module. The
         * core keeps its port names, so they become wires here. */
        lines.append(QString("wire %1 pad_input_value_i;").arg(vectorRange(plan.pinCount)));
        lines.append(QString("wire %1 pad_input_enable_o;").arg(vectorRange(plan.pinCount)));
        lines.append(QString("wire %1 pad_output_value_o;").arg(vectorRange(plan.pinCount)));
        lines.append(QString("wire %1 pad_output_enable_o;").arg(vectorRange(plan.pinCount)));
    }
    for (quint32 pin = 0; pin < plan.pinCount; ++pin) {
        lines.append(QString("wire %1 pin_%2_select_w;").arg(vectorRange(width)).arg(pin));
        if (plan.option.gpio) {
            lines.append(QString("wire       pin_%1_input_enable_w;").arg(pin));
            lines.append(QString("wire       pin_%1_output_value_w;").arg(pin));
            lines.append(QString("wire       pin_%1_output_enable_w;").arg(pin));
            lines.append(QString("wire       pin_%1_input_enable_src_w;").arg(pin));
            lines.append(QString("wire [1:0] pin_%1_output_value_src_w;").arg(pin));
            lines.append(QString("wire [1:0] pin_%1_output_enable_src_w;").arg(pin));
        }
    }
    lines.append(QString());

    const bool needsSync = plan.option.gpio || plan.option.interrupt;
    if (needsSync) {
        /* The pad value crosses into the bus clock before anything samples it.
         * Edge detection compares the second stage against a third. */
        lines.append(QString("reg %1 pad_input_meta_q;").arg(vectorRange(plan.pinCount)));
        lines.append(QString("reg %1 pad_input_sync_q;").arg(vectorRange(plan.pinCount)));
        if (plan.option.interrupt) {
            lines.append(QString("reg %1 pad_input_prev_q;").arg(vectorRange(plan.pinCount)));
        }
        lines.append("always @(posedge clk_i or negedge rst_ni) begin");
        lines.append("    if (!rst_ni) begin");
        lines.append(QString("        pad_input_meta_q <= %1'b0;").arg(plan.pinCount));
        lines.append(QString("        pad_input_sync_q <= %1'b0;").arg(plan.pinCount));
        if (plan.option.interrupt) {
            lines.append(QString("        pad_input_prev_q <= %1'b0;").arg(plan.pinCount));
        }
        lines.append("    end else begin");
        lines.append("        pad_input_meta_q <= pad_input_value_i;");
        lines.append("        pad_input_sync_q <= pad_input_meta_q;");
        if (plan.option.interrupt) {
            lines.append("        pad_input_prev_q <= pad_input_sync_q;");
        }
        lines.append("    end");
        lines.append("end");
        lines.append(QString());
    }

    if (plan.option.interrupt) {
        for (quint32 pin = 0; pin < plan.pinCount; ++pin) {
            lines.append(QString("wire pin_%1_high_detect_w = pad_input_sync_q[%1];").arg(pin));
            lines.append(QString("wire pin_%1_low_detect_w  = ~pad_input_sync_q[%1];").arg(pin));
            lines.append(QString(
                             "wire pin_%1_rise_detect_w = pad_input_sync_q[%1]"
                             " & ~pad_input_prev_q[%1];")
                             .arg(pin));
            lines.append(QString(
                             "wire pin_%1_fall_detect_w = ~pad_input_sync_q[%1]"
                             " & pad_input_prev_q[%1];")
                             .arg(pin));
            for (const char *kind : {"high", "low", "rise", "fall"}) {
                lines.append(QString("wire pin_%1_%2_int_en_w;").arg(pin).arg(QString(kind)));
                lines.append(QString("wire pin_%1_%2_int_pend_w;").arg(pin).arg(QString(kind)));
            }
        }
        lines.append(QString());
    }

    QStringList regsConnections;
    for (const QSocMmioPortDescription &port : regsPorts) {
        if (isControlPort(port)) {
            regsConnections.append(QString("    .%1(%1)").arg(port.name));
        }
    }
    for (quint32 pin = 0; pin < plan.pinCount; ++pin) {
        regsConnections.append(QString("    .pin_%1_select_o(pin_%1_select_w)").arg(pin));
        if (!plan.option.gpio) {
            continue;
        }
        regsConnections.append(QString("    .pin_%1_input_value_i(pad_input_sync_q[%1])").arg(pin));
        for (const char *role :
             {"input_enable",
              "output_value",
              "output_enable",
              "input_enable_src",
              "output_value_src",
              "output_enable_src"}) {
            regsConnections.append(
                QString("    .pin_%1_%2_o(pin_%1_%2_w)").arg(pin).arg(QString(role)));
        }
    }
    if (plan.option.interrupt) {
        for (quint32 pin = 0; pin < plan.pinCount; ++pin) {
            for (const char *kind : {"high", "low", "rise", "fall"}) {
                const QString k = QString(kind);
                regsConnections.append(
                    QString("    .pin_%1_%2_detect_i(pin_%1_%2_detect_w)").arg(pin).arg(k));
                regsConnections.append(
                    QString("    .pin_%1_%2_int_en_o(pin_%1_%2_int_en_w)").arg(pin).arg(k));
                regsConnections.append(
                    QString("    .pin_%1_%2_int_pend_o(pin_%1_%2_int_pend_w)").arg(pin).arg(k));
            }
        }
    }
    lines.append(QString("%1_regs u_regs (").arg(plan.moduleName));
    for (qsizetype index = 0; index < regsConnections.size(); ++index) {
        const QString suffix = index + 1 == regsConnections.size() ? QString() : QString(",");
        lines.append(regsConnections.at(index) + suffix);
    }
    lines.append(");");
    lines.append(QString());

    QStringList connConnections
        = {"    .tx_input_enable_o(tx_input_enable_w)",
           "    .tx_output_value_o(tx_output_value_w)",
           "    .tx_output_enable_o(tx_output_enable_w)",
           "    .rx_input_value_i(rx_input_value_w)"};
    for (const EndpointPort &port : endpoints) {
        connConnections.append(QString("    .%1(%1)").arg(endpointName(port)));
    }
    lines.append(QString("%1_conn u_conn (").arg(plan.moduleName));
    for (qsizetype index = 0; index < connConnections.size(); ++index) {
        const QString suffix = index + 1 == connConnections.size() ? QString() : QString(",");
        lines.append(connConnections.at(index) + suffix);
    }
    lines.append(");");
    lines.append(QString());

    QStringList coreConnections
        = {"    .pad_input_value_i(pad_input_value_i)",
           "    .pad_input_enable_o(pad_input_enable_o)",
           "    .pad_output_value_o(pad_output_value_o)",
           "    .pad_output_enable_o(pad_output_enable_o)",
           "    .tx_input_enable_i(tx_input_enable_w)",
           "    .tx_output_value_i(tx_output_value_w)",
           "    .tx_output_enable_i(tx_output_enable_w)",
           "    .rx_input_value_o(rx_input_value_w)"};
    for (quint32 pin = 0; pin < plan.pinCount; ++pin) {
        coreConnections.append(QString("    .pin_%1_select_i(pin_%1_select_w)").arg(pin));
        if (!plan.option.gpio) {
            continue;
        }
        for (const char *role :
             {"input_enable",
              "output_value",
              "output_enable",
              "input_enable_src",
              "output_value_src",
              "output_enable_src"}) {
            coreConnections.append(
                QString("    .pin_%1_%2_i(pin_%1_%2_w)").arg(pin).arg(QString(role)));
        }
    }
    if (plan.option.interrupt) {
        const quint32 dataWidth = plan.mmio.dataWidth;
        for (quint32 line = 0; line < interruptLineCount(plan); ++line) {
            const quint32 first = line * dataWidth;
            const quint32 last  = std::min(plan.pinCount, first + dataWidth);
            QStringList   terms;
            for (quint32 pin = first; pin < last; ++pin) {
                for (const char *kind : {"high", "low", "rise", "fall"}) {
                    terms.append(QString("(pin_%1_%2_int_pend_w & pin_%1_%2_int_en_w)")
                                     .arg(pin)
                                     .arg(QString(kind)));
                }
            }
            /* The enable gates the line, never the pending bit, so a disabled
             * source still records its event for a poll to find. */
            const QString target = interruptLineCount(plan) == 1 ? QStringLiteral("irq_o")
                                                                 : QString("irq_o[%1]").arg(line);
            lines.append(QString("assign %1 = %2;").arg(target, terms.join("\n    | ")));
        }
        lines.append(QString());
    }

    if (padCell.declared()) {
        const QList<QSocPadTableRow> pullRows  = padPullRows(padCell);
        const bool                   hasPull   = !pullRows.isEmpty();
        const bool                   hasDrive  = !padCell.drive.level.isEmpty();
        const quint32                pullWidth = hasPull ? encodingWidth(pullRows.size()) : 0;
        const quint32 driveWidth = hasDrive ? encodingWidth(padCell.drive.level.size()) : 0;
        const auto    codeWire = [&](const char                                           *kind,
                                     quint32                                               pin,
                                     quint32                                               codeWidth,
                                     const std::function<int(const QSocIomuxRoutePlan &)> &code) {
            /* Each slot carries a constant code from its route. A slot with no
             * route, or a code the selector cannot reach, resolves to zero,
             * which is the none row for pull and the first row for drive. */
            QString expression;
            for (const QSocIomuxRoutePlan &route : plan.routes) {
                if (route.pin != pin) {
                    continue;
                }
                const int value = code(route);
                if (value <= 0) {
                    continue;
                }
                expression += QString("(pin_%1_select_w == %2'd%3) ? %4'd%5 : ")
                                  .arg(pin)
                                  .arg(width)
                                  .arg(route.slot)
                                  .arg(codeWidth)
                                  .arg(value);
            }
            expression += QString("%1'd0").arg(codeWidth);
            lines.append(QString("wire %1 pad_%2_code_%3_w = %4;")
                             .arg(vectorRange(codeWidth))
                             .arg(QString(kind))
                             .arg(pin)
                             .arg(expression));
        };
        const bool weaves   = hasPull && padCell.canKeep() && !padCell.keeperIsNative();
        const auto flagWire = [&](const char *kind, quint32 pin, const QString &mode) {
            QStringList terms;
            for (const QSocIomuxRoutePlan &route : plan.routes) {
                if (route.pin == pin && route.pullMode == mode && !padCell.pull.has(mode)) {
                    terms.append(
                        QString("(pin_%1_select_w == %2'd%3)").arg(pin).arg(width).arg(route.slot));
                }
            }
            lines.append(QString("wire pad_%1_%2_w = %3;")
                             .arg(QString(kind))
                             .arg(pin)
                             .arg(terms.isEmpty() ? QStringLiteral("1'b0") : terms.join(" | ")));
        };
        QStringList pullConcat;
        QStringList keepConcat;
        QStringList oscConcat;
        QStringList driveConcat;
        for (quint32 pin = 0; pin < plan.pinCount; ++pin) {
            if (hasPull) {
                codeWire("pull", pin, pullWidth, [&](const QSocIomuxRoutePlan &route) {
                    if (route.pullMode.isEmpty()) {
                        return 0;
                    }
                    return padPullCode(padCell, route.pullMode, route.pullStrength);
                });
                pullConcat.prepend(QString("pad_pull_code_%1_w").arg(pin));
            }
            if (weaves) {
                flagWire("keep", pin, QStringLiteral("keeper"));
                flagWire("osc", pin, QStringLiteral("oscillator"));
                keepConcat.prepend(QString("pad_keep_%1_w").arg(pin));
                oscConcat.prepend(QString("pad_osc_%1_w").arg(pin));
            }
            if (hasDrive) {
                codeWire("drive", pin, driveWidth, [&](const QSocIomuxRoutePlan &route) {
                    return route.driveLevel.isEmpty() ? 0 : padDriveCode(padCell, route.driveLevel);
                });
                driveConcat.prepend(QString("pad_drive_code_%1_w").arg(pin));
            }
        }
        lines.append(QString());
        QStringList padConnections;
        padConnections.append("    .pad_io(pad_io)");
        if (!padCell.portInputValue.isEmpty()) {
            padConnections.append("    .pad_input_value_o(pad_input_value_i)");
        }
        if (!padCell.portInputEnable.isEmpty()) {
            padConnections.append("    .pad_input_enable_i(pad_input_enable_o)");
        }
        if (!padCell.portOutputValue.isEmpty()) {
            padConnections.append("    .pad_output_value_i(pad_output_value_o)");
        }
        if (!padCell.portOutputEnable.isEmpty()) {
            padConnections.append("    .pad_output_enable_i(pad_output_enable_o)");
        }
        if (hasPull) {
            padConnections.append(
                QString("    .pad_pull_select_i({%1})").arg(pullConcat.join(", ")));
        }
        if (weaves) {
            padConnections.append(QString("    .pad_keep_i({%1})").arg(keepConcat.join(", ")));
            padConnections.append(QString("    .pad_osc_i({%1})").arg(oscConcat.join(", ")));
        }
        if (hasDrive) {
            padConnections.append(
                QString("    .pad_drive_select_i({%1})").arg(driveConcat.join(", ")));
        }
        if (padCell.portInputValue.isEmpty()) {
            lines.append(QString("assign pad_input_value_i = %1'b0;").arg(plan.pinCount));
        }
        lines.append(QString("%1_pad u_pad (").arg(plan.moduleName));
        for (qsizetype index = 0; index < padConnections.size(); ++index) {
            const QString suffix = index + 1 == padConnections.size() ? QString() : QString(",");
            lines.append(padConnections.at(index) + suffix);
        }
        lines.append(");");
        lines.append(QString());
    }

    lines.append(QString("%1_core u_core (").arg(plan.moduleName));
    for (qsizetype index = 0; index < coreConnections.size(); ++index) {
        const QString suffix = index + 1 == coreConnections.size() ? QString() : QString(",");
        lines.append(coreConnections.at(index) + suffix);
    }
    lines.append(");");
    lines.append(QString());
    lines.append("endmodule");
    lines.append(QString());

    return generateCoreVerilog(plan) + "\n" + lines.join('\n');
}

QString QSocIomuxGenerator::generatePadVerilog(const QSocIomuxPlan &plan)
{
    const QSocPadCellPlan &cell = plan.integration.padCell;
    if (plan.pinCount == 0 || !cell.declared()) {
        return QString();
    }
    const QList<QSocPadTableRow> pullRows   = padPullRows(cell);
    const bool                   hasPull    = !pullRows.isEmpty();
    const bool                   hasDrive   = !cell.drive.level.isEmpty();
    const quint32                pullWidth  = hasPull ? encodingWidth(pullRows.size()) : 0;
    const quint32                driveWidth = hasDrive ? encodingWidth(cell.drive.level.size()) : 0;

    QStringList lines;
    lines.append("// Generated by QSoC. Do not edit.");
    lines.append(QString("module %1_pad (").arg(plan.moduleName));
    QStringList ports;
    ports.append(QString("    inout  wire %1 pad_io").arg(vectorRange(plan.pinCount)));
    if (!cell.portInputValue.isEmpty()) {
        ports.append(
            QString("    output wire %1 pad_input_value_o").arg(vectorRange(plan.pinCount)));
    }
    if (!cell.portInputEnable.isEmpty()) {
        ports.append(
            QString("    input  wire %1 pad_input_enable_i").arg(vectorRange(plan.pinCount)));
    }
    if (!cell.portOutputValue.isEmpty()) {
        ports.append(
            QString("    input  wire %1 pad_output_value_i").arg(vectorRange(plan.pinCount)));
    }
    if (!cell.portOutputEnable.isEmpty()) {
        ports.append(
            QString("    input  wire %1 pad_output_enable_i").arg(vectorRange(plan.pinCount)));
    }
    const bool weaves = hasPull && cell.canKeep() && !cell.keeperIsNative();
    if (hasPull) {
        ports.append(QString("    input  wire %1 pad_pull_select_i")
                         .arg(vectorRange(quint64(pullWidth) * plan.pinCount)));
    }
    if (weaves) {
        ports.append(QString("    input  wire %1 pad_keep_i").arg(vectorRange(plan.pinCount)));
        ports.append(QString("    input  wire %1 pad_osc_i").arg(vectorRange(plan.pinCount)));
    }
    if (hasDrive) {
        ports.append(QString("    input  wire %1 pad_drive_select_i")
                         .arg(vectorRange(quint64(driveWidth) * plan.pinCount)));
    }
    for (qsizetype index = 0; index < ports.size(); ++index) {
        const QString suffix = index + 1 == ports.size() ? QString() : QString(",");
        lines.append(ports.at(index) + suffix);
    }
    lines.append(");");
    lines.append(QString());

    const int upCode   = weaves ? padPullCode(cell, QStringLiteral("up"), QString()) : -1;
    const int downCode = weaves ? padPullCode(cell, QStringLiteral("down"), QString()) : -1;
    for (quint32 pin = 0; pin < plan.pinCount; ++pin) {
        if (hasPull) {
            QString selector = QString("pad_pull_select_i[%1:%2]")
                                   .arg((pin + 1) * pullWidth - 1)
                                   .arg(pin * pullWidth);
            if (weaves) {
                /* The keeper follows the pad and the oscillator opposes it. The
                 * feedback reads the pad itself, not the receiver output, so an
                 * input enable of zero does not silently turn either into a
                 * pull-down. The loop closes here, inside the pad module. */
                lines.append(QString(
                                 "wire %1 pad_pull_eff_%2 = pad_keep_i[%2] ? "
                                 "(pad_io[%2] ? %3'd%4 : %3'd%5) : pad_osc_i[%2] ? "
                                 "(pad_io[%2] ? %3'd%5 : %3'd%4) : %6;")
                                 .arg(vectorRange(pullWidth))
                                 .arg(pin)
                                 .arg(pullWidth)
                                 .arg(upCode)
                                 .arg(downCode)
                                 .arg(selector));
                selector = QString("pad_pull_eff_%1").arg(pin);
            }
            appendPadTableCase(&lines, pin, selector, cell.pull.port, pullRows);
        }
        if (hasDrive) {
            appendPadTableCase(
                &lines,
                pin,
                QString("pad_drive_select_i[%1:%2]")
                    .arg((pin + 1) * driveWidth - 1)
                    .arg(pin * driveWidth),
                cell.drive.port,
                cell.drive.level);
        }

        QStringList connections;
        connections.append(QString("    .%1(pad_io[%2])").arg(cell.portPad).arg(pin));
        if (!cell.portInputValue.isEmpty()) {
            connections.append(
                QString("    .%1(pad_input_value_o[%2])").arg(cell.portInputValue).arg(pin));
        }
        if (!cell.portInputEnable.isEmpty()) {
            connections.append(
                QString("    .%1(pad_input_enable_i[%2])").arg(cell.portInputEnable).arg(pin));
        }
        if (!cell.portOutputValue.isEmpty()) {
            connections.append(
                QString("    .%1(pad_output_value_i[%2])").arg(cell.portOutputValue).arg(pin));
        }
        if (!cell.portOutputEnable.isEmpty()) {
            connections.append(
                QString("    .%1(pad_output_enable_i[%2])").arg(cell.portOutputEnable).arg(pin));
        }
        for (const QString &port : cell.pull.port) {
            connections.append(QString("    .%1(%1_%2_w)").arg(port).arg(pin));
        }
        for (const QString &port : cell.drive.port) {
            connections.append(QString("    .%1(%1_%2_w)").arg(port).arg(pin));
        }
        lines.append(QString("%1 u_pad_%2 (").arg(cell.cell).arg(pin));
        for (qsizetype index = 0; index < connections.size(); ++index) {
            const QString suffix = index + 1 == connections.size() ? QString() : QString(",");
            lines.append(connections.at(index) + suffix);
        }
        lines.append(");");
        lines.append(QString());
    }

    if (!cell.constraint.isEmpty()) {
        /* The constraints live here, next to the nets they name, so no proof
         * needs a hierarchical reference. The guard keeps the file Verilog-2001
         * for every tool that does not define FORMAL. Temporal properties clock
         * on the formal global clock because this module has none of its own. */
        lines.append("`ifdef FORMAL");
        bool anyTemporal = false;
        for (const QSocPadConstraint &item : cell.constraint) {
            anyTemporal = anyTemporal || item.temporal;
        }
        if (anyTemporal) {
            lines.append("(* gclk *) wire formal_clk;");
        }
        for (qsizetype index = 0; index < cell.constraint.size(); ++index) {
            const QSocPadConstraint &item = cell.constraint.at(index);
            const QString verb = item.assume ? QStringLiteral("assume") : QStringLiteral("assert");
            for (quint32 pin = 0; pin < plan.pinCount; ++pin) {
                QString body;
                rewriteConstraint(cell, item.body, pin, &body, nullptr, nullptr);
                if (item.temporal) {
                    /* Clocked immediate form. The open engine has $past, $rose
                     * and $stable but no SVA sequences, so an implication is
                     * written as a boolean. */
                    lines.append(QString("always @(posedge formal_clk) %1_%2_%3: %4(%5);")
                                     .arg(verb, item.name)
                                     .arg(pin)
                                     .arg(verb, body));
                } else {
                    lines.append(QString("always @(*) %1_%2_%3: %4(%5);")
                                     .arg(verb, item.name)
                                     .arg(pin)
                                     .arg(verb, body));
                }
            }
        }
        lines.append("`endif");
        lines.append(QString());
    }
    lines.append("endmodule");
    lines.append(QString());
    return lines.join('\n');
}

QString QSocIomuxGenerator::padConstraintForPin(
    const QSocIomuxPlan &plan, qsizetype index, quint32 pin)
{
    const QSocPadCellPlan &cell = plan.integration.padCell;
    if (index < 0 || index >= cell.constraint.size()) {
        return QString();
    }
    QString body;
    rewriteConstraint(cell, cell.constraint.at(index).body, pin, &body, nullptr, nullptr);
    return body;
}

QString QSocIomuxGenerator::generateFileList(const QSocIomuxPlan &plan)
{
    if (plan.pinCount == 0 || plan.hsSlots == 0) {
        return QString();
    }
    QString list = QString("%1_regs.v\n%1_conn.v\n%1.v\n").arg(plan.moduleName);
    if (plan.integration.padCell.declared()) {
        list += QString("%1_pad.v\n").arg(plan.moduleName);
    }
    return list;
}

QString QSocIomuxGenerator::generateIntegrationNetlist(const QSocIomuxPlan &plan)
{
    if (plan.pinCount == 0 || plan.hsSlots == 0) {
        return QString();
    }
    const QSocIomuxIntegrationPlan &integration = plan.integration;

    QStringList lines;
    lines.append("# Generated by QSoC. Do not edit.");
    lines.append("instance:");
    lines.append(QString("  %1:").arg(integration.instance));
    lines.append(QString("    module: %1").arg(plan.moduleName));
    lines.append("    port:");
    lines.append("      clk_i:");
    lines.append(QString("        link: %1").arg(integration.clock));
    lines.append("      rst_ni:");
    lines.append(QString("        link: %1").arg(integration.reset));
    if (integration.padCell.declared()) {
        lines.append("      pad_io:");
        lines.append(QString("        uplink: %1").arg(integration.padIo));
    } else {
        lines.append("      pad_input_value_i:");
        lines.append(QString("        link: %1").arg(integration.padInputValue));
        lines.append("      pad_input_enable_o:");
        lines.append(QString("        link: %1").arg(integration.padInputEnable));
        lines.append("      pad_output_value_o:");
        lines.append(QString("        link: %1").arg(integration.padOutputValue));
        lines.append("      pad_output_enable_o:");
        lines.append(QString("        link: %1").arg(integration.padOutputEnable));
    }
    for (const EndpointPort &port : endpointPorts(plan)) {
        lines.append(QString("      %1:").arg(endpointName(port)));
        lines.append(QString("        link: %1").arg(port.endpoint->link));
        if (port.endpoint->bit.has_value()) {
            lines.append(QString("        bits: \"[%1]\"").arg(*port.endpoint->bit));
        }
    }
    lines.append("bus:");
    lines.append(QString("  %1:").arg(integration.control));
    lines.append(QString("    - instance: %1").arg(integration.instance));
    lines.append("      port: control");
    lines.append(QString());
    return lines.join('\n');
}

YAML::Node QSocIomuxGenerator::describeModuleYaml(const QSocIomuxPlan &plan)
{
    YAML::Node module(YAML::NodeType::Map);
    if (plan.pinCount == 0 || plan.hsSlots == 0) {
        return module;
    }
    const QList<QSocMmioPortDescription> ports = publicPortDescriptions(plan);
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

QString QSocIomuxGenerator::generateReport(const QSocIomuxPlan &plan)
{
    const quint32 dataWidth = plan.mmio.dataWidth;
    if (plan.pinCount == 0 || plan.hsSlots == 0 || (dataWidth != 32 && dataWidth != 64)) {
        return QString();
    }
    /* The report indexes composed registers, so refuse a plan whose register list
     * does not match the layout its own pin count implies. */
    const QList<QSocMmioRegisterPlan> &registers = plan.mmio.registers;
    qsizetype     expected  = qsizetype(1) + selectorWordCount(plan.pinCount, dataWidth);
    const quint32 bankWords = (plan.pinCount + dataWidth - 1) / dataWidth;
    if (plan.option.gpio) {
        expected += qsizetype(4) * bankWords + qsizetype(plan.pinCount);
    }
    if (plan.option.interrupt) {
        expected += qsizetype(8) * bankWords;
    }
    if (registers.size() != expected) {
        return QString();
    }
    const quint32 byteCount = dataWidth / 8;
    const quint32 lanes     = pinsPerWord(dataWidth);
    const quint32 width     = selectorWidth(plan.hsSlots);
    const quint64 aperture  = registers.constLast().byteOffset + byteCount;
    /* Fold the composed capability register so the report cannot publish a value
     * the read function does not emit. */
    quint64 capabilityValue = 0;
    for (const QSocMmioFieldPlan &field : registers.constFirst().fields) {
        if (!field.constantValue.has_value()) {
            continue;
        }
        /* The read function emits a width-sized literal, which Verilog truncates. */
        const quint64 mask = field.width >= 64 ? ~quint64(0) : ((quint64(1) << field.width) - 1);
        capabilityValue |= (*field.constantValue & mask) << field.lsb;
    }

    QStringList lines;
    lines.append(QString("IOMUX route report for %1").arg(plan.moduleName));
    lines.append(QString("pin_count: %1").arg(plan.pinCount));
    lines.append(QString("hs_slots: %1").arg(plan.hsSlots));
    lines.append(QString("data_width: %1").arg(dataWidth));
    lines.append(QString("address_width: %1").arg(plan.mmio.addressWidth));
    lines.append(QString("selector: %1-bit field in a fixed %2-bit lane per pin")
                     .arg(width)
                     .arg(kSelectorLane));
    const qsizetype selectorWords = selectorWordCount(plan.pinCount, dataWidth);
    lines.append(QString("selector registers: %1 at offset 0x%2 to 0x%3")
                     .arg(selectorWords)
                     .arg(QString::number(registers.at(1).byteOffset, 16))
                     .arg(QString::number(registers.at(selectorWords).byteOffset, 16)));
    qsizetype cursor = qsizetype(1) + selectorWords;
    if (plan.option.gpio) {
        const qsizetype count = qsizetype(4) * bankWords + qsizetype(plan.pinCount);
        lines.append(QString("gpio registers: %1 at offset 0x%2 to 0x%3")
                         .arg(count)
                         .arg(QString::number(registers.at(cursor).byteOffset, 16))
                         .arg(QString::number(registers.at(cursor + count - 1).byteOffset, 16)));
        cursor += count;
    }
    if (plan.option.interrupt) {
        const qsizetype count = qsizetype(8) * bankWords;
        lines.append(QString("interrupt registers: %1 at offset 0x%2 to 0x%3")
                         .arg(count)
                         .arg(QString::number(registers.at(cursor).byteOffset, 16))
                         .arg(QString::number(registers.at(cursor + count - 1).byteOffset, 16)));
        lines.append(QString("interrupt lines: %1, one per %2 pins")
                         .arg(interruptLineCount(plan))
                         .arg(dataWidth));
    }
    lines.append(QString("registers total: %1").arg(registers.size()));
    lines.append(QString("aperture: %1 bytes").arg(aperture));
    lines.append(QString("capability: 0x%1 at offset 0x0")
                     .arg(capabilityValue, int(dataWidth / 4), 16, QLatin1Char('0')));
    lines.append("reset: every selector resets to 0 and selects slot 0");
    lines.append("rx: pad input broadcasts to every declared sink regardless of the selector");
    if (plan.integration.padCell.declared()) {
        const QSocPadCellPlan &cell = plan.integration.padCell;
        lines.append(QString("pad cell: %1, pull modes %2, drive levels %3, constraints %4")
                         .arg(cell.cell)
                         .arg(cell.pull.mode.size())
                         .arg(cell.drive.level.size())
                         .arg(cell.constraint.size()));
    }
    lines.append(QString());

    qsizetype routeIndex = 0;
    for (quint32 pin = 0; pin < plan.pinCount; ++pin) {
        lines.append(QString("pin %1 selector word %2 lsb %3 offset 0x%4")
                         .arg(pin)
                         .arg(pin / lanes)
                         .arg((pin % lanes) * kSelectorLane)
                         .arg(QString::number(registers.at(1 + pin / lanes).byteOffset, 16)));
        QStringList unusedSlots;
        for (quint32 slot = 0; slot < plan.hsSlots; ++slot) {
            const bool hasRoute = routeIndex < plan.routes.size()
                                  && plan.routes.at(routeIndex).pin == pin
                                  && plan.routes.at(routeIndex).slot == slot;
            if (!hasRoute) {
                unusedSlots.append(QString::number(slot));
                continue;
            }

            const QSocIomuxRoutePlan &route = plan.routes.at(routeIndex++);
            lines.append(QString("  slot %1 function %2 signal %3")
                             .arg(route.slot)
                             .arg(route.function, route.signal));
            for (const QSocIomuxRole role : kRoles) {
                const QSocIomuxEndpointPlan &endpoint = routeRole(route, role);
                QString                      value;
                if (!endpoint.link.isEmpty()) {
                    value = QString("link %1").arg(endpoint.link);
                    if (endpoint.bit.has_value()) {
                        value += QString(" bit %1").arg(*endpoint.bit);
                    }
                    if (endpoint.invert) {
                        value += " invert";
                    }
                } else if (endpoint.constant.has_value()) {
                    value = QString("constant %1").arg(*endpoint.constant);
                } else if (role == QSocIomuxRole::InputValue) {
                    value = QStringLiteral("no sink");
                } else {
                    value = QStringLiteral("constant 0");
                }
                lines.append(QString("    %1: %2").arg(roleKey(role), value));
            }
            if (!route.pullMode.isEmpty()) {
                lines.append(
                    route.pullStrength.isEmpty()
                        ? QString("    pull: %1").arg(route.pullMode)
                        : QString("    pull: %1 %2").arg(route.pullMode, route.pullStrength));
            }
            if (!route.driveLevel.isEmpty()) {
                lines.append(QString("    drive: %1").arg(route.driveLevel));
            }
        }
        lines.append(
            QString("  unused slots: %1")
                .arg(unusedSlots.isEmpty() ? QStringLiteral("none") : unusedSlots.join(", ")));
    }
    lines.append(QString());
    lines.append("undeclared pin/slot pairs drive a zero tx bundle");
    lines.append(QString());
    return lines.join('\n');
}
