// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOCGENERATEMANAGER_H
#define QSOCGENERATEMANAGER_H

#include "common/qllmservice.h"
#include "common/qsocbusmanager.h"
#include "common/qsocconfig.h"
#include "common/qsocmodulemanager.h"
#include "common/qsocnumberinfo.h"
#include "common/qsocprojectmanager.h"

// Forward declarations for primitives
class QSocResetPrimitive;
class QSocClockPrimitive;
class QSocPowerPrimitive;
class QSocFSMPrimitive;
class QSocCombPrimitive;
class QSocSeqPrimitive;

#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <utility>

#include <yaml-cpp/yaml.h>

/**
 * @brief The QSocGenerateManager class.
 * @details This class is used to generate RTL code from netlist files.
 */
class QSocGenerateManager : public QObject
{
    Q_OBJECT
public:
    /**
     * @brief Constructor.
     * @details This constructor will create an instance of this object.
     * @param[in] parent parent object.
     * @param[in] projectManager project manager.
     * @param[in] moduleManager module manager.
     * @param[in] busManager bus manager.
     * @param[in] llmService LLM service.
     */
    QSocGenerateManager(
        QObject            *parent         = nullptr,
        QSocProjectManager *projectManager = nullptr,
        QSocModuleManager  *moduleManager  = nullptr,
        QSocBusManager     *busManager     = nullptr,
        QLLMService        *llmService     = nullptr);

    /**
     * @brief Destructor for QSocGenerateManager.
     * @details This destructor will free all the allocated resources.
     */
    ~QSocGenerateManager() override;

    /**
     * @brief Enum for port direction check results
     */
    enum class PortDirectionStatus : std::uint8_t {
        Valid,     /**< Consistent port directions */
        Undriven,  /**< Net has only input ports */
        Multidrive /**< Net has multiple output/inout ports */
    };
    Q_ENUM(PortDirectionStatus)

    /**
     * @brief Enum to distinguish between module ports, top-level ports, and comb/seq/fsm outputs
     */
    enum class PortType : std::uint8_t {
        Module,    /**< Instance/module port */
        TopLevel,  /**< Top-level port */
        CombSeqFsm /**< Comb/seq/fsm output */
    };
    Q_ENUM(PortType)

    /**
     * @brief Structure to represent a port connection with type information
     */
    struct PortConnection
    {
        PortType type;
        QString  instanceName; /**< For Module type only, empty for TopLevel */
        QString  portName;

        PortConnection(PortType portType, QString instanceName, QString portName)
            : type(portType)
            , instanceName(std::move(instanceName))
            , portName(std::move(portName))
        {}

        static PortConnection createModulePort(const QString &instanceName, const QString &portName)
        {
            return {PortType::Module, instanceName, portName};
        }

        static PortConnection createTopLevelPort(const QString &portName)
        {
            return {PortType::TopLevel, "", portName};
        }

        static PortConnection createCombSeqFsmPort(const QString &portName)
        {
            return {PortType::CombSeqFsm, "", portName};
        }
    };

    /**
     * @brief Structure to represent detailed port information
     */
    struct PortDetailInfo
    {
        PortType type;
        QString  instanceName; /**< For Module type only, empty for TopLevel */
        QString  portName;
        QString  width;
        QString  direction;
        QString  bitSelect; /**< Bit selection if specified (e.g. "[7:0]", "[3]") */

        PortDetailInfo(
            PortType portType,
            QString  instanceName,
            QString  portName,
            QString  widthSpec,
            QString  direction,
            QString  bitSelection = "")
            : type(portType)
            , instanceName(std::move(instanceName))
            , portName(std::move(portName))
            , width(std::move(widthSpec))
            , direction(std::move(direction))
            , bitSelect(std::move(bitSelection))
        {}

        static PortDetailInfo createModulePort(
            const QString &instanceName,
            const QString &portName,
            const QString &widthSpec,
            const QString &direction,
            const QString &bitSelection = "")
        {
            return {PortType::Module, instanceName, portName, widthSpec, direction, bitSelection};
        }

        static PortDetailInfo createTopLevelPort(
            const QString &portName,
            const QString &widthSpec,
            const QString &direction,
            const QString &bitSelection = "")
        {
            return {PortType::TopLevel, "", portName, widthSpec, direction, bitSelection};
        }

        static PortDetailInfo createCombSeqFsmPort(
            const QString &portName,
            const QString &widthSpec,
            const QString &direction,
            const QString &bitSelection = "")
        {
            return {PortType::CombSeqFsm, "", portName, widthSpec, direction, bitSelection};
        }
    };

    /**
     * @brief Check port direction consistency for a list of connections
     * @param connections List of port connections to check
     * @return PortDirectionStatus Status of the connection (OK, Undriven, or Multidrive)
     */
    PortDirectionStatus checkPortDirectionConsistency(const QList<PortConnection> &connections);

    /**
     * @brief Check port direction consistency with bit-level overlap detection
     * @param portDetails List of detailed port information to check
     * @return PortDirectionStatus Status of the connection (OK, Undriven, or Multidrive)
     */
    PortDirectionStatus checkPortDirectionConsistencyWithBitOverlap(
        const QList<PortDetailInfo> &portDetails);

    /**
     * @brief Calculate the width of a bit selection expression
     * @param bitSelect Bit selection string (e.g. "[3:2]", "[5]")
     * @return The width of the bit selection (e.g. 2 for "[3:2]", 1 for "[5]")
     */
    int calculateBitSelectWidth(const QString &bitSelect);

    /**
     * @brief Check port width consistency for a list of connections
     * @param connections List of port connections to check
     * @return Whether all ports have consistent width
     */
    bool checkPortWidthConsistency(const QList<PortConnection> &connections);

    /**
     * @brief Clean type strings for wire declarations
     * @details Removes type keywords (logic, reg, wire, etc.) from type strings,
     *          keeping only width information like [7:0]. Uses a general regex
     *          pattern to match any keyword followed by optional whitespace and
     *          then either '[' or end of string.
     * @param typeStr Input type string (e.g. "reg [7:0]", "logic", "wire [3:0]")
     * @return Cleaned string with only width information (e.g. "[7:0]", "", "[3:0]")
     */
    static QString cleanTypeForWireDeclaration(const QString &typeStr);

    /**
     * @brief Parse signal name and extract bit selection information
     * @param signalName Signal name which may contain bit selection (e.g. "out[7:0]", "data[3]")
     * @return Pair of (base_signal_name, bit_selection_string)
     */
    static QPair<QString, QString> parseSignalBitSelect(const QString &signalName);

    /**
     * @brief Collect all signals (inputs and outputs) from comb/seq/fsm sections
     * @details Analyzes netlist data and collects all signals referenced by combinational,
     *          sequential, and FSM logic blocks. This includes both output signals and
     *          input signals extracted from Verilog expressions using slang parser.
     * @return List of PortDetailInfo for all comb/seq/fsm signals with direction info
     */
    QList<PortDetailInfo> collectCombSeqFsmSignals();

    /**
     * @brief Check if two bit ranges overlap
     * @param range1 First bit range in format "[msb:lsb]" or "[bit]"
     * @param range2 Second bit range in format "[msb:lsb]" or "[bit]"
     * @return True if the ranges overlap, false otherwise
     */
    static bool doBitRangesOverlap(const QString &range1, const QString &range2);

    /**
     * @brief Check if bit ranges provide full coverage for a signal width
     * @param ranges List of bit range strings
     * @param signalWidth Expected signal width (0 means single bit)
     * @return True if ranges fully cover the signal without gaps
     */
    static bool doBitRangesProvideFullCoverage(const QStringList &ranges, int signalWidth);

    /**
     * @brief Format generated Verilog file using verible-verilog-format if available
     * @details This function attempts to format a Verilog file using verible-verilog-format.
     *          If the tool is not available, it silently returns false without error.
     * @param filePath Absolute path to the Verilog file to format
     * @return true if formatting successful, false if tool not found or formatting failed
     */
    static bool formatVerilogFile(const QString &filePath);

public slots:
    /**
     * @brief Set the project manager.
     * @details Assigns a new project manager to this object. The project
     *          manager is used for managing various project-related
     *          functionalities.
     * @param projectManager Pointer to the new project manager.
     */
    void setProjectManager(QSocProjectManager *projectManager);

    /**
     * @brief Set the module manager.
     * @details Assigns a new module manager to this object. The module manager
     *          is used for managing module-related functionalities.
     * @param moduleManager Pointer to the new module manager.
     */
    void setModuleManager(QSocModuleManager *moduleManager);

    /**
     * @brief Set the bus manager.
     * @details Assigns a new bus manager to this object. The bus manager
     *          is used for managing bus-related functionalities.
     * @param busManager Pointer to the new bus manager.
     */
    void setBusManager(QSocBusManager *busManager);

    /**
     * @brief Set the LLM service.
     * @details Assigns a new LLM service to this object.
     * @param llmService Pointer to the new LLM service.
     */
    void setLLMService(QLLMService *llmService);

    /**
     * @brief Get the project manager.
     * @details Retrieves the currently assigned project manager.
     * @return QSocProjectManager * Pointer to the current project manager.
     */
    QSocProjectManager *getProjectManager();

    /**
     * @brief Get the module manager.
     * @details Retrieves the currently assigned module manager.
     * @return QSocModuleManager * Pointer to the current module manager.
     */
    QSocModuleManager *getModuleManager();

    /**
     * @brief Get the bus manager.
     * @details Retrieves the currently assigned bus manager.
     * @return QSocBusManager * Pointer to the current bus manager.
     */
    QSocBusManager *getBusManager();

    /**
     * @brief Get the LLM service.
     * @details Retrieves the currently assigned LLM service.
     * @return QLLMService * Pointer to the current LLM service.
     */
    QLLMService *getLLMService();

    /**
     * @brief Set force overwrite mode for primitive cell files.
     * @details When enabled, existing primitive cell files (clock_cell.v, reset_cell.v)
     *          will be forcefully overwritten during generation.
     * @param force true to enable force overwrite, false to preserve existing files.
     */
    void setForceOverwrite(bool force);

    /**
     * @brief Load netlist file.
     * @details Loads a netlist file and creates an in-memory representation.
     * @param netlistFilePath Path to the netlist file.
     * @retval true Netlist file loaded successfully.
     * @retval false Failed to load netlist file.
     */
    bool loadNetlist(const QString &netlistFilePath);

    /**
     * @brief Set netlist data directly.
     * @details Sets the netlist data directly from a YAML node, useful for
     *          processing merged netlists.
     * @param netlistData YAML node containing the netlist data.
     * @retval true Netlist data set successfully.
     * @retval false Failed to set netlist data.
     */
    bool setNetlistData(const YAML::Node &netlistData);

    /**
     * @brief Process and expand the netlist.
     * @details Processes the loaded netlist, expanding buses into individual signals.
     * @retval true Netlist processed successfully.
     * @retval false Failed to process netlist.
     */
    bool processNetlist();

    /**
     * @brief Expand bus link references in instance bus sections.
     * @details Scans all instances for bus sections with link attributes,
     *          and expands them into the netlist's bus section.
     * @retval true Bus links expanded successfully.
     * @retval false Failed to expand bus links.
     */
    bool expandBusLink();

    /**
     * @brief Expand bus uplink references in instance bus sections.
     * @details Scans all instances for bus sections with uplink attributes,
     *          and expands them into port uplink attributes for each bus signal.
     * @retval true Bus uplinks expanded successfully.
     * @retval false Failed to expand bus uplinks.
     */
    bool expandBusUplink();

    /**
     * @brief Generate Verilog code from the processed netlist.
     * @details Generates Verilog code and saves it to the output directory.
     * @param outputFileName Output file name (without extension).
     * @retval true Verilog code generated and saved successfully.
     * @retval false Failed to generate or save Verilog code.
     */
    bool generateVerilog(const QString &outputFileName);

    /**
     * @brief Render a Jinja2 template with provided data files.
     * @details Loads data from CSV, YAML, JSON, SystemRDL, and RCSV files, then renders a Jinja2 template
     *          and saves the result to the output directory.
     * @param templateFilePath Path to the Jinja2 template file.
     * @param csvFiles List of CSV data files to load.
     * @param yamlFiles List of YAML data files to load.
     * @param jsonFiles List of JSON data files to load.
     * @param rdlFiles List of SystemRDL data files to load.
     * @param rcsvFiles List of RCSV (Register-CSV) data files to load.
     * @param outputFileName Output file name (without extension).
     * @retval true Template rendered and saved successfully.
     * @retval false Failed to render or save template.
     */
    bool renderTemplate(
        const QString     &templateFilePath,
        const QStringList &csvFiles,
        const QStringList &yamlFiles,
        const QStringList &jsonFiles,
        const QStringList &rdlFiles,
        const QStringList &rcsvFiles,
        const QString     &outputFileName);

    /**
     * @brief Generate stub files for selected modules.
     * @details This function generates both Verilog (.v) and Liberty (.lib) stub files
     *          for modules matching the specified library and module regex patterns.
     * @param stubName Base name for the output stub files.
     * @param libraryRegex Regular expression to filter libraries.
     * @param moduleRegex Regular expression to filter modules within libraries.
     * @retval true Stub files generated successfully.
     * @retval false Failed to generate stub files.
     */
    bool generateStub(
        const QString            &stubName,
        const QRegularExpression &libraryRegex,
        const QRegularExpression &moduleRegex);

    /**
     * @brief Generate Verilog stub file for selected modules.
     * @details This function generates a Verilog stub file containing all specified modules
     *          with their port declarations but without implementation details.
     * @param stubName Base name for the output Verilog file.
     * @param moduleNames List of module names to include in the stub.
     * @retval true Verilog stub file generated successfully.
     * @retval false Failed to generate Verilog stub file.
     */
    bool generateVerilogStub(const QString &stubName, const QStringList &moduleNames);

    /**
     * @brief Generate Liberty timing library stub file for selected modules.
     * @details This function generates a Liberty (.lib) timing library stub file
     *          containing timing information for the specified modules.
     * @param stubName Base name for the output Liberty file.
     * @param moduleNames List of module names to include in the stub.
     * @retval true Liberty stub file generated successfully.
     * @retval false Failed to generate Liberty stub file.
     */
    bool generateLibStub(const QString &stubName, const QStringList &moduleNames);

private:
    /**
     * @brief Process link and uplink connections in the netlist
     * @return true if successful, false on error
     */
    bool processLinkConnections();

    /**
     * @brief Process a single link connection
     * @param instanceName The instance name
     * @param portName The port name
     * @param netName The net name to create/connect to
     * @param moduleName The module name
     * @param moduleData The module YAML data
     * @return true if successful, false on error
     */
    bool processLinkConnection(
        const std::string &instanceName,
        const std::string &portName,
        const std::string &netName,
        const std::string &moduleName,
        const YAML::Node  &moduleData);

    /**
     * @brief Process a single uplink connection
     * @param instanceName The instance name
     * @param portName The port name
     * @param netName The net name to create/connect to
     * @param moduleName The module name
     * @param moduleData The module YAML data
     * @return true if successful, false on error
     */
    bool processUplinkConnection(
        const std::string &instanceName,
        const std::string &portName,
        const std::string &netName,
        const std::string &moduleName,
        const YAML::Node  &moduleData);

    /**
     * @brief Parse link value to extract net name and bit selection
     * @param linkValue The link value (e.g., "bus_data[7:0]", "clk_signal[3]", "simple_net")
     * @return A pair containing the net name and bit selection (empty if no selection)
     */
    std::pair<std::string, std::string> parseLinkValue(const std::string &linkValue);

    /**
     * @brief Calculate the width of a port from its type string
     * @param portType The port type string (e.g., "wire [7:0]", "reg [15:0]", "wire")
     * @return The width in bits, or -1 if cannot be determined
     */
    int calculatePortWidth(const std::string &portType);

    /**
     * @brief Process combinational logic section in the netlist
     * @return true if successful, false on error
     */
    bool processCombLogic();

    /**
     * @brief Generate combinational logic using Comb primitive
     * @param netlistData YAML node containing the full netlist
     * @param out Output text stream for generated Verilog
     * @return true if generation successful, false otherwise
     */
    bool generateCombPrimitive(const YAML::Node &netlistData, QTextStream &out);

    /**
     * @brief Process sequential logic section in the netlist
     * @return true if successful, false on error
     */
    bool processSeqLogic();

    /**
     * @brief Generate FSM Verilog code using FSM primitive
     * @param fsmItem The YAML node containing the FSM specification
     * @param out Output text stream
     * @return true if generation successful, false otherwise
     */
    bool generateFSMPrimitive(const YAML::Node &fsmItem, QTextStream &out);

    /**
     * @brief Generate reset primitive controller from YAML configuration
     * @param resetNode The YAML node containing reset configuration
     * @param out Output text stream
     * @return true if generation successful, false otherwise
     */
    bool generateResetPrimitive(const YAML::Node &resetNode, QTextStream &out);

    /**
     * @brief Generate clock control logic using Clock primitive
     * @param clockNode YAML node containing clock configuration
     * @param out Output text stream
     * @return true if generation successful, false otherwise
     */
    bool generateClockPrimitive(const YAML::Node &clockNode, QTextStream &out);

    /**
     * @brief Generate power control logic using Power primitive
     * @param powerNode YAML node containing power configuration
     * @param out Output text stream
     * @return true if generation successful, false otherwise
     */
    bool generatePowerPrimitive(const YAML::Node &powerNode, QTextStream &out);

    /**
     * @brief Generate sequential logic using Seq primitive
     * @param netlistData YAML node containing the full netlist
     * @param out Output text stream for generated Verilog
     * @return true if generation successful, false otherwise
     */
    bool generateSeqPrimitive(const YAML::Node &netlistData, QTextStream &out);

    /**
     * @brief Parse and validate ifdef/ifndef conditions from instance data
     * @param instanceData YAML node containing instance configuration
     * @param instanceName Instance name for error messages
     * @param outIfdef Output list of macros that must be defined (sorted alphabetically)
     * @param outIfndef Output list of macros that must be undefined (sorted alphabetically)
     * @return true if validation successful, false if conflict detected
     */
    bool parseMacroCondition(
        const YAML::Node &instanceData,
        const QString    &instanceName,
        QStringList      &outIfdef,
        QStringList      &outIfndef);

    /**
     * @brief Write ifdef/ifndef directives to output stream
     * @param out Output text stream
     * @param ifdef List of macros that must be defined (already sorted)
     * @param ifndef List of macros that must be undefined (already sorted)
     * @param indent Indentation string (default: "    ")
     */
    void writeIfdefBegin(
        QTextStream       &out,
        const QStringList &ifdef,
        const QStringList &ifndef,
        const QString     &indent = "    ");

    /**
     * @brief Write endif directives with proper comments to output stream
     * @param out Output text stream
     * @param ifdef List of macros that were defined (already sorted, same order as begin)
     * @param ifndef List of macros that were undefined (already sorted, same order as begin)
     * @param indent Indentation string (default: "    ")
     */
    void writeIfdefEnd(
        QTextStream       &out,
        const QStringList &ifdef,
        const QStringList &ifndef,
        const QString     &indent = "    ");

    /** Project manager. */
    QSocProjectManager *projectManager = nullptr;
    /** Module manager. */
    QSocModuleManager *moduleManager = nullptr;
    /** Bus manager. */
    QSocBusManager *busManager = nullptr;
    /** LLM service. */
    QLLMService *llmService = nullptr;
    /** Primitive generators. */
    QSocResetPrimitive *resetPrimitive = nullptr;
    QSocClockPrimitive *clockPrimitive = nullptr;
    QSocPowerPrimitive *powerPrimitive = nullptr;
    QSocFSMPrimitive   *fsmPrimitive   = nullptr;
    QSocCombPrimitive  *combPrimitive  = nullptr;
    QSocSeqPrimitive   *seqPrimitive   = nullptr;
    /** Force overwrite mode for primitive cell files */
    bool forceOverwrite = false;
    /** Netlist data. */
    YAML::Node netlistData;
};

#endif // QSOCGENERATEMANAGER_H
