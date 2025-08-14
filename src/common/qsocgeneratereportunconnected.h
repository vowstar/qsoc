// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOCGENERATEREPORTUNCONNECTED_H
#define QSOCGENERATEREPORTUNCONNECTED_H

#include <QDateTime>
#include <QList>
#include <QString>

/**
 * @brief Class for generating unconnected port reports in YAML format
 * @details This class collects information about unconnected ports during
 *          Verilog generation and outputs them in a structured YAML report
 */
class QSocGenerateReportUnconnected
{
public:
    /**
     * @brief Structure containing information about an unconnected port
     */
    struct UnconnectedPortInfo
    {
        QString instanceName; ///< Name of the module instance
        QString moduleName;   ///< Name of the module type
        QString portName;     ///< Name of the unconnected port
        QString direction;    ///< Port direction (input/output/inout)
        QString type;         ///< Port type (e.g. logic[7:0])
    };

    /**
     * @brief Constructor
     */
    QSocGenerateReportUnconnected();

    /**
     * @brief Destructor
     */
    ~QSocGenerateReportUnconnected();

    /**
     * @brief Add an unconnected port to the report
     * @param info Information about the unconnected port
     */
    void addUnconnectedPort(const UnconnectedPortInfo &info);

    /**
     * @brief Generate the unconnected port report file
     * @param outputPath Directory path where to save the report
     * @param topModuleName Name of the top-level module
     * @return true if report was successfully generated, false otherwise
     */
    bool generateReport(const QString &outputPath, const QString &topModuleName);

    /**
     * @brief Clear all collected unconnected port information
     */
    void clear();

    /**
     * @brief Get the total number of collected unconnected ports
     * @return Number of unconnected ports
     */
    int getUnconnectedPortCount() const;

    /**
     * @brief Get the number of instances with unconnected ports
     * @return Number of instances
     */
    int getInstanceCount() const;

private:
    QList<UnconnectedPortInfo> m_unconnectedPorts; ///< List of collected unconnected ports

    /**
     * @brief Generate the YAML report header with metadata
     * @param topModuleName Name of the top-level module
     * @return Header string
     */
    static QString generateReportHeader(const QString &topModuleName);

    /**
     * @brief Generate the summary section of the report
     * @return Summary string in YAML format
     */
    QString generateReportSummary() const;

    /**
     * @brief Generate the instances section of the report
     * @return Instances string in YAML format
     */
    QString generateReportInstances() const;
};

#endif // QSOCGENERATEREPORTUNCONNECTED_H
