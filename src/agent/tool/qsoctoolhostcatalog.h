// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLHOSTCATALOG_H
#define QSOCTOOLHOSTCATALOG_H

#include "agent/qsoctool.h"

class QSocHostCatalog;

/**
 * @brief Register a new named host entry in the catalog.
 * @details Pure local YAML edit; no SSH connection is opened. The new
 *          entry becomes available as a `host` argument on the next
 *          `agent` tool spawn and as an option for `/ssh <alias>`.
 */
class QSocToolHostRegister : public QSocTool
{
    Q_OBJECT
public:
    explicit QSocToolHostRegister(QObject *parent, QSocHostCatalog *catalog);
    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

private:
    QSocHostCatalog *catalog_ = nullptr;
};

/**
 * @brief Apply an ordered list of ops to an existing catalog entry.
 * @details Grows or shrinks the capability text, or amends supplemental
 *          fields. Ops applied atomically: partial failure leaves the
 *          file unchanged.
 */
class QSocToolHostUpdate : public QSocTool
{
    Q_OBJECT
public:
    explicit QSocToolHostUpdate(QObject *parent, QSocHostCatalog *catalog);
    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

private:
    QSocHostCatalog *catalog_ = nullptr;
};

/**
 * @brief Remove a catalog entry. Clears `active` when it was bound to
 *        the removed alias. Cannot remove user-scope entries; those are
 *        edited directly by the user.
 */
class QSocToolHostRemove : public QSocTool
{
    Q_OBJECT
public:
    explicit QSocToolHostRemove(QObject *parent, QSocHostCatalog *catalog);
    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

private:
    QSocHostCatalog *catalog_ = nullptr;
};

#endif // QSOCTOOLHOSTCATALOG_H
