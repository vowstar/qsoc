// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "qsocgeneratereportunconnected.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QMap>
#include <QSet>
#include <QTextStream>

QSocGenerateReportUnconnected::QSocGenerateReportUnconnected() = default;

QSocGenerateReportUnconnected::~QSocGenerateReportUnconnected() = default;

void QSocGenerateReportUnconnected::addUnconnectedPort(const UnconnectedPortInfo &info)
{
    m_unconnectedPorts.append(info);
}

bool QSocGenerateReportUnconnected::generateReport(
    const QString &outputPath, const QString &topModuleName)
{
    if (m_unconnectedPorts.isEmpty()) {
        // No unconnected ports to report
        return true;
    }

    const QString reportFileName = topModuleName + ".nc.rpt";
    const QString reportFilePath = QDir(outputPath).filePath(reportFileName);

    QFile reportFile(reportFilePath);
    if (!reportFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }

    QTextStream out(&reportFile);

    // Generate report content
    out << generateReportHeader(topModuleName);
    out << "\n";
    out << generateReportSummary();
    out << "\n";
    out << generateReportInstances();

    reportFile.close();
    return true;
}

void QSocGenerateReportUnconnected::clear()
{
    m_unconnectedPorts.clear();
}

int QSocGenerateReportUnconnected::getUnconnectedPortCount() const
{
    return m_unconnectedPorts.size();
}

int QSocGenerateReportUnconnected::getInstanceCount() const
{
    QSet<QString> uniqueInstances;
    for (const auto &port : m_unconnectedPorts) {
        uniqueInstances.insert(port.instanceName);
    }
    return uniqueInstances.size();
}

QString QSocGenerateReportUnconnected::generateReportHeader(const QString &topModuleName)
{
    QString     header;
    QTextStream stream(&header);

    stream << "# Unconnected port report - " << topModuleName << "\n";
    stream << "# Generated: " << QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss")
           << "\n";
    stream << "# Tool: " << QCoreApplication::applicationName() << " "
           << QCoreApplication::applicationVersion();

    return header;
}

QString QSocGenerateReportUnconnected::generateReportSummary() const
{
    QString     summary;
    QTextStream stream(&summary);

    stream << "summary:\n";
    stream << "  total_instance: " << getInstanceCount() << "\n";
    stream << "  total_port: " << getUnconnectedPortCount();

    return summary;
}

QString QSocGenerateReportUnconnected::generateReportInstances() const
{
    QString     instances;
    QTextStream stream(&instances);

    // Group ports by instance
    QMap<QString, QList<UnconnectedPortInfo>> instanceGroups;
    for (const auto &port : m_unconnectedPorts) {
        instanceGroups[port.instanceName].append(port);
    }

    stream << "instance:";

    // Generate output for each instance
    for (auto it = instanceGroups.constBegin(); it != instanceGroups.constEnd(); ++it) {
        const QString                    &instanceName = it.key();
        const QList<UnconnectedPortInfo> &ports        = it.value();

        if (ports.isEmpty()) {
            continue;
        }

        stream << "\n  " << instanceName << ":";
        stream << "\n    module: " << ports.first().moduleName;
        stream << "\n    port:";

        for (const auto &port : ports) {
            stream << "\n      " << port.portName << ":";
            stream << "\n        type: " << port.type;
            stream << "\n        direction: " << port.direction;
        }
    }

    return instances;
}
