// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCLICENSE_H
#define QSOCLICENSE_H

#include <QList>
#include <QString>
#include <QStringList>

/**
 * @brief Third-party components distributed with QSoC and their licenses.
 * @details License texts live under the ":/license/" resource prefix.
 */
namespace QSocLicense {

struct Component
{
    QString     name;    /* Short name, used as the CLI argument */
    QString     license; /* SPDX expression or plain description */
    QString     url;     /* Upstream home page */
    QStringList files;   /* Text files under :/license/, shown in order */
    QString     note;    /* Optional statement shown before the texts */
};

/**
 * @brief All components, sorted by name.
 */
const QList<Component> &components();

/**
 * @brief Find a component by name.
 * @return The component, or nullptr when no component has that name.
 */
const Component *find(const QString &name);

/**
 * @brief Full license text of a component: the note, then every file.
 */
QString text(const Component &component);

} // namespace QSocLicense

#endif // QSOCLICENSE_H
