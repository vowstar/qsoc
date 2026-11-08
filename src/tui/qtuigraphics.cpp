// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuigraphics.h"

#include <algorithm>

#ifndef Q_OS_WIN
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace QTuiGraphics {

namespace {
constexpr int kReservedRows = 5;
}

QByteArray encodeSixel(const QImage &image)
{
    const QImage indexed = image.convertToFormat(QImage::Format_ARGB32)
                               .convertToFormat(QImage::Format_Indexed8, Qt::AvoidDither);
    const int    width   = indexed.width();
    const int    height  = indexed.height();
    const auto   table   = indexed.colorTable();
    QByteArray   out = QStringLiteral("\x1bP0;1;0q\"1;1;%1;%2").arg(width).arg(height).toLatin1();
    for (int index = 0; index < table.size(); ++index) {
        out += QStringLiteral("#%1;2;%2;%3;%4")
                   .arg(index)
                   .arg(qRed(table[index]) * 100 / 255)
                   .arg(qGreen(table[index]) * 100 / 255)
                   .arg(qBlue(table[index]) * 100 / 255)
                   .toLatin1();
    }
    /* P2=1 leaves pixels no register draws transparent. */
    QList<QByteArray> columns(table.size());
    for (int top = 0; top < height; top += 6) {
        QList<int> used;
        for (int row = top; row < qMin(top + 6, height); ++row) {
            const uchar *line = indexed.constScanLine(row);
            for (int col = 0; col < width; ++col) {
                const int color = line[col];
                if (qAlpha(table[color]) < 128)
                    continue;
                if (columns[color].isEmpty()) {
                    columns[color] = QByteArray(width, '\0');
                    used.append(color);
                }
                columns[color][col] = char(columns[color][col] | (1 << (row - top)));
            }
        }
        std::sort(used.begin(), used.end());
        for (qsizetype index = 0; index < used.size(); ++index) {
            if (index > 0)
                out += '$';
            out += '#' + QByteArray::number(used[index]);
            const QByteArray &bits = columns[used[index]];
            for (int col = 0; col < width;) {
                int run = 1;
                while (col + run < width && bits[col + run] == bits[col])
                    ++run;
                const char symbol = char(63 + bits[col]);
                out += run > 3 ? '!' + QByteArray::number(run) + symbol : QByteArray(run, symbol);
                col += run;
            }
            columns[used[index]].clear();
        }
        out += '-';
    }
    out += "\x1b\\";
    return out;
}

bool hasTrueColor()
{
    const QByteArray colorTerm = qgetenv("COLORTERM").toLower();
    return colorTerm == "truecolor" || colorTerm == "24bit";
}

int maxImageRows()
{
    constexpr int kMaxRows = 30;
#ifndef Q_OS_WIN
    winsize size{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_row > 0)
        return qBound(1, int(size.ws_row) - kReservedRows, kMaxRows);
#endif
    return kMaxRows;
}

ImageStore &ImageStore::instance()
{
    static ImageStore store;
    return store;
}

quint32 ImageStore::allocateId()
{
    return nextId++;
}

bool ImageStore::isResident(quint32 id) const
{
    return std::any_of(residents.cbegin(), residents.cend(), [id](const Resident &resident) {
        return resident.id == id;
    });
}

QList<quint32> ImageStore::admit(quint32 id, qint64 bytes)
{
    forget(id);
    QList<quint32> evicted;
    for (qsizetype index = 0; index < residents.size() && residentBytes + bytes > limit;) {
        if (placedIds.contains(residents[index].id)) {
            ++index;
            continue;
        }
        residentBytes -= residents[index].bytes;
        evicted.append(residents[index].id);
        residents.removeAt(index);
    }
    residents.append({id, bytes});
    residentBytes += bytes;
    return evicted;
}

void ImageStore::setPlaced(quint32 id, bool placed)
{
    if (!placed) {
        placedIds.remove(id);
        return;
    }
    placedIds.insert(id);
    for (qsizetype index = 0; index < residents.size(); ++index) {
        if (residents[index].id == id) {
            residents.move(index, residents.size() - 1);
            return;
        }
    }
}

void ImageStore::forget(quint32 id)
{
    placedIds.remove(id);
    for (qsizetype index = 0; index < residents.size(); ++index) {
        if (residents[index].id == id) {
            residentBytes -= residents[index].bytes;
            residents.removeAt(index);
            return;
        }
    }
}

} // namespace QTuiGraphics
