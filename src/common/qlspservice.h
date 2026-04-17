// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QLSPSERVICE_H
#define QLSPSERVICE_H

#include "common/qlspbackend.h"

#include <QJsonArray>
#include <QMap>
#include <QObject>

/**
 * @brief LSP service singleton.
 * @details Routes LSP requests to the correct backend by file extension,
 *          manages file open/change/save lifecycle, and caches diagnostics
 *          for agent consumption.
 */
class QLspService : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief Get the global singleton instance.
     */
    static QLspService *instance();

    /**
     * @brief Register a backend. The service takes ownership.
     * @param backend Backend to register.
     * @param overrideExisting When false (default), the first backend
     *        registered for an extension wins; subsequent registrations
     *        for the same extension are ignored. When true, the new
     *        backend replaces any existing mapping. Use true to let
     *        external servers override the built-in fallback.
     */
    void addBackend(QLspBackend *backend, bool overrideExisting = false);

    /**
     * @brief Start all registered backends for the workspace.
     * @param workspaceFolder Root path of the workspace.
     */
    void startAll(const QString &workspaceFolder);

    /**
     * @brief Stop all running backends and clear state.
     */
    void stopAll();

    /**
     * @brief Find the backend that handles a given file.
     * @param filePath File path (extension is used for lookup).
     * @return Backend pointer, or nullptr if no backend matches.
     */
    QLspBackend *backendFor(const QString &filePath) const;

    /**
     * @brief Check if any backend is available and ready.
     */
    bool isAvailable() const;

    /* File synchronization, called by file tools after writes. */
    void didOpen(const QString &filePath, const QString &content);
    void didChange(const QString &filePath, const QString &content);
    void didSave(const QString &filePath);
    void didClose(const QString &filePath);

    /* Query convenience wrappers. Line and character are 1-based. */
    QJsonValue definition(const QString &filePath, int line, int character);
    QJsonValue hover(const QString &filePath, int line, int character);
    QJsonArray references(const QString &filePath, int line, int character);
    QJsonArray documentSymbol(const QString &filePath);

    /**
     * @brief Get cached diagnostics for a specific file.
     */
    QJsonArray diagnostics(const QString &filePath) const;

    /**
     * @brief Drain all pending diagnostics across all files.
     * @details Returns and clears the pending flag. Used by the agent to
     *          inject diagnostic feedback into the conversation.
     * @return Map of file path to diagnostic array, empty if nothing pending.
     */
    QMap<QString, QJsonArray> drainPendingDiagnostics();

signals:
    /**
     * @brief Emitted when diagnostics are updated for a file.
     */
    void diagnosticsUpdated(const QString &filePath, const QJsonArray &diags);

private:
    explicit QLspService(QObject *parent = nullptr);
    ~QLspService() override;

    /* Ensure a file is open on its backend before sending requests. */
    void ensureFileOpen(const QString &filePath);

    /* Build LSP TextDocumentPositionParams from 1-based line/character. */
    QJsonObject buildPositionParams(const QString &filePath, int line, int character) const;

    /* Build LSP TextDocumentIdentifier. */
    QJsonObject buildDocumentParams(const QString &filePath) const;

    /* Convert a file path to a file:// URI. */
    static QString toUri(const QString &filePath);

    /* Convert a file:// URI to a local file path. */
    static QString fromUri(const QString &uri);

    /* Handle notification from any backend. */
    void onBackendNotification(const QString &method, const QJsonObject &params);

    QList<QLspBackend *>         backends;
    QMap<QString, QLspBackend *> extMap;       /* ".v" to backend */
    QMap<QString, int>           fileVersions; /* uri to version counter */
    QMap<QString, QJsonArray>    diagCache;    /* uri to latest diagnostics */
    QSet<QString>                diagPending;  /* uris with unread diagnostics */
    QSet<QString>                openFiles;    /* uris currently open */
};

#endif // QLSPSERVICE_H
