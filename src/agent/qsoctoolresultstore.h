// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLRESULTSTORE_H
#define QSOCTOOLRESULTSTORE_H

#include <functional>
#include <memory>
#include <optional>
#include <QByteArray>
#include <QList>
#include <QRecursiveMutex>
#include <QString>

class QTemporaryDir;

class QSocToolResultStore
{
public:
    struct Limits
    {
        qint64 artifactBytes = 16 * 1024 * 1024;
        qint64 sessionBytes  = 256 * 1024 * 1024;
        qint64 pageBytes     = 32 * 1024;
    };
    struct Reference
    {
        QString id;
        QString sha256;
        qint64  capturedBytes = 0;
        QString origin;
        QString completion;
        QString sourceCompleteness = QStringLiteral("unknown");
    };
    struct Page
    {
        Reference reference;
        QString   text;
        qint64    offset     = 0;
        qint64    nextOffset = 0;
        bool      eof        = false;
    };
    using Guard = std::function<bool()>;

    QSocToolResultStore(QString directory, QString owner);
    QSocToolResultStore(QString directory, QString owner, Limits limits);
    static std::shared_ptr<QSocToolResultStore> temporary(Limits limits);
    bool    isTemporary() const { return bool(temporaryDirectory_); }
    bool    isBound() const;
    QString directory() const { return directory_; }
    QString owner() const { return owner_; }
    Limits  limits() const { return limits_; }
    qint64  reservedBytes() const;
    qint64  storedBytes(QString *error = nullptr) const;

    std::optional<Reference> publish(
        const QString &text,
        const QString &completion,
        const QString &sourceCompleteness = QStringLiteral("unknown"),
        QString       *error              = nullptr,
        const Guard   &guard              = {});
    std::optional<Page> read(
        const QString &id, qint64 offset, qint64 limit, QString *error = nullptr) const;
    bool discardPublished(const Reference &reference, QString *error = nullptr);
    bool inherit(
        const QSocToolResultStore &source,
        const QList<Reference>    &references,
        QString                   *error = nullptr,
        const Guard               &guard = {});

    static bool   validId(const QString &id);
    static qint64 utf8End(const QByteArray &data, qint64 begin, qint64 limit);

private:
    struct Record
    {
        Reference  reference;
        QByteArray data;
    };
    std::optional<Record> load(const QString &id, QString *error) const;
    QByteArray            encode(const Reference &reference, const QByteArray &data) const;
    bool                  write(
        const Reference &reference, const QByteArray &payload, QString *error, const Guard &guard);
    QString path(const QString &id) const;

    QByteArray                     binding_;
    std::shared_ptr<QTemporaryDir> temporaryDirectory_;
    QString                        directory_;
    QString                        owner_;
    Limits                         limits_;
    mutable QRecursiveMutex        mutex_;
    qint64                         reservedBytes_ = 0;
};

#endif // QSOCTOOLRESULTSTORE_H
