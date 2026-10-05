// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLRESULTSTATUS_H
#define QSOCTOOLRESULTSTATUS_H

#include <QMetaType>
#include <QString>

enum class QSocToolResultStatus { Ok, Failed, Uncertain, Dispatched };
Q_DECLARE_METATYPE(QSocToolResultStatus)

namespace QSocToolResult {

/**
 * @brief Classify a tool result by its own declaration.
 * @details A first line @c "status: <name>", a leading @c "Error:", or the
 *          @c status member of a top-level JSON object. Anything else is Ok.
 */
QSocToolResultStatus classify(const QString &result);

/** @brief The stored and wire name: ok, failed, uncertain or dispatched. */
QString name(QSocToolResultStatus status);

} // namespace QSocToolResult

#endif // QSOCTOOLRESULTSTATUS_H
