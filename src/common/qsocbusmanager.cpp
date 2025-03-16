#include "common/qsocbusmanager.h"

#include "common/qstaticregex.h"

#include <QDebug>
#include <QDir>
#include <QFile>

#include <fstream>
#include <string>
#include <vector>

#include <rapidcsv.h>

QSocBusManager::QSocBusManager(QObject *parent, QSocProjectManager *projectManager)
    : QObject{parent}
{
    /* Set projectManager */
    setProjectManager(projectManager);
}

void QSocBusManager::setProjectManager(QSocProjectManager *projectManager)
{
    /* Set projectManager */
    if (projectManager) {
        this->projectManager = projectManager;
    }
}

QSocProjectManager *QSocBusManager::getProjectManager()
{
    return projectManager;
}

bool QSocBusManager::isBusPathValid()
{
    /* Validate projectManager */
    if (!projectManager) {
        qCritical() << "Error: projectManager is nullptr.";
        return false;
    }
    /* Validate bus path. */
    if (!projectManager->isValidBusPath()) {
        qCritical() << "Error: Invalid bus path:" << projectManager->getBusPath();
        return false;
    }
    return true;
}

bool QSocBusManager::importFromFileList(
    const QString &libraryName, const QString &busName, const QStringList &filePathList)
{
    /* Check if libraryName is empty */
    if (libraryName.isEmpty()) {
        qCritical() << "Error: library name is empty.";
        return false;
    }
    /* Check if busName is empty */
    if (busName.isEmpty()) {
        qCritical() << "Error: bus name is empty.";
        return false;
    }
    /* Define standard column names */
    const QStringList standardColumns
        = {"name", "mode", "direction", "width", "qualifier", "description"};

    /* Initialize result CSV data */
    QStringList        resultHeaders = standardColumns;
    QList<QStringList> resultData;

    /* Process each CSV file */
    for (const QString &filePath : filePathList) {
        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            continue;
        }

        QTextStream in(&file);
        QString     firstLine = in.readLine();
        file.close();

        /* Auto-detect delimiter */
        QChar delimiter = firstLine.count(',') >= firstLine.count(';') ? ',' : ';';

        /* Configure rapidcsv */
        rapidcsv::SeparatorParams params(static_cast<char>(delimiter.unicode()));

        /* Parse CSV file using rapidcsv */
        rapidcsv::Document doc(filePath.toStdString(), rapidcsv::LabelParams(0, -1), params);

        /* Get column names from first row */
        std::vector<std::string> fileColumnsStd = doc.GetColumnNames();
        QStringList              fileColumns;
        for (const auto &col : fileColumnsStd) {
            fileColumns.append(QString::fromStdString(col));
        }

        /* Map file columns to standard columns */
        QMap<int, int> columnMapping; /* maps file column index to standard column index */

        /* For each standard column, track matching columns and their lengths */
        QMap<QString, QMap<int, int>> columnLengths; /* standard column -> {file index -> length} */

        /* First pass - find all potential matches and their lengths */
        for (int i = 0; i < fileColumns.size(); i++) {
            QString fileCol = fileColumns[i].trimmed().toLower();

            /* Check against each standard column */
            for (const QString &stdCol : standardColumns) {
                if (fileCol.contains(stdCol, Qt::CaseInsensitive)) {
                    if (!columnLengths.contains(stdCol)) {
                        columnLengths[stdCol] = QMap<int, int>();
                    }
                    columnLengths[stdCol][i] = fileColumns[i].trimmed().length();
                }
            }
        }

        /* Second pass - map shortest column for each standard column */
        for (const QString &stdCol : standardColumns) {
            if (columnLengths.contains(stdCol)) {
                /* Find column with shortest name */
                int shortestLength = INT_MAX;
                int shortestIndex  = -1;

                const auto &lengthMap = columnLengths[stdCol];
                for (auto it = lengthMap.begin(); it != lengthMap.end(); ++it) {
                    if (it.value() < shortestLength) {
                        shortestLength = it.value();
                        shortestIndex  = it.key();
                    }
                }

                /* Map the shortest column */
                columnMapping[shortestIndex] = standardColumns.indexOf(stdCol);
            }
        }

        /* Read and map data rows */
        for (size_t rowIdx = 0; rowIdx < doc.GetRowCount(); rowIdx++) {
            QStringList mappedRow(standardColumns.size());

            for (auto it = columnMapping.begin(); it != columnMapping.end(); ++it) {
                int fileColIdx = it.key();
                int stdColIdx  = it.value();

                std::string cellValue = doc.GetCell<std::string>(fileColIdx, rowIdx);
                mappedRow[stdColIdx]  = QString::fromStdString(cellValue);
            }

            resultData.append(mappedRow);
        }
    }

    /* Convert CSV data to YAML format */
    YAML::Node busYaml;

    for (const QStringList &row : resultData) {
        /* Ensure we have all required fields */
        if (row.size() >= 6) {
            QString signalName = row[0].trimmed();
            QString mode       = row[1].trimmed();
            QString direction  = row[2].trimmed();
            QString width      = row[3].trimmed();
            QString qualifier  = row[4].trimmed();

            if (!signalName.isEmpty() && !mode.isEmpty()) {
                /* Create nested structure: busName -> signalName -> mode -> properties */
                if (!direction.isEmpty()) {
                    busYaml[busName.toStdString()][signalName.toStdString()][mode.toStdString()]
                           ["direction"]
                        = direction.toStdString();
                }
                if (!width.isEmpty()) {
                    busYaml[busName.toStdString()][signalName.toStdString()][mode.toStdString()]
                           ["width"]
                        = width.toStdString();
                }
                if (!qualifier.isEmpty()) {
                    busYaml[busName.toStdString()][signalName.toStdString()][mode.toStdString()]
                           ["qualifier"]
                        = qualifier.toStdString();
                }
            }
        }
    }

    /* Save YAML file */
    if (!saveLibraryYaml(libraryName, busYaml)) {
        qCritical() << "Error: Failed to save bus library YAML file";
        return false;
    }

    return true;
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
        qCritical() << "Error: projectManager is null or invalid bus path.";
        return false;
    }
    /* Check file path */
    const QString &busPath  = projectManager->getBusPath();
    const QString &filePath = busPath + "/" + libraryName + ".soc_bus";
    if (QFile::exists(filePath)) {
        /* Load library YAML file */
        std::ifstream inputFileStream(filePath.toStdString());
        localLibraryYaml = mergeNodes(YAML::Load(inputFileStream), libraryYaml);
        qDebug() << "Load and merge";
    } else {
        localLibraryYaml = libraryYaml;
    }

    /* Save YAML file */
    std::ofstream outputFileStream(filePath.toStdString());
    if (!outputFileStream.is_open()) {
        qCritical() << "Error: Unable to open file for writing:" << filePath;
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
        qCritical() << "Error: projectManager is null or invalid bus path.";
        return result;
    }
    /* Validate libraryNameRegex */
    if (!QStaticRegex::isNameRegexValid(libraryNameRegex)) {
        qCritical() << "Error: Invalid or empty regex:" << libraryNameRegex.pattern();
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

bool QSocBusManager::isExist(const QString &libraryName)
{
    /* Validate projectManager and its bus path */
    if (!isBusPathValid()) {
        qCritical() << "Error: projectManager is null or invalid bus path.";
        return false;
    }

    /* Check library basename */
    if (libraryName.isEmpty()) {
        qCritical() << "Error: library basename is empty.";
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
        qCritical() << "Error: projectManager is null or invalid bus path.";
        return false;
    }

    /* Check if library file exists */
    if (!isExist(libraryName)) {
        qCritical() << "Error: Library file does not exist for basename:" << libraryName;
        return false;
    }

    /* Get the full file path by joining bus path and basename with extension */
    const QString filePath = QDir(projectManager->getBusPath()).filePath(libraryName + ".soc_bus");

    /* Open the YAML file */
    std::ifstream fileStream(filePath.toStdString());
    if (!fileStream.is_open()) {
        qCritical() << "Error: Unable to open file:" << filePath;
        return false;
    }

    try {
        /* Load YAML content into a temporary node */
        YAML::Node tempNode = YAML::Load(fileStream);

        /* Iterate through the temporary node and add to busData */
        for (YAML::const_iterator it = tempNode.begin(); it != tempNode.end(); ++it) {
            const auto key = it->first.as<std::string>();

            /* Add to busData */
            busData[key]            = it->second;
            busData[key]["library"] = libraryName.toStdString();

            /* Update libraryMap with libraryName to key mapping */
            libraryMapAdd(libraryName, QString::fromStdString(key));
        }
    } catch (const YAML::Exception &e) {
        qCritical() << "Error parsing YAML file:" << filePath << ":" << e.what();
        return false;
    }

    return true;
}

bool QSocBusManager::load(const QRegularExpression &libraryNameRegex)
{
    /* Validate projectManager and its path */
    if (!isBusPathValid()) {
        qCritical() << "Error: projectManager is null or invalid bus path.";
        return false;
    }
    /* Validate libraryNameRegex */
    if (!QStaticRegex::isNameRegexValid(libraryNameRegex)) {
        qCritical() << "Error: Invalid or empty regex:" << libraryNameRegex.pattern();
        return false;
    }

    /* Get the list of library basenames matching the regex */
    const QStringList matchingBasenames = listLibrary(libraryNameRegex);

    /* Iterate through the list and load each library */
    for (const QString &basename : matchingBasenames) {
        if (!load(basename)) {
            qCritical() << "Error: Failed to load library:" << basename;
            return false;
        }
    }

    return true;
}

bool QSocBusManager::load(const QStringList &libraryNameList)
{
    if (!projectManager || !projectManager->isValid()) {
        qCritical() << "Error: Invalid or null projectManager.";
        return false;
    }

    const QSet<QString> uniqueBasenames(libraryNameList.begin(), libraryNameList.end());

    for (const QString &basename : uniqueBasenames) {
        if (!load(basename)) {
            qCritical() << "Error: Failed to load library:" << basename;
            return false;
        }
    }

    return true;
}

bool QSocBusManager::remove(const QString &libraryName)
{
    /* Validate projectManager and its bus path */
    if (!isBusPathValid()) {
        qCritical() << "Error: projectManager is null or invalid bus path.";
        return false;
    }

    /* Check library basename */
    if (libraryName.isEmpty()) {
        qCritical() << "Error: library basename is empty.";
        return false;
    }

    /* Get the full file path */
    const QString filePath = QDir(projectManager->getBusPath()).filePath(libraryName + ".soc_bus");

    /* Check if library file exists */
    if (!QFile::exists(filePath)) {
        qCritical() << "Error: library file does not exist for basename:" << libraryName;
        return false;
    }

    /* Remove the file */
    if (!QFile::remove(filePath)) {
        qCritical() << "Error: Failed to remove bus file:" << filePath;
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
        qCritical() << "Error: projectManager is null or invalid bus path.";
        return false;
    }
    /* Validate libraryNameRegex */
    if (!QStaticRegex::isNameRegexValid(libraryNameRegex)) {
        qCritical() << "Error: Invalid or empty regex:" << libraryNameRegex.pattern();
        return false;
    }

    /* Get the list of library basenames matching the regex */
    const QStringList matchingBasenames = listLibrary(libraryNameRegex);

    /* Iterate through the list and remove each library */
    for (const QString &basename : matchingBasenames) {
        if (!remove(basename)) {
            qCritical() << "Error: Failed to remove library:" << basename;
            return false;
        }
    }

    return true;
}

bool QSocBusManager::remove(const QStringList &libraryNameList)
{
    if (!projectManager || !projectManager->isValid()) {
        qCritical() << "Error: Invalid or null projectManager.";
        return false;
    }

    const QSet<QString> uniqueBasenames(libraryNameList.begin(), libraryNameList.end());

    for (const QString &basename : uniqueBasenames) {
        if (!remove(basename)) {
            qCritical() << "Error: Failed to remove library:" << basename;
            return false;
        }
    }

    return true;
}

bool QSocBusManager::save(const QString &libraryName)
{
    /* Validate projectManager and its path */
    if (!isBusPathValid()) {
        qCritical() << "Error: projectManager is null or invalid library path.";
        return false;
    }

    /* Check if the libraryName exists in libraryMap */
    if (!libraryMap.contains(libraryName)) {
        qCritical() << "Error: Library basename not found in libraryMap.";
        return false;
    }

    /* Extract buses from busData */
    YAML::Node dataToSave;
    /* Iterate through each bus in libraryMap */
    for (const auto &busItem : libraryMap[libraryName]) {
        const std::string busNameStd = busItem.toStdString();
        if (!busData[busNameStd]) {
            qCritical() << "Error: Bus data is not exist: " << busNameStd;
            return false;
        }
        dataToSave[busNameStd] = busData[busNameStd];
        dataToSave[busNameStd].remove("library");
    }

    /* Serialize and save to file */
    const QString filePath = QDir(projectManager->getBusPath()).filePath(libraryName + ".soc_bus");
    std::ofstream outputFileStream(filePath.toStdString());
    if (!outputFileStream.is_open()) {
        qCritical() << "Error: Unable to open file for writing:" << filePath;
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
        qCritical() << "Error: projectManager is null or invalid bus path.";
        return false;
    }
    /* Validate libraryNameRegex */
    if (!QStaticRegex::isNameRegexValid(libraryNameRegex)) {
        qCritical() << "Error: Invalid or empty regex:" << libraryNameRegex.pattern();
        return false;
    }

    /* Iterate over libraryMap and save matching libraries */
    for (const QString &libraryName : libraryMap.keys()) {
        if (QStaticRegex::isNameExactMatch(libraryName, libraryNameRegex)) {
            if (!save(libraryName)) {
                qCritical() << "Error: Failed to save library:" << libraryName;
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
        qCritical() << "Error: projectManager is null or invalid bus path.";
        return false;
    }

    const QSet<QString> uniqueBasenames(libraryNameList.begin(), libraryNameList.end());

    for (const QString &basename : uniqueBasenames) {
        if (!save(basename)) {
            qCritical() << "Error: Failed to save library:" << basename;
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
        qCritical() << "Error: Invalid or empty regex:" << busNameRegex.pattern();
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
        qCritical() << "Error: projectManager is null or invalid bus path.";
        return false;
    }
    /* Validate busNameRegex */
    if (!QStaticRegex::isNameRegexValid(busNameRegex)) {
        qCritical() << "Error: Invalid or empty regex:" << busNameRegex.pattern();
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
        qCritical() << "Error: Failed to save libraries.";
        return false;
    }

    /* Remove buses with no remaining associations in libraryMap */
    if (!remove(libraryToRemoveList)) {
        qCritical() << "Error: Failed to remove buses.";
        return false;
    }

    return true;
}

YAML::Node QSocBusManager::getBusNode(const QRegularExpression &busNameRegex)
{
    YAML::Node result;

    /* Check if the regex is valid, if not, return an empty node */
    if (!QStaticRegex::isNameRegexValid(busNameRegex)) {
        qWarning() << "Invalid regular expression provided.";
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
