// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "common/qstringutils.h"

QString QStringUtils::truncateMiddle(const QString &str, int maxLen)
{
    if (str.length() <= maxLen) {
        return str;
    }

    /* Minimum 4 chars needed: "a..." */
    if (maxLen < 4) {
        return str.left(maxLen);
    }

    const int ellipsisLen  = 3;
    const int availableLen = maxLen - ellipsisLen;
    const int leftLen      = availableLen / 2;
    const int rightLen     = availableLen - leftLen;

    return str.left(leftLen) + "..." + str.right(rightLen);
}
