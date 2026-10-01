// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocgenerateartifact.h"
#include "common/qsocuvmresources.h"

#include <memory>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>

#include <QFile>
#include <QSaveFile>

namespace {

using GeneratedArtifact = QSocGenerateArtifact::Artifact;

QString prepareGeneratedArtifacts(std::vector<GeneratedArtifact> *artifacts, const QDir &output)
{
    QMap<QString, QString> paths;
    for (const auto &artifact : *artifacts) {
        paths.insert(QFileInfo(artifact.path).fileName(), artifact.path);
    }
    std::vector<GeneratedArtifact> dependencies;
    for (auto &artifact : *artifacts) {
        const QFileInfo info(artifact.path);
        const QDir      directory = info.dir();
        const QString   legacy    = QDir::cleanPath(
            directory.filePath(QStringLiteral("../") + info.fileName()));
        if (QFileInfo::exists(legacy) || QFileInfo(legacy).isSymLink()) {
            return QCoreApplication::translate(
                       "main",
                       "Error: legacy output exists: %1. Move the old module output directory "
                       "before generating.")
                .arg(QDir::cleanPath(legacy));
        }
        const bool fileList = artifact.path.endsWith(QStringLiteral(".fl"));
        const bool sby      = artifact.path.endsWith(QStringLiteral(".sby"));
        if (!fileList && !sby) {
            continue;
        }
        QStringList lines = QString::fromUtf8(artifact.contents).split(QLatin1Char('\n'));
        bool        files = fileList;
        for (QString &line : lines) {
            if (sby && line.startsWith(QLatin1Char('['))) {
                files = line == QStringLiteral("[files]");
            }
            if (files && paths.contains(line)) {
                line = (fileList ? output : directory).relativeFilePath(paths.value(line));
            }
        }
        artifact.contents = lines.join(QLatin1Char('\n')).toUtf8();
        if (directory.dirName() != QStringLiteral("uvm")
            || !info.fileName().endsWith(QStringLiteral("_uvm.fl"))) {
            continue;
        }
        const auto sources = QSocUvmResources::sources();
        if (!sources.contains(QStringLiteral("src/uvm_pkg.sv"))) {
            return QCoreApplication::translate("main", "Error: embedded UVM sources are missing.");
        }
        for (auto source = sources.cbegin(); source != sources.cend(); ++source) {
            dependencies.push_back(
                {directory.filePath(QStringLiteral("uvm-core/") + source.key()), source.value()});
        }
        QString standalone = info.fileName();
        standalone.chop(3);
        standalone += QStringLiteral("_standalone.fl");
        const QByteArray core = output.relativeFilePath(directory.filePath("uvm-core/src")).toUtf8();
        dependencies.push_back(
            {directory.filePath(standalone),
             "+incdir+" + core + '\n' + core + "/uvm_pkg.sv\n" + artifact.contents});
    }
    artifacts->insert(artifacts->end(), dependencies.begin(), dependencies.end());
    return {};
}

} // namespace

namespace QSocGenerateArtifact {

QString write(std::vector<GeneratedArtifact> artifacts, bool force, const QString &outputDirectory)
{
    if (artifacts.empty())
        return {};
    const QString preparationError = prepareGeneratedArtifacts(&artifacts, QDir(outputDirectory));
    if (!preparationError.isEmpty()) {
        return preparationError;
    }
    const QString root = QFileInfo(QFileInfo(artifacts.front().path).absolutePath()).absolutePath();
    for (const auto &artifact : artifacts) {
        const QFileInfo target(artifact.path);
        if (target.isSymLink() || (target.exists() && !target.isFile())) {
            return QCoreApplication::translate("main", "Error: output is not a regular file: %1")
                .arg(artifact.path);
        }
        if (!force && target.exists()) {
            return QCoreApplication::translate("main", "Error: output file already exists: %1")
                .arg(artifact.path);
        }
        QString parent = target.absolutePath();
        while (true) {
            const QFileInfo entry(parent);
            if (entry.isSymLink() || (entry.exists() && !entry.isDir())) {
                return QCoreApplication::translate("main", "Error: invalid output directory: %1")
                    .arg(parent);
            }
            if (parent == root) {
                break;
            }
            const QString up = entry.absolutePath();
            if (up == parent) {
                return QCoreApplication::translate("main", "Error: output is outside %1: %2")
                    .arg(root, artifact.path);
            }
            parent = up;
        }
    }
    for (const auto &artifact : artifacts) {
        const QFileInfo target(artifact.path);
        if (!QDir().mkpath(target.absolutePath())) {
            return QCoreApplication::translate("main", "Error: could not create output directory: %1")
                .arg(target.absolutePath());
        }
    }
    QLockFile outputLock(QDir(root).filePath(QStringLiteral(".generate.lock")));
    if (!outputLock.tryLock()) {
        return QCoreApplication::translate("main", "Error: module output is locked: %1").arg(root);
    }
    if (!force) {
        for (const GeneratedArtifact &artifact : artifacts) {
            if (QFile::exists(artifact.path)) {
                return QCoreApplication::translate("main", "Error: output file already exists: %1")
                    .arg(artifact.path);
            }
        }
    }

    std::vector<std::unique_ptr<QSaveFile>> outputFiles;
    outputFiles.reserve(artifacts.size());
    for (const GeneratedArtifact &artifact : artifacts) {
        auto outputFile = std::make_unique<QSaveFile>(artifact.path);
        outputFile->setDirectWriteFallback(false);
        if (!outputFile->open(QIODevice::WriteOnly)) {
            return QCoreApplication::translate("main", "Error: could not open output file: %1")
                .arg(outputFile->errorString());
        }
        if (outputFile->write(artifact.contents) != artifact.contents.size()) {
            const QString error = outputFile->errorString();
            outputFile->cancelWriting();
            return QCoreApplication::translate("main", "Error: could not write output file: %1")
                .arg(error);
        }
        outputFiles.push_back(std::move(outputFile));
    }
    for (const std::unique_ptr<QSaveFile> &outputFile : outputFiles) {
        if (!outputFile->commit()) {
            return QCoreApplication::translate("main", "Error: could not commit output file: %1")
                .arg(outputFile->errorString());
        }
    }
    return QString();
}

} // namespace QSocGenerateArtifact
