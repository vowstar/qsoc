// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#ifndef QSTATICREGEX_H
#define QSTATICREGEX_H

#include <QRegularExpression>
#include <QString>

/* Static regular-expression utilities. */
class QStaticRegex
{
public:
    /**
     * @brief Check if a regular expression is valid and non-empty.
     * @details Validates the provided regular expression object. It checks
     *          whether the regex is a valid pattern and not an empty string.
     *          This function is useful for ensuring that regex patterns used
     *          for filtering or matching are correctly formatted and not blank.
     * @param regex The QRegularExpression object to validate.
     * @retval true If the regex is valid and non-empty.
     * @retval false If the regex is invalid or an empty string.
     */
    static bool isNameRegexValid(const QRegularExpression &regex);

    /**
     * @brief Determines if a string contains a regular expression.
     * @details Checks if the provided string contains characters or patterns
     *          commonly used in regular expressions. This function is helpful
     *          for distinguishing between plain text and regex patterns,
     *          especially when both could be user inputs. It uses a heuristic
     *          approach to identify regex-specific characters or sequences.
     * @param str The string to be checked for regular expression content.
     * @retval true If the string appears to contain a regular expression.
     * @retval false If the string does not contain common regex characters or
     *         patterns.
     */
    static bool isNameRegularExpression(const QString &str);

    /**
     * @brief Checks if a string exactly matches a regular expression.
     * @details This function determines if the given string strictly matches
     *          the provided regular expression. If the regex pattern is a plain
     *          string (not a typical regular expression), it converts it to a regex
     *          that matches the exact string. Useful for validating inputs against
     *          specific patterns or keywords.
     * @param str The string to compare against the regex.
     * @param regex The QRegularExpression to match against the string.
     * @retval true If the string exactly matches the regex.
     * @retval false If the string does not match, or the pattern is empty.
     */
    static bool isNameExactMatch(const QString &str, const QRegularExpression &regex);

private:
    QStaticRegex() = delete;
};

#endif // QSTATICREGEX_H
