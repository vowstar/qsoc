// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "common/qsocmodulemanager.h"
#include "common/qsocconsole.h"
#include "common/qsocverilogutils.h"
#include "common/qstaticregex.h"
#include "common/qstaticstringweaver.h"

#include <algorithm>
#include <fstream>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>

namespace {

const QSet<QString> kModuleFields       = {"parameter", "port", "bus", "library"};
const QSet<QString> kPortFields         = {"direction", "type", "visible", "group", "description"};
const QSet<QString> kParameterFields    = {"type", "value", "description"};
const QSet<QString> kBusInterfaceFields = {"bus", "mode", "mapping"};

bool isSafeBasename(const QString &name)
{
    if (name.isEmpty() || name == "." || name == "..") {
        return false;
    }
    const QString invalidChars = "\\/:*?\"<>|.";
    for (const QChar character : invalidChars) {
        if (name.contains(character)) {
            return false;
        }
    }
    return true;
}

QString scalarToString(const YAML::Node &node)
{
    if (!node || !node.IsScalar()) {
        return {};
    }
    return QString::fromStdString(node.as<std::string>());
}

void copyMapExcept(const YAML::Node &source, YAML::Node &target, const QSet<QString> &skipKeys)
{
    if (!source || !source.IsMap()) {
        return;
    }
    if (!target || !target.IsMap()) {
        target = YAML::Node(YAML::NodeType::Map);
    }
    for (YAML::const_iterator it = source.begin(); it != source.end(); ++it) {
        if (!it->first.IsScalar()) {
            continue;
        }
        const QString key = QString::fromStdString(it->first.as<std::string>());
        if (!skipKeys.contains(key)) {
            target[key.toStdString()] = YAML::Clone(it->second);
        }
    }
}

void copyMapInto(const YAML::Node &source, YAML::Node &target)
{
    copyMapExcept(source, target, {});
}

void setScalarIfNotEmpty(YAML::Node &node, const char *key, const QString &value)
{
    if (!value.isEmpty()) {
        node[key] = value.toStdString();
    }
}

bool hasModuleContent(const QSocModuleDefinition &definition)
{
    return definition.extraAttributes.size() > 0 || !definition.ports.isEmpty()
           || !definition.parameters.isEmpty() || !definition.busInterfaces.isEmpty();
}

QSocModuleProblem makeModuleProblem(
    QSocModuleProblemSeverity   severity,
    const QString              &code,
    const QSocModuleDefinition &definition,
    const QString              &section,
    const QString              &itemName,
    int                         row,
    const QString              &message)
{
    QSocModuleProblem problem;
    problem.severity    = severity;
    problem.code        = code;
    problem.libraryName = definition.libraryName;
    problem.moduleName  = definition.moduleName;
    problem.section     = section;
    problem.itemName    = itemName;
    problem.row         = row;
    problem.message     = message;
    return problem;
}

bool hasErrorProblems(const QList<QSocModuleProblem> &problems)
{
    for (const QSocModuleProblem &problem : problems) {
        if (problem.severity == QSocModuleProblemSeverity::Error) {
            return true;
        }
    }
    return false;
}

QString normalizedDirection(QString direction)
{
    direction = direction.trimmed().toLower();
    if (direction == "input")
        return "in";
    if (direction == "output")
        return "out";
    return direction;
}

bool directionMismatch(const QString &expected, const QString &actual)
{
    const QString normalizedExpected = normalizedDirection(expected);
    const QString normalizedActual   = normalizedDirection(actual);
    if (normalizedExpected.isEmpty() || normalizedActual.isEmpty())
        return false;
    return normalizedExpected != normalizedActual;
}

QSocBusDefinition findLoadedBusDefinition(
    const QSocBusManager *busManager, const QString &busName, bool *found)
{
    if (found)
        *found = false;
    if (!busManager || busName.isEmpty())
        return {};

    for (const QString &libraryName : busManager->listLoadedLibraries()) {
        if (!busManager->listBusesInLibrary(libraryName).contains(busName))
            continue;
        if (found)
            *found = true;
        return busManager->getBusDefinition(libraryName, busName);
    }
    return {};
}

QSocBusSignalMode busSignalMode(
    const QSocBusDefinition &definition, const QString &signal, const QString &mode)
{
    for (const QSocBusSignalMode &row : definition.rows) {
        if (row.signal != signal)
            continue;
        if (mode.isEmpty() || row.mode.compare(mode, Qt::CaseInsensitive) == 0)
            return row;
    }
    return {};
}

QSet<QString> busSignalsForMode(const QSocBusDefinition &definition, const QString &mode)
{
    QSet<QString> signalNames;
    for (const QSocBusSignalMode &row : definition.rows) {
        if (row.signal.isEmpty())
            continue;
        if (!mode.isEmpty() && row.mode.compare(mode, Qt::CaseInsensitive) != 0)
            continue;
        signalNames.insert(row.signal);
    }
    return signalNames;
}

QSet<QString> busModes(const QSocBusDefinition &definition)
{
    QSet<QString> modes;
    for (const QSocBusSignalMode &row : definition.rows) {
        if (!row.mode.isEmpty())
            modes.insert(row.mode);
    }
    return modes;
}

QString projectRelativeFilePath(QSocProjectManager *projectManager, const QString &filePath)
{
    if (!projectManager || !projectManager->isValidProjectPath())
        return QFileInfo(filePath).fileName();
    return QDir(projectManager->getProjectPath()).relativeFilePath(filePath);
}

QStringList scalarMapKeys(const YAML::Node &node)
{
    QStringList keys;
    if (!node || !node.IsMap())
        return keys;
    for (YAML::const_iterator it = node.begin(); it != node.end(); ++it) {
        if (it->first.IsScalar())
            keys.append(QString::fromStdString(it->first.as<std::string>()));
    }
    keys.sort(Qt::CaseInsensitive);
    return keys;
}

void appendNetlistModuleUsages(
    const YAML::Node       &root,
    const QString          &moduleName,
    const QString          &filePath,
    QList<QSocModuleUsage> *usages)
{
    if (!root || !root.IsMap() || !root["instance"] || !root["instance"].IsMap() || !usages)
        return;

    for (YAML::const_iterator it = root["instance"].begin(); it != root["instance"].end(); ++it) {
        if (!it->first.IsScalar() || !it->second.IsMap())
            continue;

        const QString referencedModule = scalarToString(it->second["module"]);
        if (referencedModule.isEmpty()
            || (!moduleName.isEmpty() && referencedModule != moduleName)) {
            continue;
        }

        QSocModuleUsage usage;
        usage.sourceType    = "netlist";
        usage.filePath      = filePath;
        usage.instanceName  = QString::fromStdString(it->first.as<std::string>());
        usage.moduleName    = referencedModule;
        usage.portNames     = scalarMapKeys(it->second["port"]);
        usage.busInterfaces = scalarMapKeys(it->second["bus"]);
        usage.status        = "Netlist reference";
        usages->append(usage);
    }
}

void fillUsageFromModuleYaml(QSocModuleUsage *usage, const YAML::Node &moduleYaml)
{
    if (!usage || !moduleYaml || !moduleYaml.IsMap())
        return;
    usage->portNames     = scalarMapKeys(moduleYaml["port"]);
    usage->busInterfaces = scalarMapKeys(moduleYaml["bus"]);
}

void appendSchematicModuleUsages(
    const YAML::Node       &node,
    const QString          &moduleName,
    const QString          &filePath,
    QList<QSocModuleUsage> *usages)
{
    if (!node || !usages)
        return;

    if (node.IsMap()) {
        const QString referencedModule = scalarToString(node["module_name"]);
        if (!referencedModule.isEmpty()
            && (moduleName.isEmpty() || referencedModule == moduleName)) {
            QSocModuleUsage usage;
            usage.sourceType   = "schematic";
            usage.filePath     = filePath;
            usage.instanceName = scalarToString(node["instance_name"]);
            usage.moduleName   = referencedModule;
            usage.status       = "Placed copy";
            if (usage.instanceName.isEmpty())
                usage.instanceName = referencedModule;

            const QString yamlText = scalarToString(node["module_yaml"]);
            if (!yamlText.isEmpty()) {
                try {
                    fillUsageFromModuleYaml(&usage, YAML::Load(yamlText.toStdString()));
                } catch (const YAML::Exception &) {
                    usage.status = "Placed copy with invalid module YAML";
                }
            }
            usages->append(usage);
        }

        for (YAML::const_iterator it = node.begin(); it != node.end(); ++it)
            appendSchematicModuleUsages(it->second, moduleName, filePath, usages);
        return;
    }

    if (node.IsSequence()) {
        for (YAML::const_iterator it = node.begin(); it != node.end(); ++it)
            appendSchematicModuleUsages(*it, moduleName, filePath, usages);
    }
}

} // namespace

QSocModuleManager::QSocModuleManager(
    QObject            *parent,
    QSocProjectManager *projectManager,
    QSocBusManager     *busManager,
    QLLMService        *llmService)
    : QObject(parent)
    , projectManager(projectManager)
    , busManager(busManager)
    , llmService(llmService)
    , slangDriver(new QSlangDriver(this, projectManager))
{
    /* All private members set by constructor */
}

QSocModuleManager::~QSocModuleManager() = default;

YAML::Node QSocModuleManager::mergeNodes(const YAML::Node &toYaml, const YAML::Node &fromYaml)
{
    if (!fromYaml.IsMap()) {
        /* If fromYaml is not a map, merge result is fromYaml, unless fromYaml is null */
        return fromYaml.IsNull() ? toYaml : fromYaml;
    }
    if (!toYaml.IsMap()) {
        /* If toYaml is not a map, merge result is fromYaml */
        return fromYaml;
    }
    if (!fromYaml.size()) {
        /* If toYaml is a map, and fromYaml is an empty map, return toYaml */
        return toYaml;
    }
    /* Create a new map 'resultYaml' with the same mappings as toYaml, merged with fromYaml */
    YAML::Node resultYaml = YAML::Node(YAML::NodeType::Map);
    for (auto iter : toYaml) {
        if (iter.first.IsScalar()) {
            const std::string &key      = iter.first.Scalar();
            auto               tempYaml = YAML::Node(fromYaml[key]);
            if (tempYaml) {
                resultYaml[iter.first] = mergeNodes(iter.second, tempYaml);
                continue;
            }
        }
        resultYaml[iter.first] = iter.second;
    }
    /* Add the mappings from 'fromYaml' not already in 'resultYaml' */
    for (auto iter : fromYaml) {
        if (!iter.first.IsScalar() || !resultYaml[iter.first.Scalar()]) {
            resultYaml[iter.first] = iter.second;
        }
    }
    return resultYaml;
}

void QSocModuleManager::libraryMapAdd(const QString &libraryName, const QString &moduleName)
{
    /* Check if the library exists in the map */
    if (!libraryMap.contains(libraryName)) {
        /* If the key doesn't exist, create a new QSet and insert the moduleName */
        QSet<QString> moduleSet;
        moduleSet.insert(moduleName);
        libraryMap.insert(libraryName, moduleSet);
    } else {
        /* If the key exists, just add the moduleName to the existing QSet */
        libraryMap[libraryName].insert(moduleName);
    }
}

void QSocModuleManager::libraryMapRemove(const QString &libraryName, const QString &moduleName)
{
    /* Check if the library exists in the map */
    if (libraryMap.contains(libraryName)) {
        QSet<QString> &modules = libraryMap[libraryName];

        /* Remove the module if it exists in the set */
        modules.remove(moduleName);

        /* If the set becomes empty after removal, delete the library entry */
        if (modules.isEmpty()) {
            libraryMap.remove(libraryName);
        }
    }
}

QString QSocModuleManager::activeLibraryForModule(const QString &moduleName) const
{
    for (auto it = libraryLoadOrder.crbegin(); it != libraryLoadOrder.crend(); ++it) {
        if (libraryMap.value(*it).contains(moduleName)) {
            return *it;
        }
    }
    return {};
}

void QSocModuleManager::rebuildActiveModule(const QString &moduleName)
{
    moduleData.remove(moduleName.toStdString());
    const QString libraryName = activeLibraryForModule(moduleName);
    if (libraryName.isEmpty()) {
        return;
    }
    YAML::Node moduleYaml = YAML::Clone(libraryData[libraryName][moduleName.toStdString()]);
    if (!moduleYaml || moduleYaml.IsNull()) {
        moduleYaml = YAML::Node(YAML::NodeType::Null);
    }
    moduleYaml["library"]                = libraryName.toStdString();
    moduleData[moduleName.toStdString()] = moduleYaml;
}

void QSocModuleManager::rememberLoadedLibrary(const QString &libraryName)
{
    libraryLoadOrder.removeAll(libraryName);
    libraryLoadOrder.append(libraryName);
}

void QSocModuleManager::setProjectManager(QSocProjectManager *projectManager)
{
    /* Set projectManager */
    if (projectManager) {
        this->projectManager = projectManager;
    }
}

void QSocModuleManager::setBusManager(QSocBusManager *busManager)
{
    /* Set busManager */
    if (busManager) {
        this->busManager = busManager;
    }
}

void QSocModuleManager::setLLMService(QLLMService *llmService)
{
    this->llmService = llmService;
}

QSocProjectManager *QSocModuleManager::getProjectManager()
{
    return projectManager;
}

QSocBusManager *QSocModuleManager::getBusManager()
{
    return busManager;
}

QLLMService *QSocModuleManager::getLLMService()
{
    return llmService;
}

bool QSocModuleManager::isModulePathValid()
{
    /* Validate projectManager */
    if (!projectManager) {
        QSocConsole::error() << "projectManager is nullptr.";
        return false;
    }
    /* Validate module path. */
    if (!projectManager->isValidModulePath()) {
        QSocConsole::error() << "Invalid module path:" << projectManager->getModulePath();
        return false;
    }
    return true;
}

void QSocModuleManager::resetModuleData()
{
    /* Clear the library map */
    libraryMap.clear();
    libraryData.clear();
    libraryLoadOrder.clear();
    /* Reset the module data by creating a new empty YAML node */
    moduleData = YAML::Node();

    QSocConsole::debug() << "Module data has been reset.";
}

bool QSocModuleManager::importFromFileList(
    const QString            &libraryName,
    const QRegularExpression &moduleNameRegex,
    const QString            &fileListPath,
    const QStringList        &filePathList,
    const QStringList        &macroDefines,
    const QStringList        &macroUndefines)
{
    /* Validate projectManager and its path */
    if (!isModulePathValid()) {
        QSocConsole::error() << "projectManager is null or invalid module path.";
        return false;
    }
    /* Validate moduleNameRegex */
    if (!QStaticRegex::isNameRegexValid(moduleNameRegex)) {
        QSocConsole::error() << "Invalid or empty regex:" << moduleNameRegex.pattern();
        return false;
    }

    if (slangDriver->parseFileList(fileListPath, filePathList, macroDefines, macroUndefines)) {
        /* Parse success */
        QStringList moduleList = slangDriver->getModuleList();
        if (moduleList.isEmpty()) {
            /* No module found */
            QSocConsole::error() << "no module found.";
            return false;
        }
        YAML::Node libraryYaml;
        QString    effectiveName = libraryName;

        if (moduleNameRegex.pattern().isEmpty()) {
            /* Pick first module if pattern is empty */
            const QString &moduleName = moduleList.first();
            QSocConsole::debug() << "Pick first module:" << moduleName;
            if (effectiveName.isEmpty()) {
                effectiveName = moduleName.toLower();
                QSocConsole::debug() << "Pick library filename:" << effectiveName;
            }
            const json       &moduleAst  = slangDriver->getModuleAst(moduleName);
            const YAML::Node &moduleYaml = getModuleYaml(moduleAst);
            /* Add module to library yaml */
            libraryYaml[moduleName.toStdString()] = moduleYaml;
            saveLibraryYaml(effectiveName, libraryYaml);
            return true;
        }
        /* Find module by pattern */
        bool hasMatch = false;
        for (const QString &moduleName : moduleList) {
            if (QStaticRegex::isNameExactMatch(moduleName, moduleNameRegex)) {
                QSocConsole::debug() << "Found module:" << moduleName;
                if (effectiveName.isEmpty()) {
                    /* Use first module name as library filename */
                    effectiveName = moduleName.toLower();
                    QSocConsole::debug() << "Pick library filename:" << effectiveName;
                }
                const json       &moduleAst           = slangDriver->getModuleAst(moduleName);
                const YAML::Node &moduleYaml          = getModuleYaml(moduleAst);
                libraryYaml[moduleName.toStdString()] = moduleYaml;
                hasMatch                              = true;
            }
        }
        if (hasMatch) {
            saveLibraryYaml(effectiveName, libraryYaml);
            return true;
        }
    }
    QSocConsole::error() << "no module found.";
    return false;
}

QStringList QSocModuleManager::listLoadedLibraries() const
{
    QStringList result = libraryMap.keys();
    result.sort(Qt::CaseInsensitive);
    return result;
}

QStringList QSocModuleManager::listModulesInLibrary(const QString &libraryName) const
{
    QStringList result = libraryMap.value(libraryName).values();
    result.sort(Qt::CaseInsensitive);
    return result;
}

QSocModuleDefinition QSocModuleManager::getModuleDefinition(
    const QString &libraryName, const QString &moduleName) const
{
    const YAML::Node libraryYaml = libraryData.value(libraryName);
    const YAML::Node moduleYaml  = libraryYaml[moduleName.toStdString()];
    if (!libraryYaml || !libraryMap.value(libraryName).contains(moduleName)
        || !moduleYaml.IsDefined()) {
        QSocModuleDefinition definition;
        definition.libraryName = libraryName;
        definition.moduleName  = moduleName;
        return definition;
    }
    return moduleYamlToDefinition(libraryName, moduleName, moduleYaml);
}

bool QSocModuleManager::createLibrary(const QString &libraryName)
{
    if (!isModulePathValid()) {
        return false;
    }
    if (!isSafeBasename(libraryName)) {
        QSocConsole::error() << "Invalid module library basename:" << libraryName;
        return false;
    }
    if (libraryMap.contains(libraryName) || isLibraryFileExist(libraryName)) {
        QSocConsole::error() << "Module library already exists:" << libraryName;
        return false;
    }
    libraryMap.insert(libraryName, {});
    libraryData[libraryName] = YAML::Node(YAML::NodeType::Map);
    rememberLoadedLibrary(libraryName);
    return true;
}

bool QSocModuleManager::replaceModuleDefinition(const QSocModuleDefinition &definition)
{
    if (!isModulePathValid()) {
        return false;
    }

    const QList<QSocModuleProblem> problems = validateModuleDefinition(definition);
    if (hasErrorProblems(problems)) {
        for (const QSocModuleProblem &problem : problems) {
            if (problem.severity == QSocModuleProblemSeverity::Error) {
                QSocConsole::error() << problem.message;
            }
        }
        return false;
    }

    const bool knownLibrary = libraryMap.contains(definition.libraryName);
    if (!knownLibrary && !createLibrary(definition.libraryName)) {
        return false;
    }
    if (!libraryData.contains(definition.libraryName) || !libraryData[definition.libraryName]) {
        libraryData[definition.libraryName] = YAML::Node(YAML::NodeType::Map);
    }

    libraryData[definition.libraryName][definition.moduleName.toStdString()]
        = moduleDefinitionToYaml(definition);
    libraryMapAdd(definition.libraryName, definition.moduleName);
    rebuildActiveModule(definition.moduleName);
    return save(definition.libraryName);
}

bool QSocModuleManager::renameModuleInLibrary(
    const QString &libraryName, const QString &oldName, const QString &newName)
{
    if (!isModulePathValid()) {
        return false;
    }
    if (oldName == newName) {
        return true;
    }
    if (!QSocVerilogUtils::isValidVerilogIdentifier(newName)) {
        QSocConsole::error() << "Invalid module name:" << newName;
        return false;
    }
    if (!libraryMap.value(libraryName).contains(oldName)
        || !libraryData.value(libraryName)[oldName.toStdString()].IsDefined()) {
        QSocConsole::error() << "Module is not in library:" << oldName << libraryName;
        return false;
    }
    if (libraryMap.value(libraryName).contains(newName)) {
        QSocConsole::error() << "Module already exists in library:" << newName << libraryName;
        return false;
    }

    YAML::Node renamedModule = YAML::Clone(libraryData[libraryName][oldName.toStdString()]);
    libraryData[libraryName].remove(oldName.toStdString());
    libraryData[libraryName][newName.toStdString()] = renamedModule;
    libraryMapRemove(libraryName, oldName);
    libraryMapAdd(libraryName, newName);
    rebuildActiveModule(oldName);
    rebuildActiveModule(newName);
    return save(libraryName);
}

bool QSocModuleManager::removeModuleFromLibrary(const QString &libraryName, const QString &moduleName)
{
    if (!isModulePathValid()) {
        return false;
    }
    if (!libraryMap.value(libraryName).contains(moduleName)
        || !libraryData.value(libraryName)[moduleName.toStdString()].IsDefined()) {
        QSocConsole::error() << "Module is not in library:" << moduleName << libraryName;
        return false;
    }

    const bool removesLastModule = libraryMap.value(libraryName).size() == 1;
    libraryData[libraryName].remove(moduleName.toStdString());
    libraryMapRemove(libraryName, moduleName);
    rebuildActiveModule(moduleName);

    if (!removesLastModule) {
        return save(libraryName);
    }

    const QString filePath
        = QDir(projectManager->getModulePath()).filePath(libraryName + ".soc_mod");
    if (QFile::exists(filePath) && !QFile::remove(filePath)) {
        QSocConsole::error() << "Failed to remove module file:" << filePath;
        return false;
    }
    libraryData.remove(libraryName);
    libraryLoadOrder.removeAll(libraryName);
    return true;
}

bool QSocModuleManager::removeLibraryIfEmpty(const QString &libraryName)
{
    if (!isModulePathValid()) {
        return false;
    }
    if (!libraryMap.contains(libraryName)) {
        const QString filePath
            = QDir(projectManager->getModulePath()).filePath(libraryName + ".soc_mod");
        if (QFile::exists(filePath)) {
            QSocConsole::error() << "Library is not loaded:" << libraryName;
            return false;
        }
        return true;
    }
    if (!libraryMap.value(libraryName).isEmpty()) {
        QSocConsole::error() << "Library is not empty:" << libraryName;
        return false;
    }

    const QString filePath
        = QDir(projectManager->getModulePath()).filePath(libraryName + ".soc_mod");
    if (QFile::exists(filePath) && !QFile::remove(filePath)) {
        QSocConsole::error() << "Failed to remove module file:" << filePath;
        return false;
    }
    libraryMap.remove(libraryName);
    libraryData.remove(libraryName);
    libraryLoadOrder.removeAll(libraryName);
    return true;
}

YAML::Node QSocModuleManager::moduleDefinitionToYaml(const QSocModuleDefinition &definition) const
{
    if (definition.isNullDefinition && !hasModuleContent(definition)) {
        return YAML::Node();
    }

    YAML::Node moduleYaml(YAML::NodeType::Map);
    copyMapInto(definition.extraAttributes, moduleYaml);

    if (!definition.parameters.isEmpty()) {
        moduleYaml["parameter"] = YAML::Node(YAML::NodeType::Map);
        for (const QSocModuleParameter &parameter : definition.parameters) {
            if (parameter.name.isEmpty()) {
                continue;
            }
            YAML::Node parameterYaml(YAML::NodeType::Map);
            copyMapInto(parameter.extraAttributes, parameterYaml);
            setScalarIfNotEmpty(parameterYaml, "type", parameter.type);
            setScalarIfNotEmpty(parameterYaml, "value", parameter.value);
            setScalarIfNotEmpty(parameterYaml, "description", parameter.description);
            moduleYaml["parameter"][parameter.name.toStdString()] = parameterYaml;
        }
    }

    if (!definition.ports.isEmpty()) {
        moduleYaml["port"] = YAML::Node(YAML::NodeType::Map);
        for (const QSocModulePort &port : definition.ports) {
            if (port.name.isEmpty()) {
                continue;
            }
            YAML::Node portYaml(YAML::NodeType::Map);
            copyMapInto(port.extraAttributes, portYaml);
            setScalarIfNotEmpty(portYaml, "type", port.type);
            setScalarIfNotEmpty(portYaml, "direction", port.direction);
            if (port.hasVisible) {
                portYaml["visible"] = port.visible;
            }
            setScalarIfNotEmpty(portYaml, "group", port.group);
            setScalarIfNotEmpty(portYaml, "description", port.description);
            moduleYaml["port"][port.name.toStdString()] = portYaml;
        }
    }

    if (!definition.busInterfaces.isEmpty()) {
        moduleYaml["bus"] = YAML::Node(YAML::NodeType::Map);
        for (const QSocModuleBusInterface &busInterface : definition.busInterfaces) {
            if (busInterface.name.isEmpty()) {
                continue;
            }
            YAML::Node interfaceYaml(YAML::NodeType::Map);
            copyMapInto(busInterface.extraAttributes, interfaceYaml);
            setScalarIfNotEmpty(interfaceYaml, "bus", busInterface.busName);
            setScalarIfNotEmpty(interfaceYaml, "mode", busInterface.mode);
            if (!busInterface.mapping.isEmpty()) {
                interfaceYaml["mapping"] = YAML::Node(YAML::NodeType::Map);
                for (const QSocModuleBusMapping &mapping : busInterface.mapping) {
                    if (!mapping.busSignal.isEmpty()) {
                        interfaceYaml["mapping"][mapping.busSignal.toStdString()]
                            = mapping.modulePort.toStdString();
                    }
                }
            }
            moduleYaml["bus"][busInterface.name.toStdString()] = interfaceYaml;
        }
    }

    return moduleYaml;
}

QSocModuleDefinition QSocModuleManager::moduleYamlToDefinition(
    const QString &libraryName, const QString &moduleName, const YAML::Node &node) const
{
    QSocModuleDefinition definition;
    definition.libraryName      = libraryName;
    definition.moduleName       = moduleName;
    definition.isNullDefinition = !node || node.IsNull();
    if (definition.isNullDefinition || !node.IsMap()) {
        return definition;
    }

    copyMapExcept(node, definition.extraAttributes, kModuleFields);

    const YAML::Node parameterYaml = node["parameter"];
    if (parameterYaml && parameterYaml.IsMap()) {
        for (YAML::const_iterator it = parameterYaml.begin(); it != parameterYaml.end(); ++it) {
            if (!it->first.IsScalar() || !it->second.IsMap()) {
                continue;
            }
            QSocModuleParameter parameter;
            parameter.name        = QString::fromStdString(it->first.as<std::string>());
            parameter.type        = scalarToString(it->second["type"]);
            parameter.value       = scalarToString(it->second["value"]);
            parameter.description = scalarToString(it->second["description"]);
            copyMapExcept(it->second, parameter.extraAttributes, kParameterFields);
            definition.parameters.append(parameter);
        }
    }

    const YAML::Node portYaml = node["port"];
    if (portYaml && portYaml.IsMap()) {
        for (YAML::const_iterator it = portYaml.begin(); it != portYaml.end(); ++it) {
            if (!it->first.IsScalar() || !it->second.IsMap()) {
                continue;
            }
            QSocModulePort port;
            port.name        = QString::fromStdString(it->first.as<std::string>());
            port.type        = scalarToString(it->second["type"]);
            port.direction   = scalarToString(it->second["direction"]);
            port.group       = scalarToString(it->second["group"]);
            port.description = scalarToString(it->second["description"]);
            if (it->second["visible"] && it->second["visible"].IsScalar()) {
                port.hasVisible           = true;
                const QString visibleText = scalarToString(it->second["visible"]).toLower();
                port.visible = visibleText == "true" || visibleText == "1" || visibleText == "yes";
            }
            copyMapExcept(it->second, port.extraAttributes, kPortFields);
            definition.ports.append(port);
        }
    }

    const YAML::Node busYaml = node["bus"];
    if (busYaml && busYaml.IsMap()) {
        for (YAML::const_iterator it = busYaml.begin(); it != busYaml.end(); ++it) {
            if (!it->first.IsScalar() || !it->second.IsMap()) {
                continue;
            }
            QSocModuleBusInterface busInterface;
            busInterface.name    = QString::fromStdString(it->first.as<std::string>());
            busInterface.busName = scalarToString(it->second["bus"]);
            busInterface.mode    = scalarToString(it->second["mode"]);
            copyMapExcept(it->second, busInterface.extraAttributes, kBusInterfaceFields);

            const YAML::Node mappingYaml = it->second["mapping"];
            if (mappingYaml && mappingYaml.IsMap()) {
                for (YAML::const_iterator mapIt = mappingYaml.begin(); mapIt != mappingYaml.end();
                     ++mapIt) {
                    if (!mapIt->first.IsScalar()) {
                        continue;
                    }
                    QSocModuleBusMapping mapping;
                    mapping.busSignal  = QString::fromStdString(mapIt->first.as<std::string>());
                    mapping.modulePort = scalarToString(mapIt->second);
                    busInterface.mapping.append(mapping);
                }
            }
            definition.busInterfaces.append(busInterface);
        }
    }

    return definition;
}

QList<QSocModuleProblem> QSocModuleManager::validateModuleDefinition(
    const QSocModuleDefinition &definition) const
{
    QList<QSocModuleProblem> problems;
    if (!isSafeBasename(definition.libraryName)) {
        problems.append(makeModuleProblem(
            QSocModuleProblemSeverity::Error,
            "invalid-library-name",
            definition,
            {},
            {},
            -1,
            "Invalid module library name."));
    }
    if (!QSocVerilogUtils::isValidVerilogIdentifier(definition.moduleName)) {
        problems.append(makeModuleProblem(
            QSocModuleProblemSeverity::Error,
            "invalid-module-name",
            definition,
            {},
            definition.moduleName,
            -1,
            "Invalid module name."));
    }

    QSet<QString> seenPorts;
    for (int row = 0; row < definition.ports.size(); ++row) {
        const QSocModulePort &port = definition.ports[row];
        if (!QSocVerilogUtils::isValidVerilogIdentifier(port.name)) {
            problems.append(makeModuleProblem(
                QSocModuleProblemSeverity::Error,
                "invalid-port-name",
                definition,
                "port",
                port.name,
                row,
                "Invalid port name."));
        }
        if (seenPorts.contains(port.name)) {
            problems.append(makeModuleProblem(
                QSocModuleProblemSeverity::Error,
                "duplicate-port-name",
                definition,
                "port",
                port.name,
                row,
                "Duplicate port name."));
        }
        seenPorts.insert(port.name);
        const QStringList validDirections = {"", "in", "out", "inout", "input", "output"};
        if (!validDirections.contains(port.direction)) {
            problems.append(makeModuleProblem(
                QSocModuleProblemSeverity::Error,
                "invalid-port-direction",
                definition,
                "port",
                port.name,
                row,
                "Port direction must be in, out, inout, input, or output."));
        }
    }

    QSet<QString> seenParameters;
    for (int row = 0; row < definition.parameters.size(); ++row) {
        const QSocModuleParameter &parameter = definition.parameters[row];
        if (!QSocVerilogUtils::isValidVerilogIdentifier(parameter.name)) {
            problems.append(makeModuleProblem(
                QSocModuleProblemSeverity::Error,
                "invalid-parameter-name",
                definition,
                "parameter",
                parameter.name,
                row,
                "Invalid parameter name."));
        }
        if (seenParameters.contains(parameter.name)) {
            problems.append(makeModuleProblem(
                QSocModuleProblemSeverity::Error,
                "duplicate-parameter-name",
                definition,
                "parameter",
                parameter.name,
                row,
                "Duplicate parameter name."));
        }
        seenParameters.insert(parameter.name);
    }

    QMap<QString, QSocModulePort> portsByName;
    for (const QSocModulePort &port : definition.ports) {
        if (!port.name.isEmpty())
            portsByName.insert(port.name, port);
    }

    const bool    hasLoadedBusData = busManager && !busManager->listLoadedLibraries().isEmpty();
    QSet<QString> seenInterfaces;
    for (int row = 0; row < definition.busInterfaces.size(); ++row) {
        const QSocModuleBusInterface &busInterface = definition.busInterfaces[row];
        if (!QSocVerilogUtils::isValidVerilogIdentifier(busInterface.name)) {
            problems.append(makeModuleProblem(
                QSocModuleProblemSeverity::Error,
                "invalid-interface-name",
                definition,
                "bus",
                busInterface.name,
                row,
                "Invalid bus interface name."));
        }
        if (seenInterfaces.contains(busInterface.name)) {
            problems.append(makeModuleProblem(
                QSocModuleProblemSeverity::Error,
                "duplicate-interface-name",
                definition,
                "bus",
                busInterface.name,
                row,
                "Duplicate bus interface name."));
        }
        seenInterfaces.insert(busInterface.name);
        if (busInterface.busName.isEmpty()) {
            problems.append(makeModuleProblem(
                QSocModuleProblemSeverity::Error,
                "missing-interface-bus",
                definition,
                "bus",
                busInterface.name,
                row,
                "Bus interface is missing a bus name."));
        }
        if (busInterface.mode.isEmpty()) {
            problems.append(makeModuleProblem(
                QSocModuleProblemSeverity::Error,
                "missing-interface-mode",
                definition,
                "bus",
                busInterface.name,
                row,
                "Bus interface is missing a mode."));
        }

        bool              busFound      = false;
        QSocBusDefinition busDefinition = findLoadedBusDefinition(
            hasLoadedBusData ? busManager : nullptr, busInterface.busName, &busFound);
        const QSet<QString> modeSignals = busSignalsForMode(busDefinition, busInterface.mode);
        if (hasLoadedBusData && !busInterface.busName.isEmpty() && !busFound) {
            problems.append(makeModuleProblem(
                QSocModuleProblemSeverity::Error,
                "missing-interface-bus-definition",
                definition,
                "bus",
                busInterface.name,
                row,
                "Bus interface references an unloaded or missing bus definition."));
        } else if (
            busFound && !busInterface.mode.isEmpty()
            && !busModes(busDefinition).contains(busInterface.mode)) {
            problems.append(makeModuleProblem(
                QSocModuleProblemSeverity::Error,
                "missing-interface-mode-definition",
                definition,
                "bus",
                busInterface.name,
                row,
                "Bus interface mode is not defined by the selected bus."));
        }

        QSet<QString> mappedSignals;
        for (const QSocModuleBusMapping &mapping : busInterface.mapping) {
            if (!mapping.busSignal.isEmpty())
                mappedSignals.insert(mapping.busSignal);

            const QSocBusSignalMode expected
                = busFound ? busSignalMode(busDefinition, mapping.busSignal, busInterface.mode)
                           : QSocBusSignalMode();
            if (busFound && !mapping.busSignal.isEmpty() && expected.signal.isEmpty()) {
                problems.append(makeModuleProblem(
                    QSocModuleProblemSeverity::Error,
                    "unknown-mapping-signal",
                    definition,
                    "bus",
                    QStringLiteral("%1/%2").arg(busInterface.name, mapping.busSignal),
                    row,
                    "Bus mapping references a signal not defined by the selected bus mode."));
            }

            if (mapping.modulePort.isEmpty()) {
                problems.append(makeModuleProblem(
                    QSocModuleProblemSeverity::Warning,
                    "empty-mapping-target",
                    definition,
                    "bus",
                    QStringLiteral("%1/%2").arg(busInterface.name, mapping.busSignal),
                    row,
                    "Bus mapping has an empty module port target."));
                continue;
            }

            if (!portsByName.contains(mapping.modulePort)) {
                problems.append(makeModuleProblem(
                    QSocModuleProblemSeverity::Error,
                    "unknown-mapping-port",
                    definition,
                    "bus",
                    QStringLiteral("%1/%2").arg(busInterface.name, mapping.modulePort),
                    row,
                    "Bus mapping references a module port that does not exist."));
                continue;
            }

            if (busFound
                && directionMismatch(
                    expected.direction, portsByName.value(mapping.modulePort).direction)) {
                problems.append(makeModuleProblem(
                    QSocModuleProblemSeverity::Warning,
                    "mapping-direction-mismatch",
                    definition,
                    "bus",
                    QStringLiteral("%1/%2").arg(busInterface.name, mapping.modulePort),
                    row,
                    "Bus mapping direction differs from the mapped module port."));
            }
        }

        if (busFound) {
            for (const QString &signal : modeSignals) {
                if (mappedSignals.contains(signal))
                    continue;
                problems.append(makeModuleProblem(
                    QSocModuleProblemSeverity::Warning,
                    "missing-mapping-target",
                    definition,
                    "bus",
                    QStringLiteral("%1/%2").arg(busInterface.name, signal),
                    row,
                    "Bus signal has no module port mapping."));
            }
        }
    }

    const QList<QSocModuleOverlay> overlays = scanModuleOverlays(definition.moduleName);
    if (!overlays.isEmpty() && overlays.first().libraries.size() > 1) {
        problems.append(makeModuleProblem(
            QSocModuleProblemSeverity::Warning,
            "module-overlay",
            definition,
            {},
            definition.moduleName,
            -1,
            "Module name exists in another loaded library."));
    }

    return problems;
}

QList<QSocModuleOverlay> QSocModuleManager::scanModuleOverlays(const QString &moduleName) const
{
    QSet<QString> moduleNames;
    if (!moduleName.isEmpty()) {
        moduleNames.insert(moduleName);
    } else {
        for (auto it = libraryMap.constBegin(); it != libraryMap.constEnd(); ++it) {
            for (const QString &name : it.value()) {
                moduleNames.insert(name);
            }
        }
    }

    QList<QSocModuleOverlay> result;
    for (const QString &name : moduleNames) {
        QStringList libraries;
        for (const QString &libraryName : libraryLoadOrder) {
            if (libraryMap.value(libraryName).contains(name)) {
                libraries.append(libraryName);
            }
        }
        if (libraries.isEmpty() || (moduleName.isEmpty() && libraries.size() < 2)) {
            continue;
        }

        QSocModuleOverlay overlay;
        overlay.moduleName        = name;
        overlay.libraries         = libraries;
        overlay.activeLibrary     = libraries.last();
        overlay.shadowedLibraries = libraries;
        overlay.shadowedLibraries.removeAll(overlay.activeLibrary);
        result.append(overlay);
    }
    return result;
}

QList<QSocModuleUsage> QSocModuleManager::scanModuleUsages(
    const QString &moduleName, QStringList *scanErrors) const
{
    QList<QSocModuleUsage> result;
    if (!projectManager || !projectManager->isValidProjectPath()) {
        if (scanErrors)
            scanErrors->append("Project path is invalid.");
        return result;
    }

    if (projectManager->isValidOutputPath()) {
        const QDir outputDir(
            projectManager->getOutputPath(),
            "*.soc_net",
            QDir::SortFlag::Name | QDir::SortFlag::IgnoreCase,
            QDir::Files | QDir::NoDotAndDotDot);
        for (const QString &fileName : outputDir.entryList()) {
            const QString filePath = outputDir.filePath(fileName);
            try {
                appendNetlistModuleUsages(
                    YAML::LoadFile(filePath.toStdString()),
                    moduleName,
                    projectRelativeFilePath(projectManager, filePath),
                    &result);
            } catch (const YAML::Exception &exception) {
                if (scanErrors) {
                    scanErrors->append(
                        QString("Failed to parse netlist %1: %2").arg(fileName, exception.what()));
                }
            }
        }
    }

    if (projectManager->isValidSchematicPath()) {
        const QDir schematicDir(
            projectManager->getSchematicPath(),
            "*.soc_sch",
            QDir::SortFlag::Name | QDir::SortFlag::IgnoreCase,
            QDir::Files | QDir::NoDotAndDotDot);
        for (const QString &fileName : schematicDir.entryList()) {
            const QString filePath = schematicDir.filePath(fileName);
            try {
                appendSchematicModuleUsages(
                    YAML::LoadFile(filePath.toStdString()),
                    moduleName,
                    projectRelativeFilePath(projectManager, filePath),
                    &result);
            } catch (const YAML::Exception &exception) {
                if (scanErrors) {
                    scanErrors->append(
                        QString("Failed to parse schematic %1: %2").arg(fileName, exception.what()));
                }
            }
        }
    }

    std::sort(result.begin(), result.end(), [](const QSocModuleUsage &a, const QSocModuleUsage &b) {
        const QString aKey = a.sourceType + '\n' + a.filePath + '\n' + a.instanceName;
        const QString bKey = b.sourceType + '\n' + b.filePath + '\n' + b.instanceName;
        return QString::compare(aKey, bKey, Qt::CaseInsensitive) < 0;
    });
    return result;
}

YAML::Node QSocModuleManager::getModuleYaml(const json &moduleAst)
{
    YAML::Node moduleYaml;
    /* Check if the module AST contains the required fields */
    if (moduleAst.contains("kind") && moduleAst.contains("name") && moduleAst.contains("body")
        && moduleAst["kind"] == "Instance" && moduleAst["body"].contains("members")) {
        /* Extract the module's kind and name */
        const QString &kind = QString::fromStdString(moduleAst["kind"]);
        const QString &name = QString::fromStdString(moduleAst["name"]);
        const json    &body = moduleAst["body"];
        /* Set of valid member kinds */
        const QSet<QString> validKind = {"port", "parameter"};
        /* Iterate through each member in the AST */
        for (const json &member : moduleAst["body"]["members"]) {
            /* Check if the member contains the necessary fields */
            if (member.contains("kind") && member.contains("name") && member.contains("type")) {
                const QString     &memberKind    = QString::fromStdString(member["kind"]).toLower();
                const QString     &memberName    = QString::fromStdString(member["name"]);
                const QString     &memberType    = QString::fromStdString(member["type"]).toLower();
                const std::string &memberKindStd = memberKind.toStdString();
                const std::string &memberNameStd = memberName.toStdString();
                const std::string &memberTypeStd = memberType.toStdString();
                /* Check if memberKind is within the valid kinds */
                if (!validKind.contains(memberKind)) {
                    /* If memberKind is not in the set, skip to next iteration */
                    continue;
                }
                /* Add member information to the YAML node */
                moduleYaml[memberKindStd][memberNameStd]["type"] = memberTypeStd;
                /* Check for and add the direction of the member if present */
                if (member.contains("direction")) {
                    const QString &memberDirection
                        = QString::fromStdString(member["direction"]).toLower();
                    const std::string &memberDirectionStd = memberDirection.toStdString();

                    moduleYaml[memberKindStd][memberNameStd]["direction"] = memberDirectionStd;
                }
                /* Check for and add the value of the member if present */
                if (member.contains("value")) {
                    const QString     &memberValue    = QString::fromStdString(member["value"]);
                    const std::string &memberValueStd = memberValue.toStdString();

                    moduleYaml[memberKindStd][memberNameStd]["value"] = memberValueStd;
                }
            }
        }
    }
    return moduleYaml;
}

YAML::Node QSocModuleManager::getModuleYaml(const QString &moduleName)
{
    YAML::Node result;

    /* Check if module exists in moduleData */
    if (!isModuleExist(moduleName)) {
        QSocConsole::warn() << "Module does not exist:" << moduleName;
        return result;
    }

    /* Get module YAML node from moduleData */
    result = moduleData[moduleName.toStdString()];

    return result;
}

bool QSocModuleManager::saveLibraryYaml(const QString &libraryName, const YAML::Node &libraryYaml)
{
    YAML::Node localLibraryYaml;
    /* Validate projectManager and its path */
    if (!isModulePathValid()) {
        QSocConsole::error() << "projectManager is null or invalid module path.";
        return false;
    }
    /* Check file path */
    const QString &modulePath = projectManager->getModulePath();
    const QString &filePath   = QDir(modulePath).filePath(QString("%1.soc_mod").arg(libraryName));
    if (QFile::exists(filePath)) {
        /* Load library YAML file */
        std::ifstream inputFileStream(filePath.toStdString());
        localLibraryYaml = mergeNodes(YAML::Load(inputFileStream), libraryYaml);
        QSocConsole::debug() << "Load and merge";
    } else {
        localLibraryYaml = libraryYaml;
    }

    /* Save YAML file */
    std::ofstream outputFileStream(filePath.toStdString());
    if (!outputFileStream.is_open()) {
        QSocConsole::error() << "Unable to open file for writing:" << filePath;
        return false;
    }
    outputFileStream << localLibraryYaml;
    outputFileStream.close();

    QSet<QString> affectedModules = libraryMap.value(libraryName);
    libraryMap.remove(libraryName);
    libraryData[libraryName] = YAML::Clone(localLibraryYaml);
    rememberLoadedLibrary(libraryName);
    if (localLibraryYaml && localLibraryYaml.IsMap()) {
        for (YAML::const_iterator it = localLibraryYaml.begin(); it != localLibraryYaml.end();
             ++it) {
            if (!it->first.IsScalar()) {
                continue;
            }
            const QString moduleName = QString::fromStdString(it->first.as<std::string>());
            affectedModules.insert(moduleName);
            libraryMapAdd(libraryName, moduleName);
        }
    }
    for (const QString &moduleName : affectedModules) {
        rebuildActiveModule(moduleName);
    }
    return true;
}

bool QSocModuleManager::isLibraryFileExist(const QString &libraryName)
{
    /* Validate projectManager and its module path */
    if (!isModulePathValid()) {
        QSocConsole::error() << "projectManager is null or invalid module path.";
        return false;
    }

    /* Check library basename */
    if (libraryName.isEmpty()) {
        QSocConsole::error() << "library basename is empty.";
        return false;
    }

    /* Get the full file path by joining module path and basename with extension */
    const QString filePath
        = QDir(projectManager->getModulePath()).filePath(libraryName + ".soc_mod");

    /* Check if library file exists */
    return QFile::exists(filePath);
}

bool QSocModuleManager::isLibraryExist(const QString &libraryName)
{
    return libraryMap.contains(libraryName);
}

QStringList QSocModuleManager::listLibrary(const QRegularExpression &libraryNameRegex)
{
    QStringList result;
    /* Validate projectManager and its path */
    if (!isModulePathValid()) {
        QSocConsole::error() << "projectManager is null or invalid module path.";
        return result;
    }
    /* Validate libraryNameRegex */
    if (!QStaticRegex::isNameRegexValid(libraryNameRegex)) {
        QSocConsole::error() << "Invalid or empty regex:" << libraryNameRegex.pattern();
        return result;
    }
    /* QDir for '.soc_mod' files in module path, sorted by name. */
    const QDir modulePathDir(
        projectManager->getModulePath(),
        "*.soc_mod",
        QDir::SortFlag::Name | QDir::SortFlag::IgnoreCase,
        QDir::Files | QDir::NoDotAndDotDot);
    /* Add matching file basenames from projectDir to result list. */
    foreach (const QString &filename, modulePathDir.entryList()) {
        if (QStaticRegex::isNameExactMatch(filename, libraryNameRegex)) {
            result.append(filename.split('.').first());
        }
    }

    return result;
}

bool QSocModuleManager::load(const QString &libraryName)
{
    /* Validate projectManager and its path */
    if (!isModulePathValid()) {
        QSocConsole::error() << "projectManager is null or invalid module path.";
        return false;
    }

    /* Check if library file exists */
    if (!isLibraryFileExist(libraryName)) {
        QSocConsole::error() << "Library file does not exist for basename:" << libraryName;
        return false;
    }

    /* Get the full file path by joining module path and basename with extension */
    const QString filePath
        = QDir(projectManager->getModulePath()).filePath(libraryName + ".soc_mod");

    /* Open the YAML file */
    std::ifstream fileStream(filePath.toStdString());
    if (!fileStream.is_open()) {
        QSocConsole::error() << "Unable to open file:" << filePath;
        return false;
    }

    try {
        /* Load YAML content into a temporary node */
        YAML::Node tempNode = YAML::Load(fileStream);
        if (!tempNode || !tempNode.IsMap()) {
            tempNode = YAML::Node(YAML::NodeType::Map);
        }

        QSet<QString> affectedModules;
        if (libraryMap.contains(libraryName)) {
            affectedModules = libraryMap.value(libraryName);
            libraryMap.remove(libraryName);
        }
        libraryData[libraryName] = YAML::Clone(tempNode);
        rememberLoadedLibrary(libraryName);

        /* Report overlays once per library pair. Per-module lines are too
           noisy when one library replaces many definitions. */
        QMap<QString, int> overlayCounts; /* losing-library -> count */

        /* Iterate through the temporary node and add to moduleData */
        for (YAML::const_iterator it = tempNode.begin(); it != tempNode.end(); ++it) {
            const auto    key        = it->first.as<std::string>();
            const QString moduleName = QString::fromStdString(key);
            affectedModules.insert(moduleName);

            if (moduleData[key] && moduleData[key]["library"]
                && moduleData[key]["library"].IsScalar()) {
                const QString existingLib = QString::fromStdString(
                    moduleData[key]["library"].as<std::string>());
                if (existingLib != libraryName) {
                    overlayCounts[existingLib]++;
                }
            }

            /* Update libraryMap with libraryName to key mapping */
            libraryMapAdd(libraryName, moduleName);
        }

        for (const QString &moduleName : affectedModules) {
            rebuildActiveModule(moduleName);
        }

        for (auto overlayIt = overlayCounts.constBegin(); overlayIt != overlayCounts.constEnd();
             ++overlayIt) {
            QSocConsole::warn() << "Library" << libraryName << "overwrites" << overlayIt.value()
                                << "module(s) previously loaded from library" << overlayIt.key();
        }
    } catch (const YAML::Exception &e) {
        QSocConsole::error() << "failed to parse YAML file:" << filePath << ":" << e.what();
        return false;
    }

    return true;
}

bool QSocModuleManager::load(const QRegularExpression &libraryNameRegex)
{
    /* Validate projectManager and its path */
    if (!isModulePathValid()) {
        QSocConsole::error() << "projectManager is null or invalid module path.";
        return false;
    }
    /* Validate libraryNameRegex */
    if (!QStaticRegex::isNameRegexValid(libraryNameRegex)) {
        QSocConsole::error() << "Invalid or empty regex:" << libraryNameRegex.pattern();
        return false;
    }

    /* Get the list of library basenames matching the regex */
    const QStringList matchingBasenames = listLibrary(libraryNameRegex);

    /* Iterate through the list and load each library */
    for (const QString &basename : matchingBasenames) {
        if (!load(basename)) {
            QSocConsole::error() << "Failed to load library:" << basename;
            return false;
        }
    }

    return true;
}

bool QSocModuleManager::load(const QStringList &libraryNameList)
{
    if (!projectManager || !projectManager->isValid()) {
        QSocConsole::error() << "Invalid or null projectManager.";
        return false;
    }

    const QSet<QString> uniqueBasenames(libraryNameList.begin(), libraryNameList.end());

    for (const QString &basename : uniqueBasenames) {
        if (!load(basename)) {
            QSocConsole::error() << "Failed to load library:" << basename;
            return false;
        }
    }

    return true;
}

bool QSocModuleManager::save(const QString &libraryName)
{
    /* Validate projectManager and its path */
    if (!isModulePathValid()) {
        QSocConsole::error() << "projectManager is null or invalid library path.";
        return false;
    }

    /* Check if the libraryName exists in libraryMap */
    if (!libraryMap.contains(libraryName)) {
        QSocConsole::error() << "Library basename not found in libraryMap.";
        return false;
    }

    YAML::Node dataToSave = YAML::Clone(libraryData.value(libraryName));
    if (!dataToSave || !dataToSave.IsMap()) {
        dataToSave = YAML::Node(YAML::NodeType::Map);
        for (const auto &moduleItem : libraryMap[libraryName]) {
            const std::string moduleNameStd = moduleItem.toStdString();
            if (!moduleData[moduleNameStd]) {
                QSocConsole::error() << "Module data is not exist: " << moduleNameStd;
                return false;
            }
            dataToSave[moduleNameStd] = YAML::Clone(moduleData[moduleNameStd]);
            if (dataToSave[moduleNameStd]["library"]) {
                dataToSave[moduleNameStd].remove("library");
            }
        }
    } else {
        for (YAML::iterator it = dataToSave.begin(); it != dataToSave.end(); ++it) {
            if (it->second && it->second.IsMap() && it->second["library"]) {
                it->second.remove("library");
            }
        }
    }

    /* Serialize and save to file */
    const QString filePath
        = QDir(projectManager->getModulePath()).filePath(libraryName + ".soc_mod");
    std::ofstream outputFileStream(filePath.toStdString());
    if (!outputFileStream.is_open()) {
        QSocConsole::error() << "Unable to open file for writing:" << filePath;
        return false;
    }
    outputFileStream << dataToSave;
    return true;
}

bool QSocModuleManager::save(const QRegularExpression &libraryNameRegex)
{
    bool allSaved = true;

    /* Validate projectManager and its path */
    if (!isModulePathValid()) {
        QSocConsole::error() << "projectManager is null or invalid module path.";
        return false;
    }
    /* Validate libraryNameRegex */
    if (!QStaticRegex::isNameRegexValid(libraryNameRegex)) {
        QSocConsole::error() << "Invalid or empty regex:" << libraryNameRegex.pattern();
        return false;
    }

    /* Iterate over libraryMap and save matching libraries */
    for (const QString &libraryName : libraryMap.keys()) {
        if (QStaticRegex::isNameExactMatch(libraryName, libraryNameRegex)) {
            if (!save(libraryName)) {
                QSocConsole::error() << "Failed to save library:" << libraryName;
                allSaved = false;
            }
        }
    }

    return allSaved;
}

bool QSocModuleManager::save(const QStringList &libraryNameList)
{
    /* Validate projectManager and its path */
    if (!isModulePathValid()) {
        QSocConsole::error() << "projectManager is null or invalid module path.";
        return false;
    }

    const QSet<QString> uniqueBasenames(libraryNameList.begin(), libraryNameList.end());

    for (const QString &basename : uniqueBasenames) {
        if (!save(basename)) {
            QSocConsole::error() << "Failed to save library:" << basename;
            return false;
        }
    }

    return true;
}

bool QSocModuleManager::remove(const QString &libraryName)
{
    /* Validate projectManager and its module path */
    if (!isModulePathValid()) {
        QSocConsole::error() << "projectManager is null or invalid module path.";
        return false;
    }

    /* Check library basename */
    if (libraryName.isEmpty()) {
        QSocConsole::error() << "library basename is empty.";
        return false;
    }

    /* Get the full file path */
    const QString filePath
        = QDir(projectManager->getModulePath()).filePath(libraryName + ".soc_mod");

    /* Check if library file exists */
    if (!QFile::exists(filePath)) {
        QSocConsole::error() << "library file does not exist for basename:" << libraryName;
        return false;
    }

    /* Remove the file */
    if (!QFile::remove(filePath)) {
        QSocConsole::error() << "Failed to remove module file:" << filePath;
        return false;
    }

    const QStringList affectedModules = listModulesInLibrary(libraryName);
    libraryMap.remove(libraryName);
    libraryData.remove(libraryName);
    libraryLoadOrder.removeAll(libraryName);
    for (const QString &moduleName : affectedModules) {
        rebuildActiveModule(moduleName);
    }

    return true;
}

bool QSocModuleManager::remove(const QRegularExpression &libraryNameRegex)
{
    /* Validate projectManager and its path */
    if (!isModulePathValid()) {
        QSocConsole::error() << "projectManager is null or invalid module path.";
        return false;
    }
    /* Validate libraryNameRegex */
    if (!QStaticRegex::isNameRegexValid(libraryNameRegex)) {
        QSocConsole::error() << "Invalid or empty regex:" << libraryNameRegex.pattern();
        return false;
    }

    /* Get the list of library basenames matching the regex */
    const QStringList matchingBasenames = listLibrary(libraryNameRegex);

    /* Iterate through the list and remove each library */
    for (const QString &basename : matchingBasenames) {
        if (!remove(basename)) {
            QSocConsole::error() << "Failed to remove library:" << basename;
            return false;
        }
    }

    return true;
}

bool QSocModuleManager::remove(const QStringList &libraryNameList)
{
    if (!projectManager || !projectManager->isValid()) {
        QSocConsole::error() << "Invalid or null projectManager.";
        return false;
    }

    const QSet<QString> uniqueBasenames(libraryNameList.begin(), libraryNameList.end());

    for (const QString &basename : uniqueBasenames) {
        if (!remove(basename)) {
            QSocConsole::error() << "Failed to remove library:" << basename;
            return false;
        }
    }

    return true;
}

bool QSocModuleManager::isModuleExist(const QString &moduleName)
{
    return moduleData[moduleName.toStdString()].IsDefined();
}

bool QSocModuleManager::isModuleExist(const QRegularExpression &moduleNameRegex)
{
    /* Validate moduleNameRegex */
    if (!QStaticRegex::isNameRegexValid(moduleNameRegex)) {
        QSocConsole::error() << "Invalid or empty regex:" << moduleNameRegex.pattern();
        return false;
    }

    /* Iterate through each node in moduleData */
    for (YAML::const_iterator it = moduleData.begin(); it != moduleData.end(); ++it) {
        const QString moduleName = QString::fromStdString(it->first.as<std::string>());

        /* Check if the module name matches the regex */
        if (QStaticRegex::isNameExactMatch(moduleName, moduleNameRegex)) {
            return true;
        }
    }

    return false;
}

QString QSocModuleManager::getModuleLibrary(const QString &moduleName)
{
    if (!isModuleExist(moduleName)) {
        return {};
    }
    return QString::fromStdString(moduleData[moduleName.toStdString()]["library"].as<std::string>());
}

QStringList QSocModuleManager::listModule(const QRegularExpression &moduleNameRegex)
{
    QStringList result;
    /* Validate moduleNameRegex */
    if (!QStaticRegex::isNameRegexValid(moduleNameRegex)) {
        QSocConsole::error() << "Invalid or empty regex:" << moduleNameRegex.pattern();
        return result;
    }

    /* Iterate through each node in moduleData */
    for (YAML::const_iterator it = moduleData.begin(); it != moduleData.end(); ++it) {
        const QString moduleName = QString::fromStdString(it->first.as<std::string>());

        /* Check if the module name matches the regex */
        if (QStaticRegex::isNameExactMatch(moduleName, moduleNameRegex)) {
            result.append(moduleName);
        }
    }

    return result;
}

YAML::Node QSocModuleManager::getModuleYamls(const QRegularExpression &moduleNameRegex)
{
    YAML::Node result;

    /* Check if the regex is valid, if not, return an empty node */
    if (!QStaticRegex::isNameRegexValid(moduleNameRegex)) {
        QSocConsole::warn() << "Invalid regular expression provided.";
        return result;
    }

    /* Iterate over the moduleData to find matches */
    for (YAML::const_iterator it = moduleData.begin(); it != moduleData.end(); ++it) {
        const QString moduleName = QString::fromStdString(it->first.as<std::string>());

        /* Check if the module name matches the regex */
        if (QStaticRegex::isNameExactMatch(moduleName, moduleNameRegex)) {
            /* Add the module node to the result */
            result[moduleName.toStdString()] = it->second;
        }
    }

    return result;
}

bool QSocModuleManager::updateModuleYaml(const QString &moduleName, const YAML::Node &moduleYaml)
{
    /* Check if module exists in a library */
    if (!isModuleExist(moduleName)) {
        QSocConsole::error() << "Module does not exist:" << moduleName;
        return false;
    }

    /* Get the library name for this module */
    const QString libraryName = getModuleLibrary(moduleName);
    if (libraryName.isEmpty()) {
        QSocConsole::error() << "Could not find library for module:" << moduleName;
        return false;
    }

    /* Update module data */
    YAML::Node cleanModuleYaml = YAML::Clone(moduleYaml);
    if (cleanModuleYaml && cleanModuleYaml.IsMap() && cleanModuleYaml["library"]) {
        cleanModuleYaml.remove("library");
    }
    libraryData[libraryName][moduleName.toStdString()] = cleanModuleYaml;
    libraryMapAdd(libraryName, moduleName);
    rebuildActiveModule(moduleName);

    /* Save the updated library */
    return save(libraryName);
}

bool QSocModuleManager::removeModule(const QRegularExpression &moduleNameRegex)
{
    /* Validate projectManager and its path */
    if (!isModulePathValid()) {
        QSocConsole::error() << "projectManager is null or invalid module path.";
        return false;
    }
    /* Validate moduleNameRegex */
    if (!QStaticRegex::isNameRegexValid(moduleNameRegex)) {
        QSocConsole::error() << "Invalid or empty regex:" << moduleNameRegex.pattern();
        return false;
    }

    QSet<QString> libraryToSave;
    QSet<QString> libraryToRemove;
    QSet<QString> moduleToRemove;

    for (auto moduleDataIter = moduleData.begin(); moduleDataIter != moduleData.end();
         ++moduleDataIter) {
        const QString moduleName = QString::fromStdString(moduleDataIter->first.as<std::string>());
        if (QStaticRegex::isNameExactMatch(moduleName, moduleNameRegex)) {
            moduleToRemove.insert(moduleName);
            const QString libraryName = QString::fromStdString(
                moduleDataIter->second["library"].as<std::string>());
            libraryToSave.insert(libraryName);
        }
    }

    /* Remove modules from moduleData */
    for (const QString &moduleName : moduleToRemove) {
        const QString libraryName = QString::fromStdString(
            moduleData[moduleName.toStdString()]["library"].as<std::string>());
        libraryMapRemove(libraryName, moduleName);
        if (libraryData.contains(libraryName)) {
            libraryData[libraryName].remove(moduleName.toStdString());
        }
        if (!libraryMap.contains(libraryName)) {
            libraryToRemove.insert(libraryName);
        }
        rebuildActiveModule(moduleName);
    }

    /* Ensure libraryToSave does not include modules marked for removal */
    for (const QString &libraryName : libraryToRemove) {
        libraryToSave.remove(libraryName);
    }

    const QStringList libraryToSaveList = QList<QString>(libraryToSave.begin(), libraryToSave.end());
    const QStringList libraryToRemoveList
        = QList<QString>(libraryToRemove.begin(), libraryToRemove.end());

    /* Save libraries that still have associations in libraryMap */
    if (!save(libraryToSaveList)) {
        QSocConsole::error() << "Failed to save libraries.";
        return false;
    }

    /* Remove modules with no remaining associations in libraryMap */
    if (!remove(libraryToRemoveList)) {
        QSocConsole::error() << "Failed to remove modules.";
        return false;
    }

    return true;
}
