// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsociomuxgenerator.h"

#include "common/qsocmodulemanager.h"
#include "common/qsocverilogutils.h"

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <utility>
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
       "build",
       "option",
       "pad_cell",
       "pad_cells",
       "pin_cell",
       "pad_model",
       "io_lib",
       "io_ring",
       "integration",
       "route"};
const QSet<QString> kOptionKeys = {"gpio", "interrupt", "pad_control", "invert", "rx_override"};
const QSet<QString> kIntegrationKeys = {"instance", "clock", "reset", "control", "pad", "force"};
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
       "control"};
const QSet<QString> kEndpointKeys    = {"link", "bit", "invert", "open_drain"};
const QSet<QString> kRoutePullKeys   = {"mode", "strength", "link", "bit", "invert", "on", "off"};
const QSet<QString> kRouteSelectKeys = {"link", "bit", "invert", "on", "off"};

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
    bool                      hasSafe,
    QSocIomuxIntegrationPlan *integration,
    QStringList              *errors)
{
    const QString path = "generator.integration";
    if (!validateMap(node, kIntegrationKeys, path, errors)) {
        return false;
    }

    bool valid = true;
    if (hasSafe && !node["force"]) {
        appendError(errors, "REQUIRED", path + ".force", "property is required with pad_cell.safe");
        valid = false;
    } else if (!hasSafe && node["force"]) {
        appendError(errors, "CONFLICT", path + ".force", "needs pad_cell.safe to be declared");
        valid = false;
    } else if (node["force"]) {
        valid = parseIdentifier(node["force"], path + ".force", &integration->force, errors)
                && valid;
    }
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
        for (const auto &entry : pads) {
            if (pad[entry.first.toStdString()]) {
                appendError(
                    errors,
                    "CONFLICT",
                    padPath + "." + entry.first,
                    "is owned by pad_cell, remove it");
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

const QSet<QString> kPadCellKeys  = {"cell", "port", "pull", "control", "safe", "constraint"};
const QSet<QString> kPadModelKeys = {"mode", "control"};
const QSet<QString> kIoLibKeys    = {"kind", "width", "height", "variant"};
const QSet<QString> kIoLibKinds   = {"signal", "power", "corner", "fill", "other"};
const QSet<QString> kIoRingKeys = {"die", "corner", "prefix", "orient", "power", "direct", "sides"};
const QSet<QString> kIoRingDieKeys = {"width", "height"};
const QSet<QString> kIoRingItemKeys
    = {"pin", "power", "cell", "direct", "id", "name", "offset", "gap"};
const QSet<QString> kIoRingOrientKeys = {"west", "south", "east", "north", "nw", "sw", "se", "ne"};
const QSet<QString> kDefOrients       = {"N", "S", "E", "W", "FN", "FS", "FE", "FW"};
const QSet<QString> kIoRingDirectKeys = {"cell", "port"};
const QSet<QString> kConstraintKeys   = {"name", "kind", "expr", "property"};
const QSet<QString> kPadPortKeys
    = {"pad", "input_value", "input_enable", "output_value", "output_enable"};
const QSet<QString> kPadPullKeys     = {"port", "table", "kind"};
const QSet<QString> kPadControlKeys  = {"port", "table", "default"};
constexpr qsizetype kMaximumControls = 16;
constexpr qsizetype kMaximumRows     = 16; /* a 4-bit lane of the pad word */
constexpr qsizetype kPadWordLanes    = 4;  /* controls in pin_pad_ctrl */
constexpr qsizetype kCtlWordLanes    = 8;  /* controls per pin_ctl_k word */

/**
 * @brief Names a control may not take, because the generator owns them.
 *
 * A control name becomes `pin_N_<name>` in registers and `pad_<name>_select`
 * in ports, so it must stay clear of every name those prefixes already form.
 */
bool reservedControlName(const QString &name)
{
    static const QSet<QString> fixed
        = {QStringLiteral("input_value"),
           QStringLiteral("input_enable"),
           QStringLiteral("output_value"),
           QStringLiteral("output_enable"),
           QStringLiteral("select"),
           QStringLiteral("pull"),
           QStringLiteral("pull_mode"),
           QStringLiteral("up_sel"),
           QStringLiteral("down_sel")};
    return fixed.contains(name) || name.startsWith(QStringLiteral("rx_"))
           || name.endsWith(QStringLiteral("_src")) || name.endsWith(QStringLiteral("_inv"))
           || name.endsWith(QStringLiteral("_detect")) || name.endsWith(QStringLiteral("_int_en"))
           || name.endsWith(QStringLiteral("_int_pend"));
}

/**
 * @brief A row or mode label: printed as is, so no blanks and never empty.
 */
bool validRowLabel(const QString &label)
{
    static const QRegularExpression blank(QStringLiteral("\\s"));
    return !label.isEmpty() && !label.contains(blank);
}

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
        if (!validRowLabel(label)) {
            appendError(errors, "LABEL", path, "strength labels must be single words");
            valid = false;
            continue;
        }
        valid = parsePadRow(entry.second, path + "." + label, label, width, rows, errors) && valid;
    }
    return valid;
}

bool parsePullRequest(
    const YAML::Node &node, const QString &path, QSocIomuxPullRequest *request, QStringList *errors);

bool parsePadCell(
    const YAML::Node &node, const QString &path, QSocPadCellPlan *plan, QStringList *errors)
{
    plan->path = path;
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
            if (!validRowLabel(name)) {
                appendError(errors, "LABEL", tablePath, "mode names must be single words");
                valid = false;
                continue;
            }
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

    if (node["control"]) {
        const YAML::Node controls    = node["control"];
        const QString    controlPath = path + ".control";
        if (!controls.IsMap()) {
            appendError(errors, "TYPE", controlPath, "must be a map of control names");
            return false;
        }
        for (const auto &entry : controls) {
            if (!entry.first.IsScalar()) {
                appendError(errors, "TYPE", controlPath, "control names must be scalar");
                valid = false;
                continue;
            }
            QSocPadControlPlan item;
            item.name              = QString::fromStdString(entry.first.Scalar());
            const QString itemPath = controlPath + "." + item.name;
            if (!QSocVerilogUtils::isValidVerilogIdentifier(item.name)) {
                appendError(errors, "IDENTIFIER", itemPath, "must be a Verilog identifier");
                valid = false;
                continue;
            }
            if (reservedControlName(item.name)) {
                appendError(errors, "RESERVED", itemPath, "name is owned by the generator");
                valid = false;
                continue;
            }
            for (const QSocPadControlPlan &other : plan->control) {
                if (other.name == item.name) {
                    appendError(errors, "DUPLICATE", itemPath, "control is declared twice");
                    valid = false;
                }
            }
            if (!validateMap(entry.second, kPadControlKeys, itemPath, errors)) {
                valid = false;
                continue;
            }
            const YAML::Node body = entry.second;
            if (!body["port"] || !body["port"].IsSequence()) {
                appendError(
                    errors, "REQUIRED", itemPath + ".port", "must be a sequence of port names");
                valid = false;
                continue;
            }
            for (const YAML::Node &portEntry : body["port"]) {
                QString name;
                valid = parseIdentifier(portEntry, itemPath + ".port", &name, errors) && valid;
                item.port.append(name);
            }
            if (!body["table"] || !body["table"].IsMap()) {
                appendError(errors, "REQUIRED", itemPath + ".table", "must be a map of labelled rows");
                valid = false;
                continue;
            }
            for (const auto &rowEntry : body["table"]) {
                const QString label = QString::fromStdString(rowEntry.first.Scalar());
                if (!validRowLabel(label)) {
                    appendError(
                        errors, "LABEL", itemPath + ".table", "row labels must be single words");
                    valid = false;
                    continue;
                }
                valid = parsePadRow(
                            rowEntry.second,
                            itemPath + ".table." + label,
                            label,
                            item.port.size(),
                            &item.row,
                            errors)
                        && valid;
            }
            if (item.row.isEmpty()) {
                appendError(errors, "REQUIRED", itemPath + ".table", "needs at least one row");
                valid = false;
            }
            if (item.row.size() > kMaximumRows) {
                appendError(
                    errors,
                    "RANGE",
                    itemPath + ".table",
                    QString("has %1 rows, at most %2").arg(item.row.size()).arg(kMaximumRows));
                valid = false;
            }
            if (body["default"]) {
                QString label;
                if (parseLabel(body["default"], itemPath + ".default", &label, errors)) {
                    item.defaultRow = -1;
                    for (qsizetype index = 0; index < item.row.size(); ++index) {
                        if (item.row.at(index).label == label) {
                            item.defaultRow = int(index);
                        }
                    }
                    if (item.defaultRow < 0) {
                        appendError(
                            errors,
                            "VALUE",
                            itemPath + ".default",
                            QString("names no row of %1").arg(item.name));
                        valid           = false;
                        item.defaultRow = 0;
                    }
                } else {
                    valid = false;
                }
            }
            plan->control.append(item);
        }
        if (plan->control.size() > kMaximumControls) {
            appendError(
                errors,
                "RANGE",
                controlPath,
                QString("declares %1 controls, at most %2")
                    .arg(plan->control.size())
                    .arg(kMaximumControls));
            valid = false;
        }
    }

    if (node["safe"]) {
        const YAML::Node safe     = node["safe"];
        const QString    safePath = path + ".safe";
        if (!safe.IsMap()) {
            appendError(errors, "TYPE", safePath, "must be a map");
            return false;
        }
        plan->safe.declared = true;
        for (const auto &entry : safe) {
            if (!entry.first.IsScalar()) {
                appendError(errors, "TYPE", safePath, "keys must be scalar");
                valid = false;
                continue;
            }
            const QString key     = QString::fromStdString(entry.first.Scalar());
            const QString keyPath = safePath + "." + key;
            quint8       *bit     = key == "input_enable"    ? &plan->safe.inputEnable
                                    : key == "output_value"  ? &plan->safe.outputValue
                                    : key == "output_enable" ? &plan->safe.outputEnable
                                                             : nullptr;
            if (bit != nullptr) {
                quint64 value = 0;
                if (parseStrictUnsigned(entry.second, keyPath, 0, 1, &value, errors)) {
                    *bit = quint8(value);
                } else {
                    valid = false;
                }
            } else if (key == "pull") {
                valid = parsePullRequest(entry.second, keyPath, &plan->safe.pull, errors) && valid;
            } else if (key == "input_value") {
                appendError(errors, "ROLE", keyPath, "input_value is read, not driven");
                valid = false;
            } else {
                QString label;
                if (parseLabel(entry.second, keyPath, &label, errors)) {
                    plan->safe.control.insert(key, label);
                } else {
                    valid = false;
                }
            }
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

/**
 * @brief Parse the pad classes and the pin to class map.
 *
 * `pad_cell` declares one class named after its cell. `pad_cells` declares
 * several by name, and `pin_cell` says which one each pin instantiates, with
 * `default` for every pin it does not name.
 */
bool parsePadClasses(const YAML::Node &generator, QSocIomuxPlan *plan, QStringList *errors)
{
    bool valid = true;
    if (generator["pad_cell"] && generator["pad_cells"]) {
        appendError(
            errors, "CONFLICT", "generator.pad_cells", "declare pad_cell or pad_cells, not both");
        return false;
    }
    if (generator["pad_cell"]) {
        QSocPadCellPlan cell;
        valid
            = parsePadCell(generator["pad_cell"], QStringLiteral("generator.pad_cell"), &cell, errors)
              && valid;
        cell.name = cell.cell;
        plan->padCells.append(cell);
    }
    if (generator["pad_cells"]) {
        const YAML::Node cells = generator["pad_cells"];
        if (!cells.IsMap() || cells.size() == 0) {
            appendError(
                errors, "TYPE", "generator.pad_cells", "must be a map of class name to pad cell");
            return false;
        }
        for (const auto &entry : cells) {
            QSocPadCellPlan cell;
            if (!parseIdentifier(entry.first, "generator.pad_cells", &cell.name, errors)) {
                valid = false;
                continue;
            }
            valid = parsePadCell(entry.second, "generator.pad_cells." + cell.name, &cell, errors)
                    && valid;
            plan->padCells.append(cell);
        }
    }

    plan->pinClass = QList<int>(plan->pinCount, plan->padCells.size() == 1 ? 0 : -1);
    if (generator["pin_cell"]) {
        const YAML::Node map  = generator["pin_cell"];
        const QString    path = QStringLiteral("generator.pin_cell");
        if (plan->padCells.isEmpty()) {
            appendError(errors, "CONFLICT", path, "needs pad_cells to be declared");
            return false;
        }
        if (!map.IsMap()) {
            appendError(errors, "TYPE", path, "must be a map of pin or range to class name");
            return false;
        }
        const auto classOf = [&](const YAML::Node &node, const QString &keyPath) {
            QString name;
            if (!parseScalar(node, keyPath, &name, errors)) {
                return -1;
            }
            for (qsizetype index = 0; index < plan->padCells.size(); ++index) {
                if (plan->padCells.at(index).name == name) {
                    return int(index);
                }
            }
            appendError(
                errors, "UNKNOWN", keyPath, QString("no class named %1 under pad_cells").arg(name));
            return -1;
        };
        static const QRegularExpression rangeKey("^(\\d+)(?:-(\\d+))?$");
        int                             fallback = -1;
        QList<int>                      assigned(plan->pinCount, -1);
        for (const auto &entry : map) {
            if (!entry.first.IsScalar()) {
                appendError(errors, "TYPE", path, "keys must be scalar");
                valid = false;
                continue;
            }
            const QString key     = QString::fromStdString(entry.first.Scalar());
            const QString keyPath = path + "." + key;
            if (key == "default") {
                fallback = classOf(entry.second, keyPath);
                valid    = valid && fallback >= 0;
                continue;
            }
            const QRegularExpressionMatch match = rangeKey.match(key);
            if (!match.hasMatch()) {
                appendError(errors, "VALUE", keyPath, "must be default, a pin number, or a range a-b");
                valid = false;
                continue;
            }
            const quint64 first = match.captured(1).toULongLong();
            const quint64 last  = match.captured(2).isEmpty() ? first
                                                              : match.captured(2).toULongLong();
            if (last >= plan->pinCount || first > last) {
                appendError(
                    errors,
                    "RANGE",
                    keyPath,
                    QString("must lie below pin_count %1, low end first").arg(plan->pinCount));
                valid = false;
                continue;
            }
            const int index = classOf(entry.second, keyPath);
            if (index < 0) {
                valid = false;
                continue;
            }
            for (quint64 pin = first; pin <= last; ++pin) {
                if (assigned.at(qsizetype(pin)) >= 0) {
                    appendError(
                        errors, "CONFLICT", keyPath, QString("pin %1 is already assigned").arg(pin));
                    valid = false;
                    continue;
                }
                assigned[qsizetype(pin)] = index;
            }
        }
        for (qsizetype pin = 0; pin < plan->pinClass.size(); ++pin) {
            if (assigned.at(pin) >= 0) {
                plan->pinClass[pin] = assigned.at(pin);
            } else if (fallback >= 0) {
                plan->pinClass[pin] = fallback;
            }
        }
    }
    if (!plan->padCells.isEmpty()) {
        QStringList unassigned;
        for (qsizetype pin = 0; pin < plan->pinClass.size(); ++pin) {
            if (plan->pinClass.at(pin) < 0) {
                unassigned.append(QString::number(pin));
            }
        }
        if (!unassigned.isEmpty()) {
            appendError(
                errors,
                "REQUIRED",
                "generator.pin_cell.default",
                QString("pins %1 name no class").arg(unassigned.join(", ")));
            valid = false;
        }
    }
    if (generator["pad_model"]) {
        const YAML::Node model = generator["pad_model"];
        const QString    path  = QStringLiteral("generator.pad_model");
        if (!validateMap(model, kPadModelKeys, path, errors)) {
            return false;
        }
        static const QStringList fixedModes
            = {QStringLiteral("none"),
               QStringLiteral("up"),
               QStringLiteral("down"),
               QStringLiteral("keeper"),
               QStringLiteral("oscillator")};
        for (const auto &[key, target] :
             {std::pair<const char *, QStringList *>{"mode", &plan->padModeOrder},
              std::pair<const char *, QStringList *>{"control", &plan->padControlOrder}}) {
            if (!model[key]) {
                continue;
            }
            const QString keyPath = path + "." + key;
            if (!model[key].IsSequence()) {
                appendError(errors, "TYPE", keyPath, "must be a sequence of names");
                valid = false;
                continue;
            }
            for (const YAML::Node &entry : model[key]) {
                QString name;
                if (!parseLabel(entry, keyPath, &name, errors)) {
                    valid = false;
                    continue;
                }
                if (target->contains(name)) {
                    appendError(errors, "CONFLICT", keyPath, QString("%1 is listed twice").arg(name));
                    valid = false;
                } else if (QString(key) == "mode" && fixedModes.contains(name)) {
                    appendError(
                        errors,
                        "VALUE",
                        keyPath,
                        QString("%1 has a fixed number and is not listed").arg(name));
                    valid = false;
                } else {
                    target->append(name);
                }
            }
        }
    }
    plan->padModel
        = QSocIomuxGenerator::padModel(plan->padCells, plan->padModeOrder, plan->padControlOrder);
    return valid;
}

bool parseMicrons(const YAML::Node &node, const QString &path, double *value, QStringList *errors)
{
    if (!node || !node.IsScalar()) {
        appendError(errors, "TYPE", path, "must be a number of microns");
        return false;
    }
    bool         ok     = false;
    const double parsed = QString::fromStdString(node.Scalar()).toDouble(&ok);
    if (!ok || parsed < 0) {
        appendError(errors, "VALUE", path, "must be a non-negative number of microns");
        return false;
    }
    *value = parsed;
    return true;
}

/**
 * @brief Parse `io_lib`: per cell its kind, size, and side variants.
 */
bool parseIoLib(const YAML::Node &node, QSocIomuxPlan *plan, QStringList *errors)
{
    const QString path = QStringLiteral("generator.io_lib");
    if (!node.IsMap()) {
        appendError(errors, "TYPE", path, "must be a map of cell name to its properties");
        return false;
    }
    bool valid = true;
    for (const auto &entry : node) {
        QSocIoLibCell cell;
        if (!parseIdentifier(entry.first, path, &cell.name, errors)) {
            valid = false;
            continue;
        }
        const QString cellPath = path + "." + cell.name;
        if (!validateMap(entry.second, kIoLibKeys, cellPath, errors)) {
            valid = false;
            continue;
        }
        if (!entry.second["kind"]) {
            appendError(errors, "REQUIRED", cellPath + ".kind", "property is required");
            valid = false;
        } else if (!parseScalar(entry.second["kind"], cellPath + ".kind", &cell.kind, errors)) {
            valid = false;
        } else if (!kIoLibKinds.contains(cell.kind)) {
            appendError(
                errors,
                "VALUE",
                cellPath + ".kind",
                "must be signal, power, corner, fill, or other");
            valid = false;
        }
        if (entry.second["width"]) {
            valid = parseMicrons(entry.second["width"], cellPath + ".width", &cell.width, errors)
                    && valid;
        }
        if (entry.second["height"]) {
            valid = parseMicrons(entry.second["height"], cellPath + ".height", &cell.height, errors)
                    && valid;
        }
        if (entry.second["variant"]) {
            const YAML::Node variant = entry.second["variant"];
            if (!variant.IsMap()) {
                appendError(errors, "TYPE", cellPath + ".variant", "must be a map of side to module");
                valid = false;
            } else {
                for (const auto &item : variant) {
                    const QString side = QString::fromStdString(item.first.Scalar());
                    if (!QSocIomuxGenerator::ringSides().contains(side)) {
                        appendError(
                            errors,
                            "VALUE",
                            cellPath + ".variant." + side,
                            "must be west, south, east, or north");
                        valid = false;
                        continue;
                    }
                    QString module;
                    if (parseIdentifier(item.second, cellPath + ".variant." + side, &module, errors)) {
                        cell.variant.insert(side, module);
                    } else {
                        valid = false;
                    }
                }
            }
        }
        plan->ioLib.insert(cell.name, cell);
    }
    return valid;
}

/**
 * @brief Parse `io_ring`: the die, the corner, the supplies, the direct
 * cells, and each side in order.
 */
bool parseIoRing(const YAML::Node &node, QSocIomuxPlan *plan, QStringList *errors)
{
    const QString path = QStringLiteral("generator.io_ring");
    if (!validateMap(node, kIoRingKeys, path, errors)) {
        return false;
    }
    QSocIoRingPlan &ring = plan->ioRing;
    ring.declared        = true;
    bool valid           = true;
    if (node["die"]) {
        const QString diePath = path + ".die";
        if (!validateMap(node["die"], kIoRingDieKeys, diePath, errors)) {
            valid = false;
        } else {
            for (const auto &[key, target] :
                 {std::pair<const char *, double *>{"width", &ring.dieWidth},
                  std::pair<const char *, double *>{"height", &ring.dieHeight}}) {
                if (!node["die"][key]) {
                    appendError(errors, "REQUIRED", diePath + "." + key, "property is required");
                    valid = false;
                } else {
                    valid = parseMicrons(node["die"][key], diePath + "." + key, target, errors)
                            && valid;
                }
            }
        }
    }
    if (node["corner"]) {
        valid = parseIdentifier(node["corner"], path + ".corner", &ring.corner, errors) && valid;
    }
    if (node["prefix"]) {
        valid = parseScalar(node["prefix"], path + ".prefix", &ring.prefix, errors) && valid;
    }
    if (node["orient"]) {
        if (!validateMap(node["orient"], kIoRingOrientKeys, path + ".orient", errors)) {
            valid = false;
        } else {
            for (const auto &entry : node["orient"]) {
                const QString key = QString::fromStdString(entry.first.Scalar());
                QString       value;
                if (!parseScalar(entry.second, path + ".orient." + key, &value, errors)) {
                    valid = false;
                } else if (!kDefOrients.contains(value)) {
                    appendError(
                        errors,
                        "VALUE",
                        path + ".orient." + key,
                        "must be a DEF orientation: N, S, E, W, FN, FS, FE, or FW");
                    valid = false;
                } else {
                    ring.orient.insert(key, value);
                }
            }
        }
    }
    if (node["power"]) {
        if (!node["power"].IsMap()) {
            appendError(errors, "TYPE", path + ".power", "must be a map of net to cell");
            valid = false;
        } else {
            for (const auto &entry : node["power"]) {
                QString net;
                QString cell;
                if (parseIdentifier(entry.first, path + ".power", &net, errors)
                    && parseIdentifier(entry.second, path + ".power." + net, &cell, errors)) {
                    ring.power.insert(net, cell);
                } else {
                    valid = false;
                }
            }
        }
    }
    if (node["direct"]) {
        if (!node["direct"].IsMap()) {
            appendError(errors, "TYPE", path + ".direct", "must be a map of name to cell");
            valid = false;
        } else {
            for (const auto &entry : node["direct"]) {
                QSocIoRingDirect item;
                if (!parseIdentifier(entry.first, path + ".direct", &item.key, errors)) {
                    valid = false;
                    continue;
                }
                const QString itemPath = path + ".direct." + item.key;
                if (!validateMap(entry.second, kIoRingDirectKeys, itemPath, errors)) {
                    valid = false;
                    continue;
                }
                if (!entry.second["cell"]) {
                    appendError(errors, "REQUIRED", itemPath + ".cell", "property is required");
                    valid = false;
                } else {
                    valid = parseIdentifier(
                                entry.second["cell"], itemPath + ".cell", &item.cell, errors)
                            && valid;
                }
                if (entry.second["port"]) {
                    if (!entry.second["port"].IsMap()) {
                        appendError(
                            errors, "TYPE", itemPath + ".port", "must be a map of cell port to net");
                        valid = false;
                    } else {
                        for (const auto &port : entry.second["port"]) {
                            QString name;
                            QString net;
                            if (!parseIdentifier(port.first, itemPath + ".port", &name, errors)
                                || !parseScalar(
                                    port.second, itemPath + ".port." + name, &net, errors)) {
                                valid = false;
                                continue;
                            }
                            const bool constant = net == "1'b0" || net == "1'b1";
                            if (!constant && !QSocVerilogUtils::isValidVerilogIdentifier(net)) {
                                appendError(
                                    errors,
                                    "IDENTIFIER",
                                    itemPath + ".port." + name,
                                    "must be a Verilog identifier, 1'b0, or 1'b1");
                                valid = false;
                                continue;
                            }
                            item.port.insert(name, net);
                        }
                    }
                }
                ring.direct.append(item);
            }
        }
    }
    if (!node["sides"]) {
        appendError(errors, "REQUIRED", path + ".sides", "property is required");
        return false;
    }
    if (!node["sides"].IsMap()) {
        appendError(errors, "TYPE", path + ".sides", "must be a map of side to its items");
        return false;
    }
    QMap<quint32, QString> placed;
    for (const auto &entry : node["sides"]) {
        const QString side     = QString::fromStdString(entry.first.Scalar());
        const QString sidePath = path + ".sides." + side;
        if (!QSocIomuxGenerator::ringSides().contains(side)) {
            appendError(errors, "VALUE", sidePath, "must be west, south, east, or north");
            valid = false;
            continue;
        }
        if (!entry.second.IsSequence()) {
            appendError(errors, "TYPE", sidePath, "must be a sequence of items");
            valid = false;
            continue;
        }
        int index = 0;
        for (const YAML::Node &itemNode : entry.second) {
            const QString itemPath = QString("%1[%2]").arg(sidePath).arg(index++);
            if (!validateMap(itemNode, kIoRingItemKeys, itemPath, errors)) {
                valid = false;
                continue;
            }
            QSocIoRingItem item;
            int            kinds = 0;
            if (itemNode["pin"]) {
                ++kinds;
                item.kind   = QSocIoRingItem::Pin;
                quint64 pin = 0;
                if (!parseStrictUnsigned(
                        itemNode["pin"], itemPath + ".pin", 0, plan->pinCount - 1, &pin, errors)) {
                    valid = false;
                    continue;
                }
                item.pin = quint32(pin);
                if (placed.contains(item.pin)) {
                    appendError(
                        errors,
                        "CONFLICT",
                        itemPath + ".pin",
                        QString("pin %1 is already on the %2 side")
                            .arg(pin)
                            .arg(placed.value(item.pin)));
                    valid = false;
                    continue;
                }
                placed.insert(item.pin, side);
            }
            if (itemNode["power"]) {
                ++kinds;
                item.kind = QSocIoRingItem::Power;
                valid = parseIdentifier(itemNode["power"], itemPath + ".power", &item.name, errors)
                        && valid;
                if (!ring.power.contains(item.name)) {
                    appendError(
                        errors,
                        "UNKNOWN",
                        itemPath + ".power",
                        QString("net %1 is not declared under io_ring.power").arg(item.name));
                    valid = false;
                }
                if (itemNode["id"]) {
                    quint64 id = 0;
                    if (parseStrictUnsigned(itemNode["id"], itemPath + ".id", 0, 65535, &id, errors)) {
                        item.id = int(id);
                    } else {
                        valid = false;
                    }
                }
            }
            if (itemNode["cell"]) {
                ++kinds;
                item.kind = QSocIoRingItem::Cell;
                valid = parseIdentifier(itemNode["cell"], itemPath + ".cell", &item.name, errors)
                        && valid;
                if (itemNode["name"]) {
                    valid = parseIdentifier(
                                itemNode["name"], itemPath + ".name", &item.instance, errors)
                            && valid;
                }
            }
            if (itemNode["direct"]) {
                ++kinds;
                item.kind = QSocIoRingItem::Direct;
                valid = parseIdentifier(itemNode["direct"], itemPath + ".direct", &item.name, errors)
                        && valid;
                bool known = false;
                for (const QSocIoRingDirect &direct : ring.direct) {
                    known = known || direct.key == item.name;
                }
                if (!known) {
                    appendError(
                        errors,
                        "UNKNOWN",
                        itemPath + ".direct",
                        QString("%1 is not declared under io_ring.direct").arg(item.name));
                    valid = false;
                }
            }
            if (kinds != 1) {
                appendError(
                    errors, "VALUE", itemPath, "needs exactly one of pin, power, cell, or direct");
                valid = false;
                continue;
            }
            if (itemNode["offset"]) {
                valid = parseMicrons(itemNode["offset"], itemPath + ".offset", &item.offset, errors)
                        && valid;
            }
            if (itemNode["gap"]) {
                valid = parseMicrons(itemNode["gap"], itemPath + ".gap", &item.gap, errors)
                        && valid;
            }
            ring.side[side].append(item);
        }
    }
    /* Every pin has a place; no automatic placement fills the gaps yet. */
    QStringList missing;
    for (quint32 pin = 0; pin < plan->pinCount; ++pin) {
        if (!placed.contains(pin)) {
            missing.append(QString::number(pin));
        }
    }
    if (!missing.isEmpty()) {
        appendError(
            errors,
            "REQUIRED",
            path + ".sides",
            QString("pins %1 are on no side").arg(missing.join(", ")));
        valid = false;
    }
    if (plan->padCells.isEmpty()) {
        appendError(errors, "CONFLICT", path, "needs pad_cells, the ring is the pad shell");
        valid = false;
    }
    /* A cell the library describes must be used as the library says. */
    const auto expectKind = [&](const QString &cell, const char *kind, const QString &where) {
        if (plan->ioLib.contains(cell) && plan->ioLib.value(cell).kind != kind) {
            appendError(
                errors,
                "KIND",
                where,
                QString("%1 is declared %2 in io_lib, not %3")
                    .arg(cell, plan->ioLib.value(cell).kind, kind));
            valid = false;
        }
    };
    if (!ring.corner.isEmpty()) {
        expectKind(ring.corner, "corner", path + ".corner");
    }
    for (auto it = ring.power.cbegin(); it != ring.power.cend(); ++it) {
        expectKind(it.value(), "power", path + ".power." + it.key());
    }
    /* Wrapper nets of the direct cells are declared once each. */
    QSet<QString> nets;
    for (const QSocIoRingDirect &direct : ring.direct) {
        for (auto it = direct.port.cbegin(); it != direct.port.cend(); ++it) {
            if (it.value().startsWith("1'b")) {
                continue;
            }
            if (nets.contains(it.value())) {
                appendError(
                    errors,
                    "CONFLICT",
                    path + ".direct." + direct.key + ".port." + it.key(),
                    QString("net %1 is already used by another direct port").arg(it.value()));
                valid = false;
            }
            nets.insert(it.value());
        }
    }
    return valid;
}

/**
 * @param openDrain receives `open_drain`, or null for a role that cannot carry it
 */
bool parseEndpoint(
    const YAML::Node      &node,
    const QString         &path,
    bool                   allowConstant,
    QSocIomuxEndpointPlan *endpoint,
    bool                  *openDrain,
    QStringList           *errors)
{
    if (node.IsMap()) {
        if (!validateMap(node, kEndpointKeys, path, errors)) {
            return false;
        }
        bool valid = true;
        if (node["open_drain"]) {
            if (openDrain == nullptr) {
                appendError(errors, "ROLE", path + ".open_drain", "applies to output_value only");
                valid = false;
            } else {
                valid = parseStrictBool(node["open_drain"], path + ".open_drain", openDrain, errors)
                        && valid;
            }
        }
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

/**
 * @brief Read the net of a selected pair: `link`, optional `bit`, optional `invert`.
 */
bool parseSelectLink(
    const YAML::Node &node, const QString &path, QSocIomuxEndpointPlan *link, QStringList *errors)
{
    bool valid = parseIdentifier(node["link"], path + ".link", &link->link, errors);
    if (node["bit"]) {
        quint64 bit = 0;
        if (parseStrictUnsigned(
                node["bit"], path + ".bit", 0, std::numeric_limits<quint32>::max(), &bit, errors)) {
            link->bit = static_cast<quint32>(bit);
        } else {
            valid = false;
        }
    }
    if (node["invert"]) {
        valid = parseStrictBool(node["invert"], path + ".invert", &link->invert, errors) && valid;
    }
    return valid;
}

/**
 * @brief Read a pull request: a mode name, or a map of `mode` and `strength`.
 */
bool parsePullRequest(
    const YAML::Node &node, const QString &path, QSocIomuxPullRequest *request, QStringList *errors)
{
    if (!node) {
        appendError(errors, "REQUIRED", path, "property is required");
        return false;
    }
    if (node.IsScalar()) {
        return parseLabel(node, path, &request->mode, errors);
    }
    if (!node.IsMap()) {
        appendError(errors, "TYPE", path, "must be a mode name or a map");
        return false;
    }
    bool valid = true;
    if (!node["mode"]) {
        appendError(errors, "REQUIRED", path + ".mode", "property is required");
        valid = false;
    } else {
        valid = parseLabel(node["mode"], path + ".mode", &request->mode, errors) && valid;
    }
    if (node["strength"]) {
        valid = parseLabel(node["strength"], path + ".strength", &request->strength, errors)
                && valid;
    }
    return valid;
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

    int  declaredRoles = 0;
    bool openDrain     = false;
    for (const QSocIomuxRole role : kRoles) {
        const QString    key      = roleKey(role);
        const YAML::Node roleNode = node[key.toStdString()];
        if (!roleNode) {
            continue;
        }
        ++declaredRoles;
        const bool allowConstant = role != QSocIomuxRole::InputValue;
        valid                    = parseEndpoint(
                                       roleNode,
                                       path + "." + key,
                                       allowConstant,
                                       &routeRole(*route, role),
                                       role == QSocIomuxRole::OutputValue ? &openDrain : nullptr,
                                       errors)
                                   && valid;
    }
    if (openDrain) {
        /* Open drain is a wiring, not a new role: the value is a constant zero
         * and the enable follows the inverted link. */
        if (node["output_enable"]) {
            appendError(
                errors,
                "ROLE",
                path + ".output_enable",
                "is derived from an open_drain output_value, drop it");
            valid = false;
        }
        if (route->outputValue.link.isEmpty()) {
            appendError(errors, "ROLE", path + ".output_value", "open_drain needs a link");
            valid = false;
        }
        if (valid) {
            route->outputEnable         = route->outputValue;
            route->outputEnable.invert  = !route->outputValue.invert;
            route->outputValue          = QSocIomuxEndpointPlan();
            route->outputValue.constant = 0;
        }
    }
    if (node["pull"]) {
        const YAML::Node pull     = node["pull"];
        const QString    pullPath = path + ".pull";
        if (pull.IsScalar()) {
            valid = parseLabel(pull, pullPath, &route->pullMode, errors) && valid;
        } else if (pull.IsMap() && validateMap(pull, kRoutePullKeys, pullPath, errors)) {
            if (pull["link"]) {
                valid = parseSelectLink(pull, pullPath, &route->pullSelect.link, errors) && valid;
                if (pull["mode"] || pull["strength"]) {
                    appendError(errors, "ROLE", pullPath, "a linked pull names on and off, not mode");
                    valid = false;
                }
                valid = parsePullRequest(pull["on"], pullPath + ".on", &route->pullSelect.on, errors)
                        && valid;
                valid
                    = parsePullRequest(pull["off"], pullPath + ".off", &route->pullSelect.off, errors)
                      && valid;
                if (valid && route->pullSelect.on == route->pullSelect.off) {
                    appendError(errors, "ROLE", pullPath, "on and off name the same row");
                    valid = false;
                }
            } else {
                QSocIomuxPullRequest request;
                valid               = parsePullRequest(pull, pullPath, &request, errors) && valid;
                route->pullMode     = request.mode;
                route->pullStrength = request.strength;
            }
        } else {
            appendError(errors, "TYPE", pullPath, "must be a mode name or a map");
            valid = false;
        }
        ++declaredRoles;
    }
    if (node["control"]) {
        const YAML::Node controls = node["control"];
        if (!controls.IsMap()) {
            appendError(errors, "TYPE", path + ".control", "must be a map of control names to rows");
            valid = false;
        } else {
            for (const auto &entry : controls) {
                if (!entry.first.IsScalar()) {
                    appendError(errors, "TYPE", path + ".control", "control names must be scalar");
                    valid = false;
                    continue;
                }
                const QString           name     = QString::fromStdString(entry.first.Scalar());
                const QString           itemPath = path + ".control." + name;
                QSocIomuxControlRequest request;
                if (route->control.contains(name)) {
                    appendError(errors, "DUPLICATE", itemPath, "control is listed twice");
                    valid = false;
                    continue;
                }
                if (entry.second.IsMap()) {
                    if (!validateMap(entry.second, kRouteSelectKeys, itemPath, errors)) {
                        valid = false;
                        continue;
                    }
                    valid = parseSelectLink(entry.second, itemPath, &request.select.link, errors)
                            && valid;
                    for (const auto &[key, target] :
                         {std::pair{"on", &request.select.on.mode},
                          std::pair{"off", &request.select.off.mode}}) {
                        if (!entry.second[key]) {
                            appendError(
                                errors, "REQUIRED", itemPath + "." + key, "property is required");
                            valid = false;
                        } else {
                            valid
                                = parseLabel(entry.second[key], itemPath + "." + key, target, errors)
                                  && valid;
                        }
                    }
                    if (valid && request.select.on == request.select.off) {
                        appendError(errors, "ROLE", itemPath, "on and off name the same row");
                        valid = false;
                    }
                } else if (!parseLabel(entry.second, itemPath, &request.row, errors)) {
                    valid = false;
                }
                route->control.insert(name, request);
            }
            if (!route->control.isEmpty()) {
                ++declaredRoles;
            }
        }
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

quint32 bankWordCount(const QSocIomuxPlan &plan)
{
    const quint32 dataWidth = plan.mmio.dataWidth;
    return (plan.pinCount + dataWidth - 1) / dataWidth;
}

/**
 * @brief Append one banked family: one bit per pin, `data_width` pins per word.
 *
 * One store flips the same bit on a whole word of pins. Register `family_N`
 * holds field `pin_P_family`, which drives `pin_P_family_o` when it has
 * storage and reads `pin_P_inputSuffix_i` when an input suffix is given.
 */
void appendBank(
    QSocIomuxPlan *plan,
    const QString &family,
    QSocMmioAccess access,
    const QString &inputSuffix,
    quint64       *offset)
{
    const quint32 dataWidth = plan->mmio.dataWidth;
    const quint32 byteCount = dataWidth / 8;
    for (quint32 word = 0; word < bankWordCount(*plan); ++word) {
        QSocMmioRegisterPlan bank;
        bank.name       = QString("%1_%2").arg(family).arg(word);
        bank.byteOffset = *offset;
        *offset += byteCount;
        const quint32 first = word * dataWidth;
        const quint32 last  = std::min(plan->pinCount, first + dataWidth);
        for (quint32 pin = first; pin < last; ++pin) {
            QSocMmioFieldPlan field;
            field.name   = QString("pin_%1_%2").arg(pin).arg(family);
            field.lsb    = pin - first;
            field.width  = 1;
            field.access = access;
            if (qsocMmioHasStorage(access)) {
                field.resetValue = 0;
                field.outputPort = QString("pin_%1_%2_o").arg(pin).arg(family);
            }
            if (!inputSuffix.isEmpty()) {
                field.inputPort = QString("pin_%1_%2_i").arg(pin).arg(inputSuffix);
            }
            bank.fields.append(field);
        }
        plan->mmio.registers.append(bank);
    }
}

struct WordField
{
    QString name;
    quint32 lsb;
    quint32 width;
};

/**
 * @brief Append one word per pin holding the given fields.
 *
 * One store reconfigures a pin without a read-modify-write and without
 * touching its neighbours. Field `name` of pin P drives `pin_P_name_o`.
 */
void appendPinWords(
    QSocIomuxPlan *plan, const QString &prefix, const QList<WordField> &fields, quint64 offset)
{
    const quint32 byteCount = plan->mmio.dataWidth / 8;
    for (quint32 pin = 0; pin < plan->pinCount; ++pin) {
        QSocMmioRegisterPlan word;
        word.name       = QString("%1_%2").arg(prefix).arg(pin);
        word.byteOffset = offset;
        offset += byteCount;
        for (const WordField &item : fields) {
            QSocMmioFieldPlan field;
            field.name       = item.name;
            field.lsb        = item.lsb;
            field.width      = item.width;
            field.access     = QSocMmioAccess::ReadWrite;
            field.resetValue = 0;
            field.outputPort = QString("pin_%1_%2_o").arg(pin).arg(item.name);
            word.fields.append(field);
        }
        plan->mmio.registers.append(word);
    }
}

void composeGpio(QSocIomuxPlan *plan)
{
    quint64 offset = QSocIomuxGenerator::kBaseGpio;
    appendBank(plan, "input_value", QSocMmioAccess::ReadOnly, "input_value", &offset);
    appendBank(plan, "input_enable", QSocMmioAccess::ReadWrite, QString(), &offset);
    appendBank(plan, "output_value", QSocMmioAccess::ReadWrite, QString(), &offset);
    appendBank(plan, "output_enable", QSocMmioAccess::ReadWrite, QString(), &offset);
}

/**
 * @brief Append the per-pin source control word.
 *
 * Bit positions are fixed whatever options are on, so software written for
 * one design reads the same word on another: gpio owns bits 0 to 5, pad
 * control bit 6, the receive override one bit per slot from bit 8, and each
 * selectable control one bit from bit 16 in declaration order.
 * A field whose option is off is absent and reads zero.
 */
void composeSourceControl(QSocIomuxPlan *plan)
{
    QList<WordField> fields;
    if (plan->option.gpio) {
        fields.append({"input_enable_src", 0, 1});
        fields.append({"output_value_src", 2, 2});
        fields.append({"output_enable_src", 4, 2});
    }
    if (plan->option.padControl) {
        const QSocPadModel &model = plan->padModel;
        if (model.hasPull()) {
            fields.append({"pull_src", 6, 1});
        }
        /* One source bit per declared control from bit 16, in declaration
         * order, so a control keeps its bit when another one gains rows. */
        for (qsizetype index = 0; index < model.control.size(); ++index) {
            if (model.control.at(index).width > 0) {
                fields.append({model.control.at(index).name + "_src", quint32(16 + index), 1});
            }
        }
    }
    if (plan->option.rxOverride) {
        for (quint32 slot = 0; slot < plan->hsSlots; ++slot) {
            fields.append({QString("rx_src_s%1").arg(slot), 8 + slot, 1});
        }
    }
    appendPinWords(plan, "pin_src_ctrl", fields, QSocIomuxGenerator::kBaseSourceControl);
}

/**
 * @brief Append the per-pin pad control word.
 *
 * The pull mode sits at bit 0, the up and down strength selects at bits 4
 * and 8, each only when the cell has it, and the first four controls take
 * 4-bit lanes from bit 16 in declaration order. Controls past the fourth
 * take `pin_ctl_k` words, eight lanes each. A single-row control keeps its
 * lane empty so its neighbours never move.
 */
void composePadControl(QSocIomuxPlan *plan)
{
    const QSocPadModel &model = plan->padModel;
    QList<WordField>    fields;
    if (model.hasPull()) {
        fields.append({"pull_mode", 0, model.modeWidth});
        if (model.upSelWidth > 0) {
            fields.append({"up_sel", 4, model.upSelWidth});
        }
        if (model.downSelWidth > 0) {
            fields.append({"down_sel", 8, model.downSelWidth});
        }
    }
    const auto lanes = [&](qsizetype first, qsizetype count, quint32 lsb) {
        QList<WordField> result;
        for (qsizetype index = first; index < std::min(model.control.size(), first + count);
             ++index) {
            const QSocPadModel::Control &item = model.control.at(index);
            if (item.width > 0) {
                result.append({item.name, lsb + 4 * quint32(index - first), item.width});
            }
        }
        return result;
    };
    fields.append(lanes(0, kPadWordLanes, 16));
    /* Empty when the cell has no pull and its first controls are single-row;
     * cppcheck does not see that append of an empty list adds nothing. */
    // cppcheck-suppress knownConditionTrueFalse
    if (!fields.isEmpty()) {
        appendPinWords(plan, "pin_pad_ctrl", fields, QSocIomuxGenerator::kBasePadControl);
    }
    for (qsizetype word = 0; kPadWordLanes + word * kCtlWordLanes < model.control.size(); ++word) {
        const QList<WordField> extra = lanes(kPadWordLanes + word * kCtlWordLanes, kCtlWordLanes, 0);
        if (!extra.isEmpty()) {
            appendPinWords(
                plan,
                QString("pin_ctl_%1").arg(word),
                extra,
                QSocIomuxGenerator::kBaseControlWords
                    + quint64(word) * QSocIomuxGenerator::kControlWordStride);
        }
    }
}

/**
 * @brief Append the inversion banks: the three roles, the receive slots, then
 * the select net of the pull and of every selectable control.
 */
void composeInvert(QSocIomuxPlan *plan)
{
    quint64 offset = QSocIomuxGenerator::kBaseInvert;
    appendBank(plan, "input_enable_inv", QSocMmioAccess::ReadWrite, QString(), &offset);
    appendBank(plan, "output_value_inv", QSocMmioAccess::ReadWrite, QString(), &offset);
    appendBank(plan, "output_enable_inv", QSocMmioAccess::ReadWrite, QString(), &offset);
    for (quint32 slot = 0; slot < plan->hsSlots; ++slot) {
        appendBank(
            plan, QString("rx_inv_s%1").arg(slot), QSocMmioAccess::ReadWrite, QString(), &offset);
    }
    const QSocPadModel &model = plan->padModel;
    if (model.hasPull()) {
        appendBank(plan, "pull_inv", QSocMmioAccess::ReadWrite, QString(), &offset);
    }
    for (const QSocPadModel::Control &item : model.control) {
        if (item.width > 0) {
            appendBank(plan, item.name + "_inv", QSocMmioAccess::ReadWrite, QString(), &offset);
        }
    }
}

void composeRxOverride(QSocIomuxPlan *plan)
{
    quint64 offset = QSocIomuxGenerator::kBaseRxOverride;
    for (quint32 slot = 0; slot < plan->hsSlots; ++slot) {
        appendBank(
            plan, QString("rx_value_s%1").arg(slot), QSocMmioAccess::ReadWrite, QString(), &offset);
    }
}

/**
 * @brief Append the interrupt enable and pending banks.
 *
 * Pending records the event whether or not the enable is set, so a design can
 * poll a pin it never wired to an interrupt line. The enable gates the output
 * only.
 */
void composeInterrupt(QSocIomuxPlan *plan)
{
    const char *kinds[] = {"high", "low", "rise", "fall"};
    quint64     offset  = QSocIomuxGenerator::kBaseInterrupt;
    for (const char *kind : kinds) {
        appendBank(
            plan, QString("%1_int_en").arg(kind), QSocMmioAccess::ReadWrite, QString(), &offset);
    }
    for (const char *kind : kinds) {
        appendBank(
            plan,
            QString("%1_int_pend").arg(kind),
            QSocMmioAccess::WriteOneClear,
            QString("%1_detect").arg(kind),
            &offset);
    }
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
 * @brief Drive the pull pins of one pin from its mode and strength selects.
 *
 * Every mode value with a row behind it is listed; anything else, including a
 * value the cell has no row for, resolves to the none row.
 */
void appendPadPullPorts(
    QStringList           *lines,
    quint32                pin,
    const QSocPadEncoding &encoding,
    const QList<QString>  &port,
    const QString         &mode,
    const QString         &upSel,
    const QString         &downSel)
{
    struct Arm
    {
        QString                condition;
        const QSocPadTableRow *row;
    };
    QList<Arm>    arms;
    const QString modeIs = QString("%1 == %2'd").arg(mode).arg(QSocIomuxGenerator::kPadLane);
    const auto    graded =
        [&](int modeCode, const QList<QSocPadTableRow> &rows, const QString &sel, quint32 selWidth) {
            for (qsizetype index = 0; index < rows.size(); ++index) {
                QString condition = modeIs + QString::number(modeCode);
                if (selWidth > 0) {
                    condition += QString(" && %1 == %2'd%3")
                                     .arg(sel)
                                     .arg(QSocIomuxGenerator::kPadLane)
                                     .arg(index);
                }
                arms.append({condition, &rows.at(index)});
            }
        };
    graded(QSocPadEncoding::Up, encoding.upRows, upSel, encoding.upSelWidth);
    graded(QSocPadEncoding::Down, encoding.downRows, downSel, encoding.downSelWidth);
    if (encoding.keeperRow) {
        arms.append({modeIs + QString::number(int(QSocPadEncoding::Keeper)), &*encoding.keeperRow});
    }
    if (encoding.oscillatorRow) {
        arms.append(
            {modeIs + QString::number(int(QSocPadEncoding::Oscillator)), &*encoding.oscillatorRow});
    }
    for (qsizetype index = 0; index < encoding.namedRow.size(); ++index) {
        if (encoding.namedRow.at(index)) {
            arms.append(
                {modeIs + QString::number(int(QSocPadEncoding::FirstNamed) + int(index)),
                 &*encoding.namedRow.at(index)});
        }
    }
    /* A table entry of x means the pin does not matter in that row, so drive
     * zero and keep the netlist two valued. */
    const auto bit = [](const QSocPadTableRow &row, qsizetype index) {
        return row.value.at(index) == "1" ? QStringLiteral("1'b1") : QStringLiteral("1'b0");
    };
    for (qsizetype index = 0; index < port.size(); ++index) {
        QString expression;
        for (const Arm &arm : arms) {
            expression += QString("(%1) ? %2 : ").arg(arm.condition, bit(*arm.row, index));
        }
        expression += bit(encoding.noneRow, index);
        lines->append(QString("wire %1_%2_w = %3;").arg(port.at(index)).arg(pin).arg(expression));
    }
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
    const QList<QSocPadTableRow> &rows,
    qsizetype                     defaultRow = 0)
{
    const quint32 width = QSocIomuxGenerator::kPadLane;
    /* A table entry of x means the pin does not matter in that row, so drive
     * zero and keep the netlist two valued. */
    const auto bit = [&](qsizetype row, qsizetype index) {
        return rows.at(row).value.at(index) == "1" ? QStringLiteral("1'b1")
                                                   : QStringLiteral("1'b0");
    };
    for (qsizetype index = 0; index < port.size(); ++index) {
        QString expression;
        for (qsizetype row = 0; row < rows.size(); ++row) {
            if (row == defaultRow) {
                continue;
            }
            expression += QString("(%1 == %2'd%3) ? %4 : ")
                              .arg(selector)
                              .arg(width)
                              .arg(row)
                              .arg(bit(row, index));
        }
        /* The default row closes the chain, so a code with no row, which only
         * a register can produce, lands on it. */
        expression += bit(defaultRow, index);
        lines->append(QString("wire %1_%2_w = %3;").arg(port.at(index)).arg(pin).arg(expression));
    }
}

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
    if (cell.pull.port.contains(port) || cell.controlDrives(port)) {
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
 * A body over pull and control pins alone speaks about logic this generator
 * emits, so it is a claim to prove. A body that reaches a role pin speaks
 * about what routes and registers will do, so it bounds the environment.
 */
bool validatePadConstraints(QSocPadCellPlan &cell, QStringList *errors)
{
    bool valid = true;
    for (qsizetype index = 0; index < cell.constraint.size(); ++index) {
        QSocPadConstraint &item = cell.constraint[index];
        const QString      path = QString("%1.constraint[%2]").arg(cell.path).arg(index);
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
                            && (cell.pull.port.contains(port) || cell.controlDrives(port));
            }
            item.assume = !onlyTable;
        }
    }
    return valid;
}

/**
 * @brief Refuse an option whose prerequisites the source does not meet.
 */
bool validateOptions(const QSocIomuxPlan &plan, QStringList *errors)
{
    if (!plan.option.padControl) {
        return true;
    }
    if (!plan.hasPadCell()) {
        appendError(errors, "OPTION", "generator.option.pad_control", "needs a pad_cell declaration");
        return false;
    }
    if (!plan.padModel.selectable()) {
        appendError(
            errors,
            "OPTION",
            "generator.option.pad_control",
            QString("pad cell %1 has no pull table and no control with more than one row")
                .arg(
                    plan.padCells.size() == 1 ? plan.padCells.first().cell
                                              : QStringLiteral("pad_cells")));
        return false;
    }
    return true;
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
    if (!plan.hasPadCell()) {
        bool valid = true;
        for (const QSocIomuxRoutePlan &route : plan.routes) {
            if (route.pullMode.isEmpty() && !route.pullSelect.linked() && route.control.isEmpty()) {
                continue;
            }
            appendError(
                errors,
                "CAPABILITY",
                QString("generator.route.pin %1 slot %2").arg(route.pin).arg(route.slot),
                "pull and control need a pad_cell declaration");
            valid = false;
        }
        return valid;
    }
    bool valid = true;
    for (qsizetype classIndex = 0; classIndex < plan.padCells.size(); ++classIndex) {
        const QSocPadCellPlan &cell     = plan.padCells.at(classIndex);
        const QSocPadEncoding  encoding = QSocIomuxGenerator::padEncoding(cell, plan.padModel);
        const auto ownsPin = [&](quint32 pin) { return plan.pinClass.at(pin) == classIndex; };
        if (plan.padModel.safe && !cell.safe.declared) {
            appendError(
                errors,
                "REQUIRED",
                cell.path + ".safe",
                "every class declares safe once one does, so pad_force_i holds every pin");
            valid = false;
        }
        /* Codes are 4-bit fields of the pad control word. */
        if (encoding.upRows.size() > kMaximumRows || encoding.downRows.size() > kMaximumRows
            || QSocPadEncoding::FirstNamed + encoding.namedMode.size() > kMaximumRows) {
            appendError(
                errors,
                "RANGE",
                cell.path + ".pull.table",
                QString("at most %1 strength rows per direction and %2 named modes")
                    .arg(kMaximumRows)
                    .arg(kMaximumRows - QSocPadEncoding::FirstNamed));
            valid = false;
        }
        /* Every cell pin has one driver: a role, the pull group, or one control. */
        {
            QMap<QString, int> named;
            for (const QString &port :
                 {cell.portPad,
                  cell.portInputValue,
                  cell.portInputEnable,
                  cell.portOutputValue,
                  cell.portOutputEnable}) {
                if (!port.isEmpty()) {
                    ++named[port];
                }
            }
            for (const QString &port : cell.pull.port) {
                ++named[port];
            }
            for (const QSocPadControlPlan &item : cell.control) {
                for (const QString &port : item.port) {
                    ++named[port];
                }
            }
            for (auto it = named.cbegin(); it != named.cend(); ++it) {
                if (it.value() > 1) {
                    appendError(
                        errors,
                        "CAPABILITY",
                        cell.path,
                        QString("pin %1 of %2 is named %3 times, once is the limit")
                            .arg(it.key(), cell.cell)
                            .arg(it.value()));
                    valid = false;
                }
            }
        }
        /* Strength is a property of a direction. Every other mode is one row. */
        for (auto it = cell.pull.mode.cbegin(); it != cell.pull.mode.cend(); ++it) {
            if (it.key() != QStringLiteral("up") && it.key() != QStringLiteral("down")
                && it.value().size() > 1) {
                appendError(
                    errors,
                    "CAPABILITY",
                    QString("%1.pull.table.%2").arg(cell.path, it.key()),
                    "only up and down carry strength rows");
                valid = false;
            }
        }
        const auto checkPull = [&](const QSocIomuxPullRequest &request, const QString &pullPath) {
            const int mode = encoding.modeCode(request.mode);
            if (mode < 0) {
                const bool feedback = request.mode == QStringLiteral("keeper")
                                      || request.mode == QStringLiteral("oscillator");
                QString    reason
                    = QString("pad cell %1 has no pull mode %2").arg(cell.cell, request.mode);
                if (feedback && cell.pull.isDriver) {
                    reason = QString("pad cell %1 pulls with its driver, so %2 cannot be woven")
                                 .arg(cell.cell, request.mode);
                } else if (feedback && (encoding.keeperRow || encoding.oscillatorRow)) {
                    reason = QString(
                                 "pad cell %1 names its own keeper or oscillator row, so %2 "
                                 "is not woven")
                                 .arg(cell.cell, request.mode);
                } else if (feedback) {
                    reason = QString("pad cell %1 needs both up and down rows to weave %2")
                                 .arg(cell.cell, request.mode);
                }
                appendError(errors, "CAPABILITY", pullPath + ".mode", reason);
                return false;
            }
            const bool woven = encoding.weaves
                               && (mode == QSocPadEncoding::Keeper
                                   || mode == QSocPadEncoding::Oscillator);
            const bool up    = mode == QSocPadEncoding::Up;
            const bool down  = mode == QSocPadEncoding::Down;
            const bool graded = (up && encoding.upSelWidth > 0)
                                || (down && encoding.downSelWidth > 0)
                                || (woven && (encoding.upSelWidth > 0 || encoding.downSelWidth > 0));
            if (!graded && !request.strength.isEmpty()) {
                appendError(
                    errors,
                    "CAPABILITY",
                    pullPath + ".strength",
                    QString("pull mode %1 of %2 has a single row, drop strength")
                        .arg(request.mode, cell.cell));
                return false;
            }
            if ((up || down) && graded && !request.strength.isEmpty()) {
                const int sel = up ? encoding.upSel(request.strength)
                                   : encoding.downSel(request.strength);
                if (sel < 0) {
                    appendError(
                        errors,
                        "CAPABILITY",
                        pullPath + ".strength",
                        QString("pull mode %1 of %2 has no strength %3")
                            .arg(request.mode, cell.cell, request.strength));
                    return false;
                }
            } else if ((up || down) && graded) {
                appendError(
                    errors,
                    "CAPABILITY",
                    pullPath + ".strength",
                    QString("pull mode %1 of %2 has several rows, name one")
                        .arg(request.mode, cell.cell));
                return false;
            } else if (woven && !request.strength.isEmpty()) {
                /* A woven mode alternates between the two directions, so the
             * strength must exist on whichever side is graded. */
                const bool upOk = encoding.upSelWidth == 0 || encoding.upSel(request.strength) >= 0;
                const bool downOk = encoding.downSelWidth == 0
                                    || encoding.downSel(request.strength) >= 0;
                if (!upOk || !downOk) {
                    appendError(
                        errors,
                        "CAPABILITY",
                        pullPath + ".strength",
                        QString("woven %1 of %2 needs strength %3 in both up and down")
                            .arg(request.mode, cell.cell, request.strength));
                    return false;
                }
            }
            return true;
        };
        if (cell.safe.declared) {
            if (!cell.safe.pull.empty() && !checkPull(cell.safe.pull, cell.path + ".safe.pull")) {
                valid = false;
            }
            for (auto it = cell.safe.control.cbegin(); it != cell.safe.control.cend(); ++it) {
                const qsizetype index = encoding.controlIndex(it.key());
                if (index < 0 || encoding.control.at(index).cellIndex < 0) {
                    appendError(
                        errors,
                        "CAPABILITY",
                        cell.path + ".safe." + it.key(),
                        QString("pad cell %1 has no control %2").arg(cell.cell, it.key()));
                    valid = false;
                } else if (encoding.controlCode(index, it.value()) < 0) {
                    appendError(
                        errors,
                        "CAPABILITY",
                        cell.path + ".safe." + it.key(),
                        QString("control %1 of %2 has no row %3")
                            .arg(it.key(), cell.cell, it.value()));
                    valid = false;
                }
            }
            const std::pair<const char *, bool> forced[]
                = {{"input_enable", cell.safe.inputEnable != 0 && cell.portInputEnable.isEmpty()},
                   {"output_value", cell.safe.outputValue != 0 && cell.portOutputValue.isEmpty()},
                   {"output_enable",
                    cell.safe.outputEnable != 0 && cell.portOutputEnable.isEmpty()}};
            for (const auto &[role, missing] : forced) {
                if (missing) {
                    appendError(
                        errors,
                        "CAPABILITY",
                        QString("%1.safe.%2").arg(cell.path, role),
                        QString("pad cell %1 declares no port for this role").arg(cell.cell));
                    valid = false;
                }
            }
        }
        for (const QSocIomuxRoutePlan &route : plan.routes) {
            if (!ownsPin(route.pin)) {
                continue;
            }
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
                if (!checkPull({route.pullMode, route.pullStrength}, routePath + ".pull")) {
                    valid = false;
                    continue;
                }
            } else if (route.pullSelect.linked()) {
                const bool onOk  = checkPull(route.pullSelect.on, routePath + ".pull.on");
                const bool offOk = checkPull(route.pullSelect.off, routePath + ".pull.off");
                if (!onOk || !offOk) {
                    valid = false;
                    continue;
                }
            }
            for (auto it = route.control.cbegin(); it != route.control.cend(); ++it) {
                const qsizetype index    = encoding.controlIndex(it.key());
                const QString   itemPath = routePath + ".control." + it.key();
                if (index < 0 || encoding.control.at(index).cellIndex < 0) {
                    appendError(
                        errors,
                        "CAPABILITY",
                        itemPath,
                        QString("pad cell %1 has no control %2").arg(cell.cell, it.key()));
                    valid = false;
                    continue;
                }
                if (it.value().select.linked() && encoding.control.at(index).width == 0) {
                    appendError(
                        errors,
                        "CAPABILITY",
                        itemPath,
                        QString("control %1 of %2 has one row, nothing for a link to select")
                            .arg(it.key(), cell.cell));
                    valid = false;
                    continue;
                }
                const QList<std::pair<QString, QString>> rows
                = it.value().select.linked()
                      ? QList<std::pair<QString, QString>>{{".on", it.value().select.on.mode},
                                                           {".off", it.value().select.off.mode}}
                      : QList<std::pair<QString, QString>>{{QString(), it.value().row}};
                for (const auto &[suffix, label] : rows) {
                    if (encoding.controlCode(index, label) < 0) {
                        appendError(
                            errors,
                            "CAPABILITY",
                            itemPath + suffix,
                            QString("control %1 of %2 has no row %3")
                                .arg(it.key(), cell.cell, label));
                        valid = false;
                    }
                }
            }
        }

        /* The cell gates its receiver with the input enable, so a pin whose sinks
     * listen while no slot ever raises that enable reads zero forever. The gpio
     * register path can raise it at run time and the inversion bank can flip
     * the constant, which are the two ways out. */
        if (!cell.portInputEnable.isEmpty() && !plan.option.gpio && !plan.option.invert) {
            QSet<quint32> listening;
            QSet<quint32> enabling;
            for (const QSocIomuxRoutePlan &route : plan.routes) {
                if (!ownsPin(route.pin)) {
                    continue;
                }
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
    }
    return valid;
}

QSocMmioFieldPlan constantField(const QString &name, quint32 lsb, quint32 width, quint64 value)
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
 * @brief Registers the identity words occupy: two 64-bit beats or four 32-bit.
 */
qsizetype identityRegisterCount(quint32 dataWidth)
{
    return dataWidth == 64 ? 2 : 4;
}

/**
 * @brief Append the identity words at byte offsets 0x0 to 0xc.
 *
 * The byte map is the same for both data widths: version at 0x0, type at
 * 0x4, capability at 0x8, feature at 0xc. A 64-bit instance packs each pair
 * into one beat. Software that knows the layout of one instance therefore
 * finds the same bytes on every other.
 */
void composeIdentity(QSocIomuxPlan *plan)
{
    const QSocIomuxLayoutVersion layout = QSocIomuxGenerator::layoutVersion();
    const bool                   wide   = plan->mmio.dataWidth == 64;

    QSocMmioRegisterPlan version;
    version.name       = QStringLiteral("version");
    version.byteOffset = 0;
    version.fields.append(constantField(QStringLiteral("major"), 24, 8, layout.major));
    version.fields.append(constantField(QStringLiteral("minor"), 16, 8, layout.minor));
    version.fields.append(constantField(QStringLiteral("patch"), 8, 8, layout.patch));
    version.fields.append(constantField(QStringLiteral("build"), 0, 8, plan->build));

    QSocMmioRegisterPlan type;
    type.name       = QStringLiteral("type");
    type.byteOffset = 4;
    QSocMmioFieldPlan typeId
        = constantField(QStringLiteral("type_id"), 0, 32, QSocIomuxGenerator::kTypeId);

    QSocMmioRegisterPlan capability;
    capability.name       = QStringLiteral("capability");
    capability.byteOffset = 8;
    capability.fields.append(constantField(QStringLiteral("pin_count"), 0, 16, plan->pinCount));
    capability.fields.append(constantField(QStringLiteral("hs_slots"), 16, 8, plan->hsSlots));

    QSocMmioRegisterPlan feature;
    feature.name       = QStringLiteral("feature");
    feature.byteOffset = 12;
    const std::pair<const char *, bool> flags[]
        = {{"gpio", plan->option.gpio},
           {"interrupt", plan->option.interrupt},
           {"pad_control", plan->option.padControl},
           {"invert", plan->option.invert},
           {"rx_override", plan->option.rxOverride}};
    QList<QSocMmioFieldPlan> featureFields;
    for (qsizetype index = 0; index < qsizetype(std::size(flags)); ++index) {
        featureFields.append(
            constantField(QString(flags[index].first), quint32(index), 1, flags[index].second));
    }

    if (wide) {
        typeId.lsb = 32;
        version.fields.append(typeId);
        for (QSocMmioFieldPlan field : featureFields) {
            field.lsb += 32;
            capability.fields.append(field);
        }
        plan->mmio.registers.append(version);
        plan->mmio.registers.append(capability);
        return;
    }
    type.fields.append(typeId);
    feature.fields = featureFields;
    plan->mmio.registers.append(version);
    plan->mmio.registers.append(type);
    plan->mmio.registers.append(capability);
    plan->mmio.registers.append(feature);
}

bool composeMmio(QSocIomuxPlan *plan, QStringList *errors)
{
    const quint32 dataWidth = plan->mmio.dataWidth;
    const quint32 byteCount = dataWidth / 8;
    const quint32 lanes     = pinsPerWord(dataWidth);
    const quint32 words     = selectorWordCount(plan->pinCount, dataWidth);
    const quint32 width     = selectorWidth(plan->hsSlots);

    composeIdentity(plan);

    for (quint32 word = 0; word < words; ++word) {
        QSocMmioRegisterPlan selector;
        selector.name       = QString("hs_select_%1").arg(word);
        selector.byteOffset = QSocIomuxGenerator::kBaseSelector + quint64(word) * byteCount;
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

    /* Address order: the bit-per-pin banks below 0x1000, the per-pin words above. */
    if (plan->option.gpio) {
        composeGpio(plan);
    }
    if (plan->option.rxOverride) {
        composeRxOverride(plan);
    }
    if (plan->option.interrupt) {
        composeInterrupt(plan);
    }
    if (plan->option.invert) {
        composeInvert(plan);
        const QSocMmioRegisterPlan &last = plan->mmio.registers.constLast();
        if (last.byteOffset + byteCount
            > QSocIomuxGenerator::kBaseInvert + QSocIomuxGenerator::kInvertBytes) {
            appendError(
                errors,
                "RANGE",
                "generator.option.invert",
                QString("needs %1 banks, the invert region holds %2")
                    .arg(
                        (last.byteOffset + byteCount - QSocIomuxGenerator::kBaseInvert)
                        / (byteCount * bankWordCount(*plan)))
                    .arg(QSocIomuxGenerator::kInvertBytes / (byteCount * bankWordCount(*plan))));
            return false;
        }
    }
    if (plan->option.sourceControl()) {
        composeSourceControl(plan);
    }
    if (plan->option.padControl) {
        composePadControl(plan);
    }

    const quint64 aperture = std::max(
        QSocIomuxGenerator::kApertureBytes, plan->mmio.registers.constLast().byteOffset + byteCount);
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

    if (generator["build"]) {
        quint64 build = 0;
        if (parseStrictUnsigned(generator["build"], "generator.build", 0, 255, &build, errors)) {
            plan->build = static_cast<quint32>(build);
        } else {
            valid = false;
        }
    }

    if (generator["option"]) {
        const YAML::Node option = generator["option"];
        if (!validateMap(option, kOptionKeys, "generator.option", errors)) {
            valid = false;
        } else {
            const std::pair<const char *, bool *> flags[]
                = {{"gpio", &plan->option.gpio},
                   {"interrupt", &plan->option.interrupt},
                   {"pad_control", &plan->option.padControl},
                   {"invert", &plan->option.invert},
                   {"rx_override", &plan->option.rxOverride}};
            for (const auto &[key, flag] : flags) {
                if (option[key]) {
                    valid = parseStrictBool(
                                option[key], QString("generator.option.%1").arg(key), flag, errors)
                            && valid;
                }
            }
        }
    }

    valid = parsePadClasses(generator, plan, errors) && valid;
    if (generator["io_lib"]) {
        valid = parseIoLib(generator["io_lib"], plan, errors) && valid;
    }
    if (generator["io_ring"]) {
        valid = parseIoRing(generator["io_ring"], plan, errors) && valid;
    }

    if (!generator["integration"]) {
        appendError(errors, "REQUIRED", "generator.integration", "property is required");
        valid = false;
    } else {
        valid = parseIntegration(
                    generator["integration"],
                    plan->hasPadCell(),
                    plan->padModel.safe,
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
    QString                      select; /**< "pull" or a control name for a row select net */

    bool isSelect() const { return !select.isEmpty(); }
};

QString endpointName(const EndpointPort &port)
{
    if (port.isSelect()) {
        return QSocIomuxGenerator::selectPortName(port.pin, port.slot, port.select);
    }
    return QSocIomuxGenerator::endpointPortName(port.pin, port.slot, port.role);
}

/**
 * @brief The select net of one slot as an expression, inverted when asked.
 */
QString selectNet(const EndpointPort &port)
{
    const QString name = endpointName(port);
    return port.endpoint->invert ? name + " ^ 1'b1" : name;
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
            ports.append({route.pin, route.slot, role, &route, &endpoint, QString()});
        }
        if (route.pullSelect.linked()) {
            ports.append(
                {route.pin,
                 route.slot,
                 QSocIomuxRole::InputEnable,
                 &route,
                 &route.pullSelect.link,
                 QStringLiteral("pull")});
        }
        for (auto it = route.control.cbegin(); it != route.control.cend(); ++it) {
            if (it.value().select.linked()) {
                ports.append(
                    {route.pin,
                     route.slot,
                     QSocIomuxRole::InputEnable,
                     &route,
                     &it.value().select.link,
                     it.key()});
            }
        }
    }
    return ports;
}

/**
 * @brief The select nets of one route, keyed by group name.
 */
QMap<QString, EndpointPort> selectPorts(const QSocIomuxPlan &plan, const QSocIomuxRoutePlan &route)
{
    QMap<QString, EndpointPort> selects;
    for (const EndpointPort &port : endpointPorts(plan)) {
        if (port.isSelect() && port.route == &route) {
            selects.insert(port.select, port);
        }
    }
    return selects;
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

/**
 * @brief The abstract pad bus: the four roles and every select lane.
 *
 * The wrapper drives it out, the shell takes it in. Its shape depends on the
 * pad model alone, never on which cell a pin instantiates.
 */
QList<QSocMmioPortDescription> padBusPorts(const QSocIomuxPlan &plan, bool wrapperSide)
{
    QList<QSocMmioPortDescription> ports;
    const auto                     out = [&](const QString &name, quint32 width) {
        ports.append(
            {name, wrapperSide ? QStringLiteral("output") : QStringLiteral("input"), width});
    };
    ports.append(
        {"pad_input_value_" + QString(wrapperSide ? "i" : "o"),
         wrapperSide ? QStringLiteral("input") : QStringLiteral("output"),
         plan.pinCount});
    out("pad_input_enable_" + QString(wrapperSide ? "o" : "i"), plan.pinCount);
    out("pad_output_value_" + QString(wrapperSide ? "o" : "i"), plan.pinCount);
    out("pad_output_enable_" + QString(wrapperSide ? "o" : "i"), plan.pinCount);
    for (const QSocIomuxCorePort &port : QSocIomuxGenerator::corePadSelectPorts(plan)) {
        out(port.name + (wrapperSide ? "_o" : "_i"), port.width);
    }
    return ports;
}

QList<QSocMmioPortDescription> publicPortDescriptions(const QSocIomuxPlan &plan)
{
    QList<QSocMmioPortDescription> ports;
    for (const QSocMmioPortDescription &port : QSocMmioGenerator::describePorts(plan.mmio)) {
        if (isControlPort(port)) {
            ports.append(port);
        }
    }
    ports.append(padBusPorts(plan, true));
    if (plan.padModel.safe) {
        ports.append({"pad_force_i", "input", 1});
    }
    if (plan.option.interrupt) {
        /* Endpoints stay last, because the wrapper aligns route comments by
         * counting back from the end of this list. */
        ports.append({"irq_o", "output", interruptLineCount(plan)});
    }
    for (const EndpointPort &endpoint : endpointPorts(plan)) {
        ports.append(
            {endpointName(endpoint),
             !endpoint.isSelect() && endpoint.role == QSocIomuxRole::InputValue
                 ? QStringLiteral("output")
                 : QStringLiteral("input"),
             1});
    }
    return ports;
}

/**
 * @brief The ports of the shell: the pad, the bus, and the direct nets.
 */
QList<QSocMmioPortDescription> shellPortDescriptions(const QSocIomuxPlan &plan)
{
    QList<QSocMmioPortDescription> ports;
    ports.append({"pad_io", "inout", plan.pinCount});
    ports.append(padBusPorts(plan, false));
    for (const QSocIoRingDirect &direct : plan.ioRing.direct) {
        for (auto it = direct.port.cbegin(); it != direct.port.cend(); ++it) {
            if (it.value().startsWith("1'b")) {
                continue;
            }
            const QString direction = direct.cellPorts.value(it.key());
            ports.append(
                {it.value(),
                 direction == "out"     ? QStringLiteral("output")
                 : direction == "inout" ? QStringLiteral("inout")
                                        : QStringLiteral("input"),
                 1});
        }
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

QString portDeclaration(const QSocIomuxCorePort &port, const QString &suffix)
{
    return port.width == 1
               ? QString("    input  wire        %1%2").arg(port.name, suffix)
               : QString("    input  wire [%1:0]  %2%3").arg(port.width - 1).arg(port.name, suffix);
}

/**
 * @brief `expression` with the inversion bit applied when the option is on.
 */
QString withInversion(const QSocIomuxPlan &plan, const QString &expression, const QString &invPort)
{
    if (!plan.option.invert) {
        return expression;
    }
    return QString("(%1) ^ %2").arg(expression, invPort);
}

/**
 * @brief One slot's code: a constant, or a pair chosen by its select net.
 *
 * An empty string means the slot asks for the default, which the chain
 * already supplies.
 */
QString slotCode(
    const QSocIomuxPlan      &plan,
    const QSocIomuxRoutePlan &route,
    const QString            &group,
    quint32                   codeWidth,
    int                       defaultCode,
    int                       fixed,
    int                       on,
    int                       off)
{
    const QMap<QString, EndpointPort> selects = selectPorts(plan, route);
    if (selects.contains(group)) {
        QString net = selectNet(selects.value(group));
        if (plan.option.invert) {
            net += QString(" ^ pin_%1_%2_inv_i").arg(route.pin).arg(group);
        }
        return QString("(%1 ? %2'd%3 : %2'd%4)").arg(net).arg(codeWidth).arg(on).arg(off);
    }
    return fixed == defaultCode ? QString() : QString("%1'd%2").arg(codeWidth).arg(fixed);
}

/**
 * @brief The selector chain of one group on one pin, closed by its default.
 *
 * A slot with no route, and any selector value above the slot count, lands
 * on the default: none for pull, the declared default row for a control.
 */
QString slotCodeChain(
    const QSocIomuxPlan                                      &plan,
    quint32                                                   pin,
    quint32                                                   codeWidth,
    int                                                       defaultCode,
    const std::function<QString(const QSocIomuxRoutePlan &)> &code)
{
    const quint32 width = selectorWidth(plan.hsSlots);
    QString       expression;
    for (const QSocIomuxRoutePlan &route : plan.routes) {
        const QString value = route.pin == pin ? code(route) : QString();
        if (value.isEmpty()) {
            continue;
        }
        expression += QString("(pin_%1_select_i == %2'd%3) ? %4 : ")
                          .arg(pin)
                          .arg(width)
                          .arg(route.slot)
                          .arg(value);
    }
    return expression + QString("%1'd%2").arg(codeWidth).arg(defaultCode);
}

} // namespace

bool QSocPadEncoding::supports(int mode) const
{
    switch (mode) {
    case None:
        return hasPull();
    case Up:
        return hasUp();
    case Down:
        return hasDown();
    case Keeper:
        return keeperRow.has_value() || weaves;
    case Oscillator:
        return oscillatorRow.has_value() || weaves;
    default:
        return mode >= FirstNamed && mode - FirstNamed < namedRow.size()
               && namedRow.at(mode - FirstNamed).has_value();
    }
}

int QSocPadEncoding::modeCode(const QString &mode) const
{
    static const QMap<QString, int> fixed
        = {{QStringLiteral("none"), None},
           {QStringLiteral("up"), Up},
           {QStringLiteral("down"), Down},
           {QStringLiteral("keeper"), Keeper},
           {QStringLiteral("oscillator"), Oscillator}};
    int code = fixed.value(mode, -1);
    if (code < 0) {
        const qsizetype index = namedMode.indexOf(mode);
        code                  = index < 0 ? -1 : int(FirstNamed + index);
    }
    return supports(code) ? code : -1;
}

namespace {

int rowIndex(const QList<QSocPadTableRow> &rows, const QString &label)
{
    for (qsizetype index = 0; index < rows.size(); ++index) {
        if (rows.at(index).label == label) {
            return int(index);
        }
    }
    return -1;
}

} // namespace

int QSocPadEncoding::upSel(const QString &strength) const
{
    return rowIndex(upRows, strength);
}

int QSocPadEncoding::downSel(const QString &strength) const
{
    return rowIndex(downRows, strength);
}

qsizetype QSocPadEncoding::controlIndex(const QString &name) const
{
    for (qsizetype index = 0; index < control.size(); ++index) {
        if (control.at(index).name == name) {
            return index;
        }
    }
    return -1;
}

int QSocPadEncoding::controlCode(qsizetype index, const QString &label) const
{
    if (index < 0 || index >= control.size()) {
        return -1;
    }
    return int(control.at(index).label.indexOf(label));
}

int QSocPadEncoding::requestMode(const QSocIomuxPullRequest &request) const
{
    return request.empty() ? None : std::max(0, modeCode(request.mode));
}

int QSocPadEncoding::routeMode(const QSocIomuxRoutePlan &route) const
{
    if (route.pullSelect.linked()) {
        return requestMode(route.pullSelect.off);
    }
    return requestMode({route.pullMode, route.pullStrength});
}

/* A woven keeper or oscillator carries its strength into both directions;
 * a plain up or down carries it into its own. Anything else leaves the
 * first row, which is what an absent strength means. */
int QSocPadEncoding::requestUpSel(const QSocIomuxPullRequest &request) const
{
    const int mode = requestMode(request);
    if (request.strength.isEmpty()
        || (mode != Up && !(weaves && (mode == Keeper || mode == Oscillator)))) {
        return 0;
    }
    return std::max(0, upSel(request.strength));
}

int QSocPadEncoding::requestDownSel(const QSocIomuxPullRequest &request) const
{
    const int mode = requestMode(request);
    if (request.strength.isEmpty()
        || (mode != Down && !(weaves && (mode == Keeper || mode == Oscillator)))) {
        return 0;
    }
    return std::max(0, downSel(request.strength));
}

int QSocPadEncoding::routeUpSel(const QSocIomuxRoutePlan &route) const
{
    return requestUpSel(
        route.pullSelect.linked() ? route.pullSelect.off
                                  : QSocIomuxPullRequest{route.pullMode, route.pullStrength});
}

int QSocPadEncoding::routeDownSel(const QSocIomuxRoutePlan &route) const
{
    return requestDownSel(
        route.pullSelect.linked() ? route.pullSelect.off
                                  : QSocIomuxPullRequest{route.pullMode, route.pullStrength});
}

int QSocPadEncoding::controlCodeOrDefault(qsizetype index, const QString &label) const
{
    if (label.isEmpty()) {
        return control.at(index).defaultCode;
    }
    return std::max(0, controlCode(index, label));
}

int QSocPadEncoding::routeControlCode(const QSocIomuxRoutePlan &route, qsizetype index) const
{
    const QSocIomuxControlRequest request = route.control.value(control.at(index).name);
    return controlCodeOrDefault(
        index, request.select.linked() ? request.select.off.mode : request.row);
}

QString QSocPadEncoding::modeSummary() const
{
    QStringList parts;
    const char *fixed[] = {"none", "up", "down", "keeper", "oscillator"};
    for (int mode = None; mode < FirstNamed; ++mode) {
        if (supports(mode)) {
            parts.append(QString("%1 %2").arg(mode).arg(QString(fixed[mode])));
        }
    }
    for (qsizetype index = 0; index < namedMode.size(); ++index) {
        if (supports(int(FirstNamed + index))) {
            parts.append(QString("%1 %2").arg(FirstNamed + index).arg(namedMode.at(index)));
        }
    }
    return parts.join(", ");
}

const QSocPadTableRow &QSocPadEncoding::row(int mode, int upIndex, int downIndex) const
{
    switch (mode) {
    case Up:
        if (upIndex >= 0 && upIndex < upRows.size()) {
            return upRows.at(upIndex);
        }
        return noneRow;
    case Down:
        if (downIndex >= 0 && downIndex < downRows.size()) {
            return downRows.at(downIndex);
        }
        return noneRow;
    case Keeper:
        return keeperRow ? *keeperRow : noneRow;
    case Oscillator:
        return oscillatorRow ? *oscillatorRow : noneRow;
    default:
        if (mode >= FirstNamed && mode - FirstNamed < namedRow.size()
            && namedRow.at(mode - FirstNamed)) {
            return *namedRow.at(mode - FirstNamed);
        }
        return noneRow;
    }
}

QSocIomuxLayoutVersion QSocIomuxGenerator::layoutVersion()
{
    return {};
}

QString QSocIomuxGenerator::padLane(quint32 pin)
{
    return QString("[%1:%2]").arg((pin + 1) * kPadLane - 1).arg(pin * kPadLane);
}

QString QSocIomuxGenerator::padLaneValue(quint32 width, const QString &value)
{
    if (width >= kPadLane) {
        return value;
    }
    return QString("{%1'b0, %2}").arg(kPadLane - width).arg(value);
}

const QSocPadCellPlan &QSocIomuxPlan::padClass(quint32 pin) const
{
    static const QSocPadCellPlan none;
    const int                    index = pinClass.value(qsizetype(pin), 0);
    return index >= 0 && index < padCells.size() ? padCells.at(index) : none;
}

QString QSocIomuxPlan::padSide(quint32 pin) const
{
    for (auto it = ioRing.side.cbegin(); it != ioRing.side.cend(); ++it) {
        for (const QSocIoRingItem &item : it.value()) {
            if (item.kind == QSocIoRingItem::Pin && item.pin == pin) {
                return it.key();
            }
        }
    }
    return QString();
}

QString QSocIomuxPlan::padModule(quint32 pin) const
{
    return ringModule(padClass(pin).cell, padSide(pin));
}

QString QSocIomuxPlan::ringModule(const QString &cell, const QString &side) const
{
    if (side.isEmpty() || !ioLib.contains(cell)) {
        return cell;
    }
    return ioLib.value(cell).variant.value(side, cell);
}

const QStringList &QSocIomuxGenerator::ringSides()
{
    static const QStringList sides
        = {QStringLiteral("west"),
           QStringLiteral("south"),
           QStringLiteral("east"),
           QStringLiteral("north")};
    return sides;
}

QSocPadModel QSocIomuxGenerator::padModel(
    const QList<QSocPadCellPlan> &cells,
    const QStringList            &modeOrder,
    const QStringList            &controlOrder)
{
    QSocPadModel  model;
    qsizetype     upRows   = 0;
    qsizetype     downRows = 0;
    QSet<QString> named;
    for (const QString &name : controlOrder) {
        model.control.append({name, 0});
    }
    for (const QSocPadCellPlan &cell : cells) {
        model.inputValue   = model.inputValue || !cell.portInputValue.isEmpty();
        model.inputEnable  = model.inputEnable || !cell.portInputEnable.isEmpty();
        model.outputValue  = model.outputValue || !cell.portOutputValue.isEmpty();
        model.outputEnable = model.outputEnable || !cell.portOutputEnable.isEmpty();
        model.safe         = model.safe || cell.safe.declared;
        if (!cell.pull.mode.isEmpty()) {
            model.pull = true;
            upRows     = std::max(upRows, cell.pull.mode.value(QStringLiteral("up")).size());
            downRows   = std::max(downRows, cell.pull.mode.value(QStringLiteral("down")).size());
            for (auto it = cell.pull.mode.cbegin(); it != cell.pull.mode.cend(); ++it) {
                static const QStringList fixed
                    = {QStringLiteral("none"),
                       QStringLiteral("up"),
                       QStringLiteral("down"),
                       QStringLiteral("keeper"),
                       QStringLiteral("oscillator")};
                if (!fixed.contains(it.key())) {
                    named.insert(it.key());
                }
            }
        }
        for (const QSocPadControlPlan &item : cell.control) {
            const quint32 width = item.row.size() > 1 ? encodingWidth(item.row.size()) : 0;
            qsizetype     index = 0;
            while (index < model.control.size() && model.control.at(index).name != item.name) {
                ++index;
            }
            if (index == model.control.size()) {
                model.control.append({item.name, width});
            } else {
                model.control[index].width = std::max(model.control.at(index).width, width);
            }
        }
    }
    QStringList rest = QStringList(named.cbegin(), named.cend());
    rest.sort();
    model.namedMode = modeOrder;
    for (const QString &name : rest) {
        if (!model.namedMode.contains(name)) {
            model.namedMode.append(name);
        }
    }
    if (model.pull) {
        model.modeWidth = encodingWidth(QSocPadEncoding::FirstNamed + model.namedMode.size());
    }
    if (upRows > 1) {
        model.upSelWidth = encodingWidth(upRows);
    }
    if (downRows > 1) {
        model.downSelWidth = encodingWidth(downRows);
    }
    return model;
}

QSocPadEncoding QSocIomuxGenerator::padEncoding(
    const QSocPadCellPlan &cell, const QSocPadModel &model)
{
    QSocPadEncoding encoding;
    for (const QSocPadModel::Control &slot : model.control) {
        QSocPadEncoding::Control entry;
        entry.name = slot.name;
        for (qsizetype index = 0; index < cell.control.size(); ++index) {
            const QSocPadControlPlan &item = cell.control.at(index);
            if (item.name != slot.name) {
                continue;
            }
            entry.cellIndex = index;
            for (const QSocPadTableRow &row : item.row) {
                entry.label.append(row.label);
            }
            entry.width       = item.row.size() > 1 ? encodingWidth(item.row.size()) : 0;
            entry.defaultCode = item.defaultRow;
        }
        encoding.control.append(entry);
    }
    if (!cell.declared() || cell.pull.mode.isEmpty()) {
        return encoding;
    }
    const auto single = [&](const QString &name) -> std::optional<QSocPadTableRow> {
        const QList<QSocPadTableRow> rows = cell.pull.mode.value(name);
        if (rows.isEmpty()) {
            return std::nullopt;
        }
        return rows.first();
    };
    encoding.noneRow       = single(QStringLiteral("none")).value_or(QSocPadTableRow());
    encoding.upRows        = cell.pull.mode.value(QStringLiteral("up"));
    encoding.downRows      = cell.pull.mode.value(QStringLiteral("down"));
    encoding.keeperRow     = single(QStringLiteral("keeper"));
    encoding.oscillatorRow = single(QStringLiteral("oscillator"));
    /* Named modes take the model's numbers, so a code means the same mode on
     * every pin whichever class it instantiates. */
    encoding.namedMode = model.namedMode;
    for (const QString &name : model.namedMode) {
        encoding.namedRow.append(single(name));
    }
    encoding.weaves    = !encoding.keeperRow && !encoding.oscillatorRow && encoding.hasUp()
                         && encoding.hasDown() && !cell.pull.isDriver;
    encoding.modeWidth = model.modeWidth;
    if (encoding.upRows.size() > 1) {
        encoding.upSelWidth = encodingWidth(encoding.upRows.size());
    }
    if (encoding.downRows.size() > 1) {
        encoding.downSelWidth = encodingWidth(encoding.downRows.size());
    }
    return encoding;
}

bool QSocIomuxGenerator::checkPadCellPorts(
    const QSocPadCellPlan &cell, const QMap<QString, QString> &cellPorts, QStringList *errors)
{
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
    for (const QSocPadControlPlan &item : cell.control) {
        for (const QString &port : item.port) {
            expect(port, QStringLiteral("in"), QStringLiteral("control.") + item.name + ".port");
        }
    }

    /* Every input of the cell must be named somewhere, or the instance would
     * leave it floating, which elaborates and is wrong. */
    QSet<QString> driven
        = {cell.portPad, cell.portInputEnable, cell.portOutputValue, cell.portOutputEnable};
    for (const QString &port : cell.pull.port) {
        driven.insert(port);
    }
    for (const QSocPadControlPlan &item : cell.control) {
        for (const QString &port : item.port) {
            driven.insert(port);
        }
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
                         "role, pull, or control")
                         .arg(cell.cell, undriven.join(", ")));
    }

    local.sort(Qt::CaseSensitive);
    if (errors) {
        *errors = local;
    }
    return local.isEmpty();
}

bool QSocIomuxGenerator::checkDirectPorts(
    const QSocIoRingDirect &direct, const QMap<QString, QString> &cellPorts, QStringList *errors)
{
    QStringList   local;
    const QString where = "IOMUX_RING generator.io_ring.direct." + direct.key;
    for (auto it = direct.port.cbegin(); it != direct.port.cend(); ++it) {
        if (!cellPorts.contains(it.key())) {
            local.append(QString("%1: %2 has no port %3").arg(where, direct.cell, it.key()));
        } else if (it.value().startsWith("1'b") && cellPorts.value(it.key()) != "in") {
            local.append(QString("%1: port %2 of %3 is not an input, a constant cannot drive it")
                             .arg(where, it.key(), direct.cell));
        }
    }
    QStringList undriven;
    for (auto it = cellPorts.cbegin(); it != cellPorts.cend(); ++it) {
        if ((it.value() == "in" || it.value() == "inout") && !direct.port.contains(it.key())) {
            undriven.append(it.key());
        }
    }
    if (!undriven.isEmpty()) {
        undriven.sort();
        local.append(QString("%1: %2 input pins %3 are not named")
                         .arg(where, direct.cell, undriven.join(", ")));
    }
    local.sort(Qt::CaseSensitive);
    if (errors) {
        *errors = local;
    }
    return local.isEmpty();
}

QString QSocIomuxGenerator::selectPortName(quint32 pin, quint32 slot, const QString &group)
{
    return QString("hs_p%1_s%2_%3_select_i").arg(pin).arg(slot).arg(group);
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
        valid = validateOptions(localPlan, &localErrors);
    }
    if (valid && localErrors.isEmpty()) {
        valid = validatePadCapability(localPlan, &localErrors);
    }
    if (valid && localErrors.isEmpty()) {
        for (QSocPadCellPlan &cell : localPlan.padCells) {
            valid = validatePadConstraints(cell, &localErrors) && valid;
        }
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

QList<QSocIomuxCorePort> QSocIomuxGenerator::corePinOptionPorts(
    const QSocIomuxPlan &plan, quint32 pin)
{
    QList<QSocIomuxCorePort> ports;
    const QSocPadModel      &model = plan.padModel;
    if (plan.option.gpio) {
        ports.append({QString("pin_%1_input_enable").arg(pin), 1});
        ports.append({QString("pin_%1_output_value").arg(pin), 1});
        ports.append({QString("pin_%1_output_enable").arg(pin), 1});
        ports.append({QString("pin_%1_input_enable_src").arg(pin), 1});
        ports.append({QString("pin_%1_output_value_src").arg(pin), 2});
        ports.append({QString("pin_%1_output_enable_src").arg(pin), 2});
    }
    if (plan.option.padControl) {
        if (model.hasPull()) {
            ports.append({QString("pin_%1_pull_mode").arg(pin), model.modeWidth});
            if (model.upSelWidth > 0) {
                ports.append({QString("pin_%1_up_sel").arg(pin), model.upSelWidth});
            }
            if (model.downSelWidth > 0) {
                ports.append({QString("pin_%1_down_sel").arg(pin), model.downSelWidth});
            }
            ports.append({QString("pin_%1_pull_src").arg(pin), 1});
        }
        for (const QSocPadModel::Control &item : model.control) {
            if (item.width == 0) {
                continue;
            }
            ports.append({QString("pin_%1_%2").arg(pin).arg(item.name), item.width});
            ports.append({QString("pin_%1_%2_src").arg(pin).arg(item.name), 1});
        }
    }
    if (plan.option.invert) {
        ports.append({QString("pin_%1_input_enable_inv").arg(pin), 1});
        ports.append({QString("pin_%1_output_value_inv").arg(pin), 1});
        ports.append({QString("pin_%1_output_enable_inv").arg(pin), 1});
        for (quint32 slot = 0; slot < plan.hsSlots; ++slot) {
            ports.append({QString("pin_%1_rx_inv_s%2").arg(pin).arg(slot), 1});
        }
        if (model.hasPull()) {
            ports.append({QString("pin_%1_pull_inv").arg(pin), 1});
        }
        for (const QSocPadModel::Control &item : model.control) {
            if (item.width > 0) {
                ports.append({QString("pin_%1_%2_inv").arg(pin).arg(item.name), 1});
            }
        }
    }
    if (plan.option.rxOverride) {
        for (quint32 slot = 0; slot < plan.hsSlots; ++slot) {
            ports.append({QString("pin_%1_rx_src_s%2").arg(pin).arg(slot), 1});
            ports.append({QString("pin_%1_rx_value_s%2").arg(pin).arg(slot), 1});
        }
    }
    return ports;
}

QList<QSocIomuxCorePort> QSocIomuxGenerator::corePadSelectPorts(const QSocIomuxPlan &plan)
{
    QList<QSocIomuxCorePort> ports;
    const QSocPadModel      &model = plan.padModel;
    const quint32            width = kPadLane * plan.pinCount;
    if (model.hasPull()) {
        ports.append({QStringLiteral("pad_pull_mode"), width});
        if (model.upSelWidth > 0) {
            ports.append({QStringLiteral("pad_up_sel"), width});
        }
        if (model.downSelWidth > 0) {
            ports.append({QStringLiteral("pad_down_sel"), width});
        }
    }
    for (const QSocPadModel::Control &item : model.control) {
        if (item.width > 0) {
            ports.append({QString("pad_%1_select").arg(item.name), width});
        }
    }
    return ports;
}

QString QSocIomuxGenerator::generateCoreVerilog(const QSocIomuxPlan &plan)
{
    if (plan.pinCount == 0 || plan.hsSlots == 0) {
        return QString();
    }
    const quint32       width = selectorWidth(plan.hsSlots);
    const quint64       dense = quint64(plan.hsSlots) * plan.pinCount;
    const QSocPadModel &model = plan.padModel;

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
    for (const QSocIomuxCorePort &port : QSocIomuxGenerator::corePadSelectPorts(plan)) {
        ports.append(QString("    output wire %1 %2_o").arg(vectorRange(port.width), port.name));
    }
    for (quint32 pin = 0; pin < plan.pinCount; ++pin) {
        ports.append(QString("    input  wire %1 pin_%2_select_i").arg(vectorRange(width)).arg(pin));
        for (const QSocIomuxCorePort &port : QSocIomuxGenerator::corePinOptionPorts(plan, pin)) {
            ports.append(portDeclaration(port, QStringLiteral("_i")));
        }
    }
    for (const EndpointPort &port : endpointPorts(plan)) {
        if (port.isSelect()) {
            ports.append(QString("    input  wire        %1").arg(endpointName(port)));
        }
    }
    if (model.safe) {
        ports.append(QStringLiteral("    input  wire        pad_force_i"));
    }
    /* The safe row sits above every register and slot, so it wraps the
     * finished expression of each pad output. */
    const auto withForce = [&](const QString &expression, const QString &safeValue) {
        return model.safe ? QString("pad_force_i ? %1 : (%2)").arg(safeValue, expression)
                          : expression;
    };
    for (qsizetype index = 0; index < ports.size(); ++index) {
        const QString suffix = index + 1 == ports.size() ? QString() : QString(",");
        lines.append(ports.at(index) + suffix);
    }
    lines.append(");");
    lines.append(QString());

    for (quint32 pin = 0; pin < plan.pinCount; ++pin) {
        const QSocPadEncoding  encoding = padEncoding(plan.padClass(pin), model);
        const QSocPadSafePlan &safe     = plan.padClass(pin).safe;
        const QString          safeIe   = QString("1'b%1").arg(safe.inputEnable);
        const QString          safeOv   = QString("1'b%1").arg(safe.outputValue);
        const QString          safeOe   = QString("1'b%1").arg(safe.outputEnable);
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
        const QString ieInv = QString("pin_%1_input_enable_inv_i").arg(pin);
        const QString ovInv = QString("pin_%1_output_value_inv_i").arg(pin);
        const QString oeInv = QString("pin_%1_output_enable_inv_i").arg(pin);
        if (!plan.option.gpio) {
            lines.append(
                QString("assign pad_input_enable_o[%1]  = %2;")
                    .arg(pin)
                    .arg(withForce(
                        withInversion(plan, QString("tx_bundle_%1[2]").arg(pin), ieInv), safeIe)));
            lines.append(
                QString("assign pad_output_value_o[%1]  = %2;")
                    .arg(pin)
                    .arg(withForce(
                        withInversion(plan, QString("tx_bundle_%1[1]").arg(pin), ovInv), safeOv)));
            lines.append(
                QString("assign pad_output_enable_o[%1] = %2;")
                    .arg(pin)
                    .arg(withForce(
                        withInversion(plan, QString("tx_bundle_%1[0]").arg(pin), oeInv), safeOe)));
        } else {
            /* A cross tap reads the slot mux output, never the source mux output,
             * so no encoding of the two source fields can close a loop. */
            lines.append(QString("assign pad_input_enable_o[%1] = %2;")
                             .arg(pin)
                             .arg(withForce(
                                 withInversion(
                                     plan,
                                     QString(
                                         "pin_%1_input_enable_src_i"
                                         " ? pin_%1_input_enable_i : tx_bundle_%1[2]")
                                         .arg(pin),
                                     ieInv),
                                 safeIe)));
            lines.append(QString("reg pad_output_value_%1;").arg(pin));
            lines.append("always @(*) begin");
            lines.append(QString("    case (pin_%1_output_value_src_i)").arg(pin));
            lines.append(
                QString("        2'd1: pad_output_value_%1 = pin_%1_output_value_i;").arg(pin));
            lines.append(QString("        2'd2: pad_output_value_%1 = tx_bundle_%1[2];").arg(pin));
            lines.append(QString("        2'd3: pad_output_value_%1 = tx_bundle_%1[0];").arg(pin));
            lines.append(
                QString("        default: pad_output_value_%1 = tx_bundle_%1[1];").arg(pin));
            lines.append("    endcase");
            lines.append("end");
            lines.append(QString("assign pad_output_value_o[%1] = %2;")
                             .arg(pin)
                             .arg(withForce(
                                 withInversion(plan, QString("pad_output_value_%1").arg(pin), ovInv),
                                 safeOv)));
            lines.append(QString("reg pad_output_enable_%1;").arg(pin));
            lines.append("always @(*) begin");
            lines.append(QString("    case (pin_%1_output_enable_src_i)").arg(pin));
            lines.append(
                QString("        2'd1: pad_output_enable_%1 = pin_%1_output_enable_i;").arg(pin));
            lines.append(QString("        2'd2: pad_output_enable_%1 = tx_bundle_%1[1];").arg(pin));
            lines.append(QString("        2'd3: pad_output_enable_%1 = 1'b0;").arg(pin));
            lines.append(
                QString("        default: pad_output_enable_%1 = tx_bundle_%1[0];").arg(pin));
            lines.append("    endcase");
            lines.append("end");
            lines.append(
                QString("assign pad_output_enable_o[%1] = %2;")
                    .arg(pin)
                    .arg(withForce(
                        withInversion(plan, QString("pad_output_enable_%1").arg(pin), oeInv),
                        safeOe)));
        }
        /* Each slot carries the constants its route asked for; the register
         * takes over a whole group when its source bit is set. */
        const auto emitCode = [&](const char *name,
                                  const char *reg,
                                  quint32     width,
                                  const char *src,
                                  int         safeCode,
                                  int         defaultCode,
                                  const std::function<QString(const QSocIomuxRoutePlan &)> &code) {
            const QString chain = slotCodeChain(plan, pin, width, defaultCode, code);
            const QString value = withForce(
                plan.option.padControl
                    ? QString("pin_%1_%2_i ? pin_%1_%3_i : %4").arg(pin).arg(src, reg, chain)
                    : chain,
                QString("%1'd%2").arg(width).arg(safeCode));
            lines.append(QString("assign pad_%1_o%2 = %3;")
                             .arg(name, padLane(pin), padLaneValue(width, value)));
        };
        /* A lane the pin's class has nothing for is driven low; its pad
         * never reads it and the field above it stays writable. */
        const auto emitZero = [&](const QString &name) {
            lines.append(
                QString("assign pad_%1_o%2 = %3'd0;").arg(name, padLane(pin)).arg(kPadLane));
        };
        if (model.hasPull() && !encoding.hasPull()) {
            emitZero(QStringLiteral("pull_mode"));
            if (model.upSelWidth > 0) {
                emitZero(QStringLiteral("up_sel"));
            }
            if (model.downSelWidth > 0) {
                emitZero(QStringLiteral("down_sel"));
            }
        } else if (model.hasPull()) {
            emitCode(
                "pull_mode",
                "pull_mode",
                model.modeWidth,
                "pull_src",
                encoding.requestMode(safe.pull),
                0,
                [&](const auto &r) {
                    return slotCode(
                        plan,
                        r,
                        "pull",
                        model.modeWidth,
                        0,
                        encoding.routeMode(r),
                        encoding.requestMode(r.pullSelect.on),
                        encoding.requestMode(r.pullSelect.off));
                });
            if (model.upSelWidth > 0) {
                emitCode(
                    "up_sel",
                    "up_sel",
                    model.upSelWidth,
                    "pull_src",
                    encoding.requestUpSel(safe.pull),
                    0,
                    [&](const auto &r) {
                        return slotCode(
                            plan,
                            r,
                            "pull",
                            model.upSelWidth,
                            0,
                            encoding.routeUpSel(r),
                            encoding.requestUpSel(r.pullSelect.on),
                            encoding.requestUpSel(r.pullSelect.off));
                    });
            }
            if (model.downSelWidth > 0) {
                emitCode(
                    "down_sel",
                    "down_sel",
                    model.downSelWidth,
                    "pull_src",
                    encoding.requestDownSel(safe.pull),
                    0,
                    [&](const auto &r) {
                        return slotCode(
                            plan,
                            r,
                            "pull",
                            model.downSelWidth,
                            0,
                            encoding.routeDownSel(r),
                            encoding.requestDownSel(r.pullSelect.on),
                            encoding.requestDownSel(r.pullSelect.off));
                    });
            }
        }
        for (qsizetype index = 0; index < encoding.control.size(); ++index) {
            const QSocPadEncoding::Control &item       = encoding.control.at(index);
            const quint32                   fieldWidth = model.control.at(index).width;
            if (fieldWidth == 0) {
                continue;
            }
            if (item.cellIndex < 0) {
                emitZero(item.name + "_select");
                continue;
            }
            const QByteArray name = item.name.toUtf8();
            const QByteArray src  = (item.name + "_src").toUtf8();
            const QByteArray out  = (item.name + "_select").toUtf8();
            emitCode(
                out.constData(),
                name.constData(),
                fieldWidth,
                src.constData(),
                encoding.controlCodeOrDefault(index, safe.control.value(item.name)),
                item.defaultCode,
                [&](const auto &r) {
                    const QSocIomuxControlRequest request = r.control.value(item.name);
                    return slotCode(
                        plan,
                        r,
                        item.name,
                        fieldWidth,
                        item.defaultCode,
                        encoding.routeControlCode(r, index),
                        encoding.controlCodeOrDefault(index, request.select.on.mode),
                        encoding.controlCodeOrDefault(index, request.select.off.mode));
                });
        }
        lines.append(QString());
    }

    for (quint32 pin = 0; pin < plan.pinCount; ++pin) {
        for (quint32 slot = 0; slot < plan.hsSlots; ++slot) {
            QString value = QString("pad_input_value_i[%1]").arg(pin);
            if (plan.option.rxOverride) {
                value = QString("pin_%1_rx_src_s%2_i ? pin_%1_rx_value_s%2_i : %3")
                            .arg(pin)
                            .arg(slot)
                            .arg(value);
            }
            lines.append(QString("assign rx_input_value_o[%1] = %2;")
                             .arg(denseIndex(plan, slot, pin))
                             .arg(withInversion(
                                 plan, value, QString("pin_%1_rx_inv_s%2_i").arg(pin).arg(slot))));
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
        if (port.isSelect()) {
            continue;
        }
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
        if (port.isSelect() || port.role != QSocIomuxRole::InputValue) {
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
    const QSocPadModel &model = plan.padModel;
    for (quint32 pin = 0; pin < plan.pinCount; ++pin) {
        lines.append(QString("wire %1 pin_%2_select_w;").arg(vectorRange(width)).arg(pin));
        for (const QSocIomuxCorePort &port : QSocIomuxGenerator::corePinOptionPorts(plan, pin)) {
            lines.append(
                port.width == 1 ? QString("wire       %1_w;").arg(port.name)
                                : QString("wire [%1:0] %2_w;").arg(port.width - 1).arg(port.name));
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
        if (plan.option.gpio) {
            regsConnections.append(
                QString("    .pin_%1_input_value_i(pad_input_sync_q[%1])").arg(pin));
        }
        for (const QSocIomuxCorePort &port : QSocIomuxGenerator::corePinOptionPorts(plan, pin)) {
            regsConnections.append(QString("    .%1_o(%1_w)").arg(port.name));
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
        if (!port.isSelect()) {
            connConnections.append(QString("    .%1(%1)").arg(endpointName(port)));
        }
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
    for (const QSocIomuxCorePort &port : QSocIomuxGenerator::corePadSelectPorts(plan)) {
        coreConnections.append(QString("    .%1_o(%1_o)").arg(port.name));
    }
    for (quint32 pin = 0; pin < plan.pinCount; ++pin) {
        coreConnections.append(QString("    .pin_%1_select_i(pin_%1_select_w)").arg(pin));
        for (const QSocIomuxCorePort &port : QSocIomuxGenerator::corePinOptionPorts(plan, pin)) {
            coreConnections.append(QString("    .%1_i(%1_w)").arg(port.name));
        }
    }
    for (const EndpointPort &port : endpoints) {
        if (port.isSelect()) {
            coreConnections.append(QString("    .%1(%1)").arg(endpointName(port)));
        }
    }
    if (model.safe) {
        coreConnections.append(QStringLiteral("    .pad_force_i(pad_force_i)"));
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

namespace {

/**
 * @brief One ring instance with its cell and what it stands for.
 */
struct RingInstance
{
    QString instance; /**< Path below the wrapper */
    QString cell;
    QString meaning;
    bool    inRing = true; /**< False for a signal pad, which lives in the pad module */
};

/**
 * @brief The instances of one side, in order; supplies and plain cells count
 * per net or cell across the whole ring unless an item names its own.
 */
QList<RingInstance> ringInstances(
    const QSocIomuxPlan &plan, const QString &side, QMap<QString, int> *count)
{
    QList<RingInstance> result;
    for (const QSocIoRingItem &item : plan.ioRing.side.value(side)) {
        switch (item.kind) {
        case QSocIoRingItem::Pin:
            result.append(
                {QString("u_pad_%1").arg(item.pin),
                 plan.padModule(item.pin),
                 QString("pin %1 class %2").arg(item.pin).arg(plan.padClass(item.pin).name),
                 false});
            break;
        case QSocIoRingItem::Power: {
            const int number = item.id >= 0 ? item.id : (*count)[item.name]++;
            result.append(
                {QString("u_%1_%2").arg(item.name).arg(number),
                 plan.ringModule(plan.ioRing.power.value(item.name), side),
                 QString("power %1").arg(item.name)});
            break;
        }
        case QSocIoRingItem::Cell: {
            const int number = (*count)[item.name]++;
            result.append(
                {item.instance.isEmpty() ? QString("u_%1_%2").arg(item.name).arg(number)
                                         : item.instance,
                 plan.ringModule(item.name, side),
                 QStringLiteral("cell")});
            break;
        }
        case QSocIoRingItem::Direct:
            result.append(
                {QString("u_%1").arg(item.name),
                 [&] {
                     for (const QSocIoRingDirect &direct : plan.ioRing.direct) {
                         if (direct.key == item.name) {
                             return plan.ringModule(direct.cell, side);
                         }
                     }
                     return QString();
                 }(),
                 QString("direct %1").arg(item.name)});
            break;
        }
    }
    return result;
}

} // namespace

QString QSocIomuxGenerator::generateIoVerilog(const QSocIomuxPlan &plan)
{
    if (plan.pinCount == 0 || !plan.hasPadCell()) {
        return QString();
    }
    const QSocPadModel &model = plan.padModel;

    QStringList lines;
    lines.append("// Generated by QSoC. Do not edit.");
    lines.append(QString("module %1 (").arg(ioModuleName(plan.moduleName)));
    const QList<QSocMmioPortDescription> ports = shellPortDescriptions(plan);
    for (qsizetype index = 0; index < ports.size(); ++index) {
        const QSocMmioPortDescription &port = ports.at(index);
        const QString keyword = port.direction == "output"  ? QStringLiteral("output")
                                : port.direction == "inout" ? QStringLiteral("inout ")
                                                            : QStringLiteral("input ");
        const QString suffix  = index + 1 == ports.size() ? QString() : QString(",");
        lines.append(
            port.width == 1 ? QString("    %1 wire %2%3").arg(keyword, port.name, suffix)
                            : QString("    %1 wire %2 %3%4")
                                  .arg(keyword, vectorRange(port.width), port.name, suffix));
    }
    lines.append(");");
    lines.append(QString());

    const auto slice = [&](const char *name, quint32 pin) {
        return QString("pad_%1_i%2").arg(name, padLane(pin));
    };
    for (quint32 pin = 0; pin < plan.pinCount; ++pin) {
        const QSocPadCellPlan &cell     = plan.padClass(pin);
        const QSocPadEncoding  encoding = padEncoding(cell, model);
        if (cell.portInputValue.isEmpty()) {
            /* A cell with no receiver reads as zero, so the bus is always driven. */
            lines.append(QString("assign pad_input_value_o[%1] = 1'b0;").arg(pin));
        }
        if (encoding.hasPull()) {
            QString mode = slice("pull_mode", pin);
            if (encoding.weaves) {
                /* The keeper follows the pad and the oscillator opposes it. The
                 * feedback reads the pad itself, not the receiver output, so an
                 * input enable of zero does not silently turn either into a
                 * pull-down. The loop closes here, inside the pad module. */
                lines.append(QString(
                                 "wire %1 pad_mode_eff_%2 = (%3 == %4'd%5) ? "
                                 "(pad_io[%2] ? %4'd%6 : %4'd%7) : (%3 == %4'd%8) ? "
                                 "(pad_io[%2] ? %4'd%7 : %4'd%6) : %3;")
                                 .arg(vectorRange(kPadLane))
                                 .arg(pin)
                                 .arg(mode)
                                 .arg(kPadLane)
                                 .arg(int(QSocPadEncoding::Keeper))
                                 .arg(int(QSocPadEncoding::Up))
                                 .arg(int(QSocPadEncoding::Down))
                                 .arg(int(QSocPadEncoding::Oscillator)));
                mode = QString("pad_mode_eff_%1").arg(pin);
            }
            const QString upSel   = encoding.upSelWidth > 0 ? slice("up_sel", pin) : QString();
            const QString downSel = encoding.downSelWidth > 0 ? slice("down_sel", pin) : QString();
            appendPadPullPorts(&lines, pin, encoding, cell.pull.port, mode, upSel, downSel);
        }
        for (const QSocPadEncoding::Control &entry : encoding.control) {
            if (entry.cellIndex < 0) {
                continue;
            }
            const QSocPadControlPlan &item = cell.control.at(entry.cellIndex);
            if (entry.width > 0) {
                appendPadTableCase(
                    &lines,
                    pin,
                    slice((item.name + "_select").toUtf8().constData(), pin),
                    item.port,
                    item.row,
                    item.defaultRow);
                continue;
            }
            /* One row: nothing selects it, the pins take it outright. */
            for (qsizetype bitIndex = 0; bitIndex < item.port.size(); ++bitIndex) {
                lines.append(QString("wire %1_%2_w = 1'b%3;")
                                 .arg(item.port.at(bitIndex))
                                 .arg(pin)
                                 .arg(item.row.first().value.at(bitIndex) == "1" ? 1 : 0));
            }
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
        for (const QSocPadControlPlan &item : cell.control) {
            for (const QString &port : item.port) {
                connections.append(QString("    .%1(%1_%2_w)").arg(port).arg(pin));
            }
        }
        lines.append(QString("%1 u_pad_%2 (").arg(plan.padModule(pin)).arg(pin));
        for (qsizetype index = 0; index < connections.size(); ++index) {
            const QString suffix = index + 1 == connections.size() ? QString() : QString(",");
            lines.append(connections.at(index) + suffix);
        }
        lines.append(");");
        lines.append(QString());
    }

    if (plan.ioRing.declared) {
        const QSocIoRingPlan &ring = plan.ioRing;
        lines.append(QString());
        if (!ring.corner.isEmpty()) {
            for (const char *corner : {"nw", "sw", "se", "ne"}) {
                lines.append(QString("%1 u_corner_%2 ();").arg(ring.corner, QString(corner)));
            }
            lines.append(QString());
        }
        QMap<QString, int> powerCount;
        for (const QString &side : ringSides()) {
            for (const RingInstance &item : ringInstances(plan, side, &powerCount)) {
                if (!item.inRing) {
                    continue;
                }
                const QString name = item.instance;
                if (!item.meaning.startsWith("direct ")) {
                    lines.append(QString("%1 %2 ();").arg(item.cell, name));
                    continue;
                }
                for (const QSocIoRingDirect &direct : ring.direct) {
                    if ("u_" + direct.key != name) {
                        continue;
                    }
                    QStringList connections;
                    for (auto it = direct.port.cbegin(); it != direct.port.cend(); ++it) {
                        connections.append(QString("    .%1(%2)").arg(it.key(), it.value()));
                    }
                    if (connections.isEmpty()) {
                        lines.append(QString("%1 %2 ();").arg(item.cell, name));
                        continue;
                    }
                    lines.append(QString("%1 %2 (").arg(item.cell, name));
                    lines.append(connections.join(",\n"));
                    lines.append(");");
                }
            }
        }
        const QSocIoRingGeometry geometry = ringGeometry(plan);
        if (geometry.complete) {
            lines.append(QString());
            for (const QSocIoRingPlacement &item : geometry.placement) {
                if (item.meaning == "fill") {
                    lines.append(QString("%1 %2 ();").arg(item.cell, item.instance));
                }
            }
        }
    }
    lines.append("endmodule");
    lines.append(QString());
    return lines.join('\n');
}

namespace {

/** The orientation a side or corner takes: the source's, or the convention. */
QString ringOrient(const QSocIoRingPlan &ring, const QString &where)
{
    static const QMap<QString, QString> convention
        = {{"west", "E"},
           {"south", "N"},
           {"east", "W"},
           {"north", "S"},
           {"sw", "N"},
           {"se", "W"},
           {"ne", "S"},
           {"nw", "E"}};
    return ring.orient.value(where, convention.value(where));
}

/** Lower left of a box `along` wide on `side`, `depth` deep, `offset` from its first corner. */
std::pair<double, double> ringPoint(
    const QSocIoRingPlan &ring,
    const QString        &side,
    double                corner,
    double                offset,
    double                along,
    double                depth)
{
    if (side == "west") {
        return {0, ring.dieHeight - corner - offset - along};
    }
    if (side == "south") {
        return {corner + offset, 0};
    }
    if (side == "east") {
        return {ring.dieWidth - depth, corner + offset};
    }
    return {ring.dieWidth - corner - offset - along, ring.dieHeight - depth};
}

} // namespace

QSocIoRingGeometry QSocIomuxGenerator::ringGeometry(const QSocIomuxPlan &plan)
{
    QSocIoRingGeometry    geometry;
    const QSocIoRingPlan &ring = plan.ioRing;
    if (!ring.declared) {
        geometry.missing.append(QStringLiteral("io_ring"));
        return geometry;
    }
    if (ring.dieWidth <= 0 || ring.dieHeight <= 0) {
        geometry.missing.append(QStringLiteral("io_ring.die"));
    }
    if (ring.corner.isEmpty()) {
        geometry.missing.append(QStringLiteral("io_ring.corner"));
    }
    /* A side variant measures as its base cell unless the library lists it. */
    const auto baseCell = [&](const QString &cell) {
        if (plan.ioLib.contains(cell)) {
            return cell;
        }
        for (const QSocIoLibCell &entry : plan.ioLib) {
            if (entry.variant.values().contains(cell)) {
                return entry.name;
            }
        }
        return cell;
    };
    const auto size = [&](const QString &named, double *width, double *height) {
        const QString       cell  = baseCell(named);
        const QSocIoLibCell entry = plan.ioLib.value(cell);
        if (!plan.ioLib.contains(cell) || entry.width <= 0) {
            const QString need = QString("io_lib.%1.width").arg(cell);
            if (!geometry.missing.contains(need)) {
                geometry.missing.append(need);
            }
            return false;
        }
        *width  = entry.width;
        *height = entry.height;
        return true;
    };
    /* The ring has one depth: a cell without a height takes the corner's,
     * and the corner without one is square. */
    double cornerWidth  = 0;
    double cornerHeight = 0;
    if (!ring.corner.isEmpty() && size(ring.corner, &cornerWidth, &cornerHeight)
        && cornerHeight <= 0) {
        cornerHeight = cornerWidth;
    }
    const auto depthOf = [&](double height) { return height > 0 ? height : cornerHeight; };
    /* Fill cells, widest first, close whatever the items leave open. */
    QList<std::pair<double, QString>> fills;
    for (const QSocIoLibCell &cell : plan.ioLib) {
        if (cell.kind == "fill" && cell.width > 0) {
            fills.append({cell.width, cell.name});
        }
    }
    std::sort(fills.begin(), fills.end(), [](const auto &left, const auto &right) {
        return left.first > right.first;
    });

    QMap<QString, int>         powerCount;
    QList<QSocIoRingPlacement> placement;
    bool                       fits = true;
    for (const QString &side : ringSides()) {
        const QList<RingInstance>    items  = ringInstances(plan, side, &powerCount);
        const QList<QSocIoRingItem> &source = ring.side.value(side);
        const double length    = (side == "west" || side == "east" ? ring.dieHeight : ring.dieWidth)
                                 - 2 * cornerWidth;
        double       cursor    = 0;
        int          fillIndex = 0;
        const auto   fillTo    = [&](double end) {
            for (const auto &[width, cell] : fills) {
                while (end - cursor >= width - 1e-9) {
                    placement.append(
                        {QString("u_fill_%1_%2").arg(side).arg(fillIndex++),
                         cell,
                         QStringLiteral("fill"),
                         side,
                         cursor,
                         width});
                    cursor += width;
                }
            }
            cursor = end;
        };
        for (qsizetype index = 0; index < items.size(); ++index) {
            const RingInstance   &item   = items.at(index);
            const QSocIoRingItem &entry  = source.at(index);
            double                width  = 0;
            double                height = 0;
            if (!size(item.cell, &width, &height)) {
                continue;
            }
            double start = cursor + entry.gap;
            if (entry.offset >= 0) {
                if (entry.offset < cursor - 1e-9) {
                    geometry.missing.append(QString("%1 offset %2 overlaps the item before it")
                                                .arg(item.instance)
                                                .arg(entry.offset));
                    fits = false;
                }
                start = std::max(start, entry.offset);
            }
            if (start > cursor + 1e-9) {
                fillTo(start);
            }
            placement.append({item.instance, item.cell, item.meaning, side, start, width});
            cursor = start + width;
        }
        if (geometry.missing.isEmpty()) {
            if (cursor > length + 1e-9) {
                geometry.missing.append(QString("%1 side holds %2 um of cells in %3 um")
                                            .arg(side)
                                            .arg(cursor)
                                            .arg(length));
                fits = false;
            } else {
                fillTo(length);
            }
        }
    }
    if (!geometry.missing.isEmpty() || !fits) {
        return geometry;
    }
    for (QSocIoRingPlacement &item : placement) {
        double width  = 0;
        double height = 0;
        size(item.cell, &width, &height);
        const auto [x, y]
            = ringPoint(ring, item.side, cornerWidth, item.offset, item.width, depthOf(height));
        item.x      = x;
        item.y      = y;
        item.orient = ringOrient(ring, item.side);
    }
    const std::pair<const char *, std::pair<double, double>> corners[]
        = {{"sw", {0, 0}},
           {"se", {ring.dieWidth - cornerWidth, 0}},
           {"ne", {ring.dieWidth - cornerWidth, ring.dieHeight - cornerHeight}},
           {"nw", {0, ring.dieHeight - cornerHeight}}};
    for (const auto &[name, point] : corners) {
        placement.prepend(
            {QString("u_corner_%1").arg(name),
             ring.corner,
             QStringLiteral("corner"),
             QString(name),
             0,
             cornerWidth,
             point.first,
             point.second,
             ringOrient(ring, name)});
    }
    geometry.complete  = true;
    geometry.placement = placement;
    return geometry;
}

QString QSocIomuxGenerator::generateRingDef(const QSocIomuxPlan &plan)
{
    const QSocIoRingGeometry geometry = ringGeometry(plan);
    if (!geometry.complete) {
        return QString();
    }
    const QString prefix = plan.ioRing.prefix.isEmpty()
                               ? ioModuleName(plan.integration.instance) + "/"
                               : plan.ioRing.prefix;
    const auto    unit   = [](double microns) { return qint64(std::llround(microns * 1000.0)); };
    QStringList   lines;
    lines.append("# Generated by QSoC. Do not edit.");
    lines.append("VERSION 5.8 ;");
    lines.append("DIVIDERCHAR \"/\" ;");
    lines.append("BUSBITCHARS \"[]\" ;");
    lines.append(QString("DESIGN %1 ;").arg(plan.moduleName));
    lines.append("UNITS DISTANCE MICRONS 1000 ;");
    lines.append(QString("DIEAREA ( 0 0 ) ( %1 %2 ) ;")
                     .arg(unit(plan.ioRing.dieWidth))
                     .arg(unit(plan.ioRing.dieHeight)));
    lines.append(QString());
    lines.append(QString("COMPONENTS %1 ;").arg(geometry.placement.size()));
    for (const QSocIoRingPlacement &item : geometry.placement) {
        /* Cells the netlist never drives are physical, which SOURCE DIST says. */
        const bool physical = item.meaning != "direct" && !item.meaning.startsWith("pin ")
                              && !item.meaning.startsWith("direct ");
        lines.append(QString("- %1%2 %3 +%4 FIXED ( %5 %6 ) %7 ;")
                         .arg(prefix, item.instance, item.cell)
                         .arg(physical ? QStringLiteral(" SOURCE DIST +") : QString())
                         .arg(unit(item.x))
                         .arg(unit(item.y))
                         .arg(item.orient));
    }
    lines.append("END COMPONENTS");
    lines.append(QString());
    lines.append("END DESIGN");
    lines.append(QString());
    return lines.join('\n');
}

QString QSocIomuxGenerator::generateRingReport(const QSocIomuxPlan &plan)
{
    if (!plan.ioRing.declared) {
        return QString();
    }
    const QSocIoRingPlan    &ring     = plan.ioRing;
    const QSocIoRingGeometry geometry = ringGeometry(plan);
    QStringList              lines;
    lines.append(QString("ring: %1_ring").arg(plan.moduleName));
    if (ring.dieWidth > 0 || ring.dieHeight > 0) {
        lines.append(QString("die: %1 x %2 um").arg(ring.dieWidth).arg(ring.dieHeight));
    }
    if (!ring.corner.isEmpty()) {
        lines.append(QString("corner: %1, u_corner_nw sw se ne").arg(ring.corner));
    }
    lines.append("instance names come from identity: a pad that moves keeps its name");
    if (geometry.complete) {
        lines.append("def: written, offsets below are microns from each side's first corner");
    } else {
        lines.append(QString("def: not written, needs %1").arg(geometry.missing.join("; ")));
    }
    QMap<QString, int> powerCount;
    for (const QString &side : ringSides()) {
        const QList<RingInstance> items = ringInstances(plan, side, &powerCount);
        lines.append(QString());
        if (!geometry.complete) {
            lines.append(QString("%1: %2 items").arg(side).arg(items.size()));
            for (qsizetype index = 0; index < items.size(); ++index) {
                const RingInstance &item = items.at(index);
                lines.append(
                    QString("  %1 %2 %3 %4").arg(index).arg(item.instance, item.cell, item.meaning));
            }
            continue;
        }
        QList<QSocIoRingPlacement> placed;
        for (const QSocIoRingPlacement &item : geometry.placement) {
            if (item.side == side) {
                placed.append(item);
            }
        }
        std::sort(placed.begin(), placed.end(), [](const auto &left, const auto &right) {
            return left.offset < right.offset;
        });
        lines.append(QString("%1: %2 items").arg(side).arg(placed.size()));
        for (qsizetype index = 0; index < placed.size(); ++index) {
            const QSocIoRingPlacement &item = placed.at(index);
            lines.append(QString("  %1 %2 %3 %4 at %5 width %6 xy %7 %8 %9")
                             .arg(index)
                             .arg(item.instance, item.cell, item.meaning)
                             .arg(item.offset)
                             .arg(item.width)
                             .arg(item.x)
                             .arg(item.y)
                             .arg(item.orient));
        }
    }
    lines.append(QString());
    return lines.join('\n');
}

QString QSocIomuxGenerator::generateFileList(const QSocIomuxPlan &plan)
{
    if (plan.pinCount == 0 || plan.hsSlots == 0) {
        return QString();
    }
    QString list = QString("%1_regs.v\n%1_conn.v\n%1.v\n").arg(plan.moduleName);
    if (plan.hasPadCell()) {
        list += ioModuleName(plan.moduleName) + ".v\n";
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
    /* The bus between the core and the shell takes the instance's name, so
     * two blocks on one chip never share a net. */
    const auto busNet = [&](const QSocMmioPortDescription &port) {
        QString name = port.name;
        name.chop(2);
        return integration.instance + "_" + name;
    };
    if (plan.hasPadCell()) {
        for (const QSocMmioPortDescription &port : padBusPorts(plan, true)) {
            lines.append(QString("      %1:").arg(port.name));
            lines.append(QString("        link: %1").arg(busNet(port)));
        }
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
    if (plan.padModel.safe) {
        lines.append("      pad_force_i:");
        lines.append(QString("        link: %1").arg(integration.force));
    }
    for (const EndpointPort &port : endpointPorts(plan)) {
        lines.append(QString("      %1:").arg(endpointName(port)));
        lines.append(QString("        link: %1").arg(port.endpoint->link));
        if (port.endpoint->bit.has_value()) {
            lines.append(QString("        bits: \"[%1]\"").arg(*port.endpoint->bit));
        }
    }
    if (plan.hasPadCell()) {
        lines.append(QString("  %1:").arg(ioModuleName(integration.instance)));
        lines.append(QString("    module: %1").arg(ioModuleName(plan.moduleName)));
        lines.append("    port:");
        lines.append("      pad_io:");
        lines.append(QString("        uplink: %1").arg(integration.padIo));
        for (const QSocMmioPortDescription &port : padBusPorts(plan, false)) {
            lines.append(QString("      %1:").arg(port.name));
            lines.append(QString("        link: %1").arg(busNet(port)));
        }
        for (const QSocIoRingDirect &direct : plan.ioRing.direct) {
            for (auto it = direct.port.cbegin(); it != direct.port.cend(); ++it) {
                if (it.value().startsWith("1'b")) {
                    continue;
                }
                lines.append(QString("      %1:").arg(it.value()));
                lines.append(QString("        %1: %2")
                                 .arg(
                                     direct.cellPorts.value(it.key()) == "inout"
                                         ? QStringLiteral("uplink")
                                         : QStringLiteral("link"),
                                     it.value()));
            }
        }
    }
    lines.append("bus:");
    lines.append(QString("  %1:").arg(integration.control));
    lines.append(QString("    - instance: %1").arg(integration.instance));
    lines.append("      port: control");
    lines.append(QString());
    return lines.join('\n');
}

QString QSocIomuxGenerator::ioModuleName(const QString &moduleName)
{
    return moduleName + "_io";
}

YAML::Node QSocIomuxGenerator::describeIoModuleYaml(const QSocIomuxPlan &plan)
{
    YAML::Node module(YAML::NodeType::Map);
    if (plan.pinCount == 0 || plan.hsSlots == 0 || !plan.hasPadCell()) {
        return module;
    }
    for (const QSocMmioPortDescription &port : shellPortDescriptions(plan)) {
        YAML::Node portNode(YAML::NodeType::Map);
        portNode["type"]      = port.width == 1
                                    ? std::string("logic")
                                    : QString("logic[%1:0]").arg(port.width - 1).toStdString();
        portNode["direction"] = port.direction.toStdString();
        module["port"][port.name.toStdString()] = portNode;
    }
    return module;
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
    const quint32                      bankWords = (plan.pinCount + dataWidth - 1) / dataWidth;
    /* Register blocks in composition order, each with its register count. */
    struct Block
    {
        const char *name;
        qsizetype   count;
    };
    QList<Block> blocks;
    if (plan.option.gpio) {
        blocks.append({"gpio", qsizetype(4) * bankWords});
    }
    if (plan.option.rxOverride) {
        blocks.append({"rx override", qsizetype(plan.hsSlots) * bankWords});
    }
    if (plan.option.interrupt) {
        blocks.append({"interrupt", qsizetype(8) * bankWords});
    }
    if (plan.option.invert) {
        const QSocPadModel &model = plan.padModel;
        qsizetype           nets  = model.hasPull() ? 1 : 0;
        for (const QSocPadModel::Control &item : model.control) {
            nets += item.width > 0 ? 1 : 0;
        }
        blocks.append({"invert", (qsizetype(3 + plan.hsSlots) + nets) * bankWords});
    }
    if (plan.option.sourceControl()) {
        blocks.append({"source control", qsizetype(plan.pinCount)});
    }
    if (plan.option.padControl) {
        /* One word per pin when the cell has a pull table or a selectable
         * control in the first four, then one per group of eight holding one. */
        const QSocPadModel &model      = plan.padModel;
        const auto          selectable = [&](qsizetype first, qsizetype count) {
            for (qsizetype index = first; index < std::min(model.control.size(), first + count);
                 ++index) {
                if (model.control.at(index).width > 0) {
                    return true;
                }
            }
            return false;
        };
        qsizetype words = model.hasPull() || selectable(0, kPadWordLanes) ? 1 : 0;
        for (qsizetype word = 0; kPadWordLanes + word * kCtlWordLanes < model.control.size();
             ++word) {
            words += selectable(kPadWordLanes + word * kCtlWordLanes, kCtlWordLanes) ? 1 : 0;
        }
        blocks.append({"pad control", words * qsizetype(plan.pinCount)});
    }
    const qsizetype identityCount = identityRegisterCount(dataWidth);
    qsizetype       expected      = identityCount + selectorWordCount(plan.pinCount, dataWidth);
    for (const Block &block : blocks) {
        expected += block.count;
    }
    if (registers.size() != expected) {
        return QString();
    }
    const quint32 byteCount = dataWidth / 8;
    const quint32 lanes     = pinsPerWord(dataWidth);
    const quint32 width     = selectorWidth(plan.hsSlots);
    const quint64 aperture = std::max(kApertureBytes, registers.constLast().byteOffset + byteCount);
    /* Fold the composed identity words so the report cannot publish a value
     * the read function does not emit. Each word is the 32 bits at its byte
     * offset, whatever the data width. */
    const auto wordAt = [&](quint64 byteOffset) {
        quint64 value = 0;
        for (const QSocMmioRegisterPlan &reg : registers) {
            if (byteOffset < reg.byteOffset || byteOffset >= reg.byteOffset + byteCount) {
                continue;
            }
            const quint32 shift = quint32(byteOffset - reg.byteOffset) * 8;
            for (const QSocMmioFieldPlan &field : reg.fields) {
                const quint32 low  = std::max(field.lsb, shift);
                const quint32 high = std::min(field.lsb + field.width, shift + 32);
                if (!field.constantValue.has_value() || low >= high) {
                    continue;
                }
                /* The read function emits a width-sized literal, which Verilog
                 * truncates, and a field may straddle the word boundary. */
                const quint64 mask = field.width >= 64 ? ~quint64(0)
                                                       : ((quint64(1) << field.width) - 1);
                const quint64 bits = (*field.constantValue & mask) >> (low - field.lsb);
                value |= (bits & ((quint64(1) << (high - low)) - 1)) << (low - shift);
            }
        }
        return value & 0xffffffffULL;
    };
    const QSocIomuxLayoutVersion layout = layoutVersion();

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
    lines.append(QString("identity: version %1.%2.%3 build %4, type 0x%5 at offset 0x0 to 0xc")
                     .arg(layout.major)
                     .arg(layout.minor)
                     .arg(layout.patch)
                     .arg(plan.build)
                     .arg(wordAt(4), 8, 16, QLatin1Char('0')));
    lines.append(
        QString("selector registers: %1 at offset 0x%2 to 0x%3")
            .arg(selectorWords)
            .arg(QString::number(registers.at(identityCount).byteOffset, 16))
            .arg(QString::number(registers.at(identityCount + selectorWords - 1).byteOffset, 16)));
    qsizetype cursor = identityCount + selectorWords;
    for (const Block &block : blocks) {
        lines.append(
            QString("%1 registers: %2 at offset 0x%3 to 0x%4")
                .arg(QString(block.name))
                .arg(block.count)
                .arg(QString::number(registers.at(cursor).byteOffset, 16))
                .arg(QString::number(registers.at(cursor + block.count - 1).byteOffset, 16)));
        cursor += block.count;
    }
    if (plan.option.interrupt) {
        lines.append(QString("interrupt lines: %1, one per %2 pins")
                         .arg(interruptLineCount(plan))
                         .arg(dataWidth));
    }
    lines.append(QString("registers total: %1").arg(registers.size()));
    lines.append(QString("aperture: %1 bytes").arg(aperture));
    lines.append(QString("capability: 0x%1 at offset 0x8").arg(wordAt(8), 8, 16, QLatin1Char('0')));
    lines.append(QString("feature: 0x%1 at offset 0xc").arg(wordAt(12), 8, 16, QLatin1Char('0')));
    lines.append("reset: every selector resets to 0 and selects slot 0");
    lines.append("rx: pad input broadcasts to every declared sink regardless of the selector");
    for (const QSocPadCellPlan &cell : plan.padCells) {
        const QString label = plan.padCells.size() == 1 ? QStringLiteral("pad cell")
                                                        : QString("pad cell %1").arg(cell.name);
        lines.append(QString("%1: %2, pull modes %3, controls %4, constraints %5")
                         .arg(label, cell.cell)
                         .arg(cell.pull.mode.size())
                         .arg(cell.control.size())
                         .arg(cell.constraint.size()));
        if (cell.safe.declared) {
            QStringList parts
                = {QString("input_enable %1").arg(cell.safe.inputEnable),
                   QString("output_value %1").arg(cell.safe.outputValue),
                   QString("output_enable %1").arg(cell.safe.outputEnable),
                   QString("pull %1").arg(
                       cell.safe.pull.empty() ? QStringLiteral("none")
                       : cell.safe.pull.strength.isEmpty()
                           ? cell.safe.pull.mode
                           : cell.safe.pull.mode + " " + cell.safe.pull.strength)};
            for (const QSocPadControlPlan &item : cell.control) {
                parts.append(QString("%1 %2").arg(
                    item.name,
                    cell.safe.control.value(item.name, item.row.at(item.defaultRow).label)));
            }
            lines.append(QString("safe: %1, forced by pad_force_i above every register and slot")
                             .arg(parts.join(", ")));
        }
        /* The numbering is the software contract of pin_pad_ctrl. */
        const QSocPadEncoding encoding = padEncoding(cell, plan.padModel);
        if (encoding.hasPull()) {
            lines.append(QString("pull modes: %1").arg(encoding.modeSummary()));
            const auto strengths = [&](const char *name, const QList<QSocPadTableRow> &rows) {
                if (rows.size() < 2) {
                    return;
                }
                QStringList codes;
                for (qsizetype index = 0; index < rows.size(); ++index) {
                    codes.append(QString("%1 %2").arg(index).arg(rows.at(index).label));
                }
                lines.append(QString("%1 strengths: %2").arg(QString(name), codes.join(", ")));
            };
            strengths("up", encoding.upRows);
            strengths("down", encoding.downRows);
        }
        for (const QSocPadEncoding::Control &item : encoding.control) {
            if (item.cellIndex < 0) {
                continue;
            }
            QStringList codes;
            for (qsizetype row = 0; row < item.label.size(); ++row) {
                codes.append(QString("%1 %2").arg(row).arg(item.label.at(row)));
            }
            lines.append(QString("control %1: %2, default %3")
                             .arg(item.name, codes.join(", "), item.label.at(item.defaultCode)));
        }
    }
    lines.append(QString());

    qsizetype routeIndex = 0;
    for (quint32 pin = 0; pin < plan.pinCount; ++pin) {
        lines.append(
            QString("pin %1 selector word %2 lsb %3 offset 0x%4")
                .arg(pin)
                .arg(pin / lanes)
                .arg((pin % lanes) * kSelectorLane)
                .arg(QString::number(registers.at(identityCount + pin / lanes).byteOffset, 16))
            + (plan.padCells.size() > 1 ? QString(" cell %1").arg(plan.padClass(pin).name)
                                        : QString()));
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
            const auto pullText = [](const QSocIomuxPullRequest &request) {
                return request.strength.isEmpty()
                           ? request.mode
                           : QString("%1 %2").arg(request.mode, request.strength);
            };
            const auto linkText = [](const QSocIomuxEndpointPlan &link) {
                QString text = QString("link %1").arg(link.link);
                if (link.bit.has_value()) {
                    text += QString(" bit %1").arg(*link.bit);
                }
                if (link.invert) {
                    text += " invert";
                }
                return text;
            };
            if (!route.pullMode.isEmpty()) {
                lines.append(
                    QString("    pull: %1").arg(pullText({route.pullMode, route.pullStrength})));
            } else if (route.pullSelect.linked()) {
                lines.append(QString("    pull: %1 on %2 off %3")
                                 .arg(
                                     linkText(route.pullSelect.link),
                                     pullText(route.pullSelect.on),
                                     pullText(route.pullSelect.off)));
            }
            for (auto it = route.control.cbegin(); it != route.control.cend(); ++it) {
                if (it.value().select.linked()) {
                    lines.append(QString("    control %1: %2 on %3 off %4")
                                     .arg(
                                         it.key(),
                                         linkText(it.value().select.link),
                                         it.value().select.on.mode,
                                         it.value().select.off.mode));
                } else {
                    lines.append(QString("    control %1: %2").arg(it.key(), it.value().row));
                }
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
