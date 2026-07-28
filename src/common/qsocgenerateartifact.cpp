// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocgenerateartifact.h"
#include "common/qsocpaths.h"

#include <QFile>
#include <QSaveFile>

namespace {

QSocGenerateArtifact::PrimitiveCellResult writeAtomically(
    const QString &path, const QByteArray &bytes)
{
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        return {
            false,
            false,
            path,
            QStringLiteral("Cannot open primitive cell for writing: %1").arg(file.errorString())};
    }

    qsizetype offset = 0;
    while (offset < bytes.size()) {
        const qint64 written = file.write(bytes.constData() + offset, bytes.size() - offset);
        if (written <= 0) {
            const QString error = file.errorString();
            file.cancelWriting();
            return {false, false, path, QStringLiteral("Cannot write primitive cell: %1").arg(error)};
        }
        offset += written;
    }

    if (!file.commit()) {
        return {
            false,
            false,
            path,
            QStringLiteral("Cannot commit primitive cell: %1").arg(file.errorString())};
    }
    return {true, true, path, {}};
}

} // namespace

namespace QSocGenerateArtifact {

PrimitiveCellResult ensurePrimitiveCell(
    const QString &outputDirectory, const PrimitiveCellSpec &spec, bool force)
{
    const auto artifact = QSocPaths::resolveArtifactPath(outputDirectory, spec.leafName);
    if (!artifact.isValid()) {
        return {false, false, {}, artifact.error};
    }

    QFile existing(artifact.path);
    if (existing.exists() && !force) {
        if (!existing.open(QIODevice::ReadOnly)) {
            return {
                false,
                false,
                artifact.path,
                QStringLiteral("Cannot read existing primitive cell: %1")
                    .arg(existing.errorString())};
        }
        return {true, false, artifact.path, {}};
    }
    if (spec.canonicalBytes.isEmpty()) {
        return {
            false,
            false,
            artifact.path,
            QStringLiteral("Canonical primitive cell content is empty.")};
    }

    return writeAtomically(artifact.path, spec.canonicalBytes);
}

} // namespace QSocGenerateArtifact
