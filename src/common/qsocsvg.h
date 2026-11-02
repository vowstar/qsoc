// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCSVG_H
#define QSOCSVG_H

#include <QByteArray>
#include <QString>

/**
 * @brief Make model-written SVG safe to draw and resolve its theme tokens.
 * @details Keeps static drawing elements and internal references. Scripts,
 *          external references, embedded documents, and animation go away.
 *          `var(--name)` becomes the palette color for the background.
 */
namespace QSocSvg {

constexpr qsizetype kMaxSourceBytes = 256 * 1024;

const char *mimeType();
/** @return The cleaned document, or empty with @p error set. */
QByteArray sanitize(const QByteArray &source, bool lightBackground, QString *error = nullptr);

} // namespace QSocSvg

#endif // QSOCSVG_H
