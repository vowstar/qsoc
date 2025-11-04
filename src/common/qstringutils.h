// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#ifndef QSTRINGUTILS_H
#define QSTRINGUTILS_H

#include <QObject>
#include <QString>

/**
 * @brief The QStringUtils class.
 * @details Provides static utility functions for string formatting operations.
 *          This class is designed as a utility class offering string manipulation
 *          functions like truncating strings with middle ellipsis. It is not
 *          meant to be instantiated but used directly through its static methods.
 *          The class extends QObject, allowing integration with Qt's signal-slot
 *          mechanism if needed.
 */
class QStringUtils : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief Get the static instance of this object.
     * @details This function returns the static instance of this object. It is
     *          used to provide a singleton instance of the class, ensuring that
     *          only one instance of the class exists throughout the application.
     * @return The static instance of QStringUtils.
     */
    static QStringUtils &instance()
    {
        static QStringUtils instance;
        return instance;
    }

public slots:
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
    /**
     * @brief Constructor.
     * @details This is a private constructor for QStringUtils to prevent
     *          instantiation. Making the constructor private ensures that no
     *          objects of this class can be created from outside the class,
     *          enforcing a static-only usage pattern.
     */
    QStringUtils() {}
};

#endif // QSTRINGUTILS_H
