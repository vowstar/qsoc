// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLSHELL_H
#define QSOCTOOLSHELL_H

#include "agent/qsoctool.h"
#include "common/qsocprojectmanager.h"

/**
 * @brief Tool to execute shell commands
 */
class QSocToolShellBash : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolShellBash(
        QObject *parent = nullptr, QSocProjectManager *projectManager = nullptr);
    ~QSocToolShellBash() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

    void setProjectManager(QSocProjectManager *projectManager);

private:
    QSocProjectManager *projectManager = nullptr;
};

#endif // QSOCTOOLSHELL_H
