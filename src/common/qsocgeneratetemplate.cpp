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

#include <charconv>
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

            QTextStream   inputStream(&file);
            const QString firstLine = inputStream.readLine();
            file.close();

            /* Determine delimiter based on count of commas vs semicolons */
            QChar delimiter = firstLine.count(',') >= firstLine.count(';') ? ',' : ';';

            /* Parse CSV with the detected delimiter */
            const rapidcsv::SeparatorParams separatorParams(static_cast<char>(delimiter.unicode()));
            const rapidcsv::Document
                doc(csvFilePath.toStdString(), rapidcsv::LabelParams(0, -1), separatorParams);

            const QFileInfo fileInfo(csvFilePath);
            const QString   baseName = fileInfo.baseName();

            /* Create array for this CSV file */
            json csvArray = json::array();

            /* Get column names */
            const std::vector<std::string> columnNames = doc.GetColumnNames();

            /* Process rows */
            const size_t rowCount = doc.GetRowCount();
            for (size_t i = 0; i < rowCount; i++) {
                json row = json::object();
                for (const auto &colName : columnNames) {
                    try {
                        /* Retrieve the cell as a raw string */
                        const auto cellValue = doc.GetCell<std::string>(colName, i);

                        /* Trim leading and trailing whitespace characters */
                        auto trimWhitespace = [](std::string_view input) {
                            auto isSpaceChar = [](unsigned char character) {
                                return std::isspace(character);
                            };
                            std::string_view working = input;
                            while (!working.empty()
                                   && isSpaceChar(static_cast<unsigned char>(working.front())))
                                working.remove_prefix(1);
                            while (!working.empty()
                                   && isSpaceChar(static_cast<unsigned char>(working.back())))
                                working.remove_suffix(1);
                            return working;
                        };

                        const std::string_view trimmedView = trimWhitespace(cellValue);

                        /* Try to parse an integer */
                        int intValue{};
                        auto [intParseEnd, intParseErr] = std::from_chars(
                            trimmedView.data(), trimmedView.data() + trimmedView.size(), intValue);

                        if (intParseErr == std::errc{}
                            && intParseEnd == trimmedView.data() + trimmedView.size()) {
                            row[colName] = intValue;
                            continue;
                        }

                        /* Try to parse a floating-point value (accept both '.' and ',' as decimal separators) */
                        std::string floatBuffer(trimmedView);
                        std::replace(floatBuffer.begin(), floatBuffer.end(), ',', '.');

                        double doubleValue{};
                        auto [doubleParseEnd, doubleParseErr] = std::from_chars(
                            floatBuffer.data(),
                            floatBuffer.data() + floatBuffer.size(),
                            doubleValue);

                        if (doubleParseErr == std::errc{}
                            && doubleParseEnd == floatBuffer.data() + floatBuffer.size()) {
                            row[colName] = doubleValue;
                            continue;
                        }

                        /* Parsing failed: keep the original text */
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
            const YAML::Node yamlNode = YAML::LoadFile(yamlFilePath.toStdString());

            /* Convert YAML to JSON */
            json                                            yamlJson;
            std::function<void(const YAML::Node &, json &)> convertYamlToJson;

            convertYamlToJson = [&convertYamlToJson](const YAML::Node &node, json &jsonObj) {
                switch (node.Type()) {
                case YAML::NodeType::Null:
                    jsonObj = nullptr;
                    break;
                case YAML::NodeType::Scalar:
                    /* Try to parse as various types */
                    try {
                        jsonObj = node.as<int>();
                    } catch (...) {
                        try {
                            jsonObj = node.as<double>();
                        } catch (...) {
                            try {
                                jsonObj = node.as<bool>();
                            } catch (...) {
                                jsonObj = node.as<std::string>();
                            }
                        }
                    }
                    break;
                case YAML::NodeType::Sequence:
                    jsonObj = json::array();
                    for (const auto &element : node) {
                        json elementJson;
                        convertYamlToJson(element, elementJson);
                        jsonObj.push_back(elementJson);
                    }
                    break;
                case YAML::NodeType::Map:
                    jsonObj = json::object();
                    for (const auto &nodeIter : node) {
                        json value;
                        convertYamlToJson(nodeIter.second, value);
                        jsonObj[nodeIter.first.as<std::string>()] = value;
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

            const QByteArray jsonData = jsonFile.readAll();
            jsonFile.close();

            /* Parse JSON */
            QJsonParseError     parseError;
            const QJsonDocument jsonDoc = QJsonDocument::fromJson(jsonData, &parseError);
            if (parseError.error != QJsonParseError::NoError) {
                qCritical() << QCoreApplication::translate(
                                   "generate", "Error: Failed to parse JSON file \"%1\": %2")
                                   .arg(jsonFilePath)
                                   .arg(parseError.errorString());
                return false;
            }

            /* Convert to nlohmann::json */
            const json jsonObj = json::parse(jsonDoc.toJson().toStdString());

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

        const QByteArray templateData = templateFile.readAll();
        templateFile.close();

        /* Setup inja environment */
        inja::Environment env;

        /* Render template */
        const std::string templateStr = templateData.toStdString();
        const std::string result      = env.render(templateStr, dataObject);

        /* Create output file */
        const QString outputPath = projectManager->getOutputPath() + QDir::separator()
                                   + outputFileName;
        QFile outputFile(outputPath);
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
