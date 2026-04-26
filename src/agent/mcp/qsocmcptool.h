// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCMCPTOOL_H
#define QSOCMCPTOOL_H

#include "agent/mcp/qsocmcptypes.h"
#include "agent/qsoctool.h"

#include <QPointer>
#include <QString>

class QEventLoop;
class QSocMcpClient;

/**
 * @brief Adapter that exposes one MCP server tool as a QSocTool.
 * @details The adapter holds a weak reference to the owning client and
 *          forwards execute() to its JSON-RPC tools/call. The synchronous
 *          QSocTool::execute() contract is satisfied via a nested event
 *          loop that waits on the client's response signals; abort()
 *          breaks out of the loop early.
 */
class QSocMcpTool : public QSocTool
{
    Q_OBJECT

public:
    QSocMcpTool(QSocMcpClient *client, McpToolDescriptor descriptor, QObject *parent = nullptr);
    ~QSocMcpTool() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;
    void    abort() override;

    const McpToolDescriptor &descriptor() const;

private:
    QPointer<QSocMcpClient> client_;
    McpToolDescriptor       descriptor_;
    QString                 namespacedName_;

    QEventLoop *currentLoop_ = nullptr;
    bool        aborted_     = false;
};

#endif // QSOCMCPTOOL_H
