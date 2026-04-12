// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCSESSION_H
#define QSOCSESSION_H

#include <QDateTime>
#include <QList>
#include <QString>

#include <nlohmann/json.hpp>

/**
 * @brief Per-project agent session storage on disk.
 * @details A session is one append-only JSONL file under
 *          `<projectPath>/.qsoc/sessions/<sessionId>.jsonl`. Each line is a
 *          standalone JSON object with a `type` discriminator:
 *
 *            {"type":"meta","key":"created","value":"2026-04-11T10:00:00Z"}
 *            {"type":"meta","key":"first_prompt","value":"analyze the bus..."}
 *            {"type":"message","role":"user","content":"..."}
 *            {"type":"message","role":"assistant","content":"...","tool_calls":[...]}
 *            {"type":"message","role":"tool","tool_call_id":"...","content":"..."}
 *
 *          Sessions live inside the project directory so moving the project
 *          carries history with it — no global ~/.qsoc/projects/<sanitized>
 *          path-mangling like other CLI agents.
 */
class QSocSession
{
public:
    /**
     * @brief Lightweight metadata extracted by walking the session directory
     *        without parsing every entry. Used to render the resume picker.
     */
    struct Info
    {
        QString   id;
        QString   path;
        QDateTime createdAt;
        QDateTime lastModified;
        QString   firstPrompt;
        QString   title; /* User-assigned display name (/rename) */
        QString   branch;
        int       messageCount = 0;
    };

    /**
     * @brief Construct a session bound to a JSONL file path. The file is not
     *        created until the first append() call.
     */
    QSocSession(QString sessionId, QString filePath);

    QString id() const { return sessionIdValue; }
    QString filePath() const { return filePathValue; }

    /**
     * @brief Append a single OpenAI-style message to the session JSONL.
     * @details The message JSON is wrapped as
     *          `{"type":"message", ...message fields}` so loaders can quickly
     *          discriminate from meta entries on the same file.
     */
    void appendMessage(const nlohmann::json &message);

    /**
     * @brief Append a metadata key/value pair. Latest wins on load.
     */
    void appendMeta(const QString &key, const QString &value);

    /**
     * @brief Replace the entire JSONL with the given message list. Used by
     *        /clear to truncate the session, and by full rewrites when the
     *        agent compacts in place.
     */
    void rewriteMessages(const nlohmann::json &messages);

    /**
     * @brief Load a session JSONL into a flat OpenAI-style message array.
     * @return Empty array on failure or if the file does not exist.
     */
    static nlohmann::json loadMessages(const QString &filePath);

    /**
     * @brief Read just enough of a session file to populate Info without
     *        building the message list. Reads up to ~16 KB from the head
     *        for meta + first prompt detection, plus filesystem mtime.
     */
    static Info readInfo(const QString &filePath);

    /**
     * @brief List all sessions under <projectPath>/.qsoc/sessions/, newest
     *        first by mtime.
     */
    static QList<Info> listAll(const QString &projectPath);

    /**
     * @brief Resolve a session id (full or unique prefix) against the
     *        project's session directory.
     * @return Empty string when no unique match is found.
     */
    static QString resolveId(const QString &projectPath, const QString &idOrPrefix);

    /**
     * @brief Compute the canonical sessions directory for a project.
     */
    static QString sessionsDir(const QString &projectPath);

    /**
     * @brief Generate a fresh session id (UUID v4 without braces).
     */
    static QString generateId();

private:
    QString sessionIdValue;
    QString filePathValue;
};

#endif // QSOCSESSION_H
