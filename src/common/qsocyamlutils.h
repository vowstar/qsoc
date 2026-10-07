// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOCYAMLUTILS_H
#define QSOCYAMLUTILS_H

#include <QDebug>
#include <QString>
#include <QStringList>
#include <QtCore>

#include <yaml-cpp/yaml.h>

/* Static YAML merge and document utilities. */
class QSocYamlUtils
{
public:
    /* Merge library maps recursively, replacing sequences and retaining values for null overrides. */
    static YAML::Node mergeLibraryNodes(const YAML::Node &toYaml, const YAML::Node &fromYaml);

    /**
     * @brief Merge two YAML nodes recursively.
     * @details Merges fromYaml into toYaml, with fromYaml taking precedence.
     *          Maps merge recursively, sequences concatenate, and null overrides
     *          retain the original value. Other values replace the original.
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
     * @brief Clone a YAML node deeply.
     * @details Creates a deep copy of a YAML node to avoid reference issues.
     * @param original The original YAML node to clone.
     * @return A deep copy of the original node.
     */
    static YAML::Node cloneNode(const YAML::Node &original);

    /**
     * @brief Read the serialization version of a gpds YAML document.
     * @details Reads the `-version` attribute the gpds archiver writes under
     *          the document root. Used to reject a file before its contents
     *          replace the editor's current document.
     * @param filePath Path of the file to inspect.
     * @param rootName Document root key, e.g. "qschematic".
     * @param version Receives the version when the call succeeds.
     * @param errorMessage Receives the reason when the call fails.
     * @return true when a version was read.
     */
    static bool readDocumentVersion(
        const QString &filePath, const QString &rootName, int &version, QString &errorMessage);

private:
    QSocYamlUtils() = delete;
};

#endif // QSOCYAMLUTILS_H
