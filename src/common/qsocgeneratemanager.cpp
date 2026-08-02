// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "common/qsocgeneratemanager.h"
#include "common/qsocconsole.h"
#include "common/qsocgenerateprimitiveclock.h"
#include "common/qsocgenerateprimitivecomb.h"
#include "common/qsocgenerateprimitivefsm.h"
#include "common/qsocgenerateprimitivepower.h"
#include "common/qsocgenerateprimitivereset.h"
#include "common/qsocgenerateprimitiveseq.h"
#include "common/qsocverilogutils.h"
#include "common/qstaticstringweaver.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QTextStream>

#include <fstream>
#include <iostream>

QSocGenerateManager::QSocGenerateManager(
    QObject            *parent,
    QSocProjectManager *projectManager,
    QSocModuleManager  *moduleManager,
    QSocBusManager     *busManager,
    QLLMService        *llmService)
    : QObject(parent)
    , projectManager(projectManager)
    , moduleManager(moduleManager)
    , busManager(busManager)
    , llmService(llmService)
    , resetPrimitive(new QSocResetPrimitive(this))
    , clockPrimitive(new QSocClockPrimitive(this))
    , powerPrimitive(new QSocPowerPrimitive(this))
    , fsmPrimitive(new QSocFSMPrimitive(this))
    , combPrimitive(new QSocCombPrimitive(this))
    , seqPrimitive(new QSocSeqPrimitive(this))
{
    /* All private members set by constructor */
}

QSocGenerateManager::~QSocGenerateManager()
{
    delete resetPrimitive;
    delete clockPrimitive;
    delete powerPrimitive;
    delete fsmPrimitive;
    delete combPrimitive;
    delete seqPrimitive;
}

void QSocGenerateManager::setProjectManager(QSocProjectManager *projectManager)
{
    this->projectManager = projectManager;
}

void QSocGenerateManager::setModuleManager(QSocModuleManager *moduleManager)
{
    this->moduleManager = moduleManager;
}

void QSocGenerateManager::setBusManager(QSocBusManager *busManager)
{
    this->busManager = busManager;
}

void QSocGenerateManager::setLLMService(QLLMService *llmService)
{
    this->llmService = llmService;
}

QSocProjectManager *QSocGenerateManager::getProjectManager()
{
    return projectManager;
}

QSocModuleManager *QSocGenerateManager::getModuleManager()
{
    return moduleManager;
}

QSocBusManager *QSocGenerateManager::getBusManager()
{
    return busManager;
}

QLLMService *QSocGenerateManager::getLLMService()
{
    return llmService;
}

void QSocGenerateManager::setForceOverwrite(bool force)
{
    /* Propagate force setting to all primitive generators. */
    if (clockPrimitive) {
        clockPrimitive->setForceOverwrite(force);
    }
    if (resetPrimitive) {
        resetPrimitive->setForceOverwrite(force);
    }
    if (powerPrimitive) {
        powerPrimitive->setForceOverwrite(force);
    }
}

QString QSocGenerateManager::cleanTypeForWireDeclaration(const QString &typeStr)
{
    if (typeStr.isEmpty()) {
        return {};
    }

    QString cleaned = typeStr;

    /* Remove leading whitespace + keyword + keyword trailing whitespace */
    static const QRegularExpression regularExpression(R"(\s*[A-Za-z_]+\s*(?=\[|\s*$))");
    /* Explanation:
     *   \s*           optional leading whitespace
     *   [A-Za-z_]+    keyword (only letters and underscores)
     *   \s*           whitespace after keyword
     *   (?=\[|\s*$)   only match when followed by '[' or whitespace until end of line
     */
    cleaned.replace(regularExpression, "");

    /* Clean up any remaining whitespace */
    cleaned = cleaned.trimmed();

    return cleaned;
}

QString QSocGenerateManager::topPortDirection(const YAML::Node &netlistData, const QString &portName)
{
    if (!netlistData["port"] || !netlistData["port"][portName.toStdString()]) {
        return {};
    }
    const YAML::Node node = netlistData["port"][portName.toStdString()];
    if (!node["direction"] || !node["direction"].IsScalar()) {
        return QStringLiteral("input");
    }
    const QString dir = QString::fromStdString(node["direction"].as<std::string>()).toLower();
    if (dir == "out" || dir == "output") {
        return QStringLiteral("output");
    }
    if (dir == "in" || dir == "input") {
        return QStringLiteral("input");
    }
    if (dir == "inout") {
        return QStringLiteral("inout");
    }
    return QStringLiteral("input");
}

namespace {

bool isOutputTopPort(const YAML::Node &netlistData, const QString &portName)
{
    if (!netlistData["port"] || !netlistData["port"][portName.toStdString()]) {
        return false;
    }
    const YAML::Node node = netlistData["port"][portName.toStdString()];
    if (!node["direction"] || !node["direction"].IsScalar()) {
        return false;
    }
    const QString dir = QString::fromStdString(node["direction"].as<std::string>()).toLower();
    return dir == "out" || dir == "output";
}

} // namespace

QMap<QString, QSocGenerateManager::NetTopPorts> QSocGenerateManager::buildNetToTopPorts(
    const YAML::Node &netlistData)
{
    QStringList   topPortNames;
    QSet<QString> topPortNameSet;
    if (netlistData["port"] && netlistData["port"].IsMap()) {
        for (const auto &portEntry : netlistData["port"]) {
            if (!portEntry.first.IsScalar()) {
                continue;
            }
            const QString portName = QString::fromStdString(portEntry.first.as<std::string>());
            if (topPortNameSet.contains(portName)) {
                continue;
            }
            topPortNameSet.insert(portName);
            topPortNames.append(portName);
        }
    }

    /* Collect both binding spellings against the port before ordering, so the
       result follows port declaration order rather than the order the two
       spellings happen to be scanned in. */
    QMap<QString, QList<QPair<QString, QString>>> portToNets;
    const auto                                    addBinding =
        [&portToNets](const QString &portName, const QString &netName, const QString &slice) {
            QList<QPair<QString, QString>> &bindings = portToNets[portName];
            for (const auto &existing : bindings) {
                if (existing.first == netName && existing.second == slice) {
                    return;
                }
            }
            bindings.append({netName, slice});
        };

    for (const QString &portName : topPortNames) {
        const YAML::Node portNode = netlistData["port"][portName.toStdString()];
        if (!portNode || !portNode.IsMap()) {
            continue;
        }
        if (!portNode["connect"] || !portNode["connect"].IsScalar()) {
            continue;
        }
        const QString netName = QString::fromStdString(portNode["connect"].as<std::string>());
        if (!netName.isEmpty()) {
            addBinding(portName, netName, QString());
        }
    }

    /* Explicit `instance: top` entries bind the same way as `connect:`, except
       that they may carry a `bits` slice. */
    if (netlistData["net"] && netlistData["net"].IsMap()) {
        for (const auto &netEntry : netlistData["net"]) {
            if (!netEntry.first.IsScalar() || !netEntry.second.IsSequence()) {
                continue;
            }
            const QString netName = QString::fromStdString(netEntry.first.as<std::string>());
            for (const auto &connectionNode : netEntry.second) {
                if (!connectionNode.IsMap()) {
                    continue;
                }
                if (!connectionNode["instance"] || !connectionNode["instance"].IsScalar()) {
                    continue;
                }
                if (QString::fromStdString(connectionNode["instance"].as<std::string>()) != "top") {
                    continue;
                }
                if (!connectionNode["port"] || !connectionNode["port"].IsScalar()) {
                    continue;
                }
                const QString portName = QString::fromStdString(
                    connectionNode["port"].as<std::string>());
                if (!topPortNameSet.contains(portName)) {
                    continue;
                }
                QString slice;
                if (connectionNode["bits"] && connectionNode["bits"].IsScalar()) {
                    slice = QSocVerilogUtils::normalizeBitSelect(
                        QString::fromStdString(connectionNode["bits"].as<std::string>()));
                }
                addBinding(portName, netName, slice);
            }
        }
    }

    /* A whole-port binding makes the port and the net one signal, and that
       relation is transitive: chained `connect:` spellings name a single net.
       Grouping per binding instead of per component lets one port be the
       carrier of one group and a member of another, and both groups then
       emit an alias assignment onto it. */
    QMap<QString, QString> parent;
    const auto             find = [&parent](QString name) {
        while (parent.value(name, name) != name) {
            name = parent.value(name, name);
        }
        return name;
    };
    const auto unite = [&parent, &find](const QString &lhs, const QString &rhs) {
        const QString lhsRoot = find(lhs);
        const QString rhsRoot = find(rhs);
        if (lhsRoot != rhsRoot) {
            parent.insert(lhsRoot, rhsRoot);
        }
    };
    for (const QString &portName : topPortNames) {
        for (const auto &binding : portToNets.value(portName)) {
            if (binding.second.isEmpty()) {
                unite(portName, binding.first);
            }
        }
    }

    /* One ordered member list per component, then shared by every net name in
       it, so all spellings of the same signal resolve to one carrier. A port
       reached only because a net carries its name belongs to the component
       even though it declares no binding itself. */
    QSet<QString> boundRoots;
    for (const QString &portName : topPortNames) {
        for (const auto &binding : portToNets.value(portName)) {
            if (binding.second.isEmpty()) {
                boundRoots.insert(find(portName));
            }
        }
    }
    QMap<QString, QStringList> componentMembers;
    for (const QString &portName : topPortNames) {
        const QString root = find(portName);
        if (boundRoots.contains(root)) {
            componentMembers[root].append(portName);
        }
    }

    /* Whole-port members come from the component, so every net name in it
       reports the same ordered list and the same carrier. Partial bindings are
       kept apart: they bind a slice of a port, never the whole net, and mixing
       them into the member list lets a consumer mistake one for the carrier. */
    QMap<QString, NetTopPorts> netToTopPorts;
    for (const QString &portName : topPortNames) {
        for (const auto &binding : portToNets.value(portName)) {
            NetTopPorts &entry = netToTopPorts[binding.first];
            if (binding.second.isEmpty()) {
                entry.members = componentMembers.value(find(binding.first));
            } else {
                entry.slices.append(
                    {portName,
                     binding.second,
                     QSocGenerateManager::topPortDirection(netlistData, portName)});
            }
        }
    }

    /* A net spelled with the name of a component member is that member's
       signal, so its drivers belong to the same connected component. */
    if (netlistData["net"] && netlistData["net"].IsMap()) {
        for (const auto &netEntry : netlistData["net"]) {
            if (!netEntry.first.IsScalar()) {
                continue;
            }
            const QString netName = QString::fromStdString(netEntry.first.as<std::string>());
            if (netToTopPorts.contains(netName)) {
                continue;
            }
            if (topPortNameSet.contains(netName) && boundRoots.contains(find(netName))) {
                netToTopPorts[netName].members = componentMembers.value(find(netName));
            }
        }
    }

    return netToTopPorts;
}

QString QSocGenerateManager::composeBitSelect(
    const QString &boundSlice, const QString &innerSlice, const QString &context)
{
    if (boundSlice.isEmpty()) {
        return innerSlice;
    }
    if (innerSlice.isEmpty()) {
        return boundSlice;
    }
    static const QRegularExpression rangeRegex(R"(^\[(\d+)(?::(\d+))?\]$)");
    const QRegularExpressionMatch   boundMatch = rangeRegex.match(boundSlice);
    const QRegularExpressionMatch   innerMatch = rangeRegex.match(innerSlice);
    if (!boundMatch.hasMatch() || !innerMatch.hasMatch()) {
        QSocConsole::warn() << context << "cannot compose select" << innerSlice << "with binding"
                            << boundSlice << "- using the inner select";
        return innerSlice;
    }
    const int boundMsb = boundMatch.captured(1).toInt();
    const int boundLsb = boundMatch.capturedLength(2) > 0 ? boundMatch.captured(2).toInt()
                                                          : boundMsb;
    const int lo       = boundMsb <= boundLsb ? boundMsb : boundLsb;
    const int hi       = boundMsb <= boundLsb ? boundLsb : boundMsb;
    const int innerMsb = innerMatch.captured(1).toInt();
    const int innerLsb = innerMatch.capturedLength(2) > 0 ? innerMatch.captured(2).toInt()
                                                          : innerMsb;
    const int width    = hi - lo + 1;
    if (innerMsb >= width || innerLsb >= width) {
        QSocConsole::warn() << context << "select" << innerSlice << "exceeds the" << width
                            << "bits bound by" << boundSlice;
    }
    if (innerMatch.capturedLength(2) > 0) {
        return QString("[%1:%2]").arg(lo + innerMsb).arg(lo + innerLsb);
    }
    return QString("[%1]").arg(lo + innerMsb);
}

QMap<QString, QSocGenerateManager::TopPortBinding> QSocGenerateManager::buildTopPortRedirect(
    const YAML::Node &netlistData)
{
    const QMap<QString, NetTopPorts> netToTopPorts = buildNetToTopPorts(netlistData);

    QMap<QString, TopPortBinding> redirect;
    QMap<QString, QString>        outputMemberRedirect;
    for (auto netIt = netToTopPorts.constBegin(); netIt != netToTopPorts.constEnd(); ++netIt) {
        const NetTopPorts &entry = netIt.value();

        /* A process or instance driver must land on a sink, so the head is
           the first output member, then the first output slice; declaration
           order only orders equivalent sinks. */
        TopPortBinding
            head{entry.members.isEmpty() ? QString() : entry.members.first(), QString(), QString()};
        bool headFound = false;
        for (const QString &member : entry.members) {
            if (isOutputTopPort(netlistData, member)) {
                head      = {member, QString(), QStringLiteral("output")};
                headFound = true;
                break;
            }
        }
        if (!headFound) {
            for (const TopPortBinding &binding : entry.slices) {
                if (binding.direction == QStringLiteral("output")) {
                    head      = binding;
                    headFound = true;
                    break;
                }
            }
        }
        if (!headFound && entry.members.isEmpty()) {
            if (entry.slices.isEmpty()) {
                continue;
            }
            head = entry.slices.first();
        }
        redirect.insert(netIt.key(), head);

        if (entry.members.isEmpty()) {
            continue;
        }
        const QString canonical = head.slice.isEmpty() ? head.port : entry.members.first();

        /* Only an all-output group gets an alias assignment, and only then is a
           secondary member already driven by the canonical one. Folding the
           members of a mixed-direction group would redirect a legal output
           target onto an input port. */
        bool allOutputs = true;
        for (const QString &member : entry.members) {
            if (!isOutputTopPort(netlistData, member)) {
                allOutputs = false;
                break;
            }
        }
        if (!allOutputs) {
            continue;
        }
        for (int index = 1; index < entry.members.size(); ++index) {
            const QString &member = entry.members.at(index);
            outputMemberRedirect.insert(member, canonical);
            redirect.insert(member, {canonical, QString()});
        }
    }

    /* A sliced binding may name a secondary whole-port member. Keep its
       interval, but land it on the same canonical signal as every other
       consumer of the component. */
    for (auto redirectIt = redirect.begin(); redirectIt != redirect.end(); ++redirectIt) {
        const QString canonical = outputMemberRedirect.value(redirectIt->port);
        if (!canonical.isEmpty()) {
            redirectIt->port = canonical;
        }
    }

    return redirect;
}
