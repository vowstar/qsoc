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

#include <systemrdl_api.h>

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
    const QStringList &rdlFiles,
    const QStringList &rcsvFiles,
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

    /* Process SystemRDL files */
    for (const QString &rdlFilePath : rdlFiles) {
        if (!QFile::exists(rdlFilePath)) {
            qCritical() << QCoreApplication::translate(
                               "generate", "Error: SystemRDL file does not exist: \"%1\"")
                               .arg(rdlFilePath);
            return false;
        }

        try {
            /* Use SystemRDL library to elaborate the RDL file to simplified JSON */
            const systemrdl::Result result = systemrdl::file::elaborate_simplified(
                rdlFilePath.toStdString());

            if (!result.ok()) {
                qCritical() << QCoreApplication::translate(
                                   "generate",
                                   "Error: Failed to elaborate SystemRDL file \"%1\": %2")
                                   .arg(rdlFilePath)
                                   .arg(QString::fromStdString(result.error()));
                return false;
            }

            /* Parse the elaborated JSON result */
            const json rdlJson = json::parse(result.value());

            /* Get file basename for keying */
            const QFileInfo fileInfo(rdlFilePath);
            const QString   baseName = fileInfo.baseName();

            /* Add RDL data to data object using file basename as key */
            dataObject[baseName.toStdString()] = rdlJson;

        } catch (const std::exception &e) {
            qCritical() << QCoreApplication::translate(
                               "generate", "Error: Failed to process SystemRDL file \"%1\": %2")
                               .arg(rdlFilePath)
                               .arg(e.what());
            return false;
        }
    }

    /* Process RCSV files */
    for (const QString &rcsvFilePath : rcsvFiles) {
        if (!QFile::exists(rcsvFilePath)) {
            qCritical() << QCoreApplication::translate(
                               "generate", "Error: RCSV file does not exist: \"%1\"")
                               .arg(rcsvFilePath);
            return false;
        }

        try {
            /* Use SystemRDL library to convert RCSV file to SystemRDL */
            const systemrdl::Result csvToRdlResult = systemrdl::file::csv_to_rdl(
                rcsvFilePath.toStdString());

            if (!csvToRdlResult.ok()) {
                qCritical() << QCoreApplication::translate(
                                   "generate", "Error: Failed to convert RCSV file \"%1\": %2")
                                   .arg(rcsvFilePath)
                                   .arg(QString::fromStdString(csvToRdlResult.error()));
                return false;
            }

            /* Use SystemRDL library to elaborate the converted SystemRDL to simplified JSON */
            const systemrdl::Result result = systemrdl::elaborate_simplified(csvToRdlResult.value());

            if (!result.ok()) {
                qCritical() << QCoreApplication::translate(
                                   "generate", "Error: Failed to elaborate RCSV file \"%1\": %2")
                                   .arg(rcsvFilePath)
                                   .arg(QString::fromStdString(result.error()));
                return false;
            }

            /* Parse the elaborated JSON result */
            const json rcsvJson = json::parse(result.value());

            /* Get file basename for keying */
            const QFileInfo fileInfo(rcsvFilePath);
            const QString   baseName = fileInfo.baseName();

            /* Add RCSV data to data object using file basename as key */
            dataObject[baseName.toStdString()] = rcsvJson;

        } catch (const std::exception &e) {
            qCritical() << QCoreApplication::translate(
                               "generate", "Error: Failed to process RCSV file \"%1\": %2")
                               .arg(rcsvFilePath)
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

        /* Disable line statements */
        env.set_line_statement("");

        /* Add regex_search filter */
        env.add_callback("regex_search", [](inja::Arguments &args) -> nlohmann::json {
            if (args.size() < 2) {
                qWarning() << QCoreApplication::translate(
                    "generate",
                    "Warning: regex_search requires at least 2 arguments (input, pattern)");
                return nlohmann::json::array();
            }

            try {
                /* Extract required arguments */
                const std::string inputStr = args.at(0)->get<std::string>();
                const std::string pattern  = args.at(1)->get<std::string>();

                /* Extract optional arguments */
                bool multiline  = false;
                bool ignorecase = false;

                if (args.size() > 2) {
                    const auto &multilineArg = *args.at(2);
                    if (multilineArg.is_boolean()) {
                        multiline = multilineArg.get<bool>();
                    } else if (multilineArg.is_string()) {
                        const std::string multilineStr = multilineArg.get<std::string>();
                        multiline = (multilineStr == "true" || multilineStr == "True");
                    }
                }

                if (args.size() > 3) {
                    const auto &ignorecaseArg = *args.at(3);
                    if (ignorecaseArg.is_boolean()) {
                        ignorecase = ignorecaseArg.get<bool>();
                    } else if (ignorecaseArg.is_string()) {
                        const std::string ignorecaseStr = ignorecaseArg.get<std::string>();
                        ignorecase = (ignorecaseStr == "true" || ignorecaseStr == "True");
                    }
                }

                /* Create QRegularExpression with appropriate options */
                QRegularExpression::PatternOptions options = QRegularExpression::NoPatternOption;
                if (ignorecase) {
                    options |= QRegularExpression::CaseInsensitiveOption;
                }
                if (multiline) {
                    options |= QRegularExpression::MultilineOption;
                }

                const QRegularExpression regex(QString::fromStdString(pattern), options);

                if (!regex.isValid()) {
                    qWarning() << QCoreApplication::translate(
                                      "generate",
                                      "Warning: Invalid regex pattern in regex_search: \"%1\"")
                                      .arg(QString::fromStdString(pattern));
                    return nlohmann::json::array();
                }

                const QString  inputQStr   = QString::fromStdString(inputStr);
                nlohmann::json resultArray = nlohmann::json::array();

                /* Find all matches */
                QRegularExpressionMatchIterator iterator = regex.globalMatch(inputQStr);

                while (iterator.hasNext()) {
                    const QRegularExpressionMatch match = iterator.next();

                    /* Simple logic: if there are capture groups, return the first capture group
                       Otherwise return the complete match */
                    QString capturedText;
                    if (match.lastCapturedIndex() > 0) {
                        capturedText = match.captured(1);
                    } else {
                        capturedText = match.captured(0);
                    }

                    resultArray.push_back(capturedText.toStdString());
                }

                return resultArray;

            } catch (const std::exception &e) {
                qWarning() << QCoreApplication::translate(
                                  "generate", "Warning: Error in regex_search: %1")
                                  .arg(e.what());
                return nlohmann::json::array();
            }
        });

        /* Add regex_replace filter */
        env.add_callback("regex_replace", [](inja::Arguments &args) -> nlohmann::json {
            if (args.size() < 3) {
                qWarning() << QCoreApplication::translate(
                    "generate",
                    "Warning: regex_replace requires at least 3 arguments (input, pattern, "
                    "replacement)");
                return nlohmann::json{""};
            }

            try {
                /* Extract required arguments */
                const std::string inputStr    = args.at(0)->get<std::string>();
                const std::string pattern     = args.at(1)->get<std::string>();
                const std::string replacement = args.at(2)->get<std::string>();

                /* Extract optional ignorecase argument */
                bool ignorecase = false;

                if (args.size() > 3) {
                    const auto &ignorecaseArg = *args.at(3);
                    if (ignorecaseArg.is_boolean()) {
                        ignorecase = ignorecaseArg.get<bool>();
                    } else if (ignorecaseArg.is_string()) {
                        const std::string ignorecaseStr = ignorecaseArg.get<std::string>();
                        ignorecase = (ignorecaseStr == "true" || ignorecaseStr == "True");
                    }
                }

                /* Create QRegularExpression with appropriate options */
                QRegularExpression::PatternOptions options = QRegularExpression::NoPatternOption;
                if (ignorecase) {
                    options |= QRegularExpression::CaseInsensitiveOption;
                }

                const QRegularExpression regex(QString::fromStdString(pattern), options);

                if (!regex.isValid()) {
                    qWarning() << QCoreApplication::translate(
                                      "generate",
                                      "Warning: Invalid regex pattern in regex_replace: \"%1\"")
                                      .arg(QString::fromStdString(pattern));
                    return nlohmann::json{inputStr};
                }

                QString       inputQStr       = QString::fromStdString(inputStr);
                const QString replacementQStr = QString::fromStdString(replacement);

                /* Perform replacement - Qt uses \\1, \\2 for backreferences */
                const QString result = inputQStr.replace(regex, replacementQStr);

                return nlohmann::json{result.toStdString()};

            } catch (const std::exception &e) {
                qWarning() << QCoreApplication::translate(
                                  "generate", "Warning: Error in regex_replace: %1")
                                  .arg(e.what());
                return nlohmann::json{""};
            }
        });

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

        /* Generate corresponding JSON data file for debugging/third-party tools */
        const QFileInfo outputFileInfo(outputFileName);
        const QString   jsonFileName = outputFileInfo.baseName() + ".json";
        const QString jsonPath = projectManager->getOutputPath() + QDir::separator() + jsonFileName;
        QFile         jsonFile(jsonPath);
        if (!jsonFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            qWarning() << QCoreApplication::translate(
                              "generate", "Warning: Could not create JSON data file \"%1\"")
                              .arg(jsonPath);
        } else {
            try {
                /* Serialize dataObject to formatted JSON */
                const std::string formattedJson = dataObject.dump(4); /* 4 spaces indentation */

                QTextStream stream(&jsonFile);
                stream.setEncoding(QStringConverter::Utf8);
                stream << QString::fromStdString(formattedJson);
                jsonFile.close();
            } catch (const std::exception &e) {
                qWarning() << QCoreApplication::translate(
                                  "generate", "Warning: Failed to create JSON data file: %1")
                                  .arg(e.what());
            }
        }

        return true;

    } catch (const std::exception &e) {
        qCritical() << QCoreApplication::translate(
                           "generate", "Error: Failed to render template \"%1\": %2")
                           .arg(templateFilePath)
                           .arg(e.what());
        return false;
    }
}
