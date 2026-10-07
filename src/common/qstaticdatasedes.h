// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#ifndef QSTATICDATASEDES_H
#define QSTATICDATASEDES_H

#include <QString>

#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

using json = nlohmann::json;

/* Conversion between YAML, JSON and strings. */
class QStaticDataSedes
{
public:
    /**
     * @brief Serialize YAML Node to QString.
     * @details This function serializes a YAML::Node to a QString.
     * @param node The YAML::Node to serialize.
     * @return Serialized QString representation of the YAML::Node.
     */
    static QString serializeYaml(const YAML::Node &node);

    /**
     * @brief Deserialize QString to YAML Node.
     * @details This function deserializes a QString to a YAML::Node.
     * @param str The QString to deserialize.
     * @return Deserialized YAML::Node.
     */
    static YAML::Node deserializeYaml(const QString &str);

    /**
     * @brief Serialize JSON to QString.
     * @details This function serializes a JSON object to a QString.
     * @param jsonObject The JSON object to serialize.
     * @return Serialized QString representation of the JSON object.
     */
    static QString serializeJson(const json &jsonObject);

    /**
     * @brief Deserialize QString to JSON.
     * @details This function deserializes a QString to a JSON object.
     * @param str The QString to deserialize.
     * @return Deserialized JSON object.
     */
    static json deserializeJson(const QString &str);

private:
    QStaticDataSedes() = delete;
};

#endif // QSTATICDATASEDES_H
