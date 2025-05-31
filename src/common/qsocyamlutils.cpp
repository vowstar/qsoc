// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "qsocyamlutils.h"

#include <QDebug>
#include <QFile>
#include <QFileInfo>

#include <fstream>
#include <sstream>

YAML::Node QSocYamlUtils::mergeNodes(const YAML::Node &toYaml, const YAML::Node &fromYaml)
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
        if (iter.first.IsScalar()) {
            const std::string &key = iter.first.Scalar();
            /* Check if the key exists in resultYaml without creating it */
            bool keyExists = false;
            for (auto resultIter : resultYaml) {
                if (resultIter.first.IsScalar() && resultIter.first.Scalar() == key) {
                    keyExists = true;
                    break;
                }
            }
            if (!keyExists) {
                resultYaml[iter.first] = iter.second;
            }
        } else {
            /* Non-scalar keys, add directly */
            resultYaml[iter.first] = iter.second;
        }
    }
    return resultYaml;
}

YAML::Node QSocYamlUtils::loadAndMergeFiles(
    const QStringList &filePathList, const YAML::Node &baseNode)
{
    YAML::Node mergedResult = baseNode;
    bool       isFirstFile  = (baseNode.IsNull() || !baseNode.IsDefined());

    for (const QString &filePath : filePathList) {
        /* Check if file exists */
        if (!QFile::exists(filePath)) {
            qCritical() << "Error: YAML file does not exist:" << filePath;
            return YAML::Node(); /* Return null node on error */
        }

        /* Load the file */
        std::ifstream fileStream(filePath.toStdString());
        if (!fileStream.is_open()) {
            qCritical() << "Error: Unable to open YAML file:" << filePath;
            return YAML::Node(); /* Return null node on error */
        }

        try {
            YAML::Node currentNode = YAML::Load(fileStream);
            fileStream.close();

            if (isFirstFile) {
                /* For the first file (or if no base node), use it as the base */
                mergedResult = currentNode;
                isFirstFile  = false;
            } else {
                /* For subsequent files, merge them */
                mergedResult = mergeNodes(mergedResult, currentNode);
            }

            qDebug() << "Successfully loaded and merged YAML file:" << filePath;

        } catch (const YAML::Exception &e) {
            qCritical() << "Error parsing YAML file:" << filePath << ":" << e.what();
            return YAML::Node(); /* Return null node on error */
        }
    }

    return mergedResult;
}

bool QSocYamlUtils::validateNetlistStructure(const YAML::Node &yamlNode, QString &errorMessage)
{
    try {
        /* Check if the node is valid */
        if (!yamlNode.IsDefined() || yamlNode.IsNull()) {
            errorMessage = "YAML node is null or undefined";
            return false;
        }

        /* Check if it's a map */
        if (!yamlNode.IsMap()) {
            errorMessage = "YAML root must be a map/object";
            return false;
        }

        /* Check for required 'instance' section */
        if (!yamlNode["instance"]) {
            errorMessage = "Missing required 'instance' section";
            return false;
        }

        if (!yamlNode["instance"].IsMap() || yamlNode["instance"].size() == 0) {
            errorMessage = "'instance' section must be a non-empty map";
            return false;
        }

        /* Validate optional sections if they exist */
        if (yamlNode["net"] && !yamlNode["net"].IsMap()) {
            errorMessage = "'net' section must be a map";
            return false;
        }

        if (yamlNode["bus"] && !yamlNode["bus"].IsMap()) {
            errorMessage = "'bus' section must be a map";
            return false;
        }

        if (yamlNode["port"] && !yamlNode["port"].IsMap()) {
            errorMessage = "'port' section must be a map";
            return false;
        }

        /* Validation passed */
        errorMessage.clear();
        return true;

    } catch (const YAML::Exception &e) {
        errorMessage = QString("YAML validation error: %1").arg(e.what());
        return false;
    } catch (const std::exception &e) {
        errorMessage = QString("Standard validation error: %1").arg(e.what());
        return false;
    }
}

QString QSocYamlUtils::yamlNodeToString(const YAML::Node &yamlNode, int indentSize)
{
    try {
        YAML::Emitter emitter;
        emitter.SetIndent(indentSize);
        emitter.SetMapFormat(YAML::Block);
        emitter.SetSeqFormat(YAML::Block);
        emitter << yamlNode;

        return QString::fromStdString(emitter.c_str());
    } catch (const YAML::Exception &e) {
        qWarning() << "Error converting YAML node to string:" << e.what();
        return QString("Error: Failed to convert YAML node to string");
    }
}

YAML::Node QSocYamlUtils::cloneNode(const YAML::Node &original)
{
    try {
        /* Convert to string and parse back to create a deep copy */
        YAML::Emitter emitter;
        emitter << original;
        std::string yamlString = emitter.c_str();

        return YAML::Load(yamlString);
    } catch (const YAML::Exception &e) {
        qWarning() << "Error cloning YAML node:" << e.what();
        return YAML::Node(); /* Return null node on error */
    }
}

bool QSocYamlUtils::hasKeyPath(const YAML::Node &yamlNode, const QString &keyPath)
{
    if (keyPath.isEmpty()) {
        return false;
    }

    try {
        QStringList keys        = keyPath.split('.', Qt::SkipEmptyParts);
        YAML::Node  currentNode = yamlNode;

        for (const QString &key : keys) {
            std::string stdKey = key.toStdString();

            if (!currentNode.IsMap() || !currentNode[stdKey]) {
                return false;
            }

            currentNode = currentNode[stdKey];
        }

        return true;
    } catch (const YAML::Exception &e) {
        qWarning() << "Error checking key path:" << keyPath << ":" << e.what();
        return false;
    }
}

QString QSocYamlUtils::getValueByKeyPath(
    const YAML::Node &yamlNode, const QString &keyPath, const QString &defaultValue)
{
    if (keyPath.isEmpty()) {
        return defaultValue;
    }

    try {
        QStringList keys        = keyPath.split('.', Qt::SkipEmptyParts);
        YAML::Node  currentNode = yamlNode;

        for (const QString &key : keys) {
            std::string stdKey = key.toStdString();

            if (!currentNode.IsMap() || !currentNode[stdKey]) {
                return defaultValue;
            }

            currentNode = currentNode[stdKey];
        }

        if (currentNode.IsScalar()) {
            return QString::fromStdString(currentNode.as<std::string>());
        } else {
            /* For non-scalar values, convert to string representation */
            return yamlNodeToString(currentNode);
        }

    } catch (const YAML::Exception &e) {
        qWarning() << "Error getting value by key path:" << keyPath << ":" << e.what();
        return defaultValue;
    }
}

bool QSocYamlUtils::setValueByKeyPath(
    YAML::Node &yamlNode, const QString &keyPath, const QString &value)
{
    if (keyPath.isEmpty()) {
        return false;
    }

    try {
        QStringList keys = keyPath.split('.', Qt::SkipEmptyParts);

        if (keys.isEmpty()) {
            return false;
        }

        /* Ensure the root node is a map */
        if (!yamlNode.IsMap()) {
            yamlNode = YAML::Node(YAML::NodeType::Map);
        }

        YAML::Node currentNode = yamlNode;

        /* Navigate to the parent of the target key, creating intermediate nodes as needed */
        for (int i = 0; i < keys.size() - 1; ++i) {
            std::string stdKey = keys[i].toStdString();

            if (!currentNode[stdKey] || !currentNode[stdKey].IsMap()) {
                currentNode[stdKey] = YAML::Node(YAML::NodeType::Map);
            }

            currentNode = currentNode[stdKey];
        }

        /* Set the final value */
        std::string finalKey  = keys.last().toStdString();
        currentNode[finalKey] = value.toStdString();

        return true;

    } catch (const YAML::Exception &e) {
        qWarning() << "Error setting value by key path:" << keyPath << ":" << e.what();
        return false;
    }
}
