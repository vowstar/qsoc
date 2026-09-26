// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCMATH_H
#define QSOCMATH_H

#include <optional>
#include <QList>
#include <QString>
#include <QStringList>

namespace QSocMath {

struct Range
{
    int begin = 0;
    int end   = 0;
};

struct Span
{
    int  begin   = 0;
    int  end     = 0;
    bool display = false;
    bool closed  = false;
};

QList<Span> spans(const QString &source, const QList<Range> &excluded);

struct Layout
{
    QStringList rows;
    int         width    = 0;
    int         baseline = 0;
};

/* Failed and unsupported expressions retain their source at the caller. */
std::optional<Layout> render(
    const QString &source, bool display, int width = 256, int *steps = nullptr);
QString body(const QString &source, const Span &span);

} // namespace QSocMath

#endif // QSOCMATH_H
