#include "qsocgenerateprimitiveclock.h"
#include "common/qsoccelltext.h"
#include "common/qsocconsole.h"
#include "common/qsocpaths.h"
#include "qsocgeneratemanager.h"
#include "qsocverilogutils.h"
#include <cmath>
#include <QDebug>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
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

    const QStringList requiredKeys = {"cell", "in", "out"};
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

    guide.cell = QString::fromStdString(node["cell"].as<std::string>());
    guide.in   = QString::fromStdString(node["in"].as<std::string>());
    guide.out  = QString::fromStdString(node["out"].as<std::string>());
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

    // Generate Typst clock diagram (failure does not affect Verilog generation)
    if (m_parent && !m_parent->getDiagramDirectory().isEmpty()) {
        const QString outputDir = m_parent->getDiagramDirectory();
        const QString typstName = config.moduleName + QStringLiteral(".typ");
        const auto    artifact  = QSocPaths::resolveArtifactPath(outputDir, typstName);
        if (!artifact.isValid()) {
            QSocConsole::warn() << artifact.error;
        } else if (!generateTypstDiagram(config, artifact.path)) {
            QSocConsole::warn() << "Failed to generate Typst diagram (non-critical):"
                                << artifact.path;
        }
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
                    const YAML::Node muxNode      = it->second["mux"];
                    const YAML::Node staGuideNode = muxNode["sta_guide"];

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
            QString muxTempOutput = !target.mux.sta_guide.cell.isEmpty()
                                        ? QString("%1_mux_pre_sta").arg(target.name)
                                        : muxOutput;

            out << "    wire " << muxTempOutput << ";\n";
            generateMuxInstance(target, config, out, muxTempOutput);

            // MUX sta_guide (if specified) - serial insertion, keeps final signal name consistent
            if (!target.mux.sta_guide.cell.isEmpty()) {
                out << "    wire " << muxOutput << ";\n"; // Final output wire
                QString muxStaInstanceName = target.mux.sta_guide.instance.isEmpty()
                                                 ? QString("u_%1_mux_sta").arg(target.name)
                                                 : target.mux.sta_guide.instance;
                out << "    " << target.mux.sta_guide.cell << " " << muxStaInstanceName << " (\n";
                out << "        ." << target.mux.sta_guide.in << "(" << muxTempOutput << "),\n";
                out << "        ." << target.mux.sta_guide.out << "(" << muxOutput << ")\n";
                out << "    );\n";
            }

            currentSignal = muxOutput; // Always use the consistent final name
        }

        // Step 2: Apply target-level processing chain
        // Order: currentSignal -> ICG -> DIV -> INV -> target.name

        // Target-level ICG
        if (target.icg.configured) {
            QString icgOutput = QString("%1_icg_out").arg(target.name);

            // If STA guide exists, use a temporary name for ICG output
            QString icgTempOutput = !target.icg.sta_guide.cell.isEmpty()
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
            if (!target.icg.sta_guide.cell.isEmpty()) {
                out << "    wire " << icgOutput << ";\n"; // Final output wire
                QString icgStaInstanceName = target.icg.sta_guide.instance.isEmpty()
                                                 ? QString("u_%1_icg_sta").arg(target.name)
                                                 : target.icg.sta_guide.instance;
                out << "    " << target.icg.sta_guide.cell << " " << icgStaInstanceName << " (\n";
                out << "        ." << target.icg.sta_guide.in << "(" << icgTempOutput << "),\n";
                out << "        ." << target.icg.sta_guide.out << "(" << icgOutput << ")\n";
                out << "    );\n";
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
            QString invTempOutput = !target.inv.sta_guide.cell.isEmpty()
                                        ? QString("%1_inv_pre_sta").arg(target.name)
                                        : invOutput;

            out << "    wire " << invTempOutput << ";\n";
            out << "    qsoc_ck_inv " << instanceName << "_inv (\n";
            out << "        .clk_in(" << currentSignal << "),\n";
            out << "        .clk_out(" << invTempOutput << ")\n";
            out << "    );\n";

            // INV sta_guide (if specified) - serial insertion, keeps final signal name consistent
            if (!target.inv.sta_guide.cell.isEmpty()) {
                out << "    wire " << invOutput << ";\n"; // Final output wire
                QString invStaInstanceName = target.inv.sta_guide.instance.isEmpty()
                                                 ? QString("u_%1_inv_sta").arg(target.name)
                                                 : target.inv.sta_guide.instance;
                out << "    " << target.inv.sta_guide.cell << " " << invStaInstanceName << " (\n";
                out << "        ." << target.inv.sta_guide.in << "(" << invTempOutput << "),\n";
                out << "        ." << target.inv.sta_guide.out << "(" << invOutput << ")\n";
                out << "    );\n";
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
    const bool    staGuide   = !div.sta_guide.cell.isEmpty();
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
        out << "    " << div.sta_guide.cell << " "
            << (div.sta_guide.instance.isEmpty() ? staInstance : div.sta_guide.instance) << " (\n";
        out << "        ." << div.sta_guide.in << "(" << preSta << "),\n";
        out << "        ." << div.sta_guide.out << "(" << output << ")\n";
        out << "    );\n";
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
            QString icgTempWire = !link.icg.sta_guide.cell.isEmpty() ? wireName + "_preicg_pre_sta"
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
            if (!link.icg.sta_guide.cell.isEmpty()) {
                out << "    wire " << icgWire << ";\n"; // Final output wire
                QString icgStaInstanceName = link.icg.sta_guide.instance.isEmpty()
                                                 ? instanceName + "_icg_sta"
                                                 : link.icg.sta_guide.instance;
                out << "    " << link.icg.sta_guide.cell << " " << icgStaInstanceName << " (\n";
                out << "        ." << link.icg.sta_guide.in << "(" << icgTempWire << "),\n";
                out << "        ." << link.icg.sta_guide.out << "(" << icgWire << ")\n";
                out << "    );\n";
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
            QString invTempWire = !link.inv.sta_guide.cell.isEmpty()
                                      ? QString("%1_inv_wire_pre_sta").arg(instanceName)
                                      : invWire;

            out << "    wire " << invTempWire << ";\n";
            out << "    qsoc_ck_inv " << instanceName << "_inv (\n";
            out << "        .clk_in(" << currentWire << "),\n";
            out << "        .clk_out(" << invTempWire << ")\n";
            out << "    );\n";

            // INV sta_guide (if specified) - serial insertion, keeps final signal name consistent
            if (!link.inv.sta_guide.cell.isEmpty()) {
                out << "    wire " << invWire << ";\n"; // Final output wire
                QString invStaInstanceName = link.inv.sta_guide.instance.isEmpty()
                                                 ? instanceName + "_inv_sta"
                                                 : link.inv.sta_guide.instance;
                out << "    " << link.inv.sta_guide.cell << " " << invStaInstanceName << " (\n";
                out << "        ." << link.inv.sta_guide.in << "(" << invTempWire << "),\n";
                out << "        ." << link.inv.sta_guide.out << "(" << invWire << ")\n";
                out << "    );\n";
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

/* Typst Clock Diagram Generation */

QString QSocClockPrimitive::escapeTypstId(const QString &str) const
{
    QString result = str;
    return result.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]+")), QStringLiteral("_"));
}

QString QSocClockPrimitive::typstHeader() const
{
    return QStringLiteral(
        "#import \"@preview/circuiteria:0.2.1\": *\n"
        "#import \"@preview/cetz:0.3.4\": draw\n"
        "#set page(width: auto, height: auto, margin: .5cm)\n"
        "#set text(font: \"Sarasa Mono SC\", size: 10pt)\n"
        "#align(center)[\n"
        "  = Clock tree\n"
        "  #text(size: 8pt, fill: gray)[Generated by QSoC.]\n"
        "]\n"
        "#v(0.5cm)\n"
        "#circuit({\n");
}

QString QSocClockPrimitive::typstLegend() const
{
    const float y  = -1.5f;
    const float x  = 0.0f;
    const float w  = 1.6f; // Wider blocks to fit text
    const float sp = 4.0f; // Spacing between legend items

    QString     result;
    QTextStream s(&result);
    s.setRealNumberPrecision(2);
    s.setRealNumberNotation(QTextStream::FixedNotation);

    s << "  // === Legend ===\n";

    // MUX/TEST_MUX - Orange
    s << "  element.multiplexer(x: " << x << ", y: " << y << ", w: 0.8, h: 1.2, "
      << "id: \"legend_mux\", fill: util.colors.orange, entries: 2)\n";
    s << "  draw.content((" << (x + 0.4) << ", " << (y - 0.8) << "), [MUX/TEST_MUX])\n";

    // ICG - Pink
    s << "  element.block(x: " << (x + sp) << ", y: " << (y + 0.3) << ", w: " << w << ", h: 0.8, "
      << "id: \"legend_icg\", name: \"ICG\", fill: util.colors.pink, "
      << "ports: (west: ((id: \"i\"),), east: ((id: \"o\"),)))\n";
    s << "  draw.content((" << (x + sp + w / 2) << ", " << (y - 0.8) << "), [ICG])\n";

    // DIV - Yellow
    s << "  element.block(x: " << (x + sp * 2) << ", y: " << (y + 0.3) << ", w: " << w
      << ", h: 0.8, "
      << "id: \"legend_div\", name: \"÷N\", fill: util.colors.yellow, "
      << "ports: (west: ((id: \"i\"),), east: ((id: \"o\"),)))\n";
    s << "  draw.content((" << (x + sp * 2 + w / 2) << ", " << (y - 0.8) << "), [DIVIDER])\n";

    // INV - Purple
    s << "  element.block(x: " << (x + sp * 3) << ", y: " << (y + 0.3) << ", w: " << w
      << ", h: 0.8, "
      << "id: \"legend_inv\", name: \"INV\", fill: util.colors.purple, "
      << "ports: (west: ((id: \"i\"),), east: ((id: \"o\"),)))\n";
    s << "  draw.content((" << (x + sp * 3 + w / 2) << ", " << (y - 0.8) << "), [INVERTER])\n";

    // STA marker indicator - small blue triangle
    float staX = x + sp * 4;
    s << "  draw.line((" << staX << ", " << (y + 0.3) << "), (" << (staX + 0.3) << ", " << (y + 0.3)
      << "), (" << (staX + 0.15) << ", " << (y + 0.6) << "), close: true, "
      << "fill: util.colors.blue, stroke: none)\n";
    s << "  draw.content((" << (staX + 0.15) << ", " << (y - 0.8) << "), [STA marker])\n\n";

    return result;
}

QString QSocClockPrimitive::typstRootStubs(const QList<ClockInput> &inputs, float &bottomY) const
{
    if (inputs.isEmpty()) {
        bottomY = -5.0f;
        return QString();
    }

    QString     result;
    QTextStream s(&result);
    s.setRealNumberPrecision(2);
    s.setRealNumberNotation(QTextStream::FixedNotation);

    // Use Typst table for clean two-column layout
    // End the circuit block temporarily to insert table
    s << "})\n\n";

    s << "#v(0.3cm)\n";
    s << "#align(center)[\n";
    s << "  #text(weight: \"bold\", size: 10pt)[Clock Sources]\n";
    s << "]\n";
    s << "#v(0.2cm)\n";

    // Two-column table with source name and frequency
    s << "#align(center)[\n";
    s << "#table(\n";
    s << "  columns: (auto, auto, auto, auto),\n";
    s << "  align: (left, center, left, center),\n";
    s << "  stroke: 0.5pt + gray,\n";
    s << "  inset: 5pt,\n";
    s << "  fill: (col, row) => if row == 0 { rgb(\"#e0e0e0\") },\n";
    s << "  [*Source*], [*Freq*], [*Source*], [*Freq*],\n";

    // Fill table rows - two sources per row
    int numSources = inputs.size();
    for (int i = 0; i < numSources; i += 2) {
        const ClockInput &src1  = inputs[i];
        QString           freq1 = src1.freq.isEmpty() ? "-" : src1.freq;

        s << "  [" << src1.name << "], ";
        s << "[" << freq1 << "], ";

        if (i + 1 < numSources) {
            const ClockInput &src2  = inputs[i + 1];
            QString           freq2 = src2.freq.isEmpty() ? "-" : src2.freq;
            s << "[" << src2.name << "], ";
            s << "[" << freq2 << "],\n";
        } else {
            s << "[], [],\n"; // Empty cells for odd number of sources
        }
    }

    s << ")\n";
    s << "]\n\n";

    // Resume circuit block for targets
    s << "#v(0.3cm)\n";
    s << "#circuit({\n";

    // Calculate bottomY for target positioning
    int numRows = (numSources + 1) / 2; // Two sources per row
    bottomY     = -3.0f - numRows * 0.8f;

    return result;
}

QString QSocClockPrimitive::typstTarget(
    const ClockTarget &target, float x, float y, const QString &testEnable) const
{
    QString     result;
    QTextStream s(&result);
    s.setRealNumberPrecision(2);
    s.setRealNumberNotation(QTextStream::FixedNotation);

    QString tid   = escapeTypstId(target.name);
    QString title = target.name;
    if (!target.freq.isEmpty())
        title += QStringLiteral(" (") + target.freq + QStringLiteral(")");

    s << "  // ---- " << title << " ----\n";

    int numSources = target.links.size();

    // Analyze link-level components (per-link ICG/DIV/INV, before MUX)
    QVector<bool> linkHasComp(numSources, false);
    bool          anyLinkHasComp = false;

    for (int i = 0; i < numSources; ++i) {
        const ClockLink &link = target.links[i];
        if (link.icg.configured || link.div.configured || link.inv.configured) {
            linkHasComp[i] = true;
            anyLinkHasComp = true;
        }
    }

    // Analyze target-level components (Post-MUX ICG/DIV/INV)
    bool hasTargetIcg = target.icg.configured;
    bool hasTargetDiv = target.div.configured;
    bool hasTargetInv = target.inv.configured;

    // Layout calculation
    float linkCompX = x;                               // Link components start position
    float muxX      = anyLinkHasComp ? (x + 4.0f) : x; // MUX X position
    // Post-MUX components start position
    // Add extra gap when no target-level components to avoid MUX-to-MUX text overlap
    bool  hasAnyTargetComp = hasTargetIcg || hasTargetDiv || hasTargetInv;
    float postMuxX = muxX + (hasAnyTargetComp ? 2.0f : 3.5f); // Extra space if no components
    float currentX = postMuxX;

    // Calculate final output X based on target-level components
    if (hasTargetIcg)
        currentX += 2.5f;
    if (hasTargetDiv)
        currentX += 2.5f;
    if (hasTargetInv)
        currentX += 2.5f;
    if (!target.test_clock.isEmpty())
        currentX += 3.0f; // Extra space for test MUX stub labels
    float outX = currentX + 1.0f;

    // Calculate MUX height - use fixed per-port spacing for consistent layout
    const float portSpacing = 1.5f; // Spacing per MUX port
    const float compHeight  = 0.9f; // Link component block height
    float       muxHeight   = qMax(2.0f, portSpacing * numSources);
    float       muxBottomY  = y; // MUX bottom at y
    float       muxCenterY  = y + muxHeight / 2;

    // Store MUX input connection points
    QVector<QString> muxInputPorts(numSources);

    // Calculate Y positions for each link to align with MUX auto-distributed ports
    // Circuiteria MUX ports are top-to-bottom: port-in[i] at y + h * (1 - (i + 0.5) / entries)
    const bool     needMux    = (numSources > 1) || (!target.select.isEmpty() && numSources > 0);
    const int      muxEntries = needMux ? qMax(2, numSources) : numSources;
    QVector<float> linkPortY(numSources);
    for (int i = 0; i < numSources; ++i) {
        // MUX port center Y (top-to-bottom distribution)
        float muxPortY = muxBottomY + muxHeight * (1.0f - (float(i) + 0.5f) / float(muxEntries));
        // Link component should align with this port
        linkPortY[i] = muxPortY - compHeight / 2; // Block bottom-left Y
    }

    // Step 1: Draw link-level components (before MUX)
    for (int i = 0; i < numSources; ++i) {
        const ClockLink &link  = target.links[i];
        float            compY = linkPortY[i];
        float            compX = linkCompX;
        QString          prevPort;

        if (linkHasComp[i]) {
            // Helper lambda to draw STA marker (small blue triangle inside top-right corner)
            auto drawStaMarker = [&s](float bx, float by, float bw, float bh) {
                float tx = bx + bw - 0.25f;
                float ty = by + bh - 0.30f; // Inside the block (triangle height is 0.2)
                s << "  draw.line((" << tx << ", " << ty << "), (" << (tx + 0.2f) << ", " << ty
                  << "), (" << (tx + 0.1f) << ", " << (ty + 0.2f)
                  << "), close: true, fill: util.colors.blue, stroke: none)\n";
            };

            // Draw ICG if configured
            if (link.icg.configured) {
                QString icgId = escapeTypstId(
                    tid + QStringLiteral("_L") + QString::number(i) + QStringLiteral("_ICG"));
                s << "  element.block(\n";
                s << "    x: " << compX << ", y: " << compY << ", w: 1.0, h: 0.9,\n";
                s << "    id: \"" << icgId << "\", name: \"ICG\", fill: util.colors.pink,\n";
                s << "    ports: (west: ((id: \"in\"),), east: ((id: \"out\"),))\n";
                s << "  )\n";

                // STA marker if sta_guide configured
                if (!link.icg.sta_guide.cell.isEmpty()) {
                    drawStaMarker(compX, compY, 1.0f, 0.9f);
                }

                // Show enable signal above ICG
                if (!link.icg.enable.isEmpty()) {
                    s << "  draw.content((" << (compX + 0.5f) << ", " << (compY + 0.9f + 0.2f)
                      << "), text(size: 7pt)[" << link.icg.enable << "])\n";
                }

                if (prevPort.isEmpty()) {
                    s << "  wire.stub(\"" << icgId << "-port-in\", \"west\", name: \""
                      << link.source << "\")\n";
                } else {
                    s << "  wire.wire(\"w_" << tid << "_l" << i << "_to_icg\", (\n";
                    s << "    \"" << prevPort << "\", \"" << icgId << "-port-in\"\n";
                    s << "  ))\n";
                }
                prevPort = icgId + QStringLiteral("-port-out");
                compX += 1.3f;
            }

            // Draw DIV if configured
            if (link.div.configured) {
                QString divId = escapeTypstId(
                    tid + QStringLiteral("_L") + QString::number(i) + QStringLiteral("_DIV"));
                s << "  element.block(\n";
                s << "    x: " << compX << ", y: " << compY << ", w: 1.0, h: 0.9,\n";
                s << "    id: \"" << divId << "\", name: \"÷N\", fill: util.colors.yellow,\n";
                s << "    ports: (west: ((id: \"in\"),), east: ((id: \"out\"),))\n";
                s << "  )\n";

                // STA marker if sta_guide configured
                if (!link.div.sta_guide.cell.isEmpty()) {
                    drawStaMarker(compX, compY, 1.0f, 0.9f);
                }

                // Show range annotation above DIV (offset 0.5 to avoid overlap with block text)
                if (link.div.width > 0) {
                    int maxVal = (1 << link.div.width) - 1;
                    s << "  draw.content((" << (compX + 0.5f) << ", " << (compY + 0.9f + 0.5f)
                      << "), text(size: 7pt)[N∈\\[0," << maxVal << "\\]])\n";
                } else {
                    s << "  draw.content((" << (compX + 0.5f) << ", " << (compY + 0.9f + 0.5f)
                      << "), text(size: 7pt)[N=" << link.div.default_value << "])\n";
                }

                if (prevPort.isEmpty()) {
                    s << "  wire.stub(\"" << divId << "-port-in\", \"west\", name: \""
                      << link.source << "\")\n";
                } else {
                    s << "  wire.wire(\"w_" << tid << "_l" << i << "_to_div\", (\n";
                    s << "    \"" << prevPort << "\", \"" << divId << "-port-in\"\n";
                    s << "  ))\n";
                }
                prevPort = divId + QStringLiteral("-port-out");
                compX += 1.3f;
            }

            // Draw INV if configured
            if (link.inv.configured) {
                QString invId = escapeTypstId(
                    tid + QStringLiteral("_L") + QString::number(i) + QStringLiteral("_INV"));
                s << "  element.block(\n";
                s << "    x: " << compX << ", y: " << compY << ", w: 1.0, h: 0.9,\n";
                s << "    id: \"" << invId << "\", name: \"INV\", fill: util.colors.purple,\n";
                s << "    ports: (west: ((id: \"in\"),), east: ((id: \"out\"),))\n";
                s << "  )\n";

                // STA marker if sta_guide configured
                if (!link.inv.sta_guide.cell.isEmpty()) {
                    drawStaMarker(compX, compY, 1.0f, 0.9f);
                }

                if (prevPort.isEmpty()) {
                    s << "  wire.stub(\"" << invId << "-port-in\", \"west\", name: \""
                      << link.source << "\")\n";
                } else {
                    s << "  wire.wire(\"w_" << tid << "_l" << i << "_to_inv\", (\n";
                    s << "    \"" << prevPort << "\", \"" << invId << "-port-in\"\n";
                    s << "  ))\n";
                }
                prevPort = invId + QStringLiteral("-port-out");
            }

            muxInputPorts[i] = prevPort;
        } else {
            // No link component - will connect directly to MUX with stub
            muxInputPorts[i] = QString();
        }
    }

    // Step 2: Draw MUX or single source block
    QString muxOutputPort;

    if (needMux) {
        QString muxId = escapeTypstId(tid + QStringLiteral("_MUX"));
        s << "  element.multiplexer(\n";
        s << "    x: " << muxX << ", y: " << muxBottomY << ", w: 1.0, h: " << muxHeight << ",\n";
        s << "    id: \"" << muxId << "\", fill: util.colors.orange, entries: " << muxEntries
          << "\n";
        s << "  )\n";

        if (!target.select.isEmpty())
            s << "  draw.content((" << (muxX + 0.5f) << ", " << (muxBottomY + muxHeight + 0.3f)
              << "), text(size: 8pt)[" << target.select << "])\n";

        // STA marker if mux.sta_guide configured (inside top-right corner)
        if (!target.mux.sta_guide.cell.isEmpty()) {
            float mtx = muxX + 1.0f - 0.35f;            // Right edge - margin
            float mty = muxBottomY + muxHeight - 0.35f; // Top edge - margin
            s << "  draw.line((" << mtx << ", " << mty << "), (" << (mtx + 0.25f) << ", " << mty
              << "), (" << (mtx + 0.125f) << ", " << (mty + 0.25f)
              << "), close: true, fill: util.colors.blue, stroke: none)\n";
        }

        // Connect inputs to MUX
        for (int i = 0; i < numSources; ++i) {
            QString muxInPort = muxId + QStringLiteral("-port-in") + QString::number(i);
            if (muxInputPorts[i].isEmpty()) {
                // Direct connection - draw stub
                s << "  wire.stub(\"" << muxInPort << "\", \"west\", name: \""
                  << target.links[i].source << "\")\n";
            } else {
                // Connect from link component output
                s << "  wire.wire(\"w_" << tid << "_l" << i << "_to_mux\", (\n";
                s << "    \"" << muxInputPorts[i] << "\", \"" << muxInPort << "\"\n";
                s << "  ))\n";
            }
        }

        muxOutputPort = muxId + QStringLiteral("-port-out");
    } else if (numSources > 0) {
        // Single source - use solid triangle input marker aligned with target components
        if (muxInputPorts[0].isEmpty()) {
            QString sid = escapeTypstId(tid + QStringLiteral("_SRC"));
            // Right-pointing triangle (42° tip angle), sized to match output arrow
            float triWidth = 0.38f;
            float triHalfH = 0.16f;
            float triBaseX = muxX;
            float triTipX  = triBaseX + triWidth;
            float triY     = muxCenterY;
            s << "  draw.line((" << triBaseX << ", " << (triY + triHalfH) << "), (" << triTipX
              << ", " << triY << "), (" << triBaseX << ", " << (triY - triHalfH)
              << "), close: true, fill: black, stroke: none)\n";
            s << "  draw.content((" << (triBaseX - 0.1f) << ", " << triY
              << "), anchor: \"east\", text(size: 8pt)[" << target.links[0].source << "])\n";
            // Tiny invisible anchor: position so east port aligns with triangle tip
            float anchorS = 0.01f;
            s << "  element.block(x: " << (triTipX - anchorS) << ", y: " << (triY - anchorS / 2)
              << ", w: " << anchorS << ", h: " << anchorS << ", id: \"" << sid
              << "\", name: \"\", stroke: none, fill: none, ports: (east: ((id: \"out\"),)))\n";
            muxOutputPort = sid + QStringLiteral("-port-out");
        } else {
            muxOutputPort = muxInputPorts[0];
        }
    } else {
        // No connection - use solid triangle input marker with "NC" label
        QString sid      = escapeTypstId(tid + QStringLiteral("_SRC"));
        float   triWidth = 0.38f;
        float   triHalfH = 0.16f;
        float   triBaseX = muxX;
        float   triTipX  = triBaseX + triWidth;
        float   triY     = muxCenterY;
        s << "  draw.line((" << triBaseX << ", " << (triY + triHalfH) << "), (" << triTipX << ", "
          << triY << "), (" << triBaseX << ", " << (triY - triHalfH)
          << "), close: true, fill: black, stroke: none)\n";
        s << "  draw.content((" << (triBaseX - 0.1f) << ", " << triY
          << "), anchor: \"east\", text(size: 8pt)[NC])\n";
        // Tiny invisible anchor: position so east port aligns with triangle tip
        float anchorS = 0.01f;
        s << "  element.block(x: " << (triTipX - anchorS) << ", y: " << (triY - anchorS / 2)
          << ", w: " << anchorS << ", h: " << anchorS << ", id: \"" << sid
          << "\", name: \"\", stroke: none, fill: none, ports: (east: ((id: \"out\"),)))\n";
        muxOutputPort = sid + QStringLiteral("-port-out");
    }

    QString prev = muxOutputPort;
    currentX     = postMuxX;

    // Step 3: Draw target-level components (Post-MUX)

    // Target-level components - all ports should align at muxCenterY
    // For block with height h, port is at center (y + h/2), so y = muxCenterY - h/2
    const float targetCompH = 1.2f;
    const float targetCompY = muxCenterY - targetCompH / 2; // Port aligns at muxCenterY

    // Helper lambda for STA marker on target-level components (inside top-right corner)
    auto drawStaMarkerTarget = [&s](float bx, float by, float bw, float bh) {
        float tx = bx + bw - 0.35f;
        float ty = by + bh - 0.35f; // Inside the block (triangle height is 0.25)
        s << "  draw.line((" << tx << ", " << ty << "), (" << (tx + 0.25f) << ", " << ty << "), ("
          << (tx + 0.125f) << ", " << (ty + 0.25f)
          << "), close: true, fill: util.colors.blue, stroke: none)\n";
    };

    // Target-level ICG
    if (hasTargetIcg) {
        QString iid = escapeTypstId(tid + QStringLiteral("_ICG"));
        s << "  element.block(\n";
        s << "    x: " << currentX << ", y: " << targetCompY << ", w: 1.2, h: " << targetCompH
          << ",\n";
        s << "    id: \"" << iid << "\", name: \"ICG\", fill: util.colors.pink,\n";
        s << "    ports: (west: ((id: \"in\"),), east: ((id: \"out\"),))\n";
        s << "  )\n";

        // STA marker if sta_guide configured
        if (!target.icg.sta_guide.cell.isEmpty()) {
            drawStaMarkerTarget(currentX, targetCompY, 1.2f, targetCompH);
        }

        // Show enable signal above ICG
        if (!target.icg.enable.isEmpty()) {
            s << "  draw.content((" << (currentX + 0.6f) << ", "
              << (targetCompY + targetCompH + 0.2f) << "), text(size: 7pt)[" << target.icg.enable
              << "])\n";
        }

        s << "  wire.wire(\"w_" << tid << "_to_icg\", (\n";
        s << "    \"" << prev << "\", \"" << iid << "-port-in\"\n";
        s << "  ))\n";
        prev = iid + QStringLiteral("-port-out");
        currentX += 2.5f;
    }

    // Target-level DIV
    if (hasTargetDiv) {
        QString did = escapeTypstId(tid + QStringLiteral("_DIV"));
        s << "  element.block(\n";
        s << "    x: " << currentX << ", y: " << targetCompY << ", w: 1.2, h: " << targetCompH
          << ",\n";
        s << "    id: \"" << did << "\", name: \"÷N\", fill: util.colors.yellow,\n";
        s << "    ports: (west: ((id: \"in\"),), east: ((id: \"out\"),))\n";
        s << "  )\n";

        // STA marker if sta_guide configured
        if (!target.div.sta_guide.cell.isEmpty()) {
            drawStaMarkerTarget(currentX, targetCompY, 1.2f, targetCompH);
        }

        // Show range annotation above DIV (offset 0.5 to avoid overlap with block text)
        if (target.div.width > 0) {
            int maxVal = (1 << target.div.width) - 1;
            s << "  draw.content((" << (currentX + 0.6f) << ", "
              << (targetCompY + targetCompH + 0.5f) << "), text(size: 7pt)[N∈\\[0," << maxVal
              << "\\]])\n";
        } else {
            s << "  draw.content((" << (currentX + 0.6f) << ", "
              << (targetCompY + targetCompH + 0.5f)
              << "), text(size: 7pt)[N=" << target.div.default_value << "])\n";
        }

        s << "  wire.wire(\"w_" << tid << "_to_div\", (\n";
        s << "    \"" << prev << "\", \"" << did << "-port-in\"\n";
        s << "  ))\n";
        prev = did + QStringLiteral("-port-out");
        currentX += 2.5f;
    }

    // Target-level INV
    if (hasTargetInv) {
        QString invId = escapeTypstId(tid + QStringLiteral("_INV"));
        s << "  element.block(\n";
        s << "    x: " << currentX << ", y: " << targetCompY << ", w: 1.2, h: " << targetCompH
          << ",\n";
        s << "    id: \"" << invId << "\", name: \"INV\", fill: util.colors.purple,\n";
        s << "    ports: (west: ((id: \"in\"),), east: ((id: \"out\"),))\n";
        s << "  )\n";

        // STA marker if sta_guide configured
        if (!target.inv.sta_guide.cell.isEmpty()) {
            drawStaMarkerTarget(currentX, targetCompY, 1.2f, targetCompH);
        }

        s << "  wire.wire(\"w_" << tid << "_to_inv\", (\n";
        s << "    \"" << prev << "\", \"" << invId << "-port-in\"\n";
        s << "  ))\n";
        prev = invId + QStringLiteral("-port-out");
        currentX += 2.5f;
    }

    // Test clock multiplexer
    // Circuiteria MUX ports are top-to-bottom: port-in0 at y + 3h/4, port-in1 at y + h/4
    // To align port-in0 at muxCenterY: y = muxCenterY - 3h/4
    // Output port is at MUX center: y + h/2
    float finalOutY = muxCenterY; // Default output Y is muxCenterY
    if (!target.test_clock.isEmpty()) {
        QString     tmId = escapeTypstId(tid + QStringLiteral("_TM"));
        QString     te   = testEnable.isEmpty() ? QStringLiteral("test_en") : testEnable;
        const float tmH  = 2.0f;
        const float tmY  = muxCenterY - 3.0f * tmH / 4.0f; // port-in0 aligns at muxCenterY
        // cppcheck-suppress duplicateExpression
        finalOutY = tmY + tmH / 2.0f; // test MUX output at its center
        s << "  element.multiplexer(\n";
        s << "    x: " << currentX << ", y: " << tmY << ", w: 1.0, h: " << tmH << ",\n";
        s << "    id: \"" << tmId << "\", fill: util.colors.orange, entries: 2\n";
        s << "  )\n";
        s << "  wire.stub(\"" << tmId << ".north\", \"north\", name: \"" << te << "\")\n";
        s << "  wire.stub(\"" << tmId << "-port-in1\", \"west\", name: \"" << target.test_clock
          << "\")\n";
        s << "  wire.wire(\"w_" << tid << "_to_tm\", (\n";
        s << "    \"" << prev << "\", \"" << tmId << "-port-in0\"\n";
        s << "  ))\n";
        prev = tmId + QStringLiteral("-port-out");
        currentX += 2.5f;
    }

    // Step 4: Final output - arrow with label (align with last component output)
    float arrowEndX = currentX + 2.5f;
    s << "  draw.line(\"" << prev << "\", (" << arrowEndX << ", " << finalOutY
      << "), mark: (end: \">\", fill: black))\n";
    s << "  draw.content((" << (arrowEndX + 0.3f) << ", " << finalOutY << "), anchor: \"west\", ["
      << target.name << "])\n\n";

    return result;
}

bool QSocClockPrimitive::generateTypstDiagram(
    const ClockControllerConfig &config, const QString &outputPath)
{
    const QFileInfo outputInfo(outputPath);
    const auto      artifact
        = QSocPaths::resolveArtifactPath(outputInfo.absolutePath(), outputInfo.fileName());
    if (!artifact.isValid()) {
        QSocConsole::warn() << artifact.error;
        return false;
    }

    QFile file(artifact.path);
    if (!file.open(QIODevice::WriteOnly)) {
        QSocConsole::warn() << "Failed to open Typst output file:" << artifact.path;
        return false;
    }

    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);

    // Generate header
    out << typstHeader();

    // Generate legend
    out << typstLegend();

    // Generate root clock stubs
    float bottomY = -5.0f;
    out << typstRootStubs(config.inputs, bottomY);

    // Generate targets (vertical stacking with dynamic spacing)
    // Key insight: MUX extends UPWARD from y to y+muxHeight
    // So we position MUX TOP at currentY by setting y = currentY - muxHeight
    const float x0          = 0.0f;
    const float portSpacing = 1.5f; // Match typstTarget portSpacing
    const float extraMargin = 2.5f; // Extra margin between targets

    float currentY = bottomY - 3.0f;

    for (int idx = 0; idx < config.targets.size(); ++idx) {
        const ClockTarget &target     = config.targets[idx];
        int                numSources = target.links.size();

        // Calculate target height - same as typstTarget
        float muxHeight = qMax(2.0f, portSpacing * numSources);

        // Position target so MUX TOP is at currentY
        // typstTarget uses y as MUX bottom, so y = currentY - muxHeight
        float targetY = currentY - muxHeight;
        out << typstTarget(target, x0, targetY, target.test_enable);

        // Move to next target position (MUX bottom is at targetY)
        currentY = targetY - extraMargin;
    }

    // Close circuit
    out << "})\n";

    file.close();
    QSocConsole::info() << "Generated Typst clock diagram:" << artifact.path;
    return true;
}
