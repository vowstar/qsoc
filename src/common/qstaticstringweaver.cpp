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

    /* Extract parts from the common string */
    QStringList parts;

    /* First try to split by underscore */
    QStringList underscoreParts = commonLower.split("_");
    if (underscoreParts.size() > 1) {
        parts = underscoreParts;
    } else {
        /* If no underscores, try to split camelCase */
        QString original = commonLower;
        QString currentPart;

        for (int i = 0; i < original.length(); i++) {
            QChar c = original[i];
            if (i > 0 && c.isUpper()) {
                parts.append(currentPart);
                currentPart = c.toLower();
            } else {
                currentPart += c;
            }
        }
        if (!currentPart.isEmpty()) {
            parts.append(currentPart);
        }
    }

    /* If we have only one part or couldn't split, use the original */
    if (parts.size() <= 1) {
        parts.clear();
        parts.append(commonLower);
    }

    /* Generate variations of the common string for matching */
    QVector<QString> commonVariations;

    /* Add the original */
    commonVariations.append(commonLower);

    /* For a reasonable number of parts, add reversed order */
    if (parts.size() > 1 && parts.size() <= 4) {
        /* Add full reversed order */
        QStringList reversed = parts;
        std::reverse(reversed.begin(), reversed.end());
        QString reversedStr;
        for (int i = 0; i < reversed.size(); i++) {
            reversedStr += reversed[i];
            if (i < reversed.size() - 1)
                reversedStr += "_";
        }
        commonVariations.append(reversedStr);
    }

    /* Add basic camelCase and PascalCase variations for all parts */
    if (parts.size() > 1) {
        /* Generate standard camelCase */
        QString camelCase = parts[0];
        for (int i = 1; i < parts.size(); i++) {
            if (!parts[i].isEmpty()) {
                camelCase += parts[i][0].toUpper();
                if (parts[i].length() > 1) {
                    camelCase += parts[i].mid(1);
                }
            }
        }
        commonVariations.append(camelCase);

        /* Generate PascalCase */
        QString pascalCase;
        for (const QString &part : parts) {
            if (!part.isEmpty()) {
                pascalCase += part[0].toUpper();
                if (part.length() > 1) {
                    pascalCase += part.mid(1);
                }
            }
        }
        commonVariations.append(pascalCase);

        /* If parts are not too many, add reversed camel/pascal case */
        if (parts.size() <= 4) {
            QStringList reversed = parts;
            std::reverse(reversed.begin(), reversed.end());

            QString reversedCamel = reversed[0];
            for (int i = 1; i < reversed.size(); i++) {
                if (!reversed[i].isEmpty()) {
                    reversedCamel += reversed[i][0].toUpper();
                    if (reversed[i].length() > 1) {
                        reversedCamel += reversed[i].mid(1);
                    }
                }
            }
            commonVariations.append(reversedCamel);

            QString reversedPascal;
            for (const QString &part : reversed) {
                if (!part.isEmpty()) {
                    reversedPascal += part[0].toUpper();
                    if (part.length() > 1) {
                        reversedPascal += part.mid(1);
                    }
                }
            }
            commonVariations.append(reversedPascal);
        }
    }

    /* Generate additional part-based variation pattern matches */
    /* These are dynamic partial matches that we'll compare against substrings */
    QVector<QStringList> partVariations;
    partVariations.append(parts); // Original parts order

    if (parts.size() > 1 && parts.size() <= 6) {
        QStringList reversed = parts;
        std::reverse(reversed.begin(), reversed.end());
        partVariations.append(reversed); // Reversed parts order
    }

    /* Remove duplicates from variations */
    QSet<QString> uniqueVariations;
    for (const QString &var : commonVariations) {
        uniqueVariations.insert(var.toLower());
    }
    /* Convert QSet to QVector in Qt6 compatible way */
    commonVariations.clear();
    for (const QString &var : uniqueVariations) {
        commonVariations.append(var);
    }

    /* Find best match among exact variations */
    int bestPos   = -1;
    int bestLen   = 0;
    int bestScore = std::numeric_limits<int>::max();

    for (const QString &variation : commonVariations) {
        int pos = 0;
        while ((pos = sLower.indexOf(variation, pos)) != -1) {
            /* Calculate position score, preferring matches at boundaries */
            int prefixLen = qMin(pos, 5);
            int suffixLen = qMin(s.length() - (pos + variation.length()), 5);

            int contextScore = 0;
            contextScore += pos;                      /* Prefer matches closer to start */
            contextScore -= (prefixLen == 0) ? 5 : 0; /* Prefer matches at boundaries */
            contextScore -= (suffixLen == 0) ? 5 : 0;
            contextScore += prefixLen + suffixLen; /* Prefer less surrounding context */

            if (contextScore < bestScore) {
                bestScore = contextScore;
                bestPos   = pos;
                bestLen   = variation.length();
            }

            pos += 1;
        }
    }

    /* If exact match found, remove it */
    if (bestPos != -1) {
        return s.left(bestPos) + s.mid(bestPos + bestLen);
    }

    /* Try partial fuzzy matching by finding regions that might contain similar part patterns */
    if (s.length() > 5 && !parts.empty()) {
        /* Sliding window approach to find regions with part matches */
        double bestPartScore = 0.0;
        int    matchStart    = -1;
        int    matchEnd      = -1;

        for (int i = 0; i < s.length(); i++) {
            for (int len = 3; len <= qMin(s.length() - i, common.length() * 2); len++) {
                QString window = s.mid(i, len).toLower();

                /* For each variation of parts, check how many parts are in this window */
                for (const QStringList &partVariation : partVariations) {
                    double matchedPartsCount = 0;
                    int    lastMatchPos      = -1;

                    /* Count matched parts in this window */
                    for (const QString &part : partVariation) {
                        if (part.length() >= 2) { // Only consider significant parts
                            int partPos = window.indexOf(part, qMax(0, lastMatchPos));
                            if (partPos != -1) {
                                matchedPartsCount += 1.0;
                                lastMatchPos = partPos + part.length();
                            } else {
                                /* Try fuzzy part match if exact match fails */
                                double bestPartSim = 0.5; // Threshold for fuzzy matching
                                for (int wpos = 0; wpos < window.length() - 1; wpos++) {
                                    int maxPartLen = qMin(part.length() + 2, window.length() - wpos);
                                    for (int plen = qMax(2, part.length() - 1); plen <= maxPartLen;
                                         plen++) {
                                        QString subPart = window.mid(wpos, plen);
                                        double  sim     = similarity(subPart, part);
                                        if (sim > bestPartSim) {
                                            bestPartSim  = sim;
                                            lastMatchPos = wpos + plen;
                                        }
                                    }
                                }
                                if (bestPartSim > 0.5) {
                                    matchedPartsCount += bestPartSim
                                                         * 0.8; // Partial credit for fuzzy match
                                }
                            }
                        }
                    }

                    /* Calculate score ratio based on matched parts and total parts */
                    double matchRatio = matchedPartsCount / partVariation.size();

                    /* Adjust score based on length of window relative to common string length */
                    double lengthRatio = 1.0
                                         - qAbs(window.length() - common.length())
                                               / qMax(window.length(), common.length());

                    double overallScore = matchRatio * 0.7 + lengthRatio * 0.3;

                    /* If this is better than our previous best match */
                    if (overallScore > bestPartScore && overallScore > 0.5) {
                        bestPartScore = overallScore;
                        matchStart    = i;
                        matchEnd      = i + len;
                    }
                }
            }
        }

        /* If we found a good part-based match */
        if (matchStart != -1 && matchEnd != -1) {
            return s.left(matchStart) + s.mid(matchEnd);
        }
    }

    /* Basic fallback: try fuzzy matching with original common */
    double maxSim   = 0.75; /* Higher similarity threshold */
    int    matchPos = -1;
    int    matchLen = 0;

    for (int i = 0; i < s.length() - 2; i++) {
        for (int len = 3; len <= qMin(common.length() + 5, s.length() - i); len++) {
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

    return s; /* No match found, return original */
}

double QStaticStringWeaver::trimmedSimilarity(
    const QString &s1, const QString &s2, const QString &common)
{
    /* Extract parts from the common string */
    auto extractParts = [](const QString &str) -> QStringList {
        QString     strLower = str.toLower();
        QStringList parts;

        /* First try to split by underscore */
        QStringList underscoreParts = strLower.split("_");
        if (underscoreParts.size() > 1) {
            parts = underscoreParts;
        } else {
            /* If no underscores, try to split camelCase */
            QString currentPart;

            for (int i = 0; i < strLower.length(); i++) {
                QChar c = strLower[i];
                if (i > 0 && c.isUpper()) {
                    parts.append(currentPart);
                    currentPart = c.toLower();
                } else {
                    currentPart += c;
                }
            }
            if (!currentPart.isEmpty()) {
                parts.append(currentPart);
            }
        }

        /* If we have only one part or couldn't split, use the original */
        if (parts.size() <= 1) {
            parts.clear();
            parts.append(strLower);
        }

        return parts;
    };

    /* Try to identify parts in s1 and s2 that match parts in common */
    QStringList commonParts = extractParts(common);

    /* For complex, multi-part hints */
    if (commonParts.size() > 2) {
        /* Basic removal using the existing function */
        QString t1       = removeCommonString(s1, common);
        QString t2       = removeCommonString(s2, common);
        double  basicSim = similarity(t1, t2);

        /* Part-based processing for complex cases */
        QString s1Lower = s1.toLower();
        QString s2Lower = s2.toLower();

        /* Create masks of where parts appear in both strings */
        QString s1Mask = s1;
        QString s2Mask = s2;

        /* Mark positions where common parts appear with placeholder characters */
        for (const QString &part : commonParts) {
            if (part.length() < 2)
                continue; // Skip too short parts

            /* Find in s1 */
            int pos = 0;
            while ((pos = s1Lower.indexOf(part, pos)) != -1) {
                for (int i = 0; i < part.length(); i++) {
                    s1Mask[pos + i] = '*'; // Mark as matched
                }
                pos += part.length();
            }

            /* Find in s2 */
            pos = 0;
            while ((pos = s2Lower.indexOf(part, pos)) != -1) {
                for (int i = 0; i < part.length(); i++) {
                    s2Mask[pos + i] = '*'; // Mark as matched
                }
                pos += part.length();
            }
        }

        /* Extract unmatched parts */
        QString s1Remnant;
        QString s2Remnant;

        for (int i = 0; i < s1Mask.length(); i++) {
            if (s1Mask[i] != '*') {
                s1Remnant += s1[i];
            }
        }

        for (int i = 0; i < s2Mask.length(); i++) {
            if (s2Mask[i] != '*') {
                s2Remnant += s2[i];
            }
        }

        /* Calculate similarity between remnants */
        double partBasedSim = similarity(s1Remnant, s2Remnant);

        /* Return the better of the two approaches */
        return qMax(basicSim, partBasedSim);
    } else {
        /* For simpler cases, use the original method */
        QString t1 = removeCommonString(s1, common);
        QString t2 = removeCommonString(s2, common);
        return similarity(t1, t2);
    }
}

QMap<QString, QString> QStaticStringWeaver::findOptimalMatching(
    const QVector<QString> &groupA, const QVector<QString> &groupB, const QString &commonSubstr)
{
    int nB = groupB.size();
    int nA = groupA.size();
    int N  = qMax(nB, nA);

    /* Generate multiple variants of the common substring for matching */
    QVector<QString> commonVariants;

    if (!commonSubstr.isEmpty()) {
        /* Function to extract parts from a string */
        auto extractParts = [](const QString &str) -> QStringList {
            QString     strLower = str.toLower();
            QStringList parts;

            /* First try to split by underscore */
            QStringList underscoreParts = strLower.split("_");
            if (underscoreParts.size() > 1) {
                parts = underscoreParts;
            } else {
                /* If no underscores, try to split camelCase */
                QString currentPart;

                for (int i = 0; i < strLower.length(); i++) {
                    QChar c = strLower[i];
                    if (i > 0 && c.isUpper()) {
                        parts.append(currentPart);
                        currentPart = c.toLower();
                    } else {
                        currentPart += c;
                    }
                }
                if (!currentPart.isEmpty()) {
                    parts.append(currentPart);
                }
            }

            /* If we have only one part or couldn't split, use the original */
            if (parts.size() <= 1) {
                parts.clear();
                parts.append(strLower);
            }

            return parts;
        };

        QStringList commonParts = extractParts(commonSubstr);

        /* Add original common substring */
        commonVariants.append(commonSubstr);

        /* Generate variants if we have multiple parts */
        if (commonParts.size() > 1) {
            /* Underscore variant */
            QString underscoreVariant;
            for (int i = 0; i < commonParts.size(); i++) {
                underscoreVariant += commonParts[i];
                if (i < commonParts.size() - 1) {
                    underscoreVariant += "_";
                }
            }
            commonVariants.append(underscoreVariant);

            /* CamelCase variant */
            QString camelCase = commonParts[0];
            for (int i = 1; i < commonParts.size(); i++) {
                if (!commonParts[i].isEmpty()) {
                    camelCase += commonParts[i][0].toUpper();
                    if (commonParts[i].length() > 1) {
                        camelCase += commonParts[i].mid(1);
                    }
                }
            }
            commonVariants.append(camelCase);

            /* PascalCase variant */
            QString pascalCase;
            for (const QString &part : commonParts) {
                if (!part.isEmpty()) {
                    pascalCase += part[0].toUpper();
                    if (part.length() > 1) {
                        pascalCase += part.mid(1);
                    }
                }
            }
            commonVariants.append(pascalCase);
        }
    } else {
        /* If no common substring provided, add an empty one to process without any trimming */
        commonVariants.append("");
    }

    /* Calculate maximum length of B strings */
    qsizetype maxBLength = 0;
    for (int i = 0; i < nB; i++) {
        maxBLength = qMax(maxBLength, groupB[i].size());
    }

    /* Construct a cost matrix, initialize all costs to 1.0 (max cost) */
    QVector<QVector<double>> costMatrix(N, QVector<double>(N, 1.0));

    /* Fill actual costs for existing B-A pairs with length-based weighting:
       Try all common variants and use the best similarity for each pair */
    for (int i = 0; i < nB; i++) {
        /* Calculate weight factor based on B string length */
        double weight = static_cast<double>(maxBLength) / groupB[i].size();

        for (int j = 0; j < nA; j++) {
            double bestSim = 0.0;

            /* Try each common variant */
            for (const QString &commonVariant : commonVariants) {
                double sim = trimmedSimilarity(groupB[i], groupA[j], commonVariant);
                bestSim    = qMax(bestSim, sim);
            }

            costMatrix[i][j] = (1.0 - bestSim) * weight;
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
    /* Function to extract parts from string (either by underscore or camelCase) */
    auto extractParts = [](const QString &str) -> QStringList {
        QString     strLower = str.toLower();
        QStringList parts;

        /* First try to split by underscore */
        QStringList underscoreParts = strLower.split("_");
        if (underscoreParts.size() > 1) {
            parts = underscoreParts;
        } else {
            /* If no underscores, try to split camelCase */
            QString currentPart;

            for (int i = 0; i < strLower.length(); i++) {
                QChar c = strLower[i];
                if (i > 0 && c.isUpper()) {
                    parts.append(currentPart);
                    currentPart = c.toLower();
                } else {
                    currentPart += c;
                }
            }
            if (!currentPart.isEmpty()) {
                parts.append(currentPart);
            }
        }

        /* If we have only one part or couldn't split, use the original */
        if (parts.size() <= 1) {
            parts.clear();
            parts.append(strLower);
        }

        return parts;
    };

    /* Calculate similarity between two strings with part awareness */
    auto partAwareSimilarity = [&extractParts](const QString &s1, const QString &s2) -> double {
        /* Get direct similarity */
        double directSim = similarity(s1.toLower(), s2.toLower());

        /* Get parts from each string */
        QStringList parts1 = extractParts(s1);
        QStringList parts2 = extractParts(s2);

        /* If either has single part, return direct similarity */
        if (parts1.size() <= 1 || parts2.size() <= 1) {
            return directSim;
        }

        /* Calculate part match ratio */
        int    matchedParts = 0;
        double totalPartSim = 0.0;

        /* Check how many parts from s1 are in s2 */
        for (const QString &p1 : parts1) {
            double bestPartSim = 0.0;
            for (const QString &p2 : parts2) {
                double partSim = similarity(p1, p2);
                bestPartSim    = qMax(bestPartSim, partSim);
            }
            if (bestPartSim > 0.7) { // Threshold for considering a part matched
                matchedParts++;
                totalPartSim += bestPartSim;
            }
        }

        /* Calculate parts similarity score */
        double partMatchRatio = static_cast<double>(matchedParts) / parts1.size();
        double avgPartSim     = matchedParts > 0 ? totalPartSim / matchedParts : 0.0;

        /* Get the more meaningful of direct similarity vs. part-based similarity */
        double partBasedScore = partMatchRatio * 0.7 + avgPartSim * 0.3;

        /* Return higher of direct vs part-based similarity */
        return qMax(directSim, partBasedScore);
    };

    /* Generate hint variants for better matching */
    QVector<QString> hintVariants;

    /* Extract parts from hint string */
    QStringList hintParts = extractParts(hintString);

    /* Add the original hint string */
    hintVariants.append(hintString);

    /* Generate additional hint string variants */
    if (hintParts.size() > 1) {
        /* Generate underscore-separated variant */
        QString underscoreVariant;
        for (int i = 0; i < hintParts.size(); i++) {
            underscoreVariant += hintParts[i];
            if (i < hintParts.size() - 1) {
                underscoreVariant += "_";
            }
        }
        hintVariants.append(underscoreVariant);

        /* Generate camelCase variant */
        QString camelCaseVariant = hintParts[0];
        for (int i = 1; i < hintParts.size(); i++) {
            if (!hintParts[i].isEmpty()) {
                camelCaseVariant += hintParts[i][0].toUpper();
                if (hintParts[i].length() > 1) {
                    camelCaseVariant += hintParts[i].mid(1);
                }
            }
        }
        hintVariants.append(camelCaseVariant);

        /* Generate PascalCase variant */
        QString pascalCaseVariant;
        for (const QString &part : hintParts) {
            if (!part.isEmpty()) {
                pascalCaseVariant += part[0].toUpper();
                if (part.length() > 1) {
                    pascalCaseVariant += part.mid(1);
                }
            }
        }
        hintVariants.append(pascalCaseVariant);
    }

    /* Find best matching group markers for each hint variant */
    QString bestGroupMarker;
    double  bestSimilarity = 0.0;
    int     bestLength     = 0;

    for (const QString &hintVariant : hintVariants) {
        for (const QString &marker : candidateMarkers) {
            double currentSimilarity = partAwareSimilarity(marker, hintVariant);
            int    currentLength     = marker.length();
            if (currentSimilarity > bestSimilarity
                || (currentSimilarity == bestSimilarity && currentLength > bestLength)) {
                bestSimilarity  = currentSimilarity;
                bestLength      = currentLength;
                bestGroupMarker = marker;
            }
        }
    }

    /* If no good match found with variants, fall back to direct match with original hint */
    if (bestSimilarity < 0.4) {
        bestSimilarity = 0.0;
        bestLength     = 0;
        for (const QString &marker : candidateMarkers) {
            double currentSimilarity = similarity(marker.toLower(), hintString.toLower());
            int    currentLength     = marker.length();
            if (currentSimilarity > bestSimilarity
                || (currentSimilarity == bestSimilarity && currentLength > bestLength)) {
                bestSimilarity  = currentSimilarity;
                bestLength      = currentLength;
                bestGroupMarker = marker;
            }
        }
    }

    return bestGroupMarker;
}
