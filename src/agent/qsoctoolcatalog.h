// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLCATALOG_H
#define QSOCTOOLCATALOG_H

#include "agent/qsoctool.h"

struct QSocToolDispatchView
{
    QString            wireCallId;
    QString            wireName;
    QString            canonicalName;
    json               finalArguments;
    QString            schemaVersion;
    QPointer<QObject>  owner;
    QPointer<QSocTool> tool;
};

class QSocToolCatalog
{
public:
    static constexpr qsizetype argumentBytes      = 1024 * 1024;
    static constexpr int       argumentDepth      = 64;
    static constexpr qint64    automaticThreshold = 8192;

    QSocToolCatalog(json allowedDefinitions, const QString &binding);
    QString version() const { return version_; }
    QString resolvedMode(const QString &policy) const;
    json    wireDefinitions(const QString &policy) const;
    QString query(const json &arguments) const;
    QString unwrap(const json &arguments, QSocToolDispatchView *view, qsizetype outerBytes) const;
    bool    contains(const QString &name) const;
    static bool    alwaysDirect(const QString &name);
    static bool    reserved(const QString &name);
    static QString parseArguments(const QString &encoded, json *arguments, qsizetype consumed = 0);
    static QString validateArguments(const json &arguments, qsizetype consumed = 0);

private:
    json                definitions_;
    QMap<QString, json> byName_;
    QString             version_;
};

#endif
