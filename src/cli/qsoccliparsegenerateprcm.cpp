// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"
#include "common/qsocprcmbinding.h"
#include "common/qsocprcmdocument.h"
#include "common/qsocprcmmode.h"

#include <QCoreApplication>

namespace {

QString describe(const QList<QSocPrcmDiagnostic> &diagnostic)
{
    QStringList lines;
    for (const auto &item : diagnostic) {
        lines.append(item.code + ": " + item.message);
        for (const auto &source : item.source) {
            lines.append(QString("  %1:%2:%3 %4")
                             .arg(source.file)
                             .arg(source.line)
                             .arg(source.column)
                             .arg(source.path));
        }
    }
    return lines.join('\n');
}

QString statusName(QSocPrcmCheckStatus status)
{
    switch (status) {
    case QSocPrcmCheckStatus::Sat:
        return "SAT";
    case QSocPrcmCheckStatus::Unsat:
        return "UNSAT";
    case QSocPrcmCheckStatus::Unknown:
        return "UNKNOWN";
    case QSocPrcmCheckStatus::Timeout:
        return "TIMEOUT";
    case QSocPrcmCheckStatus::Cancelled:
        return "CANCELLED";
    case QSocPrcmCheckStatus::Error:
        return "ERROR";
    }
    return "ERROR";
}

} // namespace

bool QSocCliWorker::checkPrcmNetlists(const QStringList &filePathList)
{
    QList<QStringList> groups;
    if (parser.isSet("merge")) {
        groups.append(filePathList);
    } else {
        for (const auto &file : filePathList) {
            groups.append(QStringList{file});
        }
    }
    for (const auto &files : groups) {
        const auto loaded = QSocPrcmDocumentLoader::load(files);
        if (!loaded.document) {
            return showError(1, describe(loaded.diagnostic));
        }
        const auto &document = *loaded.document;
        const auto  path     = files.join(", ");
        const auto binding = QSocPrcmBinding::resolve(document.node, document.file, document.origin);
        if (!binding.plan) {
            return showError(1, describe(binding.diagnostic));
        }
        const auto mode = QSocPrcmModeCheck::check(binding.plan->input);
        if (!mode.diagnostic.isEmpty()) {
            return showError(1, describe(mode.diagnostic));
        }
        if (mode.check.isEmpty()) {
            return showError(1, "PRCM_CHECK_EMPTY: No mode query runs for " + path);
        }
        for (const auto &check : mode.check) {
            if (check.result.status != QSocPrcmCheckStatus::Sat) {
                return showError(
                    1,
                    "PRCM_CHECK_" + statusName(check.result.status) + ": " + check.name + ": "
                        + check.result.reason);
            }
        }
        showInfo(
            0,
            QCoreApplication::translate("main", "%1: resource binding and %2 stable mode queries pass.")
                .arg(path)
                .arg(mode.check.size()));
    }
    return true;
}
