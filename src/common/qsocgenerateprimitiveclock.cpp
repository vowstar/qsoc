#include "qsocgenerateprimitiveclock.h"
#include "common/qsoccelltext.h"
#include "common/qsocconsole.h"
#include "common/qsocprimitivereport.h"
#include "qsocgeneratemanager.h"
#include "qsocverilogutils.h"
#include <cmath>
#include <QHash>
#include <QSet>

/**
 * Clock controller generator.
 *
 * Port contract: one name is one port. Exact-ABI input reuse shares a
 * single declaration; any output collision or shape mismatch rejects
 * the controller before anything is written. Control constants stay
 * inline in the RTL and never become ports.
 */

namespace {

/* Above this the raw mux template's width ternary stops widening, so the
   upper half of the tree becomes unreachable. */
constexpr qsizetype kMaxRawMuxInputs = 4096;

enum class ControlAtom {
    Empty,
    Identifier,
    Constant,
    Invalid,
};

ControlAtom classifyControlAtom(const QString &value)
{
    if (value.isEmpty()) {
        return ControlAtom::Empty;
    }
    if (QSocVerilogUtils::isValidVerilogIdentifier(value)) {
        return ControlAtom::Identifier;
    }
    if (value == "1'b0" || value == "1'b1") {
        return ControlAtom::Constant;
    }
    return ControlAtom::Invalid;
}

int clockSelectWidth(qsizetype inputCount)
{
    int       width    = 1;
    qsizetype capacity = 2;
    while (capacity < inputCount) {
        capacity *= 2;
        ++width;
    }
    return width;
}

bool validateMapKeys(const YAML::Node &node, const QSet<QString> &allowedKeys, const QString &context)
{
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (!it->first.IsScalar()) {
            QSocConsole::error() << context << "contains a non-scalar property";
            return false;
        }
        const QString key = QString::fromStdString(it->first.as<std::string>());
        if (!allowedKeys.contains(key)) {
            QSocConsole::error() << context << "contains unsupported property" << key;
            return false;
        }
    }
    return true;
}

/* An STA guide in series: the named foundry cell, or the qsoc_ck_buf role. */
void emitStaGuide(
    QTextStream                             &out,
    const QSocClockPrimitive::ClockSTAGuide &guide,
    const QString                           &instance,
    const QString                           &input,
    const QString                           &output)
{
    const bool    role = guide.cell.isEmpty();
    const QString in   = role ? QStringLiteral("clk_in") : guide.in;
    const QString to   = role ? QStringLiteral("clk_out") : guide.out;
    out << "    " << (role ? QStringLiteral("qsoc_ck_buf") : guide.cell) << " " << instance
        << " (\n";
    out << "        ." << in << "(" << input << "),\n";
    out << "        ." << to << "(" << output << ")\n";
    out << "    );\n";
}

bool parseInverterStaGuide(
    const YAML::Node &node, QSocClockPrimitive::ClockSTAGuide &guide, const QString &context)
{
    if (!node.IsMap()) {
        QSocConsole::error() << context << "must be a map";
        return false;
    }
    if (!validateMapKeys(node, {"cell", "in", "out", "instance"}, context)) {
        return false;
    }

    if (!node["cell"]) {
        if (node["in"] || node["out"]) {
            QSocConsole::error() << context << "in and out need cell";
            return false;
        }
    }
    const QStringList requiredKeys = node["cell"] ? QStringList{"cell", "in", "out"}
                                                  : QStringList{};
    for (const QString &key : requiredKeys) {
        const YAML::Node value = node[key.toStdString()];
        if (!value || !value.IsScalar() || value.as<std::string>().empty()) {
            QSocConsole::error() << context << "requires cell, in, and out";
            return false;
        }
        const QString identifier = QString::fromStdString(value.as<std::string>());
        if (!QSocVerilogUtils::isValidVerilogIdentifier(identifier)) {
            QSocConsole::error() << context << key << "must be a valid Verilog identifier";
            return false;
        }
    }
    if (node["instance"]
        && (!node["instance"].IsScalar() || node["instance"].as<std::string>().empty())) {
        QSocConsole::error() << context << "instance must be a non-empty scalar";
        return false;
    }
    if (node["instance"]
        && !QSocVerilogUtils::isValidVerilogIdentifier(
            QString::fromStdString(node["instance"].as<std::string>()))) {
        QSocConsole::error() << context << "instance must be a valid Verilog identifier";
        return false;
    }

    guide.configured = true;
    if (node["cell"]) {
        guide.cell = QString::fromStdString(node["cell"].as<std::string>());
        guide.in   = QString::fromStdString(node["in"].as<std::string>());
        guide.out  = QString::fromStdString(node["out"].as<std::string>());
    }
    if (node["instance"]) {
        guide.instance = QString::fromStdString(node["instance"].as<std::string>());
    }
    return true;
}

bool parseInverter(
    const YAML::Node &node, QSocClockPrimitive::ClockInverter &inverter, const QString &context)
{
    if (node.IsNull()) {
        inverter.configured = true;
        return true;
    }
    if (node.IsScalar()) {
        try {
            inverter.configured = node.as<bool>();
            return true;
        } catch (const YAML::Exception &) {
            QSocConsole::error() << context << "must be empty, a map, or a boolean";
            return false;
        }
    }
    if (!node.IsMap()) {
        QSocConsole::error() << context << "must be empty, a map, or a boolean";
        return false;
    }
    if (!validateMapKeys(node, {"enabled", "sta_guide"}, context)) {
        return false;
    }

    inverter.configured = true;
    if (node["enabled"]) {
        if (!node["enabled"].IsScalar()) {
            QSocConsole::error() << context << "enabled must be a boolean";
            return false;
        }
        try {
            inverter.configured = node["enabled"].as<bool>();
        } catch (const YAML::Exception &) {
            QSocConsole::error() << context << "enabled must be a boolean";
            return false;
        }
    }
    if (node["sta_guide"]
        && !parseInverterStaGuide(node["sta_guide"], inverter.sta_guide, context + ".sta_guide")) {
        return false;
    }
    return true;
}

/* Targets sharing one select port declare it at the widest width, so a
   narrower target reads the low bits instead of the whole port. */
QString clockSelectExpression(const QSocClockPrimitive::ClockTarget &target)
{
    if (target.select_width >= target.select_port_width) {
        return target.select;
    }
    if (target.select_width == 1) {
        return target.select + "[0]";
    }
    return QString("%1[%2:0]").arg(target.select).arg(target.select_width - 1);
}

struct PortShape
{
    bool isInput;
    int  width;
    bool packed;
};

/* Builds config.ports, the port roster used by the header.
   One name is one port: exact-ABI input reuse shares a declaration, any
   output collision or shape mismatch rejects the controller. Control
   constants (1'b0/1'b1) stay inline in the RTL; anything else that is
   not a plain identifier rejects. Walks in header declaration order. */
bool buildClockPortPlan(QSocClockPrimitive::ClockControllerConfig &config)
{
    config.ports.clear();
    QHash<QString, PortShape> claims;

    const auto claimShape =
        [&claims,
         &config](const QString &name, bool isInput, int width, bool packed, const QString &comment) {
            const auto found = claims.constFind(name);
            if (found == claims.cend()) {
                claims.insert(name, {isInput, width, packed});
                config.ports.append({name, isInput, width, packed, comment});
                return true;
            }
            const PortShape &first = found.value();
            if (first.isInput != isInput) {
                QSocConsole::error()
                    << "Clock controller port" << name << "is declared as both input and output";
                return false;
            }
            if (!first.isInput) {
                QSocConsole::error()
                    << "Clock controller port" << name << "is driven by two outputs";
                return false;
            }
            if (first.width != width) {
                QSocConsole::error() << "Clock controller port" << name << "has incompatible widths"
                                     << first.width << "and" << width;
                return false;
            }
            if (first.packed != packed) {
                QSocConsole::error() << "Clock controller port" << name
                                     << "has incompatible scalar and packed declarations";
                return false;
            }
            return true;
        };

    /* Signals that must name a real port: clocks, outputs, and the
       division value. */
    const auto claimIdentifier =
        [&claimShape](
            const QString &name, bool isInput, int width, bool packed, const QString &comment) {
            if (name.isEmpty()) {
                return true;
            }
            if (classifyControlAtom(name) != ControlAtom::Identifier) {
                QSocConsole::error()
                    << "Clock controller port" << name << "must be a plain identifier";
                return false;
            }
            return claimShape(name, isInput, width, packed, comment);
        };

    /* Input controls the RTL can inline: 1'b0/1'b1 form no port. */
    const auto claimControl =
        [&claimShape](const QString &name, int width, bool packed, const QString &comment) {
            switch (classifyControlAtom(name)) {
            case ControlAtom::Empty:
            case ControlAtom::Constant:
                return true;
            case ControlAtom::Invalid:
                QSocConsole::error() << "Clock control signal" << name
                                     << "must be an identifier or a 1'b0/1'b1 constant";
                return false;
            case ControlAtom::Identifier:
                break;
            }
            return claimShape(name, true, width, packed, comment);
        };

    for (const auto &input : config.inputs) {
        QString comment = QString("/**< Clock input: %1").arg(input.name);
        if (!input.freq.isEmpty()) {
            comment += QString(" (%1)").arg(input.freq);
        }
        comment += " */";
        if (!claimIdentifier(input.name, true, 1, false, comment)) {
            return false;
        }
    }
    for (const auto &target : config.targets) {
        QString comment = QString("/**< Clock target: %1").arg(target.name);
        if (!target.freq.isEmpty()) {
            comment += QString(" (%1)").arg(target.freq);
        }
        comment += " */";
        if (!claimIdentifier(target.name, false, 1, false, comment)) {
            return false;
        }
    }

    for (const auto &target : config.targets) {
        if (!target.div.configured) {
            continue;
        }
        if (!claimIdentifier(
                target.div.value,
                true,
                target.div.width,
                true,
                QString("/**< Dynamic division value for %1 */").arg(target.name))) {
            return false;
        }
        /* A unity divider (no ratio, no value) ties div_valid to 1'b0;
           only an active divider keeps the historical valid port. */
        if ((target.div.default_value > 1 || !target.div.value.isEmpty())
            && !claimControl(
                target.div.valid,
                1,
                false,
                QString("/**< Division valid signal for %1 */").arg(target.name))) {
            return false;
        }
        if (!claimIdentifier(
                target.div.ready,
                false,
                1,
                false,
                QString("/**< Division ready signal for %1 */").arg(target.name))) {
            return false;
        }
        if (!claimIdentifier(
                target.div.count,
                false,
                target.div.width,
                true,
                QString("/**< Cycle counter for %1 */").arg(target.name))) {
            return false;
        }
        if (!claimControl(
                target.div.enable,
                1,
                false,
                QString("/**< Division enable for %1 */").arg(target.name))) {
            return false;
        }
    }
    for (const auto &target : config.targets) {
        for (const auto &link : target.links) {
            if (!(link.div.default_value > 1 || !link.div.value.isEmpty())) {
                continue;
            }
            const QString linkName = QString("%1_from_%2").arg(target.name, link.source);
            if (!claimIdentifier(
                    link.div.value,
                    true,
                    link.div.width,
                    true,
                    QString("/**< Dynamic division value for link %1 */").arg(linkName))) {
                return false;
            }
            if (!claimControl(
                    link.div.valid,
                    1,
                    false,
                    QString("/**< Division valid signal for link %1 */").arg(linkName))) {
                return false;
            }
            if (!claimIdentifier(
                    link.div.ready,
                    false,
                    1,
                    false,
                    QString("/**< Division ready signal for link %1 */").arg(linkName))) {
                return false;
            }
            if (!claimIdentifier(
                    link.div.count,
                    false,
                    link.div.width,
                    true,
                    QString("/**< Cycle counter for link %1 */").arg(linkName))) {
                return false;
            }
            if (!claimControl(
                    link.div.enable,
                    1,
                    false,
                    QString("/**< Division enable for link %1 */").arg(linkName))) {
                return false;
            }
        }
    }

    if (!claimControl(config.testEnable, 1, false, QString("/**< Test enable signal */"))) {
        return false;
    }
    for (const auto &target : config.targets) {
        if (!target.test_clock.isEmpty()
            && !claimControl(
                target.test_enable,
                1,
                false,
                QString("/**< Test enable for %1 */").arg(target.name))) {
            return false;
        }
    }
    for (const auto &target : config.targets) {
        if (!claimControl(
                target.icg.enable, 1, false, QString("/**< ICG enable for %1 */").arg(target.name))
            || !claimControl(
                target.icg.reset, 1, false, QString("/**< ICG reset for %1 */").arg(target.name))) {
            return false;
        }
    }
    for (const auto &target : config.targets) {
        for (const auto &link : target.links) {
            const QString linkName = QString("%1_from_%2").arg(target.name, link.source);
            if (!claimControl(
                    link.icg.enable,
                    1,
                    false,
                    QString("/**< Link ICG enable for %1 */").arg(linkName))
                || !claimControl(
                    link.icg.reset,
                    1,
                    false,
                    QString("/**< Link ICG reset for %1 */").arg(linkName))) {
                return false;
            }
        }
    }
    for (const auto &target : config.targets) {
        if (target.links.size() < 2) {
            continue;
        }
        if (!claimControl(
                target.select,
                target.select_port_width,
                target.select_port_width > 1,
                QString("/**< MUX select for %1 */").arg(target.name))) {
            return false;
        }
        if (!claimControl(
                target.reset, 1, false, QString("/**< MUX reset for %1 */").arg(target.name))) {
            return false;
        }
        if (!claimControl(
                target.test_clock,
                1,
                false,
                QString("/**< MUX test clock for %1 */").arg(target.name))) {
            return false;
        }
    }
    for (const auto &target : config.targets) {
        if (target.div.configured
            && !claimControl(
                target.div.reset,
                1,
                false,
                QString("/**< Division reset for %1 */").arg(target.name))) {
            return false;
        }
    }
    for (const auto &target : config.targets) {
        for (const auto &link : target.links) {
            if ((link.div.default_value > 1 || !link.div.value.isEmpty())
                && !claimControl(
                    link.div.reset,
                    1,
                    false,
                    QString("/**< Link division reset for %1 */")
                        .arg(QString("%1_from_%2").arg(target.name, link.source)))) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

QSocClockPrimitive::QSocClockPrimitive(QSocGenerateManager *parent)
    : m_parent(parent)
{}

bool QSocClockPrimitive::generateClockController(const YAML::Node &clockNode, QTextStream &out)
{
    if (!clockNode || !clockNode.IsMap()) {
        QSocConsole::warn() << "Invalid clock node provided";
        return false;
    }

    // Parse configuration
    ClockControllerConfig config = parseClockConfig(clockNode);

    if (config.inputs.isEmpty() || config.targets.isEmpty()) {
        QSocConsole::warn() << "Clock configuration must have at least one input and target";
        return false;
    }

    if (!config.valid) {
        QSocConsole::error() << "Clock controller" << config.name
                             << "configuration rejected; nothing generated";
        return false;
    }

    /* By design, a link source that is neither a declared input nor
       another target gets auto-promoted to a fresh input port on the
       controller (so the parent can wire software-controlled signals
       like `rst_sw_dcmi_n`). Do NOT warn about undeclared sources here;
       the typo case surfaces downstream as an unwired controller pin. */

    out << generateControllerVerilog(config);

    if (m_parent && !m_parent->getDiagramDirectory().isEmpty()) {
        QSocPrimitiveReport::stage(
            m_parent->getDiagramDirectory(), config.moduleName, QSocPrimitiveReport::clock(config));
    }

    return true;
}

QString QSocClockPrimitive::generateControllerVerilog(const ClockControllerConfig &config)
{
    if (!config.valid || config.inputs.isEmpty() || config.targets.isEmpty()) {
        return {};
    }
    QString     verilog;
    QTextStream out(&verilog);
    generateModuleHeader(config, out);
    generateWireDeclarations(config, out);
    generateClockLogic(config, out);
    generateOutputAssignments(config, out);

    out << "\nendmodule\n\n";

    out.flush();
    return verilog;
}

QSocClockPrimitive::ClockControllerConfig QSocClockPrimitive::parseClockConfig(
    const YAML::Node &clockNode)
{
    /* A malformed shape reaches yaml-cpp as an exception, and an uncaught one
       aborts the process instead of reporting the user's configuration. */
    try {
        return parseClockConfigUnguarded(clockNode);
    } catch (const YAML::Exception &error) {
        ClockControllerConfig config;
        config.valid = false;
        QSocConsole::error() << "Invalid clock configuration:" << error.what();
        return config;
    }
}

QSocClockPrimitive::ClockControllerConfig QSocClockPrimitive::parseClockConfigUnguarded(
    const YAML::Node &clockNode)
{
    ClockControllerConfig config;

    // Parse basic properties
    if (!clockNode["name"]) {
        QSocConsole::error() << "'name' field is required in clock configuration";
        QSocConsole::err() << "Example: clock: { name: my_clk_ctrl, ... }" << "\n";
        return config;
    }
    config.name       = QString::fromStdString(clockNode["name"].as<std::string>());
    config.moduleName = config.name; // Use same name for module

    // Test enable is optional - if not set, tie to 1'b0 internally
    if (clockNode["test_enable"]) {
        config.testEnable = QString::fromStdString(clockNode["test_enable"].as<std::string>());
    }
    if (classifyControlAtom(config.testEnable) == ControlAtom::Invalid) {
        QSocConsole::error() << "Clock controller" << config.name
                             << "test_enable must be a Verilog identifier, 1'b0, or 1'b1";
        config.valid = false;
    }

    // Optional ref_clock for GF_MUX
    if (clockNode["ref_clock"]) {
        config.ref_clock = QString::fromStdString(clockNode["ref_clock"].as<std::string>());
    }

    // Parse clock inputs
    if (clockNode["input"] && clockNode["input"].IsMap()) {
        for (auto it = clockNode["input"].begin(); it != clockNode["input"].end(); ++it) {
            ClockInput    input;
            const QString rawName = QString::fromStdString(it->first.as<std::string>());
            /* Bracket characters in a primitive identifier name leak straight
               into wire/instance/port identifiers and produce illegal
               Verilog. Sanitize the name here and warn the user. */
            input.name = QSocVerilogUtils::sanitizeBitSelectInName(rawName);
            if (input.name != rawName) {
                QSocConsole::warn() << "Clock input name" << rawName
                                    << "contains bracket characters; sanitized to" << input.name;
            }

            if (it->second.IsMap()) {
                if (it->second["freq"]) {
                    input.freq = QString::fromStdString(it->second["freq"].as<std::string>());
                }
                if (it->second["duty"]) {
                    input.duty = QString::fromStdString(it->second["duty"].as<std::string>());
                }
                if (it->second["comment"] && it->second["comment"].IsScalar())
                    input.comment = QString::fromStdString(it->second["comment"].as<std::string>());
            }
            for (const auto &existing : config.inputs) {
                if (existing.name == input.name) {
                    QSocConsole::error()
                        << "Duplicate clock input name after sanitization:" << input.name;
                    config.valid = false;
                }
            }
            config.inputs.append(input);
        }
    }

    // Parse clock targets
    if (clockNode["target"] && clockNode["target"].IsMap()) {
        for (auto it = clockNode["target"].begin(); it != clockNode["target"].end(); ++it) {
            ClockTarget   target;
            const QString rawName = QString::fromStdString(it->first.as<std::string>());
            target.name           = QSocVerilogUtils::sanitizeBitSelectInName(rawName);
            if (target.name != rawName) {
                QSocConsole::warn() << "Clock target name" << rawName
                                    << "contains bracket characters; sanitized to" << target.name;
            }

            /* A scalar or sequence here indexes as a map further down and
               takes the process with it. */
            if (!it->second.IsMap()) {
                QSocConsole::error() << "Clock target" << target.name << "must be a map";
                config.valid = false;
                continue;
            }

            if (it->second["freq"]) {
                target.freq = QString::fromStdString(it->second["freq"].as<std::string>());
            }
            if (it->second["comment"] && it->second["comment"].IsScalar())
                target.comment = QString::fromStdString(it->second["comment"].as<std::string>());

            // Parse target-level ICG
            if (it->second["icg"] && it->second["icg"].IsMap()) {
                target.icg.configured = true; // ICG block exists in YAML
                if (it->second["icg"]["enable"]) {
                    target.icg.enable = QString::fromStdString(
                        it->second["icg"]["enable"].as<std::string>());
                }
                target.icg.polarity = QString::fromStdString(
                    it->second["icg"]["polarity"].as<std::string>("high"));
                target.icg.test_enable = config.testEnable; // Use controller-level test_enable
                if (it->second["icg"]["reset"]) {
                    target.icg.reset = QString::fromStdString(
                        it->second["icg"]["reset"].as<std::string>());
                }
                target.icg.clock_on_reset = it->second["icg"]["clock_on_reset"].as<bool>(false);
                // Parse ICG sta_guide
                if (it->second["icg"]["sta_guide"] && it->second["icg"]["sta_guide"].IsMap()) {
                    target.icg.sta_guide.configured = true;
                    if (it->second["icg"]["sta_guide"]["cell"]) {
                        target.icg.sta_guide.cell = QString::fromStdString(
                            it->second["icg"]["sta_guide"]["cell"].as<std::string>());
                    }
                    if (it->second["icg"]["sta_guide"]["in"]) {
                        target.icg.sta_guide.in = QString::fromStdString(
                            it->second["icg"]["sta_guide"]["in"].as<std::string>());
                    }
                    if (it->second["icg"]["sta_guide"]["out"]) {
                        target.icg.sta_guide.out = QString::fromStdString(
                            it->second["icg"]["sta_guide"]["out"].as<std::string>());
                    }
                    if (it->second["icg"]["sta_guide"]["instance"]) {
                        target.icg.sta_guide.instance = QString::fromStdString(
                            it->second["icg"]["sta_guide"]["instance"].as<std::string>());
                    }
                }
                if (target.icg.enable.isEmpty()) {
                    QSocConsole::error() << "Clock target" << target.name << "icg requires enable";
                    config.valid = false;
                }
            }

            // Parse target-level divider
            if (it->second["div"] && it->second["div"].IsMap()) {
                target.div.configured = true; // DIV block exists in YAML
                // Clean field names only
                target.div.default_value  = it->second["div"]["default"].as<int>(1);
                target.div.clock_on_reset = it->second["div"]["clock_on_reset"].as<bool>(false);

                // Check if dynamic mode (has value signal)
                bool hasDynamicControl = it->second["div"]["value"]
                                         && !it->second["div"]["value"].as<std::string>().empty();

                if (hasDynamicControl) {
                    // Dynamic mode: width is required
                    target.div.width = it->second["div"]["width"].as<int>(0);
                    if (target.div.width <= 0) {
                        /* An invented width is a changed port contract. */
                        QSocConsole::error() << "Dynamic divider for target"
                                             << QString::fromStdString(it->first.as<std::string>())
                                             << "requires explicit width specification";
                        config.valid = false;
                    }
                } else {
                    // Static mode: calculate width from default value
                    target.div.width = static_cast<int>(
                        std::ceil(std::log2(std::max(target.div.default_value + 1, 2))));
                    // Override if explicitly specified (for manual control)
                    if (it->second["div"]["width"]) {
                        target.div.width = it->second["div"]["width"].as<int>(target.div.width);
                    }
                }
                /* Synthesis ignores the initial-block guard, so an oversized
                   default silently changes the division ratio in silicon. */
                if (target.div.width > 0 && target.div.width < 31
                    && target.div.default_value > (1 << target.div.width) - 1) {
                    QSocConsole::error()
                        << "Default value" << target.div.default_value << "for target"
                        << QString::fromStdString(it->first.as<std::string>())
                        << "exceeds maximum value" << ((1 << target.div.width) - 1) << "for width"
                        << target.div.width << "bits";
                    config.valid = false;
                }

                if (it->second["div"]["reset"]) {
                    target.div.reset = QString::fromStdString(
                        it->second["div"]["reset"].as<std::string>());
                }
                if (it->second["div"]["enable"]) {
                    target.div.enable = QString::fromStdString(
                        it->second["div"]["enable"].as<std::string>());
                }
                target.div.test_enable = config.testEnable; // Use controller-level test_enable

                // Clean field names - no legacy support
                if (it->second["div"]["value"]) {
                    target.div.value = QString::fromStdString(
                        it->second["div"]["value"].as<std::string>());
                }
                if (it->second["div"]["valid"]) {
                    target.div.valid = QString::fromStdString(
                        it->second["div"]["valid"].as<std::string>());
                }
                if (it->second["div"]["ready"]) {
                    target.div.ready = QString::fromStdString(
                        it->second["div"]["ready"].as<std::string>());
                }
                if (it->second["div"]["count"]) {
                    target.div.count = QString::fromStdString(
                        it->second["div"]["count"].as<std::string>());
                }
                // Parse DIV sta_guide
                if (it->second["div"]["sta_guide"] && it->second["div"]["sta_guide"].IsMap()) {
                    target.div.sta_guide.configured = true;
                    if (it->second["div"]["sta_guide"]["cell"]) {
                        target.div.sta_guide.cell = QString::fromStdString(
                            it->second["div"]["sta_guide"]["cell"].as<std::string>());
                    }
                    if (it->second["div"]["sta_guide"]["in"]) {
                        target.div.sta_guide.in = QString::fromStdString(
                            it->second["div"]["sta_guide"]["in"].as<std::string>());
                    }
                    if (it->second["div"]["sta_guide"]["out"]) {
                        target.div.sta_guide.out = QString::fromStdString(
                            it->second["div"]["sta_guide"]["out"].as<std::string>());
                    }
                    if (it->second["div"]["sta_guide"]["instance"]) {
                        target.div.sta_guide.instance = QString::fromStdString(
                            it->second["div"]["sta_guide"]["instance"].as<std::string>());
                    }
                }
            }

            // Parse target-level inverter
            const YAML::Node targetInvNode = it->second["inv"];
            if (targetInvNode
                && !parseInverter(
                    targetInvNode, target.inv, QString("Clock target %1 inv").arg(target.name))) {
                config.valid = false;
            }

            // Parse links
            if (it->second["link"] && it->second["link"].IsMap()) {
                for (auto linkIt = it->second["link"].begin(); linkIt != it->second["link"].end();
                     ++linkIt) {
                    ClockLink     link;
                    const QString rawSource = QString::fromStdString(
                        linkIt->first.as<std::string>());
                    link.source = QSocVerilogUtils::sanitizeBitSelectInName(rawSource);
                    if (link.source != rawSource) {
                        QSocConsole::warn()
                            << "Clock link source name" << rawSource
                            << "contains bracket characters; sanitized to" << link.source;
                    }

                    // Link-level inverter
                    const QString linkContext = QString("Clock link %1").arg(rawSource);
                    if (linkIt->second.IsScalar()) {
                        const QString operation = QString::fromStdString(
                            linkIt->second.as<std::string>());
                        if (operation == "inv") {
                            link.inv.configured = true;
                        } else {
                            QSocConsole::error()
                                << linkContext << "uses unsupported scalar operation" << operation;
                            config.valid = false;
                        }
                    } else if (!linkIt->second.IsNull() && !linkIt->second.IsMap()) {
                        QSocConsole::error()
                            << linkContext << "must be empty, the scalar 'inv', or a map";
                        config.valid = false;
                    } else if (linkIt->second.IsMap()) {
                        if (!validateMapKeys(linkIt->second, {"icg", "div", "inv"}, linkContext)) {
                            config.valid = false;
                        }
                        if (linkIt->second["icg"] && !linkIt->second["icg"].IsMap()) {
                            QSocConsole::error() << linkContext << "icg must be a map";
                            config.valid = false;
                        }
                        if (linkIt->second["div"] && !linkIt->second["div"].IsMap()) {
                            QSocConsole::error() << linkContext << "div must be a map";
                            config.valid = false;
                        }
                        const YAML::Node linkInvNode = linkIt->second["inv"];
                        if (linkInvNode
                            && !parseInverter(
                                linkInvNode, link.inv, QString("%1 inv").arg(linkContext))) {
                            config.valid = false;
                        }
                    }

                    // Link-level ICG configuration
                    if (linkIt->second.IsMap() && linkIt->second["icg"]
                        && linkIt->second["icg"].IsMap()) {
                        link.icg.configured = true; // ICG block exists in YAML
                        if (linkIt->second["icg"]["enable"]) {
                            link.icg.enable = QString::fromStdString(
                                linkIt->second["icg"]["enable"].as<std::string>());
                        }
                        link.icg.polarity = QString::fromStdString(
                            linkIt->second["icg"]["polarity"].as<std::string>("high"));
                        link.icg.test_enable = config.testEnable; // Use controller-level test_enable
                        if (linkIt->second["icg"]["reset"]) {
                            link.icg.reset = QString::fromStdString(
                                linkIt->second["icg"]["reset"].as<std::string>());
                        }
                        link.icg.clock_on_reset = linkIt->second["icg"]["clock_on_reset"].as<bool>(
                            false);
                        // Parse ICG sta_guide
                        if (linkIt->second["icg"]["sta_guide"]
                            && linkIt->second["icg"]["sta_guide"].IsMap()) {
                            link.icg.sta_guide.configured = true;
                            if (linkIt->second["icg"]["sta_guide"]["cell"]) {
                                link.icg.sta_guide.cell = QString::fromStdString(
                                    linkIt->second["icg"]["sta_guide"]["cell"].as<std::string>());
                            }
                            if (linkIt->second["icg"]["sta_guide"]["in"]) {
                                link.icg.sta_guide.in = QString::fromStdString(
                                    linkIt->second["icg"]["sta_guide"]["in"].as<std::string>());
                            }
                            if (linkIt->second["icg"]["sta_guide"]["out"]) {
                                link.icg.sta_guide.out = QString::fromStdString(
                                    linkIt->second["icg"]["sta_guide"]["out"].as<std::string>());
                            }
                            if (linkIt->second["icg"]["sta_guide"]["instance"]) {
                                link.icg.sta_guide.instance = QString::fromStdString(
                                    linkIt->second["icg"]["sta_guide"]["instance"].as<std::string>());
                            }
                        }
                        if (link.icg.enable.isEmpty()) {
                            QSocConsole::error() << linkContext << "icg requires enable";
                            config.valid = false;
                        }
                    }

                    // Link-level divider configuration - design only
                    if (linkIt->second.IsMap() && linkIt->second["div"]
                        && linkIt->second["div"].IsMap()) {
                        link.div.configured = true; // DIV block exists in YAML
                        // Clean field names only
                        link.div.default_value = linkIt->second["div"]["default"].as<int>(1);

                        // Check if dynamic mode (has value signal)
                        bool hasDynamicControl
                            = linkIt->second["div"]["value"]
                              && !linkIt->second["div"]["value"].as<std::string>().empty();

                        if (hasDynamicControl) {
                            // Dynamic mode: width is required
                            link.div.width = linkIt->second["div"]["width"].as<int>(0);
                            if (link.div.width <= 0) {
                                /* An invented width is a changed port contract. */
                                QSocConsole::error()
                                    << "Dynamic divider for link"
                                    << QString::fromStdString(it->first.as<std::string>()) << "->"
                                    << QString::fromStdString(linkIt->first.as<std::string>())
                                    << "requires explicit width specification";
                                config.valid = false;
                            }
                        } else {
                            // Static mode: calculate width from default value
                            link.div.width = static_cast<int>(
                                std::ceil(std::log2(std::max(link.div.default_value + 1, 2))));
                            // Override if explicitly specified (for manual control)
                            if (linkIt->second["div"]["width"]) {
                                link.div.width = linkIt->second["div"]["width"].as<int>(
                                    link.div.width);
                            }
                        }
                        /* Synthesis ignores the initial-block guard, so an
                           oversized default silently changes the ratio. */
                        if (link.div.width > 0 && link.div.width < 31
                            && link.div.default_value > (1 << link.div.width) - 1) {
                            QSocConsole::error()
                                << "Default value" << link.div.default_value << "for link"
                                << QString::fromStdString(it->first.as<std::string>()) << "->"
                                << QString::fromStdString(linkIt->first.as<std::string>())
                                << "exceeds maximum value" << ((1 << link.div.width) - 1)
                                << "for width" << link.div.width << "bits";
                            config.valid = false;
                        }
                        link.div.clock_on_reset = linkIt->second["div"]["clock_on_reset"].as<bool>(
                            false);

                        if (linkIt->second["div"]["reset"]) {
                            link.div.reset = QString::fromStdString(
                                linkIt->second["div"]["reset"].as<std::string>());
                        }
                        if (linkIt->second["div"]["enable"]) {
                            link.div.enable = QString::fromStdString(
                                linkIt->second["div"]["enable"].as<std::string>());
                        }
                        link.div.test_enable = config.testEnable; // Use controller-level test_enable

                        // Clean field names - no legacy support
                        if (linkIt->second["div"]["value"]) {
                            link.div.value = QString::fromStdString(
                                linkIt->second["div"]["value"].as<std::string>());
                        }
                        if (linkIt->second["div"]["valid"]) {
                            link.div.valid = QString::fromStdString(
                                linkIt->second["div"]["valid"].as<std::string>());
                        }
                        if (linkIt->second["div"]["ready"]) {
                            link.div.ready = QString::fromStdString(
                                linkIt->second["div"]["ready"].as<std::string>());
                        }
                        if (linkIt->second["div"]["count"]) {
                            link.div.count = QString::fromStdString(
                                linkIt->second["div"]["count"].as<std::string>());
                        }
                        // Parse DIV sta_guide
                        if (linkIt->second["div"]["sta_guide"]
                            && linkIt->second["div"]["sta_guide"].IsMap()) {
                            link.div.sta_guide.configured = true;
                            if (linkIt->second["div"]["sta_guide"]["cell"]) {
                                link.div.sta_guide.cell = QString::fromStdString(
                                    linkIt->second["div"]["sta_guide"]["cell"].as<std::string>());
                            }
                            if (linkIt->second["div"]["sta_guide"]["in"]) {
                                link.div.sta_guide.in = QString::fromStdString(
                                    linkIt->second["div"]["sta_guide"]["in"].as<std::string>());
                            }
                            if (linkIt->second["div"]["sta_guide"]["out"]) {
                                link.div.sta_guide.out = QString::fromStdString(
                                    linkIt->second["div"]["sta_guide"]["out"].as<std::string>());
                            }
                            if (linkIt->second["div"]["sta_guide"]["instance"]) {
                                link.div.sta_guide.instance = QString::fromStdString(
                                    linkIt->second["div"]["sta_guide"]["instance"].as<std::string>());
                            }
                        }
                    }

                    for (const auto &existing : target.links) {
                        if (existing.source == link.source) {
                            QSocConsole::error()
                                << "Duplicate clock link source after sanitization:" << link.source;
                            config.valid = false;
                        }
                    }
                    target.links.append(link);
                }
            }

            /* Without a source the output assignment has nothing on its right
               hand side and the emitted controller does not parse. */
            if (target.links.isEmpty()) {
                QSocConsole::error()
                    << "Clock target" << target.name << "requires a non-empty link map";
                config.valid = false;
            }

            // Parse multiplexer configuration (only if ≥2 links) - New format per documentation
            if (target.links.size() >= 2) {
                // Parse target-level MUX signals (new format)
                if (it->second["select"]) {
                    target.select = QString::fromStdString(it->second["select"].as<std::string>());
                }
                if (it->second["reset"]) {
                    target.reset = QString::fromStdString(it->second["reset"].as<std::string>());
                }
                QString requestedTestEnable;
                if (it->second["test_enable"]) {
                    requestedTestEnable = QString::fromStdString(
                        it->second["test_enable"].as<std::string>());
                }
                QString requestedTestClock;
                if (it->second["test_clock"]) {
                    requestedTestClock = QString::fromStdString(
                        it->second["test_clock"].as<std::string>());
                }
                /* test_enable without test_clock has no effect; test_clock
                   requires an explicit or inherited enable. */
                if (!requestedTestEnable.isEmpty() && requestedTestClock.isEmpty()) {
                    QSocConsole::warn() << "Clock target" << target.name
                                        << "test_enable without test_clock has no effect";
                }
                // Auto-select mux type based on reset presence
                if (!target.reset.isEmpty()) {
                    target.mux.type = GF_MUX; // Has reset → Glitch-free mux
                    if (!requestedTestClock.isEmpty()) {
                        target.test_clock  = requestedTestClock;
                        target.test_enable = requestedTestEnable.isEmpty() ? config.testEnable
                                                                           : requestedTestEnable;
                        if (classifyControlAtom(target.test_clock) != ControlAtom::Identifier) {
                            QSocConsole::error() << "Clock target" << target.name
                                                 << "test_clock must be a valid Verilog identifier";
                            config.valid = false;
                        }
                        const ControlAtom enableAtom = classifyControlAtom(target.test_enable);
                        if (enableAtom == ControlAtom::Empty) {
                            QSocConsole::error() << "Clock target" << target.name
                                                 << "test_clock requires test_enable";
                            config.valid = false;
                        } else if (enableAtom == ControlAtom::Invalid) {
                            QSocConsole::error()
                                << "Clock target" << target.name
                                << "test_enable must be a Verilog identifier, 1'b0, or 1'b1";
                            config.valid = false;
                        }
                    }
                } else {
                    target.mux.type = STD_MUX; // No reset → Standard mux
                    if (target.links.size() > kMaxRawMuxInputs) {
                        QSocConsole::error()
                            << "Clock target" << target.name << "STD_MUX supports at most"
                            << kMaxRawMuxInputs << "inputs";
                        config.valid = false;
                    }
                    if (!requestedTestClock.isEmpty()) {
                        QSocConsole::warn() << "Clock target" << target.name
                                            << "test_clock is ignored on a standard mux; add reset "
                                               "for a glitch-free mux with a DFT path";
                    }
                }

                // Parse MUX sta_guide configuration
                if (it->second["mux"] && it->second["mux"]["sta_guide"]) {
                    const YAML::Node muxNode        = it->second["mux"];
                    const YAML::Node staGuideNode   = muxNode["sta_guide"];
                    target.mux.sta_guide.configured = staGuideNode.IsMap();

                    if (staGuideNode["cell"]) {
                        target.mux.sta_guide.cell = QString::fromStdString(
                            staGuideNode["cell"].as<std::string>());
                    }
                    if (staGuideNode["in"]) {
                        target.mux.sta_guide.in = QString::fromStdString(
                            staGuideNode["in"].as<std::string>());
                    }
                    if (staGuideNode["out"]) {
                        target.mux.sta_guide.out = QString::fromStdString(
                            staGuideNode["out"].as<std::string>());
                    }
                    if (staGuideNode["instance"]) {
                        target.mux.sta_guide.instance = QString::fromStdString(
                            staGuideNode["instance"].as<std::string>());
                    }
                }

                // Validation: multi-link requires select signal
                if (target.select.isEmpty()) {
                    QSocConsole::error()
                        << "'select' signal is required for multi-link target:" << target.name;
                    QSocConsole::error()
                        << "Example: target: { link: {clk1: ~, clk2: ~}, select: sel_sig }";
                    config.valid = false;
                    return config;
                }

            } else if (it->second["test_clock"] || it->second["test_enable"]) {
                QSocConsole::warn() << "Clock target" << target.name
                                    << "has a single link; test_clock/test_enable "
                                       "apply only to a multi-link mux and are ignored";
            }

            config.targets.append(target);
        }
    }

    // One select port serves every target that names it, at the widest width
    QHash<QString, int> selectPortWidths;
    for (auto &target : config.targets) {
        if (target.links.size() < 2) {
            continue;
        }
        target.select_width = clockSelectWidth(target.links.size());
        if (target.select_width > selectPortWidths.value(target.select, 0)) {
            selectPortWidths.insert(target.select, target.select_width);
        }
    }
    for (auto &target : config.targets) {
        if (target.links.size() >= 2) {
            target.select_port_width = selectPortWidths.value(target.select);
        }
    }

    QStringList scalarDftControls;
    const auto  addScalarDftControl = [&scalarDftControls](const QString &signal) {
        if (classifyControlAtom(signal) == ControlAtom::Identifier
            && !scalarDftControls.contains(signal)) {
            scalarDftControls.append(signal);
        }
    };
    addScalarDftControl(config.testEnable);
    for (const auto &target : config.targets) {
        addScalarDftControl(target.test_enable);
        addScalarDftControl(target.test_clock);
    }
    for (const QString &signal : scalarDftControls) {
        const int selectWidth = selectPortWidths.value(signal, 0);
        if (selectWidth > 1) {
            QSocConsole::error() << "Clock DFT control" << signal << "conflicts with a"
                                 << selectWidth << "bit mux select port";
            config.valid = false;
        }
    }

    // Check for duplicate target names (output signals)
    QSet<QString> inputNames;
    for (const auto &input : config.inputs) {
        inputNames.insert(input.name);
    }

    QSet<QString> targetNames;
    for (const auto &target : config.targets) {
        if (targetNames.contains(target.name)) {
            QSocConsole::error() << "ERROR: Duplicate output target name:" << target.name;
            QSocConsole::err() << "Each target must have a unique output signal name" << "\n";
            config.valid = false;
        } else {
            targetNames.insert(target.name);
        }
        if (inputNames.contains(target.name)) {
            QSocConsole::error() << "Clock name used as both input and target after sanitization:"
                                 << target.name;
            config.valid = false;
        }
    }

    /* These names become module, port, and instance identifiers verbatim. */
    const auto requireIdentifier = [&config](const QString &name, const QString &role) {
        if (!QSocVerilogUtils::isValidVerilogIdentifier(name)) {
            QSocConsole::error() << role << name << "must be a valid Verilog identifier";
            config.valid = false;
        }
    };
    requireIdentifier(config.name, "Clock controller name");
    for (const auto &input : config.inputs) {
        requireIdentifier(input.name, "Clock input name");
    }
    for (const auto &target : config.targets) {
        requireIdentifier(target.name, "Clock target name");
        if (target.links.size() >= 2) {
            requireIdentifier(target.select, "Clock select signal");
        }
        for (const auto &link : target.links) {
            requireIdentifier(link.source, "Clock link source");
        }
    }

    /* A link source that is neither a declared input nor another target gets
       an input port, so it is driven instead of an implicit net. */
    QSet<QString> knownClocks = inputNames;
    for (const auto &target : config.targets) {
        knownClocks.insert(target.name);
    }
    for (const auto &target : config.targets) {
        for (const auto &link : target.links) {
            if (knownClocks.contains(link.source)) {
                continue;
            }
            knownClocks.insert(link.source);
            ClockInput promoted;
            promoted.name = link.source;
            config.inputs.append(promoted);
        }
    }

    QStringList dftSignals;
    const auto  addDftSignal = [&dftSignals](const QString &signal) {
        if (classifyControlAtom(signal) == ControlAtom::Identifier && !dftSignals.contains(signal)) {
            dftSignals.append(signal);
        }
    };
    addDftSignal(config.testEnable);
    for (const auto &target : config.targets) {
        addDftSignal(target.test_enable);
        addDftSignal(target.test_clock);
    }
    for (const QString &signal : dftSignals) {
        if (targetNames.contains(signal)) {
            QSocConsole::error() << "Clock DFT signal used as both input and target output:"
                                 << signal;
            config.valid = false;
        }
    }

    if (config.valid && !buildClockPortPlan(config)) {
        config.valid = false;
    }

    return config;
}

void QSocClockPrimitive::generateModuleHeader(const ClockControllerConfig &config, QTextStream &out)
{
    out << "\nmodule " << config.moduleName << " (\n";

    for (qsizetype i = 0; i < config.ports.size(); ++i) {
        const bool  isLast = (i == config.ports.size() - 1);
        const auto &port   = config.ports[i];
        out << (port.isInput ? "    input  wire " : "    output wire ");
        if (port.packed) {
            out << "[" << port.width - 1 << ":0] ";
        }
        out << port.name << (isLast ? "" : ",") << "    " << port.comment << "\n";
    }

    out << ");\n\n";
}

void QSocClockPrimitive::generateWireDeclarations(
    const ClockControllerConfig &config, QTextStream &out)
{
    out << "    /* Wire declarations for clock connections */\n";

    for (const auto &target : config.targets) {
        for (int i = 0; i < target.links.size(); ++i) {
            const auto &link     = target.links[i];
            QString     wireName = getLinkWireName(target.name, link.source, i);
            out << "    wire " << wireName << ";\n";
        }
    }

    out << "\n";
}

void QSocClockPrimitive::generateClockLogic(const ClockControllerConfig &config, QTextStream &out)
{
    out << "    /* Clock logic instances */\n";

    for (const auto &target : config.targets) {
        for (int i = 0; i < target.links.size(); ++i) {
            const auto &link = target.links[i];
            generateClockInstance(link, target.name, i, out);
        }
    }

    out << "\n";
}

void QSocClockPrimitive::generateOutputAssignments(
    const ClockControllerConfig &config, QTextStream &out)
{
    out << "    /* Clock output assignments */\n";

    for (const auto &target : config.targets) {
        QString currentSignal;
        QString instanceName = QString("u_%1_target").arg(target.name);

        // Step 1: Handle mux/single source selection
        if (target.links.size() == 1) {
            // Single source
            QString wireName = getLinkWireName(target.name, target.links[0].source, 0);
            currentSignal    = wireName;
        } else if (target.links.size() >= 2) {
            // Multiple sources - generate multiplexer first
            QString muxOutput = QString("%1_mux_out").arg(target.name);

            // If STA guide exists, use a temporary name for MUX output
            QString muxTempOutput = target.mux.sta_guide.configured
                                        ? QString("%1_mux_pre_sta").arg(target.name)
                                        : muxOutput;

            out << "    wire " << muxTempOutput << ";\n";
            generateMuxInstance(target, config, out, muxTempOutput);

            // MUX sta_guide (if specified) - serial insertion, keeps final signal name consistent
            if (target.mux.sta_guide.configured) {
                out << "    wire " << muxOutput << ";\n"; // Final output wire
                QString muxStaInstanceName = target.mux.sta_guide.instance.isEmpty()
                                                 ? QString("u_%1_mux_sta").arg(target.name)
                                                 : target.mux.sta_guide.instance;
                emitStaGuide(out, target.mux.sta_guide, muxStaInstanceName, muxTempOutput, muxOutput);
            }

            currentSignal = muxOutput; // Always use the consistent final name
        }

        // Step 2: Apply target-level processing chain
        // Order: currentSignal -> ICG -> DIV -> INV -> target.name

        // Target-level ICG
        if (target.icg.configured) {
            QString icgOutput = QString("%1_icg_out").arg(target.name);

            // If STA guide exists, use a temporary name for ICG output
            QString icgTempOutput = target.icg.sta_guide.configured
                                        ? QString("%1_icg_pre_sta").arg(target.name)
                                        : icgOutput;

            out << "    wire " << icgTempOutput << ";\n";
            out << "    qsoc_clk_gate #(\n";
            out << "        .CLOCK_DURING_RESET(" << (target.icg.clock_on_reset ? "1'b1" : "1'b0")
                << "),\n";
            out << "        .POLARITY(" << (target.icg.polarity == "high" ? "1'b1" : "1'b0")
                << ")\n";
            out << "    ) " << instanceName << "_icg (\n";
            out << "        .clk(" << currentSignal << "),\n";
            out << "        .en(" << target.icg.enable << "),\n";
            QString testEn = target.icg.test_enable.isEmpty() ? "1'b0" : target.icg.test_enable;
            out << "        .test_en(" << testEn << "),\n";
            out << "        .rst_n(" << (target.icg.reset.isEmpty() ? "1'b1" : target.icg.reset)
                << "),\n";
            out << "        .clk_out(" << icgTempOutput << ")\n";
            out << "    );\n";

            // ICG sta_guide (if specified) - serial insertion, keeps final signal name consistent
            if (target.icg.sta_guide.configured) {
                out << "    wire " << icgOutput << ";\n"; // Final output wire
                QString icgStaInstanceName = target.icg.sta_guide.instance.isEmpty()
                                                 ? QString("u_%1_icg_sta").arg(target.name)
                                                 : target.icg.sta_guide.instance;
                emitStaGuide(out, target.icg.sta_guide, icgStaInstanceName, icgTempOutput, icgOutput);
            }

            currentSignal = icgOutput; // Always use the consistent final name
        }

        // Target-level DIV
        if (target.div.configured) {
            generateDividerInstance(
                target.div,
                QString("target '%1'").arg(target.name),
                instanceName + "_div",
                currentSignal,
                QString("%1_div_out").arg(target.name),
                QString("%1_div_pre_sta").arg(target.name),
                QString("u_%1_div_sta").arg(target.name),
                out);
            currentSignal = QString("%1_div_out").arg(target.name);
        }

        // Target-level INV
        if (target.inv.configured) {
            QString invOutput = QString("%1_inv_out").arg(target.name);

            // If STA guide exists, use a temporary name for INV output
            QString invTempOutput = target.inv.sta_guide.configured
                                        ? QString("%1_inv_pre_sta").arg(target.name)
                                        : invOutput;

            out << "    wire " << invTempOutput << ";\n";
            out << "    qsoc_ck_inv " << instanceName << "_inv (\n";
            out << "        .clk_in(" << currentSignal << "),\n";
            out << "        .clk_out(" << invTempOutput << ")\n";
            out << "    );\n";

            // INV sta_guide (if specified) - serial insertion, keeps final signal name consistent
            if (target.inv.sta_guide.configured) {
                out << "    wire " << invOutput << ";\n"; // Final output wire
                QString invStaInstanceName = target.inv.sta_guide.instance.isEmpty()
                                                 ? QString("u_%1_inv_sta").arg(target.name)
                                                 : target.inv.sta_guide.instance;
                emitStaGuide(out, target.inv.sta_guide, invStaInstanceName, invTempOutput, invOutput);
            }

            currentSignal = invOutput; // Always use the consistent final name
        }

        // Final assignment
        out << "    assign " << target.name << " = " << currentSignal << ";\n";
    }

    out << "\n";
}

void QSocClockPrimitive::generateDividerInstance(
    const ClockDivider &div,
    const QString      &owner,
    const QString      &instance,
    const QString      &input,
    const QString      &output,
    const QString      &preSta,
    const QString      &staInstance,
    QTextStream        &out)
{
    if (div.width <= 0) {
        throw std::runtime_error(
            QString("Clock divider for %1 requires explicit width specification")
                .arg(owner)
                .toStdString());
    }

    const bool    dynamic    = !div.value.isEmpty();
    const bool    autoUpdate = dynamic && div.valid.isEmpty();
    const bool    staGuide   = div.sta_guide.configured;
    const QString divOut     = staGuide ? preSta : output;

    out << "    wire " << divOut << ";\n";
    out << "    qsoc_clk_div #(\n";
    out << "        .WIDTH(" << div.width << "),\n";
    out << "        .DEFAULT_VAL(" << div.default_value << "),\n";
    out << "        .CLOCK_DURING_RESET(" << (div.clock_on_reset ? "1'b1" : "1'b0") << "),\n";
    out << "        .AUTO_UPDATE(" << (autoUpdate ? "1'b1" : "1'b0") << ")\n";
    out << "    ) " << instance << " (\n";
    out << "        .clk(" << input << "),\n";
    out << "        .rst_n(" << (div.reset.isEmpty() ? "1'b1" : div.reset) << "),\n";
    out << "        .en(" << (div.enable.isEmpty() ? "1'b1" : div.enable) << "),\n";
    out << "        .test_en(" << (div.test_enable.isEmpty() ? "1'b0" : div.test_enable) << "),\n";
    if (dynamic)
        out << "        .div(" << div.value << "),\n";
    else
        out << "        .div(" << div.width << "'d" << div.default_value << "),\n";
    out << "        .div_valid(" << (dynamic && !autoUpdate ? div.valid : "1'b0") << "),\n";
    out << "        .div_ready(" << div.ready << "),\n";
    out << "        .clk_out(" << divOut << "),\n";
    out << "        .count(" << div.count << ")\n";
    out << "    );\n";

    if (staGuide) {
        out << "    wire " << output << ";\n";
        emitStaGuide(
            out,
            div.sta_guide,
            div.sta_guide.instance.isEmpty() ? staInstance : div.sta_guide.instance,
            preSta,
            output);
    }
}

void QSocClockPrimitive::generateClockInstance(
    const ClockLink &link, const QString &targetName, int linkIndex, QTextStream &out)
{
    QString wireName     = getLinkWireName(targetName, link.source, linkIndex);
    QString instanceName = getInstanceName(targetName, link.source, linkIndex);
    QString inputClk     = link.source;

    out << "    /*\n";
    out << "     * Link processing: " << link.source << " -> " << targetName;

    // Generate chain based on what's specified
    if (!link.icg.enable.isEmpty()) {
        out << " (icg)";
    }
    if (link.div.default_value > 1 || !link.div.value.isEmpty()) {
        out << " (div/" << link.div.default_value << ")";
    }
    if (link.inv.configured) {
        out << " (inv)";
    }
    out << "\n     */\n";

    // Generate processing chain
    bool hasProcessing = link.icg.configured || link.div.configured || link.inv.configured;

    if (hasProcessing) {
        // Handle link-level processing: ICG → DIV → INV
        QString currentWire = inputClk;

        // Step 1: Link-level ICG
        if (!link.icg.enable.isEmpty()) {
            QString icgWire = wireName + "_preicg";

            // If STA guide exists, use a temporary name for ICG output
            QString icgTempWire = link.icg.sta_guide.configured ? wireName + "_preicg_pre_sta"
                                                                : icgWire;

            out << "    wire " << icgTempWire << ";\n";
            out << "    qsoc_clk_gate #(\n";
            out << "        .CLOCK_DURING_RESET(" << (link.icg.clock_on_reset ? "1'b1" : "1'b0")
                << "),\n";
            out << "        .POLARITY(" << (link.icg.polarity == "high" ? "1'b1" : "1'b0") << ")\n";
            out << "    ) " << instanceName << "_icg (\n";
            out << "        .clk(" << currentWire << "),\n";
            out << "        .en(" << link.icg.enable << "),\n";
            QString testEn = link.icg.test_enable.isEmpty() ? "1'b0" : link.icg.test_enable;
            out << "        .test_en(" << testEn << "),\n";
            out << "        .rst_n(" << (link.icg.reset.isEmpty() ? "1'b1" : link.icg.reset)
                << "),\n";
            out << "        .clk_out(" << icgTempWire << ")\n";
            out << "    );\n";

            // ICG sta_guide (if specified) - serial insertion, keeps final signal name consistent
            if (link.icg.sta_guide.configured) {
                out << "    wire " << icgWire << ";\n"; // Final output wire
                QString icgStaInstanceName = link.icg.sta_guide.instance.isEmpty()
                                                 ? instanceName + "_icg_sta"
                                                 : link.icg.sta_guide.instance;
                emitStaGuide(out, link.icg.sta_guide, icgStaInstanceName, icgTempWire, icgWire);
            }

            currentWire = icgWire; // Always use the consistent final name
        }

        // Step 2: Link-level divider
        if (link.div.default_value > 1 || !link.div.value.isEmpty()) {
            generateDividerInstance(
                link.div,
                QString("link '%1'").arg(wireName),
                instanceName + "_div",
                currentWire,
                wireName + "_prediv",
                wireName + "_prediv_pre_sta",
                instanceName + "_div_sta",
                out);
            currentWire = wireName + "_prediv";
        }

        // Step 3: Link-level inverter
        if (link.inv.configured) {
            QString invWire = QString("%1_inv_wire").arg(instanceName);

            // If STA guide exists, use a temporary name for INV output
            QString invTempWire = link.inv.sta_guide.configured
                                      ? QString("%1_inv_wire_pre_sta").arg(instanceName)
                                      : invWire;

            out << "    wire " << invTempWire << ";\n";
            out << "    qsoc_ck_inv " << instanceName << "_inv (\n";
            out << "        .clk_in(" << currentWire << "),\n";
            out << "        .clk_out(" << invTempWire << ")\n";
            out << "    );\n";

            // INV sta_guide (if specified) - serial insertion, keeps final signal name consistent
            if (link.inv.sta_guide.configured) {
                out << "    wire " << invWire << ";\n"; // Final output wire
                QString invStaInstanceName = link.inv.sta_guide.instance.isEmpty()
                                                 ? instanceName + "_inv_sta"
                                                 : link.inv.sta_guide.instance;
                emitStaGuide(out, link.inv.sta_guide, invStaInstanceName, invTempWire, invWire);
            }

            currentWire = invWire; // Always use the consistent final name
        }

        // Final assignment
        out << "    assign " << wireName << " = " << currentWire << ";\n";

    } else {
        // Simple pass-through case
        out << "    assign " << wireName << " = " << inputClk << ";\n";
    }

    out << "\n";
}

void QSocClockPrimitive::generateMuxInstance(
    const ClockTarget           &target,
    const ClockControllerConfig &config,
    QTextStream                 &out,
    const QString               &outputName)
{
    QString instanceName = QString("u_%1_mux").arg(target.name);
    QString muxOut       = outputName.isEmpty() ? target.name : outputName;

    // Each link already carries its own inverter cell, so take its wire as is
    QStringList inputWires;
    for (int i = 0; i < target.links.size(); ++i) {
        inputWires << getLinkWireName(target.name, target.links[i].source, i);
    }

    int numInputs = inputWires.size();
    int selWidth  = 0;
    if (numInputs > 1) {
        selWidth = 1;
        while ((1 << selWidth) < numInputs)
            selWidth++;
    }
    const QString functionalSelect = clockSelectExpression(target);

    if (target.mux.type == STD_MUX) {
        /* Pad to a power of two so every unused select code picks a zero lane. */
        const int implementationInputs = 1 << selWidth;

        // Standard mux using qsoc_clk_mux_raw
        out << "    qsoc_clk_mux_raw #(\n";
        out << "        .NUM_INPUTS(" << implementationInputs << ")\n";
        out << "    ) " << instanceName << " (\n";

        // Connect clock inputs as array
        out << "        .clk_in({";
        for (int i = implementationInputs - 1; i >= 0; --i) {
            if (i < numInputs) {
                out << inputWires[i];
            } else {
                out << "1'b0";
            }
            if (i > 0)
                out << ", ";
        }
        out << "}),\n";

        // Connect select signal
        out << "        .clk_sel(" << functionalSelect << "),\n";
        out << "        .clk_out(" << muxOut << ")\n";
        out << "    );\n";

    } else if (target.mux.type == GF_MUX) {
        // Glitch-free mux using qsoc_clk_mux_gf
        out << "    qsoc_clk_mux_gf #(\n";
        out << "        .NUM_INPUTS(" << numInputs << "),\n";
        out << "        .NUM_SYNC_STAGES(2),\n";
        out << "        .CLOCK_DURING_RESET(1'b1)\n";
        out << "    ) " << instanceName << " (\n";

        // Connect clock inputs as array
        out << "        .clk_in({";
        for (int i = numInputs - 1; i >= 0; --i) {
            out << inputWires[i];
            if (i > 0)
                out << ", ";
        }
        out << "}),\n";

        // Connect DFT signals
        QString testClk = target.test_clock.isEmpty() ? "1'b0" : target.test_clock;
        QString testEn  = target.test_enable.isEmpty() ? "1'b0" : target.test_enable;
        out << "        .test_clk(" << testClk << "),\n";
        out << "        .test_en(" << testEn << "),\n";

        // Connect reset signal
        QString resetSig = target.reset.isEmpty() ? "1'b1" : target.reset;
        out << "        .async_rst_n(" << resetSig << "),\n";

        // Connect select signal
        out << "        .async_sel(" << functionalSelect << "),\n";
        out << "        .clk_out(" << muxOut << ")\n";
        out << "    );\n";
    }

    out << "\n";
}

QString QSocClockPrimitive::getLinkWireName(
    const QString &targetName, const QString &sourceName, int linkIndex)
{
    // Source names are unique, no need for linkIndex suffix
    Q_UNUSED(linkIndex);
    return QString("clk_%1_from_%2").arg(targetName, sourceName);
}

QString QSocClockPrimitive::getInstanceName(
    const QString &targetName, const QString &sourceName, int linkIndex)
{
    if (linkIndex == 0) {
        return QString("u_%1_%2").arg(targetName, sourceName);
    }
    return QString("u_%1_%2_%3").arg(targetName, sourceName).arg(linkIndex);
}

QString QSocClockPrimitive::generateCellVerilog()
{
    return QSocCellText::clock();
}
