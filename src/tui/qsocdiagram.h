// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCDIAGRAM_H
#define QSOCDIAGRAM_H

#include <QList>
#include <QString>

namespace QSocDiagram {
enum class Role { Node, Edge, Text };
struct Span
{
    QString text;
    Role    role = Role::Text;
};
enum class Error { None, Unsupported, Limit, TooWide };
struct Result
{
    QList<QList<Span>> rows;
    Error              error = Error::None;
    bool               valid() const { return error == Error::None; }
};
Result render(const QString &source, int width);
} // namespace QSocDiagram

#endif // QSOCDIAGRAM_H
