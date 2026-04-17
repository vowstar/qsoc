// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QLSPSLANGBACKEND_H
#define QLSPSLANGBACKEND_H

#include "common/qlspbackend.h"

#include <QMap>

/**
 * @brief Built-in Verilog/SystemVerilog LSP backend using slang library.
 * @details Uses slang as an in-process compiler frontend. No external process
 *          is spawned. Creates a fresh SourceManager per compilation to avoid
 *          buffer lifetime issues. Currently supports diagnostics; hover and
 *          definition can be added incrementally by extending request().
 */
class QLspSlangBackend : public QLspBackend
{
    Q_OBJECT

public:
    explicit QLspSlangBackend(QObject *parent = nullptr);
    ~QLspSlangBackend() override;

    bool        start(const QString &workspaceFolder) override;
    void        stop() override;
    bool        isReady() const override;
    QStringList extensions() const override;
    QJsonValue  request(const QString &method, const QJsonObject &params) override;
    void        notify(const QString &method, const QJsonObject &params) override;

private:
    bool    ready = false;
    QString workspace;

    struct FileState
    {
        std::string sourceText;
    };
    QMap<QString, FileState> files; /* uri to file state */

    /* Recompile all tracked files and emit diagnostics for the given uri. */
    void recompileAndDiagnose(const QString &uri);

    /* Build diagnostics using DiagnosticEngine with proper client. */
    QJsonArray buildDiagnostics(const QString &filterUri);

    /* Notification handlers */
    void handleDidOpen(const QJsonObject &params);
    void handleDidChange(const QJsonObject &params);
    void handleDidSave(const QJsonObject &params);
    void handleDidClose(const QJsonObject &params);
};

#endif // QLSPSLANGBACKEND_H
