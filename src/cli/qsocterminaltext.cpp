// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "cli/qsocterminaltext.h"

#include <nlohmann/json.hpp>

namespace QSocTerminalText {

namespace {

/* Index of the last character of the escape sequence that starts at i. */
qsizetype escapeEnd(const QString &text, qsizetype i)
{
    if (i + 1 >= text.size())
        return i;
    const char16_t kind = text.at(i + 1).unicode();
    if (kind == u'[') {
        qsizetype end = i + 2;
        while (end < text.size() && (text.at(end).unicode() < 0x40 || text.at(end).unicode() > 0x7e))
            ++end;
        return qMin(end, text.size() - 1);
    }
    if (kind != u']' && kind != u'P' && kind != u'X' && kind != u'^' && kind != u'_')
        return i + 1;
    for (qsizetype end = i + 2; end < text.size(); ++end) {
        const char16_t c = text.at(end).unicode();
        if (c == 0x07 || c == u'\n')
            return c == u'\n' ? end - 1 : end;
        if (c == 0x1b && end + 1 < text.size() && text.at(end + 1) == QLatin1Char('\\'))
            return end + 1;
    }
    return text.size() - 1;
}

} // namespace

QString plain(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (qsizetype i = 0; i < text.size(); ++i) {
        const char16_t c = text.at(i).unicode();
        if (c == 0x1b)
            i = escapeEnd(text, i);
        else if (c == u'\t' || c == u'\n' || (c >= 0x20 && c != 0x7f && (c < 0x80 || c > 0x9f)))
            out.append(text.at(i));
    }
    return out;
}

nlohmann::json plain(nlohmann::json value)
{
    if (value.is_string())
        return plain(QString::fromStdString(value.get<std::string>())).toStdString();
    if (value.is_structured()) {
        for (auto &item : value)
            item = plain(std::move(item));
    }
    return value;
}

} // namespace QSocTerminalText
