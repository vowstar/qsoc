// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOKENIZER_H
#define QSOCTOKENIZER_H

#include <vector>
#include <QByteArray>
#include <QByteArrayView>
#include <QString>

/**
 * @brief Local prompt token counter.
 * @details O200k encodes with the o200k_base table embedded in the binary and
 *          falls back to Bytes when the table cannot be loaded. Bytes counts
 *          (UTF-8 bytes + 3) / 4. All functions are thread safe.
 */
class QSocTokenizer
{
public:
    enum class Mode { O200k, Bytes };

    /** @brief Token count of @p text. */
    static qint64 count(const QString &text, Mode mode = Mode::O200k);

    /** @brief Prefix of @p text cut at a token boundary, at most @p maxTokens long. */
    static QString truncate(const QString &text, qint64 maxTokens, Mode mode = Mode::O200k);

    /** @brief o200k token ids of @p text, empty when the table is unavailable. */
    static std::vector<int> encode(const QString &text);

    /** @brief Merge one pre-tokenized piece up from single bytes. */
    static std::vector<int> encodePiece(QByteArrayView piece);

    /** @brief Whether the o200k table loaded and matched its digest. */
    static bool available();

    /** @brief Number of o200k ranks. */
    static int vocabularySize();

    /** @brief Bytes of one o200k rank. */
    static QByteArray tokenBytes(int rank);
};

#endif // QSOCTOKENIZER_H
