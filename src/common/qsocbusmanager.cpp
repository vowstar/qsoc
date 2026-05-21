// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "common/qsocbusmanager.h"
#include "common/qsocconsole.h"

#include "common/qstaticregex.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>
#include <QTextStream>

#include <climits>
#include <fstream>
#include <string>
#include <vector>

#include <rapidcsv.h>

namespace {

const QStringList kBusCsvColumns
    = {"name", "mode", "direction", "width", "qualifier", "description"};

const QSet<QString> kModeFields = {"direction", "width", "qualifier", "description"};

enum class BusReferenceRewrite { BusName, SignalName, ModeName };

struct ModuleLibraryRewrite
{
    QString    libraryName;
    QString    filePath;
    YAML::Node yaml;
    bool       changed = false;
};

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
    return QString::fromStdString(node.as<std::string>()).trimmed();
}

bool mapHasModeField(const YAML::Node &node)
{
    if (!node || !node.IsMap()) {
        return false;
    }
    for (YAML::const_iterator it = node.begin(); it != node.end(); ++it) {
        if (it->first.IsScalar()
            && kModeFields.contains(QString::fromStdString(it->first.as<std::string>()))) {
            return true;
        }
    }
    return false;
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

bool isPositiveIntegerText(const QString &value)
{
    static const QRegularExpression integerRegex("^[1-9][0-9]*$");
    return integerRegex.match(value).hasMatch();
}

QSocBusProblem makeBusProblem(
    QSocBusProblemSeverity   severity,
    const QString           &code,
    const QSocBusDefinition &definition,
    int                      row,
    const QString           &signal,
    const QString           &mode,
    const QString           &message)
{
    QSocBusProblem problem;
    problem.severity    = severity;
    problem.code        = code;
    problem.libraryName = definition.libraryName;
    problem.busName     = definition.busName;
    problem.row         = row;
    problem.signal      = signal;
    problem.mode        = mode;
    problem.message     = message;
    return problem;
}

bool hasErrors(const QList<QSocBusProblem> &problems)
{
    for (const QSocBusProblem &problem : problems) {
        if (problem.severity == QSocBusProblemSeverity::Error) {
            return true;
        }
    }
    return false;
}

bool appendRewriteError(QStringList *errors, const QString &message)
{
    if (errors) {
        errors->append(message);
    }
    return false;
}

QString usagePath(
    const QString &moduleLibrary, const QString &moduleName, const QString &interfaceName)
{
    return QString("%1/%2/%3").arg(moduleLibrary, moduleName, interfaceName);
}

bool mapContainsScalarKey(const YAML::Node &mapYaml, const QString &key)
{
    if (!mapYaml || !mapYaml.IsMap()) {
        return false;
    }
    const std::string keyStd = key.toStdString();
    for (YAML::const_iterator it = mapYaml.begin(); it != mapYaml.end(); ++it) {
        if (it->first.IsScalar() && it->first.as<std::string>() == keyStd) {
            return true;
        }
    }
    return false;
}

bool loadModuleLibraries(
    QSocProjectManager *projectManager, QList<ModuleLibraryRewrite> *libraries, QStringList *errors)
{
    if (!projectManager || !projectManager->isValidModulePath()) {
        return appendRewriteError(errors, "Project module path is invalid.");
    }

    const QDir moduleDir(
        projectManager->getModulePath(),
        "*.soc_mod",
        QDir::SortFlag::Name | QDir::SortFlag::IgnoreCase,
        QDir::Files | QDir::NoDotAndDotDot);

    for (const QString &fileName : moduleDir.entryList()) {
        ModuleLibraryRewrite library;
        library.libraryName = QFileInfo(fileName).completeBaseName();
        library.filePath    = moduleDir.filePath(fileName);
        try {
            library.yaml = YAML::LoadFile(library.filePath.toStdString());
        } catch (const YAML::Exception &exception) {
            appendRewriteError(
                errors,
                QString("Failed to parse module library %1: %2")
                    .arg(library.libraryName, exception.what()));
            continue;
        }
        libraries->append(library);
    }

    return !errors || errors->isEmpty();
}

bool saveModuleLibrary(const ModuleLibraryRewrite &library, QStringList *errors)
{
    YAML::Emitter emitter;
    emitter.SetMapFormat(YAML::Block);
    emitter.SetSeqFormat(YAML::Block);
    emitter << library.yaml;
    if (!emitter.good()) {
        return appendRewriteError(
            errors, QString("Failed to serialize module library %1.").arg(library.libraryName));
    }

    QSaveFile file(library.filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return appendRewriteError(
            errors, QString("Failed to open module library %1.").arg(library.libraryName));
    }
    file.write(emitter.c_str());
    file.write("\n");
    if (!file.commit()) {
        return appendRewriteError(
            errors, QString("Failed to save module library %1.").arg(library.libraryName));
    }
    return true;
}

bool rewriteModuleBusReferences(
    QSocProjectManager *projectManager,
    BusReferenceRewrite rewrite,
    const QString      &busName,
    const QString      &oldName,
    const QString      &newName,
    QStringList        *changedModules,
    QStringList        *errors)
{
    if (oldName.isEmpty() || newName.isEmpty()) {
        return appendRewriteError(errors, "Reference rename arguments must not be empty.");
    }
    if (oldName == newName) {
        return true;
    }

    QStringList                 changed;
    QStringList                 localErrors;
    QList<ModuleLibraryRewrite> libraries;
    if (!loadModuleLibraries(projectManager, &libraries, &localErrors)) {
        if (errors) {
            errors->append(localErrors);
        }
        return false;
    }

    const std::string oldNameStd = oldName.toStdString();
    const std::string newNameStd = newName.toStdString();

    for (ModuleLibraryRewrite &library : libraries) {
        if (!library.yaml || !library.yaml.IsMap()) {
            continue;
        }
        for (YAML::iterator moduleIt = library.yaml.begin(); moduleIt != library.yaml.end();
             ++moduleIt) {
            if (!moduleIt->first.IsScalar() || !moduleIt->second.IsMap() || !moduleIt->second["bus"]
                || !moduleIt->second["bus"].IsMap()) {
                continue;
            }

            const QString moduleName = QString::fromStdString(moduleIt->first.as<std::string>());
            YAML::Node    busYaml    = moduleIt->second["bus"];
            for (YAML::iterator interfaceIt = busYaml.begin(); interfaceIt != busYaml.end();
                 ++interfaceIt) {
                if (!interfaceIt->first.IsScalar() || !interfaceIt->second.IsMap()) {
                    continue;
                }

                YAML::Node    interfaceYaml = interfaceIt->second;
                const QString interfaceName = QString::fromStdString(
                    interfaceIt->first.as<std::string>());
                const QString referencedBus = scalarToString(interfaceYaml["bus"]);
                if (rewrite == BusReferenceRewrite::BusName) {
                    if (referencedBus != oldName) {
                        continue;
                    }
                    interfaceYaml["bus"] = newNameStd;
                    library.changed      = true;
                    changed.append(usagePath(library.libraryName, moduleName, interfaceName));
                    continue;
                }

                if (referencedBus != busName) {
                    continue;
                }
                if (rewrite == BusReferenceRewrite::ModeName) {
                    if (scalarToString(interfaceYaml["mode"]) != oldName) {
                        continue;
                    }
                    interfaceYaml["mode"] = newNameStd;
                    library.changed       = true;
                    changed.append(usagePath(library.libraryName, moduleName, interfaceName));
                    continue;
                }

                YAML::Node mappingYaml = interfaceYaml["mapping"];
                if (!mappingYaml || !mappingYaml.IsMap()
                    || !mapContainsScalarKey(mappingYaml, oldName)) {
                    continue;
                }
                if (mapContainsScalarKey(mappingYaml, newName)) {
                    localErrors.append(
                        QString("Module mapping already has signal %1: %2")
                            .arg(newName, usagePath(library.libraryName, moduleName, interfaceName)));
                    continue;
                }

                YAML::Node newMapping(YAML::NodeType::Map);
                for (YAML::const_iterator mappingIt = mappingYaml.begin();
                     mappingIt != mappingYaml.end();
                     ++mappingIt) {
                    if (mappingIt->first.IsScalar()
                        && mappingIt->first.as<std::string>() == oldNameStd) {
                        newMapping[newNameStd] = YAML::Clone(mappingIt->second);
                    } else {
                        newMapping[mappingIt->first] = YAML::Clone(mappingIt->second);
                    }
                }
                interfaceYaml["mapping"] = newMapping;
                library.changed          = true;
                changed.append(usagePath(library.libraryName, moduleName, interfaceName));
            }
        }
    }

    if (!localErrors.isEmpty()) {
        if (errors) {
            errors->append(localErrors);
        }
        return false;
    }

    for (const ModuleLibraryRewrite &library : libraries) {
        if (library.changed && !saveModuleLibrary(library, &localErrors)) {
            if (errors) {
                errors->append(localErrors);
            }
            return false;
        }
    }

    changed.removeDuplicates();
    changed.sort(Qt::CaseInsensitive);
    if (changedModules) {
        changedModules->append(changed);
    }
    return true;
}

} // namespace

QSocBusManager::QSocBusManager(QObject *parent, QSocProjectManager *projectManager)
    : QObject{parent}
    , projectManager(projectManager)
{
    /* All private members set by constructor */
}

QSocBusManager::~QSocBusManager() = default;

void QSocBusManager::setProjectManager(QSocProjectManager *projectManager)
{
    this->projectManager = projectManager;
}

QSocProjectManager *QSocBusManager::getProjectManager()
{
    return projectManager;
}

bool QSocBusManager::isBusPathValid()
{
    /* Validate projectManager */
    if (!projectManager) {
        QSocConsole::error() << "projectManager is nullptr.";
        return false;
    }
    /* Validate bus path. */
    if (!projectManager->isValidBusPath()) {
        QSocConsole::error() << "Invalid bus path:" << projectManager->getBusPath();
        return false;
    }
    return true;
}

void QSocBusManager::resetBusData()
{
    libraryMap.clear();
    busData = YAML::Node();
    QSocConsole::debug() << "Bus data has been reset.";
}

bool QSocBusManager::importFromFileList(
    const QString &libraryName, const QString &busName, const QStringList &filePathList)
{
    if (libraryName.isEmpty()) {
        QSocConsole::error() << "library name is empty.";
        return false;
    }
    if (busName.isEmpty()) {
        QSocConsole::error() << "bus name is empty.";
        return false;
    }
    QStringList                    warnings;
    const QList<QSocBusSignalMode> rows = parseBusCsvFiles(filePathList, &warnings);
    for (const QString &warning : warnings) {
        QSocConsole::warn() << warning;
    }

    const YAML::Node busYaml = rowsToBusYaml(busName, rows);

    if (!saveLibraryYaml(libraryName, busYaml)) {
        QSocConsole::error() << "Failed to save bus library YAML file";
        return false;
    }

    return true;
}

QStringList QSocBusManager::listLoadedLibraries() const
{
    QStringList result = libraryMap.keys();
    result.sort(Qt::CaseInsensitive);
    return result;
}

QStringList QSocBusManager::listBusesInLibrary(const QString &libraryName) const
{
    QStringList result = libraryMap.value(libraryName).values();
    result.sort(Qt::CaseInsensitive);
    return result;
}

QSocBusDefinition QSocBusManager::getBusDefinition(
    const QString &libraryName, const QString &busName) const
{
    const std::string busNameStd = busName.toStdString();
    const YAML::Node  node       = busData[busNameStd];
    if (!node || !libraryMap.value(libraryName).contains(busName)) {
        QSocBusDefinition definition;
        definition.libraryName = libraryName;
        definition.busName     = busName;
        return definition;
    }
    return busYamlToDefinition(libraryName, busName, node);
}

bool QSocBusManager::createLibrary(const QString &libraryName)
{
    if (!isBusPathValid()) {
        return false;
    }
    if (!isSafeBasename(libraryName)) {
        QSocConsole::error() << "Invalid library basename:" << libraryName;
        return false;
    }
    if (libraryMap.contains(libraryName) || isLibraryFileExist(libraryName)) {
        QSocConsole::error() << "Bus library already exists:" << libraryName;
        return false;
    }
    libraryMap.insert(libraryName, {});
    return true;
}

bool QSocBusManager::replaceBusDefinition(const QSocBusDefinition &definition)
{
    if (!isBusPathValid()) {
        return false;
    }
    const QList<QSocBusProblem> problems = validateBusDefinition(definition);
    if (hasErrors(problems)) {
        for (const QSocBusProblem &problem : problems) {
            if (problem.severity == QSocBusProblemSeverity::Error) {
                QSocConsole::error() << problem.message;
            }
        }
        return false;
    }
    if (!libraryMap.contains(definition.libraryName) && !createLibrary(definition.libraryName)) {
        return false;
    }

    const std::string busNameStd = definition.busName.toStdString();
    YAML::Node        busYaml    = busDefinitionToYaml(definition);
    busYaml["library"]           = definition.libraryName.toStdString();
    busData[busNameStd]          = busYaml;
    libraryMapAdd(definition.libraryName, definition.busName);
    return save(definition.libraryName);
}

bool QSocBusManager::renameBusInLibrary(
    const QString &libraryName, const QString &oldName, const QString &newName)
{
    if (!isBusPathValid()) {
        return false;
    }
    if (oldName == newName) {
        return true;
    }
    if (!isSafeBasename(newName)) {
        QSocConsole::error() << "Invalid bus name:" << newName;
        return false;
    }
    if (!libraryMap.value(libraryName).contains(oldName)) {
        QSocConsole::error() << "Bus is not in library:" << oldName << libraryName;
        return false;
    }
    if (isBusExist(newName)) {
        QSocConsole::error() << "Bus already exists:" << newName;
        return false;
    }

    YAML::Node renamedBus = YAML::Clone(busData[oldName.toStdString()]);
    renamedBus["library"] = libraryName.toStdString();
    busData.remove(oldName.toStdString());
    libraryMapRemove(libraryName, oldName);
    busData[newName.toStdString()] = renamedBus;
    libraryMapAdd(libraryName, newName);
    return save(libraryName);
}

bool QSocBusManager::removeBusFromLibrary(const QString &libraryName, const QString &busName)
{
    if (!isBusPathValid()) {
        return false;
    }
    if (!libraryMap.value(libraryName).contains(busName) || !busData[busName.toStdString()]) {
        QSocConsole::error() << "Bus is not in library:" << busName << libraryName;
        return false;
    }

    const bool removesLastBus = libraryMap.value(libraryName).size() == 1;
    busData.remove(busName.toStdString());
    libraryMapRemove(libraryName, busName);
    if (!removesLastBus) {
        return save(libraryName);
    }

    const QString filePath = QDir(projectManager->getBusPath()).filePath(libraryName + ".soc_bus");
    if (QFile::exists(filePath) && !QFile::remove(filePath)) {
        QSocConsole::error() << "Failed to remove bus file:" << filePath;
        return false;
    }
    return true;
}

bool QSocBusManager::removeLibraryIfEmpty(const QString &libraryName)
{
    if (!isBusPathValid()) {
        return false;
    }
    if (!libraryMap.contains(libraryName)) {
        const QString filePath
            = QDir(projectManager->getBusPath()).filePath(libraryName + ".soc_bus");
        if (QFile::exists(filePath)) {
            QSocConsole::error() << "Library is not loaded:" << libraryName;
            return false;
        }
        return true;
    }
    if (libraryMap.contains(libraryName) && !libraryMap.value(libraryName).isEmpty()) {
        QSocConsole::error() << "Library is not empty:" << libraryName;
        return false;
    }

    const QString filePath = QDir(projectManager->getBusPath()).filePath(libraryName + ".soc_bus");
    if (QFile::exists(filePath) && !QFile::remove(filePath)) {
        QSocConsole::error() << "Failed to remove bus file:" << filePath;
        return false;
    }
    libraryMap.remove(libraryName);
    return true;
}

YAML::Node QSocBusManager::busDefinitionToYaml(const QSocBusDefinition &definition) const
{
    YAML::Node busYaml(YAML::NodeType::Map);
    copyMapInto(definition.extraAttributes, busYaml);
    busYaml["port"] = YAML::Node(YAML::NodeType::Map);

    for (const QSocBusSignalMode &row : definition.rows) {
        if (row.signal.isEmpty() || row.mode.isEmpty()) {
            continue;
        }

        const std::string signalNameStd = row.signal.toStdString();
        const std::string modeNameStd   = row.mode.toStdString();
        YAML::Node        signalYaml    = busYaml["port"][signalNameStd];
        copyMapInto(row.signalExtraAttributes, signalYaml);

        YAML::Node modeYaml(YAML::NodeType::Map);
        copyMapInto(row.modeExtraAttributes, modeYaml);
        setScalarIfNotEmpty(modeYaml, "direction", row.direction);
        setScalarIfNotEmpty(modeYaml, "width", row.width);
        setScalarIfNotEmpty(modeYaml, "qualifier", row.qualifier);
        setScalarIfNotEmpty(modeYaml, "description", row.description);
        signalYaml[modeNameStd]        = modeYaml;
        busYaml["port"][signalNameStd] = signalYaml;
    }

    return busYaml;
}

QSocBusDefinition QSocBusManager::busYamlToDefinition(
    const QString &libraryName, const QString &busName, const YAML::Node &node) const
{
    QSocBusDefinition definition;
    definition.libraryName = libraryName;
    definition.busName     = busName;
    copyMapExcept(node, definition.extraAttributes, {"port", "library"});

    const YAML::Node portYaml = node["port"];
    if (!portYaml || !portYaml.IsMap()) {
        return definition;
    }

    for (YAML::const_iterator signalIt = portYaml.begin(); signalIt != portYaml.end(); ++signalIt) {
        if (!signalIt->first.IsScalar() || !signalIt->second.IsMap()) {
            continue;
        }

        const QString    signalName = QString::fromStdString(signalIt->first.as<std::string>());
        const YAML::Node signalYaml = signalIt->second;
        YAML::Node       signalExtraAttributes(YAML::NodeType::Map);
        for (YAML::const_iterator modeIt = signalYaml.begin(); modeIt != signalYaml.end();
             ++modeIt) {
            if (!modeIt->first.IsScalar()) {
                continue;
            }
            if (!modeIt->second.IsMap() || !mapHasModeField(modeIt->second)) {
                signalExtraAttributes[modeIt->first.as<std::string>()] = YAML::Clone(modeIt->second);
            }
        }

        for (YAML::const_iterator modeIt = signalYaml.begin(); modeIt != signalYaml.end();
             ++modeIt) {
            if (!modeIt->first.IsScalar() || !modeIt->second.IsMap()
                || !mapHasModeField(modeIt->second)) {
                continue;
            }

            QSocBusSignalMode row;
            row.signal                = signalName;
            row.mode                  = QString::fromStdString(modeIt->first.as<std::string>());
            row.direction             = scalarToString(modeIt->second["direction"]);
            row.width                 = scalarToString(modeIt->second["width"]);
            row.qualifier             = scalarToString(modeIt->second["qualifier"]);
            row.description           = scalarToString(modeIt->second["description"]);
            row.signalExtraAttributes = YAML::Clone(signalExtraAttributes);
            copyMapExcept(modeIt->second, row.modeExtraAttributes, kModeFields);
            definition.rows.append(row);
        }
    }

    return definition;
}

QList<QSocBusSignalMode> QSocBusManager::parseBusCsvFiles(
    const QStringList &filePaths, QStringList *warnings) const
{
    QList<QSocBusSignalMode> result;

    for (const QString &filePath : filePaths) {
        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            if (warnings) {
                warnings->append(
                    QString("Cannot open CSV file: %1").arg(QFileInfo(filePath).fileName()));
            }
            continue;
        }

        QTextStream   inputStream(&file);
        const QString firstLine = inputStream.readLine();
        file.close();

        const QChar delimiter = firstLine.count(',') >= firstLine.count(';') ? ',' : ';';

        try {
            const rapidcsv::SeparatorParams params(static_cast<char>(delimiter.unicode()));
            const rapidcsv::Document
                doc(filePath.toStdString(), rapidcsv::LabelParams(0, -1), params);

            const std::vector<std::string> fileColumnsStd = doc.GetColumnNames();
            QStringList                    fileColumns;
            for (const auto &column : fileColumnsStd) {
                fileColumns.append(QString::fromStdString(column));
            }

            QMap<int, int>                columnMapping;
            QMap<QString, QMap<int, int>> columnLengths;
            for (int i = 0; i < fileColumns.size(); i++) {
                const QString fileColumn = fileColumns[i].trimmed().toLower();
                for (const QString &standardColumn : kBusCsvColumns) {
                    if (fileColumn.contains(standardColumn, Qt::CaseInsensitive)) {
                        columnLengths[standardColumn][i] = static_cast<int>(
                            fileColumns[i].trimmed().length());
                    }
                }
            }

            for (const QString &standardColumn : kBusCsvColumns) {
                if (!columnLengths.contains(standardColumn)) {
                    continue;
                }
                int         shortestLength = INT_MAX;
                int         shortestIndex  = -1;
                const auto &lengthMap      = columnLengths[standardColumn];
                for (auto it = lengthMap.begin(); it != lengthMap.end(); ++it) {
                    if (it.value() < shortestLength) {
                        shortestLength = it.value();
                        shortestIndex  = it.key();
                    }
                }
                columnMapping[shortestIndex] = static_cast<int>(
                    kBusCsvColumns.indexOf(standardColumn));
            }

            for (size_t rowIndex = 0; rowIndex < doc.GetRowCount(); rowIndex++) {
                QStringList mappedRow(kBusCsvColumns.size());
                for (auto it = columnMapping.begin(); it != columnMapping.end(); ++it) {
                    mappedRow[it.value()] = QString::fromStdString(
                                                doc.GetCell<std::string>(it.key(), rowIndex))
                                                .trimmed();
                }

                QSocBusSignalMode row;
                row.signal      = mappedRow.value(0).trimmed();
                row.mode        = mappedRow.value(1).trimmed();
                row.direction   = mappedRow.value(2).trimmed();
                row.width       = mappedRow.value(3).trimmed();
                row.qualifier   = mappedRow.value(4).trimmed();
                row.description = mappedRow.value(5).trimmed();

                if (!row.signal.isEmpty() && !row.mode.isEmpty()) {
                    result.append(row);
                    continue;
                }
                if (warnings && mappedRow.join("").trimmed().size() > 0) {
                    warnings->append(QString("Skipping CSV row %1 with missing signal or mode")
                                         .arg(static_cast<int>(rowIndex) + 2));
                }
            }
        } catch (const std::exception &exception) {
            if (warnings) {
                warnings->append(QString("Failed to parse CSV file %1: %2")
                                     .arg(QFileInfo(filePath).fileName(), exception.what()));
            }
        }
    }

    return result;
}

YAML::Node QSocBusManager::rowsToBusYaml(
    const QString &busName, const QList<QSocBusSignalMode> &rows) const
{
    QSocBusDefinition definition;
    definition.busName = busName;
    definition.rows    = rows;
    YAML::Node result(YAML::NodeType::Map);
    result[busName.toStdString()] = busDefinitionToYaml(definition);
    return result;
}

QList<QSocBusProblem> QSocBusManager::validateBusDefinition(const QSocBusDefinition &definition) const
{
    QList<QSocBusProblem> problems;
    if (!isSafeBasename(definition.libraryName)) {
        problems.append(makeBusProblem(
            QSocBusProblemSeverity::Error,
            "invalid-library-name",
            definition,
            -1,
            {},
            {},
            "Invalid bus library name."));
    }
    if (!isSafeBasename(definition.busName)) {
        problems.append(makeBusProblem(
            QSocBusProblemSeverity::Error,
            "invalid-bus-name",
            definition,
            -1,
            {},
            {},
            "Invalid bus name."));
    }
    for (auto it = libraryMap.constBegin(); it != libraryMap.constEnd(); ++it) {
        if (it.key() != definition.libraryName && it.value().contains(definition.busName)) {
            problems.append(makeBusProblem(
                QSocBusProblemSeverity::Error,
                "duplicate-bus-name",
                definition,
                -1,
                {},
                {},
                "Bus name already exists in another loaded library."));
        }
    }

    QSet<QString> seenRows;
    for (int rowIndex = 0; rowIndex < definition.rows.size(); ++rowIndex) {
        const QSocBusSignalMode &row = definition.rows[rowIndex];
        if (row.signal.isEmpty()) {
            problems.append(makeBusProblem(
                QSocBusProblemSeverity::Error,
                "missing-signal",
                definition,
                rowIndex,
                row.signal,
                row.mode,
                "Bus row is missing a signal name."));
        }
        if (row.mode.isEmpty()) {
            problems.append(makeBusProblem(
                QSocBusProblemSeverity::Error,
                "missing-mode",
                definition,
                rowIndex,
                row.signal,
                row.mode,
                "Bus row is missing a mode name."));
        }
        if (!row.mode.isEmpty() && mapContainsScalarKey(row.signalExtraAttributes, row.mode)) {
            problems.append(makeBusProblem(
                QSocBusProblemSeverity::Error,
                "signal-attribute-mode-conflict",
                definition,
                rowIndex,
                row.signal,
                row.mode,
                "Bus row mode conflicts with a preserved signal attribute."));
        }
        const QString rowKey = row.signal + "\n" + row.mode;
        if (!row.signal.isEmpty() && !row.mode.isEmpty() && seenRows.contains(rowKey)) {
            problems.append(makeBusProblem(
                QSocBusProblemSeverity::Error,
                "duplicate-signal-mode",
                definition,
                rowIndex,
                row.signal,
                row.mode,
                "Duplicate bus signal and mode row."));
        }
        seenRows.insert(rowKey);

        const QStringList validDirections = {"in", "out", "inout"};
        if (!row.direction.isEmpty() && !validDirections.contains(row.direction)) {
            problems.append(makeBusProblem(
                QSocBusProblemSeverity::Error,
                "invalid-direction",
                definition,
                rowIndex,
                row.signal,
                row.mode,
                "Bus row direction must be in, out, or inout."));
        }
        if (!row.width.isEmpty() && !isPositiveIntegerText(row.width)) {
            problems.append(makeBusProblem(
                QSocBusProblemSeverity::Warning,
                "non-integer-width",
                definition,
                rowIndex,
                row.signal,
                row.mode,
                "Bus row width is not a positive integer."));
        }
    }

    return problems;
}

QList<QSocBusProblem> QSocBusManager::validateBusReferences(
    const QSocBusDefinition &definition, QStringList *scanErrors) const
{
    QList<QSocBusProblem> problems;
    QStringList           localScanErrors;
    QList<QSocBusUsage>   usages = scanBusUsages(definition.busName, &localScanErrors);
    if (scanErrors) {
        scanErrors->append(localScanErrors);
    }
    for (const QString &scanError : localScanErrors) {
        problems.append(makeBusProblem(
            QSocBusProblemSeverity::Error,
            "reference-scan-failed",
            definition,
            -1,
            {},
            {},
            scanError));
    }

    QSet<QString> signalNames;
    QSet<QString> modes;
    for (const QSocBusSignalMode &row : definition.rows) {
        if (!row.signal.isEmpty()) {
            signalNames.insert(row.signal);
        }
        if (!row.mode.isEmpty()) {
            modes.insert(row.mode);
        }
    }

    for (const QSocBusUsage &usage : usages) {
        if (!usage.mode.isEmpty() && !modes.contains(usage.mode)) {
            QSocBusProblem problem = makeBusProblem(
                QSocBusProblemSeverity::Error,
                "missing-referenced-mode",
                definition,
                -1,
                {},
                usage.mode,
                "Module interface references a mode that is not in the bus.");
            problem.moduleLibrary = usage.moduleLibrary;
            problem.moduleName    = usage.moduleName;
            problem.interfaceName = usage.interfaceName;
            problems.append(problem);
        }
        for (const QString &signal : usage.mappingSignals) {
            if (signalNames.contains(signal)) {
                continue;
            }
            QSocBusProblem problem = makeBusProblem(
                QSocBusProblemSeverity::Error,
                "missing-mapping-signal",
                definition,
                -1,
                signal,
                usage.mode,
                "Module mapping references a signal that is not in the bus.");
            problem.moduleLibrary = usage.moduleLibrary;
            problem.moduleName    = usage.moduleName;
            problem.interfaceName = usage.interfaceName;
            problems.append(problem);
        }
        for (const QString &signal : usage.emptyMappingSignals) {
            QSocBusProblem problem = makeBusProblem(
                QSocBusProblemSeverity::Warning,
                "empty-mapping-value",
                definition,
                -1,
                signal,
                usage.mode,
                "Module mapping has an empty signal target.");
            problem.moduleLibrary = usage.moduleLibrary;
            problem.moduleName    = usage.moduleName;
            problem.interfaceName = usage.interfaceName;
            problems.append(problem);
        }
    }

    return problems;
}

QList<QSocBusUsage> QSocBusManager::scanBusUsages(
    const QString &busName, QStringList *scanErrors) const
{
    QList<QSocBusUsage> result;
    if (!projectManager || !projectManager->isValidModulePath()) {
        if (scanErrors) {
            scanErrors->append("Project module path is invalid.");
        }
        return result;
    }

    const QDir moduleDir(
        projectManager->getModulePath(),
        "*.soc_mod",
        QDir::SortFlag::Name | QDir::SortFlag::IgnoreCase,
        QDir::Files | QDir::NoDotAndDotDot);
    for (const QString &fileName : moduleDir.entryList()) {
        const QString libraryName = QFileInfo(fileName).completeBaseName();
        const QString filePath    = moduleDir.filePath(fileName);
        try {
            const YAML::Node libraryYaml = YAML::LoadFile(filePath.toStdString());
            if (!libraryYaml || !libraryYaml.IsMap()) {
                continue;
            }
            for (YAML::const_iterator moduleIt = libraryYaml.begin(); moduleIt != libraryYaml.end();
                 ++moduleIt) {
                if (!moduleIt->first.IsScalar() || !moduleIt->second.IsMap()
                    || !moduleIt->second["bus"] || !moduleIt->second["bus"].IsMap()) {
                    continue;
                }
                const QString moduleName = QString::fromStdString(moduleIt->first.as<std::string>());
                const YAML::Node busYaml = moduleIt->second["bus"];
                for (YAML::const_iterator interfaceIt = busYaml.begin();
                     interfaceIt != busYaml.end();
                     ++interfaceIt) {
                    if (!interfaceIt->first.IsScalar() || !interfaceIt->second.IsMap()) {
                        continue;
                    }
                    const QString referencedBus = scalarToString(interfaceIt->second["bus"]);
                    if (referencedBus.isEmpty()
                        || (!busName.isEmpty() && referencedBus != busName)) {
                        continue;
                    }

                    QSocBusUsage usage;
                    usage.moduleLibrary = libraryName;
                    usage.moduleName    = moduleName;
                    usage.interfaceName = QString::fromStdString(
                        interfaceIt->first.as<std::string>());
                    usage.busName                = referencedBus;
                    usage.mode                   = scalarToString(interfaceIt->second["mode"]);
                    const YAML::Node mappingYaml = interfaceIt->second["mapping"];
                    if (mappingYaml && mappingYaml.IsMap()) {
                        for (YAML::const_iterator mappingIt = mappingYaml.begin();
                             mappingIt != mappingYaml.end();
                             ++mappingIt) {
                            if (!mappingIt->first.IsScalar()) {
                                continue;
                            }
                            const QString signal = QString::fromStdString(
                                mappingIt->first.as<std::string>());
                            usage.mappingSignals.append(signal);
                            if (scalarToString(mappingIt->second).isEmpty())
                                usage.emptyMappingSignals.append(signal);
                        }
                        usage.mappingSignals.sort(Qt::CaseInsensitive);
                        usage.emptyMappingSignals.sort(Qt::CaseInsensitive);
                    }
                    result.append(usage);
                }
            }
        } catch (const YAML::Exception &exception) {
            if (scanErrors) {
                scanErrors->append(QString("Failed to parse module library %1: %2")
                                       .arg(libraryName, exception.what()));
            }
        }
    }

    return result;
}

bool QSocBusManager::renameBusReferences(
    const QString &oldBusName,
    const QString &newBusName,
    QStringList   *changedModules,
    QStringList   *errors) const
{
    return rewriteModuleBusReferences(
        projectManager,
        BusReferenceRewrite::BusName,
        {},
        oldBusName,
        newBusName,
        changedModules,
        errors);
}

bool QSocBusManager::renameSignalReferences(
    const QString &busName,
    const QString &oldSignalName,
    const QString &newSignalName,
    QStringList   *changedModules,
    QStringList   *errors) const
{
    return rewriteModuleBusReferences(
        projectManager,
        BusReferenceRewrite::SignalName,
        busName,
        oldSignalName,
        newSignalName,
        changedModules,
        errors);
}

bool QSocBusManager::renameModeReferences(
    const QString &busName,
    const QString &oldModeName,
    const QString &newModeName,
    QStringList   *changedModules,
    QStringList   *errors) const
{
    return rewriteModuleBusReferences(
        projectManager,
        BusReferenceRewrite::ModeName,
        busName,
        oldModeName,
        newModeName,
        changedModules,
        errors);
}

YAML::Node QSocBusManager::mergeNodes(const YAML::Node &toYaml, const YAML::Node &fromYaml)
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

void QSocBusManager::libraryMapAdd(const QString &libraryName, const QString &busName)
{
    /* Check if the library exists in the map */
    if (!libraryMap.contains(libraryName)) {
        /* If the key doesn't exist, create a new QSet and insert the busName */
        QSet<QString> busSet;
        busSet.insert(busName);
        libraryMap.insert(libraryName, busSet);
    } else {
        /* If the key exists, just add the busName to the existing QSet */
        libraryMap[libraryName].insert(busName);
    }
}

void QSocBusManager::libraryMapRemove(const QString &libraryName, const QString &busName)
{
    /* Check if the library exists in the map */
    if (libraryMap.contains(libraryName)) {
        QSet<QString> &buses = libraryMap[libraryName];

        /* Remove the bus if it exists in the set */
        buses.remove(busName);

        /* If the set becomes empty after removal, delete the library entry */
        if (buses.isEmpty()) {
            libraryMap.remove(libraryName);
        }
    }
}

bool QSocBusManager::saveLibraryYaml(const QString &libraryName, const YAML::Node &libraryYaml)
{
    YAML::Node localLibraryYaml;
    /* Validate projectManager and its path */
    if (!isBusPathValid()) {
        QSocConsole::error() << "projectManager is null or invalid bus path.";
        return false;
    }
    /* Check file path */
    const QString &busPath  = projectManager->getBusPath();
    const QString &filePath = QDir(busPath).filePath(QString("%1.soc_bus").arg(libraryName));
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
    return true;
}

QStringList QSocBusManager::listLibrary(const QRegularExpression &libraryNameRegex)
{
    QStringList result;
    /* Validate projectManager and its path */
    if (!isBusPathValid()) {
        QSocConsole::error() << "projectManager is null or invalid bus path.";
        return result;
    }
    /* Validate libraryNameRegex */
    if (!QStaticRegex::isNameRegexValid(libraryNameRegex)) {
        QSocConsole::error() << "Invalid or empty regex:" << libraryNameRegex.pattern();
        return result;
    }
    /* QDir for '.soc_bus' files in bus path, sorted by name. */
    const QDir busPathDir(
        projectManager->getBusPath(),
        "*.soc_bus",
        QDir::SortFlag::Name | QDir::SortFlag::IgnoreCase,
        QDir::Files | QDir::NoDotAndDotDot);
    /* Add matching file basenames from projectDir to result list. */
    foreach (const QString &filename, busPathDir.entryList()) {
        if (QStaticRegex::isNameExactMatch(filename, libraryNameRegex)) {
            result.append(filename.split('.').first());
        }
    }

    return result;
}

bool QSocBusManager::isLibraryFileExist(const QString &libraryName)
{
    /* Validate projectManager and its bus path */
    if (!isBusPathValid()) {
        QSocConsole::error() << "projectManager is null or invalid bus path.";
        return false;
    }

    /* Check library basename */
    if (libraryName.isEmpty()) {
        QSocConsole::error() << "library basename is empty.";
        return false;
    }

    /* Get the full file path by joining bus path and basename with extension */
    const QString filePath = QDir(projectManager->getBusPath()).filePath(libraryName + ".soc_bus");

    /* Check if library file exists */
    return QFile::exists(filePath);
}

bool QSocBusManager::load(const QString &libraryName)
{
    /* Validate projectManager and its path */
    if (!isBusPathValid()) {
        QSocConsole::error() << "projectManager is null or invalid bus path.";
        return false;
    }

    /* Check if library file exists */
    if (!isLibraryFileExist(libraryName)) {
        QSocConsole::error() << "Library file does not exist for basename:" << libraryName;
        return false;
    }

    /* Get the full file path by joining bus path and basename with extension */
    const QString filePath = QDir(projectManager->getBusPath()).filePath(libraryName + ".soc_bus");

    /* Open the YAML file */
    std::ifstream fileStream(filePath.toStdString());
    if (!fileStream.is_open()) {
        QSocConsole::error() << "Unable to open file:" << filePath;
        return false;
    }

    try {
        /* Load YAML content into a temporary node */
        YAML::Node tempNode = YAML::Load(fileStream);

        /* Iterate through the temporary node and add to busData */
        for (YAML::const_iterator it = tempNode.begin(); it != tempNode.end(); ++it) {
            const auto key = it->first.as<std::string>();
            if (busData[key] && busData[key]["library"] && busData[key]["library"].IsScalar()) {
                const QString loadedLibrary = QString::fromStdString(
                    busData[key]["library"].as<std::string>());
                if (loadedLibrary != libraryName) {
                    QSocConsole::error() << "Duplicate bus name" << QString::fromStdString(key)
                                         << "already loaded from library" << loadedLibrary;
                    return false;
                }
            }

            /* Add to busData */
            busData[key] = it->second;

            /* Check if this is old format (no "port" node) and reject it */
            if (!busData[key]["port"]) {
                QSocConsole::error() << "Bus" << QString::fromStdString(key)
                                     << "has invalid structure (missing 'port' node)";
                /* Remove invalid bus data */
                busData.remove(key);
                return false;
            }

            busData[key]["library"] = libraryName.toStdString();

            /* Update libraryMap with libraryName to key mapping */
            libraryMapAdd(libraryName, QString::fromStdString(key));
        }
    } catch (const YAML::Exception &e) {
        QSocConsole::error() << "failed to parse YAML file:" << filePath << ":" << e.what();
        return false;
    }

    return true;
}

bool QSocBusManager::load(const QRegularExpression &libraryNameRegex)
{
    /* Validate projectManager and its path */
    if (!isBusPathValid()) {
        QSocConsole::error() << "projectManager is null or invalid bus path.";
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

bool QSocBusManager::load(const QStringList &libraryNameList)
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

bool QSocBusManager::remove(const QString &libraryName)
{
    /* Validate projectManager and its bus path */
    if (!isBusPathValid()) {
        QSocConsole::error() << "projectManager is null or invalid bus path.";
        return false;
    }

    /* Check library basename */
    if (libraryName.isEmpty()) {
        QSocConsole::error() << "library basename is empty.";
        return false;
    }

    /* Get the full file path */
    const QString filePath = QDir(projectManager->getBusPath()).filePath(libraryName + ".soc_bus");

    /* Check if library file exists */
    if (!QFile::exists(filePath)) {
        QSocConsole::error() << "library file does not exist for basename:" << libraryName;
        return false;
    }

    /* Remove the file */
    if (!QFile::remove(filePath)) {
        QSocConsole::error() << "Failed to remove bus file:" << filePath;
        return false;
    }

    /* Remove from busData and libraryMap */
    busData.remove(libraryName.toStdString());
    libraryMap.remove(libraryName);

    return true;
}

bool QSocBusManager::remove(const QRegularExpression &libraryNameRegex)
{
    /* Validate projectManager and its path */
    if (!isBusPathValid()) {
        QSocConsole::error() << "projectManager is null or invalid bus path.";
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

bool QSocBusManager::remove(const QStringList &libraryNameList)
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

bool QSocBusManager::save(const QString &libraryName)
{
    /* Validate projectManager and its path */
    if (!isBusPathValid()) {
        QSocConsole::error() << "projectManager is null or invalid library path.";
        return false;
    }

    /* Check if the libraryName exists in libraryMap */
    if (!libraryMap.contains(libraryName)) {
        QSocConsole::error() << "Library basename not found in libraryMap.";
        return false;
    }

    /* Extract buses from busData */
    YAML::Node dataToSave;
    /* Iterate through each bus in libraryMap */
    for (const auto &busItem : libraryMap[libraryName]) {
        const std::string busNameStd = busItem.toStdString();
        if (!busData[busNameStd]) {
            QSocConsole::error() << "Bus data is not exist: " << busNameStd;
            return false;
        }
        dataToSave[busNameStd] = busData[busNameStd];
        if (dataToSave[busNameStd]["library"]) {
            dataToSave[busNameStd].remove("library");
        }
    }

    /* Serialize and save to file */
    const QString filePath = QDir(projectManager->getBusPath()).filePath(libraryName + ".soc_bus");
    std::ofstream outputFileStream(filePath.toStdString());
    if (!outputFileStream.is_open()) {
        QSocConsole::error() << "Unable to open file for writing:" << filePath;
        return false;
    }
    outputFileStream << dataToSave;
    return true;
}

bool QSocBusManager::save(const QRegularExpression &libraryNameRegex)
{
    bool allSaved = true;

    /* Validate projectManager and its path */
    if (!isBusPathValid()) {
        QSocConsole::error() << "projectManager is null or invalid bus path.";
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

bool QSocBusManager::save(const QStringList &libraryNameList)
{
    /* Validate projectManager and its path */
    if (!isBusPathValid()) {
        QSocConsole::error() << "projectManager is null or invalid bus path.";
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

QStringList QSocBusManager::listBus(const QRegularExpression &busNameRegex)
{
    QStringList result;
    /* Validate busNameRegex */
    if (!QStaticRegex::isNameRegexValid(busNameRegex)) {
        QSocConsole::error() << "Invalid or empty regex:" << busNameRegex.pattern();
        return result;
    }

    /* Iterate through each node in busData */
    for (YAML::const_iterator it = busData.begin(); it != busData.end(); ++it) {
        const QString busName = QString::fromStdString(it->first.as<std::string>());

        /* Check if the bus name matches the regex */
        if (QStaticRegex::isNameExactMatch(busName, busNameRegex)) {
            result.append(busName);
        }
    }

    return result;
}

bool QSocBusManager::removeBus(const QRegularExpression &busNameRegex)
{
    /* Validate projectManager and its path */
    if (!isBusPathValid()) {
        QSocConsole::error() << "projectManager is null or invalid bus path.";
        return false;
    }
    /* Validate busNameRegex */
    if (!QStaticRegex::isNameRegexValid(busNameRegex)) {
        QSocConsole::error() << "Invalid or empty regex:" << busNameRegex.pattern();
        return false;
    }

    QSet<QString> libraryToSave;
    QSet<QString> libraryToRemove;
    QSet<QString> busToRemove;

    for (auto busDataIter = busData.begin(); busDataIter != busData.end(); ++busDataIter) {
        const QString busName = QString::fromStdString(busDataIter->first.as<std::string>());
        if (QStaticRegex::isNameExactMatch(busName, busNameRegex)) {
            busToRemove.insert(busName);
            const QString libraryName = QString::fromStdString(
                busDataIter->second["library"].as<std::string>());
            libraryToSave.insert(libraryName);
        }
    }

    /* Remove buses from busData */
    for (const QString &busName : busToRemove) {
        const QString libraryName = QString::fromStdString(
            busData[busName.toStdString()]["library"].as<std::string>());
        libraryMapRemove(libraryName, busName);
        if (!libraryMap.contains(libraryName)) {
            libraryToRemove.insert(libraryName);
        }
        busData.remove(busName.toStdString());
    }

    /* Ensure libraryToSave does not include buses marked for removal */
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

    /* Remove buses with no remaining associations in libraryMap */
    if (!remove(libraryToRemoveList)) {
        QSocConsole::error() << "Failed to remove buses.";
        return false;
    }

    return true;
}

YAML::Node QSocBusManager::getBusYaml(const QString &busName)
{
    YAML::Node result;

    /* Check if bus exists in busData */
    if (!isBusExist(busName)) {
        QSocConsole::warn() << "Bus does not exist:" << busName;
        return result;
    }

    /* Get bus YAML node from busData */
    result = busData[busName.toStdString()];

    /* Check for required port structure */
    if (!result["port"]) {
        QSocConsole::warn() << "Bus" << busName << "has invalid structure (missing 'port' node)";
    }

    return result;
}

bool QSocBusManager::isBusExist(const QString &busName)
{
    return busData[busName.toStdString()].IsDefined();
}

bool QSocBusManager::isBusExist(const QRegularExpression &busNameRegex)
{
    /* Check if the regex is valid, if not, return false */
    if (!QStaticRegex::isNameRegexValid(busNameRegex)) {
        QSocConsole::warn() << "Invalid regular expression provided.";
        return false;
    }

    /* Iterate over the busData to find matches */
    for (YAML::const_iterator it = busData.begin(); it != busData.end(); ++it) {
        const QString busName = QString::fromStdString(it->first.as<std::string>());

        /* Check if the bus name matches the regex */
        if (QStaticRegex::isNameExactMatch(busName, busNameRegex)) {
            return true;
        }
    }

    return false;
}

QString QSocBusManager::getBusLibrary(const QString &busName)
{
    if (!isBusExist(busName)) {
        return {};
    }
    return QString::fromStdString(busData[busName.toStdString()]["library"].as<std::string>());
}

YAML::Node QSocBusManager::getBusYamls(const QRegularExpression &busNameRegex)
{
    YAML::Node result;

    /* Check if the regex is valid, if not, return an empty node */
    if (!QStaticRegex::isNameRegexValid(busNameRegex)) {
        QSocConsole::warn() << "Invalid regular expression provided.";
        return result;
    }

    /* Iterate over the busData to find matches */
    for (YAML::const_iterator it = busData.begin(); it != busData.end(); ++it) {
        const QString busName = QString::fromStdString(it->first.as<std::string>());

        /* Check if the bus name matches the regex */
        if (QStaticRegex::isNameExactMatch(busName, busNameRegex)) {
            /* Add the bus node to the result */
            result[busName.toStdString()] = it->second;
        }
    }

    return result;
}
