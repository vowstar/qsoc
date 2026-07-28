// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCGENERATEARTIFACT_H
#define QSOCGENERATEARTIFACT_H

#include <QByteArray>
#include <QString>

namespace QSocGenerateArtifact {

struct PrimitiveCellSpec
{
    QString    leafName;
    QByteArray canonicalBytes;
};

struct PrimitiveCellResult
{
    bool    success = false;
    bool    written = false;
    QString path;
    QString error;
};

PrimitiveCellResult ensurePrimitiveCell(
    const QString &outputDirectory, const PrimitiveCellSpec &spec, bool force);

} // namespace QSocGenerateArtifact

#endif // QSOCGENERATEARTIFACT_H
