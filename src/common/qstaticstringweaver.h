#ifndef QSTATICSTRINGWEAVER_H
#define QSTATICSTRINGWEAVER_H

#include <QDebug>
#include <QList>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>
#include <QtCore>

/**
 * @brief The QStaticStringWeaver class.
 * @details This class provides static string manipulation utilities for calculating
 *          string similarities and matching patterns between string groups.
 *          It is not meant to be instantiated but used directly through its
 *          static methods.
 */
class QStaticStringWeaver : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief Get the static instance of this object.
     * @details This function returns the static instance of this object. It is
     *          used to provide a singleton instance of the class, ensuring that
     *          only one instance of the class exists throughout the
     *          application.
     * @return The static instance of QStaticStringWeaver.
     */
    static QStaticStringWeaver &instance()
    {
        static QStaticStringWeaver instance;
        return instance;
    }

public slots:
    /**
     * @brief Calculate Levenshtein distance between two strings.
     * @details The Levenshtein distance is a string metric for measuring the
     *          difference between two sequences.
     * @param s1 The first string.
     * @param s2 The second string.
     * @return The Levenshtein distance.
     */
    static int levenshteinDistance(const QString &s1, const QString &s2);

    /**
     * @brief Calculate normalized similarity between two strings.
     * @details This function returns a value between 0-1, where 1 means
     *          identical strings.
     * @param s1 The first string.
     * @param s2 The second string.
     * @return The normalized similarity value (0-1).
     */
    static double similarity(const QString &s1, const QString &s2);

    /**
     * @brief Extract candidate common substrings for clustering.
     * @details Iterates through all strings, enumerates substrings with length
     *          >= minLen, counts occurrences in unique strings (avoiding
     *          duplicates within the same string), and keeps substrings with
     *          frequency >= freqThreshold as candidate "group markers".
     * @param strings The input vector of strings.
     * @param minLen The minimum substring length.
     * @param freqThreshold The minimum frequency threshold.
     * @return A map of substrings and their frequencies.
     */
    static QMap<QString, int> extractCandidateSubstrings(
        const QVector<QString> &strings, int minLen, int freqThreshold);

    /**
     * @brief Cluster strings based on common substrings.
     * @details Traverses candidate substrings by descending length. For each
     *          string, use the first matching candidate as its group marker.
     * @param stringList The list of strings to cluster.
     * @param candidateSubstrings The map of candidate substrings.
     * @return A map of group markers and their associated strings.
     */
    static QMap<QString, QVector<QString>> clusterStrings(
        const QVector<QString> &stringList, const QMap<QString, int> &candidateSubstrings);

    /**
     * @brief Find best matching group for a string.
     * @details For a given string, use candidate markers to find the best
     *          matching group.
     * @param str The string to find a group for.
     * @param candidateMarkersSorted The sorted list of candidate markers.
     * @return The best matching group marker.
     */
    static QString findBestGroup(const QString &str, const QList<QString> &candidateMarkersSorted);

    /**
     * @brief Find best matching string in a group.
     * @details Calculate similarity with each string in the group, return the
     *          highest match.
     * @param targetStr The target string to match.
     * @param groupStrings The group of strings to match against.
     * @param threshold The similarity threshold.
     * @return The best matching string.
     */
    static QString findBestMatchingString(
        const QString &targetStr, const QVector<QString> &groupStrings, double threshold = 0.0);

    /**
     * @brief Hungarian algorithm for solving the assignment problem.
     * @details This algorithm solves the assignment problem (minimization).
     * @param costMatrix The square cost matrix (dimensions N x N).
     * @return A vector of size N where result[i] is the column assigned to row i.
     */
    static QVector<int> hungarianAlgorithm(const QVector<QVector<double>> &costMatrix);

    /**
     * @brief Remove a substring from a string.
     * @details If the string contains the given substring (case-insensitive),
     *          the first occurrence of that substring is removed.
     * @param s The input string.
     * @param substr The substring to remove.
     * @return The string with the substring removed, or the original string if not found.
     */
    static QString removeSubstring(const QString &s, const QString &substr);

    /**
     * @brief Remove a common prefix from a string.
     * @details If the string starts with the given common prefix (case-insensitive),
     *          the prefix is removed.
     * @param s The input string.
     * @param common The common prefix to remove.
     * @return The string with the common prefix removed, or the original string if not starting with the prefix.
     */
    static QString removeCommonPrefix(const QString &s, const QString &common);

    /**
     * @brief Find and remove a common substring from anywhere in the string.
     * @details Uses optimal alignment to find and remove the best matching instance
     *          of a common substring from anywhere in the string.
     * @param s The input string.
     * @param common The common substring to find and remove.
     * @return The string with the best matching instance of common string removed.
     */
    static QString removeCommonString(const QString &s, const QString &common);

    /**
     * @brief Calculate similarity after removing a common substring from both strings.
     * @details Removes the common substring from both strings and calculates similarity.
     * @param s1 The first string.
     * @param s2 The second string.
     * @param common The common substring to remove.
     * @return The similarity after removing the common substring.
     */
    static double trimmedSimilarity(const QString &s1, const QString &s2, const QString &common);

    /**
     * @brief Find optimal matching between two groups of strings.
     * @details Uses the Hungarian algorithm to find the optimal one-to-one
     *          matching between two groups of strings.
     * @param groupA The first group of strings.
     * @param groupB The second group of strings.
     * @param commonSubstr The common substring to remove before comparison.
     * @return A map of groupB strings to their matched groupA strings.
     */
    static QMap<QString, QString> findOptimalMatching(
        const QVector<QString> &groupA,
        const QVector<QString> &groupB,
        const QString          &commonSubstr = "");

    /**
     * @brief Find the best matching group marker for a hint string.
     * @details Calculate similarity between a hint string and each candidate marker,
     *          returning the marker with the highest similarity and/or length.
     * @param hintString The hint string to find a matching group for.
     * @param candidateMarkers The sorted list of candidate markers.
     * @return The best matching group marker.
     */
    static QString findBestGroupMarkerForHint(
        const QString &hintString, const QList<QString> &candidateMarkers);

private:
    /**
     * @brief Constructor.
     * @details This is a private constructor for QStaticStringWeaver to prevent
     *          instantiation. Making the constructor private ensures that no
     *          objects of this class can be created from outside the class,
     *          enforcing a static-only usage pattern.
     */
    QStaticStringWeaver() {}
};

#endif // QSTATICSTRINGWEAVER_H
