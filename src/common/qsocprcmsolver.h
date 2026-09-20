// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCPRCMSOLVER_H
#define QSOCPRCMSOLVER_H

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

#include <stop_token>

struct QSocPrcmLiteral
{
    QString symbol;
    bool    value = true;
};

struct QSocPrcmRequirement
{
    QString source;
    /* The requirement is an AND of clauses. Each clause is an OR of literals. */
    QList<QList<QSocPrcmLiteral>> clause;
};

struct QSocPrcmQuery
{
    QStringList                symbol;
    QList<QSocPrcmRequirement> requirement;
};

enum class QSocPrcmCheckStatus { Sat, Unsat, Unknown, Timeout, Cancelled, Error };

struct QSocPrcmCheckResult
{
    QSocPrcmCheckStatus status = QSocPrcmCheckStatus::Error;
    QMap<QString, bool> value;
    QStringList         conflict;
    QString             reason;
    QString             smt;
    QString             version;
};

struct QSocPrcmCheckBudget
{
    unsigned timeoutMs = 10000;
    /* Zero leaves the solver resource count unlimited. */
    unsigned resourceLimit = 0;
};

class QSocPrcmSolver
{
public:
    static QSocPrcmCheckResult check(
        const QSocPrcmQuery       &query,
        const QSocPrcmCheckBudget &budget = {},
        std::stop_token            stop   = {});
};

#endif // QSOCPRCMSOLVER_H
