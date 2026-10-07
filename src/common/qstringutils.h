// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#ifndef QSTRINGUTILS_H
#define QSTRINGUTILS_H

#include <QString>

/* Static string formatting utilities. */
class QStringUtils
{
public:
    /**
     * @brief Truncate a string by replacing the middle portion with ellipsis.
     * @details If the string exceeds maxLen, this function truncates it by
     *          removing characters from the middle and inserting "..." to
     *          indicate the truncation. The result always fits within maxLen.
     *
     *          Example: "very_long_filename.txt" with maxLen=15 becomes
     *                   "very_...me.txt"
     *
     *          If maxLen < 4, the string is simply truncated from the right
     *          without ellipsis.
     * @param str The string to truncate.
     * @param maxLen The maximum length of the result string (must be >= 0).
     * @retval QString The truncated string with middle ellipsis, or the
     *         original string if it's already within maxLen.
     */
    static QString truncateMiddle(const QString &str, int maxLen);

private:
    QStringUtils() = delete;
};

#endif // QSTRINGUTILS_H
