// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCWORKSPACEFS_H
#define QSOCWORKSPACEFS_H

#include <QByteArray>
#include <QList>
#include <QString>

#include <cstdint>
#include <functional>

/**
 * @brief The project files of the bound workspace, local or remote.
 * @details Paths are relative to @ref root and may not be absolute or step
 *          out with `..`. Todo, skill and other project stores use this, so
 *          they follow `/ssh` and `/local` without knowing the transport.
 */
class QSocWorkspaceFs
{
public:
    enum class Result : std::uint8_t {
        Ok,     /**< The answer is in the out parameter. */
        Absent, /**< The workspace has no such entry. */
        Failed, /**< No usable answer; the error says why. */
    };

    struct Entry
    {
        QString name;
        bool    regular = false; /**< A regular file, possibly behind a link. */
        qint64  size    = -1;
    };

    virtual ~QSocWorkspaceFs() = default;

    /** @brief Absolute workspace root, empty while nothing is bound. */
    virtual QString root() const = 0;

    virtual Result read(const QString &path, QByteArray *bytes, QString *error)     = 0;
    virtual Result list(const QString &path, QList<Entry> *entries, QString *error) = 0;
    virtual Result stat(const QString &path, Entry *entry, QString *error)          = 0;

    /** @brief Replace a file, creating its parent directories. */
    virtual bool write(const QString &path, const QByteArray &bytes, QString *error) = 0;

    /** @brief Whether @p path is a relative path that stays below the root. */
    static bool isContainedRelative(const QString &path);

    /** @brief `root()/path` for messages and display. */
    QString displayPath(const QString &path) const;
};

/**
 * @brief The local project directory.
 * @details The root comes from @p rootProvider at every call, so a project
 *          switch needs no rewiring.
 */
class QSocLocalWorkspaceFs : public QSocWorkspaceFs
{
public:
    explicit QSocLocalWorkspaceFs(std::function<QString()> rootProvider);

    QString root() const override;
    Result  read(const QString &path, QByteArray *bytes, QString *error) override;
    Result  list(const QString &path, QList<Entry> *entries, QString *error) override;
    Result  stat(const QString &path, Entry *entry, QString *error) override;
    bool    write(const QString &path, const QByteArray &bytes, QString *error) override;

private:
    QString absolute(const QString &path, QString *error) const;

    std::function<QString()> m_rootProvider;
};

#endif // QSOCWORKSPACEFS_H
