#ifndef QSTATICREGEX_H
#define QSTATICREGEX_H

#include <QObject>
#include <QRegularExpression>
#include <QString>

/**
 * @brief The QStaticRegex class.
 * @details Provides static utility functions for handling and validating
 *          regular expressions. This class is designed as a singleton, offering
 *          a centralized solution for regex-related operations like checking
 *          the validity of regex patterns, determining if a string contains
 *          regex patterns, and performing exact matches against regex patterns.
 *          It is not meant to be instantiated but used directly through its
 *          static methods. The class extends QObject, allowing integration
 *          with Qt's signal-slot mechanism if needed.
 */
class QStaticRegex : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief Get the static instance of this object.
     * @details This function returns the static instance of this object. It is
     *          used to provide a singleton instance of the class, ensuring that
     *          only one instance of the class exists throughout the
     *          application.
     * @return The static instance of QStaticRegex.
     */
    static QStaticRegex &instance()
    {
        static QStaticRegex instance;
        return instance;
    }

public slots:
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
    /**
     * @brief Constructor.
     * @details This is a private constructor for QStaticRegex to prevent
     *          instantiation. Making the constructor private ensures that no
     *          objects of this class can be created from outside the class,
     *          enforcing a static-only usage pattern.
     */
    QStaticRegex() {}
};

#endif // QSTATICREGEX_H
