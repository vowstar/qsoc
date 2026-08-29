// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsociomuxgenerator.h"

#include "common/qsocmodulemanager.h"
#include "common/qsocverilogutils.h"

#include <algorithm>
#include <array>
#include <limits>
#include <QRegularExpression>
#include <QSet>

namespace {

const QSet<QString> kGeneratorKeys
    = {"kind", "bus", "data_width", "address_width", "pin_count", "hs_slots", "integration", "route"};
const QSet<QString> kIntegrationKeys = {"instance", "clock", "reset", "control", "pad"};
const QSet<QString> kPadKeys = {"input_value", "input_enable", "output_value", "output_enable"};
const QSet<QString> kRouteKeys
    = {"pin",
       "slot",
       "function",
       "signal",
       "input_value",
       "input_enable",
       "output_value",
       "output_enable"};
const QSet<QString> kEndpointKeys = {"link", "bit", "invert"};

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
    const YAML::Node &node, QSocIomuxIntegrationPlan *integration, QStringList *errors)
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

    if (!generator["integration"]) {
        appendError(errors, "REQUIRED", "generator.integration", "property is required");
        valid = false;
    } else {
        valid = parseIntegration(generator["integration"], &plan->integration, errors) && valid;
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

QList<QSocMmioPortDescription> publicPortDescriptions(const QSocIomuxPlan &plan)
{
    QList<QSocMmioPortDescription> ports;
    for (const QSocMmioPortDescription &port : QSocMmioGenerator::describePorts(plan.mmio)) {
        if (isControlPort(port)) {
            ports.append(port);
        }
    }
    ports.append({"pad_input_value_i", "input", plan.pinCount});
    ports.append({"pad_input_enable_o", "output", plan.pinCount});
    ports.append({"pad_output_value_o", "output", plan.pinCount});
    ports.append({"pad_output_enable_o", "output", plan.pinCount});
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
        lines.append(QString("assign pad_input_enable_o[%1]  = tx_bundle_%1[2];").arg(pin));
        lines.append(QString("assign pad_output_value_o[%1]  = tx_bundle_%1[1];").arg(pin));
        lines.append(QString("assign pad_output_enable_o[%1] = tx_bundle_%1[0];").arg(pin));
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
        const QString keyword               = port.direction == "output" ? QStringLiteral("output")
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
    for (quint32 pin = 0; pin < plan.pinCount; ++pin) {
        lines.append(QString("wire %1 pin_%2_select_w;").arg(vectorRange(width)).arg(pin));
    }
    lines.append(QString());

    QStringList regsConnections;
    for (const QSocMmioPortDescription &port : regsPorts) {
        if (isControlPort(port)) {
            regsConnections.append(QString("    .%1(%1)").arg(port.name));
        }
    }
    for (quint32 pin = 0; pin < plan.pinCount; ++pin) {
        regsConnections.append(QString("    .pin_%1_select_o(pin_%1_select_w)").arg(pin));
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

QString QSocIomuxGenerator::generateFileList(const QSocIomuxPlan &plan)
{
    if (plan.pinCount == 0 || plan.hsSlots == 0) {
        return QString();
    }
    return QString("%1_regs.v\n%1_conn.v\n%1.v\n").arg(plan.moduleName);
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
    lines.append("      pad_input_value_i:");
    lines.append(QString("        link: %1").arg(integration.padInputValue));
    lines.append("      pad_input_enable_o:");
    lines.append(QString("        link: %1").arg(integration.padInputEnable));
    lines.append("      pad_output_value_o:");
    lines.append(QString("        link: %1").arg(integration.padOutputValue));
    lines.append("      pad_output_enable_o:");
    lines.append(QString("        link: %1").arg(integration.padOutputEnable));
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
    if (registers.size() != qsizetype(1) + selectorWordCount(plan.pinCount, dataWidth)) {
        return QString();
    }
    const quint32 byteCount       = dataWidth / 8;
    const quint32 lanes           = pinsPerWord(dataWidth);
    const quint32 width           = selectorWidth(plan.hsSlots);
    const quint64 aperture        = registers.constLast().byteOffset + byteCount;
    const quint32 capabilityValue = plan.pinCount | (plan.hsSlots << 16);

    QStringList lines;
    lines.append(QString("IOMUX route report for %1").arg(plan.moduleName));
    lines.append(QString("pin_count: %1").arg(plan.pinCount));
    lines.append(QString("hs_slots: %1").arg(plan.hsSlots));
    lines.append(QString("data_width: %1").arg(dataWidth));
    lines.append(QString("address_width: %1").arg(plan.mmio.addressWidth));
    lines.append(QString("selector: %1-bit field in a fixed %2-bit lane per pin")
                     .arg(width)
                     .arg(kSelectorLane));
    lines.append(QString("selector registers: %1 at offset 0x%2 to 0x%3")
                     .arg(registers.size() - 1)
                     .arg(QString::number(registers.at(1).byteOffset, 16))
                     .arg(QString::number(registers.constLast().byteOffset, 16)));
    lines.append(QString("registers total: %1").arg(registers.size()));
    lines.append(QString("aperture: %1 bytes").arg(aperture));
    lines.append(
        QString("capability: 0x%1 at offset 0x0").arg(capabilityValue, 8, 16, QLatin1Char('0')));
    lines.append("reset: every selector resets to 0 and selects slot 0");
    lines.append("rx: pad input broadcasts to every declared sink regardless of the selector");
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
