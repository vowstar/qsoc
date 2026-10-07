// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "qsocyamlutils.h"
#include "common/qsocconsole.h"

#include <QDebug>
#include <QFile>
#include <QFileInfo>

#include <fstream>
#include <sstream>

YAML::Node QSocYamlUtils::mergeLibraryNodes(const YAML::Node &toYaml, const YAML::Node &fromYaml)
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
                resultYaml[iter.first] = mergeLibraryNodes(iter.second, tempYaml);
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

YAML::Node QSocYamlUtils::mergeNodes(const YAML::Node &toYaml, const YAML::Node &fromYaml)
{
    /* Handle null cases */
    if (fromYaml.IsNull() || !fromYaml.IsDefined()) {
        return toYaml;
    }
    if (toYaml.IsNull() || !toYaml.IsDefined()) {
        return fromYaml;
    }

    /* Handle sequence merging - concatenate sequences instead of replacing */
    if (toYaml.IsSequence() && fromYaml.IsSequence()) {
        YAML::Node resultSequence = YAML::Node(YAML::NodeType::Sequence);

        /* Add all elements from toYaml first */
        for (const auto &item : toYaml) {
            resultSequence.push_back(item);
        }

        /* Add all elements from fromYaml */
        for (const auto &item : fromYaml) {
            resultSequence.push_back(item);
        }

        return resultSequence;
    }

    /* If one is sequence and other is not, replace with fromYaml */
    if (toYaml.IsSequence() || fromYaml.IsSequence()) {
        return fromYaml;
    }

    /* Handle scalar and other non-map types - fromYaml takes precedence */
    if (!fromYaml.IsMap()) {
        return fromYaml;
    }
    if (!toYaml.IsMap()) {
        return fromYaml;
    }

    /* Handle empty map cases */
    if (!fromYaml.size()) {
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
            QSocConsole::error() << "YAML file does not exist:" << filePath;
            return {}; /* Return null node on error */
        }

        /* Load the file */
        std::ifstream fileStream(filePath.toStdString());
        if (!fileStream.is_open()) {
            QSocConsole::error() << "Unable to open YAML file:" << filePath;
            return {}; /* Return null node on error */
        }

        try {
            const YAML::Node currentNode = YAML::Load(fileStream);
            fileStream.close();

            if (isFirstFile) {
                /* For the first file (or if no base node), use it as the base */
                mergedResult = currentNode;
                isFirstFile  = false;
            } else {
                /* For subsequent files, merge them */
                mergedResult = mergeNodes(mergedResult, currentNode);
            }

            QSocConsole::debug() << "Successfully loaded and merged YAML file:" << filePath;

        } catch (const YAML::Exception &e) {
            QSocConsole::error() << "failed to parse YAML file:" << filePath << ":" << e.what();
            return {}; /* Return null node on error */
        }
    }

    return mergedResult;
}

YAML::Node QSocYamlUtils::cloneNode(const YAML::Node &original)
{
    try {
        /* Convert to string and parse back to create a deep copy */
        YAML::Emitter emitter;
        emitter << original;
        const std::string yamlString = emitter.c_str();

        return YAML::Load(yamlString);
    } catch (const YAML::Exception &e) {
        QSocConsole::warn() << "failed to clone YAML node:" << e.what();
        return {}; /* Return null node on error */
    }
}

bool QSocYamlUtils::readDocumentVersion(
    const QString &filePath, const QString &rootName, int &version, QString &errorMessage)
{
    try {
        const YAML::Node document = YAML::LoadFile(filePath.toStdString());
        if (!document || !document.IsMap()) {
            errorMessage = QObject::tr("The file is not a YAML document.");
            return false;
        }
        const YAML::Node root = document[rootName.toStdString()];
        if (!root || !root.IsMap()) {
            errorMessage = QObject::tr("The file has no '%1' section.").arg(rootName);
            return false;
        }
        const YAML::Node versionNode = root["-version"];
        if (!versionNode || !versionNode.IsScalar()) {
            errorMessage = QObject::tr("The file carries no version.");
            return false;
        }
        version = versionNode.as<int>();
        return true;
    } catch (const std::exception &error) {
        errorMessage = QString::fromUtf8(error.what());
        return false;
    }
}
