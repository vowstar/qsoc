// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

/**
 * @file qsocagentruntimeevent.h
 * @brief Structured event stream shared by every qsoc agent frontend.
 * @details One value type describes everything the runtime can tell a
 *          frontend: streaming text, tool traffic, status transitions,
 *          session lifecycle, task notifications. The TUI renders it
 *          directly; the daemon serialises it onto the socket as JSON;
 *          tests assert on it.
 */

#ifndef QSOCAGENTRUNTIMEEVENT_H
#define QSOCAGENTRUNTIMEEVENT_H

#include <nlohmann/json.hpp>

#include <QDateTime>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>

/**
 * @brief Text style hints for Output events.
 * @details Frontends map these to their own presentation (ANSI styles,
 *          widget styles, protocol flags). They are hints, not mandates.
 */
enum class QSocAgentRuntimeStyle : quint8 {
    Normal = 0, /**< Default body text. */
    Dim,        /**< Secondary / meta text. */
    Bold,       /**< Section headings. */
    Warning,    /**< Something the user should notice. */
};

/**
 * @brief One event raised by @ref QSocAgentRuntime.
 */
struct QSocAgentRuntimeEvent
{
    /**
     * @brief Event kind.
     * @details The catalogue is closed: a frontend that meets an unknown
     *          kind must ignore the event, not fail.
     */
    enum class Kind : quint8 {
        /* ---- streaming turn content ---- */
        ContentChunk,   /**< Assistant text delta (text). */
        ReasoningChunk, /**< Reasoning text delta (text). */
        RunComplete,    /**< Turn finished (finalText). */
        RunError,       /**< Turn failed (text = error). */
        RunAborted,     /**< Turn interrupted (text = stop notice). */

        /* ---- tool traffic ---- */
        ToolStarted,  /**< Tool call started (toolName, callId, arguments). */
        ToolOutput,   /**< Live tool output line (callId, text). */
        ToolFinished, /**< Tool call finished (toolName, callId, text, ok, json.status). */

        /* ---- status / progress ---- */
        Status,     /**< Transient status line text (text). */
        Heartbeat,  /**< Turn still alive (iteration, elapsedSeconds). */
        Retrying,   /**< Request retry (attempt, maxAttempts, text). */
        Compacting, /**< Compaction running (layer, beforeTokens, afterTokens). */
        Tokens,     /**< Token usage update (inputTokens, outputTokens). */
        Stuck,      /**< No-progress warning (elapsedSeconds). */

        /* ---- session lifecycle ---- */
        SessionStarted,   /**< Fresh session opened (sessionId). */
        SessionResumed,   /**< Existing session opened (sessionId, messageCount). */
        SessionCleared,   /**< History cleared / fresh session (sessionId). */
        SessionPersisted, /**< Persistence outcome (ok). */

        /* ---- context management ---- */
        Compacted,       /**< Compaction committed (savedTokens, json.summary on replay). */
        ContextRestored, /**< Post-compaction supplies restored (files, skills, agents). */
        ContextUsage,    /**< Context chip numbers (usedTokens, maxTokens, threshold, flag). */

        /* ---- user interaction requests ---- */
        AskUser,      /**< ask_user round-trip (question, options). */
        PlanApproval, /**< Plan needs a decision (text). */

        /* ---- auxiliary ---- */
        Output,            /**< Free-form runtime text (text, style). */
        TaskNotification,  /**< Background task reached terminal state (text). */
        QueuedRequest,     /**< Input queued while running (text). */
        ProcessingQueued,  /**< A queued request is starting (text). */
        ModelChanged,      /**< Active model changed (text = model id). */
        EffortChanged,     /**< Reasoning effort changed (text). */
        PlanModeChanged,   /**< Plan mode toggled (flag). */
        RemoteChanged,     /**< Remote binding changed (text = target, flag = connected). */
        WorkingDirChanged, /**< Working directory changed (text). */
        ProjectChanged,    /**< Active project changed (text = path). */
        GoalChanged,       /**< Active goal changed (json = goal or null). */
        TasksChanged,      /**< Background task set changed (json = rows). */
        ImagePreview,      /**< Image metadata and base64 body in json. */
        UserStatusLine,    /**< Configured status-line output (text). */
        InputPrediction,   /**< Suggested next input (text). */
        Diff,              /**< Unified diff source (text, secondary, json). */
        StopNotice,        /**< Why a run stopped on its own (text). */
        UserMessage,       /**< A request the user typed entered the history (text). */
    };

    Kind           kind = Kind::Output;
    QString        text;      /**< Primary text payload. */
    QString        secondary; /**< Secondary payload (tool name, session id, ...). */
    QString        callId;    /**< Tool call id. */
    nlohmann::json json;      /**< Structured payload (todo rows, tasks, goal). */
    QDateTime      at;        /**< Timestamp (set by the runtime). */

    /* Numeric payloads, defaulted per kind. */
    int    iteration      = 0;
    int    elapsedSeconds = 0;
    int    attempt        = 0;
    int    maxAttempts    = 0;
    int    layer          = 0;
    int    beforeTokens   = 0;
    int    afterTokens    = 0;
    int    savedTokens    = 0;
    int    messageCount   = 0;
    int    usedTokens     = 0;
    int    maxTokens      = 0;
    double threshold      = 0.0;
    qint64 inputTokens    = 0;
    qint64 outputTokens   = 0;

    bool                  ok    = false; /**< Success flag (persist, tool finish). */
    bool                  flag  = false; /**< Generic boolean (plan mode, remote, estimate). */
    QSocAgentRuntimeStyle style = QSocAgentRuntimeStyle::Normal;

    /** Human-readable kind name (protocol serialisation, logs). */
    [[nodiscard]] QString kindName() const;

    /** Parse a kind name; Unknown-style input yields Output. */
    static Kind kindFromName(const QString &name);

    /** Serialise to the wire JSON object (daemon protocol). */
    [[nodiscard]] nlohmann::json toJson() const;
    /** Deserialise from the wire JSON object. */
    static QSocAgentRuntimeEvent fromJson(const nlohmann::json &value);
};

Q_DECLARE_METATYPE(QSocAgentRuntimeEvent)

#endif /* QSOCAGENTRUNTIMEEVENT_H */
