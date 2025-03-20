#include "qstaticstringweaver.h"

#include <algorithm>
#include <limits>

int QStaticStringWeaver::levenshteinDistance(const QString &s1, const QString &s2)
{
    int n = s1.size(), m = s2.size();
    if (n == 0)
        return m;
    if (m == 0)
        return n;

    QVector<QVector<int>> dp(n + 1, QVector<int>(m + 1, 0));
    for (int i = 0; i <= n; ++i)
        dp[i][0] = i;
    for (int j = 0; j <= m; ++j)
        dp[0][j] = j;

    for (int i = 1; i <= n; ++i) {
        for (int j = 1; j <= m; ++j) {
            int cost = (s1[i - 1] == s2[j - 1]) ? 0 : 1;
            dp[i][j] = std::min(
                {dp[i - 1][j] + 1,          /* Delete */
                 dp[i][j - 1] + 1,          /* Insert */
                 dp[i - 1][j - 1] + cost}); /* Replace */
        }
    }
    return dp[n][m];
}

double QStaticStringWeaver::similarity(const QString &s1, const QString &s2)
{
    int dist   = levenshteinDistance(s1, s2);
    int maxLen = qMax(s1.size(), s2.size());
    if (maxLen == 0)
        return 1.0;
    return 1.0 - static_cast<double>(dist) / maxLen;
}

QMap<QString, int> QStaticStringWeaver::extractCandidateSubstrings(
    const QVector<QString> &strings, int minLen, int freqThreshold)
{
    QMap<QString, int> substringFreq;
    for (const QString &s : strings) {
        QSet<QString> seen; /* Ensure each substring counts only once per string */
        int           len = s.size();
        for (int subLen = minLen; subLen <= len; ++subLen) {
            for (int i = 0; i <= len - subLen; ++i) {
                QString sub = s.mid(i, subLen);
                if (!seen.contains(sub)) {
                    substringFreq[sub]++;
                    seen.insert(sub);
                }
            }
        }
    }
    /* Filter out substrings below threshold */
    QMap<QString, int> candidates;
    for (auto it = substringFreq.begin(); it != substringFreq.end(); ++it) {
        if (it.value() >= freqThreshold) {
            candidates.insert(it.key(), it.value());
        }
    }
    return candidates;
}

QMap<QString, QVector<QString>> QStaticStringWeaver::clusterStrings(
    const QVector<QString> &stringList, const QMap<QString, int> &candidateSubstrings)
{
    /* Sort candidate substrings by descending length - longer ones are more specific */
    QList<QString> candidateMarkers = candidateSubstrings.keys();
    std::sort(candidateMarkers.begin(), candidateMarkers.end(), [](const QString &a, const QString &b) {
        return a.size() > b.size();
    });

    QMap<QString, QVector<QString>> groups;
    /* For each string, find the first matching candidate marker */
    for (const QString &s : stringList) {
        bool assigned = false;
        for (const QString &marker : candidateMarkers) {
            /* Check if the string starts with the marker */
            if (s.startsWith(marker)) {
                groups[marker].append(s);
                assigned = true;
                break; /* Each string is assigned to only one group */
            }
        }
        if (!assigned) {
            groups["<unknown>"].append(s);
        }
    }
    return groups;
}

QString QStaticStringWeaver::findBestGroup(
    const QString &str, const QList<QString> &candidateMarkersSorted)
{
    for (const QString &marker : candidateMarkersSorted) {
        if (str.contains(marker))
            return marker;
    }
    return "<unknown>";
}

QString QStaticStringWeaver::findBestMatchingString(
    const QString &targetStr, const QVector<QString> &groupStrings, double threshold)
{
    double  bestSim = threshold;
    QString bestMatch;
    for (const QString &s : groupStrings) {
        double sim = similarity(s, targetStr);
        if (sim > bestSim) {
            bestSim   = sim;
            bestMatch = s;
        }
    }
    return bestMatch;
}

QVector<int> QStaticStringWeaver::hungarianAlgorithm(const QVector<QVector<double>> &costMatrix)
{
    int             N   = costMatrix.size();
    const double    INF = std::numeric_limits<double>::infinity();
    QVector<double> u(N + 1, 0), v(N + 1, 0);
    QVector<int>    p(N + 1, 0), way(N + 1, 0);

    for (int i = 1; i <= N; i++) {
        p[0] = i;
        QVector<double> minv(N + 1, INF);
        QVector<bool>   used(N + 1, false);
        int             j0 = 0;
        do {
            used[j0]     = true;
            int    i0    = p[j0];
            double delta = INF;
            int    j1    = 0;
            for (int j = 1; j <= N; j++) {
                if (!used[j]) {
                    double cur = costMatrix[i0 - 1][j - 1] - u[i0] - v[j];
                    if (cur < minv[j]) {
                        minv[j] = cur;
                        way[j]  = j0;
                    }
                    if (minv[j] < delta) {
                        delta = minv[j];
                        j1    = j;
                    }
                }
            }
            for (int j = 0; j <= N; j++) {
                if (used[j]) {
                    u[p[j]] += delta;
                    v[j] -= delta;
                } else {
                    minv[j] -= delta;
                }
            }
            j0 = j1;
        } while (p[j0] != 0);
        do {
            int j1 = way[j0];
            p[j0]  = p[j1];
            j0     = j1;
        } while (j0);
    }

    QVector<int> result(N, -1);
    for (int j = 1; j <= N; j++) {
        if (p[j] > 0 && p[j] <= N)
            result[p[j] - 1] = j - 1;
    }
    return result;
}

QString QStaticStringWeaver::removeSubstring(const QString &s, const QString &substr)
{
    if (substr.isEmpty())
        return s;

    int index = s.indexOf(substr, 0, Qt::CaseInsensitive);
    if (index >= 0) {
        QString result = s;
        result.remove(index, substr.length());
        return result;
    }
    return s;
}

QString QStaticStringWeaver::removeCommonPrefix(const QString &s, const QString &common)
{
    if (s.startsWith(common, Qt::CaseInsensitive))
        return s.mid(common.length());
    return s;
}

QString QStaticStringWeaver::removeCommonString(const QString &s, const QString &common)
{
    if (common.isEmpty() || s.isEmpty())
        return s;

    /* Case insensitive search */
    QString sLower      = s.toLower();
    QString commonLower = common.toLower();

    /* Find all occurrences of common string */
    int pos       = 0;
    int bestPos   = -1;
    int bestScore = std::numeric_limits<int>::max();

    while ((pos = sLower.indexOf(commonLower, pos)) != -1) {
        /* Calculate how well this occurrence matches */
        /* For each position, calculate the context similarity around the match */
        int prefixLen = qMin(pos, 5); /* Look at up to 5 chars before */
        int suffixLen = qMin(s.length() - (pos + common.length()), 5); /* 5 chars after */

        int contextScore = 0;
        /* Basic score - prioritize earlier matches */
        contextScore += pos;

        /* Prioritize matches near string boundaries */
        contextScore -= (prefixLen == 0) ? 5 : 0; /* If match is at beginning, reduce score */
        contextScore -= (suffixLen == 0) ? 5 : 0; /* If match is at end, reduce score */

        /* Consider surrounding context complexity */
        contextScore += prefixLen + suffixLen; /* Lower priority for matches with more context */

        if (contextScore < bestScore) {
            bestScore = contextScore;
            bestPos   = pos;
        }

        pos += 1; /* Move to next position to continue search */
    }

    /* If we found a match, remove it */
    if (bestPos != -1) {
        return s.left(bestPos) + s.mid(bestPos + common.length());
    }

    /* Fallback: if no exact match, try fuzzy match for sufficiently long strings */
    if (s.length() > 5 && common.length() > 3) {
        /* Find substring with highest similarity to common */
        double maxSim   = 0.7; /* Threshold for fuzzy matching */
        int    matchPos = -1;
        int    matchLen = 0;

        for (int i = 0; i < s.length() - 2; i++) {
            for (int len = 3; len <= qMin(common.length() + 2, s.length() - i); len++) {
                QString substring = s.mid(i, len);
                double  sim       = similarity(substring.toLower(), commonLower);
                if (sim > maxSim) {
                    maxSim   = sim;
                    matchPos = i;
                    matchLen = len;
                }
            }
        }

        if (matchPos != -1) {
            return s.left(matchPos) + s.mid(matchPos + matchLen);
        }
    }

    return s; /* Return original if no match found */
}

double QStaticStringWeaver::trimmedSimilarity(
    const QString &s1, const QString &s2, const QString &common)
{
    /* Remove the common substring from both strings using the improved algorithm */
    QString s1Trimmed = removeCommonString(s1, common);
    QString s2Trimmed = removeCommonString(s2, common);

    /* Calculate base similarity between the trimmed strings */
    double sim = similarity(s1Trimmed, s2Trimmed);

    /* If s1Trimmed contains s2Trimmed (case insensitive), add a bonus proportional
       to s2Trimmed length, giving higher priority to longer common substrings */
    if (s1Trimmed.toLower().contains(s2Trimmed.toLower())) {
        /* Bonus is proportional to s2Trimmed length with scaling factor of 50 */
        double bonus = static_cast<double>(s2Trimmed.size()) / 50.0;
        sim += bonus;
        if (sim > 1.0)
            sim = 1.0; /* Limit maximum similarity to 1.0 */
    }

    return sim;
}

QMap<QString, QString> QStaticStringWeaver::findOptimalMatching(
    const QVector<QString> &groupA, const QVector<QString> &groupB, const QString &commonSubstr)
{
    int nB = groupB.size();
    int nA = groupA.size();
    int N  = qMax(nB, nA);

    /* Calculate maximum length of B strings */
    qsizetype maxBLength = 0;
    for (int i = 0; i < nB; i++) {
        maxBLength = qMax(maxBLength, groupB[i].size());
    }

    /* Construct a cost matrix, initialize all costs to 1.0 (max cost) */
    QVector<QVector<double>> costMatrix(N, QVector<double>(N, 1.0));

    /* Fill actual costs for existing B-A pairs with length-based weighting:
       cost = (1 - trimmedSimilarity) * weight, where weight = maxBLength / currentBLength
       This gives higher priority to longer B strings in the matching process */
    for (int i = 0; i < nB; i++) {
        /* Calculate weight factor based on B string length */
        double weight = static_cast<double>(maxBLength) / groupB[i].size();

        for (int j = 0; j < nA; j++) {
            double sim       = trimmedSimilarity(groupB[i], groupA[j], commonSubstr);
            costMatrix[i][j] = (1.0 - sim) * weight;
        }
    }

    /* Use Hungarian algorithm to solve assignment (assign one A to each B) */
    QVector<int> assignment = hungarianAlgorithm(costMatrix);

    /* Create a map of the results */
    QMap<QString, QString> matching;
    for (int i = 0; i < nB; i++) {
        int j = assignment[i];
        if (j < nA) {
            matching[groupB[i]] = groupA[j];
        }
    }

    return matching;
}

QString QStaticStringWeaver::findBestGroupMarkerForHint(
    const QString &hintString, const QList<QString> &candidateMarkers)
{
    QString bestHintGroupMarker;
    double  bestHintSimilarity = 0.0;
    int     bestHintLength     = 0;

    for (const QString &marker : candidateMarkers) {
        double currentSimilarity = similarity(marker.toLower(), hintString.toLower());
        int    currentLength     = marker.length();
        if (currentSimilarity > bestHintSimilarity
            || (currentSimilarity == bestHintSimilarity && currentLength > bestHintLength)) {
            bestHintSimilarity  = currentSimilarity;
            bestHintLength      = currentLength;
            bestHintGroupMarker = marker;
        }
    }

    return bestHintGroupMarker;
}
