// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "qstaticstringweaver.h"

#include <algorithm>
#include <limits>

qsizetype QStaticStringWeaver::levenshteinDistance(const QString &string1, const QString &string2)
{
    const qsizetype string1Length = string1.size();
    const qsizetype string2Length = string2.size();
    if (string1Length == 0)
        return string2Length;
    if (string2Length == 0)
        return string1Length;

    QVector<QVector<qsizetype>>
        distanceMatrix(string1Length + 1, QVector<qsizetype>(string2Length + 1, 0));
    for (qsizetype i = 0; i <= string1Length; ++i)
        distanceMatrix[i][0] = i;
    for (qsizetype j = 0; j <= string2Length; ++j)
        distanceMatrix[0][j] = j;

    for (qsizetype i = 1; i <= string1Length; ++i) {
        for (qsizetype j = 1; j <= string2Length; ++j) {
            const qsizetype editCost = (string1[i - 1] == string2[j - 1]) ? 0 : 1;
            distanceMatrix[i][j]     = std::min(
                {distanceMatrix[i - 1][j] + 1,              /* Delete */
                     distanceMatrix[i][j - 1] + 1,              /* Insert */
                     distanceMatrix[i - 1][j - 1] + editCost}); /* Replace */
        }
    }
    return distanceMatrix[string1Length][string2Length];
}

double QStaticStringWeaver::similarity(const QString &string1, const QString &string2)
{
    const qsizetype distance  = levenshteinDistance(string1, string2);
    const qsizetype maxLength = qMax(string1.size(), string2.size());
    if (maxLength == 0)
        return 1.0;
    return 1.0 - (static_cast<double>(distance) / static_cast<double>(maxLength));
}

QMap<QString, int> QStaticStringWeaver::extractCandidateSubstrings(
    const QVector<QString> &strings, int minLen, int freqThreshold)
{
    QMap<QString, int> substringFreq;
    for (const QString &string : strings) {
        QSet<QString>   seen; /* Ensure each substring counts only once per string */
        const qsizetype stringLength = string.size();
        for (int subLen = minLen; subLen <= stringLength; ++subLen) {
            for (qsizetype i = 0; i <= stringLength - subLen; ++i) {
                const QString substring = string.mid(i, subLen);
                if (!seen.contains(substring)) {
                    substringFreq[substring]++;
                    seen.insert(substring);
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
    std::sort(
        candidateMarkers.begin(),
        candidateMarkers.end(),
        [](const QString &first, const QString &second) { return first.size() > second.size(); });

    QMap<QString, QVector<QString>> groups;
    /* For each string, find the first matching candidate marker */
    for (const QString &string : stringList) {
        bool assigned = false;
        for (const QString &marker : candidateMarkers) {
            /* Check if the string starts with the marker */
            if (string.startsWith(marker)) {
                groups[marker].append(string);
                assigned = true;
                break; /* Each string is assigned to only one group */
            }
        }
        if (!assigned) {
            groups["<unknown>"].append(string);
        }
    }
    return groups;
}

QString QStaticStringWeaver::findBestGroup(
    const QString &string, const QList<QString> &candidateMarkersSorted)
{
    for (const QString &marker : candidateMarkersSorted) {
        if (string.contains(marker))
            return marker;
    }
    return "<unknown>";
}

QString QStaticStringWeaver::findBestMatchingString(
    const QString &targetString, const QVector<QString> &groupStrings, double threshold)
{
    double  bestSimilarity = threshold;
    QString bestMatch;
    for (const QString &string : groupStrings) {
        const double currentSimilarity = similarity(string, targetString);
        if (currentSimilarity > bestSimilarity) {
            bestSimilarity = currentSimilarity;
            bestMatch      = string;
        }
    }
    return bestMatch;
}

QVector<int> QStaticStringWeaver::hungarianAlgorithm(const QVector<QVector<double>> &costMatrix)
{
    const qsizetype matrixSize = costMatrix.size();
    const double    INF        = std::numeric_limits<double>::infinity();
    QVector<double> rowPotential(matrixSize + 1, 0);
    QVector<double> colPotential(matrixSize + 1, 0);
    QVector<int>    rowAssignment(matrixSize + 1, 0);
    QVector<int>    colAssignment(matrixSize + 1, 0);

    for (qsizetype i = 1; i <= matrixSize; i++) {
        rowAssignment[0] = static_cast<int>(i);
        QVector<double> minValues(matrixSize + 1, INF);
        QVector<bool>   used(matrixSize + 1, false);
        qsizetype       currentCol = 0;
        while (true) {
            used[currentCol]           = true;
            const qsizetype currentRow = rowAssignment[currentCol];
            double          delta      = INF;
            qsizetype       nextCol    = 0;
            for (qsizetype j = 1; j <= matrixSize; j++) {
                if (!used[j]) {
                    const double currentCost = costMatrix[currentRow - 1][j - 1]
                                               - rowPotential[currentRow] - colPotential[j];
                    if (currentCost < minValues[j]) {
                        minValues[j]     = currentCost;
                        colAssignment[j] = static_cast<int>(currentCol);
                    }
                    if (minValues[j] < delta) {
                        delta   = minValues[j];
                        nextCol = j;
                    }
                }
            }
            for (qsizetype j = 0; j <= matrixSize; j++) {
                if (used[j]) {
                    rowPotential[rowAssignment[j]] += delta;
                    colPotential[j] -= delta;
                } else {
                    minValues[j] -= delta;
                }
            }
            currentCol = nextCol;
            if (rowAssignment[currentCol] == 0)
                break;
        }
        while (true) {
            const qsizetype nextCol   = colAssignment[currentCol];
            rowAssignment[currentCol] = rowAssignment[nextCol];
            currentCol                = nextCol;
            if (currentCol == 0)
                break;
        }
    }

    QVector<int> result(matrixSize, -1);
    for (qsizetype j = 1; j <= matrixSize; j++) {
        if (rowAssignment[j] > 0 && rowAssignment[j] <= matrixSize)
            result[rowAssignment[j] - 1] = static_cast<int>(j - 1);
    }
    return result;
}

QString QStaticStringWeaver::removeSubstring(const QString &string, const QString &substring)
{
    if (substring.isEmpty())
        return string;

    const qsizetype index = string.indexOf(substring, 0, Qt::CaseInsensitive);
    if (index >= 0) {
        QString result = string;
        result.remove(index, substring.length());
        return result;
    }
    return string;
}

QString QStaticStringWeaver::removeCommonPrefix(const QString &string, const QString &common)
{
    if (string.startsWith(common, Qt::CaseInsensitive))
        return string.mid(common.length());
    return string;
}

QString QStaticStringWeaver::removeCommonString(const QString &string, const QString &common)
{
    if (common.isEmpty() || string.isEmpty())
        return string;

    /* Case insensitive search */
    const QString stringLower = string.toLower();
    const QString commonLower = common.toLower();

    /* Extract parts from the common string */
    QStringList parts;

    /* First try to split by underscore */
    const QStringList underscoreParts = commonLower.split("_");
    if (underscoreParts.size() > 1) {
        parts = underscoreParts;
    } else {
        /* If no underscores, try to split camelCase */
        QString currentPart;

        for (qsizetype i = 0; i < commonLower.length(); i++) {
            const QChar character = commonLower[i];
            if (i > 0 && character.isUpper()) {
                parts.append(currentPart);
                currentPart = character.toLower();
            } else {
                currentPart += character;
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
        for (qsizetype i = 0; i < reversed.size(); i++) {
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
        for (qsizetype i = 1; i < parts.size(); i++) {
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
            for (qsizetype i = 1; i < reversed.size(); i++) {
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
    /* Original parts order */
    partVariations.append(parts);

    if (parts.size() > 1 && parts.size() <= 6) {
        QStringList reversed = parts;
        std::reverse(reversed.begin(), reversed.end());
        /* Reversed parts order */
        partVariations.append(reversed);
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
    qsizetype bestPos   = -1;
    qsizetype bestLen   = 0;
    int       bestScore = std::numeric_limits<int>::max();

    for (const QString &variation : commonVariations) {
        qsizetype pos = 0;
        while ((pos = stringLower.indexOf(variation, pos)) != -1) {
            /* Calculate position score, preferring matches at boundaries */
            const qsizetype prefixLen = qMin(pos, 5);
            const qsizetype suffixLen = qMin(string.length() - (pos + variation.length()), 5);

            int contextScore = 0;
            contextScore += static_cast<int>(pos);    /* Prefer matches closer to start */
            contextScore -= (prefixLen == 0) ? 5 : 0; /* Prefer matches at boundaries */
            contextScore -= (suffixLen == 0) ? 5 : 0;
            contextScore += static_cast<int>(
                prefixLen + suffixLen); /* Prefer less surrounding context */

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
        return string.left(bestPos) + string.mid(bestPos + bestLen);
    }

    /* Try partial fuzzy matching by finding regions that might contain similar part patterns */
    if (string.length() > 5 && !parts.empty()) {
        /* Sliding window approach to find regions with part matches */
        double    bestPartScore = 0.0;
        qsizetype matchStart    = -1;
        qsizetype matchEnd      = -1;

        for (qsizetype i = 0; i < string.length(); i++) {
            for (qsizetype len = 3; len <= qMin(string.length() - i, common.length() * 2); len++) {
                const QString window = string.mid(i, len).toLower();

                /* For each variation of parts, check how many parts are in this window */
                for (const QStringList &partVariation : partVariations) {
                    double    matchedPartsCount = 0;
                    qsizetype lastMatchPos      = -1;

                    /* Count matched parts in this window */
                    for (const QString &part : partVariation) {
                        /* Only consider significant parts */
                        if (part.length() >= 2) {
                            const qsizetype partPos = window.indexOf(part, qMax(0, lastMatchPos));
                            if (partPos != -1) {
                                matchedPartsCount += 1.0;
                                lastMatchPos = partPos + part.length();
                            } else {
                                /* Try fuzzy part match if exact match fails */
                                /* Threshold for fuzzy matching */
                                double bestPartSim = 0.5;
                                for (qsizetype wpos = 0; wpos < window.length() - 1; wpos++) {
                                    const qsizetype maxPartLen
                                        = qMin(part.length() + 2, window.length() - wpos);
                                    for (qsizetype plen = qMax(2, part.length() - 1);
                                         plen <= maxPartLen;
                                         plen++) {
                                        const QString subPart = window.mid(wpos, plen);
                                        const double  sim     = similarity(subPart, part);
                                        if (sim > bestPartSim) {
                                            bestPartSim  = sim;
                                            lastMatchPos = wpos + plen;
                                        }
                                    }
                                }
                                if (bestPartSim > 0.5) {
                                    /* Partial credit for fuzzy match */
                                    matchedPartsCount += bestPartSim * 0.8;
                                }
                            }
                        }
                    }

                    /* Calculate score ratio based on matched parts and total parts */
                    const double matchRatio = matchedPartsCount
                                              / static_cast<double>(partVariation.size());

                    /* Adjust score based on length of window relative to common string length */
                    const double lengthRatio
                        = 1.0
                          - (static_cast<double>(qAbs(window.length() - common.length()))
                             / static_cast<double>(qMax(window.length(), common.length())));

                    const double overallScore = (matchRatio * 0.7) + (lengthRatio * 0.3);

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
            return string.left(matchStart) + string.mid(matchEnd);
        }
    }

    /* Basic fallback: try fuzzy matching with original common */
    double    maxSim   = 0.75; /* Higher similarity threshold */
    qsizetype matchPos = -1;
    qsizetype matchLen = 0;

    for (qsizetype i = 0; i < string.length() - 2; i++) {
        for (qsizetype len = 3; len <= qMin(common.length() + 5, string.length() - i); len++) {
            const QString substring = string.mid(i, len);
            const double  sim       = similarity(substring.toLower(), commonLower);

            if (sim > maxSim) {
                maxSim   = sim;
                matchPos = i;
                matchLen = len;
            }
        }
    }

    if (matchPos != -1) {
        return string.left(matchPos) + string.mid(matchPos + matchLen);
    }

    return string; /* No match found, return original */
}

double QStaticStringWeaver::trimmedSimilarity(
    const QString &string1, const QString &string2, const QString &common)
{
    /* Extract parts from the common string */
    auto extractParts = [](const QString &str) -> QStringList {
        const QString strLower = str.toLower();
        QStringList   parts;

        /* First try to split by underscore */
        const QStringList underscoreParts = strLower.split("_");
        if (underscoreParts.size() > 1) {
            parts = underscoreParts;
        } else {
            /* If no underscores, try to split camelCase */
            QString currentPart;

            for (qsizetype i = 0; i < strLower.length(); i++) {
                const QChar character = strLower[i];
                if (i > 0 && character.isUpper()) {
                    parts.append(currentPart);
                    currentPart = character.toLower();
                } else {
                    currentPart += character;
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

    /* Try to identify parts in string1 and string2 that match parts in common */
    const QStringList commonParts = extractParts(common);

    /* For complex, multi-part hints */
    if (commonParts.size() > 2) {
        /* Basic removal using the existing function */
        const QString trimmed1 = removeCommonString(string1, common);
        const QString trimmed2 = removeCommonString(string2, common);
        const double  basicSim = similarity(trimmed1, trimmed2);

        /* Part-based processing for complex cases */
        const QString string1Lower = string1.toLower();
        const QString string2Lower = string2.toLower();

        /* Create masks of where parts appear in both strings */
        QString string1Mask = string1;
        QString string2Mask = string2;

        /* Mark positions where common parts appear with placeholder characters */
        for (const QString &part : commonParts) {
            /* Skip too short parts */
            if (part.length() < 2)
                continue;

            /* Find in string1 */
            qsizetype pos = 0;
            while ((pos = string1Lower.indexOf(part, pos)) != -1) {
                for (qsizetype i = 0; i < part.length(); i++) {
                    /* Mark as matched */
                    string1Mask[pos + i] = '*';
                }
                pos += part.length();
            }

            /* Find in string2 */
            pos = 0;
            while ((pos = string2Lower.indexOf(part, pos)) != -1) {
                for (qsizetype i = 0; i < part.length(); i++) {
                    /* Mark as matched */
                    string2Mask[pos + i] = '*';
                }
                pos += part.length();
            }
        }

        /* Extract unmatched parts */
        QString string1Remnant;
        QString string2Remnant;

        for (qsizetype i = 0; i < string1Mask.length(); i++) {
            if (string1Mask[i] != '*') {
                string1Remnant += string1[i];
            }
        }

        for (qsizetype i = 0; i < string2Mask.length(); i++) {
            if (string2Mask[i] != '*') {
                string2Remnant += string2[i];
            }
        }

        /* Calculate similarity between remnants */
        const double partBasedSim = similarity(string1Remnant, string2Remnant);

        /* Return the better of the two approaches */
        return qMax(basicSim, partBasedSim);
    }

    /* For simpler cases, use the original method */
    const QString trimmed1 = removeCommonString(string1, common);
    const QString trimmed2 = removeCommonString(string2, common);
    return similarity(trimmed1, trimmed2);
}

QMap<QString, QString> QStaticStringWeaver::findOptimalMatching(
    const QVector<QString> &groupA, const QVector<QString> &groupB, const QString &commonSubstr)
{
    const qsizetype groupBSize = groupB.size();
    const qsizetype groupASize = groupA.size();
    const qsizetype matrixSize = qMax(groupBSize, groupASize);

    /* Generate multiple variants of the common substring for matching */
    QVector<QString> commonVariants;

    if (!commonSubstr.isEmpty()) {
        /* Function to extract parts from a string */
        auto extractParts = [](const QString &str) -> QStringList {
            const QString strLower = str.toLower();
            QStringList   parts;

            /* First try to split by underscore */
            const QStringList underscoreParts = strLower.split("_");
            if (underscoreParts.size() > 1) {
                parts = underscoreParts;
            } else {
                /* If no underscores, try to split camelCase */
                QString currentPart;

                for (qsizetype i = 0; i < strLower.length(); i++) {
                    const QChar character = strLower[i];
                    if (i > 0 && character.isUpper()) {
                        parts.append(currentPart);
                        currentPart = character.toLower();
                    } else {
                        currentPart += character;
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

        const QStringList commonParts = extractParts(commonSubstr);

        /* Add original common substring */
        commonVariants.append(commonSubstr);

        /* Generate variants if we have multiple parts */
        if (commonParts.size() > 1) {
            /* Underscore variant */
            QString underscoreVariant;
            for (qsizetype i = 0; i < commonParts.size(); i++) {
                underscoreVariant += commonParts[i];
                if (i < commonParts.size() - 1) {
                    underscoreVariant += "_";
                }
            }
            commonVariants.append(underscoreVariant);

            /* CamelCase variant */
            QString camelCase = commonParts[0];
            for (qsizetype i = 1; i < commonParts.size(); i++) {
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
    for (qsizetype i = 0; i < groupBSize; i++) {
        maxBLength = qMax(maxBLength, groupB[i].size());
    }

    /* Construct a cost matrix, initialize all costs to 1.0 (max cost) */
    QVector<QVector<double>> costMatrix(matrixSize, QVector<double>(matrixSize, 1.0));

    /* Fill actual costs for existing B-A pairs with length-based weighting:
       Try all common variants and use the best similarity for each pair */
    for (qsizetype i = 0; i < groupBSize; i++) {
        /* Calculate weight factor based on B string length */
        const double weight = static_cast<double>(maxBLength)
                              / static_cast<double>(groupB[i].size());

        for (qsizetype j = 0; j < groupASize; j++) {
            double bestSim = 0.0;

            /* Try each common variant */
            for (const QString &commonVariant : commonVariants) {
                const double sim = trimmedSimilarity(groupB[i], groupA[j], commonVariant);
                bestSim          = qMax(bestSim, sim);
            }

            costMatrix[i][j] = (1.0 - bestSim) * weight;
        }
    }

    /* Use Hungarian algorithm to solve assignment (assign one A to each B) */
    const QVector<int> assignment = hungarianAlgorithm(costMatrix);

    /* Create a map of the results */
    QMap<QString, QString> matching;
    for (qsizetype i = 0; i < groupBSize; i++) {
        const int assignedIndex = assignment[i];
        if (assignedIndex < groupASize) {
            matching[groupB[i]] = groupA[assignedIndex];
        }
    }

    return matching;
}

QString QStaticStringWeaver::findBestGroupMarkerForHint(
    const QString &hintString, const QList<QString> &candidateMarkers)
{
    /* Function to extract parts from string (either by underscore or camelCase) */
    auto extractParts = [](const QString &str) -> QStringList {
        const QString strLower = str.toLower();
        QStringList   parts;

        /* First try to split by underscore */
        const QStringList underscoreParts = strLower.split("_");
        if (underscoreParts.size() > 1) {
            parts = underscoreParts;
        } else {
            /* If no underscores, try to split camelCase */
            QString currentPart;

            for (qsizetype i = 0; i < strLower.length(); i++) {
                const QChar character = strLower[i];
                if (i > 0 && character.isUpper()) {
                    parts.append(currentPart);
                    currentPart = character.toLower();
                } else {
                    currentPart += character;
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
    auto partAwareSimilarity =
        [&extractParts](const QString &string1, const QString &string2) -> double {
        /* Get direct similarity */
        const double directSim = similarity(string1.toLower(), string2.toLower());

        /* Get parts from each string */
        const QStringList parts1 = extractParts(string1);
        const QStringList parts2 = extractParts(string2);

        /* If either has single part, return direct similarity */
        if (parts1.size() <= 1 || parts2.size() <= 1) {
            return directSim;
        }

        /* Calculate part match ratio */
        int    matchedParts = 0;
        double totalPartSim = 0.0;

        /* Check how many parts from string1 are in string2 */
        for (const QString &part1 : parts1) {
            double bestPartSim = 0.0;
            for (const QString &part2 : parts2) {
                const double partSim = similarity(part1, part2);
                bestPartSim          = qMax(bestPartSim, partSim);
            }
            /* Threshold for considering a part matched */
            if (bestPartSim > 0.7) {
                matchedParts++;
                totalPartSim += bestPartSim;
            }
        }

        /* Calculate parts similarity score */
        const double partMatchRatio = static_cast<double>(matchedParts)
                                      / static_cast<double>(parts1.size());
        const double avgPartSim = matchedParts > 0
                                      ? totalPartSim / static_cast<double>(matchedParts)
                                      : 0.0;

        /* Get the more meaningful of direct similarity vs. part-based similarity */
        const double partBasedScore = (partMatchRatio * 0.7) + (avgPartSim * 0.3);

        /* Return higher of direct vs part-based similarity */
        return qMax(directSim, partBasedScore);
    };

    /* Generate hint variants for better matching */
    QVector<QString> hintVariants;

    /* Extract parts from hint string */
    const QStringList hintParts = extractParts(hintString);

    /* Add the original hint string */
    hintVariants.append(hintString);

    /* Generate additional hint string variants */
    if (hintParts.size() > 1) {
        /* Generate underscore-separated variant */
        QString underscoreVariant;
        for (qsizetype i = 0; i < hintParts.size(); i++) {
            underscoreVariant += hintParts[i];
            if (i < hintParts.size() - 1) {
                underscoreVariant += "_";
            }
        }
        hintVariants.append(underscoreVariant);

        /* Generate camelCase variant */
        QString camelCaseVariant = hintParts[0];
        for (qsizetype i = 1; i < hintParts.size(); i++) {
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
    QString   bestGroupMarker;
    double    bestSimilarity = 0.0;
    qsizetype bestLength     = 0;

    for (const QString &hintVariant : hintVariants) {
        for (const QString &marker : candidateMarkers) {
            const double    currentSimilarity = partAwareSimilarity(marker, hintVariant);
            const qsizetype currentLength     = marker.length();
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
            const double    currentSimilarity = similarity(marker.toLower(), hintString.toLower());
            const qsizetype currentLength     = marker.length();
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

QString QStaticStringWeaver::stripCommonLeadingWhitespace(const QString &text)
{
    /* Split the text into lines */
    const QStringList lines = text.split('\n');

    /* Find non-empty lines to compute common whitespace
     * Empty lines (including those with only spaces) are not considered for common indentation, but will be preserved in the result */
    QStringList nonEmptyLines;
    for (const QString &line : lines) {
        if (!line.trimmed().isEmpty()) {
            nonEmptyLines.append(line);
        }
    }

    if (nonEmptyLines.isEmpty()) {
        return text;
    }

    /* Find the minimum indentation level */
    qsizetype minIndent = std::numeric_limits<qsizetype>::max();
    for (const QString &line : nonEmptyLines) {
        qsizetype leadingSpaces = 0;
        while (leadingSpaces < line.length()
               && (line[leadingSpaces] == ' ' || line[leadingSpaces] == '\t')) {
            leadingSpaces++;
        }
        minIndent = std::min(leadingSpaces, minIndent);
    }

    /* If there's no common indentation, return the original text */
    if (minIndent == std::numeric_limits<qsizetype>::max() || minIndent == 0) {
        return text;
    }

    /* Remove the common indentation
     * Preserve the positions of all original empty lines, only remove the common indentation from non-empty lines */
    QStringList resultLines;
    for (const QString &line : lines) {
        if (line.trimmed().isEmpty() || line.length() <= minIndent) {
            /* Preserve empty lines as empty strings to ensure line breaks when joining */
            resultLines.append("");
        } else {
            /* Remove the common indentation prefix */
            resultLines.append(line.mid(minIndent));
        }
    }

    /* Join the lines, preserving all empty lines */
    return resultLines.join('\n');
}
