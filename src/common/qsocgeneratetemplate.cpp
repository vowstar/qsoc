// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "common/qsocgeneratemanager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

#include <fstream>
#include <inja/inja.hpp>
#include <nlohmann/json.hpp>
#include <rapidcsv.h>
#include <sstream>

bool QSocGenerateManager::renderTemplate(
    const QString     &templateFilePath,
    const QStringList &csvFiles,
    const QStringList &yamlFiles,
    const QStringList &jsonFiles,
    const QString     &outputFileName)
{
    using json      = nlohmann::json;
    json dataObject = json::object();

    /* Create a global data array for all CSV data */
    json globalDataArray = json::array();

    /* Process CSV files */
    for (const QString &csvFilePath : csvFiles) {
        if (!QFile::exists(csvFilePath)) {
            qCritical() << QCoreApplication::translate(
                               "generate", "Error: CSV file does not exist: \"%1\"")
                               .arg(csvFilePath);
            return false;
        }

        try {
            /* First determine the CSV delimiter by reading the first line */
            QFile file(csvFilePath);
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                qCritical() << QCoreApplication::translate(
                                   "generate", "Error: Could not open CSV file \"%1\"")
                                   .arg(csvFilePath);
                return false;
            }

            QTextStream in(&file);
            QString     firstLine = in.readLine();
            file.close();

            /* Determine delimiter based on count of commas vs semicolons */
            QChar delimiter = firstLine.count(',') >= firstLine.count(';') ? ',' : ';';

            /* Parse CSV with the detected delimiter */
            rapidcsv::SeparatorParams separatorParams(static_cast<char>(delimiter.unicode()));
            rapidcsv::Document
                doc(csvFilePath.toStdString(), rapidcsv::LabelParams(0, -1), separatorParams);

            QFileInfo fileInfo(csvFilePath);
            QString   baseName = fileInfo.baseName();

            /* Create array for this CSV file */
            json csvArray = json::array();

            /* Get column names */
            std::vector<std::string> columnNames = doc.GetColumnNames();

            /* Process rows */
            size_t rowCount = doc.GetRowCount();
            for (size_t i = 0; i < rowCount; i++) {
                json row = json::object();
                for (const auto &colName : columnNames) {
                    try {
                        /* Try to get as string first (default) */
                        std::string cellValue = doc.GetCell<std::string>(colName, i);

                        /* Try to parse as number if possible */
                        try {
                            /* Check if it's an integer */
                            size_t pos      = 0;
                            int    intValue = std::stoi(cellValue, &pos);
                            if (pos == cellValue.length()) {
                                row[colName] = intValue;
                                continue;
                            }

                            /* Check if it's a float */
                            pos               = 0;
                            double floatValue = std::stod(cellValue, &pos);
                            if (pos == cellValue.length()) {
                                row[colName] = floatValue;
                                continue;
                            }
                        } catch (...) {
                            /* Not a number, keep as string */
                        }

                        /* Store as string if not a number */
                        row[colName] = cellValue;
                    } catch (const std::exception &e) {
                        qWarning() << QCoreApplication::translate(
                                          "generate",
                                          "Warning: Could not read cell value in CSV file "
                                          "\"%1\" at row %2, column \"%3\": %4")
                                          .arg(csvFilePath)
                                          .arg(i + 1)
                                          .arg(QString::fromStdString(colName))
                                          .arg(e.what());
                        row[colName] = "";
                    }
                }
                csvArray.push_back(row);
                globalDataArray.push_back(row);
            }

            /* Add to data object using file basename as key */
            dataObject[baseName.toStdString()] = csvArray;

        } catch (const std::exception &e) {
            qCritical() << QCoreApplication::translate(
                               "generate", "Error: Failed to parse CSV file \"%1\": %2")
                               .arg(csvFilePath)
                               .arg(e.what());
            return false;
        }
    }

    /* Add global data array */
    dataObject["data"] = globalDataArray;

    /* Process YAML files */
    for (const QString &yamlFilePath : yamlFiles) {
        if (!QFile::exists(yamlFilePath)) {
            qCritical() << QCoreApplication::translate(
                               "generate", "Error: YAML file does not exist: \"%1\"")
                               .arg(yamlFilePath);
            return false;
        }

        try {
            YAML::Node yamlNode = YAML::LoadFile(yamlFilePath.toStdString());

            /* Convert YAML to JSON */
            json                                            yamlJson;
            std::function<void(const YAML::Node &, json &)> convertYamlToJson;

            convertYamlToJson = [&convertYamlToJson](const YAML::Node &node, json &j) {
                switch (node.Type()) {
                case YAML::NodeType::Null:
                    j = nullptr;
                    break;
                case YAML::NodeType::Scalar:
                    /* Try to parse as various types */
                    try {
                        j = node.as<int>();
                    } catch (...) {
                        try {
                            j = node.as<double>();
                        } catch (...) {
                            try {
                                j = node.as<bool>();
                            } catch (...) {
                                j = node.as<std::string>();
                            }
                        }
                    }
                    break;
                case YAML::NodeType::Sequence:
                    j = json::array();
                    for (size_t i = 0; i < node.size(); i++) {
                        json element;
                        convertYamlToJson(node[i], element);
                        j.push_back(element);
                    }
                    break;
                case YAML::NodeType::Map:
                    j = json::object();
                    for (const auto &it : node) {
                        json value;
                        convertYamlToJson(it.second, value);
                        j[it.first.as<std::string>()] = value;
                    }
                    break;
                default:
                    break;
                }
            };

            convertYamlToJson(yamlNode, yamlJson);

            /* Merge with existing data (deep merge) */
            std::function<void(json &, const json &)> mergeJson;

            mergeJson = [&mergeJson](json &target, const json &source) {
                if (source.is_object()) {
                    for (auto it = source.begin(); it != source.end(); ++it) {
                        if (it.value().is_object() && target.contains(it.key())
                            && target[it.key()].is_object()) {
                            /* Recursive merge for nested objects */
                            mergeJson(target[it.key()], it.value());
                        } else {
                            /* Replace or add value */
                            target[it.key()] = it.value();
                        }
                    }
                }
            };

            mergeJson(dataObject, yamlJson);

        } catch (const std::exception &e) {
            qCritical() << QCoreApplication::translate(
                               "generate", "Error: Failed to parse YAML file \"%1\": %2")
                               .arg(yamlFilePath)
                               .arg(e.what());
            return false;
        }
    }

    /* Process JSON files */
    for (const QString &jsonFilePath : jsonFiles) {
        if (!QFile::exists(jsonFilePath)) {
            qCritical() << QCoreApplication::translate(
                               "generate", "Error: JSON file does not exist: \"%1\"")
                               .arg(jsonFilePath);
            return false;
        }

        try {
            /* Read JSON file */
            QFile jsonFile(jsonFilePath);
            if (!jsonFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                qCritical() << QCoreApplication::translate(
                                   "generate", "Error: Could not open JSON file \"%1\"")
                                   .arg(jsonFilePath);
                return false;
            }

            QByteArray jsonData = jsonFile.readAll();
            jsonFile.close();

            /* Parse JSON */
            QJsonParseError parseError;
            QJsonDocument   jsonDoc = QJsonDocument::fromJson(jsonData, &parseError);
            if (parseError.error != QJsonParseError::NoError) {
                qCritical() << QCoreApplication::translate(
                                   "generate", "Error: Failed to parse JSON file \"%1\": %2")
                                   .arg(jsonFilePath)
                                   .arg(parseError.errorString());
                return false;
            }

            /* Convert to nlohmann::json */
            json jsonObj = json::parse(jsonDoc.toJson().toStdString());

            /* Merge with existing data (deep merge) */
            std::function<void(json &, const json &)> mergeJson;

            mergeJson = [&mergeJson](json &target, const json &source) {
                if (source.is_object()) {
                    for (auto it = source.begin(); it != source.end(); ++it) {
                        if (it.value().is_object() && target.contains(it.key())
                            && target[it.key()].is_object()) {
                            /* Recursive merge for nested objects */
                            mergeJson(target[it.key()], it.value());
                        } else {
                            /* Replace or add value */
                            target[it.key()] = it.value();
                        }
                    }
                }
            };

            mergeJson(dataObject, jsonObj);

        } catch (const std::exception &e) {
            qCritical() << QCoreApplication::translate(
                               "generate", "Error: Failed to process JSON file \"%1\": %2")
                               .arg(jsonFilePath)
                               .arg(e.what());
            return false;
        }
    }

    /* Process template file */
    try {
        /* Read template file */
        QFile templateFile(templateFilePath);
        if (!templateFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qCritical() << QCoreApplication::translate(
                               "generate", "Error: Could not open template file \"%1\"")
                               .arg(templateFilePath);
            return false;
        }

        QByteArray templateData = templateFile.readAll();
        templateFile.close();

        /* Setup inja environment */
        inja::Environment env;

        /* Render template */
        std::string templateStr = templateData.toStdString();
        std::string result      = env.render(templateStr, dataObject);

        /* Create output file */
        QString outputPath = projectManager->getOutputPath() + QDir::separator() + outputFileName;
        QFile   outputFile(outputPath);
        if (!outputFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            qCritical() << QCoreApplication::translate(
                               "generate", "Error: Could not create output file \"%1\"")
                               .arg(outputPath);
            return false;
        }

        QTextStream stream(&outputFile);
        stream << QString::fromStdString(result);
        outputFile.close();

        return true;

    } catch (const std::exception &e) {
        qCritical() << QCoreApplication::translate(
                           "generate", "Error: Failed to render template \"%1\": %2")
                           .arg(templateFilePath)
                           .arg(e.what());
        return false;
    }
}
