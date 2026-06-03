// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "cli/qsocpredictioncontroller.h"

#include <QPointer>
#include <QRegularExpression>
#include <QStringList>

namespace {

/* Instructs the model to predict the USER's next line, not to answer. */
const QString kPredictionSystemPrompt = QStringLiteral(
    "You predict the human user's NEXT message in a coding-agent CLI. Given the "
    "conversation so far, output ONLY the single line the user is most likely to "
    "type next: a short instruction or command to the assistant. 2 to 12 words. "
    "No quotes, no preamble, no explanation. Do not answer questions, do not "
    "describe what you will do, do not use first person ('I', 'Let me'), do not "
    "call tools. If the next step is not obvious, reply with a short next-step "
    "command like 'run the tests' or 'commit'.");

constexpr int kMaxTranscriptMessages = 6;   /* Last ~3 turns */
constexpr int kMaxAssistantChars     = 500; /* Truncate long replies */
constexpr int kMaxGhostChars         = 100;
constexpr int kMinWords              = 2;
constexpr int kMaxWords              = 12;

QString jsonToQString(const json &value)
{
    return value.is_string() ? QString::fromStdString(value.get<std::string>()) : QString();
}

} // namespace

QSocPredictionController::QSocPredictionController(QObject *parent, const QLLMService *mainLlm)
    : QObject(parent)
{
    if (mainLlm != nullptr) {
        llm = mainLlm->clone(this);
    }
}

void QSocPredictionController::setEnabled(bool value)
{
    enabled = value;
    if (!enabled) {
        cancel();
    }
}

int QSocPredictionController::assistantTurnCount(const json &messages)
{
    if (!messages.is_array()) {
        return 0;
    }
    int count = 0;
    for (const auto &message : messages) {
        if (message.contains("role") && message["role"].is_string()
            && message["role"].get<std::string>() == "assistant") {
            count++;
        }
    }
    return count;
}

QString QSocPredictionController::buildTranscript(const json &messages)
{
    if (!messages.is_array()) {
        return {};
    }

    /* Collect the most recent user/assistant text turns. */
    QStringList lines;
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (lines.size() >= kMaxTranscriptMessages) {
            break;
        }
        const json &message = *it;
        if (!message.contains("role") || !message["role"].is_string()
            || !message.contains("content")) {
            continue;
        }
        const QString role    = QString::fromStdString(message["role"].get<std::string>());
        QString       content = jsonToQString(message["content"]).trimmed();
        if (content.isEmpty()) {
            continue;
        }
        if (role == "assistant") {
            if (content.size() > kMaxAssistantChars) {
                content = content.left(kMaxAssistantChars);
            }
            lines.prepend(QStringLiteral("Assistant: ") + content);
        } else if (role == "user") {
            lines.prepend(QStringLiteral("User: ") + content);
        }
    }
    return lines.join(QLatin1Char('\n'));
}

void QSocPredictionController::requestPrediction(const json &messages)
{
    /* Suppression guards: bail before issuing any network request. */
    if (!enabled || llm == nullptr || inFlight) {
        return;
    }
    if (lastTurnError) {
        /* Skip exactly one prediction after a failed turn. */
        lastTurnError = false;
        return;
    }
    const int turns = assistantTurnCount(messages);
    if (turns < 2) {
        return;
    }
    if (turns == lastPredictedTurns) {
        /* No new agent turn since the last request (e.g. a slash command). */
        return;
    }
    lastPredictedTurns = turns;

    const QString transcript = buildTranscript(messages);
    if (transcript.isEmpty()) {
        return;
    }

    /* Cancellation token: a callback whose token no longer matches is dropped. */
    const quint64 token = ++generation;
    inFlight            = true;

    QPointer<QSocPredictionController> self(this);
    llm->sendRequestAsync(
        transcript,
        [self, token](LLMResponse &response) {
            if (self.isNull() || token != self->generation) {
                return; /* Cancelled or controller gone */
            }
            self->inFlight = false;
            if (!response.success) {
                return;
            }
            /* First non-empty line only. */
            QString           line;
            const QStringList parts = response.content.split(QLatin1Char('\n'));
            for (const QString &part : parts) {
                if (!part.trimmed().isEmpty()) {
                    line = part.trimmed();
                    break;
                }
            }
            if (shouldFilter(line)) {
                return;
            }
            self->ghost = line;
            emit self->ghostReady(line);
        },
        kPredictionSystemPrompt,
        /*temperature=*/0.3,
        /*jsonMode=*/false);
}

void QSocPredictionController::cancel()
{
    /* Drop any in-flight callback and clear a visible ghost. */
    ++generation;
    inFlight = false;
    if (!ghost.isEmpty()) {
        ghost.clear();
        emit ghostCleared();
    }
}

bool QSocPredictionController::shouldFilter(const QString &line)
{
    if (line.isEmpty() || line.size() >= kMaxGhostChars) {
        return true;
    }
    /* Single line only: a stray newline or markdown marker means junk. */
    if (line.contains(QLatin1Char('\n')) || line.contains(QLatin1Char('*'))) {
        return true;
    }

    const QStringList words
        = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    const int wordCount = static_cast<int>(words.size());
    if (wordCount > kMaxWords) {
        return true;
    }
    if (wordCount < kMinWords) {
        /* Allow short slash commands and a few affirmative/action words. */
        static const QStringList kAllowed
            = {QStringLiteral("yes"),
               QStringLiteral("no"),
               QStringLiteral("ok"),
               QStringLiteral("okay"),
               QStringLiteral("sure"),
               QStringLiteral("yep"),
               QStringLiteral("yeah"),
               QStringLiteral("commit"),
               QStringLiteral("push"),
               QStringLiteral("continue"),
               QStringLiteral("run"),
               QStringLiteral("stop"),
               QStringLiteral("retry"),
               QStringLiteral("go"),
               QStringLiteral("deploy"),
               QStringLiteral("quit"),
               QStringLiteral("exit")};
        if (!line.startsWith(QLatin1Char('/')) && !kAllowed.contains(line.toLower())) {
            return true;
        }
    }

    /* Interrogative: a question is not an instruction the user would type. */
    if (line.endsWith(QLatin1Char('?'))) {
        return true;
    }
    /* Multiple sentences: a single command is one clause. */
    static const QRegularExpression kMultiSentence(QStringLiteral("[.!?]\\s+\\S"));
    if (kMultiSentence.match(line).hasMatch()) {
        return true;
    }
    /* Evaluative chatter rather than a next action. */
    static const QRegularExpression kEvaluative(
        QStringLiteral(
            "looks good|lgtm|sounds good|that works|makes sense|"
            "thank you|thanks|\\bnice\\b|\\bgreat\\b|perfect|awesome|excellent"),
        QRegularExpression::CaseInsensitiveOption);
    if (kEvaluative.match(line).hasMatch()) {
        return true;
    }
    /* Assistant voice rather than the user's own words. */
    static const QRegularExpression kAssistantVoice(
        QStringLiteral(
            "^(let me|i'?ll|i will|i'?ve|i'?m|here'?s|here is|"
            "sure[,!.]|of course|certainly)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    return kAssistantVoice.match(line).hasMatch();
}
