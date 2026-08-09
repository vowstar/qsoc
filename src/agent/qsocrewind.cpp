// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocrewind.h"

#include "agent/qsocsession.h"

#include <QLatin1Char>
#include <QStringList>

namespace {

/** @brief "2 files restored, 1 NOT restored, ..." or empty when nothing moved. */
QString fileSummaryOf(const QSocFileHistory::RestoreReport &files)
{
    QString    summary;
    const auto append = [&summary](const QString &part) {
        if (!summary.isEmpty()) {
            summary += QStringLiteral(", ");
        }
        summary += part;
    };
    if (!files.restored.isEmpty()) {
        append(QStringLiteral("%1 file%2 restored")
                   .arg(files.restored.size())
                   .arg(files.restored.size() == 1 ? QString() : QStringLiteral("s")));
    }
    if (!files.failed.isEmpty()) {
        append(QStringLiteral("%1 NOT restored").arg(files.failed.size()));
    }
    if (!files.unknown.isEmpty()) {
        append(QStringLiteral("%1 left alone (state unknown)").arg(files.unknown.size()));
    }
    return summary;
}

/** @brief Indented "label:" block listing every path on its own line. */
QString pathBlock(const QString &label, const QStringList &paths)
{
    if (paths.isEmpty()) {
        return {};
    }
    return QStringLiteral("  ") + label + QStringLiteral(":\n    ")
           + paths.join(QStringLiteral("\n    ")) + QLatin1Char('\n');
}

} // namespace

QSocRewindResult qsocApplyRewind(
    const QSocRewindRequest  &request,
    QSocSession              *session,
    QSocFileHistory          *history,
    const QSocRewindFileGate &gate)
{
    QSocRewindResult result;

    /* Refusal phase. Both steps below run before anything the user can
     * observe has changed, so returning from either leaves the session file
     * and the working tree exactly as they were.
     *
     * Invariant: rewriteMessages is the last step that may still refuse. It
     * is QSaveFile-backed, so a failure leaves the session file
     * byte-identical; every step after it commits and none of them may
     * refuse. Adding a fallible step below this point reintroduces the mixed
     * state where the conversation is gone and the tree is not restored. */
    const bool restoringFiles = request.restoreFiles && history != nullptr;
    if (restoringFiles && gate) {
        const QString refusal = gate();
        if (!refusal.isEmpty()) {
            result.refusal = refusal;
            return result;
        }
    }
    if (request.restoreConversation) {
        if (session == nullptr || !session->rewriteMessages(request.keptMessages)) {
            result.refusal = QStringLiteral(
                "the conversation could not be written to the session file");
            return result;
        }
    }

    /* Commit phase. */
    if (request.restoreConversation) {
        result.kept = request.keptMessages.is_array()
                          ? static_cast<int>(request.keptMessages.size())
                          : 0;
        /* rewriteMessages truncated the file, so the original creation
         * timestamp has to be re-emitted or the session picker starts
         * reporting the rewind as the session's birth. */
        if (request.originalCreatedAt.isValid()) {
            session->appendMeta(
                QStringLiteral("created"), request.originalCreatedAt.toString(Qt::ISODateWithMs));
        }
    }
    if (restoringFiles) {
        result.files = history->applySnapshot(request.targetSnapshot);
        /* A restore that straddled two transports left the tree part-way
         * between turns; the forward snapshots are what a retry restores
         * from, so they must survive. */
        if (request.restoreConversation && !result.files.transportChanged) {
            history->truncateAfter(request.targetSnapshot);
        }
    }
    /* A path left alone for unknown state did not move either, so the rewind
     * is not Done: the label has to match the list the report already prints. */
    result.outcome = (result.files.failed.isEmpty() && result.files.unknown.isEmpty())
                         ? QSocRewindResult::Outcome::Done
                         : QSocRewindResult::Outcome::Partial;
    return result;
}

QString qsocRewindReport(const QSocRewindRequest &request, const QSocRewindResult &result)
{
    if (result.outcome == QSocRewindResult::Outcome::Refused) {
        return QStringLiteral("\n(Rewind cancelled: %1. Nothing was changed.)\n").arg(result.refusal);
    }

    const QString summary = fileSummaryOf(result.files);
    QString       text;
    if (result.outcome == QSocRewindResult::Outcome::Partial) {
        if (request.restoreConversation) {
            text = QStringLiteral(
                       "\n(Rewound partially: kept %1 message%2, %3, picked text restored "
                       "for editing)\n")
                       .arg(result.kept)
                       .arg(result.kept == 1 ? QString() : QStringLiteral("s"))
                       .arg(summary);
        } else {
            text = QStringLiteral("\n(Rewound partially: code only, %1, conversation unchanged)\n")
                       .arg(summary);
        }
    } else if (request.restoreConversation) {
        text = QStringLiteral("\n(Rewound: kept %1 message%2%3, picked text restored for editing)\n")
                   .arg(result.kept)
                   .arg(result.kept == 1 ? QString() : QStringLiteral("s"))
                   .arg(summary.isEmpty() ? QString() : QStringLiteral(", ") + summary);
    } else {
        text = QStringLiteral("\n(Rewound code only: %1, conversation unchanged)\n")
                   .arg(summary.isEmpty() ? QStringLiteral("no files changed") : summary);
    }

    if (result.files.transportChanged) {
        text += QStringLiteral(
            "  the connection changed mid-restore; the tree is part-way between two turns\n"
            "  the checkpoints a retry needs were kept\n");
    }
    text += pathBlock(QStringLiteral("could not restore"), result.files.failed);
    text += pathBlock(QStringLiteral("left untouched (state unknown)"), result.files.unknown);
    return text;
}
