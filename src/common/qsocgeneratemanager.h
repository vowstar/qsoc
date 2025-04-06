// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOCGENERATEMANAGER_H
#define QSOCGENERATEMANAGER_H

#include "common/qllmservice.h"
#include "common/qsocbusmanager.h"
#include "common/qsocconfig.h"
#include "common/qsocmodulemanager.h"
#include "common/qsocprojectmanager.h"

#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>

#include <BigIntegerLibrary.h>
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
     * @brief Helper function to convert BigInteger to string with a specified base
     * @param value BigInteger value to convert
     * @param base Base for conversion (2, 8, 10, 16)
     * @return String representation of the BigInteger in the specified base
     */
    static std::string bigIntegerToStringWithBase(const BigInteger &value, int base)
    {
        std::string result;
        if (value.getSign() == BigInteger::negative) {
            result = "-";
            result += std::string(BigUnsignedInABase(value.getMagnitude(), base));
        } else {
            result = std::string(BigUnsignedInABase(value.getMagnitude(), base));
        }
        return result;
    }

    /**
     * @brief Helper function to convert string to BigInteger with a specified base
     * @param str String to convert
     * @param base Base of the input string (2, 8, 10, 16)
     * @return BigInteger parsed from the string
     */
    static BigInteger stringToBigIntegerWithBase(const std::string &str, int base)
    {
        BigUnsigned result(0);
        BigUnsigned baseVal(base);

        for (char c : str) {
            int digit;
            if (c >= '0' && c <= '9') {
                digit = c - '0';
            } else if (c >= 'a' && c <= 'f') {
                digit = c - 'a' + 10;
            } else if (c >= 'A' && c <= 'F') {
                digit = c - 'A' + 10;
            } else {
                /* Skip invalid characters */
                continue;
            }

            if (digit >= base) {
                /* Skip invalid digits for this base */
                continue;
            }

            result = result * baseVal + BigUnsigned(digit);
        }

        return BigInteger(result);
    }

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
    enum class PortDirectionStatus {
        Valid,     /**< Consistent port directions */
        Undriven,  /**< Net has only input ports */
        Multidrive /**< Net has multiple output/inout ports */
    };
    Q_ENUM(PortDirectionStatus)

    /**
     * @brief Enum to distinguish between module ports and top-level ports
     */
    enum class PortType {
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

        PortConnection(PortType t, const QString &inst, const QString &port)
            : type(t)
            , instanceName(inst)
            , portName(port)
        {}

        static PortConnection createModulePort(const QString &inst, const QString &port)
        {
            return PortConnection(PortType::Module, inst, port);
        }

        static PortConnection createTopLevelPort(const QString &port)
        {
            return PortConnection(PortType::TopLevel, "", port);
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

        PortDetailInfo(
            PortType       t,
            const QString &inst,
            const QString &port,
            const QString &w,
            const QString &dir)
            : type(t)
            , instanceName(inst)
            , portName(port)
            , width(w)
            , direction(dir)
        {}

        static PortDetailInfo createModulePort(
            const QString &inst, const QString &port, const QString &w, const QString &dir)
        {
            return PortDetailInfo(PortType::Module, inst, port, w, dir);
        }

        static PortDetailInfo createTopLevelPort(
            const QString &port, const QString &w, const QString &dir)
        {
            return PortDetailInfo(PortType::TopLevel, "", port, w, dir);
        }
    };

    /**
     * @brief NumberInfo structure to represent numeric literals with format information
     */
    struct NumberInfo
    {
        enum class Base {
            Binary      = 2,  /**< Base-2 (binary) number representation */
            Octal       = 8,  /**< Base-8 (octal) number representation */
            Decimal     = 10, /**< Base-10 (decimal) number representation */
            Hexadecimal = 16, /**< Base-16 (hexadecimal) number representation */
            Unknown     = 0   /**< Unknown or undefined numeric base */
        };

        QString    originalString;   /**< Original string representation */
        Base       base;             /**< Numeric base (2, 8, 10, 16) */
        BigInteger value;            /**< Actual numeric value */
        int        width;            /**< Bit width (either specified or calculated) */
        bool       hasExplicitWidth; /**< Whether width was explicitly specified */
        bool errorDetected; /**< Whether the number is too large for quint64 or parsing failed */

        /**
         * @brief Format the value according to its base
         * @return Formatted string (without width prefix)
         */
        QString format() const
        {
            /* If error was detected, return original string */
            if (errorDetected) {
                return originalString;
            }

            switch (base) {
            case Base::Binary:
                return QString("'b%1").arg(
                    QString::fromStdString(bigIntegerToStringWithBase(value, 2)));
            case Base::Octal:
                return QString("'o%1").arg(
                    QString::fromStdString(bigIntegerToStringWithBase(value, 8)));
            case Base::Decimal:
                return QString("'d%1").arg(
                    QString::fromStdString(bigIntegerToStringWithBase(value, 10)));
            case Base::Hexadecimal: {
                QString hexStr = QString::fromStdString(bigIntegerToStringWithBase(value, 16));
                /* Always use lowercase for hex values regardless of original casing */
                return QString("'h%1").arg(hexStr.toLower());
            }
            default:
                return QString::fromStdString(bigIntegerToStringWithBase(value, 10));
            }
        }

        /**
         * @brief Format the value with width prefix according to Verilog conventions
         * @return Complete Verilog-style formatted number string
         */
        QString formatVerilog() const
        {
            /* If error was detected, use the original string to preserve large values */
            if (errorDetected) {
                return originalString;
            }

            if (width > 0) {
                return QString("%1%2").arg(width).arg(format());
            } else {
                return format();
            }
        }

        /**
         * @brief Format the value in C-style syntax
         * @return C-style formatted number string
         */
        QString formatC() const
        {
            /* If error was detected, return original string */
            if (errorDetected) {
                return originalString;
            }

            switch (base) {
            case Base::Binary:
                return QString("0b%1").arg(
                    QString::fromStdString(bigIntegerToStringWithBase(value, 2)));
            case Base::Octal:
                return QString("0%1").arg(
                    QString::fromStdString(bigIntegerToStringWithBase(value, 8)));
            case Base::Hexadecimal: {
                QString hexStr = QString::fromStdString(bigIntegerToStringWithBase(value, 16));
                /* Always use lowercase for hex values regardless of original casing */
                return QString("0x%1").arg(hexStr.toLower());
            }
            default:
                return QString::fromStdString(bigIntegerToStringWithBase(value, 10));
            }
        }

        /**
         * @brief Format the value with proper bit width (padded zeros)
         * @return Formatted string with proper bit width
         */
        QString formatWithBitWidth() const
        {
            /* If error was detected, return original string */
            if (errorDetected) {
                return originalString;
            }

            QString result;

            switch (base) {
            case Base::Binary: {
                std::string binStr = bigIntegerToStringWithBase(value, 2);
                result             = QString::fromStdString(binStr).rightJustified(width, '0');
                return QString("%1'b%2").arg(width).arg(result);
            }
            case Base::Octal: {
                /* Calculate how many octal digits are needed */
                int         octalDigits = (width + 2) / 3; /* Ceiling division */
                std::string octStr      = bigIntegerToStringWithBase(value, 8);
                result = QString::fromStdString(octStr).rightJustified(octalDigits, '0');
                return QString("%1'o%2").arg(width).arg(result);
            }
            case Base::Hexadecimal: {
                /* Calculate how many hex digits are needed */
                int         hexDigits = (width + 3) / 4; /* Ceiling division */
                std::string hexStr    = bigIntegerToStringWithBase(value, 16);
                /* Always use lowercase for hex values */
                result = QString::fromStdString(hexStr).rightJustified(hexDigits, '0').toLower();
                return QString("%1'h%2").arg(width).arg(result);
            }
            case Base::Decimal:
            default:
                return QString("%1'd%2").arg(width).arg(
                    QString::fromStdString(bigIntegerToStringWithBase(value, 10)));
            }
        }
    };

    /**
     * @brief Check port width consistency for a list of connections
     * @param connections List of port connections to check
     * @return Whether all ports have consistent width
     */
    bool checkPortWidthConsistency(const QList<PortConnection> &connections);

    /**
     * @brief Check port direction consistency for a list of connections
     * @param connections List of port connections to check
     * @return PortDirectionStatus Status of the connection (OK, Undriven, or Multidrive)
     */
    PortDirectionStatus checkPortDirectionConsistency(const QList<PortConnection> &connections);

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
    NumberInfo parseNumber(const QString &numStr);

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
     * @brief Format a Verilog file using the verible-verilog-format tool.
     * @details This function calls the external verible-verilog-format tool
     *          to format the generated Verilog file with standardized style settings.
     * @param filePath Path to the Verilog file to format.
     * @retval true File formatted successfully.
     * @retval false Failed to format file or tool not available.
     */
    bool formatVerilogFile(const QString &filePath);

private:
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