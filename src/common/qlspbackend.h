// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QLSPBACKEND_H
#define QLSPBACKEND_H

#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QStringList>

/**
 * @brief Abstract LSP backend interface.
 * @details Both built-in (library) and external (process) backends implement
 *          this interface. Method names and params follow the LSP specification
 *          directly, so external servers need no translation layer.
 */
class QLspBackend : public QObject
{
    Q_OBJECT

public:
    explicit QLspBackend(QObject *parent = nullptr)
        : QObject(parent)
    {}
    ~QLspBackend() override = default;

    /**
     * @brief Start the backend for a given workspace.
     * @param workspaceFolder Root path of the workspace.
     * @return true if started successfully.
     */
    virtual bool start(const QString &workspaceFolder) = 0;

    /**
     * @brief Stop the backend gracefully.
     */
    virtual void stop() = 0;

    /**
     * @brief Check whether the backend is ready for requests.
     */
    virtual bool isReady() const = 0;

    /**
     * @brief File extensions this backend handles, e.g. {".v", ".sv"}.
     */
    virtual QStringList extensions() const = 0;

    /**
     * @brief Send a synchronous LSP request.
     * @param method LSP method name, e.g. "textDocument/definition".
     * @param params LSP parameters as JSON.
     * @return LSP result JSON, or null QJsonValue if unsupported or error.
     */
    virtual QJsonValue request(const QString &method, const QJsonObject &params) = 0;

    /**
     * @brief Send a fire-and-forget LSP notification.
     * @param method LSP method name, e.g. "textDocument/didOpen".
     * @param params LSP parameters as JSON.
     */
    virtual void notify(const QString &method, const QJsonObject &params) = 0;

signals:
    /**
     * @brief Emitted when the server sends a notification to the client.
     * @param method LSP method name, e.g. "textDocument/publishDiagnostics".
     * @param params Notification parameters.
     */
    void notification(const QString &method, const QJsonObject &params);
};

#endif // QLSPBACKEND_H
