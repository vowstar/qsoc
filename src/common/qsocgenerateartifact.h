// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCGENERATEARTIFACT_H
#define QSOCGENERATEARTIFACT_H

#include <vector>
#include <QByteArray>
#include <QString>

namespace QSocGenerateArtifact {

struct Artifact
{
    QString    path;
    QByteArray contents;
};

/**
 * @brief Write artifacts atomically.
 * @details A .fl line naming another artifact becomes its path relative to
 *          outputDirectory. A .sby [files] line becomes relative to the .sby.
 */
QString write(std::vector<Artifact> artifacts, bool force, const QString &outputDirectory);

} // namespace QSocGenerateArtifact

#endif // QSOCGENERATEARTIFACT_H
