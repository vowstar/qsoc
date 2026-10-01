// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsoccelllibrary.h"
#include "common/qsocgenerateartifact.h"
#include "common/qsocgenerateprimitiveclock.h"
#include "common/qsocgenerateprimitivepower.h"
#include "common/qsocgenerateprimitivereset.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("main", text);
}

QString writeCells(const QString &outputDirectory)
{
    const QDir rtl(QDir(outputDirectory).filePath(QSocCellLibrary::unit() + "/rtl"));
    std::vector<QSocGenerateArtifact::Artifact> artifacts;
    QByteArray                                  list;
    for (const QSocCellLibrary::Cell &cell : QSocCellLibrary::cells()) {
        artifacts.push_back({rtl.filePath(cell.file), cell.text.toUtf8()});
        list += QSocCellLibrary::path(cell.file).toUtf8() + '\n';
    }
    artifacts.push_back({rtl.filePath(QSocCellLibrary::unit() + ".fl"), list});
    return QSocGenerateArtifact::write(std::move(artifacts), true, outputDirectory);
}

/* Unit lists relative to output: qsoc_cell, then <u> and <lib>/<module> in name order. */
QStringList unitLists(const QDir &output)
{
    const auto hasList = [&output](const QString &unit) {
        return QFileInfo(output.filePath(unit + "/rtl/" + QFileInfo(unit).fileName() + ".fl"))
            .isFile();
    };
    QStringList units;
    for (const QString &name : output.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (hasList(name)) {
            units.append(name);
            continue;
        }
        const QDir library(output.filePath(name));
        for (const QString &module :
             library.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            if (hasList(name + "/" + module))
                units.append(name + "/" + module);
        }
    }
    units.sort();
    units.removeAll(QSocCellLibrary::unit());
    if (hasList(QSocCellLibrary::unit()))
        units.prepend(QSocCellLibrary::unit());
    QStringList lists;
    for (const QString &unit : units)
        lists.append(unit + "/rtl/" + QFileInfo(unit).fileName() + ".fl");
    return lists;
}

/* A plain relative file path that stays inside output. */
bool plainEntry(const QString &entry)
{
    const QString clean = QDir::cleanPath(entry);
    return !entry.isEmpty() && !entry.startsWith('+') && !entry.startsWith('-')
           && !entry.contains('\r') && QDir::isRelativePath(entry) && clean != ".."
           && !clean.startsWith("../");
}

class TopList
{
public:
    explicit TopList(const QDir &output)
        : output(output)
    {}

    QString add(const QString &list)
    {
        QFile file(output.filePath(list));
        if (!file.open(QIODevice::ReadOnly))
            return tr("Error: cannot read file list: %1").arg(list);
        QStringList lines = QString::fromUtf8(file.readAll()).split('\n');
        if (lines.last().isEmpty())
            lines.removeLast();
        for (const QString &entry : lines) {
            const QString error = addEntry(list, entry);
            if (!error.isEmpty())
                return error;
        }
        return {};
    }

    QByteArray text() const
    {
        return entries.join('\n').toUtf8() + (entries.isEmpty() ? "" : "\n");
    }

private:
    QString addEntry(const QString &list, const QString &entry)
    {
        if (!plainEntry(entry))
            return tr("Error: %1 lists an entry that is not a plain relative file: %2")
                .arg(list, entry);
        QFile file(output.filePath(entry));
        if (!file.open(QIODevice::ReadOnly))
            return tr("Error: %1 lists a missing file: %2").arg(list, entry);
        static const QRegularExpression
            module("^\\s*module\\s+([A-Za-z_][A-Za-z_0-9$]*)", QRegularExpression::MultilineOption);
        auto match = module.globalMatch(QString::fromUtf8(file.readAll()));
        while (match.hasNext()) {
            const QString name = match.next().captured(1);
            if (owner.contains(name))
                return tr("Error: module %1 is defined in both %2 and %3")
                    .arg(name, owner.value(name), entry);
            owner.insert(name, entry);
        }
        entries.append(entry);
        return {};
    }

    const QDir             output;
    QStringList            entries;
    QMap<QString, QString> owner;
};

} // namespace

QList<QSocCellLibrary::Cell> QSocCellLibrary::cells()
{
    return {
        {clockFile(), QSocClockPrimitive().generateCellVerilog()},
        {resetFile(), QSocResetPrimitive().generateCellVerilog()},
        {powerFile(), QSocPowerPrimitive().generateCellVerilog()},
    };
}

QString QSocCellLibrary::path(const QString &file)
{
    return unit() + "/rtl/" + file;
}

bool QSocCellLibrary::isReserved(const QString &name)
{
    return name.compare(QStringLiteral("qsoc"), Qt::CaseInsensitive) == 0
           || name.startsWith(QStringLiteral("qsoc_"), Qt::CaseInsensitive);
}

QString QSocCellLibrary::writeFileList(const QString &outputDirectory)
{
    const QDir output(outputDirectory);
    TopList    top(output);
    for (const QString &list : unitLists(output)) {
        const QString error = top.add(list);
        if (!error.isEmpty())
            return error;
    }
    const QByteArray text = top.text();
    QSaveFile        file(output.filePath("qsoc.fl"));
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(text) != text.size() || !file.commit())
        return tr("Error: could not write file list: %1").arg(file.fileName());
    return {};
}

QString QSocCellLibrary::publish(const QString &outputDirectory)
{
    const QString error = writeCells(outputDirectory);
    return error.isEmpty() ? writeFileList(outputDirectory) : error;
}
