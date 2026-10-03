// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCRESOURCEFORMAT_H
#define QSOCRESOURCEFORMAT_H

#include <cmath>
#include <limits>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

namespace QSocResourceFormat {

inline QString text(QString value, int limit = 64)
{
    for (auto &character : value)
        if (!character.isPrint())
            character = QLatin1Char(' ');
    value = value.simplified();
    return value.isEmpty() ? QStringLiteral("unknown") : value.left(limit);
}

inline QString bytes(const QJsonValue &value)
{
    double number = value.toDouble(-1);
    if (!value.isDouble() || !std::isfinite(number) || number < 0
        || number > double((std::numeric_limits<qint64>::max)()))
        return QStringLiteral("unknown");
    const QStringList units{"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
    int               unit = 0;
    while (number >= 1024 && unit + 1 < units.size()) {
        number /= 1024;
        ++unit;
    }
    return QString::number(number, 'f', unit ? 1 : 0) + ' ' + units.at(unit);
}

inline QString count(const QJsonValue &value)
{
    const auto number = value.toInteger(-1);
    return number < 0 ? QStringLiteral("unknown") : QString::number(number);
}

inline QString summary(const QJsonObject &report, qint64 daemonPid)
{
    const auto  system    = report.value("system").toObject();
    const auto  processes = report.value("processes").toArray();
    QJsonObject daemon;
    for (const auto &value : processes) {
        const auto process = value.toObject();
        if (daemonPid > 0 && process.value("pid").toInteger(-1) == daemonPid) {
            daemon = process;
            break;
        }
    }
    const auto    smt       = report.value("smt").toObject();
    const QString available = bytes(system.value("memory_available_bytes"));
    const QString estimate  = available != "unknown"
                                      && system.value("memory_available_kind") == "estimate"
                                  ? QStringLiteral(" (estimate)")
                                  : QString();
    const auto    cpu       = daemon.value("cpu_time_ns").toInteger(-1);
    const QString cpuTime   = cpu < 0 ? QStringLiteral("unknown")
                                      : QString::number(double(cpu) / 1000000000.0, 'f', 2) + " s";
    QStringList   lines{
        "Resources: " + text(report.value("status").toString()) + " (local daemon)",
        "Host RAM: " + available + " available" + estimate + " / "
            + bytes(system.value("memory_total_bytes")) + " total",
        "Daemon RSS: " + bytes(daemon.value("resident_bytes")) + " | CPU time: " + cpuTime,
        "Observed descendants: "
            + (daemon.isEmpty() ? QStringLiteral("unknown") : QString::number(processes.size() - 1)),
        "SMT: " + count(smt.value("active_count")) + " active, " + count(smt.value("queued_count"))
            + " queued, " + text(smt.value("admission").toString())};
    const auto storage = report.value("storage").toArray();
    for (int index = 0; index < storage.size() && index < 8; ++index) {
        const auto volume = storage.at(index).toObject();
        lines.append(
            "Disk " + text(volume.value("path").toString(), 40) + ": "
            + (volume.value("valid").toBool() ? bytes(volume.value("available_bytes"))
                                              : QStringLiteral("unknown"))
            + " available" + (volume.value("read_only").toBool() ? " (read-only)" : ""));
    }
    if (!report.value("reason").toString().isEmpty())
        lines.append("Reason: " + text(report.value("reason").toString()));
    return lines.join('\n') + '\n';
}

} // namespace QSocResourceFormat

#endif
