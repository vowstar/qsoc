// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLLSP_H
#define QSOCTOOLLSP_H

#include "agent/qsoctool.h"
#include <functional>
class QLspService;

/**
 * @brief Agent tool for LSP code intelligence operations.
 * @details Exposes LSP diagnostics, definition, hover, references, and
 *          symbol queries to the AI agent. Routes requests through the
 *          global QLspService singleton.
 */
class QSocToolLsp : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolLsp(QObject *parent = nullptr);
    ~QSocToolLsp() override;
    void setService(QLspService *service) { service_ = service; }
    void setWorkingDirectoryProvider(std::function<QString()> provider)
    {
        workingDirectory_ = std::move(provider);
    }

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;
    bool    isReadOnly() const override { return true; }

private:
    QLspService             *service_ = nullptr;
    std::function<QString()> workingDirectory_;
    QString                  formatDiagnostics(const QString &filePath);
    QString                  formatDefinition(const QString &filePath, int line, int character);
    QString                  formatHover(const QString &filePath, int line, int character);
    QString                  formatReferences(const QString &filePath, int line, int character);
    QString                  formatDocumentSymbol(const QString &filePath);
};

#endif // QSOCTOOLLSP_H
