#include "qstaticregex.h"

bool QStaticRegex::isNameRegexValid(const QRegularExpression &regex)
{
    /* Retrieve the pattern of the regular expression */
    const QString pattern = regex.pattern();
    /* Check if the regular expression is valid, return false if the pattern is
       invalid or empty or contains only white spaces */
    return regex.isValid() && !(pattern.isEmpty() || pattern.trimmed().isEmpty());
}

bool QStaticRegex::isNameRegularExpression(const QString &str)
{
    /* List of special characters commonly used in regular expressions */
    const QStringList specialCharacters
        = {"*", "+", "?", "|", "[", "]", "(", ")", "{", "}", "^", "$", "\\", "."};

    /* Check for special characters */
    for (const auto &character : specialCharacters) {
        if (str.contains(character)) {
            return true;
        }
    }

    /* Check for common regex patterns */
    const QStringList regexPatterns = {"\\d", "\\w", "\\s", "\\b"};
    for (const auto &pattern : regexPatterns) {
        if (str.contains(pattern)) {
            return true;
        }
    }

    /* If none of the special characters or patterns are found, assume it's not a regex */
    return false;
}

bool QStaticRegex::isNameExactMatch(const QString &str, const QRegularExpression &regex)
{
    const QString pattern = regex.pattern();
    if (pattern.isEmpty()) {
        return false;
    }
    const QRegularExpression strictRegex = QStaticRegex::isNameRegularExpression(pattern)
                                               ? regex
                                               : QRegularExpression(
                                                   "^" + QRegularExpression::escape(pattern) + "$");
    return strictRegex.match(str).hasMatch();
}
