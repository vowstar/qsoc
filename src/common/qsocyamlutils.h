// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOCYAMLUTILS_H
#define QSOCYAMLUTILS_H

#include <QDebug>
#include <QString>
#include <QStringList>
#include <QtCore>

#include <yaml-cpp/yaml.h>

/**
 * @brief The QSocYamlUtils class.
 * @details This class provides static YAML manipulation utilities for common
 *          YAML operations like merging, validation, and conversion.
 *          It is not meant to be instantiated but used directly through its
 *          static methods.
 */
class QSocYamlUtils
{
public:
    /**
     * @brief Get the static instance of this object.
     * @details This function returns the static instance of this object. It is
     *          used to provide a singleton instance of the class, ensuring that
     *          only one instance of the class exists throughout the
 *          application.
     * @return The static instance of QSocYamlUtils.
     */
    static QSocYamlUtils &instance()
    {
        static QSocYamlUtils instance;
        return instance;
    }

    /**
     * @brief Merge two YAML nodes recursively.
     * @details Merges fromYaml into toYaml, with fromYaml taking precedence.
     *          For maps, recursively merges the contents. For other types,
     *          fromYaml completely replaces toYaml.
     * @param toYaml The base YAML node (lower precedence).
     * @param fromYaml The YAML node to merge from (higher precedence).
     * @return The merged YAML node.
     */
    static YAML::Node mergeNodes(const YAML::Node &toYaml, const YAML::Node &fromYaml);

    /**
     * @brief Load and merge multiple YAML files.
     * @details Loads multiple YAML files in order and merges them into a single node.
     * @param filePathList List of file paths to load and merge.
     * @param baseNode Optional base node to start merging from.
     * @return The merged YAML node, or null node on error.
     */
    static YAML::Node loadAndMergeFiles(
        const QStringList &filePathList, const YAML::Node &baseNode = YAML::Node());

    /**
     * @brief Validate basic YAML structure for netlist files.
     * @details Checks if the YAML node has the required structure for a valid netlist.
     * @param yamlNode The YAML node to validate.
     * @param errorMessage Output parameter for error message.
     * @return true if valid, false otherwise.
     */
    static bool validateNetlistStructure(const YAML::Node &yamlNode, QString &errorMessage);

    /**
     * @brief Convert YAML node to string representation.
     * @details Converts a YAML node to its string representation for debugging or output.
     * @param yamlNode The YAML node to convert.
     * @param indentSize The indentation size for formatting.
     * @return The string representation of the YAML node.
     */
    static QString yamlNodeToString(const YAML::Node &yamlNode, int indentSize = 2);

    /**
     * @brief Clone a YAML node deeply.
     * @details Creates a deep copy of a YAML node to avoid reference issues.
     * @param original The original YAML node to clone.
     * @return A deep copy of the original node.
     */
    static YAML::Node cloneNode(const YAML::Node &original);

    /**
     * @brief Check if a YAML node contains a specific key path.
     * @details Checks if a nested key path exists in the YAML node.
     * @param yamlNode The YAML node to check.
     * @param keyPath The key path (e.g., "section.subsection.key").
     * @return true if the key path exists, false otherwise.
     */
    static bool hasKeyPath(const YAML::Node &yamlNode, const QString &keyPath);

    /**
     * @brief Get a value from a YAML node using a key path.
     * @details Retrieves a value from a nested key path in the YAML node.
     * @param yamlNode The YAML node to search.
     * @param keyPath The key path (e.g., "section.subsection.key").
     * @param defaultValue The default value if key path is not found.
     * @return The value as a string, or defaultValue if not found.
     */
    static QString getValueByKeyPath(
        const YAML::Node &yamlNode, const QString &keyPath, const QString &defaultValue = "");

    /**
     * @brief Set a value in a YAML node using a key path.
     * @details Sets a value at a nested key path in the YAML node, creating
     *          intermediate nodes if necessary.
     * @param yamlNode The YAML node to modify.
     * @param keyPath The key path (e.g., "section.subsection.key").
     * @param value The value to set.
     * @return true if successful, false otherwise.
     */
    static bool setValueByKeyPath(YAML::Node &yamlNode, const QString &keyPath, const QString &value);

private:
    /**
     * @brief Constructor.
     * @details This is a private constructor for QSocYamlUtils to prevent
     *          instantiation. Making the constructor private ensures that no
     *          objects of this class can be created from outside the class,
     *          enforcing a static-only usage pattern.
     */
    QSocYamlUtils() = default;

    /**
     * @brief Copy constructor (deleted).
     * @details This copy constructor is deleted to prevent copying of the
     *          singleton instance.
     */
    QSocYamlUtils(const QSocYamlUtils &) = delete;

    /**
     * @brief Assignment operator (deleted).
     * @details This assignment operator is deleted to prevent assignment of
     *          the singleton instance.
     */
    QSocYamlUtils &operator=(const QSocYamlUtils &) = delete;
};

#endif // QSOCYAMLUTILS_H
