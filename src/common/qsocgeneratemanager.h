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
     * @brief Enum to distinguish between module ports and top-level ports
     */
    enum class PortType : std::uint8_t {
        Module,  /**< Instance/module port */
        TopLevel /**< Top-level port */
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
    };

    /**
     * @brief Check port direction consistency for a list of connections
     * @param connections List of port connections to check
     * @return PortDirectionStatus Status of the connection (OK, Undriven, or Multidrive)
     */
    PortDirectionStatus checkPortDirectionConsistency(const QList<PortConnection> &connections);

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
     * @brief Parse a Verilog or C-style numeric literal
     *
     * Handles formats like:
     * - Standard: 123, 0xFF, 0644
     * - Verilog: 8'b10101010, 32'hDEADBEEF
     * - With underscores: 32'h1234_5678, 16'b1010_1010
     *
     * If width is not specified, calculates a reasonable width based on the value.
     *
     * @param numStr Input string containing the numeric literal
     * @return NumberInfo struct with parsed information
     */
    QSocNumberInfo parseNumber(const QString &numStr);

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
     * @brief Load netlist file.
     * @details Loads a netlist file and creates an in-memory representation.
     * @param netlistFilePath Path to the netlist file.
     * @retval true Netlist file loaded successfully.
     * @retval false Failed to load netlist file.
     */
    bool loadNetlist(const QString &netlistFilePath);

    /**
     * @brief Process and expand the netlist.
     * @details Processes the loaded netlist, expanding buses into individual signals.
     * @retval true Netlist processed successfully.
     * @retval false Failed to process netlist.
     */
    bool processNetlist();

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
     * @details Loads data from CSV, YAML, and JSON files, then renders a Jinja2 template
     *          and saves the result to the output directory.
     * @param templateFilePath Path to the Jinja2 template file.
     * @param csvFiles List of CSV data files to load.
     * @param yamlFiles List of YAML data files to load.
     * @param jsonFiles List of JSON data files to load.
     * @param outputFileName Output file name (without extension).
     * @retval true Template rendered and saved successfully.
     * @retval false Failed to render or save template.
     */
    bool renderTemplate(
        const QString     &templateFilePath,
        const QStringList &csvFiles,
        const QStringList &yamlFiles,
        const QStringList &jsonFiles,
        const QString     &outputFileName);

    /**
     * @brief Format a Verilog file using the verible-verilog-format tool.
     * @details This function calls the external verible-verilog-format tool
     *          to format the generated Verilog file with standardized style settings.
     * @param filePath Path to the Verilog file to format.
     * @retval true File formatted successfully.
     * @retval false Failed to format file or tool not available.
     */
    bool formatVerilogFile(const QString &filePath);

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
     * @brief Calculate the width of a port from its type string
     * @param portType The port type string (e.g., "wire [7:0]", "reg [15:0]", "wire")
     * @return The width in bits, or -1 if cannot be determined
     */
    int calculatePortWidth(const std::string &portType);

    /** Project manager. */
    QSocProjectManager *projectManager = nullptr;
    /** Module manager. */
    QSocModuleManager *moduleManager = nullptr;
    /** Bus manager. */
    QSocBusManager *busManager = nullptr;
    /** LLM service. */
    QLLMService *llmService = nullptr;
    /** Netlist data. */
    YAML::Node netlistData;
};

#endif // QSOCGENERATEMANAGER_H
