// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuiansi.h"

#include <QRegularExpression>

namespace QTuiAnsi {

namespace {

/* The screen's fg/bg encode "Default" as 0, so palette index 0 (black)
 * needs a stand-in. 16 is the identical black of the 6x6x6 cube. */
constexpr int kBlackIndex = 16;

int clampIndex(int index)
{
    if (index <= 0) {
        return kBlackIndex;
    }
    return index > 255 ? 255 : index;
}

/* Classic 30-37/40-47 map straight onto palette 0-7; bright variants
 * 90-97/100-107 onto 8-15. */
int classicIndex(int sgr, int base, int offset)
{
    return clampIndex(sgr - base + offset);
}

struct State
{
    bool        bold     = false;
    bool        dim      = false;
    bool        inverted = false;
    QTuiFgColor fg       = QTuiFgColor::Default;
    QTuiBgColor bg       = BG_DEFAULT;
};

void applySgr(State &state, const QList<int> &params)
{
    /* Empty parameter list means reset, same as ESC[0m */
    if (params.isEmpty()) {
        state = State{};
        return;
    }
    for (int i = 0; i < params.size(); ++i) {
        const int code = params[i];
        if (code == 0) {
            state = State{};
        } else if (code == 1) {
            state.bold = true;
        } else if (code == 2) {
            state.dim = true;
        } else if (code == 7) {
            state.inverted = true;
        } else if (code == 22) {
            state.bold = false;
            state.dim  = false;
        } else if (code == 27) {
            state.inverted = false;
        } else if (code >= 30 && code <= 37) {
            state.fg = static_cast<QTuiFgColor>(classicIndex(code, 30, 0));
        } else if (code >= 90 && code <= 97) {
            state.fg = static_cast<QTuiFgColor>(classicIndex(code, 90, 8));
        } else if (code == 39) {
            state.fg = QTuiFgColor::Default;
        } else if (code >= 40 && code <= 47) {
            state.bg = static_cast<QTuiBgColor>(classicIndex(code, 40, 0));
        } else if (code >= 100 && code <= 107) {
            state.bg = static_cast<QTuiBgColor>(classicIndex(code, 100, 8));
        } else if (code == 49) {
            state.bg = BG_DEFAULT;
        } else if ((code == 38 || code == 48) && i + 1 < params.size()) {
            const bool isFg  = code == 38;
            const int  mode  = params[i + 1];
            int        index = -1;
            if (mode == 5 && i + 2 < params.size()) {
                index = clampIndex(params[i + 2]);
                i += 2;
            } else if (mode == 2 && i + 4 < params.size()) {
                index = rgbTo256(params[i + 2], params[i + 3], params[i + 4]);
                i += 4;
            } else {
                /* Malformed extended color: stop parsing this sequence */
                return;
            }
            if (isFg) {
                state.fg = static_cast<QTuiFgColor>(index);
            } else {
                state.bg = static_cast<QTuiBgColor>(index);
            }
        }
        /* Unknown codes are ignored */
    }
}

} // namespace

int rgbTo256(int red, int green, int blue)
{
    auto clamp255 = [](int value) { return value < 0 ? 0 : (value > 255 ? 255 : value); };
    red           = clamp255(red);
    green         = clamp255(green);
    blue          = clamp255(blue);

    /* Gray ramp candidate (palette 232-255: 8 + 10*n) */
    if (red == green && green == blue) {
        if (red < 4) {
            return kBlackIndex;
        }
        if (red > 246) {
            return 231; /* cube white is closer than the ramp's 238 max */
        }
        return 232 + (red - 8 + 5) / 10;
    }

    /* 6x6x6 cube: levels 0, 95, 135, 175, 215, 255 */
    auto cubeLevel = [](int value) {
        if (value < 48) {
            return 0;
        }
        if (value < 115) {
            return 1;
        }
        return (value - 35) / 40;
    };
    return 16 + 36 * cubeLevel(red) + 6 * cubeLevel(green) + cubeLevel(blue);
}

QList<Span> parse(const QString &text)
{
    QList<Span> spans;
    State       state;
    QString     pending;

    auto flush = [&]() {
        if (pending.isEmpty()) {
            return;
        }
        Span span;
        span.text     = pending;
        span.bold     = state.bold;
        span.dim      = state.dim;
        span.inverted = state.inverted;
        span.fg       = state.fg;
        span.bg       = state.bg;
        spans.append(span);
        pending.clear();
    };

    const int len = static_cast<int>(text.size());
    for (int i = 0; i < len; ++i) {
        const QChar chr = text[i];
        if (chr != QChar(u'\033')) {
            /* Drop other C0 controls that would corrupt a status row */
            if (chr == QChar(u'\r') || chr == QChar(u'\a')) {
                continue;
            }
            pending.append(chr);
            continue;
        }
        if (i + 1 >= len) {
            break;
        }
        const QChar kind = text[i + 1];
        if (kind == QLatin1Char('[')) {
            /* CSI: ESC [ params final-byte(0x40-0x7e) */
            int j = i + 2;
            while (j < len && (text[j].unicode() < 0x40 || text[j].unicode() > 0x7e)) {
                ++j;
            }
            if (j >= len) {
                break;
            }
            if (text[j] == QLatin1Char('m')) {
                flush();
                const QString body = text.mid(i + 2, j - i - 2);
                QList<int>    params;
                /* Both ';' and ':' separate SGR parameters in the wild */
                static const QRegularExpression sepRe(QStringLiteral("[;:]"));
                for (const QString &part : body.split(sepRe)) {
                    if (!part.isEmpty()) {
                        bool      good  = false;
                        const int value = part.toInt(&good);
                        if (good) {
                            params.append(value);
                        }
                    }
                }
                applySgr(state, params);
            }
            i = j;
        } else if (kind == QLatin1Char(']')) {
            /* OSC: ESC ] ... terminated by BEL or ESC \ */
            int j = i + 2;
            while (j < len) {
                if (text[j] == QChar(u'\a')) {
                    break;
                }
                if (text[j] == QChar(u'\033') && j + 1 < len && text[j + 1] == QLatin1Char('\\')) {
                    ++j;
                    break;
                }
                ++j;
            }
            i = j;
        } else {
            /* Two-byte escape (ESC c, ESC 7, ...): skip the second byte */
            ++i;
        }
    }
    flush();
    return spans;
}

} // namespace QTuiAnsi
