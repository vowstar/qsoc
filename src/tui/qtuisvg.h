// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QTUISVG_H
#define QTUISVG_H

#include <QByteArray>
#include <QString>

#include <functional>

/**
 * @brief Draw SVG in a helper process of this same program.
 * @details Text needs QGuiApplication, which the terminal client must not
 *          create. The helper also keeps a hostile or huge document away
 *          from the client.
 */
namespace QTuiSvg {

/** @return Exit code when @p argv asks for the helper, otherwise -1. */
int helperMain(int argc, char *argv[]);

/** @brief Queue one drawing. @p done runs on the event loop with PNG or an error. */
void rasterize(
    const QByteArray &source, std::function<void(const QByteArray &png, const QString &error)> done);

} // namespace QTuiSvg

#endif // QTUISVG_H
