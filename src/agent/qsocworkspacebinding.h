// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCWORKSPACEBINDING_H
#define QSOCWORKSPACEBINDING_H

#include <QString>

/**
 * @brief The workspace a piece of project state belongs to.
 * @details An empty target is this machine. A remote binding is the alias
 *          the user bound by plus the remote workspace root. Goals, loops and
 *          resumed sessions carry one so they act only on the binding that
 *          created them.
 */
struct QSocWorkspaceBinding
{
    QString target;    /**< Alias the binding was made by; empty for local. */
    QString workspace; /**< Remote workspace root; empty for local. */

    bool isLocal() const { return target.isEmpty(); }

    /** @brief `local`, or `alias:workspace`, for notices. */
    QString label() const
    {
        return isLocal() ? QStringLiteral("local") : target + QLatin1Char(':') + workspace;
    }

    bool operator==(const QSocWorkspaceBinding &other) const = default;
};

#endif // QSOCWORKSPACEBINDING_H
